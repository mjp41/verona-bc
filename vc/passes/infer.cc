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
//   - If fwd is TypeVar and bwd is a single concrete type, same.
//   - Union backward constraints do NOT trigger refinement.
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
#include <set>
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

  bool is_default_type(const Node& type)
  {
    return type && !type->empty() &&
      type->front()->in({DefaultInt, DefaultFloat});
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

    // Cross-function mappings.
    std::map<Node, size_t> func_entry; // Function -> entry label index
    std::map<Node, std::vector<size_t>>
      func_returns; // Function -> return label indices

    // Shared mutable state.
    std::map<Location, Node> lookup_stmts;
    std::set<std::pair<Location, Location>> typevar_aliases;
    std::map<Location, std::pair<Location, size_t>> ref_to_tuple;

    // Method resolution cache (instance variable, not static global).
    struct MethodLookupKey
    {
      std::string owner_key;
      std::string method_name;
      std::string hand_name;
      size_t arity;
      auto operator<=>(const MethodLookupKey&) const = default;
    };
    std::map<MethodLookupKey, Node> method_cache;

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

      // TODO: Iteration 2 -- use merge_type here for proper widening.
      // For now, just keep existing type (stub).
      snmalloc::UNUSED(type);
      snmalloc::UNUSED(call_node);
      return false;
    }

    // Cross-function constraint push.
    bool push_cross(
      const Node& /*target*/, const Node& /*type*/, Direction /*dir*/)
    {
      // TODO: Iteration 3b -- implement cross-function constraint flow.
      return false;
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

      // Allocate per-label state.
      fwd.resize(n);
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

      // Enqueue all labels for initial processing.
      for (size_t i = 0; i < n; i++)
        enqueue(i, Direction::Both);
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

        bool run_fwd =
          (dir == Direction::Forward || dir == Direction::Both);
        bool run_bwd =
          (dir == Direction::Backward || dir == Direction::Both);

        // TODO: Iteration 2 -- forward transfer functions
        if (run_fwd)
        {
          // Stub: no forward processing yet.
        }

        // TODO: Iteration 3a -- backward transfer functions
        if (run_bwd)
        {
          // Stub: no backward processing yet.
        }

        // TODO: Iteration 1 -- refinement step
        // For each location in fwd[label]:
        //   if fwd is Default/TypeVar and bwd has compatible concrete:
        //     refine fwd, enqueue label Forward.

        snmalloc::UNUSED(run_fwd);
        snmalloc::UNUSED(run_bwd);
      }
    }

    void finalize()
    {
      // TODO: Iteration 4a -- write converged types to AST
      // TODO: Iteration 4b -- return types, TypeArgs, error checking
    }

    void run()
    {
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

      // Sweep: DefaultInt -> u64, DefaultFloat -> f64.
      top->traverse([](Node& node) {
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
