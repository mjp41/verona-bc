// Ring 7: Generic function where type parameters are inferrable
// from argument types (forward inference).
// identity[T](x: T): T — calling identity(x) where x: i32
// infers T = i32 from the argument.
// Tests: forward TypeArg inference from concrete argument types
// into generic function type parameters.

identity[T](x: T): T
{
  x
}

pair[A, B]
{
  first: A;
  second: B;

  create(first: A, second: B): pair[A, B]
  {
    new {first, second}
  }
}

main(): i32
{
  var result: i32 = 0;

  let x: i32 = 42;
  let a = ring_generic_fwd::identity(x);
  if a != 42 { result = result + 1 }

  let p = pair(i32 10, i32 20);
  if p.first != 10 { result = result + 2 }
  if p.second != 20 { result = result + 4 }

  result
}
