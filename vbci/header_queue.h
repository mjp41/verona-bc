#pragma once

#include "header.h"

namespace vbci
{
  struct HeaderQueue
  {
    Header* first = nullptr;
    Header* last = nullptr;

    HeaderQueue() = default;

    HeaderQueue(HeaderQueue&& other) noexcept
    : first(other.first), last(other.last)
    {
      other.first = nullptr;
      other.last = nullptr;
    }

    HeaderQueue(const HeaderQueue&) = delete;
    HeaderQueue& operator=(const HeaderQueue&) = delete;
    HeaderQueue& operator=(HeaderQueue&&) = delete;

    bool empty() const
    {
      return first == nullptr;
    }

    void append(HeaderQueue queue)
    {
      if (queue.empty())
        return;

      if (last)
        last->set_next(queue.first);
      else
        first = queue.first;

      last = queue.last;
      queue.first = nullptr;
      queue.last = nullptr;
    }

    void push(Header* header)
    {
      header->set_next(nullptr);

      if (last)
        last->set_next(header);
      else
        first = header;

      last = header;
    }

    void push_front(Header* header)
    {
      header->set_next(first);
      first = header;
      if (!last)
        last = header;
    }

    Header* pop()
    {
      assert(first);
      auto* header = first;
      first = header->next();
      if (!first)
        last = nullptr;
      return header;
    }

  };
}
