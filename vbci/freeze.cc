#include "freeze.h"

#include "array.h"
#include "header.h"
#include "object.h"
#include "program.h"
#include "region_ext.h"

#include <unordered_set>
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
  // Returns the number of frozen-to-frozen edges found.
  static RC trace_fields(
    Header* h,
    std::vector<Header*>& dfs,
    Region* region = nullptr,
    std::unordered_set<Header*>* frozen_set = nullptr)
  {
    RC frozen_edges = 0;
    auto& program = Program::get();
    auto fn = [&](Header* h) {
      auto loc = h->location();

      if (loc.is_scc_ptr() || loc.is_pending())
      {
        dfs.push_back(h);
        if (region)
          frozen_edges++;
        return;
      }

      if (loc.is_immutable() || loc.is_immortal())
      {
        // Count edges to objects frozen by THIS call.
        if (frozen_set && frozen_set->count(h))
          frozen_edges++;
        return;
      }

      if (loc.is_region())
      {
        dfs.push_back(h);
        if (region && loc.to_region() == region)
          frozen_edges++;
        return;
      }
    };

    if (program.is_array(h->get_type_id()))
      static_cast<Array*>(h)->trace_fn(fn);
    else
      static_cast<Object*>(h)->trace_fn(fn);

    return frozen_edges;
  }

  bool freeze(Header* root)
  {
    assert(root);
    auto loc = root->location();

    if (loc.is_immutable() || loc.is_immortal())
      return true;

    if (loc.is_stack())
      return false;

    auto region = loc.to_region();

    std::vector<Header*> worklist;
    std::vector<Header*> dfs;
    std::vector<Header*> pending;
    std::unordered_set<Header*> frozen_set;

    worklist.push_back(root);

    while (!worklist.empty())
    {
      root = worklist.back();
      worklist.pop_back();

      // Skip duplicate worklist entries and already-frozen objects.
      if (root->location().is_immutable() || root->location().is_scc_ptr())
        continue;

      region = root->region();
      RC arc_sum = 0;
      RC frozen_internal = 0;
      // Frozen edges already removed from RCs by SCC collapse.
      RC scc_internal = 0;
      frozen_set.clear();

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
            arc_sum += rep->get_rc();
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
          scc_internal++;

          while (!pending.empty() && pending.back() != rep)
          {
            scc_union(pending.back(), rep);
            scc_internal++;
            pending.pop_back();
          }
        }
        else if (rep_loc.is_immutable())
        {
          // Already frozen. No action needed.
        }
        else if (rep_loc.is_region())
        {
          if (rep_loc.to_region() == region)
          {
            region->remove(h);
            h->set_location(Location::from_raw(Location::Pending));
            frozen_set.insert(h);

            pending.push_back(h);
            dfs.push_back(post_order_mark(h));
            frozen_internal += trace_fields(h, dfs, region, &frozen_set);
          }
          else
            worklist.push_back(h);
        }
      }

      // Count unfrozen-to-frozen field references.
      RC unfrozen_to_frozen = 0;
      auto& program = Program::get();

      region->for_each_header([&](Header* h) {
        auto count_fn = [&](Header* target) {
          if (frozen_set.count(target))
            unfrozen_to_frozen++;
        };

        if (program.is_array(h->get_type_id()))
          static_cast<Array*>(h)->trace_fn(count_fn);
        else
          static_cast<Object*>(h)->trace_fn(count_fn);
      });

      assert(frozen_internal >= scc_internal);
      RC frozen_cross = frozen_internal - scc_internal;
      assert(arc_sum >= frozen_cross + unfrozen_to_frozen);
      RC stack_adjustment =
        arc_sum - frozen_cross - unfrozen_to_frozen;

      // Adjust stack_rc. May free the region if stack_rc hits 0 with
      // no owner.
      if (stack_adjustment > 0)
        region->stack_dec(stack_adjustment);
    }

    return true;
  }
}
