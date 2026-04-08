// Ring 5: Multiple lambdas that invoke each other in a
// topologically sortable order (no cycles).
// triple calls double — a one-way dependency.
// Tests: the inference algorithm processes lambda dependencies
// in the correct order (double before triple).

main(): i32
{
  var result: i32 = 0;
  let double = (x: i32): i32 -> x + x;
  let triple = (x: i32): i32 -> double(x) + x;

  let a = triple(5);
  if a != 15 { result = result + 1 }

  let b = double(triple(3));
  if b != 18 { result = result + 2 }

  result
}
