# 19. Memory Model

[← Table of Contents](README.md) | [Previous: Type Inference](18-type-inference.md) | [Next: Compiler Pipeline →](20-compiler-pipeline.md)

This chapter describes Verona's copy-based memory and ownership model — how objects are allocated, where they live, and what rules govern references between them.

---

## 19.1 Overview

Mutable objects and arrays are independently reference counted. The runtime
also detects unreachable mutable cycles, so cyclic object graphs do not require
a tracing garbage collector or explicit region ownership.

Ordinary assignment creates aliases. Isolation is introduced only at explicit
ownership boundaries, where the language semantics require a deep copy:

- `mem::freeze` returns an immutable deep copy;
- scheduling a `when` copies the non-cown captured graph;
- behavior completion copies each writable result graph into its cown;
- `native_state(value)` copies `value` behind a foreign-owned state handle;
- `ffi::callback(callable)` copies the callable graph into the callback.

The runtime may later reuse an exclusively owned graph in place, but programs
must behave as if the copy occurred.

---

## 19.2 Allocation and Lifetime

`new` and array construction allocate ordinary managed values. There are no
program-visible stack, frame-region, heap-region, dragging, or region-parent
distinctions.

A managed value remains alive while it is reachable from a runtime root such
as:

- a local register or object field;
- an array element;
- cown content;
- queued behavior state;
- a callback-owned callable;
- a `native_state[T]` cell.

Dropping the last acyclic reference reclaims the value. Decrements that may
leave a cycle are classified by the mutable-cycle collector. Finalization and
cycle collection are synchronous at runtime ownership boundaries.

---

## 19.3 Aliasing and Copy Boundaries

Local bindings, field stores, function arguments, and returns alias their
values. They do not implicitly deep-copy mutable graphs. Deep copying is
reserved for the ownership boundaries listed in §19.1.

Copying preserves sharing within the copied graph. If two source paths point to
the same object, the corresponding destination paths point to one copied
object. Cycles are likewise reproduced as cycles.

Some handles are ownership boundaries rather than traversable graph nodes.
Copying a cown, callback handle, or `native_state[T]` handle retains the same
identity instead of copying the state behind that handle.

---

## 19.4 Binding Semantics: `let` vs `var`

`let` and `var` control the **binding**, not the object:

| Binding | Reassignable? | Object mutable? |
|---------|---------------|-----------------|
| `let` | No — single assignment | Yes |
| `var` | Yes — can be reassigned | Yes |

Both `let` and `var` create **aliases** to objects — not copies or moves. The object itself is always mutable through any alias:

```verona
let a = point(1, 2);
let b = a;                            // b aliases the same object
b.x = 10;                            // a.x is now also 10
```

Assignment never copies, never moves, never invalidates the source binding. Two names for the same object see each other's mutations.

The `let`/`var` distinction is about the **name**, not the **value**:

```verona
var q = point(1, 2);
q = point(3, 4);                     // allowed — reassigns the binding

let r = point(1, 2);
// r = point(3, 4);                  // NOT allowed — let is single-assignment
```

> **Aliasing and concurrency:** Because assignment aliases, two aliases to the same object can both mutate it. Within a single thread, this is intentional — Verona is not a pure language. Across concurrent behaviors, shared mutable state must go through [cowns](15-concurrency.md). Native callbacks may instead use an explicitly unsafe, externally serialized FFI contract.

---

## 19.5 Assignment Returns the Previous Value

Assignment (`=`) is an expression that evaluates to the **previous value** of the left-hand side:

```verona
var x = 1;
let old = (x = 2);                   // old is 1, x is now 2
```

For field writes through `ref[T]`, the same applies — the exchange returns the old value at that location.

---

## 19.6 References and `ref[T]`

`ref[T]` is a mutable reference wrapper used for field and array element writes. It is not a general-purpose pointer — it exists specifically to enable assignment syntax.

### How Field Assignment Works

When you write `obj.field = val`, three things happen:

1. **The compiler calls the `ref` accessor.** Every class field automatically generates a `ref` method:
   ```verona
   // For a field `x: i32` in class `point`, the compiler generates:
   ref x(self: point): ref[i32] { :::fieldref(self, x) }
   ```
   This returns a `ref[i32]` pointing into the object.

2. **The compiler emits a Store instruction.** The store retains or transfers `val`, installs it through the reference, and releases the previous value.

3. **The old value is returned.** Assignment is an exchange — it returns the previous value (see §19.5).

When you read `obj.field`, the compiler either calls a non-ref accessor (if one exists) or auto-generates one that loads through the ref.

### `ref[T]` Is Not User-Constructible

You cannot create `ref[T]` values directly. They are produced only by `ref` methods (field accessors, `ref apply` on arrays).

### Register Reference Lifetimes

A `ref[T]` to a local register must not outlive the function frame containing
that register. Returning it, raising it past that frame, or carrying it through
a tail call produces `bad stack escape`. Register references also cannot be
stored into managed object or array fields.

The compiler normally generates code that stores through or loads from
references immediately. Prefer using the resulting value rather than retaining
a reference for later use:

```verona
let val = obj.field;                  // loads through ref, binds the value
obj.field = new_val;                  // stores through ref in one statement
```

---

## 19.7 Reference Counting and Cycles

Mutable objects use per-object reference counts. A decrement that leaves a
cycle-capable object alive records it as a cycle candidate. At collection
boundaries, the runtime performs synchronous trial deletion over the candidate
graph and reclaims unreachable strongly connected components.

Immutable graphs produced by `mem::freeze` use shared reference counting over
their immutable components. Native-state handles and callback closures have
separate atomic reference counts because native code may hold or invoke them
outside ordinary object traversal.

These mechanisms are runtime implementation details. Programs observe only
the value-level copying and lifetime rules.

---

## 19.8 Runtime Errors

The runtime reports errors for invalid value operations, not region topology:

| Error | Cause |
|-------|-------|
| `bad store` | Storing a read-only value or transient register reference, or writing mutable state into immutable storage. |
| `bad store target` | Writing through a read-only or otherwise invalid reference target. |
| `bad type` | A call, return, field store, array store, or native-state store fails its subtype check. |
| `bad array index` | Array index out of bounds. |
| `bad args` | Wrong number of arguments to a function or constructor. |
| `bad stack escape` | A transient register reference would outlive the frame that owns its register. |

There are no region-parent, region-merge, or dragging errors.

These errors are **fatal** — the current function (or behavior, in a `when` block) terminates immediately. They indicate logic bugs, not expected failure modes.

---

## 19.9 Freezing and Other Ownership Splits

`mem::freeze(value)` returns an immutable deep clone of `value`. The mutable
source and immutable destination are semantically independent. The runtime may
freeze in place only when it can prove that no observable external alias
remains.

Scheduling a `when` similarly copies its non-cown captures. Cown handles retain
their identities and provide the serialized access set for the behavior.

When a behavior completes, each writable cown receives a graph semantically
copied from the behavior's final state. Exclusive ownership may permit an
in-place implementation, but cannot change program behavior.

---

## 19.10 Cowns and Ownership Transfer

A cown is an asynchronous serialization boundary. Its contents are accessed
only inside a `when` that acquires that cown.

Scheduling copies ordinary captured state into the behavior. The cown itself
is not copied; its handle identifies the same serialized resource. On
completion, writable state is copied back into the cown before acquired cowns
are released.

See [Concurrency](15-concurrency.md) for the user-facing semantics of cowns.

---

## 19.11 Object Teardown

When a managed value or unreachable cycle is reclaimed, the runtime runs its
registered finalization logic and then releases its fields or elements.
Finalization may create more managed work, so collection continues until the
resulting candidate set is stable.

Callback closures release their copied callable graphs after the last callback
handle and explicit native retention are released. Native-state handles
release their contained graph on the last `native_state[T]` handle.

---

## 19.12 Native FFI State

`native_state[T]` is a synchronous, reference-counted state handle intended for
native access. Construction deep-copies its initial value. Copying the handle
shares the same state identity. Each `with[R]` copies its handler in and its
result out, preventing aliases from crossing the state boundary.

See [FFI §17.9](17-ffi.md) for serialization, retained-pointer, callback
ownership, deregistration, and scheduler-liveness requirements.
