#pragma once

// Delay deallocation to handle re-entrancy.

namespace vbci
{
  struct Header;
  struct HeaderQueue;

  struct SuppressFinalizers
  {
    SuppressFinalizers();
    ~SuppressFinalizers();

    SuppressFinalizers(const SuppressFinalizers&) = delete;
    SuppressFinalizers& operator=(const SuppressFinalizers&) = delete;

    static bool active();
  };

  void collect(Header* h);
  void collect_finalizing(HeaderQueue headers);
  void collect_mutable_cycles();
} // namespace vbci
