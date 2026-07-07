// Regression test for vc infer non-termination.
//
// This program is intentionally not accompanied by golden files: compiling it
// currently reaches ANF and then hangs in the infer pass. Use:
//
//   timeout 30s dist/vc/vc build -p infer ../testsuite/v/infer_y_combinator_hang
//
// Expected current result: timeout exit code 124.

use Fn[T, U] = T -> U;

Y[T, U](f: Fn[Fn[T, U], Fn[T, U]], x: T): U
{
  f((y: T): U -> { infer_y_combinator_hang::Y[T, U](f, y) }, x)
}

main(): none
{
  ffi::exit_code(0)
}
