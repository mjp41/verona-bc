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

The code is in `vc/passes/infer.cc` (the original 6780-line file, fully
working). A backup exists at `vc/passes/infer.cc.old`. The refactoring
should produce a replacement `infer.cc` that passes all existing tests.

Ring tests (`testsuite/v/ring_*`) are in place and pass with the current code.
They provide incremental validation targets for the new implementation.

**Pre-existing test failure**: `union_method_infer` fails with the current
code (1 failure out of 1100 tests). This is NOT caused by the refactoring.

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
    std::map<Location, Node> all_def_stmts;

    void add_function(const Node& func);  // Add labels to global array
    void finalize();                       // Build edges and lookups
    size_t size() const;
};
```

**Key design choice**: CFG does NOT own `top` or `functions`. It does not
traverse the AST. `GlobalInfer::build()` collects functions via AST
traversal and calls `cfg.add_function(func)` for each, then `cfg.finalize()`.

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
  `gi.cfg.all_def_stmts`, `gi.typevar_aliases`, `gi.ref_to_tuple`.
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
    std::map<Node, Node> slot_types;
    std::map<Node, std::vector<size_t>> slot_subscribers;

    // Direction-aware worklist
    std::deque<size_t> worklist;
    std::map<size_t, Direction> worklist_dir;

    void enqueue(size_t label, Direction dir);
    std::pair<size_t, Direction> dequeue();
    Node read_slot(const Node& key) const;
    void publish(const Node& key, const Node& type);
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

**API**:
- `publish(node, type)`: set the type, enqueue all subscribers with
  `Direction::Both` if the type changed.
- `subscribe(node, label)`: register interest. Caller reads current value
  via `read_slot()` immediately.
- `read_slot(node)`: return current type or null.

**Late subscriptions**: `subscribe()` can be called during Phase 2 (solve).
This handles `CallDyn` where the receiver type isn't known until mid-solve.

## Implementation Steps

### Step 1: Write the new file structure

Write the complete `infer.cc` as a replacement (not incremental patching).
The file should contain, in order:

1. Dispatch tables (verbatim from old code, lines 23-111)
2. Forward declarations (`is_lambda_function`, `lambda_returns_omitted`, etc.)
3. Type constructors and predicates (verbatim, lines 112-397)
4. Core types (`LocalTypeInfo`, `TypeEnv`, `PendingError`)
5. Helper function declarations (`typename_path_key`, `infer_type_name`)
6. Forward declarations for method resolution
7. Comparison helpers (`same_type_tree`, `same_type_env`)
8. Env mutation helpers (`replace_if_changed`, `refine_const_local`,
   `upsert_lookup_stmt`, `upsert_ref_to_tuple`)
9. Type lattice (`merge_type`, `merge_env`)
10. Liveness helpers (`collect_label_defs/uses/kills`, `prune_bwd_env`)
11. Dependency cascade (`run_dependency_cascade` and helpers)
12. Method resolution (`resolve_method_owner`, `resolve_class_method`,
    `build_class_subst`, `extract_constraints`, `apply_subst`,
    `resolve_method`, `resolve_method_return_type`,
    `resolve_callable_method`, `propagate_shape_to_lambda`)
13. Backward helpers (`ScopeInfo`, `navigate_call`, `backward_refine_call`,
    `backward_refine_calldyn`, `trace_typetest`, `propagate_call_node`,
    `propagate_call_constraint`, `infer_typeargs`,
    `push_arg_types_to_params`, `retarget_numeric_type`,
    `extract_backward_primitive`, `recover_local_type_from_def`,
    `refine_function_return_consts`, `infer_tracked_tuple_type`)
14. `Direction` enum, `TupleTracking` struct
15. Forward declaration of `GlobalInfer`
16. `InferContext` struct (declarations only)
17. `CFG` struct
18. `Liveness` struct
19. `GlobalInfer` struct with `build()`, `solve()`, `finalize()`, `run()`
20. `InferContext` method implementations (inline methods + all transfer
    functions)
21. `PassDef infer()` entry point

### Step 2: Adapt transfer functions

When copying transfer functions from the old file, make these changes:

**Mechanical replacements** (in all InferContext methods):
- `top` → `gi.top`
- `lookup_stmts` → `gi.lookup_stmts`
- `all_def_stmts` → `gi.cfg.all_def_stmts`
- `typevar_aliases` → `gi.typevar_aliases`
- `ref_to_tuple` → `gi.ref_to_tuple`
- `changes.forward = true` → `forward_changed = true`
- `changes.backward = true` → `backward_changed = true`
- `changes = {}` → `forward_changed = false; backward_changed = false;`
- `return changes;` → `return;`

**Profiling adaptation** (in transfer functions and helpers):

Most profiling infrastructure can be KEPT. The `InferScopedTimer` calls in
transfer functions and `InferStmtScope` categorization are still useful.

What must be REMOVED or ADAPTED:
- `InferProcessScope`: manages per-function profiling lifetime — the concept
  of "per-function processing" goes away in the global worklist. Replace with
  a simpler scope that manages `active_method_cache`.
- `note_infer_transfer_change()` and `active_infer_transfer_epoch`: these
  implemented a skip optimization based on transfer epochs. They can be
  removed — the direction-aware worklist handles skip logic differently.
  When `note_infer_transfer_change()` is the sole body of an `if`, fix the
  `if`:
  ```cpp
  // Old:
  if (typevar_aliases.insert({dst, src}).second)
      note_infer_transfer_change();
  // New:
  typevar_aliases.insert({dst, src});
  ```
- `prior_transfer_epochs`: no longer needed (was per-label epoch tracking
  for the old skip optimization).
- Profile counters specific to the old per-function loop (`labels_processed`,
  `labels_skipped`, `worklist_iterations`, etc.) need updating for the global
  loop structure.

**CRITICAL**: Do NOT use regex-based Python scripts to strip profiling code.
This was attempted and destroyed brace structure, mixed code from different
locations, and produced unfixable corruption. Instead, manually identify and
remove profiling lines during the copy of each function, or copy functions
one at a time using `read_file`/`replace_string_in_file`.

### Step 3: Implement solve()

The solve loop was implemented and tested. The key insights:

**Entry env construction** (from old `process_function` lines 4740-4960):
1. Entry label (i == func_first): copy from exit_envs[func_first]
2. Other labels: merge predecessor exit_envs (with branch_exits overrides)
3. Merge bwd_envs from self and successors into entry
4. Recover forward metadata for locally-defined locations

**Skip logic** (CRITICAL for convergence):
```cpp
bool same_entry = prior_entry_valid[i] && same_type_env(prior_entry_envs[i], env);
if (same_entry && !run_bwd)
    continue;
// For backward-only: also check if successor bwd envs have new info
if (same_entry && run_bwd && !run_fwd) {
    bool bwd_incoming_changed = /* compare bwd_envs[succ] with bwd_envs[i] */;
    if (!bwd_incoming_changed) continue;
}
```

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

**Terminator handling** (adapted from old code lines 5010-5210):
- Return: backward-merge declared return type into env
- Raise: refine const literals against raise type
- Cond: create branch exits with typetest narrowing

### Step 4: Implement finalize()

Finalize writes converged types to the AST. Adapted from the old
`process_function` finalization code (lines 5280-5650):

1. **Dependency cascade**: `run_dependency_cascade()` for each label
2. **Const/TypeAssertion/NewArrayConst**: write types, remove assertions,
   compute tuple types
3. **TypeVar backprop**: fixpoint over `typevar_aliases`
4. **Params and fields**: write from exit_envs + slot_types
5. **Return type inference**: from Return terminator exit_envs
6. **Error checking**: report unresolved TypeVars
7. **Slot_types → AST**: write FieldDef, ParamDef types from slot_types
8. **DefaultInt/DefaultFloat sweep**: in the pass entry point, after
   `GlobalInfer::run()`

### Step 5: Test and validate

Run the full test suite (`ctest -j$(nproc)`). Expected:
- 1099/1100 pass (pre-existing `union_method_infer` failure)
- Ring tests pass: `ctest -R "^vbc/ring_"`

## Test Rings

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

## Key Findings from Implementation Attempts

### Location Collision

`Location::operator==` compares by `view()` (string content), not source
position. `local$10` in function A and `local$10` in function B are the
**same** Location. A single global `TypeEnv` keyed by `Location` alone would
have collisions.

**Solution**: Per-label envs (already the case — each label has its own
`TypeEnv`). Cross-function flows use the publish/subscribe slot system
keyed by AST node identity, not Location.

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
Mutation 5 is re-derived during finalization from converged envs.

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
`active_infer_profile`, etc.). Most of this can be KEPT — the timers and
statement categorization are still useful for profiling the new global
worklist.

What must change:
- `InferProcessScope`: remove (per-function concept gone)
- `note_infer_transfer_change()` / `active_infer_transfer_epoch`: remove
  (old skip optimization, replaced by direction-aware worklist)
- `prior_transfer_epochs`: remove (same)
- Profile counters: adapt for global loop (the per-function counters like
  `worklist_iterations`, `labels_processed` move from per-function to global)

**WARNING**: profiling patterns often use multi-line constructs:
```cpp
InferScopedTimer timer(
    (active_infer_profile != nullptr) ?
        &active_infer_profile->some_field :
        nullptr);
```
And conditional statements with profiling as the only body:
```cpp
if (condition)
    note_infer_transfer_change();
```
Removing the body without fixing the `if` creates syntax errors. Automated
regex stripping was attempted and destroyed the file structure. Use manual
function-by-function copying with targeted edits instead.

### Old Code Structure Reference

The old `infer.cc` (6780 lines) has this layout:

| Lines | Content |
|-------|---------|
| 1-12 | Includes, namespace |
| 13-21 | `is_lambda_function` fwd decl, `lambda_returns_omitted` |
| 23-111 | Dispatch tables |
| 112-397 | Type constructors and predicates |
| 398-660 | Profiling infrastructure (SKIP) |
| 662-680 | `LocalTypeInfo`, `TypeEnv`, `PendingError` |
| 682-760 | `MethodInfo`, `MethodOwner`, `MethodLookupKey`, cache types |
| 761-808 | `typename_path_key`, `infer_type_name` |
| 809-904 | `resolve_method_owner`, `resolve_class_method` |
| 905-970 | `same_type_tree`, `same_type_env` |
| 972-977 | `note_infer_transfer_change` (profiling — SKIP) |
| 978-1087 | `replace_if_changed`, `refine_const_local`, `upsert_*` |
| 1088-1304 | `merge_type`, `merge_env` |
| 1305-1693 | `SrcIndex`, liveness helpers, `run_dependency_cascade` |
| 1694-2006 | `build_class_subst`, `extract_constraints`, `apply_subst`, method resolution |
| 2007-2094 | `propagate_shape_to_lambda` |
| 2095-2604 | Backward helpers (`navigate_call`, `backward_refine_*`, `trace_typetest`, `propagate_call_*`) |
| 2605-2771 | `infer_typeargs`, `push_arg_types_to_params` |
| 2772-3016 | `is_lambda_function`, `retarget_numeric_type`, `extract_backward_primitive`, `recover_local_type_from_def`, `refine_function_return_consts`, `infer_tracked_tuple_type` |
| 3017-3083 | `LabelChanges`, `InferStmtScope` (profiling — SKIP) |
| 3084-3286 | `InferContext` struct with inline methods |
| 3287-4578 | `InferContext` method implementations (transfer functions) |
| 4580-5657 | `process_function` (worklist + finalization) |
| 5658-5686 | `has_typevar`, `use_global_infer` |
| 5687-6672 | `infer_global` (old global attempt — SKIP entirely) |
| 6673-6780 | `PassDef infer()` entry point |

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
