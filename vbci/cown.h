#pragma once

#include "header.h"
#include "program.h"
#include "region.h"
#include "value.h"

#include <boc/behaviourcore.h>
#include <ds/heap.h>
#include <format>
#include <new>

namespace vbci
{
  struct VbciObjectModel;

  struct alignas(8) Cown : public Header
  {
  private:
    friend VbciObjectModel;

    verona::rt::CownSchedulerState<VbciObjectModel> scheduler_state;
    Value content;
    AllocationGroup allocation_group;

    Cown(uint32_t type_id)
    : Header(type_id),
      allocation_group(AllocationGroup::create())
    {}

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
      assert(content.is_invalid());
      assert(!allocation_group);
      LOG(Trace) << "Destroyed cown @" << this;
    }

    void finalize()
    {
      assert(allocation_group);
      content.dec();
      content = Value();
      allocation_group.clear();
    }

    void destroy()
    {
      this->~Cown();
      verona::rt::heap::dealloc<sizeof(Cown)>(this);
    }

    uint32_t content_type_id()
    {
      return Program::get().uncown(get_type_id());
    }

    void immortalize() noexcept
    {
      mark_immortal();
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
