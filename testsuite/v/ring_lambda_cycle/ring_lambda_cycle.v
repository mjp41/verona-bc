// Ring 6: Lambdas with cyclic data dependencies.
// step_a reads var b (written by step_b).
// step_b calls step_a and reads var a (written by step_a).
// Data flow graph: step_a depends on step_b's writes,
// step_b depends on step_a (calls it).
// Tests: the inference algorithm handles cyclic data flow
// between lambdas through shared mutable state.

main(): i32
{
  var result: i32 = 0;
  var a: i32 = 0;
  var b: i32 = 0;

  let step_a = (): none -> { a = b + 1 }
  let step_b = (): none -> { step_a(); b = a + 1 }

  step_b();
  // step_a: a = 0+1 = 1, then step_b: b = 1+1 = 2
  if a != 1 { result = result + 1 }
  if b != 2 { result = result + 2 }

  step_b();
  // step_a: a = 2+1 = 3, then step_b: b = 3+1 = 4
  if a != 3 { result = result + 4 }
  if b != 4 { result = result + 8 }

  result
}
