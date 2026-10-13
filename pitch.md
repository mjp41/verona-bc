
# Copy-on-split: data-race-freedom the easy way

Data-race freedom is a desirable property for programming languages.  It simplifies the programmer's need to understand what other threads are doing when reasoning about state.  When data-race freedom is provided through ownership, it also simplifies memory management: an owner can reclaim its unreachable objects without racing with another thread reading them.

Languages that use ownership can provide data-race-freedom.  The ownership model of Rust uses aliasing xor mutability (AxM): this forbids certain patterns of objects, but as such gives a checkable property. This ownership model provides data race freedom because the type system ensures that independently mutable states do not overlap.  However, Rust's ownership and borrowing rules cannot directly express many low-level data structures and optimisations. The standard library therefore uses `unsafe` internally to implement containers such as vectors, hash tables, trees, and linked structures, while exposing safe interfaces that preserve Rust's ownership guarantees.

There are more liberal designs such as Pony or Reggio, which use sharing xor mutability (SxM). These can be understood as ownership of regions, or reachable object graphs, rather than ownership of individual objects as in Rust. Their type systems prevent the same region from being mutable through more than one owner at a time, ensuring that independently mutable regions remain disjoint. This allows more object topologies than Rust, but the programmer must still understand and satisfy the region and capability restrictions.

In this document, we propose a new approach. Rather than require a proof that an existing graph can be divided safely, copy-on-split gives each resulting owner a copy of the state it requires. Overlap between those states causes copying rather than rejection.

Copy-on-split is different from the copying used by many actor systems. Actors keep their state separate, and many implementations copy values sent between actors. In Behaviour-Oriented Concurrency, a behavior temporarily combines its local state with the state of every cown it acquires. The behavior may create links between those states while it runs. When the behavior completes, the cowns must be separated again. Copy-on-split provides each cown with its own copy of the final state it requires.

Deep copying everywhere is obviously not ideal for performance. Copy-on-split supports a progression from dynamic graph transfer and partial copying, through static analysis and programmer annotations, to verified manual memory management. Failure to justify an optimization falls back to unconditional deep copying rather than rejecting the program.

Copy-on-split would let Verona keep the data-race freedom of Behaviour-Oriented Concurrency without making ownership proofs a prerequisite for programming. Unconditional deep copying provides a simple and correct implementation for every ownership split. Runtime checks, static analysis, annotations, and verification can then reuse storage and avoid allocations as the program is hardened and optimised. This gives Verona a path where programs run first, fast programs require more information only where it matters, and fully verified components can remove the remaining runtime memory-management costs.

## Behaviour-Oriented Concurrency

Actor systems make local reasoning easier by isolating each actor's state, but operations that must atomically access several actors are difficult to express. [Behaviour-Oriented Concurrency](https://doi.org/10.1145/3622852) (BoC) addresses this by allowing an asynchronous behavior to request exclusive access to several independent resources, called cowns.

A `when` synchronously spawns a behavior with a fixed set of required cowns, then the spawning computation continues. The behavior runs asynchronously once its cowns are available and earlier behaviors requiring the same cowns have completed.

The scheduler acquires all required cowns atomically. The behavior has exclusive access to their state until it completes; it cannot acquire more cowns or release any early. Behaviors with disjoint cown sets may run in parallel, while behaviors with overlapping sets run in spawn order. Nested `when`s have the same semantics as any other `when`.

The BoC scheduler prevents two behaviors from acquiring the same cown, but the paper explicitly relies on the underlying language for memory isolation. It assigns two obligations to the underlying language:

- **At spawn:** state accessed by the new behavior must be protected or uniquely owned by its required cowns.
- **At completion:** the states protected by the cowns being released must be disjoint.

Copy-on-split replaces these two proof obligations with copying. Scheduling copies captured state, while an implementation may transfer state that the spawning behavior no longer retains. Completion copies overlapping final state so that the released cowns are separate again.

## Copy-on-Split by Example

The examples use Verona-like pseudocode. They are intended to show where the semantics copies, rather than propose final surface syntax.

### Capturing state

The first ownership split occurs when a behavior captures mutable state that is still used by the scheduling behavior. This is the familiar copy-on-send case: scheduling sends the captured state to the new behavior while the continuation retains the original state.

```verona
let shared = cell(0);

when () ->
{
  assert shared.value == 0;
};

shared.value = 1;
```

When the `when` is scheduled, the copy relation gives the scheduling behavior and the new behavior separate graphs rooted at `shared`:

```text
scheduling behavior:  shared ──► x0(value = 0)

new behavior:         shared ──► x1(value = 0)
```

The labels `x0` and `x1` distinguish the destination objects; either may reuse the source location. The continuation then assigns `1` through its `shared` root. The new behavior still observes `x1.value == 0`, regardless of when it runs. Without the copy, the result would depend on the scheduling order.

If the continuation did not retain any reference to the graph, the implementation could realize the copy relation by transferring the original graph to the new behavior. This is still copying in the semantic sense; its implementation reuses the original locations because transfer does not introduce sharing between owners.

### Re-separating cowns

The second ownership split occurs when a behavior completes. While a behavior owns several cowns, it may create links between their states:

```verona
when (left, right) (l, r) ->
{
  let shared = cell(0);

  (*l).next = shared;
  (*r).next = shared;

  shared.value = 1;

  assert (*l).next.value == 1;
  assert (*r).next.value == 1;

  when (left) (l) ->
  {
    (*l).next.value = 2;

    when (right) (r) ->
    {
      assert (*r).next.value == 1;
    };
  };
};
```

While the outer behavior runs, `shared`, `(*l).next`, and `(*r).next` all refer to the same object:

```text
shared ────────┐
               ▼
(*l).next ───► x(value = 1)
               ▲
(*r).next ────┘
```

The assignment to `shared.value` does not copy anything. All three references are owned by the same behavior, so both assertions observe the assignment.

The nested behavior on `left` cannot start while the outer behavior still owns `left`. When the outer behavior completes, `left` and `right` must become separate owners again. Their final states overlap at `x`, so copy-on-split gives each cown its own copy:

```text
left cown:                 right cown:

left.next ──► x1(1)       right.next ──► x2(1)
```

The nested behavior changes `x1` to `2`. The later behavior on `right` still observes `x2.value == 1`.

This example highlights the central rule: assignment does not copy; copying occurs when ownership is split.

### Freezing and sharing immutable state

`freeze` returns an immutable snapshot of a mutable graph. It must not make the original graph immutable when the continuation retains an alias:

```verona
let mutable_config = cell(0);
let config = freeze mutable_config;

mutable_config.value = 1;
assert config.value == 0;
```

The `freeze` operation performs an ownership split between the continuation and the graph rooted at `mutable_config`. Because the continuation retains `mutable_config`, the two results must be separated:

```text
continuation:   mutable_config ──► mutable cell(value = 0)

freeze result:  config         ──► immutable cell(value = 0)
```

The implementation may reuse locations on either side when it can still produce these two results. After the split, mutation through `mutable_config` does not affect `config`.

No object in an immutable graph can be mutated, so the graph can subsequently be shared between owners even when it is reached through mutable state. Here the new behavior captures `state`, while the scheduling behavior retains it:

```verona
let state = cell(config);

when () ->
{
  assert state.value.value == 0;
};

assert state.value.value == 0;
```

The copy relation separates the mutable outer cell but preserves its reference to the immutable inner cell:

```text
scheduling behavior:  state ──► mutable cell ──┐
                                               ├──► immutable cell(value = 0)
new behavior:         state ──► mutable cell1 ─┘
```

Mutable destination graphs must be separate, but immutable graphs may be shared between them.

## What Copying Means

At an ownership split, copy-on-split applies a semantic copy operation. Copying is defined as a relation between heaps, not as an allocation algorithm. Consider a source heap `H` and, for each resulting owner `i`, an ordered sequence of roots `Ri`. Let `Gi` be the mutable object graph reached from the roots in `Ri` by following strong references. Immutable objects are not part of `Gi`; references to them are preserved without duplication. References to cowns are also preserved as handles, and traversal does not enter the state owned by a cown.

The copy produces a heap `H'` and a corresponding root sequence `Ri'` of the same length for each resulting owner. For each `i`, the mutable graph reachable from `Ri'` in `H'` must be isomorphic to `Gi`. At every position in the root sequences, a mutable destination root is the image under `fi` of the corresponding source root, while a primitive or immutable root is unchanged. The isomorphism `fi` also preserves object types, primitive field values, strong reference edges, and references to immutable objects and cown handles. It therefore preserves aliases between roots, as well as aliases and cycles within each destination graph.

The images of these isomorphisms must be pairwise disjoint. Even if two source graphs overlap, their corresponding destination objects are distinct:

```text
source:                    copied destinations:

left ──┐                  left1  ──► shared1
       ├──► shared
right ─┘                  right1 ──► shared2
```

`shared1` and `shared2` have the same state, but they are different mutable objects owned by different destinations. Mutation through one cannot affect the other.

The copy relation places no freshness constraint between locations in `H` and `H'`. Where pairwise disjointness permits it, a mapping such as `fi(x) = x` realizes the copy by reusing a location in place. A mapping such as `fi(x) = y` may realize it by allocating, moving, or compacting the object. These are implementation choices admitted by the relation, rather than separate semantic cases.

### Shared components

A shared component is a rooted heap component that copying preserves rather than duplicates. This covers the immutable object graphs introduced above, as well as cown and foreign-function interface (FFI) handles. Several owners may refer to the same component, and copy traversal stops when it reaches one. Because owners may create and destroy these references concurrently, shared components are atomically reference counted and reclaimed when their final strong reference is destroyed.

Handles are opaque shared components. The handle itself is immutable, but the cown state or external resource it denotes need not be; access is mediated by the handle's operations. For an FFI handle, destroying the final strong reference releases the external resource exactly once. Mutable managed objects that refer to a handle are copied normally.

### Weak references

Weak references do not extend a destination graph. After copying, a weak reference in destination `i` is redirected to `fi(x)` if its target `x` belongs to `Gi`. A weak reference to a shared component remains weak and continues to target that component while it exists; it does not contribute to the component's reference count. Otherwise, the weak reference is cleared.

### Identity-sensitive structures

Representations derived from object identity may need repair when copying changes those identities. For example, an identity-hashed table may need to rehash its keys. A type may provide a post-copy hook, which runs after all references, including weak references, have been rewritten. The hook receives only the destination object and can access only its destination graph and shared components. Any mutable object allocated by the hook belongs to that destination graph. The hook runs when the object's identity or a strongly reachable identity changes, and must preserve the abstract value and restore representation invariants. We assume that post-copy hooks satisfy these obligations.

### Finalization across ownership splits

Finalization occurs only when an object's semantic lifetime ends; reclaiming its former storage after its state has been copied elsewhere is not finalization. If copying leaves several destination images live, it forks the object's semantic lifetime. Each image then has an independent lifetime and is finalized when that lifetime ends. Treating discarded source storage as finalized would make the copying strategy observable: unconditional deep copying would run a finalizer, whereas in-place reuse would not. The two implementations would then produce the same copied graph but different finalization behavior.

The lifetime fork depends on reachability at the split. A stack variable is not retained merely because its lexical scope continues. If its last use transfers it into a closure and the continuation does not use it again, that transfer consumes the variable's reference-count contribution rather than adding another reference. The closure is then the sole root unless some other stack or heap reference reaches the same object.

In the examples below, `resource()` creates a finalizable managed object. Any native resource it denotes is held through a shared component.

In the simplest case, the closure is the resource's only root:

```verona
let r = resource();

when () ->
{
  use r;
};
// r is not live in the continuation.
```

The capture is the last use of `r`, so it transfers the existing reference into the closure. The runtime can transfer the resource into the new behavior without forking its lifetime.

Adding a heap reference changes the split even though `r` is still dead in the continuation:

```verona
let r = resource();
let holder = cell(r);

when () ->
{
  use r;
};
// r is not live in the continuation.

holder.value = none;
```

At scheduling, the continuation retains the resource through `holder`, while the new behavior captures it through `r`. Scheduling therefore forks the resource's lifetime. The final assignment makes the resource unreachable from the continuation's image and finalizes it, while the behavior's image remains live and is finalized only when its own lifetime ends. Heap reachability from the continuation, rather than the lexical scope of `r`, determines whether scheduling forks the lifetime.

When a behavior completes, the final states of its writable cowns, together with its result, determine the live source objects. The remaining behavior-owned objects form a fixed garbage set. Finalization processes exactly this set; dropping its references does not recursively add objects to it. Every object in the garbage set remains allocated until all finalizer bodies in the set have run. Live source objects also remain allocated while garbage finalizers run, so a garbage object may safely refer to a live object. An object replaced during the behavior, for example, belongs to the garbage set if no final root reaches it.

Each destination graph is copied independently. The semantics does not order garbage finalizers relative to post-copy hooks. It requires only that a hook run after its complete destination graph has been initialized, and that source objects remain available while garbage finalizers may read them. Once these uses are complete, source storage not reused by any destination can be reclaimed without running its user finalizer, while still releasing its references to shared components.

If one destination reuses a source object, that destination continues the existing lifetime. If several destination images remain live, the split forks the lifetime and each image is finalized when its own lifetime ends.

Native resources must be represented by shared components rather than by independently finalizable values embedded in copied objects. Copying a mutable wrapper therefore copies a reference to the shared component, not the native resource itself. The shared component releases the native resource exactly once, when its final strong reference is destroyed.

## Cycle collection in a reference-counted implementation

Cycle collection is an implementation concern rather than part of the copy semantics. It may reclaim storage but does not change the semantic lifetimes or finalization obligations established by the copy relation. It nevertheless requires care because ordinary reference counting cannot reclaim a copied source cycle after its last external root is consumed.

For example, suppose `finalizable_cycle()` returns the sole external root of a two-node cycle whose root object has a user finalizer:

```verona
let a = finalizable_cycle();
let frozen = freeze(a);
// The freeze is the last use of a.
```

`freeze(a)` creates an immutable copy of the two-node cycle and consumes its sole external source reference. Decrementing that reference does not reclaim the source nodes because their internal references retain each other. If ordinary cycle collection later reclaims the source cycle, it runs the source root's finalizer even though that semantic lifetime continues in `frozen`. The immutable root remains finalizable and runs its finalizer when its lifetime eventually ends, making the copying strategy observable.

The [`copy_freeze_cycle_finalizer`](testsuite/vir/copy_freeze_cycle_finalizer/copy_freeze_cycle_finalizer.vir) VIR example observes the distinction without a post-copy hook. The root finalizer schedules an increment on a shared counter, and each increment sets the exit code to its new value. Finalizing only the destination root therefore produces `1`; finalizing both source and destination roots produces `2`. The runtime currently produces `2` because consuming the source root does not collect its cycle, which is later reclaimed and finalized by ordinary cycle collection. This remains a known limitation until backup cycle detection is implemented.

An unreachable cycle can also retain an otherwise live source object. Suppose a garbage cycle refers to `x`, while `x` remains reachable through an external root that is about to be copied. Consuming that root after the copy does not reclaim the source image of `x`, because the garbage cycle still refers to it. If ordinary cycle collection later reclaims the garbage cycle, it also releases and finalizes the source image of `x`, even though `x`'s semantic lifetime continues in the destination.

The [`copy_freeze_garbage_cycle_finalizer`](testsuite/vir/copy_freeze_garbage_cycle_finalizer/copy_freeze_garbage_cycle_finalizer.vir) VIR example observes this case without a post-copy hook. The finalizer schedules an increment on a shared counter, and each increment sets the exit code to its new value. Finalizing only the destination therefore produces `1`; finalizing both the retained source and the destination produces `2`. The runtime currently produces `2` because the uncollected garbage cycle retains the source through the copy; later ordinary cycle collection releases and finalizes it. This also remains a known limitation until backup cycle detection is implemented.

A reference-counted implementation can avoid this duplicate finalization with two collection phases around the copy. Before copying, ordinary reference-count and cycle collection runs to a fixed point while all source roots remain present. This phase normally finalizes every object unreachable from all original roots. The runtime then copies the complete graphs reachable from the source roots. After constructing those destination graphs, it consumes the original source roots and runs collection to a fixed point again, suppressing user finalizers for objects made unreachable by consuming those roots. Any such source object was reachable from a copied root immediately before its consumption and therefore has a destination image. The second phase can consequently reclaim source cycles represented by destination images without running their user finalizers, while still destroying fields and releasing references to shared components. These collection points require backup cycle detection that the current runtime does not yet implement. The examples therefore document the present limitation rather than blocking the rest of the copy-based design work.

An object retained through another root survives the second pass. Such an object represents a genuine lifetime fork and is finalized normally when that remaining lifetime ends.

## Optimization Ladder

The copy relation defines the required result without prescribing how it is constructed. Every implementation must produce the required destination isomorphisms and pairwise disjoint mutable images, treat shared components as boundaries without duplicating them, preserve finalization obligations, and run any required post-copy hooks.

The levels below are not separate language modes. An implementation may choose a different level for each ownership split, or for different parts of one graph. Failure to justify an optimization falls back to unconditional deep copying, which is one direct realization of the copy relation.

### 1. Unconditional deep copying

The simplest implementation allocates fresh mutable objects for every destination graph, rewrites their references, and runs any required post-copy hooks. This requires no ownership analysis and directly realizes the copy relation.

### 2. Dynamic graph transfer

The runtime can realize the semantic copy entirely through reuse when it can establish that no other resulting owner can reach the graph. We call this implementation strategy graph transfer. Reference counting provides one way to detect it dynamically.

The runtime traverses the candidate graph and counts the incoming strong managed-heap references to each object from within that graph. It compares this internal count, together with the roots being transferred, with the object's reference count. If they account for every strong managed-heap reference, then no state retained by another resulting owner can reach the object. The graph can realize its destination image by reusing the existing locations.

### 3. Partial copying

If an object's reference count is larger than this accounted total, a reference reaches it from outside the candidate graph. These objects are entry points to the overlap between resulting owners. The runtime can trace from those entry points and copy the overlapping state, while transferring the remainder. At a split between several owners, one destination image may reuse a physical object while the other images use distinct locations.

A split runs synchronously while the current behavior exclusively owns the mutable source graph. No other behavior can access that graph or change its strong references until the split completes. Shared components are excluded from this traversal and maintain their own atomic reference counts.

### 4. Static ownership or region analysis

A compiler can prove that graphs are isolated or that destination graphs are disjoint before the split occurs. A successful proof allows the copy relation to be realized by transferring, reusing, or partitioning storage without allocating corresponding objects. Failure to prove the property does not reject the program; it selects one of the dynamic or deep-copying implementations above.

### 5. Programmer annotations

Programmer annotations can state expected ownership, isolation, or lifetime properties at performance-critical splits. The compiler, verifier, or runtime must check these claims before using them to reuse storage. An unproved or failed annotation falls back to unconditional deep copying rather than weakening the ownership guarantee.

### 6. Verified manual memory management

A separation-logic proof can verify explicit allocation, mutation, ownership transfer, and deallocation for selected components. Those components can then realize copies by reusing state where the proof establishes disjointness, and can use entirely manual memory management without runtime reference counting, tracing, or backup cycle detection.

At every level, the result must satisfy the copy relation and the requirements on shared components, weak references, finalization, and post-copy hooks.

## Next steps

Copy-on-split replaces BoC's separation obligations with a semantic copy operation. Each resulting owner receives an isomorphic mutable graph with a disjoint image, while shared components remain shared. Unconditional deep copying gives this model a simple implementation, and stronger ownership information permits increasing reuse. The next question is whether this simplicity survives a complete semantics and a practical implementation.

The semantics and implementation should develop together. A more thorough account of scheduling, completion, and freezing will provide the reference behavior, while an unconditional-deep-copy Verona-BC implementation will expose missing cases. Programs written against this baseline will show whether copy-on-split is easy to understand and useful in practice. These results should feed back into both tracks, and the formal development should ultimately prove that the copy relation preserves data-race freedom.

Once the baseline stabilizes, it can become the reference for successive optimizations, beginning with dynamic graph transfer and partial copying. Microbenchmarks should accompany each optimization and isolate the cost of individual split patterns. System benchmarks can then compare complete applications across the baseline, the optimized implementations, and current Verona-BC, measuring allocation, copied data, memory use, latency, and throughput.
