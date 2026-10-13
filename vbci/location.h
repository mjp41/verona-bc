#pragma once

#include "ident.h"

namespace vbci
{
  struct Region;
  struct Header;

  struct Location
  {
  private:
    static constexpr auto Immutable = uintptr_t(0x1);
    static constexpr auto Pending = uintptr_t(0x2);
    static constexpr auto Immortal = uintptr_t(0x3);
    static constexpr auto SccPtr = uintptr_t(0x4);
    static constexpr auto Mask = uintptr_t(0x7);

    uintptr_t value;

    // Location objects are intentionally only constructible via the public
    // static factory methods (and internal helpers). The default constructor
    // is deleted and the raw uintptr_t constructor is kept private to enforce
    // invariants on the encoded location value.
    Location() = delete;
    constexpr Location(uintptr_t v) : value(v) {}

  public:
    static constexpr Location immutable()
    {
      return Location(Immutable);
    };

    static constexpr Location immortal()
    {
      return Location(Immortal);
    };

    static constexpr Location pending()
    {
      return Location(Pending);
    };

    explicit Location(Region* r) : value(reinterpret_cast<uintptr_t>(r)) {}

    bool is_region() const
    {
      return (value & Mask) == 0;
    }

    bool is_immortal() const
    {
      return (value & Mask) == Immortal;
    }

    bool is_immutable() const
    {
      auto tag = value & Mask;
      return tag == Immutable || tag == SccPtr;
    }

    bool is_scc_ptr() const
    {
      return (value & Mask) == SccPtr;
    }

    bool is_pending() const
    {
      return (value & Mask) == Pending;
    }

    Region* to_region() const
    {
      assert(is_region());
      return reinterpret_cast<Region*>(value);
    }

    static Location scc_ptr(Header* h)
    {
      return Location(SccPtr | reinterpret_cast<uintptr_t>(h));
    }

    Header* scc_target() const
    {
      assert(is_scc_ptr());
      return reinterpret_cast<Header*>(value & ~Mask);
    }
  };
}
