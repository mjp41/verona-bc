# Global Worklist Inference Plan

## Goal

Replace the three-level inference structure (outer deferred loop → per-function
`process_function` → per-label worklist) with a single flat worklist over all
labels in the program. Lambda inter-function inference becomes ordinary worklist
edges instead of AST mutation.

## Key Constraint: Location Collision

`Location::operator==` compares by `view()` (string content), not source
position. So `local$10` in function A and `local$10` in function B are the
**same** Location. A single global `TypeEnv` keyed by `Location` alone would
have collisions.

**Solution**: Use `std::pair<Node, Location>` as the global key — the function
node pointer disambiguates. `Node` comparison is by identity (pointer), so
same-named locals in different functions are distinct.

```cpp
using GlobalLoc = std::pair<Node, Location>;  // (function, local_name)
using GlobalTypeEnv = std::map<GlobalLoc, LocalTypeInfo>;
```

## Architecture

### Data Structures

```cpp
struct GlobalLabel {
    size_t func_idx;   // which function owns this label
    Node function;     // Function AST node
    Node label;        // Label AST node
};

struct InferProgram {
    std::vector<GlobalLabel> labels;       // all labels, globally indexed
    std::vector<std::vector<size_t>> succ; // forward edges (Jump/Cond + lambda)
    std::vector<std::vector<size_t>> pred; // reverse edges

    // Per-label state (indexed by global label index)
    std::vector<TypeEnv> exit_envs;
    std::vector<TypeEnv> bwd_envs;

    // Shared across all labels
    std::map<Location, Node> lookup_stmts;
    std::map<Location, Node> all_def_stmts;

    Node top;
};
```

### Phases

**Phase 1: Build global label graph**
- Walk all functions, add labels to flat array
- Build intra-function edges (Jump/Cond)
- Scan for `New lambda$X` patterns → add inter-function edges
- Build liveness info across all labels

**Phase 2: Initialize**
- Seed entry labels with param types
- Seed Return labels with declared return types in bwd_envs
- Add all labels to worklist

**Phase 3: Single worklist fixpoint**
```
while (!worklist.empty()) {
    pop label i
    build entry env from pred exit_envs (using GlobalLoc keys)
    merge bwd_envs from self + successors
    forward_pass(body)
    backward_pass(body)
    finalize_tuples()
    update exit_envs[i]
    if forward changed → add succ to worklist
    if backward changed → add pred to worklist
}
```

**Phase 4: Finalization**
- Write converged types to AST (Const nodes, params, fields, return types)
- Dependency cascade per label
- Error checking

### InferContext Changes

`InferContext` gains a `Node current_function` member. Its `merge()`,
`merge_bwd()`, and env lookups translate `Location` → `GlobalLoc` at the
boundary. The `_fwd`/`_bwd` handlers continue to work with `Location` internally.

At lambda call boundaries, the context switches `current_function` to address
the callee's namespace.

## What Changes in Transfer Functions

### Lambda calls (infer_call_fwd / infer_calldyn_fwd)
- Don't call `push_arg_types_to_params` (no AST mutation)
- Write arg types into env at `(lambda_func, param_loc)` keys
- Add lambda entry label to worklist

### Lambda backward (infer_call_bwd / infer_calldyn_bwd)
- Don't call `backward_refine_call`/`backward_refine_calldyn` for lambdas
- Write expected return type into lambda's bwd env
- Add lambda return labels to worklist

### Field refinement (infer_field_ref)
- Don't mutate `FieldDef` type in AST during iteration
- Write field type into env at field's GlobalLoc

### Const refinement (refine_const_local)
- During iteration: env-only update (no AST mutation)
- During finalization: write converged type to AST Const node

### Shape-to-lambda (propagate_shape_to_lambda)
- Eliminated — shape context flows through env naturally via FieldRef/New/CallDyn

## What Gets Eliminated

| Current code | Replacement |
|---|---|
| `process_function` inner worklist | Absorbed into global worklist |
| `infer()` outer deferred loop | Gone — worklist handles it |
| `push_arg_types_to_params` | Env writes at call sites |
| `propagate_shape_to_lambda` | Flows through env naturally |
| `backward_refine_call` for lambdas | Backward worklist edges |
| `backward_refine_calldyn` for lambdas | Backward worklist edges |
| `refine_const_local` AST mutation | Deferred to finalization |
| `replace_if_changed` during iteration | Deferred to finalization |
| `note_infer_transfer_change` / epoch | Gone |
| `lambda_returns_omitted` global set | Gone |
| `deferred_param_errors` global map | Local to finalization |
| `has_typevar` / deferred tracking | Gone |

## What Stays Unchanged

- `InferContext` struct and all `_fwd`/`_bwd` handlers (logic unchanged)
- `merge_type`, `merge_env`, `same_type_tree` (unchanged)
- `resolve_method` / `resolve_method_return_type` (for external calls)
- `extract_constraints`, `apply_subst`, `infer_typeargs` (for generic calls)
- Typetest narrowing via `branch_exits`
- `run_dependency_cascade` (post-convergence)
- Finalization (Const rewriting, NewArrayConst, TypeVar back-prop, return type)

## Implementation Strategy

1. Build as parallel implementation behind `VC_INFER_GLOBAL` env var
2. Keep existing `process_function` + deferred loop as default
3. Test both paths against all 1060 tests
4. Once passing: remove old code

## Estimated Impact

- `process_function`: ~400 lines → ~50 lines (global worklist setup)
- `infer()` pass definition: ~100 lines → ~30 lines (no deferred loop)
- Cross-function propagation: ~300 lines → ~50 lines (worklist edges)
- `push_arg_types_to_params`, `propagate_shape_to_lambda`: ~150 lines → 0
- `refine_const_local`: simplified (env-only during iteration)
- Net: roughly **-700 lines** from ~5700

## Risks

1. Lambda call edge discovery: `New lambda$X` → `Lookup apply` → `CallDyn`
   must be reliably traced. Currently `is_lambda_function` does class-name
   matching; extend to edge building.
2. Multiple instantiations: same lambda class constructed in multiple labels →
   all become predecessors of lambda entry. Env merge handles correctly.
3. External call backward: calls to non-lambda functions read declared
   signature from AST. No worklist edges needed — these are monotone reads.
4. Non-lambda functions with declared signatures converge in one pass and never
   re-enter the worklist.
