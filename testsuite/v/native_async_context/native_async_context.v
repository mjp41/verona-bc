use
{
  test_start_native_context =
    "test_start_native_context"(array[u8], usize, ffi::ptr, ffi::ptr): none;
}

context_state
{
  buffer: array[u8];
  first: ffi::callback[(u64) -> none];
  second: ffi::callback[(u64) -> none];

  create(buffer: array[u8]): context_state
  {
    new
    {
      buffer,
      first = ffi::callback[(u64) -> none](),
      second = ffi::callback[(u64) -> none]()
    }
  }
}

main(): none
{
  let initial = context_state(::(u8 0));
  let context = native_state(initial);

  // The native thread writes 40 before calling this callback. Entering the
  // shared native state here updates the same buffer to 42.
  let first = ffi::callback[(u64) -> none] (
    (_amount: u64): none ->
    {
      context.with[none] state ->
      {
        let old = (state.buffer)(usize 0);
        (state.buffer)(usize 0) = old + u8 2
      };
    });

  // The second callback observes the first callback's update, then releases
  // both callbacks and allows the scheduler to finish.
  let second = ffi::callback[(u64) -> none] (
    (_ignored: u64): none ->
    {
      context.with[none] state ->
      {
        if (state.buffer)(usize 0) != u8 42
        {
          ffi::exit_code 1;
        }

        state.first = ffi::callback[(u64) -> none]();
        state.second = ffi::callback[(u64) -> none]();
        ffi::external.remove
      };
    });

  // Storing the callbacks in native state keeps them alive while native code
  // holds their raw function pointers. The external event source keeps the
  // scheduler alive until the second callback completes.
  context.with[none] state ->
  {
    state.first = first;
    state.second = second;

    ffi::external.add;
    :::test_start_native_context(
      state.buffer, state.buffer.size, state.first.raw, state.second.raw)
  };
}
