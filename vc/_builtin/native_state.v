native_state[T]
{
  create(value: T): native_state[T]
  {
    :::newnative[T](value)
  }

  with[R](self: native_state[T], handler: T->R): R
  {
    :::nativewith[R](self, handler)
  }
}
