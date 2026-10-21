#pragma once

#include "collect.h"
#include "header.h"
#include "logging.h"
#include "program.h"
#include "register.h"
#include "value.h"

#include <ds/heap.h>
#include <format>
#include <new>

namespace vbci
{
  struct alignas(8) NativeState : public Header
  {
  private:
    Value content;

    explicit NativeState(uint32_t type_id) : Header(type_id) {}

  public:
    static NativeState* create(uint32_t type_id, ValueTransfer content)
    {
      if (!Program::get().is_native(type_id))
        Value::error(Error::BadType);

      auto* memory = verona::rt::heap::alloc<sizeof(NativeState)>();
      auto* state = new (memory) NativeState(type_id);
      state->content = content;
      LOG(Trace) << "Created native state @" << state;
      return state;
    }

    ~NativeState()
    {
      LOG(Trace) << "Destroying native state @" << this;
      assert(content.is_invalid());
      LOG(Trace) << "Destroyed native state @" << this;
    }

    void finalize()
    {
      auto previous_content = content;
      content = Value();
      previous_content.dec();
      collect_mutable_cycles();
    }

    void destroy()
    {
      this->~NativeState();
      verona::rt::heap::dealloc<sizeof(NativeState)>(this);
    }

    ValueBorrow load() const
    {
      return content;
    }

    std::string to_string() const
    {
      return std::format("native: {}", static_cast<const void*>(this));
    }
  };
}
