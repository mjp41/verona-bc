#pragma once

#include "header_state.h"
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

  inline void check_exchange(
    HeaderState::Snapshot store_state, const Value& value)
  {
    if (store_state.is_shared() || store_state.is_immortal())
      Value::error(Error::BadStore);

    check_value(value);
  }

  inline void
  init(
    HeaderState::Snapshot store_state,
    void* addr,
    ValueType type,
    Register next)
  {
    check_value(next.borrow());

    if (store_state.is_shared() || store_state.is_immortal())
    {
      auto next_state = next->state();

      if (!next_state.is_shared() && !next_state.is_immortal())
        Value::error(Error::BadStore);
    }

    next->to_addr(type, addr);
    next.clear_unsafe();
  }

  template<bool is_move>
  ValueTransfer exchange(
    HeaderState::Snapshot store_state,
    void* addr,
    ValueType type,
    Reg<is_move> next)
  {
    auto prev = Value::from_addr(type, addr);
    check_exchange(store_state, next.borrow());

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
    auto value_state = value.state();

    if (value_state.is_immortal() || value_state.is_pending())
      return;

    value.dec();
  }
}
