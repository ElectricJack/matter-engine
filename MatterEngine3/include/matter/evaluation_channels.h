#pragma once

// Raw, synchronized channels from one internal raster frame. Each plane is
// tightly packed, row-major, top-left origin. The displayed PNG may have a
// different size when the internal render uses DLSS or a docked viewport.

#include <array>
#include <cstdint>
#include <vector>

namespace matter {

struct EvaluationChannels {
    enum Plane : unsigned {
        IdentityRg32u = 0, // material index + frame-local instance token
        DepthF32 = 1,      // renderer reversed-Z device depth
        NormalRgba16f = 2, // linear G-buffer normal
        ColorRgba16f = 3,  // linear HDR composite before display transform
        AlbedoRgba8 = 4,   // linear material base color
        OrmRgba16f = 5,    // occlusion, roughness, metallic, displacement
        Count = 6
    };
    uint32_t width = 0;
    uint32_t height = 0;
    std::array<std::vector<uint8_t>, Count> planes;

    static constexpr std::array<uint32_t, Count> bytes_per_pixel =
        {8, 4, 8, 8, 4, 8};
};

struct VisiblePartResource {
    uint64_t part_hash = 0;
    uint32_t visible_instances = 0;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    uint32_t blas_rungs_ready = 0;
    uint32_t blas_rungs_total = 0;
    uint32_t vt_rungs_active = 0;
    uint32_t vt_rungs_total = 0;
};

struct EvaluationObjectIdentity {
    uint32_t frame_token = 0;
    uint64_t part_hash = 0;
    uint64_t entity_id = 0;
    uint32_t entity_generation = 0;
    bool dynamic_entity = false;
    bool resolved = false;
};

struct VisibleDetailReport {
    bool observed = false;
    uint32_t visible_instances = 0;
    uint32_t unmatched_tokens = 0;
    uint32_t missing_draws = 0;
    uint32_t coarse_draws = 0;
    uint32_t missing_blas = 0;
    uint32_t missing_vt = 0;
    uint32_t max_selected_lod = 0;
    uint32_t desired_max_lod = 0;
    std::vector<VisiblePartResource> parts;
    std::vector<EvaluationObjectIdentity> identities;
};

} // namespace matter
