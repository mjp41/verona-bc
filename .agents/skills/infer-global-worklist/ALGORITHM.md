# Bidirectional Type Inference with Angelic Constraints

## 1. Types

The type universe consists of:

- **Primitive types**: `i8`, `i16`, `i32`, `i64`, `u8`, ..., `usize`,
  `f32`, `f64`, `bool`, `none`, `string`
- **Compound types**: `Class[T₁,...,Tₙ]`, `ref[T]`, `cown[T]`,
  `Tuple(T₁,...,Tₙ)`
- **Union**: `Union(T₁,...,Tₙ)` — the value IS one of these types at
  runtime (demonic choice)
- **Intersection**: `Isect(T₁,...,Tₙ)` — the value satisfies ALL of
  these simultaneously
- **Angelic**: `Angelic(T)` — the value WILL BECOME some `S <: T`,
  determined by context at compile time (angelic choice)
- **Concrete**: a constraint meaning "must resolve to exactly one
  type, not a Union"
- **TypeVar**: no type information (bottom)


### 1.1 Angelic vs Demonic

- `Union(i32, string)` is **demonic**: at runtime, the value could be
  either type. The program cannot assume which.

- `Angelic(Concrete ∩ IntSet)` is **angelic**: the value is a literal
  whose type the compiler chooses at compile time to satisfy all
  constraints.

This distinction matters at control flow joins:
- Demonic (Union): different values may arrive → join widens
- Angelic: the SAME literal arrives from all paths → its type must
  satisfy all paths simultaneously → join tightens the bound


### 1.2 The Concrete Constraint

`Concrete` is not a runtime type — it is a **constraint** in the
inference lattice meaning "the resolved type must be a single
concrete type." It factors resolution behaviour into the bound
itself, eliminating the need for separate resolution modes.

The key identity:

```
DefaultInt  =  Angelic(Concrete ∩ IntSet)
DefaultFloat  =  Angelic(Concrete ∩ FloatSet)
TypeParam T  =  Angelic(Any)
```

| Kind | Bound | Concrete? | Meaning |
|------|-------|-----------|---------|
| Integer literal | `Concrete ∩ IntSet` | Yes | Must pick ONE integer |
| Float literal | `Concrete ∩ FloatSet` | Yes | Must pick ONE float |
| Type parameter | `Any` | No | Can resolve to Union |

`Concrete` propagates naturally through intersection:

```
Isect(Concrete ∩ IntSet, Union(i32, i64))
  = Concrete ∩ Isect(IntSet, {i32, i64})
  = Concrete ∩ {i32, i64}
```

The literal is constrained to `{i32, i64}` and must pick one.


### 1.3 Angelic Sets and Preference Orders

Named angelic sets with a **partial preference order**:

| Set name | Members | Top (default) |
|----------|---------|---------------|
| `IntSet` | `i8, i16, i32, i64, u8, u16, u32, u64, isize, usize, ilong, ulong` | `u64` |
| `FloatSet` | `f32, f64` | `f64` |

The partial order `≤` has a unique maximum (the default). The
order need not be total — incomparable candidates at resolution
time indicate ambiguity.

For `IntSet`, the partial order is:

```
         u64 (top/default)
        / | \
i8 ≤ i16 ≤ i32 ≤ i64    (signed chain)
u8 ≤ u16 ≤ u32 ──┘       (unsigned chain)
isize  usize  ilong  ulong  (platform-specific, incomparable
                             with fixed-width except all ≤ u64)
```

Signed and unsigned of the same width are incomparable (e.g.,
`i32` and `u32`). All types are `≤ u64`. Within a sign family,
wider is preferred (e.g., `i32 ≤ i64`).

For `FloatSet`: `f32 ≤ f64` (total order, f64 is top).


### 1.4 Resolution

At finalization, remaining Angelic types are resolved:

```
resolve(Angelic(T), ≤) =
  let candidates = members(T)      // strip Concrete from T
  if |candidates| = 0: error("contradictory constraints")
  if |candidates| = 1: return the_member
  if Concrete ∈ T:
    let maxima = {c ∈ candidates | ¬∃c' ∈ candidates. c < c'}
    if |maxima| = 1: return the_maximum
    error("ambiguous — incomparable candidates: " + maxima)
  else:
    return Union(candidates)
```

Examples:
- `Angelic(Concrete ∩ IntSet)` unconstrained → `u64` (top of order)
- `Angelic(Concrete ∩ {i32, i64})` → depends on partial order
- `Angelic(Concrete ∩ {i32, usize})` → ambiguity if incomparable
- `Angelic(Any)` with ub = `{i32, string}` → `Union(i32, string)`
- `Angelic(Any)` unconstrained → error (unresolved type parameter)
- `Angelic(i32)` → `i32` (singleton, Concrete irrelevant)


## 2. The Two Lattices

Type inference operates on a **product lattice** with two
independent components per location:

| | Forward (lower bound) | Backward (upper bound) |
|---|---|---|
| **Tracks** | What the value IS | What the value must SATISFY |
| **Merge** | Join (⊔) — widen | Meet (⊓) — tighten |
| **Direction** | Ascending (grows) | Descending (shrinks) |
| **Applies to** | All locations | Angelic locations only |

### 2.1 Forward Lattice (Lower Bounds)

```
TypeVar  ⊑  Angelic(T)  ⊑  concrete S  ⊑  Union(S₁,...,Sₙ)  ⊑  Dyn
(⊥)                                                              (⊤)
```

Forward types grow monotonically via join. Within the Angelic
sub-lattice, a tighter bound carries more information:
`Angelic(i32) ⊒ Angelic({i32, i64}) ⊒ Angelic(IntSet)`.

### 2.2 Backward Lattice (Upper Bounds)

```
Dyn  ⊒  Union(T₁,...,Tₙ)  ⊒  concrete S  ⊒  ∅
(⊤)                                         (⊥)
```

Upper bounds shrink monotonically via meet (intersection). The
initial upper bound is `⊤` (no constraint). Each use-site
tightens.

The ordering: `⊤` (Dyn, least restrictive) is the top; `∅`
(contradictory, most restrictive) is the bottom. A smaller
set = more restrictive = lower in the lattice = more
information about what the type CANNOT be. Meet goes downward
(more restrictive). This is the standard "must" analysis
ordering.

Combined: for an Angelic location, the valid type set at any
point is `members(fwd) ∩ ub`. Refinement fires when this changes.

### 2.3 Why Backward is Only for Angelic

A **concrete** forward type is already determined. Upper bounds
cannot change it. Violations are caught by the downstream typecheck
pass. Computing upper bounds for concrete locations is wasted work.

An **Angelic** forward type is undetermined. Upper bounds narrow the
choice set. This is the only case where backward information
affects the inference result.


## 3. Forward Analysis

### 3.1 Forward Transfer Functions

| Statement | Forward rule |
|-----------|-------------|
| `x = literal(42)` | `fwd[x] = Angelic(Concrete ∩ IntSet)` |
| `x = literal(3.14)` | `fwd[x] = Angelic(Concrete ∩ FloatSet)` |
| `x = literal(true)` | `fwd[x] = bool` |
| `x = copy y` | `fwd[x] = fwd[y]` |
| `x = new C{...}` | `fwd[x] = C` |
| `x = call f(args)` | `fwd[x] = return_type(f, subst)` |
| `x = y.method(args)` | `fwd[x] = method_return_type(fwd[y], ...)` |
| `x = y + z` | `fwd[x] = fwd[y]` (LHS propagates) |
| `x = -y` | `fwd[x] = fwd[y]` (operand propagates) |
| `x = y == z` | `fwd[x] = bool` |
| TypeParam `T` | `fwd[T] = Angelic(Any)` |

### 3.1.1 Angelic Method Resolution (Cross-Product Rule)

When the receiver or arguments of a dynamic method call are Angelic,
the result type is the Angelic set of all valid return types across
the cross-product of member types:

```
recv : Angelic(T₁, ..., Tₙ)
arg  : Angelic(S₁, ..., Sₘ)

result = Angelic({ Rᵢⱼ | Tᵢ.method(Sⱼ) → Rᵢⱼ })
```

If either the receiver or an argument is concrete (not Angelic),
that dimension has exactly one value in the cross-product.

**Key properties**:
- The receiver is NOT refined — it stays Angelic. Only the result
  type is computed.
- If all valid combinations produce the same return type (e.g., all
  integer `==` returns `bool`), the result is that concrete type.
- If valid combinations produce different return types, the result
  is `Angelic({R₁, ..., Rₖ})` with the Concrete constraint
  inherited from the receiver.
- If no valid combination exists, the result is undefined (method
  not found for any member).

This rule also applies to binary operators like `+`, `==`, etc.,
which are desugared to method calls via Lookup + CallDyn.

### 3.2 Forward Join

**F1 — Bottom absorbs**: `⊔(TypeVar, X) = X`

**F2 — Angelic intersection**:
`⊔(Angelic(T₁), Angelic(T₂)) = Angelic(T₁ ∩ T₂)`

The same literal reaches the join from both paths. The bound
tightens. `Concrete` propagates through intersection:
`Angelic(Concrete ∩ A) ⊔ Angelic(Concrete ∩ B) = Angelic(Concrete ∩ (A ∩ B))`

Note: requires the same definition on both paths.

**F2a — Mixed Concrete fallback**:
`⊔(Angelic(Concrete ∩ T₁), Angelic(T₂)) = Union(Angelic(Concrete ∩ T₁), Angelic(T₂))`

When Concrete is present in one operand but not the other, the
two Angelics represent fundamentally different kinds of
undetermined type (literal vs type parameter). They cannot be
intersected because the resolution semantics differ. Fall back
to demonic Union. The sweep at finalization resolves each nested
Angelic independently. This case is rare (requires the same
location to carry literal-like and parameter-like types from
different paths).

**F3 — Concrete absorbs Angelic**:
`⊔(Angelic(T), concrete S) = S`

One path refined the literal → concrete wins.

**F4 — Concrete-Concrete union**:
`⊔(S₁, S₂) = Union(S₁, S₂)` when incomparable

**F5 — Subtype absorption**:
`⊔(S₁, S₂) = S₂` when `S₁ <: S₂`

### 3.3 Typetest Narrowing

At a Cond testing `x : T`:
- True successor: `fwd[x] = T`
- False successor: `fwd[x] = exclude(fwd[x], T)`


## 4. Backward Analysis (Upper Bounds)

Backward analysis computes upper bounds for Angelic locations only.
Each use-site contributes an upper bound. Multiple bounds combine
via meet (intersection).

### 4.1 Backward Transfer Functions

| Statement | Upper bound rule |
|-----------|-----------------|
| `call f(x)` where param: `T` | `ub[x] ⊓= T` |
| `store ref.field = x` where field: `T` | `ub[x] ⊓= T` |
| `return x` where func returns `T` | `ub[x] ⊓= T` |
| `y = copy x` | `ub[x] ⊓= ub[y]` |
| `new C{f = x}` where field `f: T` | `ub[x] ⊓= T` |

The `⊓=` operator is meet: `ub[x] := ub[x] ∩ incoming`.

**Angelic forward types as upper bound sources**: When an
argument's forward type is `Angelic(T)`, the upper bound
contribution is `members(T)` — the concrete member set, not
the `Angelic` wrapper itself. `Angelic` is a forward-lattice
concept and does not appear in the backward lattice. The
`Concrete` tag is stripped when computing `members()` and is
NOT transferred to the target's upper bound (e.g., a TypeParam
should not inherit `Concrete` from a literal argument).

### 4.2 Why Meet is Always Correct

Every constraint says "x's type must be ≤ T." Multiple constraints
combine as `T₁ ∩ T₂ ∩ ... ∩ Tₙ` — the type must satisfy ALL.

This holds for:
- Sequential uses (both execute → both must be satisfied)
- Different branches (angelic choice is path-independent)
- Cross-function flow (callee constrains caller)

There is no union-vs-intersection decision. Upper bound merge is
ALWAYS meet.

### 4.3 Skipping Non-Angelic Locations

If `fwd[x]` is concrete, skip the upper bound update. Performance
optimization, no semantic effect.

### 4.4 Path-Sensitive Backward Push

Through a Cond predecessor:
- True edge (x narrowed to T): push `ub[x] ∩ T`
- False edge: push unconditionally
- Non-Cond: always push


## 5. Refinement

For each Angelic location after processing a label:

```
candidates = members(fwd[x]) ∩ ub[x]
```

(Where `members` strips `Concrete` from the bound and returns the
set of concrete types.)

**R1 — Singleton**:
`|candidates| = 1 → fwd[x] := the_candidate`

**R2 — Tightened bound**:
`candidates ⊂ members(fwd[x]) → fwd[x] := Angelic(bound')`

Where `bound' = candidates`, preserving `Concrete` if present in
the original bound. Re-enqueue.

**R3 — No change**: `candidates = members(fwd[x])` → nothing.

**R4 — Empty**: `candidates = ∅` → contradictory. Leave `fwd[x]`
unchanged and set a contradiction flag on the location. At
finalization, resolution reports "contradictory constraints"
with the location info. This also handles `Angelic(∅)` produced
by F2 when forward bounds have empty intersection.

### 5.2 R1 Safety Invariant

**Theorem**: R1 never picks the wrong type from multiple valid
candidates. It fires only when `|candidates| = 1`. A later
backward constraint can only shrink `ub` further (meet is
monotone descending). Two cases:
- Singleton still in `ub` → R1 was correct.
- Singleton removed from `ub` → `ub = ∅` (contradiction) →
  program is ill-typed regardless of which type was chosen.

Consequence: premature R1 may produce a less informative error
message ("can't pass i32 to u32 parameter" instead of
"contradictory constraints") but never generates incorrect code.
The downstream typecheck pass catches all violations.

### 5.3 Finalization

After convergence, resolve remaining Angelics per §1.4.
Locations with contradiction flags report targeted errors.


## 6. Convergence

### 6.1 Monotonicity

- **Forward**: ascending. Angelic bounds tighten (F2). Concrete
  absorbs Angelic (F3). Both upward in info ordering.

- **Backward**: descending. Meet only tightens.

Both monotone on a finite lattice → fixpoint in finite steps.

### 6.2 Refinement Bound

- R2 fires at most `O(|members|)` per location (set shrinks).
- R1 fires at most once per location (concrete is absorbing).
- Total: `O(|labels| × |locations| × |members|)`.

### 6.3 Mode Switch

When R1 commits to concrete:
1. Forward: F3/F4/F5 apply. Angelic absorbed.
2. Backward: location skipped (§4.3).

Irreversible and monotone.

### 6.4 Termination

Total work ≤ `O(|L| × |V| × h)`.
Safety net: `MAX_ITERS_PER_LABEL`.


## 7. Worked Examples

### E1: Store into Union-typed field

```
x = 0
y.f = x        // f: Union(i32, i64)
```

| Step | fwd[x] | ub[x] | Action |
|------|--------|-------|--------|
| Fwd | Angelic(Concrete ∩ IntSet) | ⊤ | Literal |
| Bwd | — | Union(i32, i64) | ub ⊓= Union(i32, i64) |
| Refine | Angelic(Concrete ∩ {i32,i64}) | — | R2: tightened |
| Final | depends on ≤ | — | resolve with partial order |

### E2: Sequential stores, intersecting

```
x = 0
y.f = x        // f: Union(i32, i64)
z.f = x        // f: Union(i32, u32)
```

| Step | fwd[x] | ub[x] | Action |
|------|--------|-------|--------|
| Fwd | Angelic(Concrete ∩ IntSet) | ⊤ | |
| Bwd (z.f) | — | {i32, u32} | ub ⊓= Union(i32, u32) |
| Bwd (y.f) | — | {i32} | ub ⊓= Union(i32, i64) → {i32} |
| Refine | i32 | — | R1: singleton |

### E3: Branching

```
x = 0
if cond:
    y.f = x    // f: Union(i32, i64)
else:
    z.f = x    // f: Union(i32, u32)
```

| Step | fwd[x] | ub[x] | Action |
|------|--------|-------|--------|
| Fwd | Angelic(Concrete ∩ IntSet) | ⊤ | |
| Bwd (A) | — | ub ⊓= {i32, i64} | |
| Bwd (B) | — | ub ⊓= {i32, u32} | |
| At def | — | {i32} | Meet |
| Refine | i32 | — | R1 |

### E4: Disjoint constraints

```
x = 0
if cond:
    y.f = x    // f: i32
else:
    z.f = x    // f: string
```

| Step | fwd[x] | ub[x] | Action |
|------|--------|-------|--------|
| Fwd | Angelic(Concrete ∩ IntSet) | ⊤ | |
| Bwd | — | ∅ | Isect(i32, string) |
| Refine | unchanged | — | R4: empty |
| Final | u64 | — | Fallback; typecheck error |

### E5: Call with Union parameter

```
x = 0
f(x)          // param: Union(i32, usize)
```

| Step | fwd[x] | ub[x] | Action |
|------|--------|-------|--------|
| Fwd | Angelic(Concrete ∩ IntSet) | ⊤ | |
| Bwd | — | {i32, usize} | ub ⊓= Union(i32, usize) |
| Refine | Angelic(Concrete ∩ {i32,usize}) | — | R2 |
| Final | resolve({i32,usize}, ≤) | — | Depends on order |

### E6: Sequential calls, narrowing

```
x = 0
f(x)          // param: Union(i32, i64)
g(x)          // param: i32
```

| Step | fwd[x] | ub[x] | Action |
|------|--------|-------|--------|
| Fwd | Angelic(Concrete ∩ IntSet) | ⊤ | |
| Bwd (g) | — | i32 | |
| Bwd (f) | — | i32 | Isect(i32, {i32,i64}) = i32 |
| Refine | i32 | — | R1 |

### E7: Binop operand

```
x = 0
y = x + z      // z: i32
```

| Step | fwd[x] | ub[x] | Action |
|------|--------|-------|--------|
| Fwd | Angelic(Concrete ∩ IntSet) | ⊤ | |
| Bwd (+) | — | i32 | RHS constrains LHS |
| Refine | i32 | — | R1 |

### E8: Copy chain

```
x = 0
y = copy x
z.f = y        // f: i32
```

| Step | fwd | ub | Action |
|------|-----|-----|--------|
| Fwd | x,y: Angelic(Concrete ∩ IntSet) | ⊤ | |
| Bwd (store) | — | ub[y] ⊓= i32 | |
| Bwd (copy) | — | ub[x] ⊓= i32 | |
| Refine | x: i32, y: i32 | — | R1 |

### E9: Type argument inference

```
identity[T](x: T) -> T

y = identity(0)
z.f = y            // f: i32
```

| Step | fwd[T] | ub[T] | Action |
|------|--------|-------|--------|
| Start | Angelic(Any) | ⊤ | TypeParam — no Concrete |
| Call arg | — | ub ⊓= IntSet | Arg is Angelic → push members(fwd[arg]) |
| Return use | — | Isect(IntSet, i32) = i32 | z.f constrains return |
| Refine | i32 | — | R1 |

Since the bound is `Any` (no `Concrete`), if the constraints
had left multiple types, resolution would produce a Union —
the correct type for a generic parameter called polymorphically.

### E10: Polymorphic type parameter

```
wrap[T](x: T) -> wrapper[T]

a = wrap(42)       // site 1
b = wrap("hello")  // site 2
```

Each call site independently infers TypeArgs:
- Site 1: `T` constrained by arg `Angelic(Concrete ∩ IntSet)` → T = u64
- Site 2: `T` constrained by arg `string` → T = string

Type parameters are per-call-site (each call has its own
substitution context). The reify pass monomorphizes.

### E11: Branch with post-join constraint

```
x = 0
if cond:
    y.f = x    // f: Union(i32, i64)
else:
    z.f = x    // f: Union(u32, u64)
w.f = x        // f: i32
```

| Step | fwd[x] | ub[x] | Action |
|------|--------|-------|--------|
| Fwd | Angelic(Concrete ∩ IntSet) | ⊤ | |
| Bwd (w.f) | — | i32 | |
| Bwd (A, y.f) | — | Isect(i32, {i32,i64}) = i32 | |
| Bwd (B, z.f) | — | Isect(i32, {u32,u64}) = ∅ | |

Result: `ub[x] = ∅` — contradictory. The literal cannot satisfy
both `w.f: i32` and `z.f: Union(u32, u64)`.


## 8. Cross-Function Flow

Upper bounds flow across function boundaries:

- **Call f(x)**: `ub[x] ⊓= param_type(f)`
- **Shape/lambda**: shape method signatures become upper bounds
  on lambda parameters and return types.
- **Return**: call result's upper bound pushes to callee return.

All use the same meet (⊓) operator.


## 9. Summary

| Concept | Formulation |
|---------|-------------|
| Integer literal | `Angelic(Concrete ∩ IntSet)` |
| Float literal | `Angelic(Concrete ∩ FloatSet)` |
| Type parameter | `Angelic(Any)` |
| Forward merge | Join (⊔) — widen for concrete, tighten for Angelic |
| Backward merge | Meet (⊓) — always tighten (upper bounds) |
| Refinement | `members(fwd) ∩ ub` → singleton → commit |
| Resolution | Concrete → pick one (partial order); else → Union |
| Convergence | Product lattice, both components monotone |


## 10. Practical Considerations

### 10.1 Forward Join F2 and Incrementality

F2 tightens Angelic bounds at joins — non-standard for a join but
correct because Angelic is in a reversed sub-lattice (tighter =
more info = higher). Solutions for incremental merge:
1. Recompute from all predecessors on change
2. Track in info ordering (tightening is monotone up)
3. Defer intersection to refinement (loses precision)

### 10.2 Separate Definitions

The algorithm requires separate definitions to get separate
locations. In ANF, each `Const` has a unique `LocalId`. Two
`x = 0` in different branches are different locations. After the
branch, a Copy/phi creates a new location. F2's intersection
requires the same literal on both paths.

### 10.3 Intersection of Type Sets

`Isect(Union(T₁,...,Tₙ), Union(S₁,...,Sₘ))` = set intersection on
tokens for primitives, subtype checking for classes. Usually cheap.

### 10.4 Empty Upper Bound

`ub[x] = ∅` means contradictory constraints. Leave `fwd[x]`
unchanged. Resolution falls back to default. Typecheck reports
the error.

### 10.5 Concrete Propagation Through Copy

When `y = copy x` and `fwd[x] = Angelic(Concrete ∩ T)`:
- Forward: `fwd[y] = Angelic(Concrete ∩ T)` (Copy propagates)
- Backward: `ub[x] ⊓= ub[y]` (constraints propagate back)

The `Concrete` constraint follows the value through copies. If `y`
is later used where a Union would be acceptable, the `Concrete` in
the bound still requires a single-type resolution — correct,
because the original literal must have one type.

### 10.6 Type Parameters are Per-Call-Site

TypeParam inference (`Angelic(Any)`) happens per call site, not
per function declaration. Each `call f[?](args)` independently
infers TypeArgs from its arguments' types. The reify pass
monomorphizes, creating separate function copies per distinct
TypeArgs. The global inference pass sees TypeParams only through
their substitution into parameter/return types at each call site.
