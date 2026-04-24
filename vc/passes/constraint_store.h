// ===== Constraint Store for Forward-Only Type Inference =====
//
// Each unresolved type gets a fresh TypeVarId (index into the store).
// The ID is embedded in an AngelicSubtype AST node's location string.
//
// The store tracks per-variable:
//   - concrete: must resolve to exactly one type (literals)
//   - member_set: allowed primitive types (e.g., IntSet, FloatSet),
//     progressively tightened by constraints. Singleton = resolved.
//   - upper_bounds: 'a <: T constraints from use sites
//   - lower_bounds: T <: 'a constraints from definition sites
//   - observers: labels to re-enqueue when member_set tightens
//   - source_stmt: AST node that created this variable (for stability map)
//
// Tightening triggers when:
//   - member_set ∩ upper_bounds shrinks the set
//   - non-concrete + single upper bound
//
// Observer notifications fire on any set shrinkage, enabling
// incremental cascading before full resolution.

#pragma once

#include "../lang.h"

#include <functional>
#include <optional>
#include <vector>

namespace vc
{
  using TypeVarId = size_t;

  struct ConstraintEntry
  {
    bool concrete = false; // Must resolve to one type (literals).
    std::vector<Token> member_set; // Allowed primitives, tightened over time.
    std::vector<Node> upper_bounds; // 'a <: T (from use sites).
    std::vector<Node> lower_bounds; // T <: 'a (from definition sites).
    std::vector<size_t> observers; // Labels to re-enqueue on tightening.
  };

  // Callback type for observer notification.
  using EnqueueCallback = std::function<void(size_t label)>;

  struct ConstraintStore
  {
    std::vector<ConstraintEntry> entries;

    // Create a fresh type variable.
    TypeVarId fresh(
      bool is_concrete,
      std::vector<Token> members)
    {
      TypeVarId id = entries.size();
      ConstraintEntry entry;
      entry.concrete = is_concrete;
      entry.member_set = std::move(members);
      entries.push_back(std::move(entry));
      return id;
    }

    // Get the current member set for a variable.
    const std::vector<Token>& member_set(TypeVarId var) const
    {
      static const std::vector<Token> empty;
      return var < entries.size() ? entries[var].member_set : empty;
    }

    // Get the upper bounds for a variable.
    const std::vector<Node>& upper_bounds(TypeVarId var) const
    {
      static const std::vector<Node> empty;
      return var < entries.size() ? entries[var].upper_bounds : empty;
    }

    // Get the lower bounds for a variable.
    const std::vector<Node>& lower_bounds(TypeVarId var) const
    {
      static const std::vector<Node> empty;
      return var < entries.size() ? entries[var].lower_bounds : empty;
    }

    // Add an upper bound: 'a <: T.
    // Returns true if the member set was tightened or observers notified.
    bool add_upper_bound(
      TypeVarId var,
      Node type,
      const EnqueueCallback& enqueue = {})
    {
      if (var >= entries.size())
        return false;
      auto& e = entries[var];

      // Check if this is a genuinely new upper bound.
      bool is_new = true;
      for (auto& existing : e.upper_bounds)
        if (structural_eq(existing, type))
        { is_new = false; break; }

      if (!is_new)
        return false;

      e.upper_bounds.push_back(clone(type));
      bool tightened = tighten(var, enqueue);

      // For non-concrete variables (fields, returns), notify
      // observers on any new upper bound, even if tighten didn't
      // find a member_set change. The observer may read the
      // upper bounds directly.
      if (is_new && !tightened && !e.concrete)
        notify(e, enqueue);

      return tightened || is_new;
    }

    // Add a lower bound: T <: 'a.
    // Returns true if the member set was tightened.
    bool add_lower_bound(
      TypeVarId var,
      Node type,
      const EnqueueCallback& enqueue = {})
    {
      if (var >= entries.size())
        return false;
      auto& e = entries[var];

      // Deduplicate.
      for (auto& existing : e.lower_bounds)
        if (structural_eq(existing, type))
          return false;

      e.lower_bounds.push_back(clone(type));
      return tighten(var, enqueue);
    }

    // Register a label as an observer of this variable.
    void add_observer(TypeVarId var, size_t label)
    {
      if (var >= entries.size())
        return;
      auto& obs = entries[var].observers;
      for (auto o : obs)
        if (o == label)
          return; // Already registered.
      obs.push_back(label);
    }

    size_t size() const { return entries.size(); }

  private:
    // Deep structural comparison of two AST nodes.
    static bool structural_eq(const Node& a, const Node& b)
    {
      if (a == b)
        return true;
      if (!a || !b)
        return false;
      if (a->type() != b->type())
        return false;
      if (a->size() != b->size())
        return false;
      if (a == Ident)
        return a->location().view() == b->location().view();
      for (size_t i = 0; i < a->size(); i++)
        if (!structural_eq(a->at(i), b->at(i)))
          return false;
      return true;
    }

    // Tighten the member set from constraints.
    // Notifies observers on any shrinkage.
    bool tighten(TypeVarId var, const EnqueueCallback& enqueue)
    {
      auto& e = entries[var];

      // Collect candidate primitives from upper bounds.
      std::vector<Token> ub_prims;
      for (auto& ub : e.upper_bounds)
      {
        auto toks = extract_primitive_tokens(ub);
        for (auto& t : toks)
        {
          bool dup = false;
          for (auto& u : ub_prims)
            if (u == t)
            {
              dup = true;
              break;
            }
          if (!dup)
            ub_prims.push_back(t);
        }
      }

      if (e.concrete && !e.member_set.empty() && !ub_prims.empty())
      {
        // Intersect member_set with upper bound primitives.
        std::vector<Token> candidates;
        for (auto& m : e.member_set)
          for (auto& u : ub_prims)
            if (m == u)
              candidates.push_back(m);

        if (!candidates.empty() && candidates.size() < e.member_set.size())
        {
          e.member_set = std::move(candidates);
          notify(e, enqueue);
          return true;
        }
      }
      else if (!e.concrete && !ub_prims.empty())
      {
        // Non-concrete (type param): if all upper bounds agree on
        // one type, set member_set to singleton.
        if (ub_prims.size() == 1 && e.member_set.empty())
        {
          e.member_set = {ub_prims[0]};
          notify(e, enqueue);
          return true;
        }
      }

      // Non-primitive upper bounds with no member set:
      // if all agree structurally, notify observers.
      if (e.member_set.empty() && !e.upper_bounds.empty())
      {
        bool all_agree = true;
        for (size_t i = 1; i < e.upper_bounds.size(); i++)
        {
          if (!structural_eq(e.upper_bounds[i], e.upper_bounds[0]))
          {
            all_agree = false;
            break;
          }
        }
        if (all_agree)
        {
          notify(e, enqueue);
          return true;
        }
      }

      return false;
    }

    void notify(ConstraintEntry& e, const EnqueueCallback& enqueue)
    {
      if (enqueue)
        for (auto label : e.observers)
          enqueue(label);
    }

    // Extract primitive tokens from a Type node.
    static std::vector<Token> extract_primitive_tokens(const Node& type)
    {
      std::vector<Token> result;
      if (type != Type)
        return result;
      auto inner = type->front();

      if (inner == TypeName)
      {
        auto tok = extract_single_primitive(inner);
        if (tok.has_value())
          result.push_back(tok.value());
      }
      else if (inner == Union)
      {
        for (auto& child : *inner)
        {
          if (child == TypeName)
          {
            auto tok = extract_single_primitive(child);
            if (tok.has_value())
            {
              bool dup = false;
              for (auto& r : result)
                if (r == tok.value())
                {
                  dup = true;
                  break;
                }
              if (!dup)
                result.push_back(tok.value());
            }
          }
        }
      }
      return result;
    }

    static std::optional<Token> extract_single_primitive(const Node& typename_node)
    {
      static const std::map<std::string_view, Token> prim_map = {
        {"none", None}, {"bool", Bool},
        {"i8", I8}, {"i16", I16}, {"i32", I32}, {"i64", I64},
        {"u8", U8}, {"u16", U16}, {"u32", U32}, {"u64", U64},
        {"ilong", ILong}, {"ulong", ULong},
        {"isize", ISize}, {"usize", USize},
        {"f32", F32}, {"f64", F64},
      };

      if (typename_node->size() < 2)
        return std::nullopt;
      auto first = (typename_node->front() / Ident)->location().view();
      if (first != "_builtin")
        return std::nullopt;
      auto name = (typename_node->back() / Ident)->location().view();
      auto it = prim_map.find(name);
      if (it != prim_map.end())
        return it->second;
      return std::nullopt;
    }

    static Node make_prim_type(const Token& tok)
    {
      return Type
        << (TypeName << (NameElement << (Ident ^ "_builtin") << TypeArgs)
                     << (NameElement << (Ident ^ tok.str()) << TypeArgs));
    }
  };

  // ===== AngelicSubtype node helpers =====

  // Build an AngelicSubtype node carrying a TypeVarId.
  // The ID is stored in the node's location.
  // Child nodes carry the local bound information.
  inline Node make_angelic_var(
    TypeVarId id,
    bool is_concrete,
    const std::vector<Token>& member_set)
  {
    // Add member set indicator.
    // Check for full IntSet/FloatSet.
    static const std::vector<Token> int_members = {
      I8, I16, I32, I64, U8, U16, U32, U64, ILong, ULong, ISize, USize};
    static const std::vector<Token> float_members = {F32, F64};

    bool is_full_intset = (member_set.size() == int_members.size());
    if (is_full_intset)
      for (auto& t : int_members)
      {
        bool found = false;
        for (auto& m : member_set)
          if (m == t)
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

    bool is_full_floatset = (member_set.size() == float_members.size());
    if (is_full_floatset)
      for (auto& t : float_members)
      {
        bool found = false;
        for (auto& m : member_set)
          if (m == t)
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

    // Build the bound node. For non-concrete with no member set,
    // use an empty AngelicSubtype (unconstrained variable).
    Node bound;
    if (is_concrete && (is_full_intset || is_full_floatset))
    {
      bound = Isect;
      bound << Concrete;
      if (is_full_intset)
        bound << IntSet;
      else
        bound << FloatSet;
    }
    else if (is_concrete)
    {
      bound = Isect;
      bound << Concrete;
    }
    else if (is_full_intset)
    {
      bound = IntSet;
    }
    else if (is_full_floatset)
    {
      bound = FloatSet;
    }
    else
    {
      // Unconstrained variable — use TypeVar as sentinel bound.
      bound = TypeVar;
    }

    auto angelic = AngelicSubtype ^ std::to_string(id);
    angelic << bound;
    return Type << angelic;
  }

  // Extract the TypeVarId from an AngelicSubtype node.
  // Returns nullopt if the node is not an AngelicSubtype with an ID.
  inline std::optional<TypeVarId> get_angelic_var_id(const Node& type)
  {
    if (!type || type != Type || type->empty())
      return std::nullopt;
    auto inner = type->front();
    if (inner != AngelicSubtype)
      return std::nullopt;
    auto loc = inner->location().view();
    if (loc.empty())
      return std::nullopt; // Old-style angelic without ID.
    TypeVarId id = 0;
    for (char c : loc)
    {
      if (c < '0' || c > '9')
        return std::nullopt;
      id = id * 10 + (c - '0');
    }
    return id;
  }

} // namespace vc
