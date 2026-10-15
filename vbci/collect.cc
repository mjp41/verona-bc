/**
 * Implements a two-phase collector for objects, regions, and arrays.
 *
 * Phase 1 (finalizing): Run finalizers and drop field references. This may
 * cause other objects' RCs to hit 0, adding them to the finalizing worklist.
 * Drain until empty.
 *
 * Phase 2 (deleting): Free memory for all finalized objects.
 *
 * This separation ensures that all finalizers run before any memory is freed,
 * preventing use-after-free when finalizers reference sibling objects.
 */

#include "collect.h"

#include "array.h"
#include "cown.h"
#include "header.h"
#include "object.h"
#include "region.h"

#include <queue>
#include <vector>

namespace vbci
{
  enum class WorkType
  {
    Region,
    Immutable
  };

  struct WorkItem
  {
    WorkType type;
    union
    {
      Region* region;
      Header* header;
    };

    explicit WorkItem(Region* region)
    : type(WorkType::Region), region(region)
    {}

    explicit WorkItem(Header* header)
    : type(WorkType::Immutable), header(header)
    {}
  };

  struct HeaderQueue
  {
    Header* first = nullptr;
    Header* last = nullptr;

    bool empty() const
    {
      return first == nullptr;
    }

    void push(Header* header)
    {
      header->set_next(nullptr);

      if (last)
        last->set_next(header);
      else
        first = header;

      last = header;
    }

    void push_front(Header* header)
    {
      header->set_next(first);
      first = header;
      if (!last)
        last = header;
    }

    Header* pop()
    {
      assert(first);
      auto* header = first;
      first = header->next();
      if (!first)
        last = nullptr;
      return header;
    }
  };

  struct Collector
  {
    std::queue<WorkItem> work;
    HeaderQueue headers_to_finalize;
    HeaderQueue headers_to_delete;
    HeaderQueue cowns_to_finalize;
    Region* collecting_region = nullptr;
    Region* region_to_delete = nullptr;

    void enqueue_header(Header* header)
    {
      headers_to_finalize.push(header);
    }

    void enqueue(Header* header, Region* region)
    {
      header->set_state(HeaderState::finalizing());

      if (collecting_region)
      {
        assert(collecting_region == region);
        enqueue_header(header);
        return;
      }

      assert(current_region() == region);
      collecting_region = region;
      enqueue_header(header);
      drain_batch();
      collecting_region = nullptr;
    }

    void enqueue(Region* region)
    {
      if (collecting_region == region)
      {
        assert(region_to_delete == nullptr);
        region_to_delete = region;
      }
      else
      {
        work.emplace(region);
      }
    }

    void enqueue(Header* header)
    {
      assert(header->state().is_shared());

      if (!header->state().is_cown())
      {
        work.emplace(header);
        return;
      }

      auto* cown = static_cast<Cown*>(header);
      cowns_to_finalize.push_front(cown);
    }

    void drain_batch()
    {
      auto& program = Program::get();

      while (true)
      {
        while (!headers_to_finalize.empty())
        {
          auto* header = headers_to_finalize.pop();
          LOG(Trace) << "Finalizing work item: " << static_cast<void*>(header);

          if (program.is_array(header->get_type_id()))
            static_cast<Array*>(header)->finalize();
          else
            static_cast<Object*>(header)->finalize();
          headers_to_delete.push_front(header);
        }

        if (!region_to_delete)
          break;

        auto* header = region_to_delete->take_header();
        if (!header)
          break;

        do
        {
          header->set_state(HeaderState::finalizing());
          enqueue_header(header);
          header = region_to_delete->take_header();
        } while (header);
      }

      // Delete storage only after every generation has been finalized.
      while (!headers_to_delete.empty())
      {
        auto* header = headers_to_delete.pop();
        LOG(Trace) << "Deleting work item: " << static_cast<void*>(header);

        assert(
          header->state().is_pending() ||
          header->state().is_finalizing());
        delete[] reinterpret_cast<uint8_t*>(header);
      }

      if (region_to_delete)
      {
        LOG(Trace) << "Deleting work item: "
                   << static_cast<void*>(region_to_delete);
        region_to_delete->destroy();
        region_to_delete = nullptr;
      }
    }

    void enqueue_scc_members(Header* root)
    {
      assert(root->state().is_shared() && !root->state().is_cown());
      root->set_state(HeaderState::pending());
      std::vector<Header*> work;
      work.push_back(root);

      auto& program = Program::get();
      auto add_member = [&](Header* header) {
        if (!header->state().is_scc_ptr())
          return;

        auto* representative = Header::find(header);
        if (representative->state().is_pending())
        {
          header->set_state(HeaderState::pending());
          work.push_back(header);
        }
      };

      while (!work.empty())
      {
        auto* header = work.back();
        work.pop_back();
        enqueue_header(header);

        // Only discover members of this SCC. Normal finalization drops
        // external immutable references and queues them if their ARC reaches
        // zero.
        if (program.is_array(header->get_type_id()))
          static_cast<Array*>(header)->trace_fn(add_member);
        else
          static_cast<Object*>(header)->trace_fn(add_member);
      }
    }

    void drain()
    {
      while (!work.empty() || !cowns_to_finalize.empty())
      {
        if (!work.empty())
        {
          auto item = work.front();
          work.pop();

          if (item.type == WorkType::Immutable)
          {
            collecting_region = current_region();
            enqueue_scc_members(item.header);
            drain_batch();
            collecting_region = nullptr;
            continue;
          }

          collecting_region = item.region;

          ScopeRegion region_scope(*collecting_region);
          region_to_delete = collecting_region;
          drain_batch();

          collecting_region = nullptr;
          continue;
        }

        auto* cown = static_cast<Cown*>(cowns_to_finalize.pop());
        collecting_region = &cown->get_allocation_group();

        ScopeRegion region_scope(*collecting_region);
        LOG(Trace) << "Finalizing cown: " << static_cast<void*>(cown);
        cown->finalize();
        drain_batch();
        LOG(Trace) << "Deleting work item: " << static_cast<void*>(cown);
        cown->destroy();

        collecting_region = nullptr;
      }
    }
  };

  static thread_local Collector* current_collector = nullptr;
  static thread_local bool suppress_finalizers = false;

  struct CurrentCollector
  {
    explicit CurrentCollector(Collector& collector)
    {
      assert(current_collector == nullptr);
      current_collector = &collector;
    }

    ~CurrentCollector()
    {
      assert(current_collector != nullptr);
      current_collector = nullptr;
    }

    CurrentCollector(const CurrentCollector&) = delete;
    CurrentCollector& operator=(const CurrentCollector&) = delete;
  };

  template<typename Fn>
  static void use_collector(Fn&& fn)
  {
    if (current_collector)
    {
      fn(*current_collector);
      return;
    }

    Collector collector;
    CurrentCollector current(collector);
    fn(collector);
    collector.drain();
  }

  SuppressFinalizers::SuppressFinalizers()
  {
    assert(!suppress_finalizers);
    suppress_finalizers = true;
  }

  SuppressFinalizers::~SuppressFinalizers()
  {
    assert(suppress_finalizers);
    suppress_finalizers = false;
  }

  bool SuppressFinalizers::active()
  {
    return suppress_finalizers;
  }

  void collect(Header* h)
  {
    if (h->state().is_shared())
    {
      use_collector([&](Collector& collector) { collector.enqueue(h); });
      return;
    }

    assert(h->state().is_mutable());
    auto* region = current_region();
    assert(region);

    // Detach explicit header work items from the region up front so a later
    // Region work item won't re-finalize them.
    if (!region->remove(h))
      return;

    use_collector(
      [&](Collector& collector) { collector.enqueue(h, region); });
  }

  void collect(Region* region)
  {
    if (!region->begin_finalizing())
      return;

    use_collector([&](Collector& collector) { collector.enqueue(region); });
  }

}
