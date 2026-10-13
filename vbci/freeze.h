#pragma once

namespace vbci
{
  struct Header;

  // Freeze the reachable subgraph from root: calculate SCCs, set up
  // union-find for ARC tracking, and make reachable objects immutable.
  // The allocation group survives with any unfrozen objects.
  // Objects reached across allocation groups are frozen in the same way.
  // No-op if root is already immutable or immortal.
  void freeze(Header* root);
}
