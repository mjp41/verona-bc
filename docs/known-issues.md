# Known Issues

This page records current implementation defects with precise trigger
conditions. These are not intended language or VIR semantics.

## VC loses generic narrowing before callback reification

**Current:** VC's `infer` pass tracks type-test narrowing per control-flow
edge, but `reify` reconstructs local types with one function-wide environment.
A value extracted from `T | none` by a match arm can therefore be seen as
`dyn` by operations that require its concrete reified type.

For example, this callback reconstruction pattern fails:

```verona
(match self._callable
{
  (callable: T) -> self._cb = :::make_callback(callable);
}) else (none);
```

VC reports:

```text
make_callback: cannot determine lambda type
```

Introducing an explicitly typed local inside the match arm does not help: ANF
lowers the declaration to a `Var` and a `Copy`, and reification propagates the
original `dyn` type through that copy.

The underlying issue is that branch-specific narrowing recorded by `infer` is
not available when `reify` resolves `make_callback`. Until that is fixed,
runtime copy-on-split experiments use concrete textual VIR fixtures rather
than relying on this VC path.

## VC callback reification can recurse indefinitely

**Current:** the same direct callback reconstruction pattern can take a second
failure path for programs such as `testsuite/v/callback_basic`. Instead of
reporting the missing reified type, VC repeatedly reduces subtype sequents
through `shape_functions_conflict` until it exhausts the native stack and
terminates with a segmentation fault.

The compiler must diagnose or reject unsupported callback types without
crashing. This failure was reproduced while testing a `post_copy` method in
`_builtin/ffi/callback.v`; that experimental VC hook is not part of the
VIR-only copy-on-split implementation.
