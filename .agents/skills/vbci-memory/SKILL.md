# vbci Memory Management

## Overview

The interpreter uses explicit allocation-group ownership. Mutable objects and
arrays are allocated in a `RegionRC`, and exactly one move-only
`AllocationGroup` owns that region. The group is released as a unit when its
owner is destroyed.

There is no region topology, dragging, sendability check, write barrier for
group ownership, or allocation-group reference count.

## Allocation-group ownership

`AllocationGroup` is the only API that creates and owns a `Region`:

- `AllocationGroup::create()` creates a fresh group.
- Moving an `AllocationGroup` transfers exclusive ownership.
- `absorb()` consumes another owner and moves all of its objects into the
  destination group.
- Destroying the owner finalizes and releases all objects still in the group.

`Thread::allocation_group` is a non-owning pointer selecting the group used by
new allocations. `ScopeAllocationGroup` temporarily replaces that context.

Values do not keep allocation groups alive. Any value that can outlive the
current owner must be stored together with an explicit owner:

- cowns own their dormant state group;
- queued behavior bodies own their copied closure group;
- executing behaviors own one combined execution group;
- queued teardown callbacks own their copied callback group;
- standalone synchronous results use `Thread::OwnedRegister`.

The owner must be declared before the managed value so C++ destroys the value
before releasing its group.

## Object reference counting

`Header::rc` counts mutable references to an individual object or array.
Register and field references use the same `inc()` / `dec()` operations.
Reaching zero collects the individual object unless its group is already
finalizing. The write barrier separately rejects storing readonly borrowed
values.

Object RC can collect acyclic garbage early. Allocation-group destruction also
collects remaining cyclic or otherwise live objects as a group.

Readonly behavior arguments do not adjust object RC because their cown-owned
allocation group remains alive for the behavior. Cown handles themselves
remain independently reference counted.

## Stores

`writebarrier.h` now implements ordinary managed exchange semantics:

- reject writes to immutable storage;
- reject references to invalid stack lifetimes;
- retain or transfer the incoming value;
- install it;
- release the outgoing value.

A mutable field may only refer to a value in the same allocation group. This
is a debug invariant rather than a runtime ownership-repair mechanism.

## Copy-on-split boundaries

Ownership boundaries deep-copy reachable mutable graphs:

- `freeze` copies into a fresh group, freezes the copy, and consumes the
  source reference;
- scheduling copies a closure into a queued owner;
- behavior completion copies each writable cown, including the result cown,
  into a fresh owned group;
- teardown registration copies its callback into queued owned work.

Post-copy hooks execute with the destination group as the active allocation
context.

## Freezing

`freeze()` converts the reachable copied graph to immutable SCCs:

- Tarjan-style DFS discovers SCCs;
- union-by-address and path compression identify representatives;
- `scc_union(child, rep)` transfers `child.rc - 1` to the representative;
- back edges decrement the representative before union;
- SCC completion sets `arc` from the representative's resulting RC.

The pending-stack loop must compare `pending.back() != rep` directly. Calling
`Header::find(pending.back())` there is incorrect because union can change the
representative during the loop.

Frozen objects are removed from their mutable allocation group. When the
temporary copy group is released after freezing, only non-frozen leftovers
remain to be collected.

## Finalization

Group collection is two phase:

1. run finalizers and drop managed fields;
2. release object storage.

`SuppressFinalizers` is used when consuming a source graph whose semantic
lifetime continues in a copy. Copy finalization must distinguish the live
source image from abandoned garbage so finalizers run exactly once.

Finalizers execute with readonly access. Any allocations made by standalone
cleanup execution use an explicitly owned temporary group.

## Cowns

A dormant cown owns both its content and its `AllocationGroup`. Writable cown
acquisition transfers that owner into the behavior execution group. Completion
copies the resulting state into a fresh group and returns that owner to the
cown.

Readonly acquisition leaves the cown's group in place and exposes only a
readonly borrowed value for the behavior lifetime.

## Foreign lifetimes

Generic `pin` and `unpin` operations have been removed. `ffi::external` only
keeps the scheduler alive; it does not own managed values.

Future asynchronous FFI support must use explicit mechanisms for each foreign
lifetime, such as an exported byte-buffer token or an identity-bearing callback
component. It must not reintroduce an allocation-group reference count.

## Key files

| File | Purpose |
|------|---------|
| `region.h` | `Region` and move-only `AllocationGroup` |
| `region_rc.h/cc` | Mutable allocation storage and group collection |
| `header.h` | Object RC and immutable ARC operations |
| `register.h` | Register ownership and transfer semantics |
| `writebarrier.h` | Managed field/array exchange |
| `copy_graph.cc` | Graph copying and destination hooks |
| `freeze.cc` | SCC construction and immutable ARC conversion |
| `cown.h` | Cown lifetime and allocation-group transfer |
| `thread.cc` | Behavior, synchronous-call, teardown, and callback ownership |
| `collect.cc` | Deferred object and group finalization |
