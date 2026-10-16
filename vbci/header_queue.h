#pragma once

#include "header.h"

namespace vbci
{
  struct HeaderQueue
  {
    Header* first = nullptr;
    Header* last = nullptr;

    bool empty() const
    {
      return first == nullptr;
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

    Header* release()
    {
      auto* headers = first;
      first = nullptr;
      last = nullptr;
      return headers;
    }
  };
}
