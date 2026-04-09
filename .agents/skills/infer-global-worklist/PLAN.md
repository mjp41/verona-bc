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

## Current State

The code is in `vc/passes/infer.cc` (6364 lines, fully working). A backup
exists at `vc/passes/infer.cc.old` (6780 lines, the pre-cleanup version).
The refactoring should produce a replacement `infer.cc` that passes all
existing tests.

An existing prototype `infer_global()` (~400 lines, gated behind
`VC_INFER_GLOBAL` env var) already implements the basic structure:
flat label collection, CFG construction, liveness, and an undirected
worklist loop with `process_function` fallback for finalization. The new
implementation replaces this prototype with the full architecture below.

All 1100 tests currently pass. Ring tests (`testsuite/v/ring_*`) are
planned as incremental validation targets but do not yet exist — they
should be created as part of the skeleton iteration.

## Architecture (Validated)

The architecture was designed, skeleton-tested, and validated through
interactive review. The final design has four core structs:

### CFG (immutable after construction)

```cpp
struct CFG {
    struct LabelInfo {
        Node function;  // Owning Function node
        Node label;     // Label AST node
    };

    std::vector<LabelInfo> labels;
    std::map<Node, std::pair<size_t, size_t>> func_label_range;
    std::vector<std::vector<size_t>> succ, pred;
    std::map<Node, std::map<std::string, size_t>> func_label_idx;
    // Per-function def stmt maps — scoped to avoid Location collisions.
    // Location::operator== compares by string content, so local$10 in
    // function A and local$10 in function B are the "same" Location.
    std::map<Node, std::map<Location, Node>> func_def_stmts;

    void add_function(const Node& func);  // Add labels to global array
    void finalize();                       // Build edges and lookups
    size_t size() const;
    const std::map<Location, Node>& def_stmts_for(const Node& func) const;
};
```

**Key design choice**: CFG does NOT own `top` or `functions`. It does not
traverse the AST. `GlobalInfer::build()` collects functions via AST
traversal and calls `cfg.add_function(func)` for each, then `cfg.finalize()`.

**Key design choice**: `func_def_stmts` is per-function, not a single global
map. `Location::operator==` compares by string content (`view()`), so
`local$10` in different functions would collide in a flat map. The accessor
`def_stmts_for(func)` returns the scoped map for a given function.

### Liveness (computed from CFG, queried during solve)

```cpp
struct Liveness {
    std::vector<std::set<Location>> defs, uses, kills, live_in, live_out;

    void build(const CFG& cfg);
    bool is_live_in(size_t label, const Location& loc) const;
    bool is_defined(size_t label, const Location& loc) const;
    bool is_killed(size_t label, const Location& loc) const;
};
```

**Key design choice**: Liveness is NOT part of CFG. CFG is pure graph topology.
Liveness is an analysis computed on the CFG and used by the solver.

### InferContext (per-label, created for each worklist item)

```cpp
struct InferContext {
    TypeEnv& env;        // Forward env (entry → exit for this label)
    TypeEnv& bwd;        // Backward expectations
    Node function;       // Owning function for this label
    GlobalInfer& gi;     // Access to all shared/global state

    // Per-invocation bookkeeping
    bool forward_changed = false;
    bool backward_changed = false;
    std::map<Location, Node> const_defs;
    std::map<Location, Node> def_stmts;
    std::map<Location, TupleTracking> tuple_locals;

    // Methods (declared, defined out-of-line after GlobalInfer)
    bool merge(...);
    bool refine_local_const(...);
    bool merge_bwd(...);
    void propagate_backward(...);
    void refine_and_propagate(...);
    PendingError pending_when_lookup_error(...);
    void forward_pass(const Node& body);
    void backward_pass(const Node& body);
    void process_body(const Node& body, Direction dir);
    // ... all infer_* transfer functions ...
};
```

**Key design choices**:
- InferContext holds ONLY per-label state plus a `GlobalInfer&` reference.
  No duplicated shared state (the old code passed `top`, `lookup_stmts`,
  `all_def_stmts`, `typevar_aliases`, `ref_to_tuple` as separate references).
- Transfer functions access shared state via `gi.top`, `gi.lookup_stmts`,
  `gi.cfg.def_stmts_for(function)`, `gi.typevar_aliases`, `gi.ref_to_tuple`.
- `forward_changed`/`backward_changed` replace the old `LabelChanges` struct.
  The worklist tracks direction, not the return value.
- `process_body` takes a `Direction dir` parameter and only runs the
  requested passes (forward_pass if Forward/Both, backward_pass if
  Backward/Both).

### GlobalInfer (orchestrates the 3-phase algorithm)

```cpp
struct GlobalInfer {
    Node top;
    std::vector<Node> functions;
    CFG cfg;

    // Per-label solver state
    std::vector<TypeEnv> exit_envs, bwd_envs;
    std::vector<TypeEnv> prior_entry_envs;
    std::vector<bool> prior_entry_valid;
    std::map<std::pair<size_t, size_t>, TypeEnv> branch_exits;

    Liveness liveness;

    // Shared mutable state
    std::map<Location, Node> lookup_stmts;
    std::set<std::pair<Location, Location>> typevar_aliases;
    std::map<Location, std::pair<Location, size_t>> ref_to_tuple;

    // Publish/subscribe slots (cross-function communication)
    std::map<Node, SlotInfo> slots;
    std::map<Node, std::vector<size_t>> slot_subscribers;

    // Per-call-site inferred TypeArgs (keyed by Call/CallDyn stmt node)
    std::map<Node, Node> call_typeargs;

    // Direction-aware worklist
    std::deque<size_t> worklist;
    std::map<size_t, Direction> worklist_dir;

    void enqueue(size_t label, Direction dir);
    std::pair<size_t, Direction> dequeue();
    SlotInfo read_slot(const Node& key) const;
    void publish_upper(const Node& key, const Node& type);
    void publish_lower(const Node& key, const Node& type);
    void subscribe(const Node& key, size_t label);

    void build(Node top);
    void solve();
    void finalize();
    void run(Node top);
};
```

### Direction-Aware Worklist

```cpp
enum class Direction { Forward, Backward, Both };
```

**Implementation**: `std::deque<size_t>` (FIFO queue) + `std::map<size_t,
Direction>` (direction tracking). `enqueue()` merges directions — if Forward
is pending and Backward arrives, it becomes Both. `dequeue()` pops from the
front and removes the direction entry.

**Seeding**: Only entry labels (first label of each function) are seeded,
with `Direction::Forward`. Forward propagation naturally reaches all
reachable labels. Backward starts when forward reaches a return label
that has seeded `bwd_envs`.

### Publish/Subscribe Slots (Cross-Function Communication)

Replaces the four typed side maps from the earlier design with a single
unified mechanism. Each slot is keyed by an AST node pointer:

| AST Node | Purpose | Publisher | Subscriber |
|-----------|---------|-----------|------------|
| `FieldDef` | Field type | `infer_new_bwd` | `infer_field_ref` |
| `ParamDef` | Lambda param type | `infer_call_fwd` | `infer_const` (params) |
| `Function` | Return type | backward refinement | `CallDyn` resolution |
| `When` | Cown type | `infer_when_fwd` | when-body params |

**Bidirectional bounds**: Each slot stores two types:
- **Upper bound** (forward/writes): union of all types published by
  writers (e.g., all `new MyClass{f = val}` sites for a FieldDef).
  Accumulated via `merge_type`.
- **Lower bound** (backward/constraints): intersection of all types
  expected by readers (e.g., all `obj.f` read sites for a FieldDef).
  Accumulated via type intersection.

Subscribers see both bounds through `read_slot()`, which returns a
`SlotInfo` with `upper` and `lower` fields.

```cpp
struct SlotInfo {
    Node upper;  // Union of writes (forward)
    Node lower;  // Intersection of read constraints (backward)
};
```

**API**:
- `publish_upper(node, type)`: merge type into upper bound via
  `merge_type`. Enqueue all subscribers with `Direction::Both` if
  the bound changed.
- `publish_lower(node, type)`: intersect type into lower bound.
  Enqueue all subscribers with `Direction::Both` if the bound changed.
- `subscribe(node, label)`: register interest. Caller reads current
  value via `read_slot()` immediately.
- `read_slot(node)`: return current `SlotInfo` (upper/lower, either
  may be null if no writes/constraints yet).

**Late subscriptions**: `subscribe()` can be called during Phase 2 (solve).
This handles `CallDyn` where the receiver type isn't known until mid-solve.

**Simplification note**: The upper/lower split may collapse to a single
type if practice shows they always converge. Keep both initially to
validate the design.

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

Write the complete `infer.cc` replacement with this structure:

1. Includes + namespace (verbatim from old code)
2. Forward declarations (`is_lambda_function`, `lambda_returns_omitted`)
3. Dispatch tables (verbatim)
4. Type constructors and predicates (verbatim)
5. Core types (`LocalTypeInfo`, `TypeEnv`, `PendingError`)
6. `Direction` enum, `TupleTracking` struct
7. Forward declaration of `GlobalInfer`
8. `CFG` struct (with `add_function`, `finalize`, `def_stmts_for`)
9. `Liveness` struct (with `build`)
10. `InferContext` struct — declarations only, all methods stubbed
11. `GlobalInfer` struct with `build()`, `solve()`, `finalize()`, `run()`
12. `InferContext` stub method bodies (empty forward/backward passes)
13. `PassDef infer()` entry point (calls `GlobalInfer::run()` unconditionally)

**Validation**: `ninja install` succeeds. Running `dist/vc/vc build
../testsuite/v/hello` produces a `.vbc` (may have wrong types, but no crash).

### Iteration 1: Infrastructure helpers

**Goal**: Port all stateless helper functions that don't touch InferContext.

Port verbatim (with scoped def_stmts adjustment):
- Comparison helpers (`same_type_tree`, `same_type_env`)
- Env mutation helpers (`replace_if_changed`, `refine_const_local`, etc.)
- Type lattice (`merge_type`, `merge_env`)
- Liveness helpers (`collect_label_defs/uses/kills`, `prune_bwd_env`)
- Method resolution (`resolve_method_owner`, `resolve_class_method`,
  `build_class_subst`, `extract_constraints`, `apply_subst`, etc.)
- Backward helpers (`navigate_call`, `backward_refine_*`, `trace_typetest`,
  `propagate_call_*`, `infer_typeargs`, `push_arg_types_to_params`, etc.)
- Dependency cascade (`run_dependency_cascade`)
- Misc (`is_lambda_function`, `retarget_numeric_type`,
  `extract_backward_primitive`, `recover_local_type_from_def`, etc.)

**Validation**: Compiles. Same behavior as Iteration 0 (stubs still active).

### Iteration 2: Solve loop

**Goal**: `build()`, `solve()` produce converged `exit_envs`/`bwd_envs`.
Transfer functions still stubbed, so envs just propagate seeds.

Implement:
- `GlobalInfer::build()`: collect functions, build CFG, build liveness,
  seed entry envs with param types, seed bwd_envs with return types
- `GlobalInfer::solve()`: worklist loop, entry env construction,
  requeueing (using change flags, NOT direction flags),
  terminator handling (Return/Raise/Cond with branch exits)
- Publish/subscribe stubs (`publish_upper`, `publish_lower`, `subscribe`,
  `read_slot`)

**Validation**: Compiles. `solve()` terminates (no infinite loop). Can
add temporary tracing to verify worklist processes expected number of labels.

### Iteration 3: Forward transfer functions

**Goal**: Port forward_pass and all forward transfer functions.

Port InferContext methods category by category, validating compilation after
each group:
- `infer_const`, `infer_copy_move`, `infer_convert`
- `infer_field_ref`, `infer_register_ref`, `infer_array_ref`
- `infer_new_fwd`, `infer_load`, `infer_store`
- `infer_call_fwd`, `infer_calldyn_fwd`, `infer_trycalldyn_fwd`
- `infer_lookup`, `infer_binop`, `infer_unop`, `infer_nulop`
- `infer_when_fwd`, `infer_typecond`
- `forward_pass` dispatch

Apply mechanical replacements per the table above. Remove profiling:
- Strip `InferScopedTimer`, `InferStmtScope`, `note_infer_transfer_change()`
- Fix `if` statements left bodyless by profiling removal
- Remove `InferProcessScope` (replace with direct `active_method_cache` mgmt)

**Validation**: Ring 1 (`ring_basic`) and Ring 2 (`ring_call_typed`) tests
pass if created.

### Iteration 4: Backward transfer functions + pub/sub

**Goal**: Port backward_pass, implement pub/sub for cross-function flows.

Port:
- `backward_pass` dispatch
- All `infer_*_bwd` methods
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

**Validation**: Ring 3-6 (lambda tests) pass if created. Full test suite
should show significant improvement.

### Iteration 5: Finalize + TypeArgs

**Goal**: `finalize()` writes converged types to AST. TypeArgs re-derived.

Implement `GlobalInfer::finalize()`:
1. Dependency cascade per label
2. Const/TypeAssertion/NewArrayConst: write types from converged envs
3. TypeVar backprop over `typevar_aliases`
4. Params and fields: write from `exit_envs` + `slot_types`
5. Return type inference from Return terminator exit_envs (prefer per-label
   `exit_envs` which have typetest narrowing, NOT a global env)
6. TypeArgs: re-derive from converged envs (call `infer_typeargs` with
   final types, write to AST once)
7. Error checking: report unresolved TypeVars
8. DefaultInt/DefaultFloat sweep in entry point, after `run()`

**Deferred generics**: The old `infer()` entry point has a deferred loop
that re-processes functions with unresolved TypeVars. The global worklist
subsumes this — all functions are processed together, so TypeVar resolution
across function boundaries happens naturally during solve. If edge cases
remain, a second `solve()` pass can be added.

**Error reporting**: Errors (type mismatches, undefined methods, etc.) may
be reported during solve if it simplifies the code. This may complicate
future parallelization but is acceptable for the initial implementation.

**Validation**: Ring 7-10 (generics, default literals) pass if created.
Full test suite: all 1100 tests pass.

### Iteration 6: Profiling (optional)

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
re-added in Iteration 6. During transfer function porting (Iterations 3-4):
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

The solve loop was validated through implementation. Key insights:

**Entry env construction**:
1. Entry label (i == func_first): copy from exit_envs[func_first]
2. Other labels: merge predecessor exit_envs (with branch_exits overrides)
3. Merge bwd_envs from self and successors into entry
4. Recover forward metadata for locally-defined locations

**No skip logic initially**: Every dequeued label is processed with
`Direction::Both`. Convergence is driven entirely by change-based
requeueing — if processing produces no changes, no neighbors are enqueued.
Skip optimizations (comparing entry env to prior) can be added later as
a performance improvement once correctness is validated.

**Requeueing** (CRITICAL — this was the convergence bug):
```cpp
// WRONG: uses direction flags for requeueing
if (forward_out_changed || run_fwd)  // run_fwd is always true for Both!
    for (auto s : cfg.succ[i]) enqueue(s, Forward);

// CORRECT: uses actual change flags from body processing
if (forward_out_changed || body_fwd_changed)
    for (auto s : cfg.succ[i]) enqueue(s, Forward);

bool bwd_changed = body_bwd_changed;  // NOT run_bwd!
// ... merge successor bwd envs, set bwd_changed if anything new ...
if (bwd_changed)
    for (auto p : cfg.pred[i]) enqueue(p, Backward);
```

The bug: `run_fwd` (from the dequeued Direction) was used for requeueing
decisions. When direction is Both, `run_fwd` is always true, causing
successors to always be enqueued even when nothing changed. This creates
infinite cycling. The fix: use `body_fwd_changed` (from InferContext) and
`forward_out_changed` (from exit_env comparison) instead.

**Terminator handling**:
- Return: backward-merge declared return type into env
- Raise: refine const literals against raise type
- Cond: create branch exits with typetest narrowing

**Termination bound**:
```cpp
constexpr size_t MAX_ITERS_PER_LABEL = 200;
if (wl_iters > cfg.size() * MAX_ITERS_PER_LABEL)
{
    std::cerr << "infer: global worklist exceeded iteration limit ("
              << wl_iters << " iterations, " << cfg.size() << " labels)\n";
    break;
}
```

## Finalize Details

Finalize writes converged types to the AST:

1. **Dependency cascade**: `run_dependency_cascade()` for each label
2. **Const/TypeAssertion/NewArrayConst**: write types, remove assertions,
   compute tuple types
3. **TypeVar backprop**: fixpoint over `typevar_aliases`
4. **Params and fields**: write from exit_envs + slot_types
5. **Return type inference**: from Return terminator exit_envs (prefer
   per-label `exit_envs` which have typetest narrowing, NOT a global env)
6. **TypeArgs**: re-derive from converged envs (call `infer_typeargs` with
   final types, write to AST once). During solve, inferred TypeArgs are
   tracked in `gi.call_typeargs` (keyed by Call/CallDyn statement node).
   At finalization, these are written to the AST.
7. Error checking: report unresolved TypeVars. Errors (type mismatches,\n   undefined methods, etc.) may be reported during solve if convenient,\n   or during finalization. Solve primarily tracks types; finalize\n   validates and reports remaining issues.\n8. **Slot bounds → AST**: write FieldDef, ParamDef types from slot bounds
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
Create golden files with `ninja update-dump` after the relevant iteration
passes.

## Validation Strategy

| Iteration | Validation |
|-----------|------------|
| 0 | `ninja install` succeeds, `vc build` runs without crash |
| 1 | Same as 0 (stubs still active) |
| 2 | `solve()` terminates, no infinite loop |
| 3 | Simple forward-only tests pass |
| 4 | Lambda cross-function tests pass |
| 5 | Full test suite: all 1100 tests pass |
| 6 | Profiling output matches expectations |

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

## Design Decisions (Resolved)

Decisions made during plan review, recorded for reference:

1. **Per-function def_stmts scoping**: `func_def_stmts` is per-function
   (mathematically equivalent to indexing by `(function, location)` pair).
   Prevents `Location` string-equality collisions across functions.

2. **Bidirectional pub/sub slots**: Each slot stores upper bound (union of
   writes via `merge_type`) and lower bound (intersection of read
   constraints). May simplify to a single type if practice shows they
   always converge.

3. **Error reporting during solve**: Acceptable if it simplifies code.
   May complicate future parallelization but not a concern for initial
   implementation.

4. **No skip optimization initially**: Every dequeued label is processed
   (always `Direction::Both`). Convergence relies solely on change-based
   requeueing. Skip optimizations (entry env comparison) deferred as a
   later performance investigation.

5. **Call TypeArgs tracking**: `std::map<Node, Node> call_typeargs` in
   `GlobalInfer`, keyed by Call/CallDyn statement node. Written to AST
   once during finalization.

6. **Worklist ordering**: FIFO (`std::deque`) vs ordered (`std::set`)
   is not initially important. FIFO chosen in the plan; measure later.

7. **Ring tests**: Created incrementally per iteration, not all upfront.

8. **Line budget**: Target similar or smaller than current 6364 lines.
