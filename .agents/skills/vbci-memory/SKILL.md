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
Reaching zero collects the individual object. Headers already claimed by the
collector use the `Finalizing` header state, whose increments and decrements
are no-ops. Newly allocated objects remain `Mutable` even when their region is
being finalized, so their ordinary decrements still collect acyclic garbage.
The write barrier separately rejects storing readonly borrowed values. Mutable
headers carry no group pointer; decrementing uses `current_region()` when zero
removes the object from its group.

Object RC can collect acyclic garbage early. Allocation-group destruction also
collects remaining cyclic or otherwise live objects as a group.
Mutable counts are thread-confined and use relaxed atomic loads and stores;
only shared immutable and cown counts use atomic read-modify-write operations.

Each runtime type has a `can_cycle` capability computed once after loading all
type metadata. The runtime builds the possible managed-reference graph and
marks only types in a cyclic SCC, defined as an SCC containing at least one
internal edge. User objects, arrays, and tuples are cycle candidates. Union,
`ref`, and `dyn` type IDs are auxiliary traversal vertices; cowns are ownership
boundaries. Erased `dyn` values conservatively target every valid type ID. A
`ref[T]` targets cycle candidates with a direct field or element type
equivalent to `T`, matching the possible base headers of interior references.
The cycle detector can skip headers whose type is not marked.
Each trace step expands one type ID by one level using the shared iterative
`traverse_graph` primitive. SCC discovery mirrors freezing: an edge to a
pending type unions the pending suffix into that representative. No reverse
graph is constructed.

Mutable cycle candidates use the previously unused header tag. Its invariant
is that the authoritative real RC is in the thread-local candidate table.
Ordinary mutable objects temporarily use the same state while enrolled in
cycle detection. Each persistent slot stores only the header reverse pointer
and real RC. Classification allocates a parallel table containing one tagged
trial word per candidate index. Zero means unvisited. The low bit distinguishes
a direct SCC representative count from an indirect representative `Header*`; a
second bit distinguishes pending and completed direct representatives or tags
an intrusive representative-stack link. Direct counts are offset by one so an
SCC RC of zero remains distinct from an unvisited trial entry. The header
state's upper bits identify the candidate slot directly. Free candidate records
have real RC zero and reuse the header field as a free-list index. A nonzero
decrement of a cycle-capable mutable type creates a persistent candidate
record. Candidate increments restore the header to `Mutable`, and candidate
decrements update the record or collect at zero.

`classify_mutable_cycle_candidates()` runs synchronous trial deletion over the
complete thread-local candidate table and has no region dependency.
Classification traces the cycle-capable mutable closure, temporarily assigning
slots to ordinary mutable objects as they are discovered. Header slot indices
provide traversal identity and deduplication; no separate header map is built.
Classification uses the same path-based SCC construction as freezing. Each
new representative starts with its member's real RC. Union transfers the
child SCC's count minus the tree edge, and an edge to a pending representative
subtracts the back edge before unioning the pending suffix. A completed
representative therefore holds the RC of its SCC in the condensation DAG:
external references and references from other SCCs remain counted, while
intra-SCC references do not. The trial word supplies union-find forwarding and
path compression; a temporary pending vector supplies DFS ordering.

Zero-count SCC representatives are processed as garbage roots. Tracing their
members decrements the direct counts of target SCC representatives, cascading
through the acyclic condensation graph. Strong connectivity means traversal
from the representative, restricted to children with the same representative,
reconstructs the complete SCC without a member table. The tagged trial word
links a representative into the intrusive zero-SCC work stack immediately
when SCC completion or cascading deletion makes its direct count zero. That
stack is drained immediately: representative-restricted traversal processes
outgoing SCC edges, changes every discovered member to `Finalizing`, releases
its candidate slot, and appends it directly to the intrusive `HeaderQueue`.
Because union-find links never cross SCCs, reaching any `Finalizing`
representative while following an indirect member proves that member belongs
to the SCC currently being claimed; the traversal follows and claims it
without needing the original root pointer. Representatives left with nonzero
counts are live; their members return to ordinary `Mutable` state by slot
iteration without retraversing the live graph. No separate SCC worklist,
member table, garbage-slot list, or garbage pointer vector is allocated.

While allocation groups remain, reclamation is a separate adapter: the caller
removes the already-finalizing intrusive garbage list from its region and
transfers the same links to the collector before dropping fields. Copying
drains the candidate table before tracing the source, then drains it again
after post-copy hooks. The first pass leaves no old candidates, so the second
pass contains only hook-created candidates. Each pass repeats if finalization
creates further candidates.

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
whole group. Work for another group is deferred until that group becomes
active. Cross-region trial deletion is the exception: it uses a nested
collector so the candidate records are resolved before that region can cross
an ownership boundary. A mutable root starts or joins the active batch
directly. Frozen SCC roots use a tagged work queue, while cowns remain
intrusive.

`HeaderState` encapsulates one atomic tagged word. The bottom three bits
identify the state; the upper bits hold mutable RC, shared ARC, an SCC
representative, or an intrusive collector link. Finalization uses an intrinsic
FIFO, then moves each header onto an intrinsic deletion list.

`Immortal` and `Finalizing` currently have separate tags even though both
ignore reference-count operations. The finalizer receives readonly `self`, so
it cannot store that reference outside the stack. If every copy boundary is
confirmed unable to receive such a readonly reference, the states may be
collapsed to recover one tag value.

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

Finalizers execute with readonly access and allocate in the current group,
including when that group is finalizing. The collector removes every current
header from `RegionRC` into its intrinsic finalization queue before invoking
user code, marks those headers `Finalizing`, and leaves the region set empty
for finalizer allocations. Acyclic finalizer allocations can still reach zero
normally; only remaining cycles are transferred in the next batch. Finalized
headers move to the collector's deletion queue. The collector repeats until
the region set remains empty, then deletes all headers and the region.

## Cowns

A dormant cown owns both its content and its `AllocationGroup`. Writable cown
acquisition transfers that owner into the behavior execution group. Completion
copies the resulting state into a fresh group and returns that owner to the
cown.

Readonly acquisition leaves the cown's group in place and exposes only a
readonly borrowed value for the behavior lifetime.

`Cown` derives from `Header` and uses the `Cown` header state. Cowns and
immutable SCC representatives use the same atomic header count with relaxed
increments and release/acquire zero decrements. The header state determines
which collection path zero selects. After zero, the upper bits of the tagged
state become the intrusive collector link.

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
| `header_state.h` | Tagged header state, counts, SCC links, and collector links |
| `mutable_cycle_candidates.h/cc` | Thread-local mutable candidate RC records |
| `region.h` | `Region` and move-only `AllocationGroup` |
| `region_rc.h/cc` | Mutable allocation storage and group collection |
| `header.h` | Mutable RC, shared atomic RC, and collector links |
| `register.h` | Register ownership and transfer semantics |
| `writebarrier.h` | Managed field/array exchange |
| `copy_graph.cc` | Graph copying and destination hooks |
| `freeze.cc` | SCC construction and immutable ARC conversion |
| `cown.h` | Cown lifetime and allocation-group transfer |
| `thread.cc` | Behavior, synchronous-call, teardown, and callback ownership |
| `collect.cc` | Deferred object and group finalization |
