# VIR Format

Verona Intermediate Representation (VIR) is the typed, reified contract between
VC and VIRC. It is represented as a Trieste AST whose token definitions and
well-formedness schema are in `include/vir.h` under namespace `vir`.

## Producers and Consumers

VC produces VIR directly after reification. The standalone VIRC reader also
constructs the same tree from textual `.vir` fixtures. Shared VIRC passes
validate, analyze, optimize, and index the tree before an output backend runs.

The VBC emitter consumes the validated form. VBCI consumes serialized VBC, not
VIR directly. Future emitters can consume the same validated form without
changing the VIR contract.

## Root Shape

`vir::wfIR` defines the authoritative tree shape. A top-level VIR tree contains
definitions such as:

- primitive, class, type-alias, function, symbol, and library definitions;
- optional memo initialization metadata;
- function parameters, variables, labels, statement bodies, and terminators;
- explicit type, ownership, region, call, FFI, and source-location nodes.

Identifiers are symbolic tokens before VIRC's ID-assignment pass. The indexed
compilation model is derived state and is not part of VIR.

## Textual Form

Textual `.vir` is a test and tool input syntax parsed by the standalone reader.
It is not an additional semantic IR. The parser must produce a tree accepted by
`vir::wfIR`, and VC may bypass it by handing reified nodes directly to VIRC.

The textual reader accepts the callback statements defined by `vir::wfIR`:

- `makecallback` deep-copies an object whose class provides `@callback` and
  creates a reference-counted native callback handle owning that copy;
- `codeptrcallback` borrows a callback handle and extracts its callable native
  code pointer;
- `retaincallback` borrows a callback handle, retains its closure, and returns
  an opaque `ptr` representing that retention;
- `releasecallback` releases one retention represented by such an opaque
  pointer.

The code pointer remains valid only while a callback handle or explicit
retention keeps the closure alive. Each opaque retention must be released
exactly once.

It also accepts the native-state forms:

- `native T` is a reference-counted synchronous state handle containing `T`;
- `newnative value : T` consumes and deep-copies `value` into a new `native T`;
- `nativewith context handler method : R` borrows `context`, consumes and
  deep-copies `handler`, invokes `method(handler, state)` synchronously, and
  consumes and deep-copies the result as `R`.

Copying a native-state handle shares the cell identity. Graph copying stops at
native-state and callback handles and retains them rather than traversing their
owned state. `nativewith` exposes the contained `T`, not a replaceable
`ref T`; use an explicit mutable wrapper inside `T` when whole-root replacement
is required.

## Reserved Methods

VIRC assigns stable method IDs to `@final`, `@callback`, and `@post_copy`.
`@post_copy` must name a function with exactly one parameter. VBCI invokes it
on a copied destination object after that object's fields have been
initialized.

## Compatibility

VIR currently has no serialized version field or external stability guarantee.
Changes to `vir::wfIR` must update all producers, shared passes, emitters, the
textual reader, fixtures, and this document in one change.

`include/vbcc.h` is a migration header that imports `vir` names into namespace
`vbcc`. New code includes `vir.h` directly.