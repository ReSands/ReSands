// Generic local-model task API: the model's C++ implements run(), which must return exactly the
// text the task's Python reference `canon(data)` returns for the same input file.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

std::string run(const uint8_t *data, size_t len);
