#pragma once

#include <cstddef>

namespace matter {

// Shared serialized and renderer capacity. Flat artifacts must never publish
// more levels than either GPU culling path can address.
inline constexpr std::size_t kMaxSerializedLodLevels = 9;

} // namespace matter
