#include "mutable_cycle_candidates.h"

#include "header.h"
#include "program.h"
#include "region_ext.h"

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

      void restore(Region& region)
      {
        region.for_each_header([&](Header* header) {
          auto state = header->state();
          if (!state.is_mutable_cycle_candidate())
            return;

          auto index = state.mutable_cycle_candidate_index();
          auto rc = remove(index);
          header->restore_mutable_cycle_candidate(rc);
        });
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

  void restore_mutable_cycle_candidates(Region& region)
  {
    candidates.restore(region);
  }
}
