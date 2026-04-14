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

## Current State (Updated April 14, 2026)

The clean-room rewrite is substantially complete. `infer.cc` is ~3900
lines (down from 6364). 976/1105 tests pass (88%). The architecture is:

- **Global worklist** with direction-aware scheduling (Forward/Backward/Both)
- **No AST mutations during solve** (except local TypeArgs write)
- **AngelicSubtype** node type for default literal inference
- **Extended refinement** covers both entry AND exit envs (no re-pass needed)
- **No post-convergence cascade** — all refinement is solve-time
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

This replaces the earlier "local refinement re-pass" (~200 lines)
with a ~10 line extension to the existing refinement loop.

**Backward Copy handler**: Only pushes backward constraints when the
expected type is concrete (not Angelic). This prevents Angelic types
from flowing backward through Copy chains as uninformative noise.
The backward constraint must come from a concrete source (e.g., a
Call param type, a return type, a Store field type).

**Finalization**: Unresolved `Angelic(DefaultInt)` → u64,
`Angelic(DefaultFloat)` → f64, `Angelic(any)` → error.

### Theory-Lens Analysis (April 14, 2026)

| Aspect | Status |
|--------|--------|
| Forward monotonicity | SOUND — Const handler preserves concrete types; Angelic never reintroduced |
| Absorption | SOUND — `merge(concrete, Angelic) → {}` by design |
| Convergence | SOUND — ~800 iterations for hello (685 labels), monotone lattice |
| Path-sensitive backward | SOUND — typetest filtering correct |
| Angelic nesting | GAP — no formal invariant preventing Angelic inside Union/Isect |
| Refinement completeness | CONCERN — depends on backward constraints reaching all Const definitions via Copy chain traversal in backward_pass |

### Remaining Failures (~35 real + golden mismatches)

| Category | Count | Description |
|----------|-------|-------------|
| Default literal refinement (i32↔u64) | ~20 | Backward constraint reaches variable but not the Const Location through Copy chain. Timing-dependent — backward constraint arrives after the label has already been processed. RPO ordering would likely fix. |
| Callback/generic TypeArgs | 5 | Generic callback TypeArgs not inferred — lambda type mismatch. |
| When param inference | 2 | Cown inner type not reaching lambda param at finalization. |
| Generic backward TypeArgs | 2 | Generic wrapper TypeArgs not re-inferred from backward constraint. |
| Return type inference | 2 | Lambda return type not resolved from shape/caller constraints. |
| Other (crash, error msg, FFI, lambda cycle) | 4 | Miscellaneous. |
| Golden file mismatches | ~60 | Tests compile correctly but intermediate dump output differs due to AngelicSubtype token in type nodes. Will resolve with golden file regeneration. |

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

### Next Steps

1. **RPO worklist ordering**: Process labels in reverse post-order for
   faster convergence. Would reduce iterations from ~800 to ~100 and
   should fix timing-dependent refinement failures where backward
   constraints arrive after the source label has already been processed.

2. **When param finalization**: Write cown inner types to lambda param
   AST nodes during finalization.

3. **Backward Call TypeArgs re-inference**: When a Call result has a
   backward constraint, re-infer TypeArgs from return_type vs
   backward_constraint and propagate to args.

4. **AngelicSubtype for TypeVar**: Replace TypeVar with
   Angelic(any) to unify the DefaultInt/DefaultFloat/TypeVar
   special cases into one mechanism.

5. **Angelic nesting invariant**: Add assertion that AngelicSubtype
   never appears inside Union/Isect/container types.

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
- `CallDyn`: env[dst] = resolved_method_return_type
- `Binop/Unop`: env[dst] = result_type
- etc.

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


