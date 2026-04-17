# Forward-Only Type Inference with Constraint Variables — Plan v2

## Motivation

The bidirectional (forward+backward) approach has fundamental composition
problems:
- `bwd_entry` vs `bwd_exit` are at different program points
- Intra-label backward flow requires a mini-analysis inside the transfer
  function
- The forward pass needs backward results, creating coordination problems
- The §4.3 bug (backward constraints not flowing through concrete Copy
  sources) is a symptom of tangled framework and domain concerns

## Core Idea

Replace the bidirectional analysis with a **single forward analysis** where
type variables carry constraints. "Backward" information flows naturally
when a type variable reaches a typed context and acquires a constraint.

## Type Variable Representation

Each unresolved type gets a fresh TypeVarId (a `size_t` index into the
constraint store). The ID is embedded directly in an `AngelicSubtype`
AST node:

```
Type << (AngelicSubtype ^ std::to_string(id) << Isect << Concrete << IntSet)
```

The `AngelicSubtype` node carries both:
- Its TypeVarId (in the node's location/value)
- Its constraint bound (as child nodes: Concrete, IntSet, etc.)

This keeps the environment as `Location → Node` (no separate
`Location → TypeVarId` mapping). When `forward_transfer` encounters
an `AngelicSubtype`, it reads the ID from the node, looks up the
constraint store, and checks if the variable is resolved. If resolved,
it replaces the Angelic with the concrete type. If not, it passes
through and registers the current label as an observer.

Concrete types (explicitly typed literals, class types, etc.) have no
TypeVarId — they are just regular AST `Type` nodes.

The `join` function works on Nodes directly:
- Two `AngelicSubtype` nodes with the same ID → identity, no change
- Two `AngelicSubtype` nodes with different IDs → `Union('a, 'b)`
- `AngelicSubtype` + concrete → `Union('a, concrete)`
- concrete + concrete → standard Union/subtype rules

## Constraint Store

A global (per-analysis) structure, owned by the domain. Each type
variable has an **upper bound** (what it must be a subtype of) and
a **lower bound** (what must be a subtype of it). The resolved type
must satisfy `lower <: resolved <: upper`. If `lower ⊄ upper` at
any point, that is a type error.

```cpp
struct ConstraintEntry {
    bool concrete;                     // Must resolve to one type
    std::vector<Token> member_set;     // Allowed primitives (IntSet, FloatSet, etc.)
    std::vector<Node> upper_bounds;    // 'a <: T (from use sites)
    std::vector<Node> lower_bounds;    // T <: 'a (from definition sites)
    std::optional<Node> resolved;      // Resolved concrete type
    std::vector<size_t> observers;     // Labels to re-enqueue on resolution
};

struct ConstraintStore {
    std::vector<ConstraintEntry> entries;

    TypeVarId fresh(bool concrete, std::vector<Token> members);
    void add_upper_bound(TypeVarId var, Node type);  // 'a <: T
    void add_lower_bound(TypeVarId var, Node type);  // T <: 'a
    void add_observer(TypeVarId var, size_t label);
    std::optional<Node> resolved(TypeVarId var);
};
```

Bounds are always concrete types. When a bound is added:
1. Intersect new upper bound with existing upper bounds
2. Union new lower bound with existing lower bounds
3. Check `lower <: upper` — if not, type error
4. If the intersection of `member_set` with `upper_bounds` is a
   singleton → resolve the variable
5. If resolved, notify all observers

## Copy/Move Identity

`Copy %2 = %1` → `env[%2] = clone(env[%1])`. If `env[%1]` is an
`AngelicSubtype` node, the clone carries the same TypeVarId. Both
locations now reference the same constraint entry. No merging needed.

## CFG Join — Split Approach

The join operation splits each type into two disjoint parts:

- **Angelic part**: all `AngelicSubtype` components, tracked by TypeVarId
- **Non-angelic part**: all concrete/TypeName components

These are handled independently:

### Angelic part: set union by TypeVarId

Angelic components are deduplicated by TypeVarId (integer equality),
not by Subtype. The Subtype checker never sees AngelicSubtype nodes.

When the same TypeVarId appears on both sides with different local
bounds (from typetest narrowing on different paths), the bounds are
**widened** by taking their union:

```
α_k[Concrete ∩ {i32}] ⊔ α_k[Concrete ∩ IntSet]
  = α_k[Concrete ∩ IntSet]      (union of member sets)
```

This is correct because the local bound represents "what this variable
could be on this path." At a join, we lose path information and must
take the union of possibilities. The `Concrete` flag is preserved if
either side has it.

### Non-angelic part: subtype-aware union

Concrete components use the standard union with subtype absorption:
- If incoming is a subtype of an existing member, skip (covered)
- If incoming subsumes an existing member, replace
- Otherwise add to the union

### Recombination

The result is the union of the angelic and non-angelic parts:

```
result = Angelic_result ∪ NonAngelic_result
```

If total components = 1, produce a bare type (not wrapped in Union).

### Join rules (derived from split)

```
⊥ ⊔ X = X                              (bottom absorbs)
X ⊔ X = X                              (structural equality)
α_i ⊔ α_j = Union(α_i, α_j)            (different IDs)
α_k[B1] ⊔ α_k[B2] = α_k[B1 ∪ B2]      (same ID, widen bounds)
α_k ⊔ C = Union(α_k, C)                (angelic + concrete)
C1 ⊔ C2 = Union/subtype rules           (concrete + concrete)
Union(A,B) ⊔ X = split, join parts, recombine
```

### Convergence

The ascending chain stabilizes because:
- Angelic IDs are **stable** (Const reuses TypeVarId per statement)
- The set of angelic IDs only grows (bounded by number of Const stmts)
- Local bounds only widen (monotone in the subset lattice)
- Concrete part follows existing lattice convergence
- Structural equality check after recombination detects no-change

### Constraint decomposition on Unions

When a type containing angelic variables reaches a typed context:

```
Return %x           →  constraint: Union(α_a, α_b, i32) <: i32
                    →  decompose:  α_a <: i32 AND α_b <: i32
                        (i32 <: i32 is trivially satisfied)
```

Each angelic variable gets a concrete upper bound. Resolution triggers
observer notification → defining label re-runs → stability map
produces concrete type → join stabilizes.

## Subtype Constraints

When a type (which may contain type variables) reaches a typed context,
the constraint is decomposed into upper and lower bounds:

```
Return %1           →  env[%1] <: i32
                    →  if env[%1] = 'a:  add_upper_bound('a, i32)
                    →  if env[%1] = Union('a, 'b):  add_upper_bound('a, i32)
                                                     add_upper_bound('b, i32)
                    →  if env[%1] = i32:  (concrete, check i32 <: i32)
```

For assignments / definitions:
```
Copy %2 = %1        →  env[%2] = clone(env[%1]) (same TypeVarId)
var x: i32 = %1     →  add_upper_bound('a, i32)
                         add_lower_bound(param_var, i32) if applicable
```

### Angelic-to-Angelic subtype check

When `Angelic(id1, ...) <: Angelic(id2, ...)` is required (e.g., at
a call site or store):
1. Union `lower_bounds(id1)` into `lower_bounds(id2)` — everything
   that flows into id1 also flows into id2
2. Intersect `upper_bounds(id2)` into `upper_bounds(id1)` — id1
   must also satisfy id2's constraints
3. Check `lower <: upper` for both variables after the update
4. If either variable can now resolve, resolve it + notify observers

This is eager — all values are concrete, so the checks are immediate.

### Angelic in Union subtype check

When `Foo | Angelic(id1, Top) <: Foo | Bar`:
- `Foo <: Foo | Bar` → satisfied
- `Angelic(id1, Top) <: Foo | Bar` → add_upper_bound(id1, Foo | Bar)
- The Angelic node in the env is tightened: `Angelic(id1, Foo | Bar)`

### Structural decomposition for generics

```
Call unwrap_i32(%2)  where param type = wrapper[i32]
                     and env[%2] type = wrapper['a]
→  match structure: wrapper['a] <: wrapper[i32]
→  decompose: add_upper_bound('a, i32) + add_lower_bound('a, i32)
              (invariant param → equality)
```

This replaces the old `backward_refine_call` / `extract_constraints`.

## Typetest Narrowing

At a `Cond` testing `x : T` where `env[x] = Angelic(id, bound)`:
- True successor: tighten the bound in the Angelic node locally.
  `env[x] = Angelic(id, bound ∩ T)`. Same TypeVarId, same constraint
  store entry. Only the bound carried in the node is narrowed.
  Example: `Angelic(id1, Concrete ∩ IntSet)` tested against `i32`
  → `Angelic(id1, Concrete ∩ {i32})` in the true branch env.
- False successor: tighten by subtracting: `env[x] = Angelic(id, bound \ T)`.
  Example: `Angelic(id1, Concrete ∩ IntSet)` false for `i32`
  → `Angelic(id1, Concrete ∩ IntSet\{i32})`. If subtraction is too
  complex, approximate by keeping the original bound (sound, imprecise).

The global constraint store is NOT modified by typetest. The TypeVarId
stays the same — constraints added at use sites on either branch go
to the same store entry via the shared ID. The narrowing is purely
symbolic: the Angelic node's bound tracks the local type information
while the store tracks the global constraints.

## Dynamic Dispatch

```
Lookup %m = %recv.method
CallDyn %result = %m(%recv, %args)
```

- If `resolved(env[recv])` is concrete → resolve method normally
- If unresolved → `env[%result] = fresh('b)`, add_observer('a, label)
- When `'a` resolves → label re-runs → method resolves → `'b` gets
  concrete type or constraints

Type variables are ephemeral per-run of a label. On re-run, the old
`'b` is orphaned and a new variable (or concrete type) is produced.

## Re-Run Stability

When a label re-runs (due to observer notification or forward join
change), the transfer function must be **stable**: if a Const's type
variable was already resolved in a previous iteration, the re-run
should emit the resolved concrete type directly, NOT create a fresh
variable.

The domain maintains a `statement → resolved_type` map (keyed by
the AST statement node). When `forward_transfer` processes a Const:
1. Check the stability map for this statement
2. If a resolved type exists and is compatible with the Const's
   member set → use the concrete type (no variable created)
3. Otherwise → create a fresh variable as normal

The stability map is populated when a variable resolves: the
resolution callback records `statement → resolved_type` for the
statement that created the variable.

This eliminates:
- Orphaned constraint entries (no fresh var → no garbage)
- Non-convergence cycles (resolved type is stable → no re-enqueue)
- Stale field_types_ references (New re-run writes concrete, not var)

## Closures / Fields

Lambda captures are represented as fields in the lambda-lifted class.
The constraint store maintains a field type variable map:

```cpp
// Domain-internal state:
std::map<Node, TypeVarId> field_types_;  // FieldDef → TypeVarId
```

### Flow through New (field writes)

When `New lambda$0 {x = %1}` executes and `env[%1] = Angelic('a)`:
- Look up (or create) a TypeVarId for field `lambda$0.x`
- If field has a declared type T (not TypeVar): add_upper_bound('a, T)
- If field type is TypeVar (unresolved): assign `field_types_[x] = 'a`
  (the field inherits the argument's type variable)

### Flow through FieldRef/Load (field reads)

When `FieldRef %ref = self.x` in the lambda:
- Look up `field_types_[x]` → gets 'a (or whatever was assigned)
- `env[%ref] = ref[Angelic('a)]`
- Register current label as observer of 'a

When `Load %val = *%ref`:
- Extract inner type from ref → Angelic('a)
- `env[%val] = Angelic('a)`
- Register observer of 'a

### Flow through Store (field writes inside lambda)

When `Store *%ref = %result` where `env[%result]` is concrete:
- Look up the field's TypeVarId from the ref
- add_upper_bound(field_var, result_type)

### Worked Example

```
// main:
Const %1 = 0                   →  env[%1] = Angelic('a, Concrete∩IntSet)
New %lam = lambda$0 {x=%1}     →  field_types_[x] = 'a
Call ... %lam.apply()           →  enqueue lambda$0::apply entry label
Return %1                      →  add_upper_bound('a, i32)
                                →  'a resolves to i32
                                →  observers notified

// lambda$0::apply(self):
FieldRef %xref = self.x        →  env[%xref] = ref[Angelic('a)]
                                →  observer on 'a
Load %xval = *%xref             →  env[%xval] = Angelic('a)
// ... x + 1 ...               →  'a used as i32 (resolved by now or on re-run)
```

When 'a resolves to i32, the lambda's labels are re-enqueued.
On re-run, FieldRef/Load produce `ref[i32]` / `i32` instead of
Angelic. The `x + 1` operation resolves correctly.

### Multiple Writers

If multiple sites write to the same field, their type variables
all get constrained by the field's declared type (if any). If the
field has no declared type, the field's TypeVarId is shared — all
readers see the same variable, and any write constrains it for
all readers.

## Observer Mechanism

When `forward_transfer` reads a type variable from the env:
- `add_observer(var, current_label)`

Observers fire ONLY on resolution (variable goes from unresolved to
resolved), NOT on every constraint addition. This bounds total
notifications to O(V) resolution events.

When a resolution occurs:
- All observer labels are re-enqueued Forward via `ai.enqueue(label)`
- The worklist's set property deduplicates

## Framework

Forward-only `AbstractInterpreter`:

```cpp
template <typename Domain>
struct AbstractInterpreter {
    using Env = typename Domain::Env;

    std::vector<Env> fwd_;       // forward entry
    std::vector<Env> fwd_exit_;  // forward exit

    // No bwd_ anything. No meet. No backward_transfer.

    bool push_fwd(size_t label, const Env& env);
    void enqueue(size_t label);
    const Env& get_fwd(size_t label) const;
    const Env& get_fwd_exit(size_t label) const;
    const CFG& cfg() const;

    void solve() {
        while (dequeue(label)) {
            fwd_exit_[label] = domain_.clone_env(fwd_[label]);
            domain_.forward_transfer(fwd_exit_[label], body, func, label, *this);
            // push to successors (with split_cond for Cond)
        }
    }
};
```

Domain concept:

```
struct Domain {
    Env empty_env();
    Env clone_env(const Env&);
    bool join(Env& target, const Env& incoming);

    void forward_transfer(Env& env, body, func, label_idx, ai);
    void split_cond(cond, body, fwd_exit, true_env, false_env);
    void seed(ai);
    void finalize(FinalizeContext<Env>& ctx);
};
```

## Resolution (in finalize)

After convergence, resolve remaining type variables:
- Concrete with member set → pick maximum under preference order
  (u64 for ints, f64 for floats)
- Non-Concrete → Union of all concrete upper bounds
- No constraints → error (for type params) or default (for literals)

Write resolved types to AST Const nodes, Param types, Return types.

## Convergence

- Forward join is monotone ascending
- Constraint addition is monotone (constraints only tighten)
- Type variable resolution is monotone (once resolved, stays resolved)
- Observer re-enqueue fires only on resolution: O(V) total events
- Transitive closure is bounded by the constraint DAG size
- Each label re-runs at most O(V) times (once per variable resolution
  that affects its env)
- Total: O(L × V × S) where L=labels, V=variables, S=statements

## Comparison with Bidirectional

| Aspect | Bidirectional | Forward + Constraints |
|--------|--------------|----------------------|
| Analyses | 2 (forward + backward) | 1 (forward only) |
| Framework | Complex (bwd_entry vs exit, self-loops, meet) | Simple (forward + push) |
| §4.3 bug | Structural: backward blocked by concrete vars | Doesn't exist: constraints flow through variables |
| Closures | Requires cross-function backward flow | Observer on shared type variables |
| Generics | backward_refine_call procedural matching | Structural decomposition at call sites |
| Worklist | Forward + Backward ordering | Forward only (RPO) |
| Typetest | Replace variable with concrete (breaks link) | Constrain variable (preserves link) |

## Implementation Phases

### Phase 1: Framework
- Remove all backward infrastructure
- Forward-only: fwd_, fwd_exit_, push_fwd, enqueue, solve
- Remove Direction enum (everything is forward)

### Phase 2: ConstraintStore
- ConstraintEntry struct with bounds, members, Concrete, observers
- add_upper_bound with resolution check
- Resolution logic + observer notification
- Stability map for re-run convergence

### Phase 3: Forward Transfer
- Const: create AngelicSubtype node with fresh TypeVarId + Concrete + member set
- Copy/Move: clone the Node (preserves TypeVarId if Angelic)
- Return: decompose constraint from return type
- Call: structural decomposition + add_upper_bound from param types
- Binop: constrain LHS and RHS to same type
- Store: add_upper_bound from field type
- Lookup/CallDyn: observer on receiver variable, fresh result variable

### Phase 4: split_cond
- Typetest true branch: tighten Angelic bound in-place (intersect with
  tested type). Same TypeVarId, no fresh variable, no store modification.
- Typetest false branch: tighten by subtraction (or approximate).

### Phase 5: Finalize
- Resolve all remaining variables
- Write types to AST
- Report errors for contradictory constraints
