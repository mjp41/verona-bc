#pragma once

#include "ident.h"
#include "location.h"
#include "region.h"
#include "register.h"
#include "value.h"

#include <atomic>
#include <new>

namespace vbci
{
  struct Header
  {
  private:
    Location loc;

    union
    {
      RC rc;
      std::atomic<size_t> atomic_rc;
      Header* next_to_collect;
    };

    uint32_t type_id;

  protected:
    Header(Location loc, uint32_t type_id) : loc(loc), rc(1), type_id(type_id)
    {}

    explicit Header(uint32_t type_id)
    : loc(Location::cown()), atomic_rc(1), type_id(type_id)
    {}

    void mark_immortal()
    {
      loc = Location::immortal();
    }

    void atomic_inc() noexcept
    {
      if (loc.is_immortal())
        return;

      assert(loc.is_shared());
      auto previous = atomic_rc.fetch_add(1, std::memory_order_relaxed);
      assert(previous > 0);
    }

    bool atomic_dec() noexcept
    {
      if (loc.is_immortal())
        return false;

      assert(loc.is_shared());
      auto previous = atomic_rc.fetch_sub(1, std::memory_order_release);
      assert(previous > 0);
      if (previous != 1)
        return false;

      std::atomic_thread_fence(std::memory_order_acquire);
      return true;
    }

  public:
    uint32_t get_type_id() const
    {
      return type_id;
    }

    RC get_rc()
    {
      return rc;
    }

    void set_rc(RC v)
    {
      rc = v;
    }

    void set_arc(RC v)
    {
      new (&atomic_rc) std::atomic<size_t>(v);
    }

    Header* collector_get_next() const
    {
      return next_to_collect;
    }

    void collector_set_next(Header* next)
    {
      next_to_collect = next;
    }

    Location location() const
    {
      return loc;
    }

    void set_location(Location l)
    {
      loc = l;
    }

    // Union-find representative lookup with path compression.
    // Used by frozen SCC objects to find their SCC root.
    static Header* find(Header* h)
    {
      if (!h->loc.is_scc_ptr())
        return h;

      auto target = h->loc.scc_target();
      auto root = find(target);

      // Path compression: point directly to root.
      if (root != target)
        h->loc = Location::scc_ptr(root);

      return root;
    }

    void inc()
    {
      if (loc.is_scc_ptr())
      {
        find(this)->inc();
        return;
      }

      if (loc.is_immortal())
        return;

      if (loc.is_finalizing())
        return;

      if (loc.is_shared())
      {
        atomic_inc();
        return;
      }

      assert(loc.is_mutable());
      rc++;
    }

    void dec()
    {
      if (loc.is_scc_ptr())
      {
        find(this)->dec();
        return;
      }

      if (loc.is_immortal())
        return;

      if (loc.is_finalizing())
        return;

      if (loc.is_shared())
      {
        if (atomic_dec())
          collect(this);

        return;
      }

      assert(loc.is_mutable());
      auto* r = current_region();
      assert(r);

      if (--rc == 0)
        collect(this);
    }
  };
}
