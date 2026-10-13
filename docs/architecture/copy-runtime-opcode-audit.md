# Copy Runtime Opcode Audit

**Status: Target.**

This audit classifies the VIR and VBC operations affected by the move from
region enforcement to copy-on-split. It defines the contract that the baseline
runtime must preserve before the region implementation can be removed.

The copy-on-split semantics observes values, aliases, mutation, ownership
splits, finalization, and external roots. It does not observe which region
contains an allocation. An operation belongs in the semantic contract only
when changing or removing it can change one of those observations.

## Summary

| Classification | Operations | Required action |
|---|---|---|
| Retained semantics | `copy`, `move`, `drop`, `freeze`, `read`, `when`, references, `load`, `store`, bulk array operations, callbacks, external scheduler resources, `pin`, `unpin`, generic object and array allocation | Preserve the operation's value-level effect. Remove region checks, dragging, and sendability checks from its implementation. |
| Simplified storage | `new`, `new [T]`, stores, external pinning, values crossing frames or entering cowns | Use ordinary managed allocation and reference counting. Storage placement must not introduce region errors. |
| Removed storage machinery | `stack`, `heap`, `region`, their array variants, `merge`, region kinds `rc` and `arena` | Canonicalize allocation to `new`, then remove these forms from VIR, VBC, and source builtins. |

Arithmetic, control-flow, type, memo, and FFI operations that do not retain or
mutate managed values are unaffected by this audit.

`singleton` remains an immortal singleton-value operation. It does not perform
ordinary managed allocation and is not a storage-placement operation. All
ordinary object and array allocation uses `New`, `NewArray`, or
`NewArrayConst`.

## Retained semantic operations

### Ownership and references

`copy`, `move`, and `drop` remain explicit because they determine when a
register adds, transfers, or releases a managed reference. The corresponding
argument and reference variants remain for the same reason.

Field and array references, `load`, and `store` also remain semantic
operations. A store must update the selected location and maintain ordinary
reference counts. It must not drag the incoming graph, merge allocation
regions, establish a parent region, or reject the write because of region
topology.

`arraycopy` and `arrayfill` are bulk stores and must use the same managed
exchange semantics for non-primitive elements. Their primitive fast paths
remain direct memory operations. `arraycompare` is read-only and remains
unchanged.

### Ownership splits

`freeze` remains a consuming ownership split. Its baseline behavior is to
produce an immutable deep copy and consume the source reference. In-place
freezing is a later optimization when reuse is justified.

`when` and dynamic `when` remain scheduling operations. They preserve cown
handles and copy the non-cown captured graph. The current runtime checks
region-based sendability after copying. The migration must remove that check:
successful graph copying establishes the copy-on-split isolation contract, so
region topology cannot invalidate the copied closure.

The resulting closure copy is transferred into the scheduled behavior's body
as an owning managed value. Behavior dispatch transfers that value into the
behavior frame, whose teardown releases it. Cown arguments continue to enter
the scheduler as transferred cown handles, and the result cown continues to own
the completed result. Removing region ownership must preserve this transfer
chain rather than merely deleting the sendability check.

`read` remains the operation that requests readonly access to a cown. It is a
scheduler access mode, not a region operation.

Behavior completion has no dedicated opcode. The interpreter must continue to
copy writable cown state before releasing acquired cowns. This runtime boundary
is part of the same ownership-split contract as `freeze` and `when`.

### Teardown callbacks

`atteardown` is another scheduling ownership split. Registration copies and
consumes the callback graph into a fresh allocation group, and the queued work
owns that group until it executes at scheduler quiescence. Cleanup runs with
the callback group as its allocation context, so allocations and mutations
performed by the callback remain within its graph. Reverse-registration
execution order is unchanged.

### Cown-owned state

Creating or replacing cown-owned state currently rejects stack values, drags
frame-local values, and records region ownership. The simplified runtime must
instead materialize a valid managed graph for the cown. It may transfer a
uniquely owned graph or copy when ownership is shared, but it must not reject
otherwise valid state because of storage placement.

Cown content replacement must preserve the same managed exchange invariant as
field and array stores: retain or transfer the incoming graph, install it,
then release the outgoing graph. The migration removes region-owner detachment
and attachment from this sequence without weakening its lifetime effects.

### External roots

`pin` and `unpin` remain because foreign code needs to add and remove external
roots for managed objects, arrays, and cowns. Pinning becomes a direct lifetime
operation on the value. It must not drag an object into a fresh region or keep
a region-owning cown alive indirectly.

`makecallback`, `codeptrcallback`, and `freecallback` also remain. A callback
closure currently borrows its callable; the enclosing Verona callback object
owns that callable and must keep it alive. Separately, `freecallback` releases
the native `CallbackClosure` and its libffi closure. The migration must
preserve both lifetime relationships without relying on the callable's region
or stack placement.

### External scheduler resources

`addexternal` and `removeexternal` remain scheduler-lifetime operations. They
keep the scheduler alive while native work can still schedule Verona
behaviors. Removing regions does not change their balancing or quiescence
semantics.

## Simplified allocation contract

The internal `New`, `NewArray`, and `NewArrayConst` nodes become the canonical
object and array allocation operations. Their textual VIR forms remain
`new @C(args)` and `new [T] size`, while their result types remain unchanged.
"Frame-local region" is no longer part of their contract. The baseline runtime
may allocate every result as an independently reference-counted managed value.

Values returned or raised across frames, registered for teardown, placed in
cown-owned state, captured by callbacks, or exposed through `pin` must remain
live through managed ownership. These paths currently reject stack values or
relocate frame-local graphs. Under the simplified contract they must copy,
transfer, or fall back to managed allocation so that storage placement does not
cause failure.

VC emits `New`, `NewArray`, and `NewArrayConst` for ordinary allocations and
compiler-generated blocks. Textual VIR no longer exposes `Stack`, `Heap`, or
`Region`, and no corresponding allocation node remains in rebuilt VIR. This
does not by itself remove the interpreter's call-frame stack data structure.

## Removed storage-placement operations

`stack` requests frame storage, `heap` allocates beside another value, and
`region` creates an allocation in a new `rc` or `arena` region. Their array
variants express the same placement choices. These distinctions have no
value-level effect in the copy-on-split semantics. In textual VIR and the
corresponding internal nodes, the canonical rewrites are:

- `stack @C(args)` becomes `new @C(args)`;
- `heap target @C(args)` and `region kind @C(args)` become `new @C(args)`;
- `stack [T] size` becomes `new [T] size`;
- `heap target [T] size` and `region kind [T] size` become `new [T] size`;
- `StackArrayConst`, `HeapArrayConst`, and `RegionArrayConst` become
  `NewArrayConst`;
- `StackArray`, `HeapArray`, and `RegionArray` become `NewArray`;
- `Stack`, `Heap`, and `Region` object nodes become `New`.

The removed target operand is no longer a liveness use. The removed region kind
has no replacement.

The runtime `merge` operation existed only to combine allocation regions. It
had no remaining semantic role after allocation became placement-neutral, so
it was removed across the source wrapper, compiler, VIR, VBC, interpreter, and
runtime. Compiler control-flow and type-lattice joins that also use the word
"merge" are unrelated.

VIR has no external stability guarantee, so its token definitions,
well-formedness rules, textual parser, validation, liveness, type checking, and
VBC emitter can remove these forms together. VBC is also experimental and has
no compatibility guarantee. Its enum entries, operand layouts,
`vbc::RegionType`, decoder cases, and region-type encoder can therefore be
removed in the same change, with all producers, consumers, fixtures, and
generated bytecode rebuilt together.

## Future optimization evidence

After allocation canonicalization, the prototype carries no explicit stack,
region, reuse, or non-escape instructions. This keeps generic managed
allocation as the only baseline storage contract.

The optimization ladder will later require evidence for stack placement,
in-place reuse, graph transfer, partial copying, and verified manual memory
management. That evidence needs a coherent redesign rather than preserving
individual operations from the region runtime. The redesign is outside this
prototype audit.

## Runtime machinery implied by the old contract

After the operation changes above, no semantic operation requires:

- region parent or cown-owner links;
- write-barrier tracing, dragging, or parent updates;
- dragging allocations between frame-local and heap regions;
- region merging;
- region-based sendability;
- `RegionRC` versus `RegionArena`;
- frame-owned regions used to make escaping values valid.
- runtime stack-allocation escape checks.

Managed stores still require exchange semantics: retain or transfer the
incoming value, install it, and release the outgoing value. Field stores,
array stores, `arraycopy`, and `arrayfill` must share that rule after the
region-specific write-barrier logic is removed.

Reference counting and backup cycle detection remain memory-management
mechanisms. The prototype retains a cut-down allocation group containing the
set of allocated object headers. Each dormant cown has a group, and each
executing behavior has a group that absorbs its writable cown state and
captured closure. Group membership determines only bulk reclamation
responsibility; it does not restrict references or operations.

Allocation-group ownership is represented by a move-only `AllocationGroup`
handle. Dormant cowns, queued behavior captures, and executing behaviors hold
these handles explicitly and transfer them when groups are absorbed. The
thread's current-allocation pointer is only a scoped allocation context; it
does not itself confer ownership.

Allocation groups preserve the finalization contract for acyclic graphs: an
abandoned source image is reclaimed without its user finalizer, while each
surviving destination image finalizes when its semantic lifetime ends. The
`freeze`, scheduling, and behavior-completion split paths all require this
rule. Backup cycle detection is deferred, so a cyclic source image may survive
source-root consumption and later finalize in addition to its destination
image. The migration must preserve this limitation explicitly, rather than
accidentally changing finalizer ordering or suppressing unrelated finalizers.
The reproductions are recorded in
[Known Issues](../known-issues.md).

## Migration order

1. **Canonicalize allocation in VIR.** Apply the object, dynamic-array, and
   constant-array rewrites above. Remove the discarded placement operands from
   liveness and type checking. Change compiler-generated block allocation from
   `Stack` to `New`. Remove `Stack`, `Heap`, and `Region`; remove
   `StackArray`, `HeapArray`, and `RegionArray`; remove `StackArrayConst`,
   `HeapArrayConst`, and `RegionArrayConst`; and remove `RegionRC` and
   `RegionArena` from VIR parsing and well-formedness after no node consumes
   them. Migrate or remove fixtures that test only placement.
2. **Remove merge end to end.** Completed: `mem::merge`, VC builtin
   registration and lowering, the VIR operation, shared-pass support, VBC
   emission, interpreter dispatch, and the runtime implementation were
   removed.
3. **Remove the corresponding VBC forms.** Delete the experimental bytecode
   opcodes, operand encoders and decoders, `vbc::RegionType`, and format
   documentation. Rebuild all generated bytecode and fixtures. This removes
   the serialized type only; the runtime region classes and their internal
   `RegionType` uses remain until the later runtime paths have been converted.
4. **Simplify managed writes and lifetime extension.** Replace the
   region-specific parts of write-barrier exchange while preserving managed
   retain, transfer, install, and release behavior. Apply the same rule to bulk
   array writes.
5. **Remove relocation paths.** Remove dragging and stack rejection from
   returns, raises, teardown callbacks, cown state, callback capture, and
   pinning. All managed allocations already use the generic lifetime model.
6. **Remove region validation.** Delete region-based sendability and the
   remaining storage-topology error paths.
7. **Reduce regions to allocation groups.** Remove parent tracking, region
   kinds, frame-local placement, dragging, merging, and their topology tests.
   Retain only header membership, external-root accounting, and two-phase bulk
   reclamation. Lins' cyclic reference-counting algorithm can later replace
   the allocation group's interim role in cyclic garbage collection.

The first implementation work package is allocation canonicalization. The
second removes `merge` across every producer and consumer. Keeping these as
separate changes makes each exposed contract change reviewable before the
reference-counting implementation is rewritten.
