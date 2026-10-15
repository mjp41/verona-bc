#include "region_rc.h"

#include "array.h"
#include "object.h"

namespace vbci
{
  Object* RegionRC::object(Class& cls)
  {
    auto mem = new uint8_t[cls.size];
    auto state = HeaderState::mutable_();
    auto obj = Object::create(mem, cls, state);
    headers.emplace(obj);
    return obj;
  }

  Array* RegionRC::array(uint32_t type_id, size_t size)
  {
    auto content_type_id = Program::get().unarray(type_id);
    auto rep = Program::get().layout_type_id(content_type_id);
    auto mem = new uint8_t[Array::size_of(size, rep.second->size)];
    auto state = HeaderState::mutable_();
    auto arr =
      Array::create(mem, state, type_id, rep.first, size, rep.second->size);
    headers.emplace(arr);
    return arr;
  }

  void RegionRC::insert(Header* h)
  {
    LOG(Trace) << "Inserting header @" << h << " into RegionRC @" << this;
    headers.emplace(h);
  }

  bool RegionRC::remove(Header* h)
  {
    return headers.erase(h) != 0;
  }

  bool RegionRC::begin_finalizing()
  {
    if (finalizing)
      return false;

    finalizing = true;
    return true;
  }

  Header* RegionRC::take_header()
  {
    assert(finalizing);

    if (headers.empty())
      return nullptr;

    auto it = headers.begin();
    auto* header = *it;
    headers.erase(it);
    return header;
  }

  void RegionRC::destroy()
  {
    assert(headers.empty());
    delete this;
  }

  void RegionRC::absorb(Region* source_region)
  {
    assert(source_region != this);
    auto* source = static_cast<RegionRC*>(source_region);
    assert(!source->finalizing);

    headers.merge(source->headers);
    delete source;
  }
}
