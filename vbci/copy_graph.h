#pragma once

#include "register.h"

#include <utility>

namespace vbci
{
  /**
   * Owns a fresh mutable copy whose nodes have no external mutable references.
   * Releasing transfers the root; otherwise destruction reclaims the complete
   * reachable mutable graph without trial deletion.
   */
  struct IsolatedGraph
  {
  private:
    Register root;

    void reset();

  public:
    IsolatedGraph() = default;
    explicit IsolatedGraph(Register root) : root(std::move(root)) {}
    ~IsolatedGraph();
    IsolatedGraph(IsolatedGraph&& other) noexcept
    : root(std::move(other.root))
    {}

    IsolatedGraph& operator=(IsolatedGraph&& other) noexcept;

    IsolatedGraph(const IsolatedGraph&) = delete;
    IsolatedGraph& operator=(const IsolatedGraph&) = delete;

    ValueBorrow borrow() const
    {
      return root.borrow();
    }

    [[nodiscard]] Register release()
    {
      return std::move(root);
    }
  };

  [[nodiscard]] IsolatedGraph copy_graph(ValueBorrow root);

  /**
   * Copy a graph and consume the source reference without finalizing objects
   * whose semantic lifetimes continue in the copy.
   */
  [[nodiscard]] IsolatedGraph copy_graph_and_consume(Register root);
}
