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
  let source = box(1);
  let context = native_state(source);
  source.value = 2;

  // Creating native state copies its initial value.
  let copied = context.with[u64] state -> state.value;
  if copied != 1
  {
    result = result + 1
  }

  // Copies of a native-state handle share the same native state.
  let shared_context = context;
  shared_context.with[none] state ->
  {
    state.value = 3
  };

  let original = context.with[u64] state -> state.value;
  if original != 3
  {
    result = result + 2
  }

  // Values returned from with are copied out of the native state.
  let returned = context.with[box] state -> state;
  returned.value = 4;

  let still_three = context.with[u64] state -> state.value;
  if still_three != 3
  {
    result = result + 4
  }

  // Captures are copied into with, so mutations do not escape the handler.
  let captured = box(5);
  context.with[none] state ->
  {
    state.value = captured.value;
    captured.value = 6
  };

  if captured.value != 5
  {
    result = result + 8
  }

  let stored = context.with[u64] state -> state.value;
  if stored != 5
  {
    result = result + 16
  }

  ffi::exit_code result
}
