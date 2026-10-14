#pragma once

#include "program.h"
#include "region.h"

#include <cstdlib>
#include <iostream>
#include <unordered_set>

namespace vbci
{
  struct RegionRC : public Region
  {
    friend struct Region;

  private:
    std::unordered_set<Header*> headers;
    bool finalizing;

  public:
    RegionRC() : finalizing(false)
    {
      LOG(Trace) << "Created RegionRC @" << this;
    }

    Object* object(Class& cls) override;
    Array* array(uint32_t type_id, size_t size) override;

    void insert(Header* h) override;
    bool remove(Header* h) override;
    bool begin_finalizing() override;
    Header* take_header() override;
    void destroy() override;
    void absorb(Region* source) override;

    ~RegionRC()
    {
      LOG(Trace) << "Destroyed RegionRC @" << this;
    }

    void trace_fn(auto&& fn) const;
    void for_each_header(auto&& fn) const;
  };
}
