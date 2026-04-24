// ===== Forward-Only Type Inference with Constraint Variables =====
//
// Architecture (FORWARD-CONSTRAINT-PLAN.md):
//
// A single forward analysis where type variables carry constraints.
// "Backward" information flows naturally when a type variable reaches
// a typed context and acquires a constraint.
//
// Each unresolved type (untyped literal, type parameter) gets a fresh
// TypeVarId stored in an AngelicSubtype node. The constraint store
// tracks upper/lower bounds. When bounds determine a unique type,
// the variable resolves and observers (labels) are re-enqueued.
//
// The framework (AbstractInterpreter) handles CFG propagation.
// The domain (InferDomain) handles type-level operations.

#include "../lang.h"
#include "../subtype.h"
#include "abstract_interp.h"
#include "constraint_store.h"

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

  // ===== Forward Join — Split Approach =====
  //
  // Decomposes types into (angelic, concrete) parts.
  // Angelic part: set union by TypeVarId, bound widening for same ID.
  // Concrete part: standard Union with Subtype absorption.
  // Subtype never sees AngelicSubtype nodes.
  //
  // Returns the joined type, or {} if no change from existing.
  static Node join_type(const Node& existing, const Node& incoming, Node top)
  {
    // Bottom absorbs.
    if (!existing || existing->empty() || existing->front() == TypeVar)
    {
      if (incoming && !incoming->empty() && incoming->front() == TypeVar)
        return {};
      return clone(incoming);
    }
    if (!incoming || incoming->empty() || incoming->front() == TypeVar)
      return {};

    // Fast paths.
    if (existing == incoming)
      return {};
    if (same_type_tree(existing, incoming))
      return {};

    // --- Split both sides into angelic and concrete parts ---

    struct SplitResult
    {
      std::vector<TypeVarId> ids;
      Nodes angelic_nodes; // AngelicSubtype nodes (parallel with ids)
      Nodes concrete;      // Type nodes (non-angelic)
    };

    auto do_split = [](const Node& type) -> SplitResult {
      SplitResult r;
      auto inner = type->front();
      if (inner == AngelicSubtype)
      {
        auto ct = Type << clone(inner);
        auto id = get_angelic_var_id(ct);
        if (id.has_value())
          r.ids.push_back(id.value());
        r.angelic_nodes.push_back(clone(inner));
      }
      else if (inner == Union)
      {
        for (auto& child : *inner)
        {
          if (child == AngelicSubtype)
          {
            auto ct = Type << clone(child);
            auto id = get_angelic_var_id(ct);
            if (id.has_value())
              r.ids.push_back(id.value());
            r.angelic_nodes.push_back(clone(child));
          }
          else
          {
            r.concrete.push_back(Type << clone(child));
          }
        }
      }
      else
      {
        r.concrete.push_back(clone(type));
      }
      return r;
    };

    auto ex = do_split(existing);
    auto in = do_split(incoming);

    // --- Join angelic part: set union by TypeVarId, bound widening ---
    auto result_ids = ex.ids;
    auto result_angelic = std::move(ex.angelic_nodes);
    for (size_t i = 0; i < in.ids.size(); i++)
    {
      bool found = false;
      for (size_t j = 0; j < result_ids.size(); j++)
      {
        if (result_ids[j] == in.ids[i])
        {
          found = true;
          // Same ID — widen bounds if different.
          if (!same_type_tree(
                Type << clone(result_angelic[j]),
                Type << clone(in.angelic_nodes[i])))
          {
            auto ex_bound = result_angelic[j]->front();
            auto in_bound = in.angelic_nodes[i]->front();
            auto ex_mems = members(ex_bound);
            auto in_mems = members(in_bound);
            std::vector<Token> widened = ex_mems;
            for (auto& m : in_mems)
            {
              bool dup = false;
              for (auto& w : widened)
                if (w == m) { dup = true; break; }
              if (!dup)
                widened.push_back(m);
            }
            bool is_c = has_concrete(ex_bound) || has_concrete(in_bound);
            auto nb = make_angelic_bound(widened, is_c);
            result_angelic[j] =
              (AngelicSubtype ^ std::to_string(in.ids[i])) << nb;
          }
          break;
        }
      }
      if (!found)
      {
        result_ids.push_back(in.ids[i]);
        result_angelic.push_back(clone(in.angelic_nodes[i]));
      }
    }

    // --- Join concrete part: Subtype-aware union ---
    auto result_concrete = std::move(ex.concrete);
    if (!in.concrete.empty())
    {
      // Self type handling.
      bool ex_self = false, in_self = false;
      for (auto& c : result_concrete)
        if (contains_self_type(c)) ex_self = true;
      for (auto& c : in.concrete)
        if (contains_self_type(c)) in_self = true;
      if (ex_self != in_self)
      {
        if (ex_self)
          result_concrete = std::move(in.concrete);
      }
      else if (!result_concrete.empty())
      {
        SequentCtx ctx{top, {}, {}};
        for (auto& inc : in.concrete)
        {
          bool covered = false;
          for (auto& rc : result_concrete)
          {
            if (same_type_tree(rc, inc) || Subtype(ctx, inc, rc))
            { covered = true; break; }
          }
          if (!covered)
          {
            auto it = result_concrete.begin();
            while (it != result_concrete.end())
            {
              if (Subtype(ctx, *it, inc))
                it = result_concrete.erase(it);
              else
                ++it;
            }
            result_concrete.push_back(clone(inc));
          }
        }
      }
      else
      {
        result_concrete = std::move(in.concrete);
      }
    }

    // --- Recombine ---
    size_t total = result_angelic.size() + result_concrete.size();
    if (total == 0)
      return {};

    Node result;
    if (total == 1)
    {
      if (!result_angelic.empty())
        result = Type << result_angelic[0];
      else
        result = result_concrete[0];
    }
    else
    {
      Node u = Union;
      for (auto& a : result_angelic)
        u << a;
      for (auto& c : result_concrete)
        u << clone(c->front());
      result = Type << u;
    }

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

  struct MethodCache
  {
    Node top;

    Node resolve_class_method(
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

      auto it = cache_.find(key);
      if (it != cache_.end())
        return it->second;

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

      cache_[std::move(key)] = func;
      return func;
    }

    MethodInfo resolve(
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
        auto func = resolve_class_method(
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
          auto info =
            resolve(member_type, method_ident, hand, arity, method_typeargs);
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
        return resolve(
          ref_inner, method_ident, hand, arity, method_typeargs);
      return {};
    }

    Node resolve_return_type(
      const Node& receiver_type,
      const Node& method_ident,
      Token hand,
      size_t arity,
      const Node& method_typeargs)
    {
      auto info =
        resolve(receiver_type, method_ident, hand, arity, method_typeargs);
      if (!info.func)
        return {};
      auto ret = apply_subst(top, info.func / Type, info.subst);

      if (ret && ret->front() == TypeVar && hand == Rhs)
      {
        auto lhs_info =
          resolve(receiver_type, method_ident, Lhs, arity, method_typeargs);
        if (lhs_info.func)
        {
          auto lhs_ret =
            apply_subst(top, lhs_info.func / Type, lhs_info.subst);
          auto inner = extract_ref_inner(lhs_ret);
          if (inner)
            return inner;
        }
      }
      return ret;
    }

    MethodInfo resolve_callable(
      const Node& receiver_type,
      const Node& method_ident,
      Token hand,
      size_t arity,
      const Node& method_typeargs)
    {
      auto info =
        resolve(receiver_type, method_ident, hand, arity, method_typeargs);
      if (!info.func && hand == Rhs)
        info =
          resolve(receiver_type, method_ident, Lhs, arity, method_typeargs);
      return info;
    }

  private:
    std::map<
      std::tuple<std::string, std::string, std::string, size_t>,
      Node>
      cache_;
  };

  // ===== Shape propagation helpers =====

  static bool replace_if_changed(
    const Node& owner, const Node& old_child, const Node& new_child)
  {
    if (same_type_tree(old_child, new_child))
      return false;
    owner->replace(old_child, new_child);
    return true;
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

  // ===== InferDomain =====
  //
  // Domain for the forward-only AbstractInterpreter.
  // Manages type environments, constraint store, and transfer functions.

  struct InferDomain
  {
    using Env = TypeEnv;

    Node top;

    // Constraint store for type variable tracking.
    ConstraintStore constraints;

    // Stable TypeVarId per statement — ensures re-runs produce the
    // same variable, not fresh ones.
    std::map<const void*, TypeVarId> stmt_var_ids;

    // Forward pass shared state (reset per-label).
    std::map<Location, Node> lookup_stmts;
    std::map<Location, std::pair<Location, size_t>> ref_to_tuple;
    std::map<Location, TupleTracking> tuple_locals;

    // Method resolution cache.
    MethodCache method_cache;

    // Lambda return tracking (moved from file-static).
    std::unordered_set<const void*> lambda_returns_omitted;

    // Cross-function return type constraint variables.
    // For each function with TypeVar return, a constraint variable
    // tracks the inferred return type. Callers observe this variable;
    // return labels tighten it.
    std::map<const void*, TypeVarId> func_return_var;

    // Field type constraint variables: for each lambda FieldDef with
    // TypeVar type, a constraint variable tracks the refined type.
    // FieldRef reads from this; finalize writes to the AST.
    std::map<std::pair<const void*, std::string>, TypeVarId> field_var;

    // ===== Domain concept implementation =====

    Env empty_env() const { return {}; }

    Env clone_env(const Env& env) const
    {
      Env result;
      for (auto& [loc, info] : env)
        result[loc] = {clone(info.type), info.call_node};
      return result;
    }

    bool join(Env& target, const Env& incoming)
    {
      bool changed = false;
      for (auto& [loc, info] : incoming)
      {
        auto it = target.find(loc);
        if (it == target.end())
        {
          target[loc] = {clone(info.type), info.call_node};
          changed = true;
        }
        else
        {
          auto joined = join_type(it->second.type, info.type, top);
          if (joined)
          {
            it->second.type = joined;
            if (info.call_node && !it->second.call_node)
              it->second.call_node = info.call_node;
            changed = true;
          }
        }
      }
      return changed;
    }

    // ===== Cross-function push =====
    //
    // These use the AbstractInterpreter reference stored during
    // forward_transfer to push types across function boundaries.

    AbstractInterpreter<InferDomain>* ai_ = nullptr;

    // RAII guard for ai_ pointer — ensures cleanup on early return.
    struct AiGuard
    {
      AbstractInterpreter<InferDomain>*& ptr;
      AiGuard(AbstractInterpreter<InferDomain>*& p,
              AbstractInterpreter<InferDomain>& ai) : ptr(p) { ptr = &ai; }
      ~AiGuard() { ptr = nullptr; }
      AiGuard(const AiGuard&) = delete;
      AiGuard& operator=(const AiGuard&) = delete;
    };

    bool push_param_type(
      const Node& func_def,
      const Location& param_loc,
      const Node& type)
    {
      if (!ai_)
        return false;
      auto entry_it = ai_->func_entry().find(func_def);
      if (entry_it == ai_->func_entry().end())
        return false;
      Env param_env;
      param_env[param_loc] = {clone(type), {}};
      return ai_->push_fwd(entry_it->second, param_env);
    }

    bool push_return_constraint(
      const Node& func_def,
      const Node& type)
    {
      // TODO: Implement shape return type propagation.
      snmalloc::UNUSED(func_def, type);
      return false;
    }

    void push_args_to_callee(
      const Node& func_def,
      const Node& args,
      TypeEnv& caller_env)
    {
      if (!ai_)
        return;
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
              (is_lambda_function(af) &&
               lambda_returns_omitted.count(af.get()) > 0))
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

    // ===== Call TypeArg refinement =====
    //
    // When G[V] <: G[E] and V is a concrete class implementing
    // shape E, refine the Call that produced G[V] to produce G[E].
    // This fixes premature concretisation of generic TypeArgs.
    void refine_call_typeargs(
      const LocalTypeInfo& value_info,
      const Node& expected_type,
      const EnqueueCallback& enqueue_cb)
    {
      snmalloc::UNUSED(enqueue_cb);

      auto& value_type = value_info.type;
      if (!value_type || !expected_type)
        return;
      if (value_type == Type && expected_type == Type &&
          !value_type->empty() && !expected_type->empty() &&
          value_type->front() == TypeName &&
          expected_type->front() == TypeName)
      {
        auto v_tn = value_type->front();
        auto e_tn = expected_type->front();

        if (v_tn->size() != e_tn->size() || v_tn->size() < 1)
          return;

        // Find mismatched TypeArgs where the value is a concrete
        // class implementing the expected shape.
        for (size_t i = 0; i < v_tn->size(); i++)
        {
          auto v_ne = v_tn->at(i);
          auto e_ne = e_tn->at(i);
          if (v_ne != NameElement || e_ne != NameElement)
            return;
          if ((v_ne / Ident)->location().view() !=
              (e_ne / Ident)->location().view())
            return;

          auto v_ta = v_ne / TypeArgs;
          auto e_ta = e_ne / TypeArgs;
          if (v_ta->size() != e_ta->size())
            return;

          for (size_t j = 0; j < v_ta->size(); j++)
          {
            if (same_type_tree(v_ta->at(j), e_ta->at(j)))
              continue;

            // Check shape subtyping: V_j implements E_j's shape.
            auto v_inner = v_ta->at(j)->front();
            auto e_inner = e_ta->at(j)->front();
            if (v_inner != TypeName || e_inner != TypeName)
              continue;
            auto v_def = find_def(top, v_inner);
            auto e_def = find_def(top, e_inner);
            if (!v_def || !e_def)
              continue;
            if (v_def != ClassDef || e_def != ClassDef)
              continue;
            if ((e_def / Shape) != Shape)
              continue;

            // V implements shape E — find the Call and rewrite
            // its TypeArgs.
            auto call_node = value_info.call_node;
            if (!call_node)
              continue;

            // The Call's FuncName contains TypeArgs to update.
            Node funcname;
            if (call_node == Call)
              funcname = call_node / FuncName;
            else
              continue;

            // Find the NameElement in FuncName that has TypeArgs
            // containing the mismatched type.
            for (auto& ne : *funcname)
            {
              if (ne != NameElement)
                continue;
              auto ta = ne / TypeArgs;
              for (size_t k = 0; k < ta->size(); k++)
              {
                if (same_type_tree(ta->at(k), v_ta->at(j)))
                {
                  // Replace V with E in the Call's TypeArgs.
                  ta->replace(ta->at(k), clone(e_ta->at(j)));
                }
              }
            }
          }
        }
      }
    }

    // ===== Constraint decomposition =====
    //
    // Adds upper bound constraints to all type variables in a type.
    // Handles: direct TypeVarId, Union containing Angelics.
    // No-op for concrete types without Angelics.
    void constrain_type(
      const Node& value_type,
      const Node& expected_type,
      const EnqueueCallback& enqueue_cb)
    {
      if (!value_type || !expected_type)
        return;
      if (is_uninformative_backward_type(expected_type))
        return;
      if (contains_typevar(expected_type))
        return;

      // Direct TypeVarId.
      auto var_id = get_angelic_var_id(value_type);
      if (var_id.has_value())
      {
        constraints.add_upper_bound(
          var_id.value(), expected_type, enqueue_cb);
        return;
      }

      // Union containing Angelics: decompose.
      // Union('a, 'b) <: T → 'a <: T AND 'b <: T.
      if (value_type == Type && !value_type->empty() &&
          value_type->front() == Union)
      {
        for (auto& component : *(value_type->front()))
        {
          if (component == AngelicSubtype)
          {
            auto comp_type = Type << clone(component);
            auto comp_id = get_angelic_var_id(comp_type);
            if (comp_id.has_value())
              constraints.add_upper_bound(
                comp_id.value(), expected_type, enqueue_cb);
          }
        }
      }

      // Generic decomposition:
      // G[V1,...,Vn] <: G[E1,...,En] → Vi <: Ei for each i.
      // Only when both are TypeName with same class identity.
      if (value_type == Type && expected_type == Type &&
          !value_type->empty() && !expected_type->empty() &&
          value_type->front() == TypeName &&
          expected_type->front() == TypeName)
      {
        auto v_tn = value_type->front();
        auto e_tn = expected_type->front();

        // Match class identity: same number of NameElements,
        // same Ident at each position.
        if (v_tn->size() == e_tn->size() && v_tn->size() >= 1)
        {
          bool same_class = true;
          for (size_t i = 0; i < v_tn->size(); i++)
          {
            auto v_ne = v_tn->at(i);
            auto e_ne = e_tn->at(i);
            if (v_ne != NameElement || e_ne != NameElement)
            { same_class = false; break; }
            auto v_id = (v_ne / Ident)->location().view();
            auto e_id = (e_ne / Ident)->location().view();
            if (v_id != e_id)
            { same_class = false; break; }
          }

          if (same_class)
          {
            // Extract TypeArgs from each NameElement and
            // constrain pairwise.
            for (size_t i = 0; i < v_tn->size(); i++)
            {
              auto v_ta = v_tn->at(i) / TypeArgs;
              auto e_ta = e_tn->at(i) / TypeArgs;
              for (size_t j = 0;
                   j < v_ta->size() && j < e_ta->size();
                   j++)
              {
                constrain_type(
                  v_ta->at(j), e_ta->at(j), enqueue_cb);
              }
            }
          }
        }
      }
    }

    // ===== Type splitting =====
    //
    // Extract the concrete part of a type, stripping all angelic
    // components. Returns {} if the type is purely angelic.
    // This is the fundamental operation for handlers that need to
    // dispatch on concrete structure.
    static Node extract_concrete_part(const Node& type)
    {
      if (!type || type != Type || type->empty())
        return {};
      auto inner = type->front();
      if (inner == AngelicSubtype)
        return {}; // Purely angelic.
      if (inner != Union)
        return clone(type); // Purely concrete.
      // Union — extract non-angelic components.
      Nodes concrete;
      for (auto& child : *inner)
        if (child != AngelicSubtype)
          concrete.push_back(clone(child));
      if (concrete.empty())
        return {};
      if (concrete.size() == 1)
        return Type << concrete[0];
      Node u = Union;
      for (auto& c : concrete)
        u << c;
      return Type << u;
    }

    // ===== Cross-function return type inference =====
    //
// When a callee's declared return type is TypeVar, get or create
    // a constraint variable for the callee's inferred return type.
    // The caller observes this variable. Return labels tighten it.

    TypeVarId get_func_return_var(Node callee_func)
    {
      auto key = callee_func.get();
      auto it = func_return_var.find(key);
      if (it != func_return_var.end())
        return it->second;

      // Create a non-concrete variable with no member set.
      auto id = constraints.fresh(false, {});
      func_return_var[key] = id;
      return id;
    }

    // Get or create a constraint variable for a lambda field.
    TypeVarId get_field_var(
      const void* class_ptr, const std::string& field_name)
    {
      auto key = std::make_pair(class_ptr, field_name);
      auto it = field_var.find(key);
      if (it != field_var.end())
        return it->second;
      auto id = constraints.fresh(false, {});
      field_var[key] = id;
      return id;
    }

    // Read the callee's inferred return type from its constraint variable.
    // Register the caller as an observer. Returns {} if no info yet.
    Node infer_callee_return(
      Node callee_func,
      size_t caller_label)
    {
      auto id = get_func_return_var(callee_func);
      constraints.add_observer(id, caller_label);

      auto& ms = constraints.member_set(id);
      if (ms.size() == 1)
        return primitive_type(ms[0]);

      // If upper bounds contain angelics, return them directly.
      auto& ubs = constraints.upper_bounds(id);
      if (ubs.size() == 1)
        return clone(ubs[0]);

      // Multiple upper bounds — join them.
      if (ubs.size() > 1)
      {
        Node result = clone(ubs[0]);
        for (size_t i = 1; i < ubs.size(); i++)
        {
          auto joined = join_type(result, ubs[i], top);
          if (joined)
            result = joined;
        }
        return result;
      }

      return {};
    }

    // Called from return labels: tighten the function's return
    // constraint variable with the current fwd_exit return type.
    void tighten_func_return(
      const Node& func,
      const Node& ret_type,
      const EnqueueCallback& enqueue_cb)
    {
      if (!ret_type || ret_type->empty() || ret_type->front() == TypeVar)
        return;

      // Only relevant for functions with TypeVar declared return.
      auto func_ret = func / Type;
      if (!contains_typevar(func_ret))
        return;

      auto id = get_func_return_var(func);
      constraints.add_upper_bound(id, ret_type, enqueue_cb);
    }

    // ===== Forward Transfer Function =====

    void forward_transfer(
      Env& env,
      const Node& body,
      const Node& func,
      size_t label_idx,
      AbstractInterpreter<InferDomain>& ai)
    {
      snmalloc::UNUSED(ai);

      // Clear per-label state.
      tuple_locals.clear();
      ref_to_tuple.clear();

      // Enqueue callback for constraint resolution notifications.
      auto enqueue_cb = [&](size_t lbl) { ai.enqueue(lbl); };

      auto merge = [&](const Location& loc, const Node& type,
                       Node call_node = {}) -> bool {
        auto it = env.find(loc);
        if (it == env.end())
        {
          env[loc] = {clone(type), call_node};
          return true;
        }
        auto joined = join_type(it->second.type, type, top);
        if (!joined)
          return false;
        it->second.type = joined;
        if (call_node && !it->second.call_node)
          it->second.call_node = call_node;
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
            // Untyped literal — reuse existing constraint variable
            // or create a fresh one.
            auto lit = stmt->back();
              if (lit->in({Bin, Oct, Int, Hex, Char}))
              {
                auto sit = stmt_var_ids.find(stmt.get());
                TypeVarId id;
                if (sit != stmt_var_ids.end())
                  id = sit->second;
                else
                {
                  id = constraints.fresh(
                    true, std::vector<Token>(intset_members()));
                  stmt_var_ids[stmt.get()] = id;
                }
                auto& cur = constraints.member_set(id);
                if (cur.size() == 1)
                  type = primitive_type(cur[0]);
                else
                {
                  type = make_angelic_var(id, true, cur);
                  constraints.add_observer(id, label_idx);
                }
              }
              else if (lit->in({Float, HexFloat}))
              {
                auto sit = stmt_var_ids.find(stmt.get());
                TypeVarId id;
                if (sit != stmt_var_ids.end())
                  id = sit->second;
                else
                {
                  id = constraints.fresh(
                    true, std::vector<Token>(floatset_members()));
                  stmt_var_ids[stmt.get()] = id;
                }
                auto& cur = constraints.member_set(id);
                if (cur.size() == 1)
                  type = primitive_type(cur[0]);
                else
                {
                  type = make_angelic_var(id, true, cur);
                  constraints.add_observer(id, label_idx);
                }
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
          {
            merge(dst_loc, src_it->second.type, src_it->second.call_node);

            // If source contains type variables and destination already
            // has a concrete type, constrain the source.
            if (is_angelic(src_it->second.type) ||
                contains_angelic(src_it->second.type))
            {
              auto dst_it = env.find(dst_loc);
              if (dst_it != env.end() &&
                  !is_angelic(dst_it->second.type) &&
                  dst_it->second.type->front() != TypeVar)
              {
                constrain_type(
                  src_it->second.type, dst_it->second.type, enqueue_cb);
              }
            }
          }
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
                  // Check field constraint variable first, then AST.
                  auto fv_key = std::make_pair(
                    class_def.get(), std::string(fname));
                  auto fv_it = field_var.find(fv_key);
                  Node ft;
                  if (fv_it != field_var.end())
                  {
                    auto& ms = constraints.member_set(fv_it->second);
                    if (ms.size() == 1)
                      ft = primitive_type(ms[0]);
                    else
                    {
                      // Check upper bounds for non-primitive types.
                      auto& ubs = constraints.upper_bounds(fv_it->second);
                      if (ubs.size() >= 1)
                      {
                        // Use the last upper bound (most refined).
                        ft = clone(ubs.back());
                      }
                    }
                    // Register current label as observer so we
                    // re-process when the field type refines.
                    constraints.add_observer(fv_it->second, label_idx);
                  }
                  if (!ft)
                    ft = apply_subst(top, f / Type, subst);
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

              // Constrain Angelic stored value from field type.
              auto val_it2 = env.find(val_loc);
              if (val_it2 != env.end() && !is_any_type(inner))
              {
                constrain_type(
                  val_it2->second.type, inner, enqueue_cb);

                // Refine Call TypeArgs when stored value's generic
                // TypeArgs differ from the ref's inner type.
                refine_call_typeargs(
                  val_it2->second, inner, enqueue_cb);
              }

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
          auto loc = (stmt / LocalId)->location();
          auto ta_type = stmt / Type;
          if (contains_typevar(ta_type))
          {
            // TypeVar assertions are initial seeds — use merge
            // so they don't overwrite refined types.
            merge(loc, clone(ta_type));
          }
          else
          {
            // Concrete assertions are hard constraints.
            env[loc] = {clone(ta_type), {}};
          }
        }
        else if (stmt->in({New, Stack}))
        {
          merge((stmt / LocalId)->location(), clone(stmt / Type));

          // Constrain Angelic NewArg values from field types.
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
                auto arg_it = env.find(arg_loc);
                if (arg_it == env.end())
                  continue;
                auto fname = (na / Ident)->location().view();
                for (auto& f : *(class_def / ClassBody))
                {
                  if (f != FieldDef)
                    continue;
                  if ((f / Ident)->location().view() != fname)
                    continue;
                  auto ft = apply_subst(top, f / Type, subst);
                  if (ft && !contains_typevar(ft))
                  {
                    constrain_type(
                      arg_it->second.type, ft, enqueue_cb);

                    // Refine Call TypeArgs when the value's generic
                    // TypeArgs differ from the field's. This handles
                    // G[concrete_A] <: G[shape_B] by rewriting the
                    // Call to produce G[shape_B].
                    refine_call_typeargs(
                      arg_it->second, ft, enqueue_cb);
                  }

                  // Refine field type from NewArg value when
                  // the field has TypeVar or angelic types.
                  // Add upper bound to field constraint variable.
                  if (ft &&
                      (contains_typevar(ft) || contains_angelic(ft)) &&
                      !contains_typevar(arg_it->second.type))
                  {
                    auto fv_id = get_field_var(
                      class_def.get(), std::string(fname));
                    constraints.add_upper_bound(
                      fv_id, arg_it->second.type, enqueue_cb);
                  }
                  break;
                }
              }
            }
          }
        }
        else if (stmt->in(propagate_lhs_ops))
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto lhs_loc = (stmt / Lhs)->location();
          auto rhs_loc = (stmt / Rhs)->location();
          auto lhs_it = env.find(lhs_loc);
          auto rhs_it = env.find(rhs_loc);

          // For homogeneous ops: extract concrete parts for dispatch,
          // constrain angelic parts from the concrete side.
          auto lhs_concrete = lhs_it != env.end()
            ? extract_concrete_part(lhs_it->second.type) : Node{};
          auto rhs_concrete = rhs_it != env.end()
            ? extract_concrete_part(rhs_it->second.type) : Node{};

          // Constrain angelic from concrete on opposite side.
          if (lhs_it != env.end() && rhs_concrete &&
              contains_angelic(lhs_it->second.type))
            constrain_type(
              lhs_it->second.type, rhs_concrete, enqueue_cb);
          if (rhs_it != env.end() && lhs_concrete &&
              contains_angelic(rhs_it->second.type))
            constrain_type(
              rhs_it->second.type, lhs_concrete, enqueue_cb);

          // Result type = concrete part of LHS (or full LHS if no concrete).
          if (lhs_concrete)
            merge(dst_loc, lhs_concrete);
          else if (lhs_it != env.end())
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
          snmalloc::UNUSED(all_angelic);

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
          // If declared return type is TypeVar, try inferred return.
          if (ret && ret->front() == TypeVar)
          {
            auto inferred = infer_callee_return(func_def, label_idx);
            if (inferred)
              ret = inferred;
          }
          if (ret)
            merge(
              (stmt / LocalId)->location(),
              ret,
              stmt);

          for (size_t i = 0; i < params->size() && i < args->size(); i++)
          {
            auto pt = apply_subst(top, params->at(i) / Type, subst);
            if (pt && pt->front() != TypeVar)
            {
              auto arg_loc = (args->at(i) / Rhs)->location();
              auto arg_it = env.find(arg_loc);
              if (arg_it != env.end())
              {
                push_shape_to_lambda(pt, arg_it->second.type);
                constrain_type(
                  arg_it->second.type, pt, enqueue_cb);
                refine_call_typeargs(
                  arg_it->second, pt, enqueue_cb);
              }
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
            auto hand = (stmt / Lhs)->type();
            auto method_ident = lookup_method_name(stmt);
            auto method_ta = stmt / TypeArgs;
            auto arity = from_chars_sep_v<size_t>(stmt / Int);

            // Extract concrete part for method resolution.
            auto concrete_recv = extract_concrete_part(src_it->second.type);
            if (concrete_recv)
            {
              auto ret = method_cache.resolve_return_type(
                concrete_recv, method_ident, hand, arity, method_ta);
              // If return type is TypeVar, try inferred return.
              if (ret && ret->front() == TypeVar)
              {
                auto info = method_cache.resolve_callable(
                  concrete_recv, method_ident, hand, arity, method_ta);
                if (info.func)
                {
                  auto inferred =
                    infer_callee_return(info.func, label_idx);
                  if (inferred)
                    ret = inferred;
                }
              }
              if (ret)
                merge(dst_loc, ret);
            }

            // Handle angelic part: resolve method for each member.
            if (contains_angelic(src_it->second.type))
            {
              // Collect all angelic bounds' members.
              std::vector<Token> all_mems;
              auto collect_angelic_mems = [&](const Node& n) {
                if (n == AngelicSubtype && !n->empty())
                {
                  auto b = n->front();
                  auto m = members(b);
                  for (auto& t : m)
                  {
                    bool dup = false;
                    for (auto& a : all_mems)
                      if (a == t) { dup = true; break; }
                    if (!dup)
                      all_mems.push_back(t);
                  }
                }
              };
              if (is_angelic(src_it->second.type))
                collect_angelic_mems(src_it->second.type->front());
              else if (src_it->second.type->front() == Union)
                for (auto& child : *(src_it->second.type->front()))
                  if (child == AngelicSubtype)
                    collect_angelic_mems(child);

              if (!all_mems.empty())
              {
                Node common_ret;
                bool all_agree = true;
                for (auto& m : all_mems)
                {
                  auto mtype = primitive_type(m);
                  auto ret = method_cache.resolve_return_type(
                    mtype, method_ident, hand, arity, method_ta);
                  if (!ret || ret->front() == TypeVar)
                  { all_agree = false; break; }
                  if (!common_ret)
                    common_ret = ret;
                  else if (!same_type_tree(common_ret, ret))
                  { all_agree = false; break; }
                }

                if (all_agree && common_ret)
                  merge(dst_loc, common_ret);
                else
                {
                  std::vector<Token> ret_prims;
                  for (auto& m : all_mems)
                  {
                    auto mtype = primitive_type(m);
                    auto ret = method_cache.resolve_return_type(
                      mtype, method_ident, hand, arity, method_ta);
                    if (!ret || ret->front() == TypeVar)
                      continue;
                    auto rp = extract_primitive(ret);
                    if (!rp)
                      continue;
                    bool dup = false;
                    for (auto& r : ret_prims)
                      if (r == rp->type()) { dup = true; break; }
                    if (!dup)
                      ret_prims.push_back(rp->type());
                  }
                  if (!ret_prims.empty())
                  {
                    auto sit = stmt_var_ids.find(stmt.get());
                    TypeVarId id;
                    if (sit != stmt_var_ids.end())
                      id = sit->second;
                    else
                    {
                      id = constraints.fresh(
                        true, std::vector<Token>(ret_prims));
                      stmt_var_ids[stmt.get()] = id;
                    }
                    auto& cur = constraints.member_set(id);
                    auto& mems = cur.empty() ? ret_prims : cur;
                    if (mems.size() == 1)
                      merge(dst_loc, primitive_type(mems[0]));
                    else
                    {
                      merge(dst_loc,
                        make_angelic_var(id, true, mems));
                      constraints.add_observer(id, label_idx);
                    }
                  }
                }
              }
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
            auto recv_loc = (lookup_node / Rhs)->location();
            auto recv_it = env.find(recv_loc);

            // Angelic cross-product resolution:
            // recv: Angelic(T1,...,Tn), args: possibly Angelic
            // result: Angelic({ Rij | Ti.method(Sj) → Rij })
            // If recv or an arg is concrete, that dimension has
            // only one value.
            if (
              recv_it != env.end() &&
              contains_angelic(recv_it->second.type))
            {
              auto hand = (lookup_node / Lhs)->type();
              auto method_ident = lookup_method_name(lookup_node);
              auto method_ta = lookup_node / TypeArgs;
              auto arity = from_chars_sep_v<size_t>(lookup_node / Int);

              // Collect all angelic members from receiver.
              std::vector<Token> recv_mems;
              auto collect_mems = [&](const Node& n) {
                if (n == AngelicSubtype && !n->empty())
                  for (auto& m : members(n->front()))
                  {
                    bool dup = false;
                    for (auto& r : recv_mems)
                      if (r == m) { dup = true; break; }
                    if (!dup) recv_mems.push_back(m);
                  }
              };
              if (is_angelic(recv_it->second.type))
                collect_mems(recv_it->second.type->front());
              else if (recv_it->second.type->front() == Union)
                for (auto& child : *(recv_it->second.type->front()))
                  if (child == AngelicSubtype)
                    collect_mems(child);

              // Collect arg member sets.
              std::vector<std::vector<Token>> arg_mem_sets;
              for (auto& arg_node : *args)
              {
                auto arg_it =
                  env.find((arg_node / Rhs)->location());
                std::vector<Token> arg_mems;
                if (arg_it != env.end())
                {
                  if (is_angelic(arg_it->second.type))
                  {
                    arg_mems = members(
                      arg_it->second.type->front()->front());
                  }
                  else
                  {
                    auto p = extract_callable_primitive(
                      arg_it->second.type);
                    if (p)
                      arg_mems.push_back(p->type());
                  }
                }
                arg_mem_sets.push_back(std::move(arg_mems));
              }

              // For each receiver member, resolve the method and
              // check arg compatibility. Collect valid return types.
              std::vector<Token> result_prims;
              Node result_nonprim;
              for (auto& ti : recv_mems)
              {
                auto ti_type = primitive_type(ti);
                auto info = method_cache.resolve_callable(
                  ti_type, method_ident, hand, arity, method_ta);
                if (!info.func)
                  continue;

                auto params = info.func / Params;
                bool args_ok = true;
                for (size_t j = 0;
                     j < params->size() && j < arg_mem_sets.size();
                     j++)
                {
                  auto pt =
                    apply_subst(top, params->at(j) / Type, info.subst);
                  if (!pt || pt->front() == TypeVar)
                    continue;
                  auto pt_prim = extract_primitive(pt);
                  if (!pt_prim)
                    continue;
                  // Check if any arg member matches param type.
                  bool found = false;
                  for (auto& sj : arg_mem_sets[j])
                    if (sj == pt_prim->type())
                    {
                      found = true;
                      break;
                    }
                  if (!found && !arg_mem_sets[j].empty())
                  {
                    args_ok = false;
                    break;
                  }
                }

                if (!args_ok)
                  continue;

                auto ret = apply_subst(
                  top, info.func / Type, info.subst);
                if (!ret || ret->front() == TypeVar)
                  continue;

                auto ret_prim = extract_primitive(ret);
                if (ret_prim)
                {
                  bool dup = false;
                  for (auto& r : result_prims)
                    if (r == ret_prim->type())
                    {
                      dup = true;
                      break;
                    }
                  if (!dup)
                    result_prims.push_back(ret_prim->type());
                }
                else if (!result_nonprim)
                  result_nonprim = ret;
              }

              // Build result type from collected returns.
              if (!result_prims.empty())
              {
                if (result_prims.size() == 1)
                  merge(dst_loc, primitive_type(result_prims[0]));
                else
                {
                  bool concrete = true;
                  auto sit = stmt_var_ids.find(stmt.get());
                  TypeVarId id;
                  if (sit != stmt_var_ids.end())
                    id = sit->second;
                  else
                  {
                    id = constraints.fresh(
                      concrete, std::vector<Token>(result_prims));
                    stmt_var_ids[stmt.get()] = id;
                  }
                  auto& cur = constraints.member_set(id);
                  auto& mems = cur.empty() ? result_prims : cur;
                  if (mems.size() == 1)
                    merge(dst_loc, primitive_type(mems[0]));
                  else
                  {
                    merge(dst_loc,
                      make_angelic_var(id, concrete, mems));
                    constraints.add_observer(id, label_idx);
                  }
                }
              }
              else if (result_nonprim)
                merge(dst_loc, result_nonprim);

              // Reverse constraint: if result is resolved/constrained,
              // filter receiver members and constrain angelic args.
              auto dst_it2 = env.find(dst_loc);
              if (dst_it2 != env.end() &&
                  !contains_angelic(dst_it2->second.type) &&
                  dst_it2->second.type->front() != TypeVar)
              {
                // Result is concrete — filter recv_mems to those
                // whose method returns this type, then constrain args.
                auto result_prim = extract_primitive(dst_it2->second.type);
                std::vector<Token> filtered_recv;
                std::vector<MethodInfo> filtered_info;
                for (auto& ti : recv_mems)
                {
                  auto ti_type = primitive_type(ti);
                  auto info = method_cache.resolve_callable(
                    ti_type, method_ident, hand, arity, method_ta);
                  if (!info.func)
                    continue;
                  auto ret = apply_subst(
                    top, info.func / Type, info.subst);
                  if (!ret || ret->front() == TypeVar)
                    continue;
                  if (result_prim)
                  {
                    auto rp = extract_primitive(ret);
                    if (!rp || rp->type() != result_prim->type())
                      continue;
                  }
                  filtered_recv.push_back(ti);
                  filtered_info.push_back(std::move(info));
                }

                // Constrain receiver angelic.
                if (!filtered_recv.empty() &&
                    filtered_recv.size() < recv_mems.size())
                {
                  auto recv_var =
                    get_angelic_var_id(recv_it->second.type);
                  if (recv_var.has_value())
                  {
                    // Tighten receiver member set.
                    for (auto& frt : filtered_recv)
                      constraints.add_upper_bound(
                        recv_var.value(),
                        primitive_type(frt),
                        enqueue_cb);
                  }
                }

                // Constrain angelic args from surviving methods.
                if (filtered_info.size() == 1)
                {
                  auto& fi = filtered_info[0];
                  auto params = fi.func / Params;
                  for (size_t j = 0;
                       j < params->size() && j < args->size();
                       j++)
                  {
                    auto pt = apply_subst(
                      top, params->at(j) / Type, fi.subst);
                    if (pt && pt->front() != TypeVar)
                    {
                      auto arg_loc =
                        (args->at(j) / Rhs)->location();
                      auto arg_it = env.find(arg_loc);
                      if (arg_it != env.end())
                        constrain_type(
                          arg_it->second.type, pt, enqueue_cb);
                    }
                  }
                }
              }
            }

            // Resolve on concrete part of receiver.
            auto concrete_recv = recv_it != env.end()
              ? extract_concrete_part(recv_it->second.type) : Node{};
            if (concrete_recv)
            {
              auto hand = (lookup_node / Lhs)->type();
              auto method_ident = lookup_method_name(lookup_node);
              auto method_ta = lookup_node / TypeArgs;
              auto arity = from_chars_sep_v<size_t>(lookup_node / Int);
              auto info = method_cache.resolve_callable(
                concrete_recv,
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
                    auto arg_loc = (args->at(i) / Rhs)->location();
                    auto arg_it = env.find(arg_loc);
                    if (arg_it != env.end())
                    {
                      push_shape_to_lambda(
                        pt, arg_it->second.type);
                      // Constrain Angelic arg from param type.
                      constrain_type(
                        arg_it->second.type, pt, enqueue_cb);
                    }
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

      // ---- Return terminator: constrain returned value ----
      auto term = body->parent() / Return;
      if (term == Return)
      {
        auto ret_loc = (term / LocalId)->location();
        auto ret_it = env.find(ret_loc);
        if (ret_it != env.end())
        {
          auto func_ret = func / Type;
          if (!contains_typevar(func_ret) && !is_angelic(func_ret))
          {
            constrain_type(
              ret_it->second.type, func_ret, enqueue_cb);
          }

          // Tighten the function's return constraint variable
          // with the current return type.
          tighten_func_return(func, ret_it->second.type, enqueue_cb);
        }
      }
      else if (term == Raise)
      {
        // Raise carries the enclosing function's return type
        // (from lambda lifting). Constrain the raised value.
        auto raise_loc = (term / LocalId)->location();
        auto raise_it = env.find(raise_loc);
        if (raise_it != env.end())
        {
          auto raise_ret = term / Type;
          if (!contains_typevar(raise_ret))
            constrain_type(
              raise_it->second.type, raise_ret, enqueue_cb);
        }
      }
    }

    // ===== split_cond (typetest narrowing) =====

    void split_cond(
      const Node& cond,
      const Node& body,
      const Env& fwd_exit,
      Env& true_env,
      Env& false_env)
    {
      auto trace = trace_typetest(cond / LocalId, body);
      if (!trace)
        return;

      auto src_loc = trace->src->location();
      auto src_it = fwd_exit.find(src_loc);
      if (src_it == fwd_exit.end())
        return;

      auto true_target = trace->negated ? &false_env : &true_env;
      auto false_target = trace->negated ? &true_env : &false_env;

      // True branch: narrow to tested type.
      (*true_target)[src_loc] = {clone(trace->type), src_it->second.call_node};

      // False branch: exclude tested type if possible.
      auto excluded = exclude_tested_type(
        top, src_it->second.type, trace->type);
      if (excluded)
        (*false_target)[src_loc] = {excluded, src_it->second.call_node};
    }

    // ===== seed =====

    void seed(AbstractInterpreter<InferDomain>& ai)
    {
      // Keep AI reference for constraint notifications during join.
      ai_ = &ai;

      // Initialize method cache with top node.
      method_cache.top = top;

      // Track lambda functions with omitted return types.
      lambda_returns_omitted.clear();
      auto& lro = lambda_returns_omitted;
      top->traverse([&](auto node) {
        if (
          node == Function && is_lambda_function(node) &&
          (node / Type)->front() == TypeVar)
          lro.insert(node.get());
        return true;
      });

      // Seed fwd[entry_label] with param types and enqueue.
      for (auto& [func, entry_idx] : ai.func_entry())
      {
        Env param_env;
        for (auto& pd : *(func / Params))
        {
          auto type = pd / Type;
          param_env[(pd / Ident)->location()] = {clone(type), {}};
        }
        if (!param_env.empty())
          ai.push_fwd(entry_idx, param_env);
        // Always enqueue entry labels — even parameterless functions
        // need forward_transfer to process their body.
        ai.enqueue(entry_idx);
      }
    }

    // ===== Finalize =====

    void finalize(FinalizeContext<Env>& ctx)
    {
      // Clear AI reference.
      ai_ = nullptr;

      auto& cfg = ctx.cfg;
      auto& fwd = ctx.fwd;
      auto& fwd_exit = ctx.fwd_exit;
      auto& func_entry = ctx.func_entry;
      auto& func_returns = ctx.func_returns;

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

            // Resolve the Const's type from fwd_exit, resolving any
            // angelic components from the constraint store.
            Node final_type;
            auto env_it = fwd_exit[i].find(loc);
            if (env_it != fwd_exit[i].end())
            {
              auto& etype = env_it->second.type;
              if (!contains_angelic(etype))
              {
                // Pure concrete — use directly.
                final_type = etype;
              }
              else
              {
                // Contains angelic — resolve each angelic component
                // from constraint store, rebuild as concrete.
                Nodes resolved_parts;
                bool all_ok = true;

                auto resolve_node = [&](const Node& n) {
                  if (n == AngelicSubtype)
                  {
                    auto ct = Type << clone(n);
                    auto cid = get_angelic_var_id(ct);
                    if (cid.has_value())
                    {
                      auto& ms = constraints.member_set(cid.value());
                      if (ms.size() == 1)
                        resolved_parts.push_back(primitive_type(ms[0]));
                      else
                        all_ok = false; // Not yet singleton.
                    }
                    else
                      all_ok = false;
                  }
                  else
                    resolved_parts.push_back(Type << clone(n));
                };

                if (is_angelic(etype))
                  resolve_node(etype->front());
                else if (etype->front() == Union)
                  for (auto& child : *(etype->front()))
                    resolve_node(child);
                else
                  resolved_parts.push_back(clone(etype));

                if (all_ok && !resolved_parts.empty())
                {
                  // Deduplicate.
                  Nodes deduped;
                  for (auto& rp : resolved_parts)
                  {
                    bool covered = false;
                    for (auto& d : deduped)
                      if (same_type_tree(d, rp))
                      { covered = true; break; }
                    if (!covered)
                      deduped.push_back(rp);
                  }
                  if (deduped.size() == 1)
                    final_type = deduped[0];
                  else
                  {
                    Node u = Union;
                    for (auto& d : deduped)
                      u << clone(d->front());
                    final_type = Type << u;
                  }
                }
              }
            }

            // Fallback: look up constraint variable by stmt.
            if (!final_type)
            {
              auto svit = stmt_var_ids.find((*it).get());
              if (svit != stmt_var_ids.end())
              {
                auto& ms = constraints.member_set(svit->second);
                if (ms.size() == 1)
                  final_type = primitive_type(ms[0]);
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
            auto fname_view =
              (child / Ident)->location().view();

            // Check field constraint variable first.
            auto fv_key = std::make_pair(
              parent_cls.get(), std::string(fname_view));
            auto fv_it = field_var.find(fv_key);
            if (fv_it != field_var.end())
            {
              auto& ms = constraints.member_set(fv_it->second);
              if (ms.size() == 1)
              {
                child->replace(
                  child / Type, primitive_type(ms[0]));
                continue;
              }
              auto& ubs = constraints.upper_bounds(fv_it->second);
              if (!ubs.empty())
              {
                // Use the most refined non-angelic non-TypeVar bound.
                for (auto rit = ubs.rbegin(); rit != ubs.rend(); ++rit)
                {
                  if (!contains_typevar(*rit) && !contains_angelic(*rit))
                  {
                    child->replace(child / Type, clone(*rit));
                    break;
                  }
                }
                continue;
              }
            }

            // Fallback: check fwd_exit.
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
      InferDomain domain;
      domain.top = top;

      AbstractInterpreter<InferDomain> ai(top, std::move(domain));
      ai.run();

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
