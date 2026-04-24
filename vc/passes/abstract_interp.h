// ===== Abstract Interpretation Framework (Forward-Only) =====
//
// A generic interprocedural forward abstract interpretation engine.
// Parameterized on a Domain that provides the lattice and transfer functions.
//
// "Backward" information flows through constraint variables in the domain,
// NOT through a separate backward analysis. The framework only manages
// forward environments; the domain's ConstraintStore handles constraints
// that flow in the "backward" direction via observer notifications.
//
// The framework operates on opaque abstract environments (Env). It never
// inspects their contents — it only passes them to domain-provided
// operations (join, clone, transfer).
//
// The domain receives a reference to the framework during transfer
// functions, so it can:
//   - Read the current fwd state of any label
//   - Push forward environments to arbitrary labels (cross-function flow)
//   - Enqueue labels for reprocessing (observer notifications)
//
// State per label:
//   fwd_[label] — join of predecessor outputs (entry state)
//
// Transfer function output is pushed directly to successors,
// not stored persistently. The constraint store and side tables
// hold all information needed for finalization.
//
// Worklist ordering:
//   Forward: ascending RPO (predecessors before successors).
//   Observer re-enqueue events are inserted into the same worklist.

#pragma once

#include "../lang.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace vc
{
  // ===== CFG =====
  //
  // Immutable after build(). Owns the global label array, successor/
  // predecessor edges, per-function label indices, and RPO ordering.

  struct CFG
  {
    struct LabelInfo
    {
      Node function;
      Node label;
    };

    std::vector<LabelInfo> labels;
    std::map<Node, std::pair<size_t, size_t>> func_label_range;
    std::vector<std::vector<size_t>> succ;
    std::map<Node, std::map<std::string, size_t>> func_label_idx;
    std::vector<size_t> rpo_index;

    size_t size() const { return labels.size(); }

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

      compute_rpo();
    }

  private:
    void compute_rpo()
    {
      size_t n = labels.size();
      rpo_index.resize(n, 0);
      std::vector<bool> visited(n, false);
      std::vector<size_t> post_order;
      post_order.reserve(n);

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

      for (size_t i = 0; i < n; i++)
        if (!visited[i])
          post_order.push_back(i);

      for (size_t i = 0; i < post_order.size(); i++)
        rpo_index[post_order[post_order.size() - 1 - i]] = i;
    }
  };

  // Forward declaration.
  template <typename Domain>
  struct AbstractInterpreter;

  // ===== FinalizeContext =====

  template <typename Env>
  struct FinalizeContext
  {
    const CFG& cfg;
    const std::vector<Env>& fwd;
    const std::map<Node, size_t>& func_entry;
    const std::map<Node, std::vector<size_t>>& func_returns;
  };

  // ===== Domain Concept (Forward-Only) =====
  //
  //   using Env = ...;
  //
  //   Env empty_env() const;
  //   Env clone_env(const Env&) const;
  //
  //   bool join(Env& target, const Env& incoming) const;
  //
  //   void forward_transfer(
  //     Env& env,              // in: fwd_entry, out: fwd_exit
  //     const Node& body,
  //     const Node& function,
  //     size_t label_idx,
  //     AbstractInterpreter<Self>& ai);
  //
  //   void split_cond(
  //     const Node& cond,
  //     const Node& body,
  //     const Env& fwd_exit,
  //     Env& true_env,         // pre-cloned from fwd_exit; narrow in place
  //     Env& false_env);       // pre-cloned from fwd_exit; narrow in place
  //
  //   void seed(AbstractInterpreter<Self>& ai);
  //   void finalize(FinalizeContext<Env>& ctx);

  // ===== AbstractInterpreter =====

  constexpr size_t DEFAULT_MAX_ITERS_PER_LABEL = 200;

  template <typename Domain>
  struct AbstractInterpreter
  {
    using Env = typename Domain::Env;

    AbstractInterpreter(const AbstractInterpreter&) = delete;
    AbstractInterpreter& operator=(const AbstractInterpreter&) = delete;
    AbstractInterpreter(AbstractInterpreter&&) = delete;
    AbstractInterpreter& operator=(AbstractInterpreter&&) = delete;

    AbstractInterpreter(Node top, Domain domain)
      : top_(std::move(top)), domain_(std::move(domain))
    {}

    // --- Push interface (used by domain during transfer) ---

    bool push_fwd(size_t label, const Env& env)
    {
      assert(label < fwd_.size());
      if (domain_.join(fwd_[label], env))
      {
        enqueue(label);
        return true;
      }
      return false;
    }

    // Enqueue a label for reprocessing (e.g., observer notification).
    void enqueue(size_t label)
    {
      assert(label < fwd_.size());
      worklist_->insert({label});
    }

    // --- Read accessors (used by domain during transfer) ---

    const Env& get_fwd(size_t label) const { return fwd_[label]; }

    const CFG& cfg() const { return cfg_; }
    const std::map<Node, size_t>& func_entry() const { return func_entry_; }
    const std::map<Node, std::vector<size_t>>& func_returns() const
    {
      return func_returns_;
    }

    // --- Phased API ---

    void build()
    {
      cfg_.build(top_);
      size_t n = cfg_.size();
      fwd_.resize(n);
      for (size_t i = 0; i < n; i++)
        fwd_[i] = domain_.empty_env();

      // Build func_entry_ and func_returns_.
      for (auto& [func, range] : cfg_.func_label_range)
      {
        func_entry_[func] = range.first;
        for (size_t i = range.first; i < range.second; i++)
        {
          auto term = cfg_.labels[i].label / Return;
          if (term == Return)
            func_returns_[func].push_back(i);
        }
      }

      // Initialize worklist.
      wi_cmp_.rpo = &cfg_.rpo_index;
      worklist_.emplace(wi_cmp_);
    }

    void seed() { domain_.seed(*this); }

    void finalize()
    {
      FinalizeContext<Env> ctx{
        cfg_, fwd_, func_entry_, func_returns_};
      domain_.finalize(ctx);
    }

    bool run()
    {
      build();
      seed();
      bool ok = solve();
      finalize();
      return ok;
    }

    size_t max_iterations = 0;

  private:
    Node top_;
    Domain domain_;
    CFG cfg_;

    std::vector<Env> fwd_;

    std::map<Node, size_t> func_entry_;
    std::map<Node, std::vector<size_t>> func_returns_;

    // --- Worklist ---

    struct WorkItem
    {
      size_t label;
    };

    struct WorkItemCompare
    {
      const std::vector<size_t>* rpo = nullptr;
      bool operator()(const WorkItem& a, const WorkItem& b) const
      {
        auto ra = (*rpo)[a.label];
        auto rb = (*rpo)[b.label];
        // Ascending RPO (predecessors first).
        return (ra != rb) ? (ra < rb) : (a.label < b.label);
      }
    };

    WorkItemCompare wi_cmp_;
    std::optional<std::set<WorkItem, WorkItemCompare>> worklist_;

    bool dequeue(size_t& label)
    {
      if (!worklist_ || worklist_->empty())
        return false;
      auto it = worklist_->begin();
      label = it->label;
      worklist_->erase(it);
      return true;
    }

    // --- Solve ---

    bool solve()
    {
      size_t n = cfg_.size();
      if (n == 0)
        return true;

      size_t cap = max_iterations > 0
        ? max_iterations
        : n * DEFAULT_MAX_ITERS_PER_LABEL;
      size_t iters = 0;
      size_t label;

      while (dequeue(label))
      {
        if (++iters > cap)
          return false;

        const auto& li = cfg_.labels[label];
        auto body = li.label / Body;
        auto func = li.function;

        // Clone entry, run transfer on the clone.
        Env env = domain_.clone_env(fwd_[label]);
        domain_.forward_transfer(env, body, func, label, *this);

        // Push output to successors.
        auto term = li.label / Return;
        if (term == Cond)
        {
          auto fi = cfg_.func_label_idx.find(func);
          assert(fi != cfg_.func_label_idx.end());
          auto& func_idx = fi->second;
          auto t_it = func_idx.find(
            std::string((term / Lhs)->location().view()));
          auto f_it = func_idx.find(
            std::string((term / Rhs)->location().view()));

          Env true_env = domain_.clone_env(env);
          Env false_env = domain_.clone_env(env);
          domain_.split_cond(term, body, env, true_env, false_env);

          if (t_it != func_idx.end())
            push_fwd(t_it->second, true_env);
          if (f_it != func_idx.end())
            push_fwd(f_it->second, false_env);
        }
        else if (term == Jump)
        {
          auto fi = cfg_.func_label_idx.find(func);
          assert(fi != cfg_.func_label_idx.end());
          auto& func_idx = fi->second;
          auto t_it = func_idx.find(
            std::string((term / LabelId)->location().view()));
          if (t_it != func_idx.end())
            push_fwd(t_it->second, env);
        }
      }

      return true;
    }
  };

} // namespace vc
