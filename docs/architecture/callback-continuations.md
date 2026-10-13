# `when` Arguments and FFI Callback Identity

**Status: Planned.**

This note records a proposed formulation of scheduling under copy-on-split.
The `when` operation copies the arguments supplied to the new behavior. It does
not copy the continuation of the behavior that schedules the `when`, including
the continuation's live variables.

This distinction matters for foreign-function interface (FFI) callbacks.
Native code may retain a callback code pointer and later invoke it. Scheduling
a behavior must not duplicate or replace a callback that remains live in the
scheduling continuation.

The current interpreter does not represent this distinction directly. It
packages the new behavior's body and captured values in a managed closure, then
copies that closure when scheduling. This note describes the intended semantic
boundary rather than the current representation.

## Terminology

A **scheduling behavior** is the behavior that executes a `when`.

A **scheduled behavior** is the new behavior created by that `when`. It runs
after the scheduler has acquired its required cowns.

The **scheduling continuation** is the computation performed by the scheduling
behavior after the `when` has been scheduled. It includes the code following
the `when` and the live variables used by that code.

A **behavior argument** is a value supplied to the scheduled behavior. A
source language may express an argument explicitly or as a free variable used
by the behavior body.

An **FFI callback** is a Verona callable exposed to native code through a
stable native code pointer. Native code supplies **callback arguments** when it
invokes that callback, such as a completion status, byte count, request
pointer, or buffer description.

## Scheduling rule

Scheduling copies the behavior arguments and leaves the scheduling
continuation unchanged.

Abstractly:

```text
when(cowns, body, arguments); continuation
```

creates:

```text
scheduled behavior:       body(copy(arguments))
scheduling behavior:      continuation
```

The scheduling continuation retains its existing live variables. The runtime
does not traverse or copy that environment merely because the behavior
schedules a `when`.

If the scheduled body uses a free variable, that use supplies an implicit
behavior argument. When the scheduling continuation also retains the value,
the scheduled behavior receives a copy and the continuation retains the
original. If the continuation does not retain the value, an implementation may
transfer the original storage to the scheduled behavior. The semantic result
is still the value defined by copying.

This rule is the ordinary copy-on-send split stated in terms of the two
computations created by scheduling. It does not introduce a second
continuation object or require new source syntax.

## Example

Consider a callback and mutable data that are both used by a scheduled behavior
and by the scheduling continuation:

The following is pseudocode rather than proposed Verona syntax:

```text
let cb = callback(...);
let data = cell(0);

when () ->
{
  use(cb, data);
};

continue_with(cb, data);
```

Conceptually, scheduling produces:

```text
scheduled behavior:       use(cb, copy(data))
scheduling continuation:  continue_with(cb, data)
```

The scheduled behavior receives its own mutable image of `data`. The
scheduling continuation keeps the original `data`.

The callback is different from ordinary mutable data. The native code pointer
must continue to identify the same callback. Copy traversal therefore preserves
the callback as a shared identity-bearing component:

```text
scheduled behavior:       cb ──► callback identity
                                  ▲
scheduling continuation:  cb ────┘
```

Scheduling neither clones the native trampoline nor redirects it to a copied
callable.

## Scheduling from an FFI callback

An FFI callback may schedule a `when` to update cown-protected Verona state.
The arguments supplied by native code can become arguments to the scheduled
behavior:

```text
read_callback(buffer, byte_count, status):
  when target_cown:
    process(cown_state, buffer, byte_count, status)
  continue callback execution
```

The new behavior receives the behavior-argument representations of `buffer`,
`byte_count`, and `status`. Ordinary mutable managed values may be copied or
uniquely transferred according to ownership; whether an operation-owned buffer
is copied or transferred remains open. The callback invocation and its
existing live variables remain unchanged.

Copying an argument does not imply duplicating a native resource. Cown handles,
immutable values, and other shared components follow the surrounding
copy-on-split design. This note requires only that an FFI callback and its
native trampoline preserve one shared identity. Other native handles,
resources, and operation-owned buffers require their own ownership policies.

## Stable callback identity

An FFI callback must remain valid until its owning callback component is
released. If a callback is live in the scheduling continuation, scheduling a
`when` cannot replace it with a copy. If the callback is also passed to the
scheduled behavior, both computations retain the same callback identity.

The current callback representation is unsafe under ordinary graph copying. A
managed `ffi::callback` wrapper contains its callable and a raw pointer to a
separately allocated native closure. The native closure borrows the original
callable. Copying the wrapper duplicates its fields, but the copied raw pointer
still denotes a closure that invokes the original callable. Both wrappers may
also attempt to free the same native closure.

The target representation must therefore use an identity-bearing shared
component that keeps the Verona callable and native trampoline alive together
and releases the trampoline exactly once, or otherwise provide the same
guarantees. This requirement is independent of whether an ordinary mutable
behavior argument is copied or transferred.

## Relationship to the existing copy-on-split account

The copy-on-split pitch already describes scheduling as an ownership split:
the scheduled behavior receives captured state, while the scheduling
continuation retains its original state. This note sharpens that account by
calling the captured state the behavior arguments.

That formulation prevents an implementation detail from broadening the
semantic copy boundary. A runtime may represent the scheduled body and its
arguments as a closure, but copying that closure is only a way to copy the
behavior arguments. It does not mean that the scheduling continuation or all
of its live variables are copied.

The formulation also makes callback treatment explicit. An FFI callback is a
shared component reached while copying an argument, not an ordinary mutable
object that acquires a second native identity.

## Representation obligation

The compiler and runtime must distinguish:

- the body of the scheduled behavior;
- the arguments delivered to that body; and
- the scheduling continuation and its live variables.

Current `when` lowering packages the first two items in a managed closure. That
representation remains usable only if graph copying treats callback components
according to their stable shared identity and if the closure's fields
correspond exactly to the behavior arguments.

No new surface syntax is selected here. Existing free-variable analysis may be
enough to identify the implicit arguments captured by the scheduled body. A
later implementation design must establish whether the current intermediate
representation preserves the distinction precisely enough.

## Concurrency and lifetime obligations

Preserving callback identity allows native code and multiple scheduled
behaviors to retain the same callback. The callback implementation must
therefore specify:

- how the callable and native trampoline remain alive together;
- how the final reference releases the native closure exactly once;
- whether native code may invoke one callback concurrently;
- what state, if any, a callback may mutate synchronously; and
- how callback destruction synchronizes with an invocation already in
  progress.

These obligations belong to the callback shared component. They do not require
copying the scheduling continuation.

## Open questions

The design still needs to decide:

1. Which callback state may be mutable outside a cown?
2. Are invocations of one callback serialized?
3. Can existing free-variable analysis define the complete behavior-argument
   set, or must the intermediate representation make that set explicit?
4. How does a callback shared component own its Verona callable and native
   closure?
5. How are operation-owned buffers transferred or copied when a callback
   schedules a behavior?

These questions must be resolved before replacing the current callback and
`when` representations.
