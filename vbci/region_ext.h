#pragma once

#include "array.h"
#include "object.h"
#include "program.h"
#include "region.h"
#include "region_rc.h"

namespace vbci
{
  void Region::trace_fn(auto&& fn) const
  {
    static_cast<const RegionRC*>(this)->trace_fn(fn);
  }

  void Region::for_each_header(auto&& fn) const
  {
    static_cast<const RegionRC*>(this)->for_each_header(fn);
  }

  void RegionRC::trace_fn(auto&& fn) const
  {
    auto& program = Program::get();

    for (auto h : headers)
    {
      if (program.is_array(h->get_type_id()))
        static_cast<Array*>(h)->trace_fn(fn);
      else
        static_cast<Object*>(h)->trace_fn(fn);
    }
  }

  void RegionRC::for_each_header(auto&& fn) const
  {
    for (auto h : headers)
      fn(h);
  }
}
