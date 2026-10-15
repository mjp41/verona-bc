#pragma once

#include "array.h"
#include "header.h"
#include "object.h"
#include "program.h"

#include <vector>

namespace vbci
{
  namespace detail
  {
    struct NopPost
    {
      template<typename T>
      void operator()(T)
      {}
    };
  }

  template<
    typename T,
    typename Trace,
    typename Pre,
    typename Post = detail::NopPost>
  void traverse_graph(
    T root, Trace trace, Pre pre, Post post = detail::NopPost())
  {
    struct Work
    {
      T value;
      bool post;
    };

    std::vector<Work> worklist{{root, false}};

    while (!worklist.empty())
    {
      auto [value, is_post] = worklist.back();
      worklist.pop_back();

      if (is_post)
      {
        post(value);
        continue;
      }

      if (!pre(value))
        continue;

      worklist.push_back({value, true});
      trace(value, [&](T child) { worklist.push_back({child, false}); });
    }
  }

  template<typename Pre, typename Post = detail::NopPost>
  void traverse(Header* root, Pre pre, Post post = detail::NopPost())
  {
    auto trace = [](Header* header, auto add_child) {
      auto& program = Program::get();
      if (program.is_array(header->get_type_id()))
        static_cast<Array*>(header)->trace_fn(add_child);
      else
        static_cast<Object*>(header)->trace_fn(add_child);
    };

    traverse_graph(root, trace, pre, post);
  }
}
