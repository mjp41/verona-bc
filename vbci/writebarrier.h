#pragma once

#include "location.h"
#include "register.h"

namespace vbci::writebarrier
{
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

  inline void
  init(Location store_loc, void* addr, ValueType type, Register next)
  {
    if (next->is_readonly())
      Value::error(Error::BadStore);

    auto next_loc = next->location();
    check_store(store_loc, next_loc);
    next->to_addr(type, addr);
    next.clear_unsafe();
  }

  template<bool is_move>
  ValueTransfer
  exchange(Location store_loc, void* addr, ValueType type, Reg<is_move> next)
  {
    auto prev = Value::from_addr(type, addr);
    auto next_loc = next->location();

    if (next->is_readonly())
      Value::error(Error::BadStore);

    check_store(store_loc, next_loc);

    next->to_addr(type, addr);

    if constexpr (is_move)
    {
      next.clear_unsafe();
    }
    else
    {
      next->inc();
    }

    return ValueTransfer(prev);
  }

  inline void drop(Value& value)
  {
    auto value_loc = value.location();

    if (
      value_loc.is_immortal() || value_loc.is_stack() || value_loc.is_pending())
      return;

    value.dec();
  }
}
