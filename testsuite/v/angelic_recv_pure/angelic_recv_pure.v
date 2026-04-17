// Purely angelic receiver, no lambdas.
// Both if-branches produce untyped literals -> angelic.
// At the join, 'a' is purely angelic (no concrete part).
// The CallDyn for 'a + 1' should constrain args from angelic member methods.
main() : i32
{
  var x = i32 10;
  var a = if x == 0 { 1 } else { 0 };
  var b = a + 1;
  b
}
