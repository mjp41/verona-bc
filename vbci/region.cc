#include "region.h"

#include "region_rc.h"

namespace vbci
{
  Region* Region::create()
  {
    return new RegionRC;
  }

  AllocationGroup AllocationGroup::create()
  {
    return AllocationGroup(Region::create());
  }
}
