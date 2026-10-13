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

  struct Region
  {
  private:
    friend class AllocationGroup;

    RC stack_rc;
    bool owned;

    static Region* create();

    void acquire_owner()
    {
      assert(!owned);
      owned = true;
    }

    void release_owner()
    {
      assert(owned);
      owned = false;

      if (stack_rc == 0)
        free_region();
    }

    void free_region()
    {
      assert(!owned);
      assert(stack_rc == 0);
      collect(this);
    }

  protected:
    Region() : stack_rc(0), owned(false) {}

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

    void stack_inc(RC inc = 1)
    {
      LOG(Trace) << "Region @" << this << " stack_rc incremented from "
                 << stack_rc << " to " << (stack_rc + inc);

      stack_rc += inc;
    }

    bool stack_dec(RC dec = 1)
    {
      assert(stack_rc >= dec);
      LOG(Trace) << "Region @" << this << " stack_rc decremented from "
                 << stack_rc << " to " << (stack_rc - dec);

      stack_rc -= dec;

      if (stack_rc == 0)
      {
        if (!owned)
        {
          // Returns false if the region has been freed.
          free_region();
          return false;
        }
      }

      return true;
    }

    RC get_stack_rc() const
    {
      return stack_rc;
    }

    void transfer_stack_rc_to(Region* target)
    {
      assert(target != this);

      if (stack_rc > 0)
        target->stack_inc(stack_rc);

      stack_rc = 0;
    }
  };

  class AllocationGroup
  {
    Region* region = nullptr;

    explicit AllocationGroup(Region* region) : region(region)
    {
      assert(region);
      region->acquire_owner();
    }

    Region* release()
    {
      return std::exchange(region, nullptr);
    }

  public:
    AllocationGroup() = default;

    static AllocationGroup create();

    ~AllocationGroup()
    {
      if (region)
        region->release_owner();
    }

    AllocationGroup(AllocationGroup&& other) noexcept
    : region(other.release())
    {}

    AllocationGroup& operator=(AllocationGroup&& other) noexcept
    {
      if (this == &other)
        return *this;

      if (region)
        region->release_owner();

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
