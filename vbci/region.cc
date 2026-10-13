#include "region.h"

#include "region_rc.h"

namespace vbci
{
  Region* Region::create()
  {
    return new RegionRC;
  }
}
