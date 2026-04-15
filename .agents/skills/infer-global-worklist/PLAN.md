# Infer Pass: Clean-Room Rewrite Plan (v2)

## Goal

Rewrite `infer.cc` from scratch implementing the algorithm in
ALGORITHM.md. The prior plan (PLAN-v1.md) documented the first
rewrite and incremental patching attempts. Both approaches
produced half-ported states that were hard to debug. This plan
uses the skeleton-first approach that succeeded for the original
rewrite.

## Baseline

- **Algorithm**: ALGORITHM.md (Angelic constraints, product
  lattice, backward meet, Concrete constraint, partial order
  resolution)
- **Starting code**: revert `infer.cc` to `origin/main` (the
  prior rewrite at 92% / 3962 lines). The incremental patches
  are abandoned.
- **Reusable infrastructure**: CFG construction, method
  resolution, type constructors/predicates, subtype checking,
  `extract_constraints`/`apply_subst`, `infer_typeargs` — these
  are algorithmically independent of the solve loop.
- **Test suite**: 1105 tests, 1013 pass on baseline (92%).

## Architecture Summary (from ALGORITHM.md)

```
Product lattice per location:
  fwd[x] ∈ {TypeVar, Angelic(T), concrete S, Union(...), Dyn}
  ub[x]  ∈ {⊤, Union(...), concrete S, ∅}

Forward: join (⊔) — ascending
  F1: ⊔(TypeVar, X) = X
  F2: ⊔(Angelic(T₁), Angelic(T₂)) = Angelic(T₁ ∩ T₂)
  F2a: ⊔(Angelic(Concrete∩T₁), Angelic(T₂)) = Union(...)
  F3: ⊔(Angelic(T), concrete S) = S
  F4/F5: standard concrete join

Backward: meet (⊓) — descending, Angelic locations only
  ub[x] ⊓= T  (always intersection)

Refinement: candidates = members(fwd) ∩ ub
  R1: |candidates| = 1 → commit to concrete
  R2: candidates ⊂ members(fwd) → tighten Angelic bound
  R3: no change → skip
  R4: candidates = ∅ → contradiction flag

Resolution: Concrete → pick max under partial ≤; else → Union
```

## Reusable Code (copy from current infer.cc)

These sections are algorithmically neutral — they work with any
solve loop:

| Section | Lines | Function |
|---------|-------|----------|
| Dispatch tables | ~100 | `primitive_from_name`, `integer_types`, `propagate_lhs_ops`, etc. |
| Type constructors | ~50 | `primitive_type()`, `string_type()`, `ref_type()`, etc. |
| Type predicates | ~200 | `is_any_type()`, `extract_ref_inner()`, `extract_primitive()`, etc. |
| Method resolution | ~200 | `resolve_method_owner()`, `resolve_class_method_cached()`, `resolve_method()`, etc. |
| Generic helpers | ~150 | `build_class_subst()`, `extract_constraints()`, `apply_subst()` |
| `infer_typeargs()` | ~80 | TypeArg inference from call args |
| Call navigation | ~30 | `navigate_call()` |
| CFG construction | ~150 | `CFG::build()`, RPO computation |
| `is_lambda_function` | ~15 | Lambda detection |

Total reusable: ~975 lines (copied verbatim with fixes from R1-R4).

## New Code (written from scratch per ALGORITHM.md)

| Component | Est. lines | Description |
|-----------|-----------|-------------|
| Angelic type system | ~100 | `Angelic()`, `Concrete`, `IntSet`/`FloatSet` tokens, `members()`, `has_concrete()`, `is_angelic()` |
| `join_type()` | ~80 | Forward join: F1-F5 + F2a |
| `meet_type()` | ~60 | Backward meet: type set intersection |
| `GlobalInfer` struct | ~50 | `fwd`, `ub`, `fwd_exit`, worklist, cross-function maps |
| `build()` | ~60 | Seed fwd with params, seed ub with return types |
| `solve()` | ~80 | Worklist loop: forward push, backward push, refinement |
| `forward_pass()` | ~350 | Transfer functions (ported from current, adapted to new types) |
| `backward_pass()` | ~250 | Upper bound transfer functions (new: meet-based) |
| `push_fwd()` / `push_ub()` | ~40 | Push with enqueue |
| Refinement | ~60 | `candidates = members(fwd) ∩ ub`, R1-R4 dispatch |
| `finalize()` | ~300 | Write types to AST, resolution, error checking |
| Pass entry + sweep | ~40 | `infer()` PassDef, post-sweep |

Total new: ~1470 lines.

**Estimated total**: ~2450 lines (down from 3962).

## Iterations

### Iteration 0: Skeleton

**Goal**: New `infer.cc` compiles. `infer()` entry point calls
`GlobalInfer::run()`. All transfer functions are stubs. Tests
will fail but the build succeeds and the pass runs without
crashing.

**Deliverables**:
1. Copy reusable infrastructure (dispatch tables, type
   constructors, predicates, method resolution, CFG, helpers).
   Apply R1-R4 fixes during copy.
2. New Angelic type system: `Angelic()` constructor,
   `members()`, `has_concrete()`. `IntSet`/`FloatSet` as
   named tokens with static member arrays.
3. `join_type(existing, incoming, top)` — F1-F5 + F2a.
4. `meet_type(existing, incoming, top)` — intersection.
5. `GlobalInfer` struct with:
   - `fwd`, `ub`, `fwd_exit` per-label envs
   - `push_fwd()`, `push_ub()` 
   - RPO-ordered worklist
   - `build()`: collect functions, build CFG, seed fwd/ub
   - `solve()`: worklist loop with forward/backward push +
     refinement (R1-R4). Transfer functions are stubs.
   - `finalize()`: stub
6. Pass entry point calling `GlobalInfer::run()` + sweep.
7. Architecture comment at top (80 lines from ALGORITHM.md §2).

**Validation**: `ninja install` succeeds. `dist/vc/vc build`
runs without crash on `hello`. No assertions.

**Review**: C++ expert reviews struct layout, ownership, and
the lattice helper signatures.

### Iteration 1: Forward Transfer Functions

**Goal**: `forward_pass()` produces correct `fwd_exit` for each
label. Backward and refinement still stubbed, so only explicitly
typed programs work.

**Port from current code** (adapt to new Angelic types):
- Const → `Angelic(Concrete ∩ IntSet/FloatSet)` or concrete
- Copy/Move, Convert, RegisterRef, FieldRef, ArrayRef
- Load, Store
- New/Stack, NewArray/NewArrayConst
- Call (with `push_args_to_callee`)
- Lookup, CallDyn, TryCallDyn
- Binops, Unops, Nulops, FFI
- When, Typetest, TypeAssertion

**Key differences from current code**:
- Const produces `Angelic(...)` not `angelic_int()`/`angelic_float()`
- No `is_default_int/float` checks — use `is_angelic()`
- No `refine_local_const` in forward — refinement is separate

**Validation**: Simple tests with explicit types pass.
`ring_basic`, `ring_call_typed`.

**Review**: Type theory expert verifies forward transfer
functions match ALGORITHM.md §3.

### Iteration 2: Backward Transfer Functions

**Goal**: `backward_pass()` produces upper bounds using meet.

**Write from scratch** per ALGORITHM.md §4:
- Call: `ub[arg] ⊓= param_type`
- Store: `ub[val] ⊓= field_type`
- Copy/Move: `ub[src] ⊓= ub[dst]`
- New: `ub[arg] ⊓= field_type`
- Return: `ub[ret] ⊓= func_return_type`
- Binops: `ub[lhs] ⊓= ub[dst]`; `ub[rhs] ⊓= fwd[lhs]`
- FFI: `ub[arg] ⊓= ffi_param_type`
- Skip non-Angelic locations (§4.3)
- Angelic forward types contribute `members()` not themselves

**Key differences from current code**:
- `merge_bwd` uses `meet_type` not `merge_type`
- No backward-specific `refine_local_const`
- Cross-label push uses `push_ub` with meet

**Validation**: Literal inference works. `ring_int_constrained`,
`ring_int_default`. E1-E8 from ALGORITHM.md pass.

**Review**: Static analysis expert verifies backward meet
convergence and cross-label push correctness.

### Iteration 3: Solve Loop + Refinement

**Goal**: Full solve loop with forward push, backward push,
refinement (R1-R4), path-sensitive Cond handling.

**Write from scratch** per ALGORITHM.md §5-6:
- Forward push: after forward_pass, push fwd_exit to successors
  via `push_fwd` (using `join_type`). Cond: typetest narrowing.
- Backward push: after backward_pass, push ub_entry to
  predecessors via `push_ub` (using `meet_type`). Path-sensitive
  filtering on Cond edges.
- Refinement: `candidates = members(fwd) ∩ ub`. R1-R4 dispatch.
  Preserve Concrete in R2. Re-enqueue on change.
- Cross-function: `push_param_type`, `push_return_constraint`,
  `push_shape_to_lambda`, `push_args_to_callee`.
- Iteration cap with error.

**Validation**: All worked examples E1-E11 from ALGORITHM.md.
Full test suite: target ≥ 92% (match baseline). New tests for
backward meet intersection cases.

**Review**: All three experts review the solve loop:
- Type theory: refinement correctness, R1 safety invariant
- Static analysis: convergence, worklist ordering, no cycles
- C++: ownership, performance, no UB

### Iteration 4: Finalization

**Goal**: `finalize()` writes converged types to AST.

**Port and adapt from current code**:
- Const type writes from fwd_exit
- TypeAssertion removal
- NewArrayConst/tuple type updates
- Param/FieldDef type updates from fwd_exit
- Return type inference from fwd_exit at Return labels
- TypeArgs re-derivation from converged envs
- Error reporting for unresolved types
- Resolution per §1.4 (partial order for Concrete, Union for
  non-Concrete)

**Validation**: Full test suite. Target: ≥ 92% (match baseline),
ideally > 92% from improved inference.

**Review**: C++ expert reviews AST mutation correctness.

### Iteration 5: Post-Pass Sweep + Polish

**Goal**: Clean up remaining Angelic/Default types in AST.
Apply polish.

**Deliverables**:
- Post-pass sweep: traverse AST, resolve any remaining
  `AngelicSubtype` / `DefaultInt` / `DefaultFloat` via §1.4.
- Post-sweep assertion: no unresolved types remain.
- Golden file regeneration.
- Remove dead code, add final comments.

**Validation**: All tests pass with regenerated golden files.
Target: > 92%.

## Review Protocol

Each iteration gets a focused review from the relevant expert(s)
BEFORE proceeding to the next iteration. The review checks:

1. **Does the code match ALGORITHM.md?** Every join/meet/refinement
   rule must trace to a section in the algorithm document.
2. **Are there regressions?** Full test suite at each iteration.
3. **Are there new edge cases?** Each reviewer brings a set of
   adversarial examples.

If reviewers disagree, the issue is escalated (not overridden).

## Failed Approaches (from PLAN-v1.md)

These are documented in PLAN-v1.md and MUST NOT be repeated:
1. Incremental patching of union→meet in backward merge
2. Post-convergence cascade
3. merge_type with Angelic narrowing (breaks monotonicity)
4. Forward Lookup resolving on default representative
5. FIFO worklist (use RPO)
6. Regex-based profiling removal (destroys brace structure)

The clean-room skeleton approach is the ONLY path that has
succeeded. Do not attempt incremental modification of the
existing solve loop.
# Global Worklist Inference Plan

## Goal

Replace the ad-hoc inference algorithm that repeatedly mutates the AST to
communicate state changes between functions. Instead:

1. A single side-structure records the current inferred state for each label
   and identifier — no AST mutations during iteration.
2. A single global worklist processes ALL labels in the program (including
   lambda-lifted functions) uniformly, eliminating the confused multi-level
   processing (outer deferred loop → per-function → per-label).
3. The AST is mutated exactly once, at finalization, after the fixpoint is
   reached.
4. Placeholder nodes for inferred type arguments are maintained in the side
   structure, not injected into the AST during iteration.

## Current State (Updated April 15, 2026)

The clean-room rewrite is substantially complete. `infer.cc` is ~3962
lines (down from 6364). 1013/1105 tests pass (92%). The architecture is:

- **Global worklist** with RPO-ordered scheduling
- **No AST mutations during solve** (except local TypeArgs write)
- **AngelicSubtype** node type for default literal inference
- **Extended refinement** covers both entry AND exit envs
- **No post-convergence cascade or re-pass** — all refinement is solve-time
- **RPO worklist ordering** — processes labels in reverse post-order
- **Backward CallDyn handler** resolves default receivers from backward constraints

### AngelicSubtype Design

`AngelicSubtype(T)` wraps an upper bound T, meaning "will become some
S where S <: T, determined by backward constraints." Used for:
- `Angelic(DefaultInt)` — unresolved integer literal
- `Angelic(DefaultFloat)` — unresolved float literal
- Future: `Angelic(any)` — unresolved type parameter (replaces TypeVar)

**Merge semantics (forward joins)**: Angelic is opaque to `merge_type`
— structural equality only. Angelic is less information than concrete,
so `merge(concrete, Angelic(T))` → concrete wins (keeps concrete).
`merge(Angelic(T), concrete)` → concrete wins (widens to concrete).
Two angelics with same structure → no change.

**Key insight**: Angelic narrowing happens ONLY in the refinement step,
never in `merge_type`. This prevents the forward-backward oscillation
that caused convergence failures in earlier attempts. Forward joins are
purely monotone (Angelic is a fixed point when both sides agree).

At forward joins, Angelic semantics are:
- Angelic is "angelic" — context (backward) CHOOSES the subtype
- Union is "demonic" — value COULD BE any member
- At a join, both branches must agree: if both have `Angelic(T)`,
  the join is `Angelic(T)` (intersection of bounds = same bound).
  Treating Angelic as opaque achieves this via structural equality.

**Refinement**: The refinement step iterates BOTH `fwd[label]` (entry
env) AND `fwd_exit[label]` (exit env with locally-defined variables).
When an Angelic/Default/TypeVar type has a compatible backward
constraint in `bwd[label]`, it is narrowed to the backward type.
Both entry and exit envs are updated, and the label is re-enqueued.

**Backward Copy handler**: Only pushes backward constraints when the
expected type is concrete (not Angelic). This prevents Angelic types
from flowing backward through Copy chains as uninformative noise.
The backward constraint must come from a concrete source (e.g., a
Call param type, a return type, a Store field type).

**Finalization**: Unresolved `Angelic(DefaultInt)` → u64,
`Angelic(DefaultFloat)` → f64, `Angelic(any)` → error.

### RPO Worklist Ordering

Labels are processed in Reverse Post-Order: entry labels first,
return labels last. Computed via DFS from each function's entry
label during `build()`. The worklist dequeues the label with the
lowest RPO index.

This ensures forward types are computed at predecessors before
successors need them, and backward constraints from successors
reach predecessors before they process. For DAGs, this is optimal
(single pass). For loops (back edges), the worklist re-iterates
at loop heads until convergence (O(lattice_height) iterations).

Impact: 88% → 92% (54 more tests passing). Fixed all
timing-dependent default literal refinement failures.

### Theory-Lens Analysis (April 15, 2026)

| Aspect | Status |
|--------|--------|
| Forward monotonicity | SOUND — Const handler preserves concrete types; Angelic never reintroduced |
| Absorption | SOUND — `merge(concrete, Angelic) → {}` by design |
| Convergence | SOUND — monotone lattice, RPO reduces iterations |
| `merge_type` commutativity | SOUND — asymmetric calling convention, but join *value* is commutative |
| `merge_type` associativity | SOUND (informal) — holds via union cleanup normalization; no formal proof |
| Path-sensitive backward | SOUND but OVER-CONSERVATIVE — true-edge gates instead of intersecting (completeness loss, not unsoundness) |
| Backward union semantics | SOUND across labels; INCOMPLETE within label — intra-label uses union but should intersect (see Future Work §3) |
| Default vs TypeVar refinement | SOUND — well-motivated by value-level vs type-level polymorphism distinction |
| Cross-function cycles | SOUND — monotone + bounded lattice implies termination; no formal complexity bound (safety net adequate) |
| Mechanism completeness | SOUND — constraints propagate correctly; scheduling was the only gap (fixed by RPO) |
| Angelic nesting | SOUND — Angelic inside Union is semantically valid (see below); sweep handles via traverse |
| Angelic-Angelic merge | SOUND — `Union(Angelic(X), Angelic(Y))` = demonic branch choice + angelic within each bound; correct (see below) |
| Liveness dead code | GAP — liveness computed but never queried; wasted work. Intended for forward push filtering (R26) |

### Remaining Work (26 real failures + golden mismatches)

#### Priority 1 — Cross-function type flow (8 tests)

| # | Tests | Error | Fix |
|---|-------|-------|-----|
| 1 | `when`, `when_missing_lookup` | Cannot infer type of parameter | Write cown inner types to lambda param AST at finalization |
| 2 | `callback_field_runtime`, `callback_generic_apply`, `callback_shape_field`, `liveness_drop_fieldref` | callback::0 not subtype of callback::1 | Generic callback TypeArgs inference — lambda type mismatch |
| 3 | `infer_backward`, `ring_generic_bwd` | wrapper::1 not subtype of wrapper::0 | Backward Call TypeArgs re-inference from return constraint |

#### Priority 2 — Default literal edge cases (6 tests)

| # | Tests | Error | Fix |
|---|-------|-------|-----|
| 4 | `match_nomatch`, `match_value`, `string_api`, `when_match_before_send` | u64 not subtype of usize | `usize` handling — backward constraint from method param not reaching Const |
| 5 | `match_value_infer`, `raise_infer` | u64 not subtype of i32 | Match/raise desugaring backward flow |
| 6 | `match_value_match` | i32 not subtype of u64 | Match desugaring — TryCallDyn backward |

#### Priority 3 — Other inference (5 tests)

| # | Tests | Error | Fix |
|---|-------|-------|-----|
| 7 | `each_loop`, `reify_union_wrapper_return` | Cannot infer return type | Lambda return type from shape/caller constraints |
| 8 | `match_basic`, `iowise_regression` | Union(i32, u64) | Angelic partially refined at join — one branch narrowed, other stayed u64 |
| 9 | `ring_lambda_cycle` | Could not resolve field type | Lambda capture FieldDef TypeVar not resolved |

#### Priority 4 — Miscellaneous (6 tests)

| # | Tests | Error | Fix |
|---|-------|-------|-----|
| 10 | `ffi_struct_layout`, `ident_alias_ordering`, `type_alias_create_sugar` | Array(usize) not subtype of usize | `_builtin/ffi/struct.v` type mismatch |
| 11 | `partial_app` | Core dump | Debug with GDB |
| 12 | `array_bulk` | (empty/timeout) | Convergence or timeout issue |
| 13 | `tuple_err_over` | Error message differs | Golden file regeneration |

#### Final step — Golden file regeneration (~65 tests)

Tests that compile correctly but produce different intermediate
dump output due to AngelicSubtype tokens. Regenerate after all
real fixes are done.

### Failed Approaches (documented for posterity)

1. **Naive Union expansion**: Replaced DefaultInt with Union(i8,...,usize).
   Convergence was fine (800 iterations) but initially appeared to hang
   due to unrelated bugs. Abandoned prematurely. Later investigation
   proved it was viable but superseded by AngelicSubtype.

2. **Post-convergence cascade**: Forward dataflow pass after convergence
   to refine remaining defaults. Worked (92%) but was architecturally
   unsound — post-convergence fixes can't trigger further backward
   propagation, leading to cases requiring multiple cascade passes.
   A minimal example was found (`wrap[T] + consume`) where the cascade
   could not re-infer TypeArgs from backward constraints.

3. **Forward Lookup resolving on default representative**: Committed
   u64 too early in forward, causing forward-backward conflicts at
   joins (u64 vs i32 → Union(u64, i32) instead of narrowing).

4. **merge_type with Angelic narrowing**: Added narrowing logic inside
   merge_type (Angelic(T) + concrete S where S <: T → S). Caused
   oscillation because narrowing and widening competed on every push,
   breaking monotonicity. Root cause: forward and backward both call
   merge_type, so narrowing in merge_type fires in BOTH directions.
   The fix: narrowing happens ONLY in the refinement step.

5. **Local refinement re-pass** (~200 lines): After backward_pass,
   walked the body forward refining locally-defined defaults from
   bwd_entry, then re-ran forward_pass. Replaced by extending the
   refinement step to cover `fwd_exit[label]` (10 lines).

6. **FIFO worklist**: Original deque-based FIFO caused timing-dependent
   failures (88%) where backward constraints arrived after the source
   label had already been processed. RPO ordering fixed all of these
   (92%), confirming the theory-lens finding that the mechanism was
   complete but scheduling was wrong.

### Future Work

1. **AngelicSubtype for TypeVar**: Replace TypeVar with
   Angelic(any) to unify the DefaultInt/DefaultFloat/TypeVar
   special cases into one mechanism.

2. **RPO dequeue optimization**: Current dequeue scans entire set
   for lowest RPO index. Could use a proper priority queue keyed
   by RPO index for O(log n) dequeue.

3. **Angelic intersection algebra**: The current Angelic design
   has a completeness gap for Union backward constraints. The
   key identity is:

   `Union(Angelic(T1), Angelic(T2)) = Angelic(Isect(T1, T2))`

   Semantics: Angelic represents a literal whose type context
   will choose. At a forward join, both branches constrain the
   same literal — it must satisfy BOTH bounds, so the bound
   tightens (intersection). This is distinct from demonic Union
   where the VALUE differs across branches.

   **Two changes required**:

   (a) **Forward merge**: `merge(Angelic(T1), Angelic(T2))` where
       T1 ≠ T2 → `Angelic(Isect(T1, T2))`. Currently produces
       `Union(Angelic(T1), Angelic(T2))` which loses the
       intersection structure.

   (b) **Intra-label backward intersection**: When the same
       location gets multiple backward constraints within ONE
       label (sequential execution, not branches), intersect
       instead of union. Currently `merge_bwd` uses `merge_type`
       (union). Cross-label backward push remains union
       (may-analysis at unknown control flow).

   **Convergence**: (a) is monotone — Angelic(Isect(T1,T2)) is
   more information than Union(Angelic(T1), Angelic(T2)), and
   once committed to a tighter Angelic bound, future merges
   with the same or wider Angelic can only maintain or tighten.
   (b) is safe because bwd_entry is recomputed from scratch on
   each backward_pass — inputs (bwd_exit) grow monotonically
   via cross-label union, so the intersection of a growing set
   of constraints can only shrink or stabilize.

   **Refinement impact**: With tighter Angelic bounds, the
   refinement step can fire on Union backward constraints IF all
   Union members are within the Angelic bound. E.g.,
   `Angelic(Union(i32, i64))` with bwd `i32` → refine to `i32`
   (i32 is within the bound). Without this, the literal falls
   to u64 and later typechecking fails.

   **Test examples** (to be created under `testsuite/v/`):

   | # | Name | Code pattern | Expected | Currently |
   |---|------|-------------|----------|-----------|
   | E1 | `angelic_store_union` | `x = 0; y.f = x` where `y.f: Union(i32, i64)` | i32 or i64 (either valid) | u64 (Union bwd skipped) |
   | E2 | `angelic_store_isect` | `x = 0; y.f = x; z.f = x` where `y.f: Union(i32, i64)`, `z.f: Union(i32, u32)` | i32 (intersection) | u64 |
   | E3 | `angelic_branch_isect` | branch A: `x = 0; y.f = x` (y.f: Union(i32, i64)); branch B: `x = 0; z.f = x` (z.f: Union(i32, u32)); join | i32 (Angelic intersection at join) | u64 |
   | E4 | `angelic_branch_disjoint` | branch A: `x = 0; y.f = x` (y.f: i32); branch B: `x = 0; z.f = x` (z.f: string) | error or u64 (disjoint bounds) | u64 |
   | E5 | `angelic_call_union` | `f(x)` where param: Union(i32, usize) | i32 or usize | u64 |
   | E6 | `angelic_chain` | `x = 0; f(x)` (param: Union(i32, i64)), then `g(x)` (param: i32) | i32 (sequential intersection) | u64 |

   **Implementation plan**: This is a Phase C item — after the
   critical/high fixes (R1–R8) and the remaining test failures.
   The change to `merge_type` is small but the intra-label
   backward intersection requires either a separate `merge_bwd`
   function or a mode flag. Design the algebra first, then
   implement against the test examples.

### Rearchitect Plan (April 15, 2026)

The algorithm in ALGORITHM.md represents a significant redesign.
The rearchitecture was reviewed by three expert lenses (C++, type
theory, static analysis). All three confirmed the algorithm is
sound and the plan is implementable. Key consensus:

- Backward meet (intersection) fixes a known semantic bug
- `Concrete` as AST token is the right representation
- `IntSet`/`FloatSet` as named tokens with lookup tables
- F2 intersection correct under same-definition precondition
- Convergence sound on the product lattice
- `members()` should normalize to flat set after every R2

#### Implementation Phases

**Phase 0 — Foundation (prerequisite for all others)**

Pure functions with no worklist interaction, unit-testable:
1. `intersect_type(A, B, top)` — compute type set intersection.
   For primitives: set intersection on tokens. For class types:
   subtype checking. Returns the intersection or `∅`.
2. `members(angelic_bound)` — strip `Concrete` and `Angelic`,
   return flat set of concrete types. For named sets (`IntSet`),
   expand from static lookup table. Normalize nested
   Isect/Union to a flat canonical form.
3. `has_concrete(angelic_bound)` — check if `Concrete` tag present.
4. Define partial preference order for `IntSet` as a static
   comparison function. Signed chain `i8 ≤ i16 ≤ i32 ≤ i64`,
   unsigned chain `u8 ≤ u16 ≤ u32`, both `≤ u64` (top).
   Signed/unsigned incomparable. Platform types incomparable
   with fixed-width except `≤ u64`.
5. Critical fixes R1-R4 from the review (navigate_call guard,
   backprop bound, static globals, lambda_returns_omitted).

**Validation**: Unit test `intersect_type`, `members()`, and
preference order. Build succeeds. No test changes.

**Phase 1 — Backward meet (highest-value change)**

Replace backward union with meet (intersection):
1. Add `push_ub(target_env, loc, type)` using `intersect_type`
   (separate from forward `push()` which uses `merge_type`).
2. Replace `merge_bwd` lambda body: call `intersect_type` instead
   of `merge_type`.
3. Replace cross-label backward push `push(bwd[p], ...)` with
   `push_ub(bwd[p], ...)`.
4. Initialize `bwd[label]` to `⊤` (Dyn) instead of empty.
   Seed return labels: `bwd[ret] ⊓= declared_return_type`.
5. Skip backward updates for non-Angelic locations (§4.3).
6. Remove dead liveness code (R8).

**Validation**: Run full test suite. Expect:
- Single-constraint cases: identical results (≥98% of tests)
- Multi-constraint cases: sharper types (improvements)
- Some golden file changes. Regenerate with `ninja update-dump`.
- No test should go from pass→fail.

**Phase 2 — Refinement update**

Update refinement to use the new `candidates = members(fwd) ∩ ub`
formulation:
1. Replace scattered refinement logic with unified dispatch:
   R1 (singleton→commit), R2 (tightened→narrow Angelic, preserve
   Concrete, re-enqueue), R3 (no change), R4 (empty→flag).
2. R2: normalize the Angelic bound to a flat set after tightening
   (prevents deep nesting from repeated intersections).
3. Remove the old `if (bwd_front->in({Union, Isect})) continue;`
   skip — Union upper bounds now participate in refinement via
   intersection with `members(fwd)`.
4. Remove `refine_local_const` — subsumed by unified refinement.
5. Add contradiction flag to `LocalTypeInfo` for R4 tracking.

**Validation**: Tests that previously fell to u64 due to Union
backward skipping should now resolve correctly. Expect
improvements in Priority 2 test failures (usize/i32 cases).

**Phase 3 — Angelic representation + forward F2**

Change the AST representation and forward join:
1. Add `Concrete` and `IntSet`/`FloatSet` AST tokens.
2. Update `wfPassInfer` well-formedness rules.
3. Replace `DefaultInt` → `Angelic(Concrete ∩ IntSet)` in
   forward Const handler. Replace `DefaultFloat` similarly.
4. Replace `is_default_int/float/type` predicates with
   `is_angelic() + has_concrete()` checks.
5. Change `merge_type` Angelic-Angelic case from opaque/structural
   equality to bound intersection (F2).
6. Add F2a: mixed-Concrete → demonic Union fallback.
7. Update `fwd_exit` seeding (lines 3090-3099) to preserve
   tighter Angelic bounds from prior iterations — use forward
   lattice ordering (call `merge_type` on both, use result).

**Validation**: Full test suite. F2 triggers rarely (same-def
at joins). Most tests unchanged. Golden file regeneration.

**Phase 4 — TypeVar → Angelic(Any)**

Unify TypeVar handling:
1. Replace `TypeVar` in type-param contexts with `Angelic(Any)`.
2. Remove `typevar_aliases` mechanism — subsumed by backward
   Copy propagation (`ub[src] ⊓= ub[dst]`).
3. Remove TypeVar special cases in `merge_type`, refinement,
   and finalization.
4. Update resolution: `Angelic(Any)` without `Concrete` resolves
   to `Union(candidates)` per §1.4.
5. Remove `contains_typevar()` checks — replace with
   `is_angelic()` or remove where unnecessary.

**Validation**: TypeVar tests. The semantic equivalence was
confirmed by the type theory reviewer: single-constraint and
Union-constraint cases produce identical results. The alias
tracking removal is the main risk — verify Copy chain tests.

**Phase 5 — Resolution + cleanup**

1. Replace post-pass sweep (`DefaultInt → u64` etc.) with
   unified resolution per §1.4.
2. Implement partial preference order resolution for Concrete
   Angelic types.
3. For non-Concrete (`Angelic(Any)`), resolve to
   `Union(candidates)` or error if unconstrained.
4. Remove `resolve_default()`, `default_literal_type()`,
   `contains_default_type()`, and other dead predicates.
5. Update architecture comment at top of file to match
   ALGORITHM.md.

**Validation**: Full test suite. No new test failures. Golden
file regeneration for final form.

#### Phase Dependencies

```
Phase 0 ──→ Phase 1 ──→ Phase 2 ──→ Phase 3 ──→ Phase 4 ──→ Phase 5
(helpers)   (bwd meet)  (refine)    (repr+F2)   (TypeVar)   (resolve)
```

Phases 1 and 2 can optionally be combined (they're tightly
coupled semantically). Phases 3 and 4 are independent of each
other but both depend on Phase 2.

#### Reviewer Consensus

| Aspect | C++ | Type Theory | Static Analysis |
|--------|-----|-------------|-----------------|
| Backward meet is safe first step | ✓ | ✓ | ✓ |
| IntSet as named token | ✓ | — | — |
| Concrete as AST token | ✓ | ✓ | — |
| F2 intersection correct | ✓ | ✓ | ✓ |
| Convergence | ✓ | ✓ | ✓ (proved) |
| R1 safety invariant | ✓ | ✓ (needs proof in doc) | ✓ (proved) |
| `members()` normalization | ✓ (flat set) | — | ✓ (Phase 0) |
| TypeVar→Angelic compat | — | ✓ (95%+ tests same) | — |
| Total test impact | ~5% golden churn | ≥95% identical | Low risk |

No unresolved disagreements between reviewers.

#### Risk Assessment

| Phase | Risk | Mitigation |
|-------|------|-----------|
| 1 (bwd meet) | Low — fixes known bug | Most tests single-constraint → no change |
| 2 (refinement) | Medium — core inference change | Golden file diff review |
| 3 (repr+F2) | Medium — representation change | Phase 1 test-neutral if mapping faithful |
| 4 (TypeVar) | Medium — alias removal | Copy chain tests; in-lattice propagation equivalent |
| 5 (resolution) | Low — final sweep replacement | Backward-compatible with current defaults |

### Four-Expert Review (April 15, 2026)

The implementation and plan were reviewed by four specialist lenses:
security, C++, type theory, and static analysis. Findings are
categorized below with a response plan.

#### Tier 0 — Critical (must fix before further feature work)

| # | Finding | Source | Action |
|---|---------|--------|--------|
| R1 | `navigate_call()` calls `defs.front()` without checking emptiness — UB on empty result | Security | Guard with `if (defs.empty()) return {};` |
| R2 | Finalize TypeVar alias backprop loop has no iteration bound | Security | Add `max_iters = typevar_aliases.size() * fwd_exit.size()` safety cap with error |
| R3 | `active_method_cache` is a file-scope `static` raw pointer — thread-unsafe, non-RAII | Security, C++ | Already a `GlobalInfer` member (`method_cache_storage`). Remove the static pointer; pass cache via parameter to `resolve_class_method_cached()` |
| R4 | `get_lambda_returns_omitted()` is a file-scope `static` set of `void*` — thread-unsafe, fragile pointer identity | Security, C++ | Move to a `std::unordered_set<Node>` member of `GlobalInfer`. Remove the global accessor |

**Rationale**: R1 is undefined behavior. R2 is a potential infinite
loop. R3/R4 are thread-safety hazards and violate RAII — they also
make the code harder to reason about since lifetime is implicit.

#### Tier 1 — High (fix alongside next feature batch)

| # | Finding | Source | Action |
|---|---------|--------|--------|
| R5 | No post-sweep assertion that `AngelicSubtype`/`DefaultInt`/`DefaultFloat` are fully eliminated | Security, Type Theory | Add a `traverse()` assertion after the sweep in `infer()` entry point. Fail compilation if any remain |
| R6 | `infer_typeargs()` mutates AST TypeArgs during solve — violates "no AST mutations during solve" invariant | C++, Security | Track inferred TypeArgs in `call_typeargs` side-structure (already declared). Write to AST in `finalize()` only. Remove `replace_if_changed` from `infer_typeargs()` |
| R7 | `push()` returns `true` when only `call_node` metadata changes (no type change) — causes spurious re-enqueue | C++ | Return `false` when only `call_node` changes. The metadata update is not a dataflow change and should not trigger re-processing |
| R8 | Liveness analysis computed but never queried — dead code wasting startup time | Static Analysis | Remove `Liveness` struct, `liveness` member, and `liveness.build(cfg)` call. Re-add as liveness-filtered forward push (R26) once profiling shows env sizes are a bottleneck |

**Rationale**: R5 is a soundness defence-in-depth. R6 is an
architectural invariant violation that could cause subtle bugs if
TypeArgs are read by other parts of the solve loop expecting
immutability. R7 wastes worklist iterations. R8 is dead code whose
intended use (env filtering) was never wired in — see R26 for the
proper integration plan.

#### Tier 2 — Medium (address for robustness/performance)

| # | Finding | Source | Action |
|---|---------|--------|--------|
| R9 | `dequeue()` is O(n) per extraction — linear scan for min RPO | All four experts | Replace `std::set<size_t>` + linear scan with `std::set<size_t, RPOCompare>` using a custom comparator that orders by `cfg.rpo_index[label]`. O(log n) dequeue via `begin()`. Fold `worklist_dir` into the set element or a parallel map |
| R10 | `resolve_method()` recursion through Union/ref/cown is unbounded | Security | Add a `depth` parameter with max 32. Return `{}` when exceeded |
| R11 | TypeAlias chain following in `resolve_method_owner()` and `push_shape_to_lambda()` has no cycle detection | Security | Add a visited set or max-depth counter (64) |
| R12 | `lookup_stmts` not scoped per-function — Location collision possible across functions | Security | Scope to `std::map<Node, std::map<Location, Node>>` keyed by function, or clear between functions |
| R13 | Path-sensitive backward filtering is over-conservative — true-edge gates instead of intersecting | Type Theory, Static Analysis | On true edge, compute `intersect(bwd_type, narrowed_type)` and push the result, instead of requiring `bwd_type <: narrowed_type` as a gate. Recovers completeness for `Union(i32, string)` through a typetest-true edge for `i32` |
| R14 | `Angelic(DefaultInt)` merged with `Angelic(DefaultFloat)` produces `Union(Angelic, Angelic)` | Type Theory | **No fix needed.** `Union(Angelic(X), Angelic(Y))` is semantically correct: demonic branch choice (runtime picks which path) + angelic within each bound (context picks type). This is strictly more precise than `Angelic(Union(X,Y))` which loses per-branch information. The code handles it correctly: `is_angelic()` returns false (top-level is Union), refinement skips it, and the sweep resolves nested Angelics via `traverse()`. Example: branches with `x.f : i32|i64` and `y.f : u32|u64` produce `Union(Angelic(i32|i64), Angelic(u32|u64))` — each branch retains its own constraint space |
| R15 | False-edge backward push is unconditional — plan specifies disjointness check but code omits it | Static Analysis | The unconditional push is sound (conservative). Adding disjointness checking is a precision improvement that can be deferred. Document the decision |
| R16 | `backward_pass` rebuilds `const_defs`/`def_stmts` maps on every invocation — per-label invariant | C++ | Precompute in `CFG::build()` alongside `func_def_stmts`. Store as `std::map<size_t, std::map<Location, Node>>` per label index |

#### Tier 3 — Low / Deferred (future optimization or hardening)

| # | Finding | Source | Action |
|---|---------|--------|--------|
| R17 | `TypeEnv = std::map<Location, ...>` with string keys — O(log n) per lookup on hot path | C++ | Consider `std::unordered_map` or Location interning. Benchmark first — the log factor may be dominated by other costs. Defer |
| R18 | Heavy `clone()` usage — deep copies on every push/merge | C++ | Type interning or COW for common types (`primitive_type(U64)` etc.) would reduce allocations. Defer to performance phase |
| R19 | `same_type_tree()` recursive without depth limit | C++ | Bounded by AST well-formedness from earlier passes. Add a depth parameter as hardening if stack overflow is observed |
| R20 | RPO suboptimal for backward analysis | Static Analysis | Separate forward (RPO) and backward (reverse-RPO) worklists would reduce iteration count. Deferred optimization — current approach is correct, just does extra work |
| R21 | `extract_constraints()` nested O(n²) method matching on shapes | C++ | Precompute method-name→Function map for shape classes. Defer unless profiling shows it's a bottleneck |
| R22 | `GlobalInfer` is a 2300-line monolithic struct | C++ | Splitting into sub-structs (CFGBuilder, ForwardAnalysis, etc.) improves maintainability. Defer — the code is actively evolving and splitting during development increases merge conflicts |
| R23 | Duplicate FFI lookup patterns in forward/backward | C++ | Extract to a shared `resolve_ffi_symbol()` helper. Minor — do when touching FFI code next |
| R24 | Convergence bound for cross-function cycles not formally stated | Type Theory | The bound is O(|L| × |V| × h) where h is lattice height. Document in architecture comment. The safety net is adequate |
| R25 | Backward documentation says "must satisfy all" but uses union | Type Theory | Union is correct across labels (may-analysis). Within a label, intersection is needed (must-analysis). See Future Work §3 for the Angelic intersection algebra design |
| R26 | Forward push sends all exit-env entries to all successors — dead variables waste merge work | Static Analysis, C++ | Use liveness to filter forward pushes: `if (!live_in[succ].count(loc)) continue;`. Requires correct liveness (fix def/use ordering bug in current `Liveness::build()`). See "Liveness-Filtered Push" section below |

#### Execution Order

**Phase A — Critical fixes (R1–R4)**: These are small, self-contained
changes. Do them immediately as a single commit.

1. R1: One-line guard in `navigate_call()`.
2. R2: Add iteration cap to finalize TypeVar backprop loop.
3. R3: Remove `active_method_cache` static; thread through param.
4. R4: Move `lambda_returns_omitted` into `GlobalInfer`.

**Phase B — High fixes (R5–R8)**: Do alongside the next batch of
test fixes (Priority 1–2 from Remaining Work).

5. R8: Remove dead liveness code (simplest — pure deletion).
6. R7: Fix `push()` return value for metadata-only changes.
7. R5: Add post-sweep assertion.
8. R6: Track TypeArgs in side-structure; write in finalize.
   (This is the most complex — requires `call_typeargs` plumbing.
   The field is already declared in `GlobalInfer`.)

**Phase C — Medium fixes (R9–R16)**: Do after all Priority 1–4
test failures are resolved.

9. R9: RPO-ordered worklist with custom comparator.
10. R10/R11: Recursion/cycle depth limits.
11. R12: Scope `lookup_stmts` per-function.
12. R13: Path-sensitive backward intersection (precision improvement).
13. R16: Precompute `const_defs` in `CFG::build()`.

**Phase D — Low/deferred (R17–R26)**: Address during performance
optimization or as needed.

#### Liveness-Filtered Push (R26)

The dead liveness code (R8) was intended to optimize what is stored
in each label's env. Analysis of the `hello` program shows:

- 670/685 functions have 1 label — no CFG, liveness irrelevant
- 15 functions have 2–23 labels, up to 55 distinct locals
- Without filtering, forward push sends ALL exit-env entries to
  ALL successors, even for locals that are dead at the successor

For `_builtin`-dominated programs, the impact is modest (max 55
locals × 23 labels). For larger user programs with complex control
flow, env bloat could become significant — each dead local in a
successor's env triggers a `merge_type()` call (which may invoke
`Subtype()`) on every worklist iteration.

**Integration plan** (Phase D, after profiling confirms need):

1. Fix the def/use ordering bug in `Liveness::build()`: process
   statements sequentially, adding uses before defs per statement,
   rather than bulk traversal + bulk removal.
2. Wire liveness into forward push:
   ```cpp
   for (auto& [loc, info] : label_exit)
     if (liveness.is_live_in(succ, loc))
       if (push(fwd[succ], loc, info.type, info.call_node))
         enqueue(succ, Direction::Forward);
   ```
3. Optionally prune `fwd[label]` entries for dead-on-entry
   locations during `build()` seeding.
4. Backward push does NOT need filtering — backward constraints
   only matter for locations that have definitions, and the
   backward transfer functions naturally skip irrelevant locations.

**Why defer**: The current bottleneck is correctness (92% → 100%),
not performance. Liveness filtering is a pure optimization that
does not affect the fixed point — it only reduces wasted work.
Add instrumentation (env size per label) first to validate the
hypothesis before investing in the fix.

#### Findings Considered and Rejected

1. **`Liveness::build()` has the classic def/use ordering bug**
   (Static Analysis): The liveness analysis removes all defs from
   uses in bulk, which loses upward-exposed uses (e.g., `x = x + 1`
   where the RHS use precedes the LHS def). This is a real bug in the
   liveness computation. Must be fixed before R26 integration.
   See "Liveness-Filtered Push" section above.

2. **`Liveness::build()` fixpoint has no iteration bound** (Security):
   Standard dataflow liveness is guaranteed to converge (sets grow
   monotonically, bounded by total variables). A safety net would be
   cheap insurance. Add one when re-implementing for R26.

3. **Direction merge `Forward + Backward = Both` wastes work**
   (Static Analysis): Running both when only one direction changed is
   conservative but correct. Separate forward/backward worklists (R20)
   would fix this. Not worth a standalone change.

4. **`const` correctness on parameters** (C++): `forward_pass` takes
   unused `func` param. Minor — remove when touching the function next.

5. **`snmalloc::UNUSED()` suppresses return values** (C++): These are
   intentional — the return values indicate local change that is
   already tracked via the `changed` flag. No action needed.

## Original Plan

(The original plan content follows below, retained for reference.)

All 1100 tests currently pass. Ring tests (`testsuite/v/ring_*`) are
planned as incremental validation targets but do not yet exist — they
should be created as part of the skeleton iteration.


## Architecture: Bidirectional Bounds

The core insight: type inference is two independent analyses that interact
through refinement.

- **Forward analysis** computes what each variable *could be* (upper bounds).
  Flows from definitions toward uses. Merges widen (union at joins).
- **Backward analysis** collects use-site constraints on each variable.
  Flows from uses toward definitions. Merges widen (union — a variable
  flowing to multiple uses must satisfy all of them, so constraints
  accumulate).
- **Refinement** resolves unresolved types (DefaultInt, DefaultFloat,
  TypeVar) when a backward constraint provides a concrete type.

These three concerns are cleanly separated. No `is_fixed` hack needed —
typetest narrowing is just an edge-specific tighter upper bound in the
forward analysis.

### Per-label state

```cpp
struct LocalTypeInfo {
    Node type;
    Node call_node;  // Call/CallDyn that produced this (for backward prop)
};
using TypeEnv = std::map<Location, LocalTypeInfo>;

// Per-label, two envs storing INPUTS:
std::vector<TypeEnv> fwd;  // fwd[i] = forward types at ENTRY of label i
std::vector<TypeEnv> bwd;  // bwd[i] = backward constraints at EXIT of label i
```

Both store what a label **knows before it starts processing** in the
respective direction. No `is_fixed` field. The forward and backward
channels don't contaminate each other.

### Push-based data flow

The key design choice: labels store their **inputs**, and processing a
label **pushes outputs directly into successor/predecessor inputs** via
`merge_type`. This eliminates the "build entry env" phase entirely.

**Forward processing of label `i`**:
1. Read `fwd[i]` (already built by predecessors pushing into it).
2. Run forward transfer functions over the body: `fwd[i]` → `fwd_exit`.
3. For each successor `s`: merge `fwd_exit` into `fwd[s]` via `merge_type`.
   If `fwd[s]` changed, enqueue `s` Forward.
4. For Cond terminators: push narrowed types directly into `fwd[true_succ]`
   and excluded types into `fwd[false_succ]`. No separate BranchNarrowing
   struct needed. If the variable being tested is `TypeVar`, the true
   branch receives the tested concrete type (e.g., `i32`), and the false
   branch receives `TypeVar` unchanged (we don't know what to exclude
   from an unresolved type). Once the TypeVar is refined via backward
   constraints, subsequent iterations will produce meaningful exclusions.

**Backward processing of label `i`**:
1. Read `bwd[i]` (already built by successors pushing into it).
2. Run backward transfer functions over the body in reverse:
   `bwd[i]` → `bwd_entry`.
3. For each predecessor `p`: push `bwd_entry` into `bwd[p]`, but only
   for locations where the backward constraint is **compatible** with
   the forward type on that edge (see below). If `bwd[p]` changed,
   enqueue `p` Backward.

**Path-sensitive backward push**: At a join point where one predecessor
produces `x: i32` (via typetest narrowing) and another produces
`x: string`, a backward constraint "x must be i32" is only valid for
the first predecessor. Pushing it blindly to both would incorrectly
refine the second path.

The only source of path-sensitive forward types is Cond terminators
(typetest narrowing). The rule: when pushing backward constraint
`bwd_entry[loc]` to predecessor `p`, check if `p`'s terminator is a
Cond that narrowed `loc` on the edge to `i`. If so, check compatibility
between the backward type and the narrowed forward type on that
specific edge. If incompatible, skip the push for that (predecessor,
location) pair. For non-Cond predecessors, push unconditionally.

Note: the old code does NOT do this — it pushes backward constraints
uniformly to all predecessors. This is a soundness improvement.

**Why this is better than storing outputs**:
- No "build entry env" merge phase — the entry IS the stored state.
- Change detection is trivial: `merge_type` returns whether it changed
  the target. If it did, enqueue. No whole-env comparison.
- Branch narrowing is natural: computed exit types are pushed directly
  into the appropriate successor's `fwd`, not stored as edge metadata.

### Forward transfer functions

Update the env based on definitions:
- `Const`: env[dst] = literal_type (DefaultInt, DefaultFloat, Bool, etc.)
- `Copy/Move`: env[dst] = env[src]
- `New`: env[dst] = class_type
- `Call`: env[dst] = return_type_of_callee
- `CallDyn`: env[dst] = resolved_method_return_type (see cross-product rule)
- `Binop/Unop`: env[dst] = result_type
- etc.

**Angelic cross-product rule for CallDyn/Lookup**: When the receiver
or arguments are Angelic, compute `result = Angelic({ Rij | Ti.method(Sj) → Rij })`
over the cross-product of receiver members × arg members. The receiver
is NOT refined — only the result type is computed. See ALGORITHM.md §3.1.1.

### Backward transfer functions

Propagate constraints from uses to defs:
- `Return x` where func returns `T`: bwd[x] ⊇ T
- `Call f(x)` where param is `T`: bwd[x] ⊇ T
- `Store ref.field = x` where field is `T`: bwd[x] ⊇ T
- `Copy/Move dst = src`: bwd[src] ⊇ bwd[dst]
- etc.

### Refinement

Refinement runs **inline at each label during the solve loop**, not as a
separate post-fixpoint phase. It has two forms:

#### Cross-label refinement (entry types)

After processing forward and backward for a label, for each location
in `fwd[i]` (the entry state):
- If `fwd` is `DefaultInt`/`DefaultFloat` and `bwd` is a compatible
  **single concrete** type, refine `fwd` to `bwd`.
- If `fwd` is `TypeVar` and `bwd` is any concrete type (including
  Union/Isect), refine `fwd` to `bwd`. This handles generic type
  parameters where multiple call sites constrain T to different
  types — the Union IS the inferred type.
- Refinement changes `fwd[i]`, which triggers forward requeueing.

**Union backward constraints do NOT trigger refinement for
DefaultInt/DefaultFloat.** If backward derives `Union(i32, string)`
for a DefaultInt location, refinement does not fire — the constraints
are ambiguous. The DefaultInt will fall through to the default sweep
(`DefaultInt → u64`) at the end. However, Union backward constraints
DO trigger refinement for TypeVar, because the union is the correct
inferred type for a generic parameter.

This distinction is a consequence of the asymmetric semantics of
Default vs TypeVar: Defaults have a known fallback (u64/f64) and
represent literals that should resolve to a single type. TypeVars
represent genuinely unknown types that accumulate constraints.

**Phase 2 note**: This distinction will be eliminated by the
uniform bounded type variable representation (see "Uniform bounded
type variables" in Deferred Optimizations). Under that model, the
upper bound of DefaultInt constrains which backward types are
compatible, and the check becomes simply `lower ⊆ upper`.

"Compatible single concrete" (for DefaultInt/DefaultFloat) means:
the backward type is a single primitive type (not a Union, not
TypeVar, not DefaultInt/DefaultFloat), and the Default type is
compatible with it (`DefaultInt` with integer types,
`DefaultFloat` with float types).

#### Intra-label refinement (local definitions)

For locations **defined within the label** (e.g., `x = 42`), the
forward type is set by the forward transfer function, not inherited
from predecessors. These are not in `fwd[i]` (the entry state), so
cross-label refinement doesn't apply.

Instead, forward transfer functions themselves perform refinement at
definition sites: when a `Const` statement produces `DefaultInt`, the
forward transfer checks the backward constraint on that location
(from `bwd[i]`). If the backward constraint is a compatible single
concrete type, the forward transfer produces the refined type directly.

Example: `Const x = 42` in a label where `bwd` has `x: i32`:
- Forward transfer for Const: check bwd for dst location
- bwd has `x: i32`, compatible with DefaultInt → produce `x: i32`
- No separate refinement step needed; it's part of forward processing

This naturally handles the intra-label case: the backward constraint
is already available in `bwd[i]` when the forward pass runs.

#### Why inline refinement is necessary

This must be inline because refinement affects dynamic dispatch:
refining a variable from `TypeVar` to `i32` changes how `CallDyn`
resolves on it, which produces a concrete return type that flows
forward. Deferring refinement to a final phase would miss these
cascading effects.

#### Intra-label refinement example

Consider a label containing:
```
y = x.method()     // CallDyn on x
z.f = x            // Store x into field f of type i32
```

Assume `x: TypeVar` on entry and `z: MyClass` where `MyClass.f: i32`.

**Iteration 1**:
- Forward pass: `y = x.method()` — x is TypeVar, can't resolve, y = TypeVar.
  `z.f = x` — Store executes but doesn't constrain x in the forward direction.
- Backward pass (reverse order): `z.f = x` — field f is i32, so backward
  produces `x must be i32`. Then `y = x.method()` — backward for CallDyn
  sees x now has a backward constraint, may propagate further.
- Refinement: fwd has `x: TypeVar`, backward derived `x: i32` within this
  label → refine fwd entry `x = i32`. Enqueue this label Forward.

**Iteration 2**:
- Forward pass (re-run): `y = x.method()` — now x is i32, CallDyn resolves
  to `i32::method()`, y gets concrete return type. Push to successors.

Key point: the backward constraint from `z.f = x` is derived **within
the label's own backward pass**, not pushed from a successor. The
backward pass walks statements in reverse, accumulating constraints.
These locally-derived constraints participate in refinement at the same
label, triggering a forward re-run that resolves previously-stuck
dynamic dispatch.

### Why this is cleaner

1. **No `is_fixed`**: typetest narrowing is a direct push of a tighter
   type into the true successor's `fwd`. The forward and backward
   channels don't interfere.

2. **No conflated merge blocks**: forward merge is pure `merge_type`
   and backward merge is pure `merge_type` — same function, different
   data, different direction.

3. **No "build entry env" phase**: the entry IS the stored state,
   accumulated by predecessors/successors pushing into it.

4. **Simpler convergence**: both forward and backward are monotone
   (union/widen). No priority logic that could break monotonicity.

5. **Trivial change detection**: `merge_type` returns a bool. No
   whole-environment comparison needed.

### Convergence and termination

The system has three interacting operations: forward merge (widen),
backward merge (widen), and refinement (narrow). Forward and backward
are individually monotone. Refinement is not monotone in isolation, but
the combined system converges because of a key property of `merge_type`:

**Absorption property**: `merge_type` absorbs unresolved types into
concrete types:
- `merge_type(i32, DefaultInt)` → no change (i32 stays)
- `merge_type(i32, TypeVar)` → no change (i32 stays)
- `merge_type(Union(i32, string), DefaultInt)` → DefaultInt replaced
  by i32 in the union (no new Default introduced)

This means: **once a location is refined from DefaultInt/TypeVar to a
concrete type, `merge_type` will never reintroduce DefaultInt/TypeVar
at that location.** A subsequent merge from another path that has
DefaultInt will be absorbed by the existing concrete type.

**Proof of termination**:
1. Forward and backward envs grow monotonically (merge_type only widens
   or stays the same).
2. The type lattice has finite height (bounded by the number of types
   in the program). Forward/backward reach fixpoint in finite steps.
3. Refinement fires at most once per (label, location) pair. After
   refinement sets `fwd[i][loc]` to a concrete type, merge_type's
   absorption property prevents any subsequent merge from reverting it
   to DefaultInt/TypeVar. So the refinement condition
   (fwd is Default/TypeVar AND bwd is concrete) cannot be re-triggered
   at that location.
4. Each refinement may trigger forward requeueing, but the forward
   re-processing can only widen further (monotone), potentially
   triggering more refinements downstream. The total number of
   refinement events is bounded by `labels × locations_per_label`.
5. Therefore the algorithm terminates.

**Safety net**: `MAX_ITERS_PER_LABEL = 200` catches bugs during
development. Hitting this limit indicates a convergence bug, not
expected behavior.

### Path-sensitive backward: compatibility definition

When pushing backward constraint `bwd_entry[loc]` to predecessor `p`,
and `p`'s terminator is a Cond that narrowed `loc`:
- Look up which edge (true/false) connects `p` to label `i`.
- The Cond's `trace_typetest` gives the tested type `T`.
- On the true edge: `loc` was narrowed to `T`.
  Compatible if `bwd_type <: T` (backward constraint is within the
  narrowed type).
- On the false edge: `loc` had `T` excluded.
  Compatible if `bwd_type` and `T` are disjoint (backward constraint
  doesn't require the excluded type).
- If incompatible, skip the push for that (predecessor, location) pair.

For non-Cond predecessors (Jump, unconditional): always push.

### Default literal fallback

After the solve loop converges and `finalize()` writes types to the AST,
any remaining unresolved types are resolved:
- `DefaultInt` → `u64`
- `DefaultFloat` → `f64`
- `TypeVar` → error (reported to user)

This sweep happens in the pass entry point after `GlobalInfer::run()`,
not inside finalize.

### Cross-function constraint flow

The solve loop's forward/backward push follows CFG edges within a
function. But type constraints also flow across function boundaries
in three cases: call sites, shape propagation, and lambda captures.

#### 1. Call sites

- **Forward (return → caller)**: When the callee's return type is
  resolved, it flows forward to the call site's result variable.
- **Backward (arg → param)**: When `f(x)` is called and x has type T,
  the callee's parameter must accept T. Push T as a backward constraint
  to the callee's entry label.

#### 2. Shape propagation

When a lambda is passed where a shape type is expected, the shape's
method signatures become constraints on the lambda's params and return
type. This is a backward constraint from the call site to the lambda.

#### 3. Lambda captures (bidirectional)

Lambdas capture variables from their enclosing scope. Captures require
**bidirectional** cross-function flow:

- **Forward (outer → lambda)**: The outer scope writes a captured
  variable (e.g., `x = 42`). The lambda reads it (e.g., `return x`).
  The outer scope's forward type for `x` must reach the lambda's `fwd`
  at labels where `x` is used, so the lambda can determine the return
  type.

- **Backward (outer → lambda)**: The outer scope has a backward
  constraint on a captured variable (e.g., `call expects_i32(x)` after
  the lambda). That constraint must reach the lambda's `bwd` at labels
  where `x` is written, so default literals assigned to `x` inside the
  lambda can be refined.

- **Forward (lambda → outer)**: The lambda writes a captured variable
  (e.g., `x = 42` inside the lambda). The forward type from the
  lambda's write must reach the outer scope's `fwd` at labels after the
  lambda call, so the outer scope knows x's updated type.

- **Backward (lambda → outer)**: The lambda reads a captured variable
  in a constrained context (e.g., `call expects_i32(x)` inside the
  lambda). That backward constraint must reach the outer scope's `bwd`
  at labels where `x` is defined, so the outer scope can refine `x`.

#### Mechanism: `push_cross()`

All cross-function flows use the same primitive:

```cpp
// Push a type constraint across function boundaries.
// target: the AST node identifying the destination (ParamDef, FieldDef,
//         Function, or captured variable's defining Ident)
// type: the constraint type
// dir: Forward or Backward
bool push_cross(const Node& target, const Node& type, Direction dir);
```

Internally, `push_cross` maps the target AST node to the label(s)
that reference it, and pushes the type into the appropriate `fwd` or
`bwd` env at those labels. If anything changed, those labels are
enqueued.

#### Implementation status

| Flow | Direction | Implemented | Function |
|------|-----------|-------------|----------|
| Call arg → callee param | Forward | Yes | `push_args_to_callee` |
| Shape → lambda param/return | Forward+Backward | Yes | `push_shape_to_lambda` |
| Call result ub → callee return | Backward | Yes | `push_return_constraint` |
| Callee param ub → caller arg | Backward | **No** | — |
| Lambda capture → outer scope | Bidirectional | **No** | — |

The missing "callee param ub → caller arg" flow is the primary
cause of remaining test failures (91% → target 92%+). Match value
literals captured as lambda parameters don't get backward-refined.
See ALGORITHM.md §8.3.

**Concrete mappings** (built during `GlobalInfer::build()`):
- `func_entry[Function] → label_index` — entry label of each function
- `func_returns[Function] → [label_indices]` — labels with Return terminators
- `param_label[ParamDef] → label_index` — which label the param is live in
- `capture_labels[FieldDef] → [(label_index, Location)]` — labels in
  both the outer scope and the lambda that reference a captured variable,
  with the local Location used at each label

For captures, `build()` identifies lambda-lifted classes with captured
fields (FieldDefs whose names correspond to outer scope variables) and
builds the bidirectional mapping.

**TypeVar unification**: TypeVar aliases within a function (from
Copy/Move of TypeVar↔concrete) are resolved by the standard
intra-label refinement (fwd has TypeVar, bwd has concrete → refine).
Cross-function TypeVars (lambda captures its enclosing scope's
TypeParam) are resolved by `push_cross`: the outer scope's resolved
TypeParam flows forward to the lambda, and the lambda's use-site
constraints flow backward to the outer scope.

#### When `push_cross` fires

`push_cross` is called **by transfer functions** during normal forward
and backward processing, not as a separate phase:

- **Call site forward transfer** (`infer_call_fwd`): after computing
  argument types, push each arg type as a backward constraint to the
  callee's corresponding ParamDef. After determining the callee's
  return type, push it forward to the call site's result.

- **Call site backward transfer** (`infer_call_bwd`): push backward
  constraints on the result variable to the callee's return labels.

- **Capture forward transfer** (`infer_field_ref`, `infer_load`): when
  reading a captured field, `push_cross` queries the outer scope's
  forward type for that capture and uses it. When writing a captured
  field (`infer_store`), `push_cross` pushes the written type forward
  to labels that read the capture.

- **Capture backward transfer**: when a captured variable is used in a
  constrained context (e.g., passed to a typed parameter), `push_cross`
  pushes the constraint backward to labels that define the capture.

Because transfer functions call `push_cross` during normal processing,
and `push_cross` enqueues affected labels, the worklist naturally
schedules cross-function work. No special phase or ordering is needed.

For nested lambdas (lambda1 captures x from outer, lambda2 captures x
from lambda1), `push_cross` chains automatically: lambda2's transfer
pushes to lambda1's capture labels, lambda1's transfer pushes to
outer's labels. Each push enqueues the target, so the worklist handles
depth naturally.

This mechanism must be part of the skeleton (Iteration 0) because
it is structural — the solve loop, transfer functions, and refinement
all depend on cross-function constraints flowing correctly. Deferring
it would require fundamentally different plumbing.

### Core structs

#### CFG (immutable after construction)

```cpp
struct CFG {
    struct LabelInfo {
        Node function;
        Node label;
    };

    std::vector<LabelInfo> labels;
    std::map<Node, std::pair<size_t, size_t>> func_label_range;
    std::vector<std::vector<size_t>> succ, pred;
    std::map<Node, std::map<std::string, size_t>> func_label_idx;
    std::map<Node, std::map<Location, Node>> func_def_stmts;

    void add_function(const Node& func);
    void finalize();
    size_t size() const;
    const std::map<Location, Node>& def_stmts_for(const Node& func) const;
};
```

#### Liveness (computed from CFG)

```cpp
struct Liveness {
    std::vector<std::set<Location>> defs, uses, live_in, live_out;
    void build(const CFG& cfg);
    bool is_live_in(size_t label, const Location& loc) const;
    bool is_defined(size_t label, const Location& loc) const;
};
```

#### LabelProcessor (per-label processing)

```cpp
struct LabelProcessor {
    TypeEnv& fwd_entry;  // Forward input (read)
    TypeEnv& bwd_exit;   // Backward input (read)
    Node function;
    GlobalInfer& gi;

    // Computed outputs (local, pushed to neighbors after processing)
    TypeEnv fwd_exit;     // Forward output → pushed into fwd[succ]
    TypeEnv bwd_entry;    // Backward output → pushed into bwd[pred]

    void forward_pass(const Node& body);
    void backward_pass(const Node& body);
};
```

#### GlobalInfer (orchestrator)

```cpp
struct GlobalInfer {
    Node top;
    std::vector<Node> functions;
    CFG cfg;
    Liveness liveness;

    // Per-label state: stored INPUTS in each direction
    std::vector<TypeEnv> fwd;   // fwd[i] = forward state at ENTRY of label i
    std::vector<TypeEnv> bwd;   // bwd[i] = backward state at EXIT of label i

    // Cross-function mappings (built during build())
    std::map<Node, size_t> func_entry;       // Function → entry label index
    std::map<Node, std::vector<size_t>> func_returns; // Function → return labels
    std::map<Node, size_t> param_label;      // ParamDef → label where param is live

    // Shared mutable state
    std::map<Location, Node> lookup_stmts;
    std::set<std::pair<Location, Location>> typevar_aliases;
    std::map<Location, std::pair<Location, size_t>> ref_to_tuple;

    // Direction-aware worklist
    std::deque<size_t> worklist;
    std::map<size_t, Direction> worklist_dir;

    void enqueue(size_t label, Direction dir);
    std::pair<size_t, Direction> dequeue();

    // Merge a type into a target env. Returns true if changed.
    bool push(TypeEnv& target, const Location& loc,
              const Node& type, Node call_node = {});

    // Cross-function constraint push.
    bool push_cross(const Node& target, const Node& type, Direction dir);

    void build(Node top);
    void solve();
    void finalize();
    void run(Node top);
};
```

### Solve loop pseudocode

```
dequeue label i with direction dir:

if dir is Forward or Both:
    // fwd[i] is already populated by predecessors
    fwd_exit = run forward_pass(body, fwd[i])

    // Push outputs to successors
    if terminator is Cond:
        trace typetest, compute narrowed/excluded types
        push narrowed into fwd[true_succ]
        push excluded into fwd[false_succ]
    elif terminator is Jump:
        push fwd_exit into fwd[target]
    elif terminator is Return:
        seed bwd[i] with declared return type (backward starts here)

    // For each push that changed fwd[succ], enqueue succ Forward

if dir is Backward or Both:
    // bwd[i] is already populated by successors
    bwd_entry = run backward_pass(body_reverse, bwd[i])

    // Push outputs to predecessors (Cond-aware)
    for each pred p:
        for each (loc, type) in bwd_entry:
            if p's terminator is Cond that narrowed loc on edge to i:
                check compatibility with narrowed type; skip if incompatible
            push into bwd[p]
            if changed: enqueue p Backward

refinement:
    for each loc in fwd[i]:
        if fwd is Default/TypeVar and bwd has compatible concrete:
            refine fwd[i][loc]
            re-run forward from this label (enqueue i Forward)
```

## Implementation Strategy: Skeleton-First

The implementation uses a clean-room rewrite built up through iterations.
Each iteration produces a compiling, testable `infer.cc`. The skeleton
starts with the new architectural scaffolding + stub transfer functions,
then progressively fills in real implementations.

**Why clean-room**: The AST-mutation communication pattern pervades every
transfer function. Incremental removal creates cascading failures because
each mutation depends on others. The skeleton approach lets us establish
the new architecture first (structs, worklist, pub/sub, finalize), then
port transfer functions one category at a time with clear validation.

**Termination guarantee**: The solve loop must include a hard iteration cap
(`worklist_iterations > cfg.size() * MAX_ITERS_PER_LABEL`) with a
diagnostic message if hit. Convergence relies on the type lattice being
finite-height and `merge_type` being monotone, but the safety net catches
bugs during development.

### Iteration 0: Skeleton

**Goal**: New file compiles, `infer()` entry point calls `GlobalInfer::run()`,
all transfer functions are stubs that do nothing. Tests will fail (types
unrefined) but the build succeeds and the pass runs without crashing.

Key deliverables:
- CFG, Liveness, LabelProcessor (stubbed), GlobalInfer structs
- Cross-function mappings: `func_entry`, `func_returns`, `param_label`
- `push()` and `push_cross()` primitives (structural, even if unused by stubs)
- `build()`: collect functions, build CFG, build liveness, build
  cross-function maps, seed `fwd` with param types
- `solve()`: worklist loop with forward/backward push and refinement
  (all producing no changes since transfer functions are stubs)
- `finalize()`: stub
- Pass entry point calling `GlobalInfer::run()`
- Static globals (`active_method_cache`, `active_infer_profile`,
  `active_infer_transfer_epoch`) moved to `GlobalInfer` instance vars
- Iteration cap: exceed → compilation error with diagnostic (not silent)
- 80-line architecture comment at top of file documenting bidirectional
  bounds model, push mechanism, convergence proof, and Cond-aware
  backward compatibility rule

**Validation**: `ninja install` succeeds. `dist/vc/vc build` runs without
crash on any test. Verify ring tests exist (`testsuite/v/ring_*`).

### Iteration 1: Solve loop

**Goal**: `build()`, `solve()` produce converged `fwd`/`bwd` envs.
Transfer functions still stubbed, so envs just propagate seeds.

Implement:
- Forward push: process label, push fwd_exit to successors
- Backward push: process label, push bwd_entry to predecessors
  (Cond-aware compatibility check)
- Cond terminator: push narrowed/excluded types to true/false successors
- Return terminator: seed `bwd[i]` from declared return type
- `push_cross()`: push arg types to callee params, return types to callers
- Refinement: Default/TypeVar fwd + concrete bwd → refine fwd
- Change-based requeueing (NOT direction-based)

**Validation**: Compiles. `solve()` terminates (no infinite loop).

### Iteration 2: Forward transfer functions

**Goal**: Port forward_pass and all forward transfer functions.

Port InferContext methods category by category, pulling in only the
helpers each category needs. Validate compilation after each group:
- `infer_const`, `infer_copy_move`, `infer_convert`
  - Helpers needed: `merge_type`, `replace_if_changed`, `refine_const_local`,
    `same_type_tree`, type predicates
- `infer_field_ref`, `infer_register_ref`, `infer_array_ref`
  - Helpers needed: `upsert_*` helpers
- `infer_new_fwd`, `infer_load`, `infer_store`
- `infer_call_fwd`, `infer_calldyn_fwd`, `infer_trycalldyn_fwd`
  - Helpers needed: `resolve_method_owner`, `resolve_class_method`,
    `build_class_subst`, `extract_constraints`, `apply_subst`,
    `infer_typeargs`, method cache types
- `infer_lookup`, `infer_binop`, `infer_unop`, `infer_nulop`
- `infer_when_fwd`, `infer_typecond`
- `forward_pass` dispatch

Each helper is added to the file only when first needed by a transfer
function being ported. No standalone "infrastructure helpers" iteration.

Apply mechanical replacements per the table above. Remove profiling:
- Strip `InferScopedTimer`, `InferStmtScope`, `note_infer_transfer_change()`
- Fix `if` statements left bodyless by profiling removal
- Remove `InferProcessScope` (replace with direct `active_method_cache` mgmt)

**Validation**: Ring 1 (`ring_basic`) and Ring 2 (`ring_call_typed`) tests
pass if created.

### Iteration 3a: Backward transfer functions

**Goal**: Port backward_pass and all backward transfer functions.

Port:
- `backward_pass` dispatch
- All `infer_*_bwd` methods
- `backward_refine_call`, `backward_refine_calldyn`
- `propagate_call_node`, `propagate_call_constraint`

Apply the same mechanical replacements and profiling removal as
Iteration 2. Backward transfer functions read `bwd` (backward
constraints from successors) and produce `bwd_entry` (constraints
to push to predecessors).

**Validation**: Ring 3-4 (lambda_one, lambda_multi) should show
improvement. Convergence testing: verify backward constraints
flow and refine default literals.

### Iteration 3b: Cross-function pub/sub

**Goal**: Implement pub/sub for cross-function constraint flow.

Port and adapt:
- `push_arg_types_to_params` → adapted to use `gi.publish_upper(ParamDef, type)`
- `propagate_shape_to_lambda` → adapted to use `gi.publish_lower(Function, type)`
- `infer_new_bwd` → adapted to use `gi.publish_upper(FieldDef, type)` (from
  writes) and `gi.publish_lower(FieldDef, type)` (from read constraints)
- `infer_when_fwd` → adapted to use `gi.publish_upper(When node, type)`

Implement full pub/sub:
- `publish_upper()`: merge type into slot upper bound via `merge_type`,
  enqueue all subscribers with `Direction::Both` if bound changed
- `publish_lower()`: intersect type into slot lower bound, enqueue all
  subscribers with `Direction::Both` if bound changed
- `subscribe()`: register label + read current value
- `read_slot()`: return current `SlotInfo` (upper/lower)

Forward transfer functions that read cross-function state (e.g.,
`infer_field_ref` reading field types, `infer_const` reading param types)
adapted to use `gi.read_slot()` + `gi.subscribe()`. Reads use the
slot's upper bound (what was written) for forward inference, and may
publish to the lower bound (what is expected) for backward propagation.

**Validation**: Ring 5-6 (lambda_topo, lambda_cycle) pass if created.
Full test suite should show significant improvement.

### Iteration 4a: Finalize (type writes)

**Goal**: `finalize()` writes converged types to AST.

Implement `GlobalInfer::finalize()`:
1. Dependency cascade per label
2. Const/TypeAssertion/NewArrayConst: write types from converged envs
3. TypeVar backprop over `typevar_aliases`
4. Params and fields: write from `exit_envs` + `slot_types`
5. Slot bounds → AST: write FieldDef, ParamDef types from slot bounds

**Validation**: Basic tests with explicit types should pass.
Forward-only tests (Ring 1-2) should produce correct AST output.

### Iteration 4b: Return types, TypeArgs, errors

**Goal**: Return type inference, TypeArgs re-derivation, error checking.

Implement:
1. Return type inference from Return terminator exit_envs (prefer per-label
   `exit_envs` which have typetest narrowing, NOT a global env)
2. TypeArgs: re-derive from converged envs (call `infer_typeargs` with
   final types, write to AST once)
3. Error checking: report unresolved TypeVars
4. DefaultInt/DefaultFloat sweep in entry point, after `run()`

**Deferred generics**: The old `infer()` entry point has a deferred loop
that re-processes functions with unresolved TypeVars. The global worklist
subsumes this — all functions are processed together, so TypeVar resolution
across function boundaries happens naturally during solve. If edge cases
remain, a second `solve()` pass can be added.

**Error reporting**: Structural errors (undefined methods, failed lookups)
are reported during solve when first encountered; they stop processing of
that label. Type mismatches and unresolved TypeVars are reported during
finalization, after the solver converges. Default literal ambiguity
(Union backward for DefaultInt) is silent — DefaultInt falls through to
the default sweep (u64).

**Validation**: Ring 7-10 (generics, default literals) pass if created.
Full test suite: all 1100 tests pass.

### Iteration 5: Profiling (optional)

**Goal**: Re-add profiling infrastructure adapted for global worklist.

- Global counters: total worklist iterations, labels processed, labels
  skipped, convergence iterations per label
- Per-transfer-function timers (same categories as old code)
- No per-function `InferProcessScope` — replaced by global stats

### Step-by-Step Transfer Function Port

When copying each transfer function from the old file:

**Mechanical replacements** (in all InferContext methods):
- `top` → `gi.top`
- `lookup_stmts` → `gi.lookup_stmts`
- `all_def_stmts` → `gi.cfg.def_stmts_for(function)` (scoped per-function)
- `typevar_aliases` → `gi.typevar_aliases`
- `ref_to_tuple` → `gi.ref_to_tuple`
- `changes.forward = true` → `forward_changed = true`
- `changes.backward = true` → `backward_changed = true`
- `changes = {}` → `forward_changed = false; backward_changed = false;`
- `return changes;` → `return;`

**Profiling removal** (in transfer functions and helpers):

Most profiling infrastructure is STRIPPED in the skeleton. It can be
re-added in Iteration 5. During transfer function porting (Iterations 2-3):
- Remove `InferScopedTimer` calls
- Remove `InferStmtScope` categorization
- Remove `note_infer_transfer_change()` calls
- When `note_infer_transfer_change()` is the sole body of an `if`, remove
  the entire `if`:
  ```cpp
  // Old:
  if (typevar_aliases.insert({dst, src}).second)
      note_infer_transfer_change();
  // New:
  typevar_aliases.insert({dst, src});
  ```
- Remove `InferProcessScope` (replace with direct `active_method_cache` mgmt)

**CRITICAL**: Do NOT use regex-based Python scripts to strip profiling code.
This was attempted and destroyed brace structure. Instead, manually identify
and remove profiling lines during the copy of each function.

### process_body and Direction

`process_body(body, dir)` runs forward_pass if `dir` is Forward or Both,
then backward_pass if `dir` is Backward or Both. When backward-only is
requested, `const_defs` and `def_stmts` must still be rebuilt from the body
before running `backward_pass`, since backward transfer functions depend on
them. This is a scan-only step (no env mutations), done at the start of
`process_body` regardless of direction.

## Solve Loop Details

See the Architecture section above. The solve loop pseudocode
captures the full algorithm. Key implementation notes:

**Termination bound**:
```cpp
constexpr size_t MAX_ITERS_PER_LABEL = 200;
if (wl_iters > cfg.size() * MAX_ITERS_PER_LABEL) {
  // MUST fail compilation — do NOT silently truncate.
  err(top, "Type inference did not converge");
  return;
}
```

**Convergence**: Both forward and backward analyses are monotone
(merge_type only widens). Refinement lowers Default/TypeVar to
concrete types, which is also monotone (each location refines at
most once per type). The type lattice has finite height, so the
fixpoint is guaranteed.

## Finalize Details

Finalize writes converged types to the AST:

1. **Dependency cascade**: `run_dependency_cascade()` for each label
2. **Const/TypeAssertion/NewArrayConst**: write types, remove assertions,
   compute tuple types
3. **TypeVar backprop**: fixpoint over `typevar_aliases`
4. **Params and fields**: write from `fwd` envs
5. **Return type inference**: from Return terminator `fwd` envs (prefer
   per-label `fwd[i]` which have typetest narrowing, NOT a global env)
6. **TypeArgs**: re-derive from converged envs (call `infer_typeargs` with
   final types, write to AST once). During solve, inferred TypeArgs are
   tracked in `gi.call_typeargs` (keyed by Call/CallDyn statement node).
   At finalization, these are written to the AST.
7. Error checking: report unresolved TypeVars. Errors (type mismatches,
   undefined methods, etc.) may be reported during solve if convenient,
   or during finalization. Solve primarily tracks types; finalize
   validates and reports remaining issues.
8. **Slot bounds → AST**: write FieldDef, ParamDef types from slot bounds
9. **DefaultInt/DefaultFloat sweep**: in the pass entry point, after
   `GlobalInfer::run()`

**Deferred generics**: The old `infer()` entry point has a deferred loop
that re-processes functions with unresolved TypeVars. The global worklist
subsumes this — all functions are processed together, so TypeVar resolution
across function boundaries happens naturally during solve. If edge cases
remain, a second `solve()` pass can be added.

## Test Rings (to be created)

Tests are organized into 10 rings of increasing difficulty:

| Ring | Test | Feature Tested |
|------|------|----------------|
| 1 | `ring_basic` | Single function, explicit types, forward propagation |
| 2 | `ring_call_typed` | Calling functions with fully-typed signatures |
| 3 | `ring_lambda_one` | Single lambda with explicit param/return types |
| 4 | `ring_lambda_multi` | Multiple independent lambdas |
| 5 | `ring_lambda_topo` | Lambdas with topological call dependencies |
| 6 | `ring_lambda_cycle` | Lambdas with cyclic data dependencies |
| 7 | `ring_generic_fwd` | TypeArg inference from argument types |
| 8 | `ring_generic_bwd` | TypeArg inference from output context |
| 9 | `ring_int_constrained` | Default int literals refined by context |
| 10 | `ring_int_default` | Unconstrained literals stay as u64 |

Each ring should be a self-contained test (no external deps, no
`use "_builtin"`, bitmask exit code pattern) under `testsuite/v/ring_*`.
Ring tests are created incrementally as each iteration is implemented,
not all upfront. Create golden files with `ninja update-dump` after the
relevant iteration passes.

## Validation Strategy

| Iteration | Validation |
|-----------|------------|
| 0 | `ninja install` succeeds, `vc build` runs without crash |
| 1 | `solve()` terminates, no infinite loop |
| 2 | Simple forward-only tests pass (Ring 1-2) |
| 3a | Backward refinement works (Ring 3-4) |
| 3b | Lambda cross-function tests pass (Ring 5-6) |
| 4a | Basic type writes to AST correct |
| 4b | Full test suite: all 1100 tests pass (Ring 7-10) |
| 5 | Profiling output matches expectations |

## Key Findings from Implementation Attempts

### Location Collision (Resolved)

`Location::operator==` compares by `view()` (string content), not source
position. `local$10` in function A and `local$10` in function B are the
**same** Location. A single global `TypeEnv` keyed by `Location` alone would
have collisions.

**Solution**: Per-label envs (already the case — each label has its own
`TypeEnv`). Cross-function flows use the publish/subscribe slot system
keyed by AST node identity, not Location. `func_def_stmts` in the CFG is
scoped per-function to avoid the same collision.

### AST-as-Communication-Channel

The existing handlers use the AST as a shared communication channel between
functions. During iteration they mutate:

1. **Const nodes** (`refine_const_local`): rewrites type token
2. **Lambda params** (`push_arg_types_to_params`): writes types into params
3. **Lambda fields** (`infer_field_ref`, `infer_new_bwd`): writes to FieldDef
4. **Lambda return types** (`propagate_shape_to_lambda`): writes return type
5. **TypeArgs** (`infer_typeargs`): writes to call site FuncName
6. **When types** (`infer_when_fwd`): writes to When node

The publish/subscribe slot system replaces mutations 2-4 and 6.
Mutation 1 is handled by the env (Const type is tracked per-location).
Mutation 5 (TypeArgs) requires special handling: during solve, TypeArgs
are tracked in the env as part of the call site's type state. At
finalization, `infer_typeargs` is called with converged types and writes
to the AST once. Within a function, later calls that depend on resolved
TypeArgs from earlier calls see the resolved types through the env
(the call's result type reflects the resolved TypeArgs), not by reading
the TypeArgs node itself.

### Why Incremental Removal Fails

Removing individual AST mutations one at a time creates cascading failures.
Each mutation depends on others. The clean-room rewrite approach (all
transfer functions adapted simultaneously) is the only viable path.

**Three prior incremental attempts have failed.** The AI agent does not
make large enough structural changes when working incrementally — it
patches around the existing architecture rather than restructuring.
This produces half-ported states where some transfer functions use the
new env while others still communicate through AST mutations, creating
subtle ordering bugs that are nearly impossible to debug. The clean-room
approach is not optional; it is the only path that has any chance of
succeeding.

### Convergence Bug (Solved)

The worklist loop must distinguish between "this direction was *requested*"
(`run_fwd`/`run_bwd` from the dequeued Direction) and "this direction
*produced changes*" (`body_fwd_changed`/`body_bwd_changed` from InferContext,
`forward_out_changed` from exit_env comparison). Only actual changes should
trigger requeueing of neighbors. Using the requested direction for
requeueing causes infinite cycling because Both always has `run_fwd=true`.

### Profiling Infrastructure

The old code has ~300 lines of profiling infrastructure
(`InferProfileStats`, `InferScopedTimer`, `InferStmtScope`,
`active_infer_profile`, etc.). All profiling is OMITTED from the skeleton
and Iterations 1-5. It is re-added in Iteration 6 once the algorithm is
validated.

Items to remove during porting:
- `InferProcessScope`: per-function concept gone
- `note_infer_transfer_change()` / `active_infer_transfer_epoch`: old skip
  optimization, replaced by direction-aware worklist
- `prior_transfer_epochs`: same
- All `InferScopedTimer` and `InferStmtScope` instances
- Profile counters (`labels_processed`, `labels_skipped`, etc.)

**WARNING**: profiling patterns often use multi-line constructs and
conditional statements with profiling as the only body. Removing the body
without fixing the `if` creates syntax errors. Handle manually per function.

### Old Code Structure Reference

The old `infer.cc.old` (6780 lines) has the canonical layout. The current
`infer.cc` (6364 lines) is a cleaned-up version with the same structure
but slightly different line numbers. Use `infer.cc.old` line numbers as
reference when porting:

| Lines (`.old`) | Content |
|----------------|---------|
| 1-12 | Includes, namespace |
| 13-21 | `is_lambda_function` fwd decl, `lambda_returns_omitted` |
| 23-111 | Dispatch tables |
| 112-397 | Type constructors and predicates |
| 398-660 | Profiling infrastructure (OMIT in skeleton) |
| 662-680 | `LocalTypeInfo`, `TypeEnv`, `PendingError` |
| 682-760 | `MethodInfo`, `MethodOwner`, `MethodLookupKey`, cache types |
| 761-808 | `typename_path_key`, `infer_type_name` |
| 809-904 | `resolve_method_owner`, `resolve_class_method` |
| 905-970 | `same_type_tree`, `same_type_env` |
| 972-977 | `note_infer_transfer_change` (profiling — OMIT) |
| 978-1087 | `replace_if_changed`, `refine_const_local`, `upsert_*` |
| 1088-1304 | `merge_type`, `merge_env` |
| 1305-1693 | `SrcIndex`, liveness helpers, `run_dependency_cascade` |
| 1694-2006 | `build_class_subst`, `extract_constraints`, `apply_subst`, method resolution |
| 2007-2094 | `propagate_shape_to_lambda` |
| 2095-2604 | Backward helpers |
| 2605-2771 | `infer_typeargs`, `push_arg_types_to_params` |
| 2772-3016 | `is_lambda_function`, `retarget_numeric_type`, misc helpers |
| 3017-3083 | `LabelChanges`, `InferStmtScope` (profiling — OMIT) |
| 3084-3286 | `InferContext` struct with inline methods |
| 3287-4578 | `InferContext` method implementations (transfer functions) |
| 4580-5657 | `process_function` (worklist + finalization — REPLACE) |
| 5658-5686 | `has_typevar`, `use_global_infer` |
| 5687-6672 | `infer_global` (existing prototype — REPLACE) |
| 6673-6780 | `PassDef infer()` entry point (REPLACE) |

### `_builtin` Functions

The `hello` test compiles ~685 labels globally (mostly from `_builtin`).
Functions like `find`, `each`, `reserve`, `pairs` in the string/array
builtins have loops (Cond/Jump cycles) that require multiple worklist
iterations to converge. The convergence bug manifested as these functions
cycling indefinitely.

## Acceptable Divergences

Slight differences from the existing inference are expected and acceptable:
- Different convergence paths due to different processing order
- The global worklist has no deferred re-processing limit structure
- Error messages may differ in location/wording
- Some edge cases may resolve differently (likely bugs in the old
  algorithm's ordering sensitivity)

The following are **NOT** acceptable divergences:
- Type narrowing changing for the same input (same program infers
  different types)
- Return types becoming less precise (e.g., concrete → Union → dyn)
- Runtime type errors appearing that didn't appear before
- Programs that compiled successfully now failing to compile

## Multi-Perspective Review Findings

The plan was evaluated through five lenses (theory, speed, security,
usability, conservative) with adversarial review. Key findings
incorporated below. The conservative recommendation for incremental
extension was rejected — three prior attempts at incremental rewriting
failed because the AI agent does not make large enough structural changes.
Clean-room is the only viable path.

### Theory-lens: Confirmed Sound

1. **Monotonicity**: SOUND. Forward/backward analyses are individually
   monotone. `merge_type` only widens or stays the same.

2. **Termination proof**: SOUND. Refinement fires at most once per
   (label, location) because `merge_type`'s absorption property prevents
   reintroduction of DefaultInt/TypeVar after refinement.

3. **Push vs pull fixpoint equivalence**: SOUND. Both models reach the
   same fixpoint (same inference result). Push has order-dependent
   intermediate states but converges to the same answer.

4. **Union backward constraints not refining**: SOUND. Correct design —
   ambiguous literals are genuine ambiguity, resolved by default sweep.

5. **Path-sensitive backward filtering**: SOUND but needs tightening.
   The false-edge case with Union backward constraints should use
   Option A (conservative skip of entire constraint) initially. More
   precise filtering (intersect Union with complement of excluded type)
   can be optimized later.

6. **Cross-function flow via push_cross**: SOUND on monotonicity.
   Lambda capture cycles don't cause non-termination because each
   `push_cross` is monotone (`merge_type`), the worklist re-processes
   affected labels, and refinement is bounded.

7. **Bidirectional refinement cascade**: NOT explicitly proven in the
   plan, but sound on analysis. The interaction between intra-label
   and cross-label refinement is subtle — document it in the
   architecture comment. The potential function
   Φ = (# of unrefined DefaultInt/TypeVar locations) decreases
   monotonically.

### Security-lens: Non-Negotiable Fixes

The following MUST be implemented:

1. **Iteration cap → compilation error**: When `MAX_ITERS_PER_LABEL`
   is exceeded, the compiler MUST fail with a non-zero exit code and
   diagnostic listing affected labels. Do NOT silently truncate
   inference — this hides bugs and produces wrong types.
   ```cpp
   if (wl_iters > cfg.size() * MAX_ITERS_PER_LABEL) {
     // Emit error with affected label list; return non-zero.
   }
   ```

2. **Static globals → instance variables**: Move `active_method_cache`,
   `active_infer_profile`, `active_infer_transfer_epoch` into the
   `GlobalInfer` struct. These are thread-unsafe data races. The
   refactoring is the natural time to fix this.

3. **Null checks on find_def()**: `find_def()` can return null nodes.
   In `push_cross()` and all `find_def()` call sites, guard against
   null with context-specific diagnostics rather than crashing.

4. **Memory monitoring**: Add instrumentation (gated behind env var)
   to track total labels, total env entries, and peak memory during
   the infer pass. Log a warning if any function exceeds 10K
   locations or 100K env entries.

### Usability-lens: Required Improvements

1. **Rename InferContext → LabelProcessor**: The old `InferContext`
   struct has different semantics (shared state + scratchpad). The new
   struct has pure I/O (fwd_entry/bwd_exit → fwd_exit/bwd_entry).
   Different name avoids confusion during review.

2. **80-line architecture comment**: Add at the top of the new
   `infer.cc` in Iteration 0. Summarise: bidirectional bounds model,
   push mechanism, Cond-aware backward compatibility, convergence
   proof, termination bound. Future maintainers MUST understand why
   `merge_type` checks exist and the refinement firing-once property
   before modifying the algorithm.

3. **Split Iteration 3**: The original plan bundles backward transfer
   functions + full pub/sub system + cross-function adaptations. Split:
   - **Iteration 3a**: Backward transfer functions (mechanical port)
   - **Iteration 3b**: Pub/sub system + cross-function adaptations

4. **Split Iteration 4**: The original plan bundles finalization +
   TypeArgs + error checking + DefaultInt sweep. Split:
   - **Iteration 4a**: Finalization (Const/TypeAssertion writes,
     dependency cascade, TypeVar backprop, params/fields writes)
   - **Iteration 4b**: Return type inference + TypeArgs re-derivation
     + error checking + DefaultInt/DefaultFloat sweep

5. **Error reporting policy**: Define upfront:
   - Structural errors (undefined methods) → reported during solve
     when first encountered; stop processing that label.
   - Type mismatches and unresolved TypeVars → reported during
     finalization, after the solver converges.
   - Default literal ambiguity (Union backward for DefaultInt) →
     silent; DefaultInt falls through to default sweep (u64).

6. **Transfer function dependency table**: Each transfer function
   group should list its helper dependencies so porting order is
   unambiguous. (Already partially done in Iteration 2 description;
   complete it for backward transfer functions too.)

7. **Explicit acceptable divergence criteria**: Error message wording
   and line numbers may differ. Type narrowing, return type precision,
   and successfully-compiling programs must NOT change. Validate ring
   tests produce identical exit codes.

### Speed-lens: Deferred Optimizations

These items are noted for Phase 2 (after correctness is validated):

1. **RPO-ordered worklist**: Compute Reverse Post-Order on CFG at
   startup. Process labels in RPO order for faster convergence
   (~2-5x fewer iterations on real CFGs).

2. **Bitset dedup**: Use two bitsets (`in_fwd_queue`, `in_bwd_queue`)
   instead of `std::map<size_t, Direction>` for O(1) membership test.

3. **Structural hashing for same_type_tree**: Hash type tree structure
   at construction time. Use hash equality as fast path; only recurse
   on hash mismatch. Reduces ~68K tree traversals to ~5K.

4. **Lazy cloning**: During solve, store type pointers + substitution
   deltas instead of cloned AST nodes. Defer cloning to finalization.
   Reduces allocations from ~274K to ~685 (one per label).

5. **Subtype memoization**: Cache `Subtype(ctx, A, B)` results in a
   small LRU cache (~100 entries). Expected hit rate > 60%.

6. **Global method cache key review**: Verify that the cache key
   correctly includes type instantiation info. If type parameters vary
   during inference for the same method, cache misses or stale results
   will occur. Current key uses owner + name + hand + arity but NOT
   TypeArgs — this may need augmentation.

7. **AngelicSubtype for TypeVar** (PARTIALLY IMPLEMENTED):
   `AngelicSubtype(T)` is now implemented for DefaultInt/DefaultFloat.
   Extending to TypeVar (replacing TypeVar with `Angelic(any)`) would
   unify all three concepts into one mechanism:

   | Current      | AngelicSubtype form      | Default |
   |--------------|--------------------------|---------|
   | DefaultInt   | Angelic(DefaultInt)       | u64     |
   | DefaultFloat | Angelic(DefaultFloat)     | f64     |
   | TypeVar      | Angelic(any) — TODO       | error   |

   The merge/refinement is uniform:
   - `merge_type`: Angelic is opaque (structural equality). Concrete
     wins over Angelic at forward joins.
   - Refinement: when `fwd` is Angelic(T) and `bwd` is concrete S
     admitted by T, narrow to S.
   - Finalization: unresolved Angelic → default or error.

   The key design distinction from the earlier BoundedVar proposal:
   - `AngelicSubtype` is a SINGLE child node (the upper bound T),
     not a pair of upper/lower bounds
   - It is "angelic" — context (backward) CHOOSES the subtype,
     rather than "demonic" Union where the value COULD BE any member
   - Narrowing happens ONLY in refinement, never in merge_type
   - This prevents forward-backward oscillation that caused
     convergence failures with the BoundedVar approach

### Adversarial Review: Resolved Findings

| Finding | Resolution |
|---------|-----------|
| `process_function()` reuse infeasible for finalization | Correct — plan already specifies ground-up `finalize()`. No reuse of `process_function()`. |
| Path-sensitive backward is core, not deferrable | Agreed — it is part of the core algorithm (Iteration 1), not a Phase 2 optimization. |
| Forward transfer functions should NOT read bwd | Agreed for cross-label. Intra-label refinement (Const checking bwd) is part of forward transfer by design. |
| push_cross() needs concrete pseudocode | Addressed in the "When push_cross fires" section. Implementation follows directly. |
| 1100 tests per iteration unrealistic for stubs | Correct — Iterations 0-1 will fail most tests. Reframed: "each iteration targets specific test tiers." |
| Ring tests needed in Iteration 0 | Ring tests already exist (10 tests under `testsuite/v/ring_*`). Validate they pass progressively. |
| Error handling model missing | Addressed in usability section above (error reporting policy). |
| Profiling removal manual risk | Addressed by existing warning in plan. Template one function first, then replicate pattern. |


