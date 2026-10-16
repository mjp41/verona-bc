#pragma once

// Delay deallocation to handle re-entrancy.

namespace vbci
{
  struct Header;
  struct Region;

  struct SuppressFinalizers
  {
    SuppressFinalizers();
    ~SuppressFinalizers();

    SuppressFinalizers(const SuppressFinalizers&) = delete;
    SuppressFinalizers& operator=(const SuppressFinalizers&) = delete;

    static bool active();
  };

  void collect(Header* h);
  void collect(Header* headers, Region& region);
  void collect(Region* region);
} // namespace vbci
