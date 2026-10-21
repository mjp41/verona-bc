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
  let source = box(1);
  let frozen = mem::freeze(source);

  // The clone is immutable.
  frozen.value = 2;
  ffi::exit_code 0
}
