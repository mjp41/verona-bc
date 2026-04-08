// Ring 9: Integer constants constrained by forward use context.
// Default-typed literals (0, 10, 20, ...) are refined to i32
// by flowing into typed bindings or function parameters.
// Tests: backward refinement of integer constant types through
// arithmetic operations and variable annotations.

add_i32(a: i32, b: i32): i32
{
  a + b
}

main(): i32
{
  var result: i32 = 0;

  // Literal 10 and 20 are default ints.
  // a + b produces a default int result.
  // x: i32 annotation backward-refines everything to i32.
  let a = 10;
  let b = 20;
  let x: i32 = a + b;
  if x != 30 { result = result + 1 }

  // Literal 5 flows to add_i32(a: i32, ...), so it is
  // refined to i32 by the function parameter type.
  let y = ring_int_constrained::add_i32(5, x);
  if y != 35 { result = result + 2 }

  // Chain: 1 + 2 → default int, then flows to z: i32.
  let z: i32 = 1 + 2;
  if z != 3 { result = result + 4 }

  result
}
