#include "mutable_cycle_candidates.h"

#include "array.h"
#include "header.h"
#include "header_queue.h"
#include "object.h"
#include "program.h"
#include "traverse.h"

#include <algorithm>
#include <vector>

namespace vbci
{
  namespace
  {
    struct CandidateSlot
    {
      union
      {
        Header* header;
        size_t next_free;
      };
      RC rc;
      CandidateSlot() : next_free(0), rc(0) {}
    };

    struct MutableCycleCandidates
    {
      static constexpr size_t NoSlot = size_t(-1);

      std::vector<CandidateSlot> slots;
      size_t free_list = NoSlot;

      CandidateSlot& at(size_t index)
      {
        return slots.at(index);
      }

      size_t allocate()
      {
        if (free_list != NoSlot)
        {
          auto index = free_list;
          free_list = at(index).next_free;
          return index;
        }

        slots.emplace_back();
        return slots.size() - 1;
      }

      void release(size_t index)
      {
        auto& slot = at(index);
        slot.rc = 0;
        slot.next_free = free_list;
        free_list = index;
      }

      size_t add(Header* header, RC rc)
      {
        assert(header);
        assert(rc > 0);

        auto index = allocate();
        auto& slot = at(index);
        slot.header = header;
        slot.rc = rc;
        return index;
      }

      CandidateSlot& get(size_t index)
      {
        auto& slot = at(index);
        assert(slot.rc > 0);
        return slot;
      }

      RC count(size_t index)
      {
        return get(index).rc;
      }

      RC remove(size_t index)
      {
        auto rc = get(index).rc;
        release(index);
        return rc;
      }

      Header* restore(size_t index)
      {
        auto* header = get(index).header;
        auto rc = remove(index);
        header->restore_mutable_cycle_candidate(rc);
        return header;
      }

      RC decrement(size_t index)
      {
        auto& slot = get(index);
        auto previous = slot.rc;
        assert(previous > 0);

        if (previous == 1)
          release(index);
        else
          slot.rc--;

        return previous;
      }

      static void trace(Header* header, auto&& fn)
      {
        auto& program = Program::get();
        if (program.is_array(header->get_type_id()))
          static_cast<Array*>(header)->trace_fn(fn);
        else
          static_cast<Object*>(header)->trace_fn(fn);
      }

      struct Classification
      {
        struct TrialSlot
        {
          // The trial word is reused across classification phases:
          //   0: unvisited
          //   direct count: offset SCC RC, with Link distinguishing pending
          //   indirect: tagged union-find representative Header*
          //   stack link: tagged next representative Header*
          static constexpr size_t Indirect = 1;
          static constexpr size_t Link = 2;
          static constexpr size_t DirectShift = 2;
          static constexpr size_t TagMask = 3;

          size_t trial = 0;

          bool is_unvisited() const
          {
            return trial == 0;
          }

          bool is_indirect() const
          {
            return (trial & Indirect) != 0;
          }

          bool is_pending() const
          {
            assert(!is_unvisited());
            return !is_indirect() && ((trial & Link) != 0);
          }

          bool is_complete() const
          {
            assert(!is_unvisited());
            return !is_indirect() && ((trial & Link) == 0);
          }

          Header* representative() const
          {
            assert(is_indirect());
            return reinterpret_cast<Header*>(trial & ~TagMask);
          }

          size_t trial_rc() const
          {
            assert(!is_unvisited());
            assert(!is_indirect());
            return (trial >> DirectShift) - 1;
          }

          size_t trial_dec()
          {
            auto count = trial_rc();
            assert(count > 0);
            trial -= size_t(1) << DirectShift;
            return count - 1;
          }

          void set_pending(size_t count)
          {
            trial = ((count + 1) << DirectShift) | Link;
          }

          void set_complete(size_t count)
          {
            trial = (count + 1) << DirectShift;
          }

          void set_indirect(Header* representative)
          {
            auto pointer = reinterpret_cast<size_t>(representative);
            assert((pointer & TagMask) == 0);
            trial = pointer | Indirect;
          }

          void set_list_next(Header* header)
          {
            auto pointer = reinterpret_cast<size_t>(header);
            assert((pointer & TagMask) == 0);
            trial = pointer | Link;
          }

          Header* list_next() const
          {
            assert(!is_indirect());
            assert((trial & Link) != 0);
            return reinterpret_cast<Header*>(trial & ~TagMask);
          }
        };

        // Zero-count representatives no longer need their trial RC, so their
        // direct slot can temporarily hold this intrusive stack link. A
        // representative is popped before it becomes Finalizing and releases
        // that slot.
        struct RepresentativeStack
        {
          Classification& classification;
          Header* top = nullptr;

          explicit RepresentativeStack(Classification& classification)
          : classification(classification)
          {}

          bool empty() const
          {
            return top == nullptr;
          }

          void push(Header* representative)
          {
            auto& slot =
              classification.representative_trial(representative);
            slot.set_list_next(top);
            top = representative;
          }

          Header* pop()
          {
            assert(!empty());
            auto* representative = top;
            top =
              classification.representative_trial(representative).list_next();
            return representative;
          }
        };

        MutableCycleCandidates& candidates;
        Program& program = Program::get();
        std::vector<TrialSlot> trials;
        std::vector<Header*> pending;
        RepresentativeStack zero;
        HeaderQueue garbage;

        explicit Classification(MutableCycleCandidates& candidates)
        : candidates(candidates),
          trials(candidates.slots.size()),
          zero(*this)
        {}

        TrialSlot& trial(size_t index)
        {
          return trials.at(index);
        }

        size_t add_scan_member(Header* header)
        {
          auto index = candidates.add(header, header->get_rc());
          if (index == trials.size())
            trials.emplace_back();
          else
            trial(index) = {};

          header->set_state(HeaderState::mutable_cycle_candidate(index));
          return index;
        }

        Header* find(size_t index)
        {
          while (true)
          {
            auto& slot = trial(index);
            if (!slot.is_indirect())
              return candidates.get(index).header;

            auto target = slot.representative();
            auto state = target->state();
            if (state.is_finalizing())
              return target;

            assert(state.is_mutable_cycle_candidate());
            auto parent_index = state.mutable_cycle_candidate_index();
            auto& parent = trial(parent_index);
            if (!parent.is_indirect())
              return target;

            // Point each visited slot to its grandparent.
            slot.set_indirect(parent.representative());
            index = parent_index;
          }
        }

        size_t representative_index(Header* representative)
        {
          auto state = representative->state();
          assert(state.is_mutable_cycle_candidate());
          auto index = state.mutable_cycle_candidate_index();
          assert(candidates.get(index).header == representative);
          return index;
        }

        TrialSlot& representative_trial(Header* representative)
        {
          return trial(representative_index(representative));
        }

        bool scc_union(Header* child, Header* representative)
        {
          auto child_state = child->state();
          auto rep_state = representative->state();
          assert(child_state.is_mutable_cycle_candidate());
          assert(rep_state.is_mutable_cycle_candidate());

          auto child_rep =
            find(child_state.mutable_cycle_candidate_index());
          auto rep = find(rep_state.mutable_cycle_candidate_index());
          // An overlapping back edge can leave the pending completion anchor
          // inside an SCC already represented by rep. Keep that anchor pending.
          if (child_rep == rep)
            return false;

          if (
            reinterpret_cast<uintptr_t>(child_rep) >
            reinterpret_cast<uintptr_t>(rep))
            std::swap(child_rep, rep);

          auto& child_slot = representative_trial(child_rep);
          auto& rep_slot = representative_trial(rep);
          assert(child_slot.is_pending());
          assert(rep_slot.is_pending());

          // Joining two pending components makes their DFS tree edge internal.
          assert(rep_slot.trial_rc() + child_slot.trial_rc() > 0);
          auto count = rep_slot.trial_rc() + child_slot.trial_rc() - 1;
          rep_slot.set_pending(count);
          child_slot.set_indirect(rep);
          return true;
        }

        void enqueue_zero(Header* representative)
        {
          auto& slot = representative_trial(representative);
          assert(slot.trial_rc() == 0);
          zero.push(representative);
        }

        bool visit_candidate(Header* header)
        {
          auto state = header->state();
          if (
            !state.is_mutable() ||
            !program.can_cycle(header->get_type_id()))
            return false;

          auto index = state.is_mutable_cycle_candidate() ?
            state.mutable_cycle_candidate_index() :
            add_scan_member(header);
          auto representative = find(index);
          auto& slot = representative_trial(representative);

          if (slot.is_unvisited())
          {
            slot.set_pending(
              candidates.get(representative_index(representative)).rc);
            pending.push_back(representative);
            return true;
          }

          if (slot.is_pending())
          {
            // Pending SCCs are checked for zero trial RC when postorder
            // completes them.
            slot.trial_dec();

            while (!pending.empty() && pending.back() != representative)
            {
              if (!scc_union(pending.back(), representative))
                break;

              pending.pop_back();
            }

            return false;
          }

          assert(slot.is_complete());
          return false;
        }

        bool visit_zero_scc(Header* representative, Header* header)
        {
          auto state = header->state();
          if (state.is_finalizing())
            return false;

          // The first visit is the zero-count representative itself. Claiming
          // it makes Finalizing the membership marker for the rest of its SCC.
          if (header == representative)
          {
            assert(state.is_mutable_cycle_candidate());
            header->set_state(HeaderState::finalizing());
            garbage.push(header);
            return true;
          }

          if (!state.is_mutable_cycle_candidate())
            return false;

          auto target =
            find(state.mutable_cycle_candidate_index());

          // Union-find links never cross SCCs. Reaching a Finalizing
          // representative therefore proves membership in this claimed SCC.
          if (target->state().is_finalizing())
          {
            header->set_state(HeaderState::finalizing());
            garbage.push(header);
            return true;
          }

          auto& target_slot = representative_trial(target);
          assert(target_slot.is_complete());
          if (target_slot.trial_dec() == 0)
            enqueue_zero(target);
          return false;
        }

        // Reconstruct each zero-count SCC, claim every member as Finalizing,
        // decrement outgoing SCC counts, and build the finalization queue.
        void drain_zero()
        {
          while (!zero.empty())
          {
            auto representative = zero.pop();

            traverse_graph(
              representative,
              [&](Header* header, auto add_child) {
                MutableCycleCandidates::trace(header, add_child);
              },
              [&](Header* header) {
                return visit_zero_scc(representative, header);
              });
          }
        }

        // Completing a pending root produces one SCC condensation-graph count.
        // A zero SCC is claimed immediately, which can expose more zero SCCs.
        void complete_candidate(Header* header)
        {
          auto state = header->state();
          assert(state.is_mutable_cycle_candidate());
          auto index = state.mutable_cycle_candidate_index();
          if (!pending.empty() && pending.back() == header)
          {
            pending.pop_back();
            auto representative = find(index);
            auto& slot = representative_trial(representative);
            assert(slot.is_pending());
            auto count = slot.trial_rc();
            if (count != 0)
            {
              slot.set_complete(count);
              return;
            }

            enqueue_zero(representative);
            drain_zero();
          }
        }

        void discover_sccs()
        {
          // Path-based SCC discovery mirrors freezing. Direct trial counts
          // begin at real RC; only proven intra-SCC edges are subtracted.
          for (size_t index = 0; index < candidates.slots.size(); index++)
          {
            auto& slot = candidates.slots.at(index);
            if (slot.rc == 0 || !trial(index).is_unvisited())
              continue;

            auto state = slot.header->state();
            assert(state.is_mutable_cycle_candidate());
            assert(state.mutable_cycle_candidate_index() == index);

            traverse_graph(
              slot.header,
              [&](Header* header, auto add_child) {
                MutableCycleCandidates::trace(header, add_child);
              },
              [&](Header* header) { return visit_candidate(header); },
              [&](Header* header) { complete_candidate(header); });
          }

          assert(pending.empty());
          assert(zero.empty());
        }

        void restore_live_candidates()
        {
          // Eager zero-SCC processing released every garbage slot. Every slot
          // still active after discovery is therefore independently live.
          for (size_t index = 0; index < candidates.slots.size(); index++)
          {
            auto& slot = candidates.slots.at(index);
            if (slot.rc == 0)
              continue;

            candidates.restore(index);
          }
        }

        // Finalizing members were claimed during SCC reconstruction, but no
        // finalizer or field drop can run until this list reaches the collector.
        Header* claim_garbage()
        {
          return garbage.release();
        }
      };

      Header* classify()
      {
        if (!std::ranges::any_of(
              slots, [](const auto& slot) { return slot.rc != 0; }))
          return nullptr;

        Classification classification(*this);
        classification.discover_sccs();
        auto* garbage = classification.claim_garbage();
        classification.restore_live_candidates();
        return garbage;
      }
    };

    thread_local MutableCycleCandidates candidates;
  }

  std::optional<size_t> add_mutable_cycle_candidate(Header* header, RC rc)
  {
    if (!Program::get().can_cycle(header->get_type_id()))
      return {};

    return candidates.add(header, rc);
  }

  RC mutable_cycle_candidate_count(size_t index)
  {
    return candidates.count(index);
  }

  RC remove_mutable_cycle_candidate(size_t index)
  {
    return candidates.remove(index);
  }

  RC decrement_mutable_cycle_candidate(size_t index)
  {
    return candidates.decrement(index);
  }

  Header* classify_mutable_cycle_candidates()
  {
    return candidates.classify();
  }
}
