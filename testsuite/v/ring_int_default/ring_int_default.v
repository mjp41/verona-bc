// Ring 10: Integer constants that are unconstrained and stay
// as default types (u64 for integers, f64 for floats).
// Tests: the inference algorithm correctly leaves unconstrained
// literals at their default types without spurious refinement.

main(): i32
{
  var result: i32 = 0;

  // a, b, c are all unconstrained — they stay as u64.
  let a = 100;
  let b = 200;
  let c = a + b;
  if c != 300 { result = result + 1 }

  // Use a u64-typed binding to verify no accidental i32 refinement.
  let d: u64 = a * b;
  if d != 20000 { result = result + 2 }

  result
}
