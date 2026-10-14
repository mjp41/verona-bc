#pragma once

#include "ident.h"
#include "location.h"
#include "region.h"
#include "register.h"
#include "value.h"

namespace vbci
{
  struct Header
  {
  private:
    Location loc;

    union
    {
      RC rc;
      ARC arc;
    };

    uint32_t type_id;

  protected:
    Header(Location loc, uint32_t type_id) : loc(loc), rc(1), type_id(type_id)
    {}

    void mark_immortal()
    {
      loc = Location::immortal();
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
      arc.store(v);
    }

    void inc_arc()
    {
      arc++;
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

      if (loc.is_immutable())
      {
        arc++;
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

      if (loc.is_immutable())
      {
        if (--arc == 0)
          collect_scc(this);

        return;
      }

      assert(loc.is_mutable());
      auto* r = current_region();
      assert(r);

      if (!r->is_finalizing() && (--rc == 0))
        collect(this);
    }
  };
}
