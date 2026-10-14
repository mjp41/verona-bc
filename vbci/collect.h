#pragma once

// Delay deallocation to handle re-entrancy.

namespace vbci
{
  struct Header;
  struct Region;
  struct Cown;

  struct SuppressFinalizers
  {
    SuppressFinalizers();
    ~SuppressFinalizers();

    SuppressFinalizers(const SuppressFinalizers&) = delete;
    SuppressFinalizers& operator=(const SuppressFinalizers&) = delete;

    static bool active();
  };

  void collect(Header* h);
  void collect(Region* region);
  void collect(Cown* cown);

  // Collect a frozen SCC: find all members and push them onto the
  // collector worklist so finalizers run before memory is freed.
  void collect_scc(Header* root);
} // namespace vbci
