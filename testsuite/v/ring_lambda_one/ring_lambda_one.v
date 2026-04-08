// Ring 3: Single function containing a single lambda.
// Lambda has explicit parameter and return types.
// Tests: type flow through lambda lifting — lambda creation,
// field capture, and CallDyn on the lambda object.

main(): i32
{
  var result: i32 = 0;
  let f = (x: i32): i32 -> x + 1;
  let a = f(41);
  if a != 42 { result = result + 1 }
  let b = f(0);
  if b != 1 { result = result + 2 }
  result
}
