#pragma once

#include "math_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace matter {

struct TerrainCollisionRegion {
    std::string id;
    Float3 min_m{};  // inclusive
    Float3 max_m{};  // exclusive
};

struct TerrainCollisionDefinition {
    float cell_size_m = 0.0f;
    std::int8_t rung = 0;
    float friction = 0.7f;
    float restitution = 0.0f;
    std::vector<TerrainCollisionRegion> regions;
};

enum class TerrainCollisionState : std::uint8_t {
    Disabled,
    Building,
    CandidateReady,
    Installed,
    Failed,
};

struct TerrainCollisionStatus {
    TerrainCollisionState state = TerrainCollisionState::Disabled;
    std::uint64_t generation_key = 0;
    std::uint64_t geometry_key = 0;
    float cell_size_m = 0.0f;
    std::int8_t rung = 0;
    std::uint32_t region_count = 0;
    std::uint32_t sector_count = 0;
    std::uint32_t non_empty_tile_count = 0;
    std::uint32_t empty_tile_count = 0;
    std::uint64_t triangle_count = 0;
    std::uint64_t unique_vertex_count = 0;
    std::uint64_t artifact_bytes = 0;
    std::uint64_t box3d_retained_bytes = 0;
    double cold_build_ms = 0.0;
    double cache_load_ms = 0.0;
    double validation_ms = 0.0;
    double install_ms = 0.0;
    std::string failure_code;
    std::string failure_message;
};

}  // namespace matter
