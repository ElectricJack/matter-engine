#pragma once

#include "terrain_collision_definition.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
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

// Optional storage for behavior tests. Paths remain cache keys; the production
// entry point always uses the filesystem. Implementations must make move
// atomic and support concurrent callers, including competing publishers.
class ArtifactStorage {
public:
    virtual ~ArtifactStorage() = default;
    virtual bool exists(const std::filesystem::path&, std::error_code&) = 0;
    virtual std::uintmax_t file_size(const std::filesystem::path&,
                                   std::error_code&) = 0;
    virtual bool read(const std::filesystem::path&, std::size_t offset,
                      std::size_t count, std::uint8_t* destination) = 0;
    virtual bool write(const std::filesystem::path&,
                       const std::vector<std::uint8_t>&) = 0;
    virtual void remove(const std::filesystem::path&) noexcept = 0;
    // Windows-compatible result codes: 0 success, 183 destination exists.
    virtual std::uint32_t move(const std::filesystem::path& source,
                               const std::filesystem::path& destination,
                               bool replace) = 0;
};

// Narrow, src-only deterministic seam for behavior tests. Production passes no
// hooks, so these callbacks have no storage or branch-visible side effects in
// the public load_or_build_candidate path.
enum class PublicationCommitPoint {
    TileNoReplace,
    TileReplace,
    TileExistingWinner,
    ManifestNoReplace,
    ManifestReplace,
    ManifestExistingWinner,
};

// A nonzero return injects that Windows error for the requested move attempt;
// zero performs the real platform operation. POSIX publication ignores this
// deterministic test seam.
enum class PublicationMoveOperation {
    NoReplace,
    Replace,
};

enum class PublicationWinnerArtifact {
    Tile,
    Manifest,
};

struct BuildTestHooks {
    ArtifactStorage* storage = nullptr;
    std::function<bool(const terrain_field::FieldRuntime&,
                       const SectorCoordinate&,
                       std::int8_t,
                       float,
                       terrain_mesher::SectorMesh&,
                       std::string&)> mesh_tile;
    std::function<void(PublicationCommitPoint)> before_publication_commit;
    std::function<std::uint32_t(PublicationMoveOperation, std::uint32_t)>
        publication_move_error;
    std::function<std::uint32_t(PublicationWinnerArtifact, std::uint32_t)>
        publication_winner_open_error;
};

bool load_or_build_candidate_with_test_hooks(
    const terrain_field::FieldRuntime& field,
    const CanonicalDefinition& definition,
    const std::filesystem::path& cache_root,
    const CancelCheck& cancelled,
    const BuildTestHooks& hooks,
    TerrainCollisionCandidate& out,
    std::string& error);

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
                                  std::string& error,
                                  ArtifactStorage* storage = nullptr);

}  // namespace detail

}  // namespace matter::terrain_collision
