# Forward Type Inference with Angelic Constraints

## 1. Overview

The infer pass determines types for untyped literals, lambda parameters,
generic type arguments, and function return types. It uses a single
forward abstract interpretation over all functions simultaneously,
with constraint variables (angelics) for undetermined types.

**Key principles:**
- One scheduling mechanism: the abstract interpreter's worklist
- No AST mutations during the solve — only env, constraint store, and side tables change
- Types are refined incrementally via constraints, never finalised early
- Finalize writes results back to the AST after the fixpoint converges

## 2. Type Domain

Each location in the env maps to a type from:

| Type | Meaning |
|------|---------|
| `TypeVar` | No information (⊥) |
| `Angelic(id, members)` | Undetermined — will refine to one of `members` |
| `Concrete` (TypeName, primitive) | Determined type |
| `Union(T₁,...,Tₙ)` | Demonic choice — value is one of these at runtime |

An angelic carries a `TypeVarId` linking it to a constraint variable
in the store. The `members` set is the current set of possibilities
(e.g., IntSet for integer literals). As constraints tighten the set,
the angelic refines. Singleton set = concrete type.

### 2.1 Angelic vs Demonic

- `Union(i32, string)` — **demonic**: different values arrive at runtime
- `Angelic(k, IntSet)` — **angelic**: the compiler chooses the type to satisfy all constraints

At control flow joins:
- Demonic values from different paths → `Union` (widen)
- The same angelic from both paths → keep the angelic (identity by `TypeVarId`)
- Angelic from one path, concrete from another → `Union(concrete, Angelic)`

### 2.2 Named Sets

| Set | Members | Default |
|-----|---------|---------|
| IntSet | i8, i16, i32, i64, u8, u16, u32, u64, isize, usize, ilong, ulong | u64 |
| FloatSet | f32, f64 | f64 |

## 3. Architecture

```
┌──────────────────────────────────────────────┐
│           Abstract Interpreter               │
│  ┌──────────┐  ┌──────────┐                  │
│  │ Worklist │  │ fwd[l]   │                  │
│  │ (RPO)    │  │ (join of │                  │
│  │          │  │  preds)  │                  │
│  └──────────┘  └──────────┘                  │
│        │              │                      │
│        ▼              ▼                      │
│  ┌─────────────────────────────────────────┐ │
│  │         Transfer Function               │ │
│  │  reads: env, constraint store,          │ │
│  │         side tables, AST                │ │
│  │  writes: env, constraint store,         │ │
│  │          side tables                    │ │
│  │  NEVER writes: AST                     │ │
│  │  output env → push_fwd to successors   │ │
│  └─────────────────────────────────────────┘ │
│        │                                     │
│        ▼                                     │
│  ┌─────────────────────────────────────────┐ │
│  │         Constraint Store                │ │
│  │  TypeVarId → member_set, upper_bounds   │ │
│  │  tighten: intersect, notify observers   │ │
│  │  observers → enqueue labels             │ │
│  │  stmt_var_ids: stmt → TypeVarId         │ │
│  │  stability_map: stmt → resolved type    │ │
│  └─────────────────────────────────────────┘ │
│        │                                     │
│        ▼                                     │
│  ┌─────────────────────────────────────────┐ │
│  │         Side Tables                     │ │
│  │  field_type_overrides: (class,name)→T   │ │
│  │  func_return_var: func → TypeVarId      │ │
│  └─────────────────────────────────────────┘ │
└──────────────────────────────────────────────┘
        │
        ▼ (after convergence)
┌──────────────────────────────────────────────┐
│      Finalize                                │
│  Reads from constraint store + side tables:  │
│  - Const: stmt_var_ids → member_set → type   │
│  - Params: field_type_overrides              │
│  - Returns: func_return_var → upper_bounds   │
│  - Sweep: AngelicSubtype → default (u64/f64) │
│  Writes to AST                               │
└──────────────────────────────────────────────┘
```

### 3.1 Solve State

During the solve, the only persistent dataflow state is `fwd[l]` —
one environment per label, holding the join of all predecessor outputs.
The transfer function runs on a clone of `fwd[l]` and pushes the
result to successors. No per-label output is retained.

All type decisions are recorded in the constraint store and side
tables. After convergence, finalize reads these to write the AST.

### 3.2 Single Scheduling Mechanism

All re-processing flows through the worklist:
- CFG successors: `push_fwd(succ, exit_env)` → enqueues if changed
- Constraint observers: `add_observer(var, label)` → `tighten` enqueues on set change
- Cross-function params: `push_param_type(func, loc, type)` → `push_fwd(entry, env)` → enqueues
- Cross-function returns: `func_return_var` constraint variable → observers enqueue callers

No parallel worklists, no recursive notification, no ad-hoc re-enqueue.

## 4. Constraint Store

Each constraint variable has:
- `member_set`: current set of possibilities (shrinks monotonically)
- `upper_bounds`: accumulated `'a <: T` constraints
- `observers`: labels to re-enqueue when `member_set` changes

### 4.1 Tightening

When `add_upper_bound(var, T)` is called:
1. Extract primitive tokens from `T`
2. Intersect `member_set` with the tokens
3. If the set shrank → notify observers (enqueue labels)

For non-primitive upper bounds (e.g., class types): if all upper bounds
agree structurally, the variable is considered refined to that type.

### 4.2 Stable TypeVarIds

`stmt_var_ids` maps each Const statement to its TypeVarId. On re-run,
the Const handler reuses the same TypeVarId rather than creating a
fresh variable. This ensures the angelic node always references the
same constraint variable across iterations, so constraints and
observers accumulated on previous runs are preserved.

The Const handler reads `member_set(id)` on every run. If singleton,
it produces a concrete type. Otherwise it produces an angelic with
the current (possibly tightened) member set. The type is never
cached or short-circuited — it always reflects the current constraint
state.

### 4.3 Angelic Upper Bounds

Upper bounds may contain angelics (e.g., from cross-function return
types). The constraint store stores them as-is. `infer_callee_return`
reads raw upper bounds and returns angelics to callers. When the
caller constrains the angelic (e.g., from a `new` field type), the
angelic's own constraint variable tightens, and its observers
cascade through the worklist.

## 5. Transfer Functions

### 5.1 Const (literals)

| Literal | Type |
|---------|------|
| Untyped int | `Angelic(k, IntSet)` — fresh `k`, observer on current label |
| Untyped float | `Angelic(k, FloatSet)` |
| Typed `i32 42` | `i32` (concrete) |
| `true`/`false` | `bool` |
| `none` | `none` |

On re-run: if `member_set(k)` is singleton → concrete type directly.
Otherwise create angelic with current (possibly tightened) member set.

### 5.2 Copy/Move

`fwd[dst] = fwd[src]`. If source is angelic and destination has a
concrete type, `constrain_type(src, dst_type)`.

### 5.3 Call

Navigate FuncName → find Function definition. Apply type substitution.
Merge return type into dst. If return type is TypeVar, fall back to
`infer_callee_return` (cross-function return via constraint variable).

Push arg types into callee params via `push_param_type` → `push_fwd`.

### 5.4 Lookup + CallDyn

Lookup resolves the method on the receiver type. CallDyn calls it.

**Concrete receiver**: resolve method, merge return type, constrain
angelic args from param types via `constrain_type`.

**Angelic receiver**: cross-product over all members. For each member,
resolve method, check arg compatibility. Collect valid return types.
If multiple → create angelic result with those possibilities.

**Reverse constraint**: if the CallDyn result is already concrete
(e.g., from a Return constraint), filter receiver members to those
whose method returns that type. Constrain args from the surviving
method's param types.

### 5.5 New / Stack

Merge the class type into dst. For each NewArg:
- If field type is concrete: `constrain_type(arg_value, field_type)`
- If field type has TypeVar/angelic AND arg is non-TypeVar: write
  `field_type_overrides[class, field_name] = arg_type` (side table,
  not AST). Re-enqueue the class's function labels.
- `refine_call_typeargs`: if the arg's generic TypeArgs differ from
  the field's (shape subtyping), update the Call's TypeArgs.

### 5.6 FieldRef

Check `field_type_overrides` side table first. If found, use that type.
Otherwise read from AST FieldDef. Wrap in `ref_type()` (FieldRef
produces a reference to the field).

### 5.7 Return Terminator

If declared return type is concrete: `constrain_type(returned_value, return_type)`.

Always: `tighten_func_return(func, returned_value_type)` — adds upper
bound to the function's return constraint variable.

### 5.8 TypeAssertion

If declared type contains TypeVar: merge (seed, doesn't overwrite).
If concrete: hard overwrite.

## 6. constrain_type

`constrain_type(value_type, expected_type)` decomposes constraints:

1. **Direct angelic**: `Angelic(k) <: T` → `add_upper_bound(k, T)`
2. **Union decomposition**: `Union(A₁,...,Aₙ) <: T` → `∀i. constrain(Aᵢ, T)`
3. **Generic decomposition**: `G[V₁,...,Vₙ] <: G[E₁,...,Eₙ]` → `∀i. constrain(Vᵢ, Eᵢ)`

No-op for concrete-to-concrete (no angelic to constrain).

## 7. Cross-Function Flow

### 7.1 Parameters

`push_param_type(func, param_loc, type)` calls `push_fwd(entry_label, {param_loc: type})`.
The worklist re-processes the callee with the refined param.

### 7.2 Return Types

Each function with TypeVar return gets a `func_return_var` constraint variable.
- Return labels: `tighten_func_return` adds upper bound on the variable
- Callers: `infer_callee_return` reads from the variable (member_set, upper_bounds)
  and registers as observer. When the variable tightens, callers re-enqueue.

### 7.3 Shape/Lambda

`push_shape_to_lambda`: when a lambda is passed where a shape is expected,
push param types from the shape into the lambda's apply function.

## 8. Join and Dataflow

### 8.1 Dataflow

Each label has one persistent environment:
- `fwd[l]`: the join of all predecessors' output environments

When a label is dequeued, the transfer function runs on a clone of
`fwd[l]`, producing an output env. This output is pushed to each
CFG successor via `push_fwd(succ, output)`, which joins into
`fwd[succ]` and enqueues `succ` if anything changed. The output
env is then discarded — it is not stored.

### 8.2 Join Rules

`join_type(existing, incoming)`:

| existing | incoming | result | rationale |
|----------|----------|--------|-----------|
| TypeVar | X | X | ⊥ absorbs |
| X | TypeVar | no change | ⊥ absorbs |
| Angelic(k) | Angelic(k) | no change | same variable, same type |
| Angelic(k₁) | Angelic(k₂) | Union(Angelic(k₁), Angelic(k₂)) | different variables |
| Angelic(k) | Concrete S | Union(S, Angelic(k)) | different values possible |
| Concrete S | Concrete S | no change | identity |
| Concrete S | Concrete T | Union(S, T) with subtype absorption | different types |

### 8.3 Angelic Join Semantics

Angelics are identified by `TypeVarId`. The TypeVarId is a reference
to a constraint variable in the store, not a type itself. Two
angelics with the same `k` are the same undetermined type — joining
them produces no change.

Two angelics with different `k` represent different undetermined types
(e.g., two different literals). At a join they form a Union — the
runtime value could be either. Each angelic refines independently
through its own constraint variable.

**Constraint store is global, not per-path.** When path A constrains
`k <: i32` and path B doesn't, the member set `{i32}` is visible on
both paths after the constraint fires. This is correct: angelic choice
is path-independent — the literal has ONE type that must satisfy all
use sites. The constraint `k <: i32` from path A applies globally.

### 8.4 Concrete Absorbs via Subtype

When `S₁ <: S₂`, `join(S₁, S₂) = S₂`. This prevents Unions from
growing unboundedly with redundant subtypes.

## 9. Convergence

**Monotonicity**: 
- `member_set` shrinks (finite, bounded by initial set size)
- `fwd[l]` grows via join (ascending)
- `field_type_overrides` refines (TypeVar → angelic → concrete)
- `func_return_var` upper bounds accumulate

**Stability**: `stmt_var_ids` ensures re-runs produce the same TypeVarId.
`check_stability` ensures resolved Consts stay resolved.

**Bound**: O(labels × max_iters_per_label). Safety cap prevents infinite loops.

## 10. Finalize

After the solve converges, finalize writes types to the AST.

**Primary sources** (constraint store + side tables):
- Const types: `stmt_var_ids[stmt]` → `member_set(id)` → singleton or default
- Lambda field types: `field_type_overrides[(class, name)]`
- Function return types: `func_return_var[func]` → `upper_bounds`
- Param types: `fwd[entry_label][param_loc]` (fwd is retained)
- Angelic sweep: TypeVarId → `member_set` → default

**Derived** (tuple/array element types): finalize re-runs the
transfer function once per label on the converged `fwd` state to
reconstruct exit environments for tuple element type computation.
This is a non-iterative pass — no worklist, no constraint updates.
A future simplification could track tuple element types in a
dedicated side table during the solve.
