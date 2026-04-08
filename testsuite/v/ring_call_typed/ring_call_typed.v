// Ring 2: Single function calling other functions with defined
// argument and return types. No lambdas, no generics.
// Tests: forward type propagation through inter-function calls
// where all callee signatures are fully annotated.

add(a: i32, b: i32): i32
{
  a + b
}

negate(x: i32): i32
{
  let zero: i32 = 0;
  zero - x
}

main(): i32
{
  var result: i32 = 0;
  let sum: i32 = ring_call_typed::add(10, 20);
  if sum != 30 { result = result + 1 }
  let neg: i32 = ring_call_typed::negate(30);
  let check: i32 = ring_call_typed::add(neg, 30);
  if check != 0 { result = result + 2 }
  result
}
