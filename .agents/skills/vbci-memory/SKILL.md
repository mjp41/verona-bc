# vbci Memory Management

## Overview

The interpreter uses per-object reference counting, thread-local mutable-cycle
classification, and atomic reference counting for immutable SCCs and cowns.
Mutable objects and arrays are allocated directly; there are no regions,
allocation groups, object-membership sets, or current-allocation context.

Values and fields are the only managed roots. Runtime boundaries that consume
roots synchronously classify mutable-cycle candidates after releasing those
roots.

## Object reference counting

`Header::rc` counts mutable references to an individual object or array.
Register and field references use the same `inc()` / `dec()` operations.
Reaching zero collects the individual object. Headers already claimed by the
collector use the `Finalizing` header state, whose increments and decrements
are no-ops. The write barrier separately rejects storing readonly borrowed
values.

Object RC collects acyclic garbage immediately. Mutable cycles are reclaimed
by synchronous trial deletion at copy, scheduling, completion, and cown
teardown boundaries.
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

The already-finalizing intrusive garbage list transfers directly to the
collector before any fields are dropped. Copying drains the candidate table
before tracing the source, after dropping construction references, after
post-copy hooks, and after consuming source roots. Each drain repeats if
finalization creates further candidates.

Readonly behavior arguments remain borrowed for the behavior lifetime. Cown
handles themselves are independently reference counted.

## Stores

`writebarrier.h` now implements ordinary managed exchange semantics:

- reject writes to immutable storage;
- reject readonly values and register references;
- retain or transfer the incoming value;
- install it;
- release the outgoing value.

Initialization may write immutable storage only with immutable or immortal
values.

## Copy-on-split boundaries

Ownership boundaries deep-copy reachable mutable graphs:

- `freeze` copies, freezes the copy, and consumes the source reference;
- scheduling copies a closure into queued work;
- behavior completion copies each writable cown, including the result cown;
- teardown registration copies its callback into queued work.

The returned register, cown content, or queued callback is the destination
graph's explicit root.

Fresh copies are represented by `IsolatedGraph` until ownership is transferred
to their destination. Every fresh mutable node is reachable from its root, and
there are no external mutable references into the graph. Destroying an
`IsolatedGraph` therefore claims and finalizes its complete mutable graph
directly; it does not use mutable-cycle trial deletion. Shared, immortal, and
cown references are traversal boundaries and are released by normal
finalization.

Graph copying uses a single post-order traversal. First discovery allocates a
destination and records it in the source-to-destination map. The post-order
callback initializes its fields with unchecked writes. After traversal, the
allocation's initial reference represents the first incoming edge, including
the result root, and repeated mutable edges increment the destination during
traversal. Field initialization transfers those already-accounted references.
Non-copied references encountered by the traversal are incremented there and
transferred during field initialization. Post-copy hooks then run after the
complete graph is initialized.

`IsolatedGraph` is always a staging owner. Abandoning it suppresses destination
finalizers because successful paths transfer the root to its semantic owner
with `release()`.

Behavior completion first validates the result cown, then stages copies for
the result and every writable cown. Only after every copy and post-copy hook
succeeds does it install all roots with unchecked exchanges. It then releases
all source roots together with finalizers suppressed. A copy or hook failure
therefore cannot leave a partially committed set of cowns.

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

## Finalization

Each outer collection creates a stack-local collector. Reentrant collection
reaches it through one thread-local pointer. Mutable headers and classified
cycle lists enter the common header-finalization queue directly. Frozen SCC
roots use a separate work queue, while cowns remain intrusive.

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

The SCC root is marked `Finalizing` before discovery. Membership must test
whether `Header::find(member)` reaches a `Finalizing` header, not whether it
still returns the original root pointer: marking an intermediate member
`Finalizing` terminates the representative chain at that member. `Pending` is
reserved for SCC construction during freezing.

Collection within the active mutable batch is two phase:

1. run finalizers and drop managed fields;
2. release object storage.

`SuppressFinalizers` is used both when consuming source graphs whose semantic
lifetimes continue in copies and when reclaiming an abandoned staged copy
whose finalizers must not run. Suppression scopes do not nest. This ensures
that each semantic object is finalized exactly once.

Finalizers execute with readonly access. New objects allocated by finalizers
are ordinary mutable allocations. Acyclic objects collect immediately; cycles
enter the candidate table and are processed by the next explicit cycle drain.

## Cowns

A dormant cown owns its content through the content root. Writable acquisition
borrows that state for execution, and completion installs a deep copy back into
the cown. Readonly acquisition exposes only a readonly borrowed value for the
behavior lifetime.

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
component.

## Key files

| File | Purpose |
|------|---------|
| `frame_id.h` | Frame ordering for register-reference lifetime checks |
| `header_state.h` | Tagged header state, counts, SCC links, and collector links |
| `mutable_cycle_candidates.h/cc` | Thread-local mutable candidate RC records |
| `header.h` | Mutable RC, shared atomic RC, and collector links |
| `register.h` | Register ownership and transfer semantics |
| `writebarrier.h` | Managed field/array exchange |
| `copy_graph.h/cc` | Isolated copied-graph ownership, copying, and destination hooks |
| `freeze.cc` | SCC construction and immutable ARC conversion |
| `cown.h` | Cown lifetime and content ownership |
| `thread.cc` | Behavior, synchronous-call, teardown, and callback ownership |
| `collect.cc` | Reentrant two-phase object finalization |
