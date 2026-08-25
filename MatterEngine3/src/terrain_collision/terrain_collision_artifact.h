#pragma once

#include "terrain_collision_definition.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace terrain_field {
class FieldRuntime;
}

namespace terrain_mesher {
struct SectorMesh;
}

namespace matter::terrain_collision {

struct TileCandidate {
    SectorCoordinate coordinate{};
    Float3 origin_m{};
    std::vector<Float3> vertices;
    std::vector<std::uint32_t> indices;
    std::uint64_t tile_key = 0;
    std::uint64_t digest = 0;
};

struct CandidateStats {
    std::uint32_t cache_hit_tiles = 0;
    std::uint32_t built_tiles = 0;
    std::uint32_t empty_tiles = 0;
    std::uint64_t triangle_count = 0;
    std::uint64_t unique_vertex_count = 0;
    std::uint64_t artifact_bytes = 0;
    double cold_build_ms = 0.0;
    double cache_load_ms = 0.0;
    double validation_ms = 0.0;
};

struct TerrainCollisionCandidate {
    std::uint64_t geometry_key = 0;
    std::uint64_t installation_key = 0;
    float friction = 0.7f;
    float restitution = 0.0f;
    std::vector<TileCandidate> tiles;  // sorted; includes successful empty tiles
    CandidateStats stats{};
};

using CancelCheck = std::function<bool()>;

bool load_or_build_candidate(
    const terrain_field::FieldRuntime& field,
    const CanonicalDefinition& definition,
    const std::filesystem::path& cache_root,
    const CancelCheck& cancelled,
    TerrainCollisionCandidate& out,
    std::string& error);

// Internal validation seams shared by the cache implementation and its
// behavior tests. This header lives under src/, so these are not public engine
// API and may evolve with the MTCT/MTCM format.
namespace detail {

bool convert_mesh_to_tile(const terrain_mesher::SectorMesh& mesh,
                          const CanonicalDefinition& definition,
                          const SectorCoordinate& coordinate,
                          TileCandidate& out,
                          std::string& error);

bool validate_tile_candidate(const TileCandidate& tile,
                             const CanonicalDefinition& definition,
                             const SectorCoordinate& coordinate,
                             std::string& error);

bool validate_tile_artifact_bytes(const std::vector<std::uint8_t>& bytes,
                                  const CanonicalDefinition& definition,
                                  const SectorCoordinate& coordinate,
                                  TileCandidate& out,
                                  std::uint64_t& artifact_bytes,
                                  std::string& error);

bool validate_generation_manifest(const std::filesystem::path& path,
                                  const CanonicalDefinition& definition,
                                  const TerrainCollisionCandidate& candidate,
                                  std::string& error);

}  // namespace detail

}  // namespace matter::terrain_collision
