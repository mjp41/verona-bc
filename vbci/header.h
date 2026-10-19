#pragma once

#include "collect.h"
#include "header_state.h"
#include "ident.h"
#include "mutable_cycle_candidates.h"
#include "register.h"
#include "value.h"

namespace vbci
{
  struct Header
  {
  private:
    HeaderState header_state;
    uint32_t type_id;

  protected:
    Header(HeaderState::Snapshot state, uint32_t type_id)
    : header_state(state), type_id(type_id)
    {}

    explicit Header(uint32_t type_id)
    : header_state(HeaderState::cown()), type_id(type_id)
    {}

    void mark_immortal()
    {
      set_state(HeaderState::immortal());
    }

  public:
    uint32_t get_type_id() const
    {
      return type_id;
    }

    RC get_rc()
    {
      auto current = state();
      if (current.is_mutable_cycle_candidate())
        return mutable_cycle_candidate_count(
          current.mutable_cycle_candidate_index());

      return static_cast<RC>(header_state.count());
    }

    void set_rc(RC v)
    {
      header_state.set_count(v);
    }

    Header* next() const
    {
      return header_state.next();
    }

    void set_next(Header* next)
    {
      header_state.set_next(next);
    }

    HeaderState::Snapshot state() const
    {
      return header_state.load();
    }

    void set_state(HeaderState::Snapshot state)
    {
      if (this->state().is_mutable_cycle_candidate())
      {
        auto current = this->state();
        auto rc = remove_mutable_cycle_candidate(
          current.mutable_cycle_candidate_index());
        header_state.set(state, rc);
        return;
      }

      header_state.transition(state);
    }

    void restore_mutable_cycle_candidate(RC rc)
    {
      assert(state().is_mutable_cycle_candidate());
      assert(rc > 0);
      header_state.set(HeaderState::mutable_(), rc);
    }

    // Union-find representative lookup with path compression.
    // Used by frozen SCC objects to find their SCC root.
    static Header* find(Header* h)
    {
      auto state = h->state();
      if (!state.is_scc_ptr())
        return h;

      auto target = state.scc_target();
      auto root = find(target);

      // Path compression: point directly to root.
      if (root != target)
        h->set_state(HeaderState::scc_ptr(root));

      return root;
    }

    void inc()
    {
      auto state = this->state();
      if (state.is_scc_ptr())
      {
        find(this)->inc();
        return;
      }

      if (state.is_immortal())
        return;

      if (state.is_finalizing())
        return;

      if (state.is_mutable_cycle_candidate())
      {
        auto count = remove_mutable_cycle_candidate(
          state.mutable_cycle_candidate_index());
        header_state.set(HeaderState::mutable_(), count + 1);
        return;
      }

      if (state.is_shared())
      {
        auto previous = header_state.increment_shared_count();
        assert(previous > 0);
        return;
      }

      assert(state.is_mutable());
      auto previous = header_state.increment_count();
      assert(previous > 0);
    }

    void dec()
    {
      auto state = this->state();
      if (state.is_scc_ptr())
      {
        find(this)->dec();
        return;
      }

      if (state.is_immortal())
        return;

      if (state.is_finalizing())
        return;

      if (state.is_mutable_cycle_candidate())
      {
        auto previous = decrement_mutable_cycle_candidate(
          state.mutable_cycle_candidate_index());
        if (previous == 1)
        {
          header_state.set(HeaderState::mutable_(), 0);
          collect(this);
        }
        return;
      }

      if (state.is_shared())
      {
        if (header_state.decrement_shared_count())
          collect(this);

        return;
      }

      assert(state.is_mutable());
      auto previous = header_state.decrement_count();
      assert(previous > 0);
      if (previous == 1)
      {
        collect(this);
        return;
      }

      if (auto index = add_mutable_cycle_candidate(this, previous - 1))
        header_state.transition(
          HeaderState::mutable_cycle_candidate(*index));
    }
  };

  static_assert(alignof(Header) >= 8);
}
