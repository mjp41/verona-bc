#pragma once

#include <compare>
#include <cstdint>

namespace vbci
{
  struct FrameId
  {
  private:
    uintptr_t value;

    explicit constexpr FrameId(uintptr_t value) : value(value) {}

  public:
    static constexpr FrameId first()
    {
      return FrameId(0);
    }

    static constexpr FrameId from_raw(uintptr_t raw)
    {
      return FrameId(raw);
    }

    constexpr uintptr_t raw() const
    {
      return value;
    }

    constexpr FrameId next() const
    {
      return FrameId(value + 1);
    }

    auto operator<=>(const FrameId&) const = default;
  };
}
