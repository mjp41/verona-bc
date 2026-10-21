#pragma once

#include "collect.h"
#include "function.h"
#include "header.h"
#include "register.h"

#include <ffi.h>
#include <vector>

namespace vbci
{
  struct CallbackClosure : public Header
  {
  public:
    ffi_closure* closure = nullptr;
    void* code_ptr = nullptr;
    ffi_cif cif;
    std::vector<ffi_type*> arg_ffi_types;
    std::vector<ValueType> arg_value_types;
    ffi_type* return_ffi_type;
    ValueType return_value_type;
    Function* func;
    // Owned callable graph.
    Value lambda;

    CallbackClosure() : Header(+ValueType::Callback) {}

    ~CallbackClosure()
    {
      finalize();
    }

    void finalize()
    {
      auto previous_lambda = lambda;
      lambda = Value();
      previous_lambda.dec();
      collect_mutable_cycles();

      if (closure)
      {
        ffi_closure_free(closure);
        closure = nullptr;
        code_ptr = nullptr;
      }
    }

    void destroy()
    {
      delete this;
    }

    void immortalize() noexcept
    {
      if (state().is_immortal())
        return;

      mark_immortal();
    }
  };

  // Create a callback closure from a lambda and its apply function.
  CallbackClosure* make_callback(ValueTransfer lambda, Function* func);

  void* retain_callback(CallbackClosure* callback);
  void release_callback(void* handle);
}
