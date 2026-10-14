#pragma once

#include "classes.h"
#include "collect.h"
#include "ident.h"
#include "location.h"
#include "logging.h"

#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

namespace vbci
{
  struct Header;
  class AllocationGroup;
  struct Region;

  Region* current_region();
  Region* exchange_current_region(Region* region);

  class ScopeRegion
  {
    Region* previous;

  public:
    explicit ScopeRegion(Region& region)
    : previous(exchange_current_region(&region))
    {}

    ~ScopeRegion()
    {
      exchange_current_region(previous);
    }

    ScopeRegion(const ScopeRegion&) = delete;
    ScopeRegion& operator=(const ScopeRegion&) = delete;
  };

  struct Region
  {
  private:
    friend class AllocationGroup;

    static Region* create();

  protected:
    Region() = default;

  public:
    virtual ~Region() = default;

    virtual Object* object(Class& cls) = 0;
    virtual Array* array(uint32_t type_id, size_t size) = 0;
    virtual void rfree(Header* h) = 0;
    virtual void insert(Header* h) = 0;
    virtual bool remove(Header* h) = 0;
    virtual bool is_finalizing() = 0;
    virtual bool begin_finalizing() = 0;
    virtual void finalize_contents() = 0;
    virtual void release_dead_objects() = 0;
    virtual void absorb(Region* source) = 0;
    void for_each_header(auto&& fn) const;

    void trace_fn(auto&& fn) const;
  };

  class AllocationGroup
  {
    Region* region = nullptr;

    explicit AllocationGroup(Region* region) : region(region)
    {
      assert(region);
    }

    Region* release()
    {
      return std::exchange(region, nullptr);
    }

  public:
    AllocationGroup() = default;

    static AllocationGroup create();

    void clear()
    {
      if (!region)
        return;

      auto* current = release();
      collect(current);
    }

    ~AllocationGroup()
    {
      clear();
    }

    AllocationGroup(AllocationGroup&& other) noexcept
    : region(other.release())
    {}

    AllocationGroup& operator=(AllocationGroup&& other) noexcept
    {
      if (this == &other)
        return *this;

      clear();

      region = other.release();
      return *this;
    }

    AllocationGroup(const AllocationGroup&) = delete;
    AllocationGroup& operator=(const AllocationGroup&) = delete;

    explicit operator bool() const
    {
      return region != nullptr;
    }

    Region& get() const
    {
      assert(region);
      return *region;
    }

    void absorb(AllocationGroup source)
    {
      assert(region);
      assert(source.region);
      region->absorb(source.release());
    }
  };
}
