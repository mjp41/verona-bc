#include "copy_graph.h"

#include "array.h"
#include "collect.h"
#include "cown.h"
#include "object.h"
#include "program.h"
#include "region.h"
#include "thread.h"
#include "traverse.h"

#include <unordered_map>
#include <vector>

namespace vbci
{
  namespace
  {
    struct GraphCopy
    {
      Region& region;
      std::unordered_map<Header*, Header*> copies;
      std::vector<std::pair<Function*, Object*>> post_copies;

      GraphCopy(Region& region) : region(region) {}

      Header* allocate(Header* source)
      {
        auto& program = Program::get();
        Header* copy;

        if (program.is_array(source->get_type_id()))
        {
          auto source_array = static_cast<Array*>(source);
          copy = region.array(source->get_type_id(), source_array->get_size());
        }
        else
        {
          auto source_object = static_cast<Object*>(source);
          copy = region.object(source_object->cls());
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
        Thread::ScopeAllocationGroup scope(region);
        for (auto [post_copy, copy] : post_copies)
          Thread::run_sync(post_copy, ValueBorrow(copy));

        return map_value(root);
      }
    };

    Register copy_graph(ValueBorrow root, Region& region)
    {
      if (!root.is_header())
        return root;

      auto state = root.get_header()->state();
      if (state.is_shared() || state.is_immortal())
        return root;

      return GraphCopy(region).run(root);
    }

  }

  Register copy_graph_and_consume(Register root, Region& destination)
  {
    Register copy = copy_graph(root.borrow(), destination);

    SuppressFinalizers suppress;
    root.clear();
    return copy;
  }

  void copy_graph_and_consume(Cown& cown)
  {
    auto group = AllocationGroup::create();
    Register copy = copy_graph(cown.load(), group.get());
    Register source = cown.exchange<true>(std::move(copy));
    cown.set_allocation_group(std::move(group));

    SuppressFinalizers suppress;
    source.clear();
  }
}
