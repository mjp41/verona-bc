callback_handle
{}

callback[T]
{
  _cb: callback_handle | none;

  create(): callback[T]
  {
    new { _cb = none }
  }

  create(callable: T): callback[T]
  {
    new { _cb = :::make_callback(callable) }
  }

  bind(self: callback[T], callable: T): callback[T]
  {
    self._cb = :::make_callback(callable);
    self
  }

  raw(self: callback[T]): ptr
  {
    :::codeptr_callback(self._cb)
  }

  retain(self: callback[T]): ptr
  {
    :::retain_callback(self._cb)
  }

  release(self: callback[T], handle: ptr): none
  {
    :::release_callback(handle)
  }
}
