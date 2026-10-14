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

`current_region()` is a thread-local, non-owning pointer selecting the group
used by new allocations and mutable reference counting.
`ScopeAllocationGroup` temporarily replaces that context.

Values do not keep allocation groups alive. Any value that can outlive the
current owner must be stored together with an explicit owner:

- cowns own their dormant state group;
- queued behavior bodies own their copied closure group;
- executing behaviors own one combined execution group;
- queued teardown callbacks own their copied callback group;
- standalone synchronous results use `Thread::OwnedRegister`.

Owner/value pairs must clear the value with their owned group active before
releasing the group. `Thread::OwnedRegister` implements this protocol.

## Object reference counting

`Header::rc` counts mutable references to an individual object or array.
Register and field references use the same `inc()` / `dec()` operations.
Reaching zero collects the individual object unless its group is already
finalizing. The write barrier separately rejects storing readonly borrowed
values. Mutable headers carry no group pointer. Incrementing RC is
group-independent; decrementing requires the owning group as
`current_region()` because reaching zero may collect the object.

Object RC can collect acyclic garbage early. Allocation-group destruction also
collects remaining cyclic or otherwise live objects as a group.

Readonly behavior arguments do not adjust object RC because their cown-owned
allocation group remains alive for the behavior. Cown handles themselves
remain independently reference counted.

## Stores

`writebarrier.h` now implements ordinary managed exchange semantics:

- reject writes to immutable storage;
- reject readonly values and register references;
- retain or transfer the incoming value;
- install it;
- release the outgoing value.

Initialization may write immutable storage only with immutable or immortal
values. Mutable cross-group references are prevented by copying at ownership
boundaries rather than repaired by stores.

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

Each outer collection creates a stack-local collector. Reentrant collection
reaches it through one thread-local pointer; the collector's queues and active
mutable batch are ordinary fields, not independent thread-local state. The
batch finalizes all queued headers, then frees those headers and optionally the
whole group. Work for another group is deferred as a header/group root until
that group becomes active. Mutable roots and frozen SCC roots share a tagged
work queue; cowns remain intrusive.

Frozen SCC discovery adds every member to the common header-finalization
batch. Field dropping performs the normal ARC decrements for immutable
references outside the SCC, so another SCC is queued only when its external
count reaches zero.

The SCC root is marked `Pending` before discovery. Membership must test whether
`Header::find(member)` reaches a `Pending` header, not whether it still returns
the original root pointer: marking an intermediate member `Pending` terminates
the representative chain at that member.

Collection within the active mutable batch is two phase:

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

When a cown's reference count reaches zero, the collector queues it
intrusively. Processing the cown activates its owned group, drops its content,
drains the resulting mutable batch, and releases the native cown storage. The
lifetime union reuses the reference count field as the collector link once the
count reaches zero.

## Foreign lifetimes

Generic `pin` and `unpin` operations have been removed. `ffi::external` only
keeps the scheduler alive; it does not own managed values.

Future asynchronous FFI support must use explicit mechanisms for each foreign
lifetime, such as an exported byte-buffer token or an identity-bearing callback
component. It must not reintroduce an allocation-group reference count.

## Key files

| File | Purpose |
|------|---------|
| `frame_id.h` | Frame ordering for register-reference lifetime checks |
| `location.h` | Mutable, frozen-SCC, immortal, and pending header state |
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
