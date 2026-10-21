#include "callback.h"

#include "program.h"
#include "thread.h"
#include "value.h"

#include <memory>

namespace vbci
{
  // Universal libffi trampoline. Called by C code through the closure's
  // code_ptr. Marshals C arguments back to Verona Values and calls the
  // lambda's apply method via Thread::handle_callback.
  static void
  callback_handler(ffi_cif* /*cif*/, void* ret, void* args[], void* user_data)
  {
    auto* cc = static_cast<CallbackClosure*>(user_data);
    Thread::handle_callback(cc, ret, args);
  }

  CallbackClosure* make_callback(ValueTransfer lambda, Function* func)
  {
    // Build the ffi_cif from the apply function's signature.
    // func->param_types[0] is self (the lambda), skip it.
    // func->param_types[1:] are the actual callback parameters.
    auto& program = Program::get();
    std::vector<ffi_type*> arg_ffi_types;
    std::vector<ValueType> arg_value_types;

    for (size_t i = 1; i < func->param_types.size(); i++)
    {
      auto rep = program.layout_type_id(func->param_types[i]);
      arg_value_types.push_back(rep.first);

      // `Value` crosses the FFI boundary as a pointer (thread_handle_callback
      // interprets args[i] as `Value*` for `Dyn`), so the CIF must declare
      // pointer-sized params, not a 16-byte by-value struct. This covers
      // `any`, `ref`, and heterogeneous unions.
      if (rep.first == ValueType::Dyn)
        arg_ffi_types.push_back(&ffi_type_pointer);
      else
        arg_ffi_types.push_back(rep.second);
    }

    // Return type.
    auto ret_rep = program.layout_type_id(func->return_type);
    auto return_value_type = ret_rep.first;
    auto* return_ffi_type = ret_rep.second;

    if (return_value_type == ValueType::Dyn)
      return_ffi_type = program.value_type();

    auto cc = std::make_unique<CallbackClosure>();
    cc->arg_ffi_types = std::move(arg_ffi_types);
    cc->arg_value_types = std::move(arg_value_types);
    cc->return_value_type = return_value_type;
    cc->return_ffi_type = return_ffi_type;

    // Prepare the cif.
    if (
      ffi_prep_cif(
        &cc->cif,
        FFI_DEFAULT_ABI,
        static_cast<unsigned>(cc->arg_ffi_types.size()),
        cc->return_ffi_type,
        cc->arg_ffi_types.data()) != FFI_OK)
      Value::error(Error::BadArgs);

    // Allocate the ffi_closure only after its complete signature is valid.
    cc->closure = static_cast<ffi_closure*>(
      ffi_closure_alloc(sizeof(ffi_closure), &cc->code_ptr));

    if (!cc->closure)
      Value::error(Error::BadArgs);

    // Prepare the closure.
    if (
      ffi_prep_closure_loc(
        cc->closure, &cc->cif, callback_handler, cc.get(), cc->code_ptr) !=
      FFI_OK)
      Value::error(Error::BadArgs);

    cc->func = func;
    cc->lambda = lambda;
    return cc.release();
  }

  void* retain_callback(CallbackClosure* callback)
  {
    callback->inc();
    return callback;
  }

  void release_callback(void* handle)
  {
    if (!handle)
      Value::error(Error::BadOperand);

    static_cast<CallbackClosure*>(handle)->dec();
  }
}
