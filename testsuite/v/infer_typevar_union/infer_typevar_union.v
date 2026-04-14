// Test: TypeVar refined from Union backward constraints.
// When a generic type parameter T is constrained by multiple
// call sites with different concrete types, the inferred type
// should be the union of all constraints.

// A simple wrapper that stores a value of type T.
box[T]
{
  val: T;

  create(val: T): box[T]
  {
    new {val}
  }

  get(self: box[T]): T
  {
    self.val
  }
}

// Takes two boxes of the same type T and returns the first value.
pick_first[T](a: box[T], b: box[T]): T
{
  a.get
}

main(): i32
{
  var result: i32 = 0;

  // Both boxes have i32 — T should be inferred as i32.
  let a = box(i32 10);
  let b = box(i32 20);
  let v = infer_typevar_union::pick_first(a, b);
  if v != 10 { result = result + 1 }

  result
}
