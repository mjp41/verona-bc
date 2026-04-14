// ===== Global Worklist Type Inference =====
//
// Architecture: Bidirectional Bounds with Push-Based Data Flow
//
// Type inference is two independent analyses that interact through
// refinement:
//
// 1. FORWARD ANALYSIS computes what each variable *could be* (upper
//    bounds). Flows from definitions toward uses. Merges widen (union
//    at control-flow joins).
//
// 2. BACKWARD ANALYSIS collects use-site constraints on each variable.
//    Flows from uses toward definitions. Merges also widen (a variable
//    flowing to multiple uses must satisfy all of them).
//
// 3. REFINEMENT resolves unresolved types (DefaultInt, DefaultFloat,
//    TypeVar) when a backward constraint provides a compatible single
//    concrete type. Fires inline during the solve loop, not as a
//    separate post-fixpoint phase.
//
// Key design: labels store their INPUTS, and processing a label pushes
// outputs directly into successor/predecessor inputs via merge_type.
// No "build entry env" phase — the entry IS the stored state.
//
// Per-label state:
//   fwd[i] = forward types at ENTRY of label i
//   bwd[i] = backward constraints at EXIT of label i
//
// Forward processing of label i:
//   1. Read fwd[i] (populated by predecessors pushing into it)
//   2. Run forward transfer functions: fwd[i] -> fwd_exit
//   3. Push fwd_exit into fwd[succ] via merge_type
//   4. Cond terminators: push narrowed type to true successor,
//      excluded type to false successor
//
// Backward processing of label i:
//   1. Read bwd[i] (populated by successors pushing into it)
//   2. Run backward transfer functions in reverse: bwd[i] -> bwd_entry
//   3. Push bwd_entry into bwd[pred] -- but only if compatible with
//      the forward type on that edge (path-sensitive backward push)
//
// Path-sensitive backward push:
//   When pushing backward to a predecessor whose terminator is a Cond
//   that narrowed the same variable, check compatibility:
//   - True edge: backward type must be subtype of narrowed type
//   - False edge: backward type must be disjoint from excluded type
//   If incompatible, skip the push for that (predecessor, location).
//
// Refinement:
//   After processing a label, for each location in fwd[i]:
//   - If fwd is DefaultInt/DefaultFloat and bwd is a compatible single
//     concrete type, refine fwd to bwd. Enqueue label Forward.
//     Union backward constraints do NOT trigger refinement for Defaults
//     (ambiguous literal: e.g. 42 used as both i32 and string).
//   - If fwd is TypeVar and bwd is any concrete type (including Union),
//     refine fwd to bwd. This handles generic type parameters where
//     multiple call sites constrain T to different types — the Union
//     IS the inferred type.
//
// Convergence:
//   Forward and backward are individually monotone (merge_type only
//   widens). Refinement fires at most once per (label, location) due
//   to merge_type's absorption property: once a location is concrete,
//   merge_type absorbs incoming DefaultInt/TypeVar without change.
//   Total refinement events bounded by labels x locations.
//   Safety net: MAX_ITERS_PER_LABEL = 200 with compilation error.
//
// The AST is mutated exactly once, at finalization, after the fixpoint
// is reached. During the solve loop, all type state lives in the
// fwd/bwd side-structures -- no AST mutations.
//
// Cross-function flow:
//   push_cross() maps AST nodes (ParamDef, FieldDef, Function) to
//   label indices and pushes types into the appropriate fwd/bwd env.
//   Called by transfer functions during normal processing.

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

  // ===== Type predicates =====

  Node angelic_int()
  {
    return Type << (AngelicSubtype << DefaultInt);
  }

  Node angelic_float()
  {
    return Type << (AngelicSubtype << DefaultFloat);
  }

  bool is_angelic(const Node& type)
  {
    return type && type == Type && !type->empty() &&
      type->front() == AngelicSubtype;
  }

  // Check if a type is a default integer or default float,
  // handling both old tokens and AngelicSubtype wrappers.
  bool is_default_int(const Node& type)
  {
    if (!type || type->empty())
      return false;
    if (type->front() == DefaultInt)
      return true;
    if (type->front() == AngelicSubtype && !type->front()->empty())
      return type->front()->front() == DefaultInt;
    return false;
  }

  bool is_default_float(const Node& type)
  {
    if (!type || type->empty())
      return false;
    if (type->front() == DefaultFloat)
      return true;
    if (type->front() == AngelicSubtype && !type->front()->empty())
      return type->front()->front() == DefaultFloat;
    return false;
  }

  bool is_default_type(const Node& type)
  {
    if (!type || type->empty())
      return false;
    if (type->front()->in({DefaultInt, DefaultFloat}))
      return true;
    // AngelicSubtype is semantically a default (unresolved).
    if (type->front() == AngelicSubtype)
      return true;
    return false;
  }

  bool contains_default_type(const Node& type)
  {
    if (!type)
      return false;

    bool found = false;
    type->traverse([&](auto node) {
      if (node->in({DefaultInt, DefaultFloat}))
      {
        found = true;
        return false;
      }
      return !found;
    });
    return found;
  }

  Node resolve_default(const Node& type)
  {
    if (!type || type->empty())
      return type;
    if (type->front() == DefaultInt)
      return primitive_type(U64);
    if (type->front() == DefaultFloat)
      return primitive_type(F64);
    if (type->front() == AngelicSubtype && !type->front()->empty())
    {
      auto bound = type->front()->front();
      if (bound == DefaultInt)
        return primitive_type(U64);
      if (bound == DefaultFloat)
        return primitive_type(F64);
    }
    return type;
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
    if (inner == DefaultInt)
      return U64;
    if (inner == DefaultFloat)
      return F64;
    if (inner == AngelicSubtype && !inner->empty())
    {
      if (inner->front() == DefaultInt)
        return U64;
      if (inner->front() == DefaultFloat)
        return F64;
    }
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

  Node default_literal_type(const Node& lit)
  {
    if (lit->in({True, False}))
      return Bool;
    if (lit == None)
      return None;
    if (lit->in({Bin, Oct, Int, Hex, Char}))
      return DefaultInt;
    if (lit->in({Float, HexFloat}))
      return DefaultFloat;
    assert(false && "unhandled literal type");
    return {};
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

  // Returns the merged type, or {} if no change from existing.
  static Node merge_type(const Node& existing, const Node& incoming, Node top)
  {
    if (!existing || existing->empty() || existing->front() == TypeVar)
    {
      // Both TypeVar -> no change.
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

    // AngelicSubtype: opaque at forward joins.
    // Angelic(T) is less info than concrete — concrete wins.
    // Two angelics with same structure → no change (same_type_tree
    // already caught this above).
    if (is_angelic(existing))
    {
      if (is_angelic(incoming))
        return {}; // Both angelic, same_type_tree already checked.
      // existing is angelic, incoming is concrete → widen to concrete.
      return clone(incoming);
    }
    if (is_angelic(incoming))
    {
      // incoming is angelic, existing is concrete → keep concrete.
      return {};
    }

    // Default yields to compatible concrete primitive.
    if (is_default_type(existing) && !is_default_type(incoming))
    {
      auto prim = extract_primitive(incoming);
      if (prim)
      {
        bool compat =
          (is_default_int(existing) && prim->in(integer_types)) ||
          (is_default_float(existing) && prim->in(float_types));
        if (compat)
          return clone(incoming);
      }
    }

    if (is_default_type(incoming) && !is_default_type(existing))
    {
      auto prim = extract_primitive(existing);
      if (prim)
      {
        bool compat =
          (is_default_int(incoming) && prim->in(integer_types)) ||
          (is_default_float(incoming) && prim->in(float_types));
        if (compat)
          return {};
      }
    }

    bool existing_has_self = contains_self_type(existing);
    bool incoming_has_self = contains_self_type(incoming);
    if (existing_has_self != incoming_has_self)
    {
      if (existing_has_self)
        return clone(incoming);
      return {};
    }

    SequentCtx ctx{top, {}, {}};

    if (Subtype.invariant(ctx, existing, incoming))
      return {};

    if (Subtype(ctx, incoming, existing))
      return {};
    if (Subtype(ctx, existing, incoming))
      return clone(incoming);

    // Build union.
    auto e_inner = existing->front();
    auto i_inner = incoming->front();
    Node u = Union;

    if (e_inner == Union)
      for (auto& c : *e_inner)
        u << clone(c);
    else
      u << clone(e_inner);

    // If incoming is a concrete primitive, replace compatible Default
    // members in the union.
    auto inc_prim = extract_primitive(incoming);
    if (inc_prim && !is_default_type(incoming))
    {
      auto it = u->begin();
      while (it != u->end())
      {
        bool is_def_int = (*it) == DefaultInt;
        bool is_def_float = (*it) == DefaultFloat;
        if (
          (is_def_int && inc_prim->in(integer_types)) ||
          (is_def_float && inc_prim->in(float_types)))
          it = u->erase(it, std::next(it));
        else
          ++it;
      }
    }

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

  // ===== Typetest trace =====

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
        else if (is_default_type(existing->second.type) && !is_default)
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

  // ===== Shape propagation =====

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
        is_default_type(arg_it->second.type));
    }

    bool all_default = !constraints.empty();
    for (auto& [tp, info] : constraints)
      if (!is_default_type(info.type))
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

  // ===== Liveness =====

  struct Liveness
  {
    std::vector<std::set<Location>> defs, uses, kills;
    std::vector<std::set<Location>> live_in, live_out;

    void build(const CFG& cfg)
    {
      size_t n = cfg.size();
      defs.resize(n);
      uses.resize(n);
      kills.resize(n);
      live_in.resize(n);
      live_out.resize(n);

      for (size_t i = 0; i < n; i++)
      {
        auto body = cfg.labels[i].label / Body;
        auto term = cfg.labels[i].label / Return;

        // Collect defs and kills.
        for (auto& stmt : *body)
        {
          if (!stmt->empty() && stmt->front() == LocalId)
          {
            auto loc = stmt->front()->location();
            kills[i].insert(loc);
            if (loc.view().starts_with("local$"))
              defs[i].insert(loc);
          }
        }

        // Collect uses (from body and terminator).
        auto collect_uses = [&](const Node& root) {
          if (!root)
            return;
          root->traverse([&](auto& node) {
            if (node == LocalId)
              uses[i].insert(node->location());
            return true;
          });
        };
        collect_uses(body);
        collect_uses(term);

        // Remove defs from uses.
        for (auto& d : defs[i])
          uses[i].erase(d);
      }

      // Fixpoint liveness.
      bool changed = true;
      while (changed)
      {
        changed = false;
        for (size_t j = n; j-- > 0;)
        {
          std::set<Location> next_out;
          for (auto s : cfg.succ[j])
            next_out.insert(live_in[s].begin(), live_in[s].end());

          std::set<Location> next_in = uses[j];
          for (auto& loc : next_out)
            if (kills[j].count(loc) == 0)
              next_in.insert(loc);

          if (next_out != live_out[j] || next_in != live_in[j])
          {
            live_out[j] = std::move(next_out);
            live_in[j] = std::move(next_in);
            changed = true;
          }
        }
      }
    }

    bool is_live_in(size_t label, const Location& loc) const
    {
      return live_in[label].count(loc) > 0;
    }

    bool is_defined(size_t label, const Location& loc) const
    {
      return defs[label].count(loc) > 0;
    }
  };

  // ===== GlobalInfer (orchestrator) =====

  struct GlobalInfer
  {
    Node top;
    CFG cfg;
    Liveness liveness;

    // Per-label state: stored INPUTS in each direction.
    std::vector<TypeEnv> fwd; // fwd[i] = forward types at ENTRY of label i
    std::vector<TypeEnv> bwd; // bwd[i] = backward constraints at EXIT

    // Forward exit envs: computed during solve, used by finalization.
    std::vector<TypeEnv> fwd_exit; // fwd_exit[i] = types at EXIT of label i

    // Cross-function mappings.
    std::map<Node, size_t> func_entry; // Function -> entry label index
    std::map<Node, std::vector<size_t>>
      func_returns; // Function -> return label indices

    // Shared mutable state.
    std::map<Location, Node> lookup_stmts;
    std::set<std::pair<Location, Location>> typevar_aliases;
    std::map<Location, std::pair<Location, size_t>> ref_to_tuple;

    // Inferred TypeArgs tracked in side-structure (not AST).
    // Key is the call statement's LocalId location; value is the
    // inferred TypeArgs for each scope in the FuncName path.
    // Written to AST at finalization.
    std::map<Node, std::vector<std::pair<Node, Node>>>
      call_typeargs; // stmt -> [(name_elem, TypeArgs)]

    // Method resolution cache.
    std::map<
      std::tuple<std::string, std::string, std::string, size_t>,
      Node>
      method_cache_storage;

    // Per-label tuple tracking state.
    std::map<Location, TupleTracking> tuple_locals;

    // Run forward transfer functions over a label body.
    // Reads from env (starts as copy of fwd[label]).
    // Returns true if any type was produced/changed.
    bool forward_pass(
      TypeEnv& env, const Node& body,
      const Node& /*func*/)
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
        if (it->second.type == type)
          return false;
        auto merged = merge_type(it->second.type, type, top);
        if (!merged)
          return false;
        it->second.type = merged;
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
          Node type_tok;
          if (stmt->size() == 3)
            type_tok = stmt->at(1);
          else
            type_tok = default_literal_type(stmt->back());
          Node type;
          if (type_tok == DefaultInt)
          {
            auto it = env.find(dst->location());
            if (it != env.end() && !is_angelic(it->second.type) &&
                !is_default_type(it->second.type))
            {
              auto prim = extract_primitive(it->second.type);
              if (prim && prim->in(integer_types))
                type = clone(it->second.type);
            }
            if (!type)
              type = angelic_int();
          }
          else if (type_tok == DefaultFloat)
          {
            auto it = env.find(dst->location());
            if (it != env.end() && !is_angelic(it->second.type) &&
                !is_default_type(it->second.type))
            {
              auto prim = extract_primitive(it->second.type);
              if (prim && prim->in(float_types))
                type = clone(it->second.type);
            }
            if (!type)
              type = angelic_float();
          }
          else
          {
            type = primitive_or_ffi_type(type_tok->type());
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
          auto dst_it = env.find(dst_loc);

          if (src_it != env.end() && dst_it != env.end())
          {
            bool dst_tv = dst_it->second.type->front() == TypeVar;
            bool src_tv = src_it->second.type->front() == TypeVar;
            if (dst_tv != src_tv)
              typevar_aliases.insert({dst_loc, src_loc});
          }

          if (src_it != env.end())
            merge(dst_loc, src_it->second.type, src_it->second.call_node);

          auto tuple_ref = ref_to_tuple.find(src_loc);
          if (tuple_ref != ref_to_tuple.end())
            ref_to_tuple[dst_loc] = tuple_ref->second;
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
          auto loc = (stmt / LocalId)->location();
          merge(loc, clone(stmt / Type));
          auto it = env.find(loc);
          if (it != env.end())
            it->second.type = clone(stmt / Type); // Fixed.
        }
        else if (stmt->in({New, Stack}))
        {
          merge((stmt / LocalId)->location(), clone(stmt / Type));
        }
        else if (stmt->in(propagate_lhs_ops))
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto lhs_loc = (stmt / Lhs)->location();
          auto rhs_loc = (stmt / Rhs)->location();
          auto lhs_it = env.find(lhs_loc);
          auto rhs_it = env.find(rhs_loc);

          if (
            lhs_it != env.end() && rhs_it != env.end() &&
            is_default_type(lhs_it->second.type))
          {
            auto rhs_prim = extract_callable_primitive(rhs_it->second.type);
            bool compatible = rhs_prim &&
              ((is_default_int(lhs_it->second.type) &&
                rhs_prim->in(integer_types)) ||
               (is_default_float(lhs_it->second.type) &&
                rhs_prim->in(float_types)));
            if (compatible)
            {
              auto refined = primitive_or_ffi_type(rhs_prim->type());
              merge(dst_loc, clone(refined));
              lhs_it = env.find(lhs_loc);
            }
          }

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

          bool all_default =
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
              all_default ? stmt : Node{});

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
            if (is_default_type(src_it->second.type) ||
                is_angelic(src_it->second.type))
            {
              // Propagate angelic/default unchanged — method
              // resolution happens when backward narrows it.
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
              !is_default_type(recv_it->second.type))
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
          {
            auto apply_ret = src_it->second.type;
            // Track When return type in env, not AST.
            merge(dst_loc, cown_type(apply_ret));
          }

          // Push cown inner types into lambda params via fwd envs.
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
                      auto new_type = ref_type(ci);
                      // Push into lambda's entry env, not AST.
                      push_param_type(
                        apply_func,
                        (param / Ident)->location(),
                        new_type);
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

    // Run backward transfer functions over a label body (reverse order).
    // Reads from env (forward types) and bwd_exit (backward constraints
    // from successors). Produces bwd_entry (constraints to push to preds).
    bool backward_pass(
      TypeEnv& env,
      TypeEnv& bwd_entry,
      const TypeEnv& bwd_exit,
      const Node& body)
    {
      bool changed = false;

      // Build const_defs and def_stmts for this body.
      std::map<Location, Node> const_defs;
      std::map<Location, Node> def_stmts;
      for (auto& stmt : *body)
      {
        if (!stmt->empty() && stmt->front() == LocalId)
          def_stmts[stmt->front()->location()] = stmt;
        if (stmt == Const)
          const_defs[(stmt / LocalId)->location()] = stmt;
      }

      // refine_local_const: refine default-typed Const literals in env
      // only. AST writes are deferred to finalization.
      auto refine_local_const =
        [&](const Location& loc, const Node& expected) -> bool {
        auto const_it = const_defs.find(loc);
        if (const_it == const_defs.end())
          return false;
        auto env_it = env.find(loc);
        if (env_it == env.end())
          return false;
        auto expected_prim = extract_primitive(expected);
        if (!expected_prim)
          return false;
        auto current_prim = extract_primitive(env_it->second.type);
        bool compatible = (is_default_int(env_it->second.type) &&
                           expected_prim->in(integer_types)) ||
          (is_default_float(env_it->second.type) &&
           expected_prim->in(float_types)) ||
          (current_prim && current_prim->in(integer_types) &&
           expected_prim->in(integer_types)) ||
          (current_prim && current_prim->in(float_types) &&
           expected_prim->in(float_types));
        if (!compatible)
          return false;
        env_it->second.type = primitive_or_ffi_type(expected_prim->type());
        // No AST mutation here — deferred to finalize().
        changed = true;
        return true;
      };

      // merge_bwd: merge a backward constraint into bwd_entry.
      auto merge_bwd = [&](const Location& loc, const Node& type) -> bool {
        if (!type || type->empty() ||
            type->front()->in({TypeVar, DefaultInt, DefaultFloat, AngelicSubtype}))
          return false;
        auto it = bwd_entry.find(loc);
        if (it == bwd_entry.end())
        {
          bwd_entry[loc] = {clone(type), {}};
          changed = true;
          return true;
        }
        auto merged = merge_type(it->second.type, type, top);
        if (!merged)
          return false;
        it->second.type = merged;
        changed = true;
        return true;
      };

      // Walk body in reverse.
      for (auto it = body->rbegin(); it != body->rend(); ++it)
      {
        auto& stmt = *it;

        if (stmt->in({Copy, Move}))
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto src_loc = (stmt / Rhs)->location();
          auto dst_it = env.find(dst_loc);
          if (dst_it != env.end())
          {
            Node expected = dst_it->second.type;
            // Check bwd_entry (locally accumulated), then bwd_exit
            // for a concrete override.
            auto be_it = bwd_entry.find(dst_loc);
            if (
              be_it != bwd_entry.end() && be_it->second.type &&
              !be_it->second.type->empty() &&
              !be_it->second.type->front()->in(
                {TypeVar, DefaultInt, DefaultFloat,
                 AngelicSubtype, Union}))
              expected = be_it->second.type;
            else
            {
              auto bx_it = bwd_exit.find(dst_loc);
              if (
                bx_it != bwd_exit.end() && bx_it->second.type &&
                !bx_it->second.type->empty() &&
                !bx_it->second.type->front()->in(
                  {TypeVar, DefaultInt, DefaultFloat,
                   AngelicSubtype, Union}))
                expected = bx_it->second.type;
            }

            // Only push if expected is concrete (not Angelic/Default).
            if (!is_default_type(expected) && !is_angelic(expected))
            {
              snmalloc::UNUSED(refine_local_const(src_loc, expected));
              snmalloc::UNUSED(merge_bwd(src_loc, expected));
            }
          }
        }
        else if (stmt == Load)
        {
          auto src_loc = (stmt / Rhs)->location();
          auto dst_loc = (stmt / LocalId)->location();
          auto dst_it = env.find(dst_loc);
          if (dst_it != env.end() && !is_default_type(dst_it->second.type))
            snmalloc::UNUSED(
              merge_bwd(src_loc, ref_type(clone(dst_it->second.type))));
        }
        else if (stmt == Store)
        {
          auto ref_loc = (stmt / Rhs)->location();
          auto val_loc = ((stmt / Arg) / Rhs)->location();
          auto ref_it = env.find(ref_loc);
          if (ref_it != env.end())
          {
            auto inner = extract_ref_inner(ref_it->second.type);
            if (inner && !is_any_type(inner))
            {
              snmalloc::UNUSED(refine_local_const(val_loc, inner));
              snmalloc::UNUSED(merge_bwd(val_loc, clone(inner)));
            }
          }
        }
        else if (stmt->in({New, Stack}))
        {
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
                  {
                    snmalloc::UNUSED(refine_local_const(arg_loc, ft));
                    snmalloc::UNUSED(merge_bwd(arg_loc, ft));
                  }
                  // FieldDef TypeVar → concrete: tracked via env,
                  // written at finalization.
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
          if (lhs_it != env.end())
            snmalloc::UNUSED(merge_bwd(rhs_loc, clone(lhs_it->second.type)));
          auto dst_it = env.find(dst_loc);
          if (dst_it != env.end() && !is_default_type(dst_it->second.type))
          {
            snmalloc::UNUSED(merge_bwd(lhs_loc, clone(dst_it->second.type)));
            snmalloc::UNUSED(merge_bwd(rhs_loc, clone(dst_it->second.type)));
          }
        }
        else if (stmt->in(propagate_rhs_ops))
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto src_loc = (stmt / Rhs)->location();
          auto dst_it = env.find(dst_loc);
          if (dst_it != env.end() && !is_default_type(dst_it->second.type))
            snmalloc::UNUSED(merge_bwd(src_loc, clone(dst_it->second.type)));
        }
        else if (stmt == FFIStore)
        {
          auto value_loc = (stmt / ValueSrc)->location();
          auto expected = clone(stmt / Type);
          snmalloc::UNUSED(refine_local_const(value_loc, expected));
          snmalloc::UNUSED(merge_bwd(value_loc, expected));
        }
        else if (stmt == Call)
        {
          std::vector<ScopeInfo> scopes;
          auto func_def = navigate_call(stmt, top, scopes);
          if (!func_def)
            continue;

          auto args = stmt / Args;
          auto params = func_def / Params;
          auto ret_type = func_def / Type;

          NodeMap<Node> subst;
          for (auto& scope : scopes)
          {
            auto ta = scope.name_elem / TypeArgs;
            auto tps = scope.def / TypeParams;
            if (!ta->empty() && ta->size() == tps->size())
              for (size_t i = 0; i < tps->size(); i++)
                subst[tps->at(i)] = ta->at(i);
          }

          // Check if the Call result has a backward constraint
          // that can refine TypeArgs.
          auto dst_loc = (stmt / LocalId)->location();
          {
            Node bwd_result;
            auto be_it = bwd_entry.find(dst_loc);
            if (
              be_it != bwd_entry.end() &&
              !is_default_type(be_it->second.type) &&
              !is_uninformative_backward_type(be_it->second.type))
              bwd_result = be_it->second.type;
            if (!bwd_result)
            {
              auto bx_it = bwd_exit.find(dst_loc);
              if (
                bx_it != bwd_exit.end() &&
                !is_default_type(bx_it->second.type) &&
                !is_uninformative_backward_type(bx_it->second.type))
                bwd_result = bx_it->second.type;
            }

            if (bwd_result && ret_type->front() != TypeVar)
            {
              // Extract TypeParam constraints from return type
              // vs backward constraint.
              NodeMap<LocalTypeInfo> constraints;
              extract_constraints(
                top,
                ret_type->front(),
                bwd_result->front(),
                constraints,
                false);

              // Update substitution with new constraints.
              for (auto& scope : scopes)
              {
                auto tps = scope.def / TypeParams;
                for (auto& tp : *tps)
                {
                  auto find = constraints.find(tp);
                  if (find != constraints.end() &&
                      !is_default_type(find->second.type))
                    subst[tp] = find->second.type;
                }
              }
            }
          }

          for (size_t i = 0; i < params->size() && i < args->size(); i++)
          {
            auto expected = apply_subst(top, params->at(i) / Type, subst);
            if (
              expected && expected->front() != TypeVar &&
              !is_uninformative_backward_type(expected))
            {
              auto arg_loc = (args->at(i) / Rhs)->location();
              snmalloc::UNUSED(refine_local_const(arg_loc, expected));
              snmalloc::UNUSED(merge_bwd(arg_loc, expected));
            }
          }
        }
        else if (stmt->in({CallDyn, TryCallDyn}))
        {
          auto dst_loc = (stmt / LocalId)->location();
          auto src_loc = (stmt / Rhs)->location();
          auto args = stmt / Args;

          auto lookup_it = lookup_stmts.find(src_loc);
          if (lookup_it != lookup_stmts.end())
          {
            auto lookup_node = lookup_it->second;
            auto recv_loc = (lookup_node / Rhs)->location();
            auto recv_it = env.find(recv_loc);
            if (recv_it != env.end())
            {
              auto hand = (lookup_node / Lhs)->type();
              auto method_ident = lookup_method_name(lookup_node);
              auto method_ta = lookup_node / TypeArgs;
              auto arity = from_chars_sep_v<size_t>(lookup_node / Int);

              Node resolve_type = recv_it->second.type;

              // If receiver is default-typed, try to determine the
              // concrete type from backward constraints or args.
              if (is_default_type(resolve_type))
              {
                // Check if the CallDyn result has a backward
                // constraint (from a downstream Call param type).
                Node target_prim;
                auto be_it = bwd_entry.find(dst_loc);
                if (be_it != bwd_entry.end())
                  target_prim = extract_primitive(be_it->second.type);
                if (!target_prim)
                {
                  auto bx_it = bwd_exit.find(dst_loc);
                  if (bx_it != bwd_exit.end())
                    target_prim = extract_primitive(bx_it->second.type);
                }

                // Check if any arg has a concrete type.
                if (!target_prim)
                {
                  for (auto& arg_node : *args)
                  {
                    auto arg_it =
                      env.find((arg_node / Rhs)->location());
                    if (
                      arg_it != env.end() &&
                      !is_default_type(arg_it->second.type))
                    {
                      target_prim =
                        extract_callable_primitive(arg_it->second.type);
                      if (target_prim)
                        break;
                    }
                  }
                }

                if (target_prim)
                {
                  bool compat =
                    (is_default_int(resolve_type) &&
                     target_prim->in(integer_types)) ||
                    (is_default_float(resolve_type) &&
                     target_prim->in(float_types));
                  if (compat)
                  {
                    resolve_type =
                      primitive_or_ffi_type(target_prim->type());
                    // Refine the receiver and its Const.
                    snmalloc::UNUSED(
                      refine_local_const(recv_loc, resolve_type));
                    snmalloc::UNUSED(
                      merge_bwd(recv_loc, clone(resolve_type)));
                  }
                }
              }

              // Only resolve and push param types when we found
              // a concrete target_prim. Otherwise leave unresolved.
              if (resolve_type && !is_default_type(resolve_type))
              {
                auto info = resolve_callable_method(
                  top,
                  resolve_type,
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
                    auto expected =
                      apply_subst(top, params->at(i) / Type, info.subst);
                    if (
                      expected && expected->front() != TypeVar &&
                      !is_uninformative_backward_type(expected))
                    {
                      auto arg_loc = (args->at(i) / Rhs)->location();
                      snmalloc::UNUSED(
                        refine_local_const(arg_loc, expected));
                      snmalloc::UNUSED(merge_bwd(arg_loc, expected));
                    }
                  }
                }
              }
            }
          }
        }
        else if (stmt == FFI)
        {
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
                  snmalloc::UNUSED(
                    refine_local_const((*fa)->location(), clone(*fp)));
                  snmalloc::UNUSED(
                    merge_bwd((*fa)->location(), clone(*fp)));
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
        // When backward is a no-op.
      }

      return changed;
    }

    // Direction-aware worklist.
    std::deque<size_t> worklist;
    std::map<size_t, Direction> worklist_dir;

    void enqueue(size_t label, Direction dir)
    {
      auto it = worklist_dir.find(label);
      if (it == worklist_dir.end())
      {
        worklist.push_back(label);
        worklist_dir[label] = dir;
      }
      else
      {
        // Merge directions: if already queued in a different direction,
        // upgrade to Both.
        if (it->second != dir)
          it->second = Direction::Both;
      }
    }

    bool dequeue(size_t& label, Direction& dir)
    {
      if (worklist.empty())
        return false;
      label = worklist.front();
      worklist.pop_front();
      auto it = worklist_dir.find(label);
      dir = it->second;
      worklist_dir.erase(it);
      return true;
    }

    // Merge a type into a target env at a location. Returns true if
    // the env changed.
    bool push(
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

      // Pointer equality fast path.
      if (it->second.type == type)
      {
        if (call_node && !it->second.call_node)
        {
          it->second.call_node = call_node;
          return true;
        }
        return false;
      }

      auto merged = merge_type(it->second.type, type, top);
      if (!merged)
      {
        if (call_node && !it->second.call_node)
        {
          it->second.call_node = call_node;
          return true;
        }
        return false;
      }

      it->second.type = merged;
      if (call_node && !it->second.call_node)
        it->second.call_node = call_node;
      return true;
    }

    // Cross-function constraint push.
    bool push_cross(
      const Node& /*target*/, const Node& /*type*/, Direction /*dir*/)
    {
      // Generic push_cross — unused, specific methods below.
      return false;
    }

    // Push a concrete arg type into a callee function's entry env
    // at the param location. Replaces push_arg_types_to_params AST
    // mutation.
    bool push_param_type(
      const Node& func_def,
      const Location& param_loc,
      const Node& type)
    {
      auto entry_it = func_entry.find(func_def);
      if (entry_it == func_entry.end())
        return false;
      bool changed = push(fwd[entry_it->second], param_loc, type);
      if (changed)
        enqueue(entry_it->second, Direction::Forward);
      return changed;
    }

    // Push a backward constraint on a callee function's return type.
    // Used to propagate shape return type expectations.
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
          if (push(bwd[idx], ret_loc, type))
          {
            enqueue(idx, Direction::Backward);
            changed = true;
          }
        }
      }
      return changed;
    }

    // Push concrete arg types into callee param locations via fwd envs.
    // Replaces the old push_arg_types_to_params that mutated AST.
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

        // Check if FieldDef has a concrete type already.
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
            contains_default_type(arg_it->second.type) && !allow_default)
            continue;
          resolved = arg_it->second.type;
        }

        push_param_type(
          func_def, (param / Ident)->location(), resolved);
      }
    }

    // Push shape type constraints into a lambda's params/return via
    // fwd/bwd envs. Replaces AST-mutating propagate_shape_to_lambda.
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
            // Push into lambda's entry env instead of mutating AST.
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
              // Push as backward constraint on lambda returns.
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

    void build()
    {
      // Build CFG from AST.
      cfg.build(top);
      size_t n = cfg.size();
      if (n == 0)
        return;

      // Compute liveness.
      liveness.build(cfg);

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
      bwd.resize(n);

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

      // Seed bwd[return_labels] with declared return types.
      for (auto& [func, return_indices] : func_returns)
      {
        auto func_ret = func / Type;
        if (!contains_typevar(func_ret) && !is_default_type(func_ret))
        {
          for (auto idx : return_indices)
          {
            auto term = cfg.labels[idx].label / Return;
            auto ret_loc = (term / LocalId)->location();
            bwd[idx][ret_loc] = {clone(func_ret), {}};
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
          // Convergence failure -- emit compilation error.
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

        // Forward exit env — shared between forward and backward phases.
        TypeEnv label_exit;

        // ---- Forward phase ----
        if (run_fwd)
        {
          // Copy entry env as starting point for forward processing.
          for (auto& [loc, info] : fwd[label])
            label_exit[loc] = {clone(info.type), info.call_node};

          // Also seed from fwd_exit[label] — locally-defined
          // variables refined by a prior backward pass should
          // survive the forward re-run.
          for (auto& [loc, info] : fwd_exit[label])
          {
            if (!is_default_type(info.type))
            {
              auto it = label_exit.find(loc);
              if (it == label_exit.end())
                label_exit[loc] = {clone(info.type), info.call_node};
              else if (is_default_type(it->second.type))
                it->second.type = clone(info.type);
            }
          }

          // Clear per-label tuple tracking.
          tuple_locals.clear();

          // Run forward transfer functions.
          forward_pass(label_exit, body, li.function);

          // Store exit env for finalization.
          fwd_exit[label] = label_exit;

          // Push to successors.
          if (term == Cond)
          {
            // Typetest narrowing on Cond.
            auto trace = trace_typetest(term / LocalId, body);
            auto& func_idx = cfg.func_label_idx[li.function];
            auto t_it =
              func_idx.find(std::string((term / Lhs)->location().view()));
            auto f_it =
              func_idx.find(std::string((term / Rhs)->location().view()));

            // Push non-narrowed locations to both successors.
            for (auto& [loc, info] : label_exit)
            {
              if (trace && loc == trace->src->location())
                continue; // Handled below with narrowing.
              if (t_it != func_idx.end())
                if (push(fwd[t_it->second], loc, info.type, info.call_node))
                  enqueue(t_it->second, Direction::Forward);
              if (f_it != func_idx.end())
                if (push(fwd[f_it->second], loc, info.type, info.call_node))
                  enqueue(f_it->second, Direction::Forward);
            }

            // Push narrowed/excluded types.
            if (trace)
            {
              auto src_loc = trace->src->location();
              auto src_it = label_exit.find(src_loc);
              if (src_it != label_exit.end())
              {
                auto true_succ = trace->negated ? f_it : t_it;
                auto false_succ = trace->negated ? t_it : f_it;

                // True branch: narrowed to tested type.
                if (true_succ != func_idx.end())
                  if (push(fwd[true_succ->second], src_loc, trace->type))
                    enqueue(true_succ->second, Direction::Forward);

                // False branch: excluded type.
                if (false_succ != func_idx.end())
                {
                  auto excluded = exclude_tested_type(
                    top, src_it->second.type, trace->type);
                  auto& push_type =
                    excluded ? excluded : src_it->second.type;
                  if (push(
                        fwd[false_succ->second],
                        src_loc,
                        push_type,
                        src_it->second.call_node))
                    enqueue(false_succ->second, Direction::Forward);
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
                if (push(fwd[t_it->second], loc, info.type, info.call_node))
                  enqueue(t_it->second, Direction::Forward);
            }
          }
          // Return: no forward successors.
        }

        // ---- Backward phase ----
        if (run_bwd)
        {
          // If forward didn't run, seed label_exit from fwd_exit[label]
          // (which has locally-defined types from the last forward run).
          // Fall back to fwd[label] if fwd_exit is empty.
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

          // Build bwd_entry from backward transfer functions.
          TypeEnv bwd_entry;
          // Seed with existing bwd constraints for this label.
          for (auto& [loc, info] : bwd[label])
            bwd_entry[loc] = {clone(info.type), info.call_node}; 
          backward_pass(label_exit, bwd_entry, bwd[label], body);

          // Update fwd_exit with backward-refined types so the
          // refinement step (which reads fwd_exit) sees them.
          fwd_exit[label] = label_exit;

          // Merge locally-derived backward constraints back into
          // bwd[label] so the refinement step can see them.
          for (auto& [loc, info] : bwd_entry)
            push(bwd[label], loc, info.type);

          // Push to predecessors (path-sensitive).
          for (auto p : cfg.pred[label])
          {
            auto& pred_li = cfg.labels[p];
            auto pred_term = pred_li.label / Return;

            for (auto& [loc, info] : bwd_entry)
            {
              // Path-sensitive check for Cond predecessors.
              if (pred_term == Cond)
              {
                auto trace =
                  trace_typetest(pred_term / LocalId, pred_li.label / Body);
                if (trace && loc == trace->src->location())
                {
                  // Determine which edge (true/false) connects p to
                  // label.
                  auto& pred_func_idx = cfg.func_label_idx[pred_li.function];
                  auto t_it = pred_func_idx.find(
                    std::string((pred_term / Lhs)->location().view()));

                  bool is_true_edge =
                    (t_it != pred_func_idx.end() &&
                     t_it->second == label) != trace->negated;

                  if (is_true_edge)
                  {
                    // True edge: backward must be subtype of narrowed.
                    SequentCtx ctx{top, {}, {}};
                    if (!Subtype(ctx, info.type, trace->type))
                      continue; // Skip incompatible.
                  }
                  // False edge: push unconditionally (conservative).
                }
              }

              if (push(bwd[p], loc, info.type))
                enqueue(p, Direction::Backward);
            }
          }
        }

        // ---- Refinement ----
        // For each unresolved type in BOTH fwd[label] (entry env)
        // AND fwd_exit[label] (locally-defined vars), check if bwd
        // has a compatible concrete constraint.
        {
          // Collect locations from both entry and exit envs.
          std::set<Location> all_locs;
          for (auto& [loc, info] : fwd[label])
            all_locs.insert(loc);
          for (auto& [loc, info] : fwd_exit[label])
            all_locs.insert(loc);

          for (auto& loc : all_locs)
          {
            // Find the type — prefer exit (has local defs), fall
            // back to entry.
            LocalTypeInfo* fwd_info = nullptr;
            auto exit_it = fwd_exit[label].find(loc);
            auto entry_it = fwd[label].find(loc);
            if (exit_it != fwd_exit[label].end())
              fwd_info = &exit_it->second;
            else if (entry_it != fwd[label].end())
              fwd_info = &entry_it->second;
            if (!fwd_info)
              continue;

            auto bwd_it = bwd[label].find(loc);
            if (bwd_it == bwd[label].end())
              continue;

            auto& fwd_type = fwd_info->type;
            auto& bwd_type = bwd_it->second.type;

            if (!fwd_type || fwd_type->empty())
              continue;
            auto fwd_front = fwd_type->front();

            // Check for unresolved forward types.
            bool is_fwd_angelic = is_angelic(fwd_type);
            bool is_fwd_default = is_default_type(fwd_type);
            bool is_fwd_typevar = fwd_front == TypeVar;
            if (!is_fwd_angelic && !is_fwd_default && !is_fwd_typevar)
              continue;

            if (!bwd_type || bwd_type->empty())
              continue;
            auto bwd_front = bwd_type->front();

            // Skip if backward is also unresolved.
            if (
              is_angelic(bwd_type) || is_default_type(bwd_type) ||
              bwd_front == TypeVar)
              continue;

            bool do_refine = false;

            if (is_fwd_angelic)
            {
              // Angelic: any concrete type within the bound.
              auto bound = fwd_type->front()->front();
              if (bound == DefaultInt)
              {
                auto p = extract_primitive(bwd_type);
                do_refine = p && p->in(integer_types);
              }
              else if (bound == DefaultFloat)
              {
                auto p = extract_primitive(bwd_type);
                do_refine = p && p->in(float_types);
              }
              else
              {
                // General bound — any concrete.
                do_refine = true;
              }
            }
            else if (is_fwd_default)
            {
              if (bwd_front->in({Union, Isect}))
                continue;
              auto prim = extract_primitive(bwd_type);
              if (prim)
                do_refine =
                  (is_default_int(fwd_type) && prim->in(integer_types)) ||
                  (is_default_float(fwd_type) && prim->in(float_types));
            }
            else if (is_fwd_typevar)
            {
              do_refine = true;
            }

            if (!do_refine)
              continue;

            fwd_info->type = clone(bwd_type);
            // Also update entry env if present.
            if (entry_it != fwd[label].end())
              entry_it->second.type = clone(bwd_type);
            // Also update exit env if present.
            if (exit_it != fwd_exit[label].end())
              exit_it->second.type = clone(bwd_type);

            // Re-enqueue this label to re-run forward/backward.
            enqueue(label, Direction::Both);

            if (fwd_info->call_node)
              enqueue(label, Direction::Backward);
          }
        }
      }
    }

    void finalize()
    {
      size_t n = cfg.size();
      if (n == 0)
        return;

      // ---- 1. TypeVar backprop across aliases ----
      {
        bool tv_changed = true;
        while (tv_changed)
        {
          tv_changed = false;
          for (auto& [dst_loc, src_loc] : typevar_aliases)
          {
            for (auto& ee : fwd_exit)
            {
              auto d = ee.find(dst_loc);
              auto s = ee.find(src_loc);
              if (d == ee.end() || s == ee.end())
                continue;
              bool dtv = d->second.type->front() == TypeVar;
              bool stv = s->second.type->front() == TypeVar;
              if (!dtv && stv)
              {
                s->second.type = clone(d->second.type);
                tv_changed = true;
              }
              else if (dtv && !stv)
              {
                d->second.type = clone(s->second.type);
                tv_changed = true;
              }
            }
          }
        }
      }

      // ---- 2. Per-label: Const writes, TypeAssertion removal,
      //         NewArrayConst updates ----
      for (size_t i = 0; i < n; i++)
      {
        auto body = cfg.labels[i].label / Body;

        // Build tuple tracking for this label.
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

        // Compute final tuple/array types from element types.
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

        // Walk body: write Const types, remove TypeAssertions, update
        // NewArrayConst.
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

            // Find the best type: prefer concrete fwd_exit, then bwd,
            // then default fwd_exit refined by bwd.
            Node final_type;
            auto env_it = fwd_exit[i].find(loc);
            if (
              env_it != fwd_exit[i].end() &&
              !is_default_type(env_it->second.type) &&
              !is_angelic(env_it->second.type))
              final_type = env_it->second.type;

            if (!final_type)
            {
              auto bwd_it = bwd[i].find(loc);
              if (
                bwd_it != bwd[i].end() &&
                !is_default_type(bwd_it->second.type) &&
                !is_angelic(bwd_it->second.type))
                final_type = bwd_it->second.type;
            }

            // Also check: if fwd_exit has Default and bwd has
            // compatible concrete, use bwd.
            if (
              !final_type && env_it != fwd_exit[i].end() &&
              is_default_type(env_it->second.type))
            {
              // Check all bwd labels in this function for a constraint.
              auto [frange_begin, frange_end] =
                cfg.func_label_range[cfg.labels[i].function];
              for (size_t j = frange_begin; j < frange_end; j++)
              {
                auto bwd_it = bwd[j].find(loc);
                if (
                  bwd_it != bwd[j].end() &&
                  !is_default_type(bwd_it->second.type))
                {
                  auto prim = extract_primitive(bwd_it->second.type);
                  if (prim)
                  {
                    bool compat =
                      (is_default_int(env_it->second.type) &&
                       prim->in(integer_types)) ||
                      (is_default_float(env_it->second.type) &&
                       prim->in(float_types));
                    if (compat)
                    {
                      final_type = bwd_it->second.type;
                      break;
                    }
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
              auto type_tok = default_literal_type(lit);
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
          // Check fwd_exit first, then fwd[entry] (for params pushed
          // via push_param_type that didn't get a final forward run).
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
            // Fallback: check fwd[entry_label] for cross-function pushes.
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
                  bool found = false;
                  for (size_t j = range.first; j < range.second; j++)
                    if (fwd_exit[j].find(loc) != fwd_exit[j].end())
                    {
                      found = true;
                      break;
                    }
                  if (!found)
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
          for (auto& pd : *(func / Params))
          {
            if ((pd / Type)->front() == TypeVar)
            {
              func->parent()->replace(
                func,
                err(pd / Ident, "Cannot infer type of parameter"));
              break;
            }
          }
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

    void run()
    {
      active_method_cache = &method_cache_storage;
      build();
      solve();
      finalize();
      active_method_cache = nullptr;
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

      // Sweep: AngelicSubtype -> default, DefaultInt -> u64,
      // DefaultFloat -> f64.
      top->traverse([](Node& node) {
        if (node == AngelicSubtype)
        {
          auto parent = node->parent();
          if (parent && !node->empty())
          {
            auto bound = node->front();
            if (bound == DefaultInt)
              parent->replace(node, primitive_type(U64)->front());
            else if (bound == DefaultFloat)
              parent->replace(node, primitive_type(F64)->front());
            else
              parent->replace(node, clone(bound));
          }
          return false;
        }
        if (node->in({DefaultInt, DefaultFloat}))
        {
          auto parent = node->parent();
          bool is_int = (node == DefaultInt);
          if (parent == Const)
            parent->replace(node, is_int ? Node{U64} : Node{F64});
          else
            parent->replace(
              node,
              is_int ? primitive_type(U64)->front() :
                       primitive_type(F64)->front());
          return false;
        }
        return true;
      });

      return 0;
    });

    return p;
  }
}
