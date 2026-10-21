cell[T]
{
  value: T;

  create(value: T): cell[T]
  {
    new {value}
  }
}

box
{
  value: u64;

  create(value: u64): box
  {
    new {value}
  }
}

main(): none
{
  var result = 0;
  let original = box(1);
  let context = native_state(cell(original));

  // A handler can replace an object nested inside the native state.
  context.with[none] state ->
  {
    state.value = box(2);
    none
  };

  let current = context.with[box] state -> state.value;
  if current.value != 2
  {
    result = result + 1
  }

  // Reading a nested object copies it out, so it is not an interior alias.
  current.value = 3;
  let unchanged = context.with[u64] state -> state.value.value;
  if unchanged != 2
  {
    result = result + 2
  }

  ffi::exit_code result
}
