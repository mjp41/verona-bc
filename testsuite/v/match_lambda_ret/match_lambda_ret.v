// Minimal: match lambda return type not visible during solve.
// 'a' gets the match result. 'a == 1' requires method lookup on 'a'.
// If 'a' is angelic (lambda return TypeVar), lookup fails.
main() : i32
{
  var a = (match 42 { (x: i32) -> x; }) else (0);
  if a == 1 { 0 } else { 1 }
}
