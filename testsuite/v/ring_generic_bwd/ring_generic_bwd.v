// Ring 8: Generic function where type parameters are inferrable
// from where the output flows (backward inference).
// wrap[T](val: T): wrapper[T] initially infers T = u64 from
// the default literal. unwrap_i32 expects wrapper[i32],
// triggering backward refinement of T from u64 to i32.
// Tests: backward TypeArg inference from expected result type.

wrapper[T]
{
  val: T;

  create(val: T): wrapper[T]
  {
    new {val}
  }

  get(self: wrapper[T]): T
  {
    self.val
  }
}

wrap[T](val: T): wrapper[T]
{
  wrapper(val)
}

unwrap_i32(w: wrapper[i32]): i32
{
  w.get
}

main(): i32
{
  var result: i32 = 0;
  let w = ring_generic_bwd::wrap(42);
  let v = ring_generic_bwd::unwrap_i32(w);
  if v != 42 { result = result + 1 }
  result
}
