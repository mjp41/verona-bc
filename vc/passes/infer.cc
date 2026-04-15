// ===== Bidirectional Type Inference with Angelic Constraints =====
//
// Architecture (ALGORITHM.md §2): Product Lattice with Forward Join +
// Backward Meet
//
// Type inference operates on a product lattice with two independent
// components per location:
//
//   fwd[x]  — Forward (lower bound): what the value IS
//   ub[x]   — Backward (upper bound): what the value must SATISFY
//
// Forward:  join (⊔) — ascending. Angelic bounds tighten (F2),
//           concrete absorbs Angelic (F3). Both upward in info order.
// Backward: meet (⊓) — descending. Meet only tightens. Always
//           intersection — every use-site constraint must be satisfied.
//
// Angelic types (ALGORITHM.md §1.1):
//   Angelic(Concrete ∩ IntSet) — integer literal, compiler chooses type
//   Angelic(Concrete ∩ FloatSet) — float literal, compiler chooses type
//   Angelic(Any) — type parameter, may resolve to Union
//
// The Concrete constraint (ALGORITHM.md §1.2):
//   Not a runtime type — a constraint meaning "must resolve to exactly
//   one concrete type." Present in literals (must pick ONE integer),
//   absent in type parameters (may resolve to Union).
//
// Refinement (ALGORITHM.md §5):
//   candidates = members(fwd[x]) ∩ ub[x]
//   R1: |candidates| = 1 → commit to concrete
//   R2: candidates ⊂ members(fwd) → tighten Angelic bound
//   R3: no change → skip
//   R4: candidates = ∅ → contradiction flag
//
// Resolution (ALGORITHM.md §1.4):
//   Concrete → pick max under partial ≤ preference order
//   Non-Concrete → Union(candidates)
//
// Forward Join Rules (ALGORITHM.md §3.2):
//   F1: ⊔(TypeVar, X) = X
//   F2: ⊔(Angelic(T₁), Angelic(T₂)) = Angelic(T₁ ∩ T₂)
//   F2a: ⊔(Angelic(Concrete∩T₁), Angelic(T₂)) = Union(...)
//   F3: ⊔(Angelic(T), concrete S) = S
//   F4: ⊔(S₁, S₂) = Union(S₁, S₂)
//   F5: ⊔(S₁, S₂) = S₂ when S₁ <: S₂
//
// Backward Meet (ALGORITHM.md §4):
//   Always intersection. Only applies to Angelic locations.
//
// Per-label state:
//   fwd[i]  = forward types at ENTRY of label i
//   ub[i]   = upper bound constraints at EXIT of label i
//   fwd_exit[i] = forward types at EXIT of label i (computed by
//                 transfer functions)
//
// Forward processing of label i:
//   1. Read fwd[i], run forward transfer functions → fwd_exit
//   2. Push fwd_exit into fwd[succ] via join_type
//   3. Cond: typetest narrowing on true/false successors
//
// Backward processing of label i:
//   1. Read ub[i], run backward transfer functions → ub_entry
//   2. Push ub_entry into ub[pred] via meet_type
//   3. Path-sensitive filtering on Cond edges
//
// Refinement after processing a label:
//   For each Angelic location: candidates = members(fwd) ∩ ub
//   R1-R4 dispatch. Re-enqueue on change.
//
// Convergence:
//   Forward monotone ascending, backward monotone descending,
//   refinement bounded by |members| per location. Product lattice
//   guarantees fixpoint in finite steps.
//
// The AST is mutated exactly once, at finalization, after convergence.

#include "../lang.h"
#include "../subtype.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>

namespace vc
{
  // ===== Constants =====

  constexpr size_t MAX_ITERS_PER_LABEL = 200;

  // ===== Direction enum =====

  enum class Direction
  {
    Forward,
    Backward,
    Both
  };

  // ===== Forward declarations =====

  bool is_lambda_function(const Node& func);

  // ===== Dispatch tables =====

  const std::map<std::string_view, Token> primitive_from_name = {
    {"none", None},
    {"bool", Bool},
    {"i8", I8},
    {"i16", I16},
    {"i32", I32},
    {"i64", I64},
    {"u8", U8},
    {"u16", U16},
    {"u32", U32},
    {"u64", U64},
    {"ilong", ILong},
    {"ulong", ULong},
    {"isize", ISize},
    {"usize", USize},
    {"f32", F32},
    {"f64", F64},
  };

  const std::map<std::string_view, Token> ffi_primitive_from_name = {
    {"ptr", Ptr}};

  const std::initializer_list<Token> integer_types = {
    I8, I16, I32, I64, U8, U16, U32, U64, ILong, ULong, ISize, USize};

  const std::initializer_list<Token> float_types = {F32, F64};

  // Binary ops: result type = LHS type.
  // Backward: ub[lhs] ⊓= ub[dst], ub[rhs] ⊓= fwd[lhs].
  // When fwd[lhs] is Angelic, members() is expanded to a Union
  // upper bound per ALGORITHM.md §4.1.
  const std::initializer_list<Token> propagate_lhs_ops = {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Pow,
    And,
    Or,
    Xor,
    Shl,
    Shr,
    Min,
    Max,
    LogBase,
    Atan2};

  // Heterogeneous binary ops where LHS and RHS have different types.
  // Backward: only push ub[dst] to LHS, NOT to RHS.
  // Currently empty — all Verona binary ops are homogeneous.
  const std::initializer_list<Token> propagate_lhs_ops_heterogeneous = {};

  // Unary ops: result type = operand type.
  const std::initializer_list<Token> propagate_rhs_ops = {
    Neg,  Abs,  Ceil, Floor, Exp,  Log,  Sqrt,  Cbrt,  Sin,   Cos,  Tan,
    Asin, Acos, Atan, Sinh,  Cosh, Tanh, Asinh, Acosh, Atanh, Read, Freeze};

  // Ops with fixed result types.
  const std::map<Token, Token> fixed_result_type = {
    {Eq, Bool},
    {Ne, Bool},
    {Lt, Bool},
    {Le, Bool},
    {Gt, Bool},
    {Ge, Bool},
    {IsInf, Bool},
    {IsNaN, Bool},
    {Not, Bool},
    {Bits, U64},
    {Len, USize},
    {Const_E, F64},
    {Const_Pi, F64},
    {Const_Inf, F64},
    {Const_NaN, F64},
    {GetRaise, U64},
    {SetRaise, U64},
    {FreeCallback, None},
    {Pin, None},
    {Unpin, None},
    {FFIStore, None},
    {AddExternal, None},
    {RemoveExternal, None},
    {ArrayCopy, None},
    {ArrayFill, None},
    {ArrayCompare, I64},
  };

  const std::map<Token, Token> fixed_ffi_result_type = {
    {MakePtr, Ptr},
    {MakeCallback, Ptr},
    {CodePtrCallback, Ptr},
  };

  // ===== Type constructors =====

  Node primitive_type(const Token& tok)
  {
    return Type
      << (TypeName << (NameElement << (Ident ^ "_builtin") << TypeArgs)
                   << (NameElement << (Ident ^ tok.str()) << TypeArgs));
  }

  Node ffi_primitive_type(const Token& tok)
  {
    return Type
      << (TypeName << (NameElement << (Ident ^ "_builtin") << TypeArgs)
                   << (NameElement << (Ident ^ "ffi") << TypeArgs)
                   << (NameElement << (Ident ^ tok.str()) << TypeArgs));
  }

  Node primitive_or_ffi_type(const Token& tok)
  {
    if (tok == Ptr)
      return ffi_primitive_type(tok);

    return primitive_type(tok);
  }

  Node string_type()
  {
    return Type
      << (TypeName << (NameElement << (Ident ^ "_builtin") << TypeArgs)
                   << (NameElement << (Ident ^ "string") << TypeArgs));
  }

  Node ref_type(const Node& inner)
  {
    return Type
      << (TypeName << (NameElement << (Ident ^ "_builtin") << TypeArgs)
                   << (NameElement << (Ident ^ "ref")
                                   << (TypeArgs << clone(inner))));
  }

  Node cown_type(const Node& inner)
  {
    return Type
      << (TypeName << (NameElement << (Ident ^ "_builtin") << TypeArgs)
                   << (NameElement << (Ident ^ "cown")
                                   << (TypeArgs << clone(inner))));
  }

  // ===== Angelic type system (ALGORITHM.md §1.2–1.3) =====

  // Named angelic sets with preference orders.
  // IntSet: all integer primitives. Partial order with u64 as top.
  // FloatSet: f32, f64. Total order with f64 as top.

  static const std::vector<Token>& intset_members()
  {
    static const std::vector<Token> m = {
      I8, I16, I32, I64, U8, U16, U32, U64, ILong, ULong, ISize, USize};
    return m;
  }

  static const std::vector<Token>& floatset_members()
  {
    static const std::vector<Token> m = {F32, F64};
    return m;
  }

  // Partial preference order for IntSet (ALGORITHM.md §1.3).
  // Returns true if a ≤ b (a is less preferred than b).
  // u64 is the top (most preferred / default).
  [[maybe_unused]]
  static bool int_pref_leq(Token a, Token b)
  {
    if (a == b)
      return true;
    // Everything ≤ u64.
    if (b == U64)
      return true;
    // Signed chain: i8 ≤ i16 ≤ i32 ≤ i64.
    if (a == I8 && (b == I16 || b == I32 || b == I64))
      return true;
    if (a == I16 && (b == I32 || b == I64))
      return true;
    if (a == I32 && b == I64)
      return true;
    // Unsigned chain: u8 ≤ u16 ≤ u32.
    if (a == U8 && (b == U16 || b == U32))
      return true;
    if (a == U16 && b == U32)
      return true;
    // All else incomparable (isize, usize, ilong, ulong vs fixed-width).
    return false;
  }

  // Float preference: f32 ≤ f64 (total order).
  [[maybe_unused]]
  static bool float_pref_leq(Token a, Token b)
  {
    if (a == b)
      return true;
    if (a == F32 && b == F64)
      return true;
    return false;
  }

  // Construct Angelic(Concrete ∩ IntSet) — default integer literal.
  Node angelic_int()
  {
    return Type << (AngelicSubtype << (Isect << Concrete << IntSet));
  }

  // Construct Angelic(Concrete ∩ FloatSet) — default float literal.
  Node angelic_float()
  {
    return Type << (AngelicSubtype << (Isect << Concrete << FloatSet));
  }

  // Check if a type is Angelic (AngelicSubtype wrapper).
  bool is_angelic(const Node& type)
  {
    return type && type == Type && !type->empty() &&
      type->front() == AngelicSubtype;
  }

  // Check if an Angelic bound contains the Concrete constraint.
  // The bound is the child of AngelicSubtype.
  bool has_concrete(const Node& angelic_bound)
  {
    if (!angelic_bound)
      return false;
    if (angelic_bound == Concrete)
      return true;
    if (angelic_bound == Isect)
    {
      for (auto& child : *angelic_bound)
        if (child == Concrete)
          return true;
    }
    return false;
  }

  // Extract the concrete member set from an Angelic bound.
  // Strips Concrete and Angelic wrappers, expands IntSet/FloatSet,
  // and returns a flat set of primitive tokens.
  // ALGORITHM.md §5: "members(T) strips Concrete from the bound
  // and returns the set of concrete types."
  std::vector<Token> members(const Node& angelic_bound)
  {
    if (!angelic_bound)
      return {};

    // Single named set.
    if (angelic_bound == IntSet)
      return intset_members();
    if (angelic_bound == FloatSet)
      return floatset_members();

    // Concrete alone — no member info.
    if (angelic_bound == Concrete)
      return {};

    // Isect: intersect the member sets of all non-Concrete children.
    if (angelic_bound == Isect)
    {
      std::vector<Token> result;
      bool first = true;
      for (auto& child : *angelic_bound)
      {
        if (child == Concrete)
          continue;
        auto child_members = members(child);
        if (first)
        {
          result = std::move(child_members);
          first = false;
        }
        else
        {
          // Set intersection.
          std::vector<Token> isect;
          for (auto& t : result)
            for (auto& u : child_members)
              if (t == u)
                isect.push_back(t);
          result = std::move(isect);
        }
      }
      return result;
    }

    // Union: union of member sets.
    if (angelic_bound == Union)
    {
      std::vector<Token> result;
      for (auto& child : *angelic_bound)
      {
        auto child_members = members(child);
        for (auto& t : child_members)
        {
          bool found = false;
          for (auto& r : result)
            if (r == t)
            {
              found = true;
              break;
            }
          if (!found)
            result.push_back(t);
        }
      }
      return result;
    }

    // TypeName — a single concrete type. Extract its primitive token.
    if (angelic_bound == TypeName)
    {
      // Check if it's a _builtin primitive.
      if (angelic_bound->size() >= 2)
      {
        auto first = (angelic_bound->front() / Ident)->location().view();
        if (first == "_builtin")
        {
          auto name = (angelic_bound->back() / Ident)->location().view();
          auto it = primitive_from_name.find(name);
          if (it != primitive_from_name.end())
            return {it->second};
        }
      }
      return {};
    }

    return {};
  }

  // Build an Angelic bound node from a set of tokens, preserving
  // Concrete if requested.
  Node make_angelic_bound(const std::vector<Token>& toks, bool concrete)
  {
    if (toks.empty())
      return concrete ? Node{Concrete} : Node{};

    // Check if toks equals full IntSet or FloatSet.
    auto& ints = intset_members();
    auto& floats = floatset_members();
    bool is_full_intset = (toks.size() == ints.size());
    if (is_full_intset)
      for (auto& t : ints)
      {
        bool found = false;
        for (auto& u : toks)
          if (t == u)
          {
            found = true;
            break;
          }
        if (!found)
        {
          is_full_intset = false;
          break;
        }
      }
    bool is_full_floatset = (toks.size() == floats.size());
    if (is_full_floatset)
      for (auto& t : floats)
      {
        bool found = false;
        for (auto& u : toks)
          if (t == u)
          {
            found = true;
            break;
          }
        if (!found)
        {
          is_full_floatset = false;
          break;
        }
      }

    Node set_node;
    if (is_full_intset)
      set_node = IntSet;
    else if (is_full_floatset)
      set_node = FloatSet;
    else if (toks.size() == 1)
      set_node = primitive_type(toks[0])->front(); // TypeName
    else
    {
      // Build Union of TypeName nodes.
      Node u = Union;
      for (auto& t : toks)
        u << primitive_type(t)->front();
      set_node = u;
    }

    if (concrete)
      return Isect << Concrete << set_node;
    return set_node;
  }

  // Build an Angelic type from a bound node.
  Node make_angelic(const Node& bound)
  {
    return Type << (AngelicSubtype << clone(bound));
  }

  // Check if a type is an Angelic integer (Concrete ∩ IntSet).
  bool is_angelic_int(const Node& type)
  {
    if (!is_angelic(type))
      return false;
    auto bound = type->front()->front();
    if (bound == Isect)
    {
      for (auto& child : *bound)
        if (child == IntSet)
          return true;
    }
    return false;
  }

  // Check if a type is an Angelic float (Concrete ∩ FloatSet).
  bool is_angelic_float(const Node& type)
  {
    if (!is_angelic(type))
      return false;
    auto bound = type->front()->front();
    if (bound == Isect)
    {
      for (auto& child : *bound)
        if (child == FloatSet)
          return true;
    }
    return false;
  }

  // Check if a backward primitive is compatible with an Angelic forward type.
  // Returns true if prim is in members(angelic_bound).
  bool is_angelic_compatible(const Node& angelic_type, const Node& prim)
  {
    if (!is_angelic(angelic_type) || !prim)
      return false;
    auto fwd_mems = members(angelic_type->front()->front());
    for (auto& m : fwd_mems)
      if (m == prim->type())
        return true;
    return false;
  }

  // Check if a type contains any Angelic sub-nodes.
  bool contains_angelic(const Node& type)
  {
    if (!type)
      return false;
    bool found = false;
    type->traverse([&](auto node) {
      if (node == AngelicSubtype)
      {
        found = true;
        return false;
      }
      return !found;
    });
    return found;
  }

  bool is_any_type(const Node& type)
  {
    if (type != Type)
      return false;
    auto inner = type->front();
    if (inner != TypeName || inner->size() != 2)
      return false;
    if ((inner->front() / Ident)->location().view() != "_builtin")
      return false;
    return (inner->back() / Ident)->location().view() == "any";
  }

  bool is_dyn_type(const Node& type)
  {
    if (type != Type)
      return false;
    auto inner = type->front();
    if (inner != TypeName || inner->size() != 2)
      return false;
    if ((inner->front() / Ident)->location().view() != "_builtin")
      return false;
    return (inner->back() / Ident)->location().view() == "dyn";
  }

  bool is_uninformative_backward_type(const Node& type)
  {
    return is_any_type(type) || is_dyn_type(type) ||
      (type == Type && !type->empty() && type->front() == TypeSelf);
  }

  Node extract_wrapper_inner(const Node& type_node, std::string_view wrapper)
  {
    if (type_node != Type)
      return {};
    auto inner = type_node->front();
    if (inner != TypeName || inner->size() != 2)
      return {};
    if ((inner->front() / Ident)->location().view() != "_builtin")
      return {};
    if ((inner->back() / Ident)->location().view() != wrapper)
      return {};
    auto ta = inner->back() / TypeArgs;
    if (ta->size() != 1)
      return {};
    return clone(ta->front());
  }

  Node extract_ref_inner(const Node& type_node)
  {
    return extract_wrapper_inner(type_node, "ref");
  }

  Node extract_cown_inner(const Node& type_node)
  {
    return extract_wrapper_inner(type_node, "cown");
  }

  Node extract_primitive(const Node& type_node)
  {
    if (type_node != Type)
      return {};
    auto inner = type_node->front();
    if (inner != TypeName)
      return {};
    auto first = (inner->front() / Ident)->location().view();
    if (first != "_builtin")
      return {};
    if (inner->size() == 2)
    {
      auto it =
        primitive_from_name.find((inner->back() / Ident)->location().view());
      return (it != primitive_from_name.end()) ? Node{it->second} : Node{};
    }
    if (inner->size() == 3)
    {
      if ((inner->at(1) / Ident)->location().view() != "ffi")
        return {};
      auto it =
        ffi_primitive_from_name.find((inner->at(2) / Ident)->location().view());
      return (it != ffi_primitive_from_name.end()) ? Node{it->second} : Node{};
    }
    return {};
  }

  Node extract_callable_primitive(const Node& type_node)
  {
    auto prim = extract_primitive(type_node);
    if (prim)
      return prim;
    if (type_node != Type)
      return {};
    auto inner = type_node->front();
    if (!inner->in({Union, Isect}))
      return {};
    Node candidate;
    for (auto& component : *inner)
    {
      auto p = extract_primitive(Type << clone(component));
      if (!p)
        continue;
      if (candidate && candidate->type() != p->type())
        return {};
      candidate = p;
    }
    return candidate;
  }

  Node exclude_tested_type(
    Node top, const Node& source_type, const Node& tested_type)
  {
    if (source_type != Type || tested_type != Type)
      return {};

    auto inner = source_type->front();
    if (inner != Union)
      return {};

    SequentCtx ctx{top, {}, {}};
    Node remaining = Union;
    for (auto& component : *inner)
    {
      auto component_type = Type << clone(component);
      if (!Subtype(ctx, component_type, tested_type))
        remaining << clone(component);
    }

    if (remaining->empty() || remaining->size() == inner->size())
      return {};

    return (remaining->size() == 1) ? Type << clone(remaining->front()) :
                                      Type << remaining;
  }

  Node extract_wrapper_primitive(const Node& type_node)
  {
    auto inner = extract_ref_inner(type_node);
    if (!inner)
      inner = extract_cown_inner(type_node);
    return inner ? extract_callable_primitive(inner) : Node{};
  }

  // Returns the fallback concrete token for a literal that wasn't
  // resolved during inference. Used in finalize only.
  Token literal_fallback_token(const Node& lit)
  {
    if (lit->in({True, False}))
      return Bool;
    if (lit == None)
      return None;
    if (lit->in({Bin, Oct, Int, Hex, Char}))
      return U64;
    if (lit->in({Float, HexFloat}))
      return F64;
    assert(false && "unhandled literal type");
    return U64;
  }

  static bool contains_typevar(const Node& type_node)
  {
    if (!type_node)
      return false;
    bool found = false;
    type_node->traverse([&](auto node) {
      if (node == TypeVar)
        found = true;
      return !found;
    });
    return found;
  }

  bool contains_self_type(const Node& type_node)
  {
    if (!type_node)
      return false;
    bool found = false;
    type_node->traverse([&](auto node) {
      if (node == TypeSelf)
        found = true;
      return !found;
    });
    return found;
  }

  Node direct_typeparam(Node top, const Node& type_node)
  {
    if (type_node != Type)
      return {};
    auto inner = type_node->front();
    if (inner != TypeName)
      return {};
    auto def = find_def(top, inner);
    return (def && def == TypeParam) ? def : Node{};
  }

  Node lookup_method_name(const Node& stmt)
  {
    Node ident = stmt / Ident;
    if (ident)
      return ident;
    return stmt / SymbolId;
  }

  // ===== Type environment =====

  struct LocalTypeInfo
  {
    Node type;
    Node call_node; // Call/CallDyn that produced this (for backward prop)
  };

  using TypeEnv = std::map<Location, LocalTypeInfo>;

  // ===== Type lattice helpers =====

  static bool same_type_tree(const Node& left, const Node& right)
  {
    if (left == right)
      return true;
    if (!left || !right)
      return false;
    if (left->type() != right->type())
      return false;
    if (left->size() != right->size())
      return false;
    if (left == Ident)
      return left->location().view() == right->location().view();
    for (size_t i = 0; i < left->size(); i++)
      if (!same_type_tree(left->at(i), right->at(i)))
        return false;
    return true;
  }

  // ===== Forward Join (ALGORITHM.md §3.2) =====
  //
  // Returns the joined type, or {} if no change from existing.
  // Rules F1-F5 + F2a.
  static Node join_type(const Node& existing, const Node& incoming, Node top)
  {
    // F1: ⊔(TypeVar, X) = X — bottom absorbs.
    if (!existing || existing->empty() || existing->front() == TypeVar)
    {
      if (incoming && !incoming->empty() && incoming->front() == TypeVar)
        return {};
      return clone(incoming);
    }
    if (!incoming || incoming->empty() || incoming->front() == TypeVar)
      return {};

    // Pointer equality fast path.
    if (existing == incoming)
      return {};
    // Structural equality fast path.
    if (same_type_tree(existing, incoming))
      return {};

    bool ex_angelic = is_angelic(existing);
    bool in_angelic = is_angelic(incoming);

    // F2/F2a: Both Angelic.
    if (ex_angelic && in_angelic)
    {
      auto ex_bound = existing->front()->front();
      auto in_bound = incoming->front()->front();
      bool ex_concrete = has_concrete(ex_bound);
      bool in_concrete = has_concrete(in_bound);

      // F2a: Mixed Concrete — one has Concrete, other doesn't.
      // Fall back to demonic Union (different resolution semantics).
      if (ex_concrete != in_concrete)
      {
        Node u = Union;
        u << clone(existing->front());
        u << clone(incoming->front());
        return Type << u;
      }

      // F2: Same Concrete status — intersect bounds.
      auto ex_members = members(ex_bound);
      auto in_members = members(in_bound);

      // Intersect member sets.
      std::vector<Token> isect;
      for (auto& t : ex_members)
        for (auto& u : in_members)
          if (t == u)
            isect.push_back(t);

      if (isect.empty())
      {
        // Empty intersection — R4 will catch this.
        // Return Angelic with empty bound.
        if (ex_concrete)
          return make_angelic(Isect << Concrete);
        // Non-Concrete empty intersection is also a contradiction.
        // Return Angelic with empty Isect bound so members() = {}.
        return make_angelic(Isect);
      }

      // Build tightened Angelic bound.
      auto new_bound = make_angelic_bound(isect, ex_concrete);
      return make_angelic(new_bound);
    }

    // F3: ⊔(Angelic(T), concrete S) = S — concrete absorbs Angelic.
    if (ex_angelic && !in_angelic)
      return clone(incoming);
    if (!ex_angelic && in_angelic)
      return {}; // existing is concrete, keep it.

    // Self type handling.
    bool existing_has_self = contains_self_type(existing);
    bool incoming_has_self = contains_self_type(incoming);
    if (existing_has_self != incoming_has_self)
    {
      if (existing_has_self)
        return clone(incoming);
      return {};
    }

    // F5: Subtype absorption.
    SequentCtx ctx{top, {}, {}};

    if (Subtype.invariant(ctx, existing, incoming))
      return {};

    if (Subtype(ctx, incoming, existing))
      return {};
    if (Subtype(ctx, existing, incoming))
      return clone(incoming);

    // F4: Build union.
    auto e_inner = existing->front();
    auto i_inner = incoming->front();
    Node u = Union;

    if (e_inner == Union)
      for (auto& c : *e_inner)
        u << clone(c);
    else
      u << clone(e_inner);

    bool covered = false;
    for (auto& m : *u)
    {
      if (Subtype(ctx, incoming, Type << clone(m)))
      {
        covered = true;
        break;
      }
    }

    if (!covered)
    {
      auto it = u->begin();
      while (it != u->end())
      {
        if (Subtype(ctx, Type << clone(*it), incoming))
          it = u->erase(it, std::next(it));
        else
          ++it;
      }
      u << clone(i_inner);
    }

    return (u->size() == 1) ? Type << clone(u->front()) : Type << u;
  }

  // ===== Backward Meet (ALGORITHM.md §4) =====
  //
  // meet_type computes the intersection of two upper bounds.
  // Used for backward constraint merging. Returns the tightened
  // type, or {} if no change from existing.
  //
  // Upper bounds are always concrete types or Unions/Isects of
  // concrete types. Angelic does NOT appear in the backward lattice.
  static Node meet_type(const Node& existing, const Node& incoming, Node top)
  {
    // Top absorbs: no existing constraint means incoming is the result.
    if (!existing || existing->empty() || existing->front() == TypeVar)
      return clone(incoming);
    if (!incoming || incoming->empty() || incoming->front() == TypeVar)
      return {};

    // Pointer/structural equality fast paths.
    if (existing == incoming)
      return {};
    if (same_type_tree(existing, incoming))
      return {};

    // Skip uninformative backward types (any, dyn, TypeSelf).
    if (is_uninformative_backward_type(incoming))
      return {};
    if (is_uninformative_backward_type(existing))
      return clone(incoming);

    // Subtype checks: if existing <: incoming, existing is already tighter.
    SequentCtx ctx{top, {}, {}};

    if (Subtype(ctx, existing, incoming))
      return {}; // existing already satisfies incoming.
    if (Subtype(ctx, incoming, existing))
      return clone(incoming); // incoming is tighter.

    // General intersection: extract concrete members from both,
    // compute set intersection.
    auto extract_members = [](const Node& type) -> Nodes {
      Nodes result;
      if (type->front() == Union)
      {
        for (auto& child : *(type->front()))
          result.push_back(Type << clone(child));
      }
      else
      {
        result.push_back(clone(type));
      }
      return result;
    };

    auto ex_members = extract_members(existing);
    auto in_members = extract_members(incoming);

    Nodes isect;
    for (auto& e : ex_members)
    {
      for (auto& i : in_members)
      {
        bool e_sub_i = Subtype(ctx, e, i);
        bool i_sub_e = !e_sub_i && Subtype(ctx, i, e);
        if (e_sub_i || i_sub_e || same_type_tree(e, i))
        {
          // Keep the more specific one.
          isect.push_back(e_sub_i ? clone(e) : clone(i));
          break;
        }
      }
    }

    if (isect.empty())
    {
      // Empty intersection — contradictory constraints.
      // Return bottom of the backward lattice: empty Union (∅).
      // This ensures the ub is tightened to ∅, so R4 will detect
      // via members(fwd) ∩ ub = ∅. ALGORITHM.md §10.4.
      return Type << Union;
    }

    if (isect.size() == 1)
    {
      if (same_type_tree(isect[0], existing))
        return {};
      return isect[0];
    }

    // Build union of intersection results.
    Node u = Union;
    for (auto& m : isect)
      u << clone(m->front());
    Node result = Type << u;
    if (same_type_tree(result, existing))
      return {};
    return result;
  }

  // ===== TypetestTrace =====

  struct TypetestTrace
  {
    Node src;
    Node type;
    bool negated;
  };

  static std::optional<TypetestTrace>
  trace_typetest(const Node& cond_local, const Node& body)
  {
    auto target_loc = cond_local->location();
    bool negated = false;
    for (auto it = body->rbegin(); it != body->rend(); ++it)
    {
      auto& stmt = *it;
      if (stmt == Not)
      {
        auto dst = stmt / LocalId;
        if (dst->location() == target_loc)
        {
          target_loc = (stmt / Rhs)->location();
          negated = !negated;
        }
      }
      else if (stmt == Typetest)
      {
        auto dst = stmt / LocalId;
        if (dst->location() == target_loc)
          return TypetestTrace{stmt / Rhs, stmt / Type, negated};
      }
    }
    return std::nullopt;
  }

  // ===== Method resolution infrastructure =====

  struct MethodInfo
  {
    Node func;
    NodeMap<Node> subst;
  };

  struct MethodOwner
  {
    Node class_def;
    Node subst_source;
    std::string key;
  };

  static std::string typename_path_key(const Node& type_name)
  {
    if (type_name != TypeName)
      return {};
    std::string key;
    for (auto& elem : *type_name)
    {
      if (!key.empty())
        key += "::";
      key += std::string((elem / Ident)->location().view());
    }
    return key;
  }

  static MethodOwner resolve_method_owner(Node top, const Node& receiver_type)
  {
    if (receiver_type != Type)
      return {};
    auto inner = receiver_type->front();
    if (inner != TypeName)
      return {};
    auto class_def = find_def(top, inner);
    Node subst_source = inner;

    while (class_def == TypeAlias)
    {
      auto alias_type = class_def / Type;
      if (alias_type->front() != TypeName)
        break;
      class_def = find_def(top, alias_type->front());
      if (!class_def)
        return {};
    }

    if ((!class_def || class_def != ClassDef) && inner->size() > 1)
    {
      Node base = TypeName;
      for (size_t i = 0; i + 1 < inner->size(); i++)
        base << clone(inner->at(i));
      auto base_def = find_def(top, base);
      if (base_def && base_def == ClassDef)
      {
        class_def = base_def;
        subst_source = base;
      }
    }

    if (!class_def || class_def != ClassDef)
      return {};
    return {class_def, subst_source, typename_path_key(subst_source)};
  }

  // ===== Generic type inference helpers =====

  NodeMap<Node>
  build_class_subst(const Node& class_def, const Node& typename_node)
  {
    NodeMap<Node> subst;
    auto tps = class_def / TypeParams;
    auto ta = typename_node->back() / TypeArgs;
    if (tps->size() == ta->size())
      for (size_t i = 0; i < tps->size(); i++)
        subst[tps->at(i)] = ta->at(i);
    return subst;
  }

  // Forward decl for mutual recursion.
  Node apply_subst(Node top, const Node& type_node, const NodeMap<Node>& subst);

  void extract_constraints(
    Node top,
    const Node& f_inner,
    const Node& a_inner,
    NodeMap<LocalTypeInfo>& constraints,
    bool is_default)
  {
    if (f_inner == TypeName)
    {
      auto def = find_def(top, f_inner);
      if (def && def == TypeParam)
      {
        Node actual_type = Type << clone(a_inner);
        auto existing = constraints.find(def);
        if (existing == constraints.end())
          constraints[def] = {actual_type, {}};
        else if (is_angelic(existing->second.type) && !is_default)
          constraints[def] = {actual_type, {}};
        return;
      }
    }

    if (f_inner == TypeName && a_inner == TypeName)
    {
      bool structural = (f_inner->size() == a_inner->size());
      if (structural)
      {
        for (size_t i = 0; i < f_inner->size(); i++)
        {
          if (
            (f_inner->at(i) / Ident)->location().view() !=
            (a_inner->at(i) / Ident)->location().view())
          {
            structural = false;
            break;
          }
          if (
            (f_inner->at(i) / TypeArgs)->size() !=
            (a_inner->at(i) / TypeArgs)->size())
          {
            structural = false;
            break;
          }
        }
      }

      if (structural)
      {
        for (size_t i = 0; i < f_inner->size(); i++)
        {
          auto f_ta = f_inner->at(i) / TypeArgs;
          auto a_ta = a_inner->at(i) / TypeArgs;
          for (size_t j = 0; j < f_ta->size(); j++)
            extract_constraints(
              top,
              f_ta->at(j)->front(),
              a_ta->at(j)->front(),
              constraints,
              is_default);
        }
        return;
      }

      auto f_def = find_def(top, f_inner);
      auto a_def = find_def(top, a_inner);
      if (
        f_def && a_def && f_def == ClassDef && (f_def / Shape) == Shape &&
        a_def == ClassDef)
      {
        auto shape_to_formal = build_class_subst(f_def, f_inner);
        if (!shape_to_formal.empty())
        {
          auto actual_subst = build_class_subst(a_def, a_inner);
          for (auto& sf : *(f_def / ClassBody))
          {
            if (sf != Function)
              continue;
            auto mname = (sf / Ident)->location().view();
            auto hand = (sf / Lhs)->type();
            auto arity = (sf / Params)->size();
            for (auto& af : *(a_def / ClassBody))
            {
              if (af != Function)
                continue;
              if ((af / Ident)->location().view() != mname)
                continue;
              if ((af / Lhs)->type() != hand)
                continue;
              if ((af / Params)->size() != arity)
                continue;
              auto formal_params = sf / Params;
              auto actual_params = af / Params;
              for (size_t j = 0; j < formal_params->size(); j++)
              {
                auto formal_param = apply_subst(
                  top, formal_params->at(j) / Type, shape_to_formal);
                auto actual_param =
                  apply_subst(top, actual_params->at(j) / Type, actual_subst);
                if (
                  !formal_param || !actual_param ||
                  actual_param->front() == TypeVar)
                  continue;
                extract_constraints(
                  top,
                  formal_param->front(),
                  actual_param->front(),
                  constraints,
                  is_default);
              }
              auto formal_ret = apply_subst(top, sf / Type, shape_to_formal);
              auto actual_ret = apply_subst(top, af / Type, actual_subst);
              extract_constraints(
                top,
                formal_ret->front(),
                actual_ret->front(),
                constraints,
                is_default);
              break;
            }
          }
        }
      }
      return;
    }

    if (
      f_inner->type() == a_inner->type() &&
      f_inner->size() == a_inner->size() &&
      f_inner->in({Union, Isect, TupleType}))
    {
      for (size_t i = 0; i < f_inner->size(); i++)
        extract_constraints(
          top, f_inner->at(i), a_inner->at(i), constraints, is_default);
    }
  }

  Node apply_subst(Node top, const Node& type_node, const NodeMap<Node>& subst)
  {
    if (type_node != Type || subst.empty())
      return clone(type_node);
    auto inner = type_node->front();

    if (inner == TypeName)
    {
      auto def = find_def(top, inner);
      if (def && def == TypeParam)
      {
        auto it = subst.find(def);
        if (it != subst.end())
          return clone(it->second);
      }
      Node new_tn = TypeName;
      for (auto& elem : *inner)
      {
        Node new_ta = TypeArgs;
        for (auto& ta_child : *(elem / TypeArgs))
          new_ta << apply_subst(top, ta_child, subst);
        new_tn << (NameElement << clone(elem / Ident) << new_ta);
      }
      return Type << new_tn;
    }

    if (inner->in({Union, Isect, TupleType}))
    {
      Node new_inner = inner->type();
      for (auto& child : *inner)
        new_inner << apply_subst(top, Type << clone(child), subst)->front();
      return Type << new_inner;
    }
    return clone(type_node);
  }

  // ===== Method resolution =====

  // Method cache: module-level (reset per run).
  static std::map<
    std::tuple<std::string, std::string, std::string, size_t>,
    Node>*
    active_method_cache = nullptr;

  // RAII guard for active_method_cache — ensures cleanup on exception.
  struct MethodCacheGuard
  {
    MethodCacheGuard(decltype(active_method_cache) cache)
    {
      active_method_cache = cache;
    }
    ~MethodCacheGuard()
    {
      active_method_cache = nullptr;
    }
    MethodCacheGuard(const MethodCacheGuard&) = delete;
    MethodCacheGuard& operator=(const MethodCacheGuard&) = delete;
  };

  static Node resolve_class_method_cached(
    const MethodOwner& owner,
    std::string_view method_name,
    Token hand,
    size_t arity)
  {
    if (!owner.class_def)
      return {};

    auto key = std::make_tuple(
      owner.key,
      std::string(method_name),
      std::string(hand.str()),
      arity);

    if (active_method_cache)
    {
      auto it = active_method_cache->find(key);
      if (it != active_method_cache->end())
        return it->second;
    }

    Node func;
    for (auto& child : *(owner.class_def / ClassBody))
    {
      if (child != Function)
        continue;
      auto child_name = lookup_method_name(child);
      if (!child_name || child_name->location().view() != method_name)
        continue;
      if ((child / Lhs)->type() != hand)
        continue;
      if ((child / Params)->size() != arity)
        continue;
      func = child;
      break;
    }

    if (active_method_cache)
      (*active_method_cache)[std::move(key)] = func;

    return func;
  }

  static MethodInfo resolve_method(
    Node top,
    const Node& receiver_type,
    const Node& method_ident,
    Token hand,
    size_t arity,
    const Node& method_typeargs)
  {
    auto owner = resolve_method_owner(top, receiver_type);
    if (owner.class_def)
    {
      auto subst = build_class_subst(owner.class_def, owner.subst_source);
      auto func = resolve_class_method_cached(
        owner, method_ident->location().view(), hand, arity);
      if (func)
      {
        auto func_tps = func / TypeParams;
        if (
          !method_typeargs->empty() &&
          method_typeargs->size() == func_tps->size())
        {
          for (size_t i = 0; i < func_tps->size(); i++)
            subst[func_tps->at(i)] = method_typeargs->at(i);
        }
        return MethodInfo{func, std::move(subst)};
      }
    }

    if (receiver_type == Type && receiver_type->front() == Union)
    {
      MethodInfo first_info;
      for (auto& member : *(receiver_type->front()))
      {
        Node member_type = Type << clone(member);
        auto info = resolve_method(
          top, member_type, method_ident, hand, arity, method_typeargs);
        if (!info.func)
          return {};
        if (!first_info.func)
          first_info = std::move(info);
      }
      return first_info;
    }

    auto ref_inner = extract_ref_inner(receiver_type);
    if (!ref_inner)
      ref_inner = extract_cown_inner(receiver_type);
    if (ref_inner)
      return resolve_method(
        top, ref_inner, method_ident, hand, arity, method_typeargs);
    return {};
  }

  static Node resolve_method_return_type(
    Node top,
    const Node& receiver_type,
    const Node& method_ident,
    Token hand,
    size_t arity,
    const Node& method_typeargs)
  {
    auto info = resolve_method(
      top, receiver_type, method_ident, hand, arity, method_typeargs);
    if (!info.func)
      return {};
    auto ret = apply_subst(top, info.func / Type, info.subst);

    if (ret && ret->front() == TypeVar && hand == Rhs)
    {
      auto lhs_info = resolve_method(
        top, receiver_type, method_ident, Lhs, arity, method_typeargs);
      if (lhs_info.func)
      {
        auto lhs_ret = apply_subst(top, lhs_info.func / Type, lhs_info.subst);
        auto inner = extract_ref_inner(lhs_ret);
        if (inner)
          return inner;
      }
    }
    return ret;
  }

  static MethodInfo resolve_callable_method(
    Node top,
    const Node& receiver_type,
    const Node& method_ident,
    Token hand,
    size_t arity,
    const Node& method_typeargs)
  {
    auto info = resolve_method(
      top, receiver_type, method_ident, hand, arity, method_typeargs);
    if (!info.func && hand == Rhs)
      info = resolve_method(
        top, receiver_type, method_ident, Lhs, arity, method_typeargs);
    return info;
  }

  // ===== Shape propagation helpers =====

  static bool replace_if_changed(
    const Node& owner, const Node& old_child, const Node& new_child)
  {
    if (same_type_tree(old_child, new_child))
      return false;
    owner->replace(old_child, new_child);
    return true;
  }

  static std::unordered_set<const void*>& get_lambda_returns_omitted()
  {
    static std::unordered_set<const void*> s;
    return s;
  }

  static bool lambda_return_was_omitted(const Node& func)
  {
    return get_lambda_returns_omitted().count(func.get()) > 0;
  }

  // ===== Call navigation =====

  struct ScopeInfo
  {
    Node name_elem;
    Node def;
  };

  Node navigate_call(Node call, Node top, std::vector<ScopeInfo>& scopes)
  {
    auto funcname = call / FuncName;
    auto args = call / Args;
    auto func_def = find_func_def(top, funcname, args->size(), call / Lhs);
    if (!func_def)
      return {};

    Node def = top;
    for (auto it = funcname->begin(); it != funcname->end(); ++it)
    {
      bool is_last = (it + 1 == funcname->end());
      if (is_last)
      {
        scopes.push_back({*it, func_def});
      }
      else
      {
        auto defs = def->look(((*it) / Ident)->location());
        def = defs.front();
        scopes.push_back({*it, def});
      }
    }
    return func_def;
  }

  // ===== TypeArg inference =====

  static bool infer_typeargs(
    Node call,
    Node func_def,
    std::vector<ScopeInfo>& scopes,
    TypeEnv& env,
    Node top)
  {
    auto args = call / Args;
    auto params = func_def / Params;

    bool needs_inference = false;
    for (auto& scope : scopes)
    {
      auto ta = scope.name_elem / TypeArgs;
      auto tps = scope.def / TypeParams;
      if (ta->empty() && !tps->empty())
      {
        needs_inference = true;
        break;
      }
    }

    if (!needs_inference)
      return false;

    NodeMap<LocalTypeInfo> constraints;
    for (size_t i = 0; i < params->size() && i < args->size(); i++)
    {
      auto arg_loc = (args->at(i) / Rhs)->location();
      auto arg_it = env.find(arg_loc);
      if (arg_it == env.end())
        continue;
      extract_constraints(
        top,
        (params->at(i) / Type)->front(),
        arg_it->second.type->front(),
        constraints,
        is_angelic(arg_it->second.type));
    }

    bool all_default = !constraints.empty();
    for (auto& [tp, info] : constraints)
      if (!is_angelic(info.type))
        all_default = false;

    for (auto& scope : scopes)
    {
      auto ta = scope.name_elem / TypeArgs;
      auto tps = scope.def / TypeParams;
      if (tps->empty())
        continue;

      if (!ta->empty())
      {
        bool needs_reinfer = false;
        for (auto& t : *ta)
          if (direct_typeparam(top, t))
          {
            needs_reinfer = true;
            break;
          }
        if (!needs_reinfer)
          continue;
      }

      bool all_constrained = true;
      Node new_ta = TypeArgs;
      for (auto& tp : *tps)
      {
        auto find = constraints.find(tp);
        if (find == constraints.end())
        {
          all_constrained = false;
          break;
        }
        new_ta << clone(find->second.type);
      }
      if (all_constrained)
        snmalloc::UNUSED(replace_if_changed(scope.name_elem, ta, new_ta));
    }

    return all_default;
  }

  // ===== Tuple tracking =====

  struct TupleTracking
  {
    size_t size;
    bool is_array_lit;
    std::vector<Node> element_types;
    std::vector<Location> element_value_locs;
  };

  static Node infer_tracked_tuple_type(const TupleTracking& tt)
  {
    if (tt.is_array_lit)
    {
      Node common;
      bool uniform = true;
      for (size_t i = 0; i < tt.size && uniform; i++)
      {
        if (tt.element_value_locs[i].view().empty())
        {
          uniform = false;
          break;
        }
        auto& et = tt.element_types[i];
        if (!et || !extract_primitive(et))
        {
          uniform = false;
          break;
        }
        if (!common)
          common = clone(et);
        else if (et->front()->type() != common->front()->type())
          uniform = false;
      }
      if (uniform && common && tt.size > 0)
        return clone(common);
      return {};
    }

    for (size_t i = 0; i < tt.size; i++)
      if (!tt.element_types[i])
        return {};
    if (tt.size == 0)
      return {};
    if (tt.size == 1)
      return Type << clone(tt.element_types[0]->front());

    Node tup = TupleType;
    for (auto& et : tt.element_types)
      tup << clone(et->front());
    return Type << tup;
  }

  // ===== CFG (immutable after construction) =====

  struct CFG
  {
    struct LabelInfo
    {
      Node function;
      Node label;
    };

    std::vector<LabelInfo> labels;
    std::map<Node, std::pair<size_t, size_t>> func_label_range;
    std::vector<std::vector<size_t>> succ, pred;

    // Per-function label ID to global index.
    std::map<Node, std::map<std::string, size_t>> func_label_idx;

    // Per-function def stmts (scoped to avoid Location collision).
    std::map<Node, std::map<Location, Node>> func_def_stmts;

    void build(Node top)
    {
      // Collect all functions.
      std::vector<Node> functions;
      top->traverse([&](auto node) {
        if (node != Function)
          return node == Top || node == ClassDef || node == ClassBody ||
            node == Lib || node == Symbols;
        functions.push_back(node);
        return false;
      });

      // Build global label array.
      for (auto& func : functions)
      {
        auto func_labels = func / Labels;
        if (func_labels->empty())
          continue;
        size_t first = labels.size();
        for (auto& lbl : *func_labels)
          labels.push_back({func, lbl});
        func_label_range[func] = {first, labels.size()};
      }

      size_t n = labels.size();
      succ.resize(n);
      pred.resize(n);

      // Build per-function label index.
      for (size_t i = 0; i < n; i++)
      {
        auto& li = labels[i];
        auto key = std::string((li.label / LabelId)->location().view());
        func_label_idx[li.function][key] = i;
      }

      // Build CFG edges.
      for (size_t i = 0; i < n; i++)
      {
        auto& li = labels[i];
        auto term = li.label / Return;
        auto& idx = func_label_idx[li.function];

        if (term == Cond)
        {
          auto t =
            idx.find(std::string((term / Lhs)->location().view()));
          auto f =
            idx.find(std::string((term / Rhs)->location().view()));
          if (t != idx.end())
            succ[i].push_back(t->second);
          if (f != idx.end())
            succ[i].push_back(f->second);
        }
        else if (term == Jump)
        {
          auto t =
            idx.find(std::string((term / LabelId)->location().view()));
          if (t != idx.end())
            succ[i].push_back(t->second);
        }
      }

      for (size_t i = 0; i < n; i++)
        for (auto s : succ[i])
          pred[s].push_back(i);

      // Compute RPO ordering for worklist scheduling.
      compute_rpo();

      // Build per-function def stmts.
      for (auto& [func, range] : func_label_range)
      {
        auto& defs = func_def_stmts[func];
        for (size_t i = range.first; i < range.second; i++)
        {
          auto body = labels[i].label / Body;
          for (auto& stmt : *body)
            if (!stmt->empty() && stmt->front() == LocalId)
              defs[stmt->front()->location()] = stmt;
        }
      }
    }

    // RPO index for each label (lower = earlier in RPO).
    std::vector<size_t> rpo_index;

    void compute_rpo()
    {
      size_t n = labels.size();
      rpo_index.resize(n, 0);
      std::vector<bool> visited(n, false);
      std::vector<size_t> post_order;
      post_order.reserve(n);

      // DFS from each function's entry label.
      std::function<void(size_t)> dfs = [&](size_t u) {
        if (visited[u])
          return;
        visited[u] = true;
        for (auto s : succ[u])
          dfs(s);
        post_order.push_back(u);
      };

      for (auto& [func, range] : func_label_range)
        dfs(range.first);

      // Any unreachable labels.
      for (size_t i = 0; i < n; i++)
        if (!visited[i])
          post_order.push_back(i);

      // Reverse post-order: reverse the post_order list.
      for (size_t i = 0; i < post_order.size(); i++)
        rpo_index[post_order[post_order.size() - 1 - i]] = i;
    }

    size_t size() const
    {
      return labels.size();
    }

    const std::map<Location, Node>& def_stmts_for(const Node& func) const
    {
      static const std::map<Location, Node> empty;
      auto it = func_def_stmts.find(func);
      return (it != func_def_stmts.end()) ? it->second : empty;
    }
  };

  // ===== GlobalInfer (orchestrator) =====
  //
  // Product lattice per location (ALGORITHM.md §2):
  //   fwd[x]  — forward type (lower bound, ascending via join)
  //   ub[x]   — upper bound (backward constraint, descending via meet)
  //
  // Forward and backward are independent analyses that interact
  // through refinement. The AST is mutated only at finalization.

  struct GlobalInfer
  {
    Node top;
    CFG cfg;

    // Per-label state: stored INPUTS in each direction.
    std::vector<TypeEnv> fwd;      // fwd[i] = forward types at ENTRY
    std::vector<TypeEnv> ub;       // ub[i] = upper bound constraints at EXIT
    std::vector<TypeEnv> fwd_exit; // fwd_exit[i] = forward types at EXIT

    // Cross-function mappings.
    std::map<Node, size_t> func_entry;
    std::map<Node, std::vector<size_t>> func_returns;

    // Forward pass shared state (reset per-label or per-solve).
    std::map<Location, Node> lookup_stmts;
    std::map<Location, std::pair<Location, size_t>> ref_to_tuple;
    std::map<Location, TupleTracking> tuple_locals;

    // Method resolution cache.
    std::map<
      std::tuple<std::string, std::string, std::string, size_t>,
      Node>
      method_cache_storage;

    // Worklist of (label, direction) pairs, ordered so that:
    //   Forward entries come before Backward (forward-first priority)
    //   Forward: ascending RPO (predecessors first)
    //   Backward: descending RPO (successors first)
    // enqueue(label, Both) inserts two entries.
    struct WorkItem
    {
      size_t label;
      Direction dir;
    };
    struct WorkItemCompare
    {
      const std::vector<size_t>& rpo;
      bool operator()(const WorkItem& a, const WorkItem& b) const
      {
        // Forward (0) before Backward (1).
        if (a.dir != b.dir)
          return a.dir < b.dir;
        auto ra = rpo[a.label];
        auto rb = rpo[b.label];
        if (a.dir == Direction::Forward)
          return (ra != rb) ? (ra < rb) : (a.label < b.label);
        else
          return (ra != rb) ? (ra > rb) : (a.label > b.label);
      }
    };
    WorkItemCompare wi_cmp{cfg.rpo_index};
    std::set<WorkItem, WorkItemCompare> worklist{wi_cmp};

    void enqueue(size_t label, Direction dir)
    {
      if (dir == Direction::Both)
      {
        worklist.insert({label, Direction::Forward});
        worklist.insert({label, Direction::Backward});
      }
      else
      {
        worklist.insert({label, dir});
      }
    }

    bool dequeue(size_t& label, Direction& dir)
    {
      if (worklist.empty())
        return false;
      auto it = worklist.begin();
      label = it->label;
      dir = it->dir;
      worklist.erase(it);
      return true;
    }

    // ===== Push helpers =====

    // Raw forward merge: join into a TypeEnv. Returns true if changed.
    // Does NOT enqueue — used by forward_pass for the local label_exit env.
    bool merge_fwd(
      TypeEnv& target,
      const Location& loc,
      const Node& type,
      Node call_node = {})
    {
      auto it = target.find(loc);
      if (it == target.end())
      {
        target[loc] = {clone(type), call_node};
        return true;
      }
      if (it->second.type == type)
        return false;
      auto joined = join_type(it->second.type, type, top);
      if (!joined)
        return false;
      it->second.type = joined;
      if (call_node && !it->second.call_node)
        it->second.call_node = call_node;
      return true;
    }

    // Forward push: join into fwd[label] and enqueue.
    bool push_fwd(
      size_t label,
      const Location& loc,
      const Node& type,
      Node call_node = {})
    {
      if (merge_fwd(fwd[label], loc, type, call_node))
      {
        enqueue(label, Direction::Forward);
        return true;
      }
      return false;
    }

    // Raw backward merge: meet into a TypeEnv. Returns true if changed.
    // Does NOT enqueue — used for self-merge of ub_entry into ub[label].
    bool merge_bwd(
      TypeEnv& target,
      const Location& loc,
      const Node& type)
    {
      auto it = target.find(loc);
      if (it == target.end())
      {
        target[loc] = {clone(type), {}};
        return true;
      }
      auto met = meet_type(it->second.type, type, top);
      if (!met)
        return false;
      it->second.type = met;
      return true;
    }

    // Backward push: meet into ub[label] and enqueue.
    // ALGORITHM.md §4: "ub[x] ⊓= T — always intersection."
    bool push_bwd(
      size_t label,
      const Location& loc,
      const Node& type)
    {
      if (merge_bwd(ub[label], loc, type))
      {
        enqueue(label, Direction::Backward);
        return true;
      }
      return false;
    }

    // ===== Cross-function push =====

    bool push_param_type(
      const Node& func_def,
      const Location& param_loc,
      const Node& type)
    {
      auto entry_it = func_entry.find(func_def);
      if (entry_it == func_entry.end())
        return false;
      return push_fwd(entry_it->second, param_loc, type);
    }

    bool push_return_constraint(
      const Node& func_def,
      const Node& type)
    {
      auto ret_it = func_returns.find(func_def);
      if (ret_it == func_returns.end())
        return false;
      bool changed = false;
      for (auto idx : ret_it->second)
      {
        auto term = cfg.labels[idx].label / Return;
        if (term == Return)
        {
          auto ret_loc = (term / LocalId)->location();
          if (push_bwd(idx, ret_loc, type))
            changed = true;
        }
      }
      return changed;
    }

    void push_args_to_callee(
      const Node& func_def,
      const Node& args,
      TypeEnv& caller_env)
    {
      auto params = func_def / Params;
      auto parent_cls = func_def->parent(ClassDef);

      for (size_t i = 0; i < params->size() && i < args->size(); i++)
      {
        auto param = params->at(i);
        auto formal_type = param / Type;
        if (!contains_typevar(formal_type))
          continue;

        Node resolved;
        bool allow_default = false;
        if (parent_cls)
        {
          auto pname = (param / Ident)->location().view();
          for (auto& child : *(parent_cls / ClassBody))
          {
            if (child != FieldDef)
              continue;
            if ((child / Ident)->location().view() != pname)
              continue;
            if (!contains_typevar(child / Type))
              resolved = child / Type;
            else if (is_lambda_function(func_def))
              allow_default = true;
            break;
          }
        }

        if (!resolved)
        {
          auto arg_loc = (args->at(i) / Rhs)->location();
          auto arg_it = caller_env.find(arg_loc);
          if (
            arg_it == caller_env.end() ||
            contains_typevar(arg_it->second.type))
            continue;
          if (
            contains_angelic(arg_it->second.type) && !allow_default)
            continue;
          resolved = arg_it->second.type;
        }

        push_param_type(
          func_def, (param / Ident)->location(), resolved);
      }
    }

    bool push_shape_to_lambda(
      const Node& shape_type,
      const Node& actual_type)
    {
      if (shape_type != Type || actual_type != Type)
        return false;
      auto shape_inner = shape_type->front();
      auto actual_inner = actual_type->front();
      if (shape_inner != TypeName || actual_inner != TypeName)
        return false;
      auto shape_def = find_def(top, shape_inner);
      auto actual_def = find_def(top, actual_inner);
      if (!shape_def || !actual_def)
        return false;

      while (shape_def == TypeAlias)
      {
        auto alias_type = shape_def / Type;
        if (alias_type->front() != TypeName)
          break;
        shape_def = find_def(top, alias_type->front());
        if (!shape_def)
          return false;
      }

      if (shape_def != ClassDef || actual_def != ClassDef)
        return false;
      if ((shape_def / Shape) != Shape)
        return false;

      bool changed = false;
      auto shape_subst = build_class_subst(shape_def, shape_inner);
      for (auto& sf : *(shape_def / ClassBody))
      {
        if (sf != Function)
          continue;
        auto mname = (sf / Ident)->location().view();
        auto hand = (sf / Lhs)->type();

        for (auto& af : *(actual_def / ClassBody))
        {
          if (af != Function)
            continue;
          if ((af / Ident)->location().view() != mname)
            continue;
          if ((af / Lhs)->type() != hand)
            continue;
          if ((af / Params)->size() != (sf / Params)->size())
            continue;

          auto shape_params = sf / Params;
          auto actual_params = af / Params;
          for (size_t j = 0; j < shape_params->size(); j++)
          {
            auto ap = actual_params->at(j);
            auto apt = ap / Type;
            if (apt->front() != TypeVar)
              continue;
            auto spt =
              apply_subst(top, shape_params->at(j) / Type, shape_subst);
            if (!spt || spt->front() == TypeVar || spt->front() == TypeSelf)
              continue;
            if (push_param_type(
                  af->parent(Function) ? af->parent(Function) : af,
                  (ap / Ident)->location(),
                  spt))
              changed = true;
          }

          auto shape_ret = apply_subst(top, sf / Type, shape_subst);
          if (shape_ret && shape_ret->front() != TypeVar)
          {
            auto actual_ret = af / Type;
            if (
              actual_ret->front() == TypeVar ||
              (is_lambda_function(af) && lambda_return_was_omitted(af)))
            {
              auto lambda_func =
                af->parent(Function) ? af->parent(Function) : af;
              if (push_return_constraint(lambda_func, shape_ret))
                changed = true;
            }
          }
          break;
        }
      }
      return changed;
    }

    // ===== Forward Transfer Functions (ALGORITHM.md §3.1) =====

    bool forward_pass(
      TypeEnv& env, const Node& body, const Node& /*func*/)
    {
      bool changed = false;
      auto merge = [&](const Location& loc, const Node& type,
                       Node call_node = {}) -> bool {
        auto it = env.find(loc);
        if (it == env.end())
        {
          env[loc] = {clone(type), call_node};
          changed = true;
          return true;
        }
        auto joined = join_type(it->second.type, type, top);
        if (!joined)
          return false;
        it->second.type = joined;
        if (call_node && !it->second.call_node)
          it->second.call_node = call_node;
        changed = true;
        return true;
      };

      for (auto& stmt : *body)
      {
        if (stmt == Const)
        {
          auto dst = stmt->front();
          Node type;
          if (stmt->size() == 3)
          {
            // Explicitly typed literal.
            type = primitive_or_ffi_type(stmt->at(1)->type());
          }
          else
          {
            // Untyped literal — produce Angelic.
            auto lit = stmt->back();
            if (lit->in({Bin, Oct, Int, Hex, Char}))
            {
              // If forward env already has a concrete integer for
              // this location (from a prior iteration's refinement),
              // preserve it.
              auto it = env.find(dst->location());
              if (it != env.end() && !is_angelic(it->second.type))
              {
                auto prim = extract_primitive(it->second.type);
                if (prim && prim->in(integer_types))
                  type = clone(it->second.type);
              }
              if (!type)
                type = angelic_int();
            }
            else if (lit->in({Float, HexFloat}))
            {
              auto it = env.find(dst->location());
              if (it != env.end() && !is_angelic(it->second.type))
              {
                auto prim = extract_primitive(it->second.type);
                if (prim && prim->in(float_types))
                  type = clone(it->second.type);
              }
              if (!type)
                type = angelic_float();
            }
            else if (lit->in({True, False}))
              type = primitive_type(Bool);
            else if (lit == None)
              type = primitive_type(None);
            else
              type = make_type(); // Fallback.
          }
          merge(dst->location(), type);
        }
        else if (stmt == ConstStr)
        {
          merge((stmt / LocalId)->location(), string_type());
        }
        else if (stmt == Convert)
        {
          merge((stmt / LocalId)->location(), clone(stmt / Type));
        }
        else if (stmt->in({Copy, Move}))
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto src_loc = (stmt / Rhs)->location();
          auto src_it = env.find(src_loc);
          if (src_it != env.end())
            merge(dst_loc, src_it->second.type, src_it->second.call_node);
        }
        else if (stmt == RegisterRef)
        {
          auto src_it = env.find((stmt / Rhs)->location());
          if (src_it != env.end())
            merge(
              (stmt / LocalId)->location(),
              ref_type(clone(src_it->second.type)));
        }
        else if (stmt == FieldRef)
        {
          auto arg_src = (stmt / Arg) / Rhs;
          auto dst_loc = (stmt / LocalId)->location();
          auto obj_it = env.find(arg_src->location());
          if (obj_it != env.end())
          {
            auto inner = obj_it->second.type->front();
            if (inner == TypeName)
            {
              auto class_def = find_def(top, inner);
              if (class_def && class_def == ClassDef)
              {
                auto subst = build_class_subst(class_def, inner);
                auto fname = (stmt / FieldId)->location().view();
                for (auto& f : *(class_def / ClassBody))
                {
                  if (f != FieldDef)
                    continue;
                  if ((f / Ident)->location().view() != fname)
                    continue;
                  auto ft = apply_subst(top, f / Type, subst);
                  if (ft)
                    merge(dst_loc, ref_type(ft));
                  break;
                }
              }
            }
          }
        }
        else if (stmt == Load)
        {
          auto src_it = env.find((stmt / Rhs)->location());
          if (src_it != env.end())
          {
            auto inner = extract_ref_inner(src_it->second.type);
            if (inner)
              merge((stmt / LocalId)->location(), inner);
          }
        }
        else if (stmt == Store)
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto ref_loc = (stmt / Rhs)->location();
          auto val_loc = ((stmt / Arg) / Rhs)->location();

          auto ref_it = env.find(ref_loc);
          if (ref_it != env.end())
          {
            auto inner = extract_ref_inner(ref_it->second.type);
            if (inner)
            {
              merge(dst_loc, clone(inner));

              auto rtt = ref_to_tuple.find(ref_loc);
              if (rtt != ref_to_tuple.end())
              {
                auto& [tup_loc, idx] = rtt->second;
                auto tt = tuple_locals.find(tup_loc);
                if (tt != tuple_locals.end() && idx < tt->second.size)
                {
                  auto val_it = env.find(val_loc);
                  if (val_it != env.end())
                  {
                    tt->second.element_types[idx] =
                      clone(val_it->second.type);
                    if (tt->second.is_array_lit)
                      tt->second.element_value_locs[idx] = val_loc;

                    auto tracked = infer_tracked_tuple_type(tt->second);
                    if (tracked)
                    {
                      auto tup_it = env.find(tup_loc);
                      if (
                        (tup_it == env.end()) ||
                        !same_type_tree(tup_it->second.type, tracked))
                      {
                        env[tup_loc] = {clone(tracked), {}};
                        changed = true;
                      }
                    }
                  }
                }
              }
            }
          }
        }
        else if (stmt->in({ArrayRef, ArrayRefConst}))
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto arg_loc = ((stmt / Arg) / Rhs)->location();
          auto src_it = env.find(arg_loc);

          if (stmt == ArrayRefConst)
          {
            auto index = from_chars_sep_v<size_t>(stmt / Rhs);
            auto rtt = ref_to_tuple.find(arg_loc);
            if (rtt != ref_to_tuple.end())
              ref_to_tuple[dst_loc] = {rtt->second.first, index};
            else
              ref_to_tuple[dst_loc] = {arg_loc, index};

            if (src_it != env.end())
            {
              auto inner = src_it->second.type->front();
              if (inner == TupleType && index < inner->size())
              {
                merge(dst_loc, ref_type(Type << clone(inner->at(index))));
                continue;
              }
            }
          }

          if (src_it != env.end())
            merge(dst_loc, ref_type(clone(src_it->second.type)));
        }
        else if (stmt == ArrayRefFromEnd)
        {
          auto arg_loc = ((stmt / Arg) / Rhs)->location();
          auto src_it = env.find(arg_loc);
          if (src_it != env.end())
          {
            auto inner = src_it->second.type->front();
            if (inner == TupleType)
            {
              auto offset = from_chars_sep_v<size_t>(stmt / Rhs);
              if (offset > 0 && offset <= inner->size())
              {
                auto index = inner->size() - offset;
                merge(
                  (stmt / LocalId)->location(),
                  ref_type(Type << clone(inner->at(index))));
                continue;
              }
            }
            merge(
              (stmt / LocalId)->location(),
              ref_type(clone(src_it->second.type)));
          }
        }
        else if (stmt == SplatOp)
        {
          auto arg_loc = ((stmt / Arg) / Rhs)->location();
          auto src_it = env.find(arg_loc);
          if (
            src_it != env.end() &&
            src_it->second.type->front() == TupleType)
          {
            auto inner = src_it->second.type->front();
            auto before = from_chars_sep_v<size_t>(stmt / Lhs);
            auto after = from_chars_sep_v<size_t>(stmt / Rhs);
            if (before + after <= inner->size())
            {
              auto remaining = inner->size() - before - after;
              if (remaining == 0)
                merge((stmt / LocalId)->location(), primitive_type(None));
              else if (remaining == 1)
                merge(
                  (stmt / LocalId)->location(),
                  Type << clone(inner->at(before)));
              else
              {
                Node tup = TupleType;
                for (size_t i = before; i < before + remaining; i++)
                  tup << clone(inner->at(i));
                merge((stmt / LocalId)->location(), Type << tup);
              }
              continue;
            }
          }
          merge((stmt / LocalId)->location(), make_type());
        }
        else if (stmt->in({NewArray, NewArrayConst}))
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto init_type = stmt / Type;
          auto dst_it = env.find(dst_loc);
          if (!(dst_it != env.end() && is_any_type(init_type) &&
                !is_any_type(dst_it->second.type) &&
                (dst_it->second.type->front() != TypeVar)))
            merge(dst_loc, clone(init_type));

          if (stmt == NewArrayConst)
          {
            auto sz = from_chars_sep_v<size_t>(stmt / Rhs);
            bool is_lit =
              dst_loc.view().find("array") != std::string_view::npos;
            tuple_locals[dst_loc] = {
              sz, is_lit, std::vector<Node>(sz), std::vector<Location>(sz)};
            ref_to_tuple[dst_loc] = {dst_loc, 0};
          }
        }
        else if (stmt == TypeAssertion)
        {
          // TypeAssertion is a hard constraint — overwrite directly,
          // not via join. This is idempotent and bounded.
          auto loc = (stmt / LocalId)->location();
          env[loc] = {clone(stmt / Type), {}};
          changed = true;
        }
        else if (stmt->in({New, Stack}))
        {
          merge((stmt / LocalId)->location(), clone(stmt / Type));
        }
        else if (stmt->in(propagate_lhs_ops))
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto lhs_loc = (stmt / Lhs)->location();
          auto lhs_it = env.find(lhs_loc);
          if (lhs_it != env.end())
            merge(dst_loc, clone(lhs_it->second.type));
        }
        else if (stmt->in(propagate_lhs_ops_heterogeneous))
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto lhs_loc = (stmt / Lhs)->location();
          auto lhs_it = env.find(lhs_loc);
          if (lhs_it != env.end())
            merge(dst_loc, clone(lhs_it->second.type));
        }
        else if (stmt->in(propagate_rhs_ops))
        {
          auto src_it = env.find((stmt / Rhs)->location());
          if (src_it != env.end())
            merge((stmt / LocalId)->location(), clone(src_it->second.type));
        }
        else if (auto frt = fixed_result_type.find(stmt->type());
                 frt != fixed_result_type.end())
        {
          merge((stmt / LocalId)->location(), primitive_type(frt->second));
        }
        else if (auto ffrt = fixed_ffi_result_type.find(stmt->type());
                 ffrt != fixed_ffi_result_type.end())
        {
          merge(
            (stmt / LocalId)->location(), ffi_primitive_type(ffrt->second));
        }
        else if (stmt == FFIStruct)
        {
          merge((stmt / LocalId)->location(), ffi_struct_result_type());
        }
        else if (stmt == FFILoad)
        {
          merge((stmt / LocalId)->location(), clone(stmt / Type));
        }
        else if (stmt == Call)
        {
          std::vector<ScopeInfo> scopes;
          auto func_def = navigate_call(stmt, top, scopes);
          if (!func_def)
            continue;

          auto args = stmt / Args;
          auto params = func_def / Params;

          bool all_angelic =
            infer_typeargs(stmt, func_def, scopes, env, top);

          NodeMap<Node> subst;
          for (auto& scope : scopes)
          {
            auto ta = scope.name_elem / TypeArgs;
            auto tps = scope.def / TypeParams;
            if (!ta->empty() && ta->size() == tps->size())
              for (size_t i = 0; i < tps->size(); i++)
                subst[tps->at(i)] = ta->at(i);
          }

          auto ret = apply_subst(top, func_def / Type, subst);
          if (ret)
            merge(
              (stmt / LocalId)->location(),
              ret,
              all_angelic ? stmt : Node{});

          for (size_t i = 0; i < params->size() && i < args->size(); i++)
          {
            auto pt = apply_subst(top, params->at(i) / Type, subst);
            if (pt && pt->front() != TypeVar)
            {
              auto arg_it = env.find((args->at(i) / Rhs)->location());
              if (arg_it != env.end())
                changed |=
                  push_shape_to_lambda(pt, arg_it->second.type);
            }
          }

          push_args_to_callee(func_def, args, env);
        }
        else if (stmt == Lookup)
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto src_it = env.find((stmt / Rhs)->location());
          if (src_it != env.end())
          {
            if (is_angelic(src_it->second.type))
            {
              // Propagate angelic unchanged — method resolution
              // happens when backward narrows it.
              merge(dst_loc, clone(src_it->second.type));
            }
            else
            {
              auto hand = (stmt / Lhs)->type();
              auto method_ident = lookup_method_name(stmt);
              auto method_ta = stmt / TypeArgs;
              auto arity = from_chars_sep_v<size_t>(stmt / Int);
              auto ret = resolve_method_return_type(
                top,
                src_it->second.type,
                method_ident,
                hand,
                arity,
                method_ta);
              if (ret)
                merge(dst_loc, ret);
            }
          }
          lookup_stmts[dst_loc] = stmt;
        }
        else if (stmt->in({CallDyn, TryCallDyn}))
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto src_loc = (stmt / Rhs)->location();
          auto args = stmt / Args;

          auto src_it = env.find(src_loc);
          if (src_it != env.end())
            merge(dst_loc, clone(src_it->second.type));

          auto dst_it = env.find(dst_loc);
          if (dst_it != env.end() && !dst_it->second.call_node)
            dst_it->second.call_node = stmt;

          auto lookup_it = lookup_stmts.find(src_loc);
          if (lookup_it != lookup_stmts.end())
          {
            auto lookup_node = lookup_it->second;
            auto recv_it = env.find((lookup_node / Rhs)->location());
            if (
              recv_it != env.end() &&
              !is_angelic(recv_it->second.type))
            {
              auto hand = (lookup_node / Lhs)->type();
              auto method_ident = lookup_method_name(lookup_node);
              auto method_ta = lookup_node / TypeArgs;
              auto arity = from_chars_sep_v<size_t>(lookup_node / Int);
              auto info = resolve_callable_method(
                top,
                recv_it->second.type,
                method_ident,
                hand,
                arity,
                method_ta);
              if (info.func)
              {
                auto params = info.func / Params;
                for (
                  size_t i = 0;
                  i < params->size() && i < args->size();
                  i++)
                {
                  auto pt =
                    apply_subst(top, params->at(i) / Type, info.subst);
                  if (pt && pt->front() != TypeVar)
                  {
                    auto arg_it =
                      env.find((args->at(i) / Rhs)->location());
                    if (arg_it != env.end())
                      changed |= push_shape_to_lambda(
                        pt, arg_it->second.type);
                  }
                }
                push_args_to_callee(info.func, args, env);
              }
            }
          }
        }
        else if (stmt == FFI)
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto sym_name = (stmt / SymbolId)->location();
          auto cls = body->parent(Function)->parent(ClassDef);
          while (cls)
          {
            bool found = false;
            for (auto& child : *(cls / ClassBody))
            {
              if (child != Lib)
                continue;
              for (auto& sym : *(child / Symbols))
              {
                if (sym != Symbol)
                  continue;
                if ((sym / SymbolId)->location() != sym_name)
                  continue;
                auto ret_type = sym / Type;
                if (!ret_type->empty())
                  merge(dst_loc, clone(ret_type));
                found = true;
                break;
              }
              if (found)
                break;
            }
            if (found)
              break;
            cls = cls->parent(ClassDef);
          }
        }
        else if (stmt == When)
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto src_it = env.find((stmt / Rhs)->location());
          if (src_it != env.end())
            merge(dst_loc, cown_type(src_it->second.type));

          // Push cown inner types into lambda params.
          auto l_it = lookup_stmts.find((stmt / Rhs)->location());
          if (l_it != lookup_stmts.end())
          {
            auto recv_it = env.find((l_it->second / Rhs)->location());
            if (recv_it != env.end())
            {
              auto recv_inner = recv_it->second.type->front();
              if (recv_inner == TypeName)
              {
                auto class_def = find_def(top, recv_inner);
                if (class_def && class_def == ClassDef)
                {
                  Node apply_func;
                  for (auto& child : *(class_def / ClassBody))
                  {
                    if (
                      child == Function &&
                      (child / Ident)->location().view() == "apply")
                    {
                      apply_func = child;
                      break;
                    }
                  }
                  if (apply_func)
                  {
                    auto params = apply_func / Params;
                    auto when_args = stmt / Args;
                    for (
                      size_t i = 1;
                      i < when_args->size() && i < params->size();
                      ++i)
                    {
                      auto param = params->at(i);
                      auto arg_it =
                        env.find((when_args->at(i) / Rhs)->location());
                      if (arg_it == env.end())
                        continue;
                      auto ci = extract_cown_inner(arg_it->second.type);
                      if (!ci || contains_typevar(ci) || is_any_type(ci))
                        continue;
                      push_param_type(
                        apply_func,
                        (param / Ident)->location(),
                        ref_type(ci));
                    }
                  }
                }
              }
            }
          }
        }
        else if (stmt == Typetest)
        {
          merge((stmt / LocalId)->location(), primitive_type(Bool));
        }
      }

      return changed;
    }

    // ===== Backward Transfer Functions (ALGORITHM.md §4) =====
    //
    // Computes upper bound constraints by walking the body in reverse.
    // Each use-site contributes: ub[x] ⊓= T (meet/intersection).
    // Only updates ub for Angelic locations (§4.3 skip optimization).
    // No AST mutations — refinement is handled by the solve loop.

    bool backward_pass(
      TypeEnv& env,
      TypeEnv& ub_entry,
      const TypeEnv& ub_exit,
      const Node& body)
    {
      bool changed = false;

      // push_bwd_local: meet a backward constraint into ub_entry.
      // Skips uninformative types (TypeVar, any, dyn, Angelic).
      // §4.3: Skip non-Angelic forward locations.
      auto push_bwd_local =
        [&](const Location& loc, const Node& type) -> bool {
        if (!type || type->empty())
          return false;
        if (type->front() == TypeVar || is_angelic(type))
          return false;
        if (is_uninformative_backward_type(type))
          return false;

        // §4.3: Only constrain Angelic forward locations.
        auto fwd_it = env.find(loc);
        if (fwd_it != env.end() && !is_angelic(fwd_it->second.type) &&
            fwd_it->second.type->front() != TypeVar)
          return false;

        auto it = ub_entry.find(loc);
        if (it == ub_entry.end())
        {
          ub_entry[loc] = {clone(type), {}};
          changed = true;
          return true;
        }
        auto met = meet_type(it->second.type, type, top);
        if (!met)
          return false;
        it->second.type = met;
        changed = true;
        return true;
      };

      // best_ub: find the best backward constraint for a location,
      // checking ub_entry first, then ub_exit.
      auto best_ub = [&](const Location& loc) -> Node {
        auto be_it = ub_entry.find(loc);
        if (be_it != ub_entry.end() && !is_angelic(be_it->second.type) &&
            be_it->second.type->front() != TypeVar)
          return be_it->second.type;
        auto bx_it = ub_exit.find(loc);
        if (bx_it != ub_exit.end() && !is_angelic(bx_it->second.type) &&
            bx_it->second.type->front() != TypeVar)
          return bx_it->second.type;
        return {};
      };

      // Walk body in reverse.
      for (auto it = body->rbegin(); it != body->rend(); ++it)
      {
        auto& stmt = *it;

        if (stmt->in({Copy, Move}))
        {
          // §4.1: ub[src] ⊓= ub[dst]
          auto dst_loc = (stmt / LocalId)->location();
          auto src_loc = (stmt / Rhs)->location();
          auto expected = best_ub(dst_loc);
          if (expected && !is_angelic(expected))
            push_bwd_local(src_loc, expected);
        }
        else if (stmt == Store)
        {
          // §4.1: ub[val] ⊓= field_type
          auto ref_loc = (stmt / Rhs)->location();
          auto val_loc = ((stmt / Arg) / Rhs)->location();
          auto ref_it = env.find(ref_loc);
          if (ref_it != env.end())
          {
            auto inner = extract_ref_inner(ref_it->second.type);
            if (inner && !is_any_type(inner))
              push_bwd_local(val_loc, inner);
          }
        }
        else if (stmt->in({New, Stack}))
        {
          // §4.1: ub[arg] ⊓= field_type
          auto new_type = stmt / Type;
          auto inner = new_type->front();
          if (inner == TypeName)
          {
            auto class_def = find_def(top, inner);
            if (class_def && class_def == ClassDef)
            {
              auto subst = build_class_subst(class_def, inner);
              for (auto& na : *(stmt / NewArgs))
              {
                auto arg_loc = (na / Rhs)->location();
                auto fname = (na / Ident)->location().view();
                for (auto& f : *(class_def / ClassBody))
                {
                  if (f != FieldDef)
                    continue;
                  if ((f / Ident)->location().view() != fname)
                    continue;
                  auto ft = apply_subst(top, f / Type, subst);
                  if (ft && !contains_typevar(ft))
                    push_bwd_local(arg_loc, ft);
                  break;
                }
              }
            }
          }
        }
        // NewArray/NewArrayConst: no direct backward handler needed.
        // Element type constraints flow through ArrayRefConst → Store:
        // the Store handler pushes ub[val] ⊓= ref_inner_type, which
        // is the array's element type from the forward pass.
        else if (stmt->in(propagate_lhs_ops))
        {
          // Backward for homogeneous (T, T) → T ops per §4.1:
          //   ub[lhs] ⊓= ub[dst]
          //   ub[rhs] ⊓= fwd[lhs]  (as concrete members if Angelic)
          auto dst_loc = (stmt / LocalId)->location();
          auto lhs_loc = (stmt / Lhs)->location();
          auto rhs_loc = (stmt / Rhs)->location();

          auto dst_ub = best_ub(dst_loc);
          if (dst_ub)
            push_bwd_local(lhs_loc, dst_ub);

          // §4.1: ub[rhs] ⊓= fwd[lhs]
          // When fwd[lhs] is Angelic, expand members() into a Union
          // upper bound per ALGORITHM.md §4.1 note: "Angelic forward
          // types contribute members() not themselves."
          auto lhs_it = env.find(lhs_loc);
          if (lhs_it != env.end())
          {
            if (is_angelic(lhs_it->second.type))
            {
              auto mems = members(lhs_it->second.type->front()->front());
              if (!mems.empty())
              {
                if (mems.size() == 1)
                {
                  push_bwd_local(rhs_loc, primitive_type(mems[0]));
                }
                else
                {
                  Node u = Union;
                  for (auto& m : mems)
                    u << primitive_type(m)->front();
                  push_bwd_local(rhs_loc, Type << u);
                }
              }
            }
            else if (lhs_it->second.type->front() != TypeVar)
            {
              push_bwd_local(rhs_loc, lhs_it->second.type);
            }
          }
        }
        else if (stmt->in(propagate_lhs_ops_heterogeneous))
        {
          // Backward for heterogeneous ops: only push ub[dst] to LHS.
          // RHS type differs from LHS — no constraint from dst or lhs.
          auto dst_loc = (stmt / LocalId)->location();
          auto lhs_loc = (stmt / Lhs)->location();
          auto dst_ub = best_ub(dst_loc);
          if (dst_ub)
            push_bwd_local(lhs_loc, dst_ub);
        }
        else if (stmt->in(propagate_rhs_ops))
        {
          // ub[src] ⊓= ub[dst]
          auto dst_loc = (stmt / LocalId)->location();
          auto src_loc = (stmt / Rhs)->location();
          auto dst_ub = best_ub(dst_loc);
          if (dst_ub)
            push_bwd_local(src_loc, dst_ub);
        }
        else if (stmt == FFIStore)
        {
          // ub[val] ⊓= ffi_field_type
          auto value_loc = (stmt / ValueSrc)->location();
          push_bwd_local(value_loc, clone(stmt / Type));
        }
        else if (stmt == Call)
        {
          // §4.1: ub[arg] ⊓= param_type
          std::vector<ScopeInfo> scopes;
          auto func_def = navigate_call(stmt, top, scopes);
          if (!func_def)
            continue;

          auto args = stmt / Args;
          auto params = func_def / Params;

          NodeMap<Node> subst;
          for (auto& scope : scopes)
          {
            auto ta = scope.name_elem / TypeArgs;
            auto tps = scope.def / TypeParams;
            if (!ta->empty() && ta->size() == tps->size())
              for (size_t i = 0; i < tps->size(); i++)
                subst[tps->at(i)] = ta->at(i);
          }

          // Backward refine TypeArgs from return constraint.
          auto dst_loc = (stmt / LocalId)->location();
          auto ret_type = func_def / Type;
          auto bwd_result = best_ub(dst_loc);
          if (bwd_result && ret_type->front() != TypeVar)
          {
            NodeMap<LocalTypeInfo> constraints;
            extract_constraints(
              top,
              ret_type->front(),
              bwd_result->front(),
              constraints,
              false);
            for (auto& scope : scopes)
            {
              auto tps = scope.def / TypeParams;
              for (auto& tp : *tps)
              {
                auto find = constraints.find(tp);
                if (find != constraints.end() &&
                    !is_angelic(find->second.type))
                  subst[tp] = find->second.type;
              }
            }
          }

          // Push param constraints to args.
          for (size_t i = 0; i < params->size() && i < args->size(); i++)
          {
            auto expected = apply_subst(top, params->at(i) / Type, subst);
            if (
              expected && expected->front() != TypeVar &&
              !is_uninformative_backward_type(expected))
            {
              auto arg_loc = (args->at(i) / Rhs)->location();
              push_bwd_local(arg_loc, expected);
            }
          }
        }
        else if (stmt->in({CallDyn, TryCallDyn}))
        {
          // Backward: if receiver is Angelic and we have a backward
          // constraint or concrete arg, resolve and push param types.
          auto dst_loc = (stmt / LocalId)->location();
          auto src_loc = (stmt / Rhs)->location();
          auto args = stmt / Args;

          auto lookup_it = lookup_stmts.find(src_loc);
          if (lookup_it == lookup_stmts.end())
            continue;

          auto lookup_node = lookup_it->second;
          auto recv_loc = (lookup_node / Rhs)->location();
          auto recv_it = env.find(recv_loc);
          if (recv_it == env.end())
            continue;

          auto hand = (lookup_node / Lhs)->type();
          auto method_ident = lookup_method_name(lookup_node);
          auto method_ta = lookup_node / TypeArgs;
          auto arity = from_chars_sep_v<size_t>(lookup_node / Int);

          Node resolve_type = recv_it->second.type;

          // If receiver is Angelic, try to determine concrete type
          // from backward constraints or concrete args.
          if (is_angelic(resolve_type))
          {
            Node target_prim;
            auto bwd_result = best_ub(dst_loc);
            if (bwd_result)
              target_prim = extract_primitive(bwd_result);

            if (!target_prim)
            {
              for (auto& arg_node : *args)
              {
                auto arg_it =
                  env.find((arg_node / Rhs)->location());
                if (arg_it != env.end() &&
                    !is_angelic(arg_it->second.type))
                {
                  target_prim =
                    extract_callable_primitive(arg_it->second.type);
                  if (target_prim)
                    break;
                }
              }
            }

            if (target_prim &&
                is_angelic_compatible(resolve_type, target_prim))
            {
              resolve_type =
                primitive_or_ffi_type(target_prim->type());
              push_bwd_local(recv_loc, resolve_type);
            }
          }

          // Push param constraints if we have a concrete receiver.
          if (!is_angelic(resolve_type) &&
              resolve_type->front() != TypeVar)
          {
            auto info = resolve_callable_method(
              top, resolve_type, method_ident, hand, arity, method_ta);
            if (info.func)
            {
              auto params = info.func / Params;
              for (
                size_t i = 0;
                i < params->size() && i < args->size();
                i++)
              {
                auto expected =
                  apply_subst(top, params->at(i) / Type, info.subst);
                if (
                  expected && expected->front() != TypeVar &&
                  !is_uninformative_backward_type(expected))
                {
                  auto arg_loc = (args->at(i) / Rhs)->location();
                  push_bwd_local(arg_loc, expected);
                }
              }
            }
          }
        }
        else if (stmt == FFI)
        {
          // §4.1: ub[arg] ⊓= ffi_param_type
          auto sym_name = (stmt / SymbolId)->location();
          auto cls = body->parent(Function)->parent(ClassDef);
          while (cls)
          {
            bool found = false;
            for (auto& child : *(cls / ClassBody))
            {
              if (child != Lib)
                continue;
              for (auto& sym : *(child / Symbols))
              {
                if (sym != Symbol)
                  continue;
                if ((sym / SymbolId)->location() != sym_name)
                  continue;
                auto ffi_params = sym / FFIParams;
                auto ffi_args = stmt / Args;
                auto fp = ffi_params->begin();
                auto fa = ffi_args->begin();
                while (fp != ffi_params->end() && fa != ffi_args->end())
                {
                  push_bwd_local((*fa)->location(), clone(*fp));
                  ++fp;
                  ++fa;
                }
                found = true;
                break;
              }
              if (found)
                break;
            }
            if (found)
              break;
            cls = cls->parent(ClassDef);
          }
        }
        // Load, When, Typetest — no backward constraints.
      }

      return changed;
    }

    // ===== Build =====

    void build()
    {
      cfg.build(top);
      size_t n = cfg.size();
      if (n == 0)
        return;

      // Track lambda functions with omitted return types.
      auto& lro = get_lambda_returns_omitted();
      lro.clear();
      top->traverse([&](auto node) {
        if (
          node == Function && is_lambda_function(node) &&
          (node / Type)->front() == TypeVar)
          lro.insert(node.get());
        return true;
      });

      // Allocate per-label state.
      fwd.resize(n);
      fwd_exit.resize(n);
      ub.resize(n);

      // Build cross-function mappings.
      for (auto& [func, range] : cfg.func_label_range)
      {
        func_entry[func] = range.first;
        for (size_t i = range.first; i < range.second; i++)
        {
          auto term = cfg.labels[i].label / Return;
          if (term == Return)
            func_returns[func].push_back(i);
        }
      }

      // Seed fwd[entry_label] with param types.
      for (auto& [func, entry_idx] : func_entry)
      {
        for (auto& pd : *(func / Params))
        {
          auto type = pd / Type;
          fwd[entry_idx][(pd / Ident)->location()] = {clone(type), {}};
        }
      }

      // Seed ub[return_labels] with declared return types.
      // ALGORITHM.md §4: "return: ub[ret] ⊓= func_return_type"
      for (auto& [func, return_indices] : func_returns)
      {
        auto func_ret = func / Type;
        if (!contains_typevar(func_ret) && !is_angelic(func_ret))
        {
          for (auto idx : return_indices)
          {
            auto term = cfg.labels[idx].label / Return;
            auto ret_loc = (term / LocalId)->location();
            ub[idx][ret_loc] = {clone(func_ret), {}};
          }
        }
      }

      // Enqueue entry labels for forward, return labels for backward.
      for (auto& [func, entry_idx] : func_entry)
        enqueue(entry_idx, Direction::Forward);
      for (auto& [func, return_indices] : func_returns)
        for (auto idx : return_indices)
          enqueue(idx, Direction::Backward);
    }

    // ===== Solve (ALGORITHM.md §6) =====

    void solve()
    {
      size_t n = cfg.size();
      if (n == 0)
        return;

      size_t wl_iters = 0;
      size_t label;
      Direction dir;

      while (dequeue(label, dir))
      {
        wl_iters++;

        if (wl_iters > n * MAX_ITERS_PER_LABEL)
        {
          auto& li = cfg.labels[label];
          li.function->parent()->replace(
            li.function,
            err(
              li.function,
              "Type inference did not converge after " +
                std::to_string(wl_iters) + " iterations"));
          return;
        }

        auto& li = cfg.labels[label];
        auto body = li.label / Body;
        auto term = li.label / Return;

        bool run_fwd =
          (dir == Direction::Forward || dir == Direction::Both);
        bool run_bwd =
          (dir == Direction::Backward || dir == Direction::Both);

        TypeEnv label_exit;

        // ---- Forward phase ----
        if (run_fwd)
        {
          // Copy entry env as starting point.
          for (auto& [loc, info] : fwd[label])
            label_exit[loc] = {clone(info.type), info.call_node};

          // Preserve locally-defined variables from prior iteration.
          for (auto& [loc, info] : fwd_exit[label])
          {
            if (!is_angelic(info.type))
            {
              auto it = label_exit.find(loc);
              if (it == label_exit.end())
                label_exit[loc] = {clone(info.type), info.call_node};
              else if (is_angelic(it->second.type))
                it->second.type = clone(info.type);
            }
          }

          // Clear per-label state for forward pass.
          tuple_locals.clear();
          ref_to_tuple.clear();

          // Run forward transfer functions.
          forward_pass(label_exit, body, li.function);

          // Store exit env for finalization.
          fwd_exit[label] = label_exit;

          // Push to successors.
          if (term == Cond)
          {
            auto trace = trace_typetest(term / LocalId, body);
            auto& func_idx = cfg.func_label_idx[li.function];
            auto t_it =
              func_idx.find(std::string((term / Lhs)->location().view()));
            auto f_it =
              func_idx.find(std::string((term / Rhs)->location().view()));

            for (auto& [loc, info] : label_exit)
            {
              if (trace && loc == trace->src->location())
                continue;
              if (t_it != func_idx.end())
                push_fwd(t_it->second, loc, info.type, info.call_node);
              if (f_it != func_idx.end())
                push_fwd(f_it->second, loc, info.type, info.call_node);
            }

            // Typetest narrowing (ALGORITHM.md §3.3).
            if (trace)
            {
              auto src_loc = trace->src->location();
              auto src_it = label_exit.find(src_loc);
              if (src_it != label_exit.end())
              {
                auto true_succ = trace->negated ? f_it : t_it;
                auto false_succ = trace->negated ? t_it : f_it;

                if (true_succ != func_idx.end())
                  push_fwd(
                    true_succ->second,
                    src_loc,
                    trace->type,
                    src_it->second.call_node);

                if (false_succ != func_idx.end())
                {
                  auto excluded = exclude_tested_type(
                    top, src_it->second.type, trace->type);
                  auto& push_type =
                    excluded ? excluded : src_it->second.type;
                  push_fwd(
                    false_succ->second,
                    src_loc,
                    push_type,
                    src_it->second.call_node);
                }
              }
            }
          }
          else if (term == Jump)
          {
            auto& func_idx = cfg.func_label_idx[li.function];
            auto t_it = func_idx.find(
              std::string((term / LabelId)->location().view()));
            if (t_it != func_idx.end())
            {
              for (auto& [loc, info] : label_exit)
                push_fwd(t_it->second, loc, info.type, info.call_node);
            }
          }
        }

        // ---- Backward phase ----
        if (run_bwd)
        {
          if (!run_fwd)
          {
            if (!fwd_exit[label].empty())
            {
              for (auto& [loc, info] : fwd_exit[label])
                label_exit[loc] = {clone(info.type), info.call_node};
            }
            else
            {
              for (auto& [loc, info] : fwd[label])
                label_exit[loc] = {clone(info.type), info.call_node};
            }
          }

          TypeEnv ub_entry;
          for (auto& [loc, info] : ub[label])
            ub_entry[loc] = {clone(info.type), info.call_node};
          backward_pass(label_exit, ub_entry, ub[label], body);

          fwd_exit[label] = label_exit;

          // Merge ub_entry into ub[label] (self-merge, no enqueue).
          for (auto& [loc, info] : ub_entry)
            merge_bwd(ub[label], loc, info.type);

          // Push to predecessors (path-sensitive).
          for (auto p : cfg.pred[label])
          {
            auto& pred_li = cfg.labels[p];
            auto pred_term = pred_li.label / Return;

            for (auto& [loc, info] : ub_entry)
            {
              // No path-sensitive backward filtering for the tested
              // variable. Precise filtering would require type-level
              // implication (¬T ∨ S), which we can't express. Instead
              // we push the raw constraint from both edges — sound but
              // possibly imprecise. The predecessor merges via meet,
              // which is correct for non-tested variables. For the
              // tested variable, the constraint may be tighter than
              // necessary, but refinement handles contradictions.
              push_bwd(p, loc, info.type);
            }
          }
        }

        // ---- Refinement (ALGORITHM.md §5) ----
        {
          std::set<Location> all_locs;
          for (auto& [loc, info] : fwd[label])
            all_locs.insert(loc);
          for (auto& [loc, info] : fwd_exit[label])
            all_locs.insert(loc);

          for (auto& loc : all_locs)
          {
            LocalTypeInfo* fwd_info = nullptr;
            auto exit_it = fwd_exit[label].find(loc);
            auto entry_it = fwd[label].find(loc);
            if (exit_it != fwd_exit[label].end())
              fwd_info = &exit_it->second;
            else if (entry_it != fwd[label].end())
              fwd_info = &entry_it->second;
            if (!fwd_info)
              continue;

            auto ub_it = ub[label].find(loc);
            if (ub_it == ub[label].end())
              continue;

            auto& fwd_type = fwd_info->type;
            auto& ub_type = ub_it->second.type;

            if (!fwd_type || fwd_type->empty())
              continue;
            auto fwd_front = fwd_type->front();

            // Only refine Angelic/TypeVar forward types.
            bool is_fwd_angelic = is_angelic(fwd_type);
            bool is_fwd_typevar = fwd_front == TypeVar;
            if (!is_fwd_angelic && !is_fwd_typevar)
              continue;

            if (!ub_type || ub_type->empty())
              continue;
            auto ub_front = ub_type->front();

            // Skip if backward is also unresolved.
            if (is_angelic(ub_type) || ub_front == TypeVar)
              continue;

            // ===== Refinement dispatch (ALGORITHM.md §5) =====
            // candidates = members(fwd) ∩ ub_members

            if (is_fwd_angelic)
            {
              auto bound = fwd_type->front()->front();
              auto fwd_mems = members(bound);

              // Extract ub member set for intersection.
              std::vector<Token> ub_mems;
              auto ub_prim = extract_primitive(ub_type);
              if (ub_prim)
              {
                ub_mems.push_back(ub_prim->type());
              }
              else if (ub_front == Union)
              {
                for (auto& child : *ub_front)
                {
                  auto p = extract_primitive(Type << clone(child));
                  if (p)
                    ub_mems.push_back(p->type());
                }
              }

              if (ub_mems.empty() && !has_concrete(bound) &&
                  fwd_mems.empty())
              {
                // Non-Concrete Angelic (type param) with no primitive
                // members and non-primitive ub — commit directly.
                if (entry_it != fwd[label].end())
                  entry_it->second.type = clone(ub_type);
                if (exit_it != fwd_exit[label].end())
                  exit_it->second.type = clone(ub_type);
                enqueue(label, Direction::Both);
                continue;
              }

              // Intersect: candidates = fwd_mems ∩ ub_mems.
              std::vector<Token> candidates;
              for (auto& fm : fwd_mems)
                for (auto& um : ub_mems)
                  if (fm == um)
                    candidates.push_back(fm);

              if (candidates.empty())
              {
                // R4: empty — contradictory. Leave fwd unchanged;
                // typecheck will report the error.
                continue;
              }

              if (candidates.size() == 1)
              {
                // R1: singleton — commit to concrete.
                auto concrete_type = primitive_type(candidates[0]);
                if (entry_it != fwd[label].end())
                  entry_it->second.type = clone(concrete_type);
                if (exit_it != fwd_exit[label].end())
                  exit_it->second.type = clone(concrete_type);
                enqueue(label, Direction::Both);
              }
              else if (candidates.size() < fwd_mems.size())
              {
                // R2: tightened bound — narrow Angelic.
                bool concrete = has_concrete(bound);
                auto new_bound = make_angelic_bound(candidates, concrete);
                auto new_type = make_angelic(new_bound);
                if (entry_it != fwd[label].end())
                  entry_it->second.type = clone(new_type);
                if (exit_it != fwd_exit[label].end())
                  exit_it->second.type = clone(new_type);
                enqueue(label, Direction::Both);
              }
              // R3: candidates == fwd_mems — no change, no enqueue.
            }
            else if (is_fwd_typevar)
            {
              // TypeVar: any concrete backward type refines.
              if (entry_it != fwd[label].end())
                entry_it->second.type = clone(ub_type);
              if (exit_it != fwd_exit[label].end())
                exit_it->second.type = clone(ub_type);
              enqueue(label, Direction::Both);
            }
          }
        }
      }
    }

    // ===== Finalize (stub for Iteration 0) =====
    // Iteration 4 will implement the full finalization logic.

    void finalize()
    {
      size_t n = cfg.size();
      if (n == 0)
        return;

      // ---- 1. Per-label: Const writes, TypeAssertion removal ----
      for (size_t i = 0; i < n; i++)
      {
        auto body = cfg.labels[i].label / Body;

        // Compute final tuple/array types.
        struct FinalTupleInfo
        {
          size_t size;
          bool is_array_lit;
        };
        std::map<Location, FinalTupleInfo> final_tuple_info;
        std::map<Location, std::pair<Location, size_t>> final_tuple_refs;
        std::map<Location, std::vector<Location>> final_tuple_values;
        std::map<Location, Node> final_newarray_types;

        for (auto& stmt : *body)
        {
          if (stmt == NewArrayConst)
          {
            auto loc = (stmt / LocalId)->location();
            auto size = from_chars_sep_v<size_t>(stmt / Rhs);
            bool is_array_lit =
              loc.view().find("array") != std::string_view::npos;
            final_tuple_info.emplace(loc, FinalTupleInfo{size, is_array_lit});
            final_tuple_values.emplace(loc, std::vector<Location>(size));
          }
          else if (stmt == ArrayRefConst)
          {
            auto arg_loc = ((stmt / Arg) / Rhs)->location();
            auto info = final_tuple_info.find(arg_loc);
            if (info == final_tuple_info.end())
              continue;
            auto index = from_chars_sep_v<size_t>(stmt / Rhs);
            if (index < info->second.size)
              final_tuple_refs[(stmt / LocalId)->location()] = {
                arg_loc, index};
          }
          else if (stmt == Store)
          {
            auto ref_it = final_tuple_refs.find((stmt / Rhs)->location());
            if (ref_it == final_tuple_refs.end())
              continue;
            auto& [tuple_loc, index] = ref_it->second;
            auto values_it = final_tuple_values.find(tuple_loc);
            if (
              values_it != final_tuple_values.end() &&
              index < values_it->second.size())
              values_it->second[index] = ((stmt / Arg) / Rhs)->location();
          }
        }

        for (auto& [tuple_loc, info] : final_tuple_info)
        {
          auto values_it = final_tuple_values.find(tuple_loc);
          if (values_it == final_tuple_values.end())
            continue;

          if (info.is_array_lit)
          {
            Node common;
            bool uniform = true;
            for (auto& value_loc : values_it->second)
            {
              if (value_loc.view().empty())
              {
                uniform = false;
                break;
              }
              auto env_it = fwd_exit[i].find(value_loc);
              if (env_it == fwd_exit[i].end())
              {
                uniform = false;
                break;
              }
              auto prim = extract_primitive(env_it->second.type);
              if (!prim)
              {
                uniform = false;
                break;
              }
              if (!common)
                common = clone(env_it->second.type);
              else if (
                env_it->second.type->front()->type() !=
                common->front()->type())
              {
                uniform = false;
                break;
              }
            }
            if (uniform && common && info.size > 0)
              final_newarray_types[tuple_loc] = clone(common);
            continue;
          }

          bool complete = true;
          std::vector<Node> elems;
          elems.reserve(info.size);
          for (auto& value_loc : values_it->second)
          {
            if (value_loc.view().empty())
            {
              complete = false;
              break;
            }
            auto env_it = fwd_exit[i].find(value_loc);
            if (env_it == fwd_exit[i].end())
            {
              complete = false;
              break;
            }
            elems.push_back(clone(env_it->second.type));
          }

          if (!complete || elems.empty())
            continue;

          if (elems.size() == 1)
            final_newarray_types[tuple_loc] = Type
              << clone(elems.front()->front());
          else
          {
            Node tup = TupleType;
            for (auto& elem : elems)
              tup << clone(elem->front());
            final_newarray_types[tuple_loc] = Type << tup;
          }
        }

        // Walk body: write Const types, remove TypeAssertions.
        auto it = body->begin();
        while (it != body->end())
        {
          if (*it == TypeAssertion)
          {
            it = body->erase(it, std::next(it));
            continue;
          }

          if (*it == Const)
          {
            auto dst = (*it)->front();
            auto loc = dst->location();

            Node final_type;
            auto env_it = fwd_exit[i].find(loc);
            if (
              env_it != fwd_exit[i].end() &&
              !is_angelic(env_it->second.type))
              final_type = env_it->second.type;

            if (!final_type)
            {
              auto ub_it = ub[i].find(loc);
              if (
                ub_it != ub[i].end() &&
                !is_angelic(ub_it->second.type))
                final_type = ub_it->second.type;
            }

            if (
              !final_type && env_it != fwd_exit[i].end() &&
              is_angelic(env_it->second.type))
            {
              auto [frange_begin, frange_end] =
                cfg.func_label_range[cfg.labels[i].function];
              for (size_t j = frange_begin; j < frange_end; j++)
              {
                auto ub_it = ub[j].find(loc);
                if (
                  ub_it != ub[j].end() &&
                  !is_angelic(ub_it->second.type))
                {
                  auto prim = extract_primitive(ub_it->second.type);
                  if (prim &&
                      is_angelic_compatible(env_it->second.type, prim))
                  {
                    final_type = ub_it->second.type;
                    break;
                  }
                }
              }
            }

            Node final_prim;
            if (final_type)
              final_prim = extract_primitive(final_type);

            if (final_prim)
            {
              if ((*it)->size() == 3)
              {
                auto old_type = (*it)->at(1);
                if (old_type->type() != final_prim->type())
                  (*it)->replace(old_type, final_prim->type());
              }
              else if ((*it)->size() == 2)
              {
                auto lit = (*it)->back();
                (*it)->erase((*it)->begin(), (*it)->end());
                *it << dst << final_prim->type() << lit;
              }
            }
            else if ((*it)->size() == 2)
            {
              auto lit = (*it)->back();
              auto type_tok = literal_fallback_token(lit);
              (*it)->erase((*it)->begin(), (*it)->end());
              *it << dst << type_tok << lit;
            }
          }
          else if (*it == NewArrayConst)
          {
            auto nloc = ((*it) / LocalId)->location();
            auto final_it = final_newarray_types.find(nloc);
            if (final_it != final_newarray_types.end())
              (*it)->replace((*it) / Type, clone(final_it->second));

            auto env_it = fwd_exit[i].find(nloc);
            if (env_it != fwd_exit[i].end())
            {
              auto inner = env_it->second.type->front();
              if (inner == TupleType)
                (*it)->replace((*it) / Type, clone(env_it->second.type));
              else
              {
                auto prim = extract_primitive(env_it->second.type);
                if (
                  prim &&
                  !Subtype.invariant(
                    top, (*it) / Type, env_it->second.type))
                  (*it)->replace(
                    (*it) / Type, clone(env_it->second.type));
              }
            }
          }

          ++it;
        }
      }

      // ---- 3. Update params and fields from fwd_exit envs ----
      for (auto& [func, range] : cfg.func_label_range)
      {
        auto parent_cls = func->parent(ClassDef);

        for (auto& pd : *(func / Params))
        {
          auto type = pd / Type;
          if (type->front() != TypeVar)
            continue;
          auto ident = pd / Ident;
          bool found = false;
          for (size_t i = range.first; i < range.second && !found; i++)
          {
            auto it = fwd_exit[i].find(ident->location());
            if (it != fwd_exit[i].end() && it->second.type->front() != TypeVar)
            {
              pd->replace(type, clone(it->second.type));
              if (parent_cls)
              {
                for (auto& child : *(parent_cls / ClassBody))
                {
                  if (child != FieldDef)
                    continue;
                  if (
                    (child / Ident)->location().view() !=
                    ident->location().view())
                    continue;
                  if (contains_typevar(child / Type))
                    child->replace(child / Type, clone(it->second.type));
                  break;
                }
              }
              found = true;
            }
          }
          if (!found)
          {
            auto entry_it = func_entry.find(func);
            if (entry_it != func_entry.end())
            {
              auto it = fwd[entry_it->second].find(ident->location());
              if (
                it != fwd[entry_it->second].end() &&
                it->second.type->front() != TypeVar)
              {
                pd->replace(type, clone(it->second.type));
                if (parent_cls)
                {
                  for (auto& child : *(parent_cls / ClassBody))
                  {
                    if (child != FieldDef)
                      continue;
                    if (
                      (child / Ident)->location().view() !=
                      ident->location().view())
                      continue;
                    if (contains_typevar(child / Type))
                      child->replace(child / Type, clone(it->second.type));
                    break;
                  }
                }
              }
            }
          }
        }

        // Update FieldDef types still at TypeVar.
        if (parent_cls)
        {
          for (auto& child : *(parent_cls / ClassBody))
          {
            if (child != FieldDef || (child / Type)->front() != TypeVar)
              continue;
            auto fname = (child / Ident)->location();
            for (size_t i = range.first; i < range.second; i++)
            {
              auto it = fwd_exit[i].find(fname);
              if (
                it != fwd_exit[i].end() &&
                it->second.type->front() != TypeVar)
              {
                child->replace(child / Type, clone(it->second.type));
                break;
              }
            }
          }
        }

        // ---- 4. Return type inference ----
        bool in_generic = (func / TypeParams)->size() > 0 ||
          (func->parent({ClassDef}) != nullptr &&
           (func->parent({ClassDef}) / TypeParams)->size() > 0);

        auto func_ret = func / Type;
        if (func_ret->front() == TypeVar)
        {
          SequentCtx ctx{top, {}, {}};
          Nodes ret_types;
          bool unresolved = false;

          if (in_generic)
          {
            for (size_t i = range.first; i < range.second; i++)
            {
              auto lbl_body = cfg.labels[i].label / Body;
              for (auto& stmt : *lbl_body)
                if (stmt->in({CallDyn, TryCallDyn}))
                {
                  auto loc = (stmt / LocalId)->location();
                  bool f = false;
                  for (size_t j = range.first; j < range.second; j++)
                    if (fwd_exit[j].find(loc) != fwd_exit[j].end())
                    {
                      f = true;
                      break;
                    }
                  if (!f)
                    unresolved = true;
                }
            }
          }

          auto ret_it = func_returns.find(func);
          if (ret_it != func_returns.end())
          {
            for (auto idx : ret_it->second)
            {
              auto term = cfg.labels[idx].label / Return;
              if (term != Return)
                continue;
              auto ret_loc = (term / LocalId)->location();
              auto eit = fwd_exit[idx].find(ret_loc);
              if (
                eit == fwd_exit[idx].end() ||
                eit->second.type->front() == TypeVar)
              {
                if (in_generic)
                  unresolved = true;
                continue;
              }
              bool covered = false;
              for (auto& rt : ret_types)
                if (Subtype(ctx, eit->second.type, rt))
                {
                  covered = true;
                  break;
                }
              if (!covered)
                ret_types.push_back(clone(eit->second.type));
            }
          }

          if (!unresolved)
          {
            if (ret_types.size() == 1)
              func->replace(func_ret, ret_types.front());
            else if (ret_types.size() > 1)
            {
              Node u = Union;
              for (auto& rt : ret_types)
                u << clone(rt->front());
              func->replace(func_ret, Type << u);
            }
            else
            {
              bool all_nonlocal = true;
              for (size_t i = range.first; i < range.second; i++)
              {
                auto term = cfg.labels[i].label / Return;
                if (term->in({Jump, Cond}))
                  continue;
                if (term != Raise)
                {
                  all_nonlocal = false;
                  break;
                }
              }
              if (all_nonlocal)
                func->replace(
                  func_ret,
                  Type
                    << (TypeName
                        << (NameElement << (Ident ^ "_builtin") << TypeArgs)
                        << (NameElement << (Ident ^ "none") << TypeArgs)));
            }
          }
        }

        // ---- 5. Error checking ----
        if (!in_generic)
        {
          bool param_error = false;
          for (auto& pd : *(func / Params))
          {
            if ((pd / Type)->front() == TypeVar)
            {
              func->parent()->replace(
                func,
                err(pd / Ident, "Cannot infer type of parameter"));
              param_error = true;
              break;
            }
          }
          if (!param_error)
          {
            func_ret = func / Type;
            if (func_ret->front() == TypeVar)
            {
              func->parent()->replace(
                func,
                err(func / Ident, "Cannot infer return type of function"));
            }
          }
        }
      }
    }

    void run()
    {
      MethodCacheGuard cache_guard(&method_cache_storage);
      build();
      solve();
      finalize();
    }
  };

  // ===== is_lambda_function =====

  bool is_lambda_function(const Node& func)
  {
    auto class_def = func->parent(ClassBody);
    if (!class_def)
      return false;
    class_def = class_def->parent(ClassDef);
    if (!class_def)
      return false;

    auto ident = class_def / Ident;
    auto name = ident->location().view();
    return name.rfind("lambda$", 0) == 0;
  }

  // ===== Pass definition =====

  PassDef infer()
  {
    PassDef p{"infer", wfPassInfer, dir::once, {}};

    p.post([](auto top) {
      GlobalInfer gi;
      gi.top = top;
      gi.run();

      // Sweep: resolve remaining AngelicSubtype nodes to concrete
      // defaults (u64 for IntSet, f64 for FloatSet).
      top->traverse([](Node& node) {
        if (node == AngelicSubtype)
        {
          auto parent = node->parent();
          if (parent && !node->empty())
          {
            auto bound = node->front();
            if (bound == Isect)
            {
              bool is_int = false, is_float = false;
              for (auto& child : *bound)
              {
                if (child == IntSet)
                  is_int = true;
                if (child == FloatSet)
                  is_float = true;
              }
              if (is_int)
                parent->replace(node, primitive_type(U64)->front());
              else if (is_float)
                parent->replace(node, primitive_type(F64)->front());
              else
                parent->replace(node, clone(bound));
            }
            else
              parent->replace(node, clone(bound));
          }
          return false;
        }
        return true;
      });

      return 0;
    });

    return p;
  }
}
