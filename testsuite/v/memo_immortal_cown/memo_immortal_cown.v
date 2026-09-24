outgoing
{
  value: bool;

  create(): outgoing
  {
    new {value = true}
  }

  final(self: outgoing): none
  {
    ffi::exit_code 99
  }
}

retained
{
  value: bool;

  create(): retained
  {
    new {value = true}
  }

  final(self: retained): none
  {
    ffi::exit_code 66
  }
}

use payload = outgoing | retained;

state
{
  value: payload;

  create(): state
  {
    new {value = outgoing}
  }

  replace(self: state): none
  {
    self.value = retained
  }
}

owner
{
  once create(): cown[state]
  {
    cown(state)
  }
}

main(): none
{
  ffi::exit_code 7;

  when owner value ->
  {
    (*value).replace
  }
}
