#pragma once

#include "ident.h"

#include <optional>

namespace vbci
{
  struct Header;
  struct Region;

  std::optional<size_t> add_mutable_cycle_candidate(Header* header, RC rc);
  RC mutable_cycle_candidate_count(size_t index);
  RC remove_mutable_cycle_candidate(size_t index);
  RC decrement_mutable_cycle_candidate(size_t index);
  void restore_mutable_cycle_candidates(Region& region);
}
