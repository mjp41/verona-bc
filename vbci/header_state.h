#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace vbci
{
  struct Header;

  class HeaderState
  {
  private:
    static constexpr auto Mutable = uintptr_t(0x1);
    static constexpr auto Immutable = uintptr_t(0x2);
    static constexpr auto Pending = uintptr_t(0x3);
    // Immortal and Finalizing are both uncounted. They remain distinct until
    // we prove that readonly finalizer references cannot reach a copy boundary;
    // after that they may be collapsed to recover a tag value.
    static constexpr auto Immortal = uintptr_t(0x4);
    static constexpr auto SccPtr = uintptr_t(0x5);
    static constexpr auto Cown = uintptr_t(0x6);
    static constexpr auto Finalizing = uintptr_t(0x7);
    static constexpr auto Shift = 3;
    static constexpr auto Mask = uintptr_t(0x7);

  public:
    class Snapshot
    {
    private:
      friend class HeaderState;

      uintptr_t value;

      explicit constexpr Snapshot(uintptr_t value) : value(value) {}

    public:
      bool is_mutable() const
      {
        return (value & Mask) == Mutable;
      }

      bool is_immortal() const
      {
        return (value & Mask) == Immortal;
      }

      bool is_shared() const
      {
        auto tag = value & Mask;
        return tag == Immutable || tag == SccPtr || tag == Cown;
      }

      bool is_scc_ptr() const
      {
        return (value & Mask) == SccPtr;
      }

      bool is_pending() const
      {
        return (value & Mask) == Pending;
      }

      bool is_cown() const
      {
        return (value & Mask) == Cown;
      }

      bool is_finalizing() const
      {
        return (value & Mask) == Finalizing;
      }

      Header* scc_target() const
      {
        assert(is_scc_ptr());
        return reinterpret_cast<Header*>(value & ~Mask);
      }
    };

  private:
    static constexpr auto CountOne = uintptr_t(1) << Shift;

    std::atomic<uintptr_t> value;

    static uintptr_t count_value(Snapshot state, size_t count)
    {
      return (uintptr_t(count) << Shift) | (state.value & Mask);
    }

    uintptr_t raw() const
    {
      return value.load(std::memory_order_relaxed);
    }

  public:
    explicit HeaderState(Snapshot state, size_t count = 1)
    : value(count_value(state, count))
    {}

    static constexpr Snapshot mutable_()
    {
      return Snapshot(Mutable);
    }

    static constexpr Snapshot immutable()
    {
      return Snapshot(Immutable);
    }

    static constexpr Snapshot immortal()
    {
      return Snapshot(Immortal);
    }

    static constexpr Snapshot pending()
    {
      return Snapshot(Pending);
    }

    static constexpr Snapshot cown()
    {
      return Snapshot(Cown);
    }

    static constexpr Snapshot finalizing()
    {
      return Snapshot(Finalizing);
    }

    static Snapshot scc_ptr(Header* header)
    {
      auto pointer = reinterpret_cast<uintptr_t>(header);
      assert((pointer & Mask) == 0);
      return Snapshot(SccPtr | pointer);
    }

    Snapshot load() const
    {
      return Snapshot(raw());
    }

    size_t count() const
    {
      return raw() >> Shift;
    }

    void set_count(size_t count)
    {
      auto current = load();
      value.store(count_value(current, count), std::memory_order_relaxed);
    }

    size_t increment_count()
    {
      auto previous = raw();
      value.store(previous + CountOne, std::memory_order_relaxed);
      return previous >> Shift;
    }

    size_t decrement_count()
    {
      auto previous = raw();
      value.store(previous - CountOne, std::memory_order_relaxed);
      return previous >> Shift;
    }

    size_t increment_shared_count()
    {
      return value.fetch_add(CountOne, std::memory_order_relaxed) >> Shift;
    }

    bool decrement_shared_count()
    {
      auto previous =
        value.fetch_sub(CountOne, std::memory_order_release) >> Shift;
      assert(previous > 0);
      if (previous != 1)
        return false;

      std::atomic_thread_fence(std::memory_order_acquire);
      return true;
    }

    void transition(Snapshot next)
    {
      if (next.is_scc_ptr())
      {
        value.store(next.value, std::memory_order_relaxed);
        return;
      }

      auto current = raw();
      value.store(
        (current & ~Mask) | (next.value & Mask), std::memory_order_relaxed);
    }

    Header* next() const
    {
      return reinterpret_cast<Header*>(raw() & ~Mask);
    }

    void set_next(Header* next)
    {
      auto pointer = reinterpret_cast<uintptr_t>(next);
      assert((pointer & Mask) == 0);
      auto current = raw();
      value.store(pointer | (current & Mask), std::memory_order_relaxed);
    }
  };
}
