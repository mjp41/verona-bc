#include "../platform.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

VBCI_FFI void test_start_native_context(
  uint8_t* data, size_t size, void (*first)(uint64_t), void (*second)(uint64_t))
{
  std::thread([data, size, first, second]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (size > 0)
      data[0] = 40;
    first(2);
    second(0);
  }).detach();
}
