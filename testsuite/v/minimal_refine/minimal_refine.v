wrap[T](x: T): T { x }

consume(a: i32, b: i32): i32 { a + b }

main(): i32
{
  let x = 1 + 2;
  let w = minimal_refine::wrap(x);
  minimal_refine::consume(w, 5)
}
