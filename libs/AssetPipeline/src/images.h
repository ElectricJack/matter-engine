#pragma once
#include "internal.h"
namespace matter::asset::detail {
Bytes png(const Bytes &, uint32_t width, uint32_t height);
Bytes image_rgba(const Bytes &, uint32_t &width, uint32_t &height,
                 uint64_t max_bytes);
} // namespace matter::asset::detail
