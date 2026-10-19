#include "copy_graph.h"

#include "array.h"
#include "collect.h"
#include "cown.h"
#include "header_queue.h"
#include "object.h"
#include "program.h"
#include "thread.h"
#include "traverse.h"

#include <unordered_map>
#include <vector>

namespace vbci
{
  namespace
  {
    void destroy_isolated_graph(Register root)
    {
      if (!root->is_header())
        return;

      auto* root_header = root->get_header();
      auto state = root_header->state();
      if (state.is_shared() || state.is_immortal())
        return;

      root.extract();
      assert(state.is_mutable());
      root_header->set_state(HeaderState::finalizing());

      HeaderQueue finalizing;
      auto claim_child = [&](Header* header) {
        auto state = header->state();
        if (state.is_finalizing() || state.is_shared() || state.is_immortal())
          return false;

        assert(state.is_mutable());
        header->set_state(HeaderState::finalizing());
        return true;
      };

      traverse_intrusive(
        root_header,
        claim_child,
        [&](Header* header) { finalizing.push_front(header); });

      collect_finalizing(std::move(finalizing));
    }

    struct GraphCopy
    {
      std::unordered_map<Header*, Header*> copies;
      std::vector<std::pair<Function*, Object*>> post_copies;

      Header* allocate(Header* source)
      {
        auto& program = Program::get();
        Header* copy;

        if (program.is_array(source->get_type_id()))
        {
          auto source_array = static_cast<Array*>(source);
          copy =
            Array::create(source->get_type_id(), source_array->get_size());
        }
        else
        {
          auto source_object = static_cast<Object*>(source);
          copy = Object::create(source_object->cls());
        }

        copies.emplace(source, copy);
        return copy;
      }

      Register map_value(Value value)
      {
        auto source = value.get_base_header();

        // Cowns still need a new reference; primitives make this a no-op.
        if (!source)
          return ValueBorrow(value);

        auto state = source->state();
        if (state.is_shared() || state.is_immortal())
          return ValueTransfer(value);

        auto target = copies.at(source);

        switch (value.get_value_type())
        {
          case ValueType::Object:
            return ValueTransfer(Value(static_cast<Object*>(target)));

          case ValueType::Array:
            return ValueTransfer(Value(static_cast<Array*>(target)));

          case ValueType::FieldRef:
          case ValueType::ArrayRef:
            return ValueTransfer(value.remap_reference(target));

          default:
            Value::error(Error::BadConversion);
        }
      }

      void fill(Header* source)
      {
        auto& program = Program::get();
        auto copy = copies.at(source);

        if (program.is_array(source->get_type_id()))
        {
          auto source_array = static_cast<Array*>(source);
          auto copy_array = static_cast<Array*>(copy);

          for (size_t i = 0; i < source_array->get_size(); i++)
            copy_array->initialize(i, map_value(source_array->load(i)));
        }
        else
        {
          auto source_object = static_cast<Object*>(source);
          auto copy_object = static_cast<Object*>(copy);

          for (size_t i = 0; i < source_object->cls().fields.size(); i++)
            copy_object->initialize(i, map_value(source_object->load(i)));

          if (auto post_copy = copy_object->cls().post_copy())
            post_copies.emplace_back(post_copy, copy_object);
        }
      }

      Register run(ValueBorrow root)
      {
        auto root_header = root.get_header();
        traverse(
          root_header,
          [&](Header* source) {
            auto state = source->state();
            if (state.is_shared() || state.is_immortal())
            {
              source->inc();
              return false;
            }

            if (auto copy = copies.find(source); copy != copies.end())
            {
              copy->second->inc();
              return false;
            }

            allocate(source);
            return true;
          },
          [&](Header* source) { fill(source); });

        // A hook can follow a back edge to a copy that has not yet been filled.
        for (auto [post_copy, copy] : post_copies)
          Thread::run_sync(post_copy, ValueBorrow(copy));

        collect_mutable_cycles();

        return map_value(root);
      }
    };

    IsolatedGraph copy_graph(ValueBorrow root)
    {
      if (!root.is_header())
        return IsolatedGraph(Register(root));

      auto state = root.get_header()->state();
      if (state.is_shared() || state.is_immortal())
        return IsolatedGraph(Register(root));

      collect_mutable_cycles();

      auto copy = GraphCopy().run(root);
      return IsolatedGraph(std::move(copy));
    }

  }

  IsolatedGraph::~IsolatedGraph()
  {
    reset();
  }

  IsolatedGraph& IsolatedGraph::operator=(IsolatedGraph&& other) noexcept
  {
    if (this == &other)
      return *this;

    reset();
    root = std::move(other.root);
    return *this;
  }

  void IsolatedGraph::reset()
  {
    destroy_isolated_graph(std::move(root));
  }

  IsolatedGraph copy_graph_and_consume(Register root)
  {
    IsolatedGraph copy = copy_graph(root.borrow());

    SuppressFinalizers suppress;
    root.clear();
    collect_mutable_cycles();
    return copy;
  }

  void copy_graph_and_consume(Cown& cown)
  {
    IsolatedGraph copy = copy_graph(cown.load());
    Register source = cown.exchange<true>(copy.release());

    SuppressFinalizers suppress;
    source.clear();
    collect_mutable_cycles();
  }
}
