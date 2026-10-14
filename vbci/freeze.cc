#include "freeze.h"

#include "array.h"
#include "header.h"
#include "object.h"
#include "program.h"
#include "region_ext.h"

#include <vector>

namespace vbci
{
  // Post-order marker: set LSB of pointer.
  static Header* post_order_mark(Header* h)
  {
    return reinterpret_cast<Header*>(reinterpret_cast<uintptr_t>(h) | 1);
  }

  static Header* remove_post_order_mark(Header* h)
  {
    return reinterpret_cast<Header*>(
      reinterpret_cast<uintptr_t>(h) & ~uintptr_t(1));
  }

  static bool is_post_order(Header* h)
  {
    return (reinterpret_cast<uintptr_t>(h) & 1) != 0;
  }

  // Union-by-address with rc transfer. Transfers child's external ref count
  // to the new representative, minus 1 for the intra-SCC tree edge.
  // Address as rank + path compression gives O(α(n)) amortized.
  static void scc_union(Header* child, Header* rep)
  {
    auto r = Header::find(child);
    auto rep_r = Header::find(rep);
    assert(r != rep_r);

    // Lower address points to higher address (address serves as rank).
    if (reinterpret_cast<uintptr_t>(r) > reinterpret_cast<uintptr_t>(rep_r))
      std::swap(r, rep_r);

    // Transfer external refs, minus 1 for the intra-SCC tree edge.
    rep_r->set_rc(rep_r->get_rc() + r->get_rc() - 1);
    r->set_location(Location::scc_ptr(rep_r));
  }

  // Trace a header's fields, pushing children onto the DFS stack.
  static void trace_fields(Header* h, std::vector<Header*>& dfs)
  {
    auto& program = Program::get();
    auto fn = [&](Header* h) {
      auto loc = h->location();

      if (loc.is_scc_ptr() || loc.is_pending())
      {
        dfs.push_back(h);
        return;
      }

      if (loc.is_immutable() || loc.is_immortal())
        return;

      if (loc.is_mutable())
        dfs.push_back(h);
    };

    if (program.is_array(h->get_type_id()))
      static_cast<Array*>(h)->trace_fn(fn);
    else
      static_cast<Object*>(h)->trace_fn(fn);
  }

  void freeze(Header* root, Region& region)
  {
    assert(root);
    auto loc = root->location();

    if (loc.is_immutable() || loc.is_immortal())
      return;

    std::vector<Header*> dfs;
    std::vector<Header*> pending;

    dfs.push_back(root);

    while (!dfs.empty())
    {
      Header* h_mark = dfs.back();
      dfs.pop_back();

      if (is_post_order(h_mark))
      {
        Header* h = remove_post_order_mark(h_mark);

        if (!pending.empty() && (pending.back() == h))
        {
          pending.pop_back();
          auto rep = Header::find(h);
          rep->set_location(Location::immutable());
          rep->set_arc(rep->get_rc());
        }

        continue;
      }

      Header* h = h_mark;
      auto rep = Header::find(h);
      auto rep_loc = rep->location();

      if (rep_loc.is_pending())
      {
        rep->set_rc(rep->get_rc() - 1);

        while (!pending.empty() && pending.back() != rep)
        {
          scc_union(pending.back(), rep);
          pending.pop_back();
        }
      }
      else if (rep_loc.is_immutable())
      {
        // Already frozen. No action needed.
      }
      else if (rep_loc.is_mutable())
      {
        if (!region.remove(h))
          Value::error(Error::BadFreeze);

        h->set_location(Location::pending());

        pending.push_back(h);
        dfs.push_back(post_order_mark(h));
        trace_fields(h, dfs);
      }
    }
  }
}
