#pragma once
#include "face_material_bake.h"
#include <memory>

namespace surface_stamp {
constexpr std::uint32_t filter_version = 1;
// Float preparation/interchange data, not the final compressed disk artifact.
// Mips store coverage-premultiplied channels and squared roughness. Decode
// only after filtering; otherwise uncovered pixels cause dark color fringes.
// The two heights retain their different axes. A receiver must explicitly
// resolve normal-oriented detail into its relief model, never add it blindly.
struct Channels {
    float albedo_coverage[4]{};
    float orm_height[4]{};       // AO, roughness squared, metallic, projection depth
    float normal_detail[4]{};    // material normal UVN, normal-oriented microheight
    float geometric_reserved[4]{}; // geometric normal UVN, zero
};
struct Level {
    std::uint32_t offset=0, width=0, height=0, reserved=0;
};
struct Stamp {
    gpu_meshing::FaceFrame frame;
    // [minimum U, minimum V, span U, span V], all metres, same for every mip.
    float domain[4]{};
    float height_min_m=0, height_max_m=0, detail_min_m=0, detail_max_m=0;
    std::uint64_t geometry_digest=0, material_digest=0, content_digest=0;
    // 0: geometry depth + separate normal detail; 1: normal offset has been
    // resampled into projection-axis depth, with color/normal/ORM at that hit.
    std::uint32_t height_projection=0;
    float projection_error_m=0;
    std::vector<Level> levels;
    std::vector<Channels> pixels;
};
// Validates the complete geometry/material pair. An immutable result escapes
// only after every level is complete and the generation is still current.
bool prepare(const gpu_meshing::FaceMaterialJob &, const gpu_meshing::FaceMaterialPatch &,
             std::shared_ptr<const Stamp> &, gpu_meshing::Error &,
             const gpu_meshing::BuildControl & = {});
bool prepare_projected(const gpu_meshing::FaceMaterialJob &, const gpu_meshing::FaceMaterialPatch &,
                       std::shared_ptr<const Stamp> &, gpu_meshing::Error &,
                       const gpu_meshing::BuildControl & = {});
// CPU semantic oracle for finite_surface_stamp.glsl. Coordinates/footprint
// are metres. No repeat or edge clamp; samples outside filter support are zero.
// Returned channels are unpremultiplied, roughness is perceptual again, and
// normals are unit vectors. Both height axes remain separate.
Channels sample(const Stamp &, float u_m, float v_m, float footprint_m);
} // namespace surface_stamp
