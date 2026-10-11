#pragma once
#include <matter/graphics/raster.h>
namespace matter::graphics::detail {
// Other producers share the raster material/camera products but place their
// identity tokens in their own draw records. Validate those references there.
data::ValidationResult validate_raster_products(const RasterInputs &, RasterLimits, bool require_placed_ids,
                                                uint64_t *input_bytes = nullptr);
} // namespace matter::graphics::detail
