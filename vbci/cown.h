#pragma once

#include "header.h"
#include "program.h"
#include "region.h"
#include "value.h"

#include <atomic>
#include <boc/behaviourcore.h>
#include <ds/heap.h>
#include <format>
#include <new>

namespace vbci
{
  struct VbciObjectModel;

  struct alignas(8) Cown
  {
  private:
    friend VbciObjectModel;

    static constexpr size_t immortal_bit = 1;
    static constexpr size_t reference_increment = 2;

    union LifetimeState
    {
      std::atomic<size_t> references;
      Cown* next_to_delete;

      LifetimeState() : references(reference_increment) {}
      ~LifetimeState() {}
    };

    verona::rt::CownSchedulerState<VbciObjectModel> scheduler_state;
    LifetimeState lifetime;
    uint32_t type_id;
    Value content;
    AllocationGroup allocation_group;

    Cown(uint32_t type_id)
    : type_id(type_id),
      allocation_group(AllocationGroup::create())
    {}

    static void destroy(Cown* cown) noexcept
    {
      thread_local Cown* pending = nullptr;
      thread_local bool draining = false;

      cown->lifetime.next_to_delete = pending;
      pending = cown;

      if (draining)
        return;

      draining = true;
      while (pending != nullptr)
      {
        auto* current = pending;
        pending = current->lifetime.next_to_delete;
        current->~Cown();
        verona::rt::heap::dealloc<sizeof(Cown)>(current);
      }
      draining = false;
    }

  public:
    static Cown* create(uint32_t type_id)
    {
      if (!Program::get().is_cown(type_id))
        Value::error(Error::BadType);

      auto* memory = verona::rt::heap::alloc<sizeof(Cown)>();
      auto* cown = new (memory) Cown(type_id);
      LOG(Trace) << "Created cown @" << cown;
      return cown;
    }

    ~Cown()
    {
      LOG(Trace) << "Destroying cown @" << this;
      assert(allocation_group);
      content.dec();
      content = Value();
      LOG(Trace) << "Destroyed cown @" << this;
    }

    uint32_t get_type_id()
    {
      return type_id;
    }

    uint32_t content_type_id()
    {
      return Program::get().uncown(type_id);
    }

    void inc() noexcept
    {
      LOG(Trace) << "Incrementing cown @" << this;
      assert(
        (lifetime.references.load(std::memory_order_relaxed) & ~immortal_bit) >
        0);
      lifetime.references.fetch_add(
        reference_increment, std::memory_order_relaxed);
    }

    void dec() noexcept
    {
      LOG(Trace) << "Decrementing cown @" << this;
      auto references = lifetime.references.fetch_sub(
        reference_increment, std::memory_order_release);
      assert((references & ~immortal_bit) > 0);
      if (references != reference_increment)
        return;

      std::atomic_thread_fence(std::memory_order_acquire);
      destroy(this);
    }

    void immortalize() noexcept
    {
      lifetime.references.fetch_or(immortal_bit, std::memory_order_relaxed);
    }

    ValueBorrow load()
    {
      return content;
    }

    Region& get_allocation_group()
    {
      assert(allocation_group);
      return allocation_group.get();
    }

    AllocationGroup take_allocation_group()
    {
      assert(allocation_group);
      return std::move(allocation_group);
    }

    void set_allocation_group(AllocationGroup group)
    {
      assert(group);
      allocation_group = std::move(group);
    }

    template<bool is_move>
    bool add_reference(Reg<is_move>& next)
    {
      // Can't store a stack value in a cown.
      if (next->get_value_type() == ValueType::RegisterRef)
        return false;

      if constexpr (is_move)
        content = next.extract();
      else
      {
        next->inc();
        content = next.borrow();
      }

      return true;
    }

    template<bool is_move>
    ValueTransfer exchange(Reg<is_move> next)
    {
      if (
        !next->is_error() &&
        !Program::get().subtype(next->type_id(), content_type_id()))
        Value::error(Error::BadType);

      Value prev = content;
      if (!add_reference<is_move>(next))
        Value::error(Error::BadStore);

      return ValueTransfer(prev);
    }

    std::string to_string()
    {
      return std::format("cown: {}", static_cast<void*>(this));
    }
  };

  struct VbciObjectModel
  {
    using Cown = vbci::Cown;

    static verona::rt::CownSchedulerState<VbciObjectModel>&
    get_cown_scheduler_state(Cown& cown) noexcept
    {
      return cown.scheduler_state;
    }

    static void acquire(Cown& cown) noexcept
    {
      cown.inc();
    }

    static void release(Cown& cown) noexcept
    {
      cown.dec();
    }

    static uintptr_t get_cown_identity(const Cown& cown) noexcept
    {
      return reinterpret_cast<uintptr_t>(&cown);
    }
  };

  using BehaviourCore = verona::rt::boc::BehaviourCore<VbciObjectModel>;
}
