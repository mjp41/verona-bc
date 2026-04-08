// Ring 1: Single function with basic typed operations.
// No function calls to other functions, no lambdas, no generics.
// Tests: forward type propagation through arithmetic, let/var
// bindings, and return value. Integer literals are constrained
// by explicit type annotations on bindings.

main(): i32
{
  var result: i32 = 0;
  let a: i32 = 10;
  let b: i32 = 20;
  let c = a + b;
  if c != 30 { result = result + 1 }
  let d = c - a;
  if d != 20 { result = result + 2 }
  let e = a * b;
  if e != 200 { result = result + 4 }
  result
}
