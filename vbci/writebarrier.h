#pragma once

#include "location.h"
#include "register.h"

namespace vbci::writebarrier
{
  inline void check_value(const Value& value)
  {
    if (
      value.is_readonly() ||
      value.get_value_type() == ValueType::RegisterRef)
      Value::error(Error::BadStore);
  }

  inline void check_exchange(Location store_loc, const Value& value)
  {
    if (store_loc.is_shared() || store_loc.is_immortal())
      Value::error(Error::BadStore);

    check_value(value);
  }

  inline void
  init(Location store_loc, void* addr, ValueType type, Register next)
  {
    check_value(next.borrow());

    if (store_loc.is_shared() || store_loc.is_immortal())
    {
      auto next_loc = next->location();

      if (!next_loc.is_shared() && !next_loc.is_immortal())
        Value::error(Error::BadStore);
    }

    next->to_addr(type, addr);
    next.clear_unsafe();
  }

  template<bool is_move>
  ValueTransfer
  exchange(Location store_loc, void* addr, ValueType type, Reg<is_move> next)
  {
    auto prev = Value::from_addr(type, addr);
    check_exchange(store_loc, next.borrow());

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

    if (value_loc.is_immortal() || value_loc.is_pending())
      return;

    value.dec();
  }
}
