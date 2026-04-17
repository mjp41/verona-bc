// Angelic receiver from match/else join (involves lambda).
// match desugars to a lambda whose return type is TypeVar during solve.
// The else branch produces an untyped literal (angelic).
// At the join, 'a' depends on cross-function return type visibility.
main() : i32
{
  var a = (match 42 { (x: i32) -> x; }) else (0);
  var b = a + 1;
  b
}
