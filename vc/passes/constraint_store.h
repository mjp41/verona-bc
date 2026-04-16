// ===== Constraint Store for Forward-Only Type Inference =====
//
// Each unresolved type gets a fresh TypeVarId (index into the store).
// The ID is embedded in an AngelicSubtype AST node's location string.
//
// The store tracks per-variable:
//   - concrete: must resolve to exactly one type (literals)
//   - member_set: allowed primitive types (e.g., IntSet, FloatSet)
//   - upper_bounds: 'a <: T constraints from use sites
//   - lower_bounds: T <: 'a constraints from definition sites
//   - resolved: concrete type once determined
//   - observers: labels to re-enqueue on resolution
//   - source_stmt: AST node that created this variable (for stability map)
//
// Resolution triggers when:
//   - member_set ∩ upper_bounds yields a singleton
//   - concrete + single candidate
//   - lower bound equals upper bound
//
// Observer notifications fire only on resolution (not on every constraint
// addition), bounding total notifications to O(V).

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
    std::vector<Token> member_set; // Allowed primitives (IntSet, FloatSet).
    std::vector<Node> upper_bounds; // 'a <: T (from use sites).
    std::vector<Node> lower_bounds; // T <: 'a (from definition sites).
    std::optional<Node> resolved; // Resolved concrete type.
    std::vector<size_t> observers; // Labels to re-enqueue on resolution.
    Node source_stmt; // AST stmt that created this variable.
  };

  // Callback type for observer notification.
  using EnqueueCallback = std::function<void(size_t label)>;

  struct ConstraintStore
  {
    std::vector<ConstraintEntry> entries;

    // Stability map: statement → resolved type.
    // When a variable resolves, we record which stmt created it
    // so re-runs of that label use the resolved type directly
    // instead of creating a fresh variable.
    std::map<const void*, Node> stability_map;

    // Create a fresh type variable.
    TypeVarId fresh(
      bool is_concrete,
      std::vector<Token> members,
      Node stmt = {})
    {
      TypeVarId id = entries.size();
      ConstraintEntry entry;
      entry.concrete = is_concrete;
      entry.member_set = std::move(members);
      entry.source_stmt = stmt;
      entries.push_back(std::move(entry));
      return id;
    }

    // Check if a variable has been resolved.
    bool is_resolved(TypeVarId var) const
    {
      return var < entries.size() && entries[var].resolved.has_value();
    }

    // Get resolved type (or empty node).
    Node resolved(TypeVarId var) const
    {
      if (var < entries.size() && entries[var].resolved.has_value())
        return entries[var].resolved.value();
      return {};
    }

    // Get the member set for a variable.
    const std::vector<Token>& member_set(TypeVarId var) const
    {
      static const std::vector<Token> empty;
      return var < entries.size() ? entries[var].member_set : empty;
    }

    // Add an upper bound: 'a <: T.
    // Returns true if the variable was resolved by this addition.
    bool add_upper_bound(
      TypeVarId var,
      Node type,
      const EnqueueCallback& enqueue = {})
    {
      if (var >= entries.size())
        return false;
      auto& e = entries[var];
      if (e.resolved.has_value())
        return false; // Already resolved.

      e.upper_bounds.push_back(clone(type));
      return try_resolve(var, enqueue);
    }

    // Add a lower bound: T <: 'a.
    // Returns true if the variable was resolved by this addition.
    bool add_lower_bound(
      TypeVarId var,
      Node type,
      const EnqueueCallback& enqueue = {})
    {
      if (var >= entries.size())
        return false;
      auto& e = entries[var];
      if (e.resolved.has_value())
        return false;

      e.lower_bounds.push_back(clone(type));
      return try_resolve(var, enqueue);
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

    // Check the stability map for a statement.
    // Returns the previously resolved type if this stmt's variable
    // was already resolved in a prior iteration.
    Node check_stability(const Node& stmt) const
    {
      if (!stmt)
        return {};
      auto it = stability_map.find(stmt.get());
      if (it != stability_map.end())
        return clone(it->second);
      return {};
    }

    // Force-resolve a variable (used in finalize).
    void force_resolve(TypeVarId var, Node type)
    {
      if (var >= entries.size())
        return;
      auto& e = entries[var];
      e.resolved = clone(type);
      if (e.source_stmt)
        stability_map[e.source_stmt.get()] = clone(type);
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

    // Try to resolve a variable from its current constraints.
    // Called after adding a bound.
    bool try_resolve(TypeVarId var, const EnqueueCallback& enqueue)
    {
      auto& e = entries[var];
      if (e.resolved.has_value())
        return false;

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

      if (e.concrete && !e.member_set.empty())
      {
        // Intersect member_set with upper bound primitives.
        if (!ub_prims.empty())
        {
          std::vector<Token> candidates;
          for (auto& m : e.member_set)
            for (auto& u : ub_prims)
              if (m == u)
                candidates.push_back(m);

          if (candidates.size() == 1)
          {
            do_resolve(var, make_prim_type(candidates[0]), enqueue);
            return true;
          }
          // Tighten member set even if not yet singleton.
          if (!candidates.empty() && candidates.size() < e.member_set.size())
            e.member_set = std::move(candidates);
        }
      }
      else if (!e.concrete && !ub_prims.empty())
      {
        // Non-concrete (type param): if all upper bounds agree on
        // one type, resolve to it.
        if (ub_prims.size() == 1)
        {
          do_resolve(var, make_prim_type(ub_prims[0]), enqueue);
          return true;
        }
      }

      // Check if there's a non-primitive upper bound and no members.
      // Only resolve if all upper bounds are structurally identical.
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
          do_resolve(var, clone(e.upper_bounds[0]), enqueue);
          return true;
        }
      }

      return false;
    }

    void do_resolve(
      TypeVarId var,
      Node type,
      const EnqueueCallback& enqueue)
    {
      auto& e = entries[var];
      e.resolved = clone(type);

      // Record in stability map.
      if (e.source_stmt)
        stability_map[e.source_stmt.get()] = clone(type);

      // Notify observers.
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
    Node bound = Isect;
    if (is_concrete)
      bound << Concrete;

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

    if (is_full_intset)
      bound << IntSet;
    else if (is_full_floatset)
      bound << FloatSet;

    return Type
      << ((AngelicSubtype ^ std::to_string(id)) << bound);
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
