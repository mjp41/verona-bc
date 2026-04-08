// Ring 4: Multiple non-interacting lambdas in a single function.
// Each lambda has explicit types and operates independently.
// Tests: the inference algorithm handles multiple lambdas
// without interference between their type environments.

main(): i32
{
  var result: i32 = 0;
  let inc = (x: i32): i32 -> x + 1;
  let dec = (x: i32): i32 -> x - 1;
  let double = (x: i32): i32 -> x + x;

  let a = inc(10);
  if a != 11 { result = result + 1 }

  let b = dec(10);
  if b != 9 { result = result + 2 }

  let c = double(5);
  if c != 10 { result = result + 4 }

  result
}
