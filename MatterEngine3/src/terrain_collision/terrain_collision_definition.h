#pragma once

#include "matter/terrain_collision.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace matter::terrain_collision {

struct SectorCoordinate {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
};

inline bool operator==(const SectorCoordinate& a, const SectorCoordinate& b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

inline bool sector_coordinate_less(const SectorCoordinate& a,
                                   const SectorCoordinate& b) noexcept {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    return a.z < b.z;
}

struct SourceIdentity {
    std::uint64_t field_hash = 0;
    std::uint64_t overlay_hash = 0;
    std::uint32_t mesher_semantic_version = 0;
    std::uint32_t geometry_format_version = 1;
};

struct CanonicalDefinition {
    SourceIdentity source{};
    float sector_size_m = 0.0f;
    float cell_size_m = 0.0f;
    std::int8_t rung = 0;
    float friction = 0.7f;
    float restitution = 0.0f;
    std::vector<TerrainCollisionRegion> regions;
    std::vector<SectorCoordinate> sectors;
    std::uint64_t geometry_key = 0;
    std::uint64_t installation_key = 0;
};

// Bounds that expand past this many base sectors are rejected before allocation.
inline constexpr std::size_t kMaxSectorCount = 1000000;

bool cell_size_to_rung(float cell_size_m, std::int8_t& out_rung) noexcept;
bool canonicalize(const TerrainCollisionDefinition& definition,
                  float sector_size_m,
                  const SourceIdentity& source,
                  CanonicalDefinition& out,
                  std::string& error);

}  // namespace matter::terrain_collision
