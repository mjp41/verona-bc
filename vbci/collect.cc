/**
 * Implements a two-phase collector for objects and arrays.
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
#include "header_queue.h"
#include "mutable_cycle_candidates.h"
#include "object.h"
#include "traverse.h"

#include <queue>

namespace vbci
{
  struct Collector
  {
    std::queue<Header*> immutable_work;
    HeaderQueue headers_to_finalize;
    HeaderQueue headers_to_delete;
    HeaderQueue cowns_to_finalize;

    void enqueue_mutable(Header* header)
    {
      header->set_state(HeaderState::finalizing());
      headers_to_finalize.push(header);
    }

    void enqueue_finalizing(HeaderQueue headers)
    {
      headers_to_finalize.append(std::move(headers));
    }

    void enqueue_shared(Header* header)
    {
      assert(header->state().is_shared());

      if (!header->state().is_cown())
      {
        immutable_work.push(header);
        return;
      }

      auto* cown = static_cast<Cown*>(header);
      cowns_to_finalize.push_front(cown);
    }

    void drain_batch()
    {
      auto& program = Program::get();

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

      while (!headers_to_delete.empty())
      {
        auto* header = headers_to_delete.pop();
        LOG(Trace) << "Deleting work item: " << static_cast<void*>(header);

        assert(header->state().is_finalizing());
        delete[] reinterpret_cast<uint8_t*>(header);
      }
    }

    HeaderQueue claim_immutable_scc(Header* root)
    {
      assert(root->state().is_shared() && !root->state().is_cown());
      root->set_state(HeaderState::finalizing());
      HeaderQueue finalizing;

      auto add_member = [&](Header* header) {
        if (!header->state().is_scc_ptr())
          return false;

        auto* representative = Header::find(header);
        if (representative->state().is_finalizing())
        {
          header->set_state(HeaderState::finalizing());
          return true;
        }

        return false;
      };

      // Only discover members of this SCC. Normal finalization drops external
      // immutable references and queues them if their ARC reaches zero.
      traverse_intrusive(
        root,
        add_member,
        [&](Header* header) { finalizing.push(header); });

      return finalizing;
    }

    void drain()
    {
      while (true)
      {
        drain_batch();

        auto garbage = classify_mutable_cycle_candidates();
        if (!garbage.empty())
        {
          enqueue_finalizing(std::move(garbage));
          continue;
        }

        if (!immutable_work.empty())
        {
          auto* header = immutable_work.front();
          immutable_work.pop();
          enqueue_finalizing(claim_immutable_scc(header));
          continue;
        }

        if (!cowns_to_finalize.empty())
        {
          auto* cown = static_cast<Cown*>(cowns_to_finalize.pop());
          LOG(Trace) << "Finalizing cown: " << static_cast<void*>(cown);
          cown->finalize();
          drain_batch();
          LOG(Trace) << "Deleting work item: " << static_cast<void*>(cown);
          cown->destroy();
          continue;
        }

        break;
      }
    }
  };

  static thread_local Collector* current_collector = nullptr;
  static thread_local bool suppress_finalizers = false;

  struct CurrentCollector
  {
    Collector* active;
    Collector* previous;

    explicit CurrentCollector(Collector& collector)
    : active(&collector), previous(current_collector)
    {
      current_collector = active;
    }

    ~CurrentCollector()
    {
      assert(current_collector == active);
      current_collector = previous;
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
      use_collector([&](Collector& collector) {
        collector.enqueue_shared(h);
      });
      return;
    }

    assert(h->state().is_mutable());
    use_collector([&](Collector& collector) {
      collector.enqueue_mutable(h);
    });
  }

  void collect_finalizing(HeaderQueue headers)
  {
    if (headers.empty())
      return;

    use_collector([&](Collector& collector) {
      collector.enqueue_finalizing(std::move(headers));
    });
  }

  void collect_mutable_cycles()
  {
    while (true)
    {
      auto garbage = classify_mutable_cycle_candidates();
      if (garbage.empty())
        return;

      collect_finalizing(std::move(garbage));
    }
  }

}
