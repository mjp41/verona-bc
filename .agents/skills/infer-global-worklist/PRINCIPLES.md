# Infer Pass Design Principles

## 1. One Mechanism

Every undetermined type is a **constraint variable** in the constraint
store. There are no side tables, no parallel tracking structures, no
special cases. The constraint store is the single source of truth for
all type decisions.

## 2. Forward Only

The analysis is forward-only. There is no backward pass. Constraints
flow forward through the env and accumulate in the constraint store
via `add_upper_bound`. The worklist processes labels in ascending RPO.

## 3. No AST Mutation During Solve

The solve reads the AST but never writes it. All type refinements are
recorded in the constraint store. Finalize is the only phase that
writes to the AST.

## 4. Single Scheduling Mechanism

All re-processing flows through the abstract interpreter's worklist:
- CFG successors: `push_fwd` → enqueues if joined env changed
- Constraint observers: `tighten` → enqueues observer labels
- Cross-function params: `push_fwd` to callee entry

No recursive notification, no parallel worklists, no ad-hoc re-enqueue.

## 5. Constraint Variables Everywhere

| What | Representation |
|------|----------------|
| Untyped integer literal | Constraint variable, member_set = IntSet |
| Untyped float literal | Constraint variable, member_set = FloatSet |
| Function return type (TypeVar) | Constraint variable, tightened by return labels |
| Lambda captured field (TypeVar) | Constraint variable, tightened by caller |
| Tuple element type | Constraint variable, tightened by stored value |
| TypeParam | Constraint variable, member_set = {} (non-concrete) |

All are TypeVarIds in the same store. All refine the same way:
`add_upper_bound` intersects the member set, observers re-enqueue.

## 6. Stable Identity

Each constraint variable has a stable identity (`stmt_var_ids` maps
AST nodes to TypeVarIds). Re-runs of a label reuse the same
TypeVarId, preserving accumulated constraints and observers.

## 7. Monotone Refinement

Member sets only shrink. Upper bounds only accumulate. The env only
grows via join. All monotone on a finite lattice. The fixpoint is
guaranteed.

## 8. Finalize Reads the Constraint Store

After convergence, finalize walks the AST. For each node that needs
a type, it looks up the constraint variable and reads the member set.
Singleton → write that type. Otherwise → write the default.

No re-running of transfer functions. No reconstructing environments.
The constraint store has everything.

## 9. constrain_type Decomposes Structurally

`constrain_type(value, expected)` is the only way constraints enter
the store from the transfer function. It decomposes:
- Angelic → `add_upper_bound`
- Union → decompose, constrain each component
- Generic `G[V] <: G[E]` → decompose, constrain each type argument

## 10. The Env is Transient

`fwd[l]` is a join cache — the accumulated join of predecessor
outputs at label `l`. It exists only to drive the worklist. It is
not a source of truth for final types. The constraint store is.
