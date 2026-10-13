#pragma once

#include "location.h"
#include "register.h"

namespace vbci::writebarrier
{
  inline bool is_stack_reference_to(Location store_loc, Location value_loc)
  {
    assert(
      !store_loc.is_region() || !value_loc.is_region() ||
      store_loc.to_region() == value_loc.to_region());
    return store_loc.is_stack() && value_loc.is_region();
  }

  inline void check_store(Location store_loc, Location value_loc)
  {
    if (store_loc.is_immutable() || store_loc.is_immortal())
      Value::error(Error::BadStore);

    if (!value_loc.is_stack())
      return;

    if (
      !store_loc.is_stack() ||
      store_loc.stack_index() < value_loc.stack_index())
      Value::error(Error::BadStore);
  }

  inline void init(Location store_loc, void* addr, ValueType type, Register next)
  {
    if (next->is_readonly())
      Value::error(Error::BadStore);

    auto next_loc = next->location();
    check_store(store_loc, next_loc);
    next->to_addr(type, addr);
    next.clear_unsafe();

    if (next_loc.is_region() && !is_stack_reference_to(store_loc, next_loc))
      next_loc.to_region()->stack_dec();
  }

  template<bool is_move>
  ValueTransfer
  exchange(Location store_loc, void* addr, ValueType type, Reg<is_move> next)
  {
    auto prev = Value::from_addr(type, addr);
    auto prev_loc = prev.location();
    auto next_loc = next->location();
    check_store(store_loc, next_loc);

    next->to_addr(type, addr);

    if (
      prev_loc.is_region() && !is_stack_reference_to(store_loc, prev_loc))
      prev_loc.to_region()->stack_inc();

    if constexpr (is_move)
    {
      next.clear_unsafe();

      if (
        next_loc.is_region() &&
        !is_stack_reference_to(store_loc, next_loc))
        next_loc.to_region()->stack_dec();
    }
    else
    {
      next->field_inc();

      if (is_stack_reference_to(store_loc, next_loc))
        next_loc.to_region()->stack_inc();
    }

    return ValueTransfer(prev);
  }

  inline void drop(Location store_loc, Value& value)
  {
    auto value_loc = value.location();

    if (
      value_loc.is_immortal() || value_loc.is_stack() ||
      value_loc.is_pending())
      return;

    bool stack_reference = is_stack_reference_to(store_loc, value_loc);
    value.field_dec();

    if (stack_reference)
      value_loc.to_region()->stack_dec();
  }
}
