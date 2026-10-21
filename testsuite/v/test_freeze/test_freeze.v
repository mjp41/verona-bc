leaf
{
  val: i32;

  create(val: i32): leaf
  {
    new {val}
  }
}

node
{
  left: leaf;
  right: leaf;
  val: i32;

  create(left: leaf, right: leaf, val: i32): node
  {
    new {left, right, val}
  }
}

main(): none
{
  var result = 0;

  // Freeze returns an independent immutable clone and preserves the source.
  let a = leaf(10);
  let frozen_a = mem::freeze(a);
  a.val = 11;
  if a.val != 11 { result = result + 1; }
  if frozen_a.val != 10 { result = result + 2; }

  // Freezing an immutable value produces the same observable value.
  let b = leaf(55);
  let frozen_b = mem::freeze(b);
  let refrozen_b = mem::freeze(frozen_b);
  if (frozen_b.val != 55) | (refrozen_b.val != 55)
  {
    result = result + 4
  }

  // The clone includes the complete reachable graph.
  let l = leaf(3);
  let r = leaf(7);
  let n = node(l, r, 100);
  let frozen_n = mem::freeze(n);
  n.val = 101;
  l.val = 4;
  r.val = 8;

  if (n.val != 101) | (n.left.val != 4) | (n.right.val != 8)
  {
    result = result + 8
  }

  if frozen_n.val != 100 { result = result + 16; }
  if frozen_n.left.val != 3 { result = result + 32; }
  if frozen_n.right.val != 7 { result = result + 64; }

  ffi::exit_code result
}
