#pragma once
#include <cstddef>
#include <cstdint>

namespace geometry_properties {
// One bounded in-memory case. The seed and group reproduce a failure exactly.
void run(unsigned group, uint32_t seed);
void fuzz(const uint8_t *bytes, size_t size);
}
