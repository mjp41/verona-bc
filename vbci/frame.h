#pragma once

#include "frame_id.h"
#include "register.h"
#include "value.h"

#include <span>
#include <vector>

namespace vbci
{
  struct Frame
  {
    Function* func;
    FrameId frame_id;
    std::vector<Register>& locals;
    size_t base;
    std::vector<Object*>& finalize;
    size_t finalize_base;
    size_t finalize_top;
    PC pc;
    size_t dst;
    FrameId raise_target;

    Frame(
      Function* func,
      FrameId frame_id,
      std::vector<Register>& locals,
      size_t base,
      std::vector<Object*>& finalize,
      size_t finalize_base,
      PC pc,
      size_t dst);

    Register& local(size_t idx);
    Register& arg(size_t idx);
    std::span<Register> args(size_t args);
    void push_finalizer(Object* obj);
    void drop();
    void drop_args(size_t& args);
    void check_var_type(size_t reg_idx, Program& prog) const;
  };
}
