#include "check.h"
#include "../src/hydrology/river_geometry.h"
#include "../src/bake_mode.h"
#include "../src/terrain_collision/terrain_collision_artifact.h"
#include "../src/terrain_mesher.h"
#include "../src/terrain_river_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using matter::Float3;
using matter::TerrainCollisionDefinition;
using matter::TerrainCollisionRegion;
using matter::terrain_collision::CanonicalDefinition;
using matter::terrain_collision::SectorCoordinate;
using matter::terrain_collision::SourceIdentity;
using matter::terrain_collision::TerrainCollisionCandidate;
using matter::terrain_collision::TileCandidate;

constexpr float kSectorSize = 8.0f;
constexpr float kCellSize = 2.0f;
constexpr std::int8_t kRung = 0;

class BakeModeGuard {
public:
    BakeModeGuard() : previous_(bake_mode::forced_contour_seams()) {}
    ~BakeModeGuard() { bake_mode::forced_contour_seams() = previous_; }
    void contour_seams(bool enabled) {
        bake_mode::forced_contour_seams() = enabled ? 1 : 0;
    }
private:
    int previous_ = -1;
};

struct TestField {
    std::uint64_t base_hash = 0;
    terrain_field::FieldRuntime runtime;
};

TestField make_field(
    const char* text,
    std::shared_ptr<const terrain_field::HeightOverlay> overlay = {}) {
    terrain_field::FieldProgram program;
    std::string error;
    const bool parsed = terrain_field::FieldProgram::parse(text, program, error);
    CHECK(parsed, error.c_str());
    const std::uint64_t base_hash = program.hash();
    return {base_hash,
            terrain_field::FieldRuntime(std::move(program), std::move(overlay))};
}

SourceIdentity source_for(const TestField& field) {
    SourceIdentity source{};
    source.field_hash = field.base_hash;
    source.overlay_hash = field.runtime.height_overlay()
        ? field.runtime.height_overlay()->hash() : 0u;
    source.bake_mode_salt = bake_mode::salt();
    source.mesher_semantic_version = terrain_mesher::kSemanticVersion;
    source.geometry_format_version = 1u;
    return source;
}

CanonicalDefinition definition_for(
    const TestField& field, Float3 min_m = {0.0f, 0.0f, 0.0f},
    Float3 max_m = {kSectorSize, kSectorSize, kSectorSize},
    float friction = 0.7f, float restitution = 0.0f) {
    TerrainCollisionDefinition input{};
    input.cell_size_m = kCellSize;
    input.rung = kRung;
    input.friction = friction;
    input.restitution = restitution;
    input.regions.push_back({"fixture", min_m, max_m});
    CanonicalDefinition result{};
    std::string error;
    CHECK(matter::terrain_collision::canonicalize(
              input, kSectorSize, source_for(field), result, error),
          error.c_str());
    return result;
}

bool mesh_tile_with_real_mesher(
    const terrain_field::FieldRuntime& field,
    const SectorCoordinate& coordinate,
    std::int8_t rung,
    float sector_size_m,
    terrain_mesher::SectorMesh& mesh,
    std::string& error) {
    return terrain_mesher::mesh_sector_tiled(
        field, coordinate.x, coordinate.y, coordinate.z, rung, sector_size_m,
        mesh, nullptr, error);
}

matter::terrain_collision::detail::BuildTestHooks counting_mesher_hooks(
    std::atomic<unsigned>& calls) {
    matter::terrain_collision::detail::BuildTestHooks hooks{};
    hooks.mesh_tile = [&calls](
        const terrain_field::FieldRuntime& field,
        const SectorCoordinate& coordinate,
        std::int8_t rung,
        float sector_size_m,
        terrain_mesher::SectorMesh& mesh,
        std::string& error) {
        calls.fetch_add(1u, std::memory_order_relaxed);
        return mesh_tile_with_real_mesher(
            field, coordinate, rung, sector_size_m, mesh, error);
    };
    return hooks;
}

bool load_with_hooks(
    const TestField& field,
    const CanonicalDefinition& definition,
    const std::filesystem::path& cache_root,
    const matter::terrain_collision::CancelCheck& cancelled,
    const matter::terrain_collision::detail::BuildTestHooks& hooks,
    TerrainCollisionCandidate& candidate,
    std::string& error) {
    return matter::terrain_collision::detail::load_or_build_candidate_with_test_hooks(
        field.runtime, definition, cache_root, cancelled, hooks, candidate, error);
}

class TempRoot {
public:
    explicit TempRoot(const char* label) {
        static std::atomic<std::uint64_t> serial{0u};
        path = std::filesystem::current_path() /
            (std::string(".terrain-collision-artifact-") + label + "-" +
             std::to_string(serial.fetch_add(1u, std::memory_order_relaxed)));
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    ~TempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

std::string hex_key(std::uint64_t key) {
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(16) << key;
    return stream.str();
}

std::filesystem::path tile_path(const std::filesystem::path& root,
                                std::uint64_t tile_key) {
    return root / "terrain_collision" / "v1" / "tiles" /
           (hex_key(tile_key) + ".mtct");
}

std::filesystem::path manifest_path(const std::filesystem::path& root,
                                    std::uint64_t installation_key) {
    return root / "terrain_collision" / "v1" / "generations" /
           (hex_key(installation_key) + ".mtcm");
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    CHECK(static_cast<bool>(stream), "test fixture file opens for reading");
    stream.seekg(0, std::ios::end);
    const std::streamoff length = stream.tellg();
    CHECK(length >= 0, "test fixture file has a nonnegative length");
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(length));
    if (!result.empty())
        stream.read(reinterpret_cast<char*>(result.data()), length);
    CHECK(static_cast<bool>(stream), "test fixture file reads completely");
    return result;
}

void write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    CHECK(static_cast<bool>(stream), "test fixture file opens for writing");
    if (!bytes.empty())
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    CHECK(static_cast<bool>(stream), "test fixture file writes completely");
}

bool float3_bytes_equal(const std::vector<Float3>& a,
                        const std::vector<Float3>& b) {
    return a.size() == b.size() &&
           (a.empty() ||
            std::memcmp(a.data(), b.data(), a.size() * sizeof(Float3)) == 0);
}

bool same_tiles(const TerrainCollisionCandidate& a,
                const TerrainCollisionCandidate& b) {
    if (a.geometry_key != b.geometry_key ||
        a.installation_key != b.installation_key ||
        a.friction != b.friction || a.restitution != b.restitution ||
        a.tiles.size() != b.tiles.size())
        return false;
    for (std::size_t i = 0; i != a.tiles.size(); ++i) {
        const TileCandidate& left = a.tiles[i];
        const TileCandidate& right = b.tiles[i];
        if (!(left.coordinate == right.coordinate) ||
            std::memcmp(&left.origin_m, &right.origin_m, sizeof(Float3)) != 0 ||
            left.tile_key != right.tile_key || left.digest != right.digest ||
            !float3_bytes_equal(left.vertices, right.vertices) ||
            left.indices != right.indices)
            return false;
    }
    return true;
}

bool valid_candidate_geometry(const TerrainCollisionCandidate& candidate,
                              const CanonicalDefinition& definition) {
    if (candidate.tiles.size() != definition.sectors.size()) return false;
    for (std::size_t tile_index = 0; tile_index != candidate.tiles.size();
         ++tile_index) {
        const TileCandidate& tile = candidate.tiles[tile_index];
        if (!(tile.coordinate == definition.sectors[tile_index])) return false;
        if (tile_index != 0u && !matter::terrain_collision::sector_coordinate_less(
                                    candidate.tiles[tile_index - 1u].coordinate,
                                    tile.coordinate))
            return false;
        if (tile.indices.size() % 3u != 0u) return false;
        for (const Float3& vertex : tile.vertices)
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
                !std::isfinite(vertex.z))
                return false;
        for (std::uint32_t index : tile.indices)
            if (index >= tile.vertices.size()) return false;
        for (std::size_t index = 0; index != tile.indices.size(); index += 3u) {
            const Float3& a = tile.vertices[tile.indices[index + 0u]];
            const Float3& b = tile.vertices[tile.indices[index + 1u]];
            const Float3& c = tile.vertices[tile.indices[index + 2u]];
            const double ux = static_cast<double>(b.x) - a.x;
            const double uy = static_cast<double>(b.y) - a.y;
            const double uz = static_cast<double>(b.z) - a.z;
            const double vx = static_cast<double>(c.x) - a.x;
            const double vy = static_cast<double>(c.y) - a.y;
            const double vz = static_cast<double>(c.z) - a.z;
            const double cx = uy * vz - uz * vy;
            const double cy = uz * vx - ux * vz;
            const double cz = ux * vy - uy * vx;
            if (!(cx * cx + cy * cy + cz * cz > 0.0)) return false;
        }
    }
    return true;
}

constexpr const char* kPlaneField =
    "const 4\nconst 0.5\n"
    "height r0\nmoisture r1\nrelief r1\nseaLevel -100\nbiome 0.65 0.35\n";

constexpr const char* kSteepSlopeField =
    "input wx\nconst 1.5\nmul r0 r1\nconst 0.5\n"
    "height r2\nmoisture r3\nrelief r3\nseaLevel -100\nbiome 0.65 0.35\n";

constexpr const char* kRoundedRavineField =
    "input wx\nconst 4.17\nsub r0 r1\nmul r2 r2\nconst 0.25\n"
    "add r3 r4\npow r5 0.5\nconst 2\nmul r6 r7\nconst 0.5\n"
    "height r8\nmoisture r9\nrelief r9\nseaLevel -100\nbiome 0.65 0.35\n";

constexpr const char* kCaveField =
    "const 6.4\ninput wy\nconst 0.5\nsub r0 r1\nconst 4.3\nsub r1 r4\n"
    "const 2.2\nsub r6 r1\nmax r5 r7\nmin r3 r8\n"
    "height r0\ndensity r9\nmoisture r2\nrelief r2\n"
    "seaLevel -100\nbiome 0.65 0.35\n";

constexpr const char* kCliffField =
    "input wx\nsmoothstep 3 5 r0\nconst 6\nmul r1 r2\nconst 0.5\n"
    "height r3\nmoisture r4\nrelief r4\nseaLevel -100\nbiome 0.65 0.35\n";

constexpr const char* kOverhangField =
    "input wx\nconst 4\nsub r0 r1\nmul r2 r2\n"
    "input wy\nconst 4\nsub r4 r5\nmul r6 r6\n"
    "input wz\nconst 4\nsub r8 r9\nmul r10 r10\n"
    "add r3 r7\nadd r12 r11\nconst 9\nsub r14 r13\n"
    "const 7\nconst 0.5\n"
    "height r16\ndensity r15\nmoisture r17\nrelief r17\n"
    "seaLevel -100\nbiome 0.65 0.35\n";

constexpr const char* kVerticalPlaneField =
    "const 4\ninput wx\nsub r0 r1\nconst 0.5\n"
    "height r0\ndensity r2\nmoisture r3\nrelief r3\n"
    "seaLevel -100\nbiome 0.65 0.35\n";

void test_fixture_conversion_is_valid_and_repeatable() {
    const std::pair<const char*, const char*> fixtures[] = {
        {"plane", kPlaneField}, {"steep-slope", kSteepSlopeField},
        {"rounded-v-ravine", kRoundedRavineField}, {"cave", kCaveField},
        {"cliff", kCliffField}, {"overhang", kOverhangField},
    };
    for (const auto& fixture : fixtures) {
        TestField field = make_field(fixture.second);
        const CanonicalDefinition definition = definition_for(field);
        TempRoot first_root(fixture.first);
        TempRoot second_root(fixture.first);
        TerrainCollisionCandidate first{};
        TerrainCollisionCandidate second{};
        std::string error;
        const bool first_ok = matter::terrain_collision::load_or_build_candidate(
            field.runtime, definition, first_root.path, {}, first, error);
        CHECK(first_ok, error.c_str());
        const bool second_ok = matter::terrain_collision::load_or_build_candidate(
            field.runtime, definition, second_root.path, {}, second, error);
        CHECK(second_ok, error.c_str());
        if (!first_ok || !second_ok) continue;
        CHECK(first.stats.built_tiles == definition.sectors.size(),
              "a cold root builds every fixture tile");
        CHECK(first.stats.cache_hit_tiles == 0u,
              "a cold root cannot report cache hits");
        CHECK(valid_candidate_geometry(first, definition),
              "fixture conversion emits sorted, finite, nondegenerate indexed triangles");
        CHECK(first.stats.triangle_count > 0u,
              "each named terrain fixture exercises non-empty collision geometry");
        CHECK(same_tiles(first, second),
              "two cold roots produce byte-repeatable tile vertices, indices, and digests");
    }
}

void test_all_material_buckets_are_flattened() {
    TestField field = make_field(
        "input wx\nconst 0.5\nmul r0 r1\nconst 0.3\nadd r2 r3\n"
        "const 0.5\nheight r4\nmoisture r5\nrelief r5\n"
        "seaLevel 2\nbiome 0.65 0.35\n");
    const CanonicalDefinition definition = definition_for(field);
    terrain_mesher::SectorMesh direct{};
    std::string error;
    CHECK(terrain_mesher::mesh_sector_tiled(
              field.runtime, 0, 0, 0, kRung, kSectorSize, direct, nullptr, error),
          error.c_str());
    CHECK(direct.buckets.size() > 1u,
          "the material fixture actually produces multiple mesher buckets");
    std::uint64_t direct_triangles = 0u;
    for (const terrain_mesher::MaterialBucket& bucket : direct.buckets)
        direct_triangles += bucket.positions.size() / 9u;
    TempRoot root("materials");
    TerrainCollisionCandidate candidate{};
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, definition, root.path, {}, candidate, error),
          error.c_str());
    CHECK(candidate.stats.triangle_count == direct_triangles,
          "collision conversion includes triangles from every material bucket");
}

void test_uniform_density_tiles_succeed_empty() {
    const char* fields[] = {
        "const -1\nconst 4\nconst 0.5\nheight r1\ndensity r0\n"
        "moisture r2\nrelief r2\nseaLevel -100\nbiome 0.65 0.35\n",
        "const 1\nconst 4\nconst 0.5\nheight r1\ndensity r0\n"
        "moisture r2\nrelief r2\nseaLevel -100\nbiome 0.65 0.35\n",
    };
    for (const char* text : fields) {
        TestField field = make_field(text);
        const CanonicalDefinition definition = definition_for(field);
        TempRoot root("empty");
        TerrainCollisionCandidate candidate{};
        std::string error;
        CHECK(matter::terrain_collision::load_or_build_candidate(
                  field.runtime, definition, root.path, {}, candidate, error),
              error.c_str());
        CHECK(candidate.tiles.size() == 1u && candidate.tiles[0].vertices.empty() &&
                  candidate.tiles[0].indices.empty(),
              "all-air and all-solid sectors remain successful empty tiles");
        CHECK(candidate.stats.empty_tiles == 1u,
              "successful empty tiles are counted in candidate statistics");
        CHECK(std::filesystem::exists(tile_path(root.path, candidate.tiles[0].tile_key)),
              "successful empty tiles still publish deterministic artifacts");
    }
}

std::shared_ptr<const terrain_field::RiverHeightOverlay> make_river_overlay() {
    matter::RiverNetworkDefinition network{};
    network.cell_size_m = 1.0f;
    network.seed = 77u;
    matter::RiverDefinition river{};
    river.name = "main";
    river.inlet = {{0.3f, 4.2f, 3.7f}, 1.0f};
    river.curve = {{0.3f, 4.2f, 3.7f}, {4.1f, 4.2f, 3.7f},
                   {7.7f, 4.2f, 3.7f}};
    river.channel_profile = {{0.0f, 3.4f, 2.1f, 0.0f},
                             {7.4f, 3.4f, 2.1f, 0.0f}};
    network.rivers.push_back(river);
    hydrology::RiverGeometry geometry{};
    std::string error;
    CHECK(hydrology::build_river_geometry(network, "main", geometry, error),
          error.c_str());
    std::shared_ptr<const terrain_field::RiverHeightOverlay> overlay;
    CHECK(terrain_field::RiverHeightOverlay::build(geometry, overlay, error),
          error.c_str());
    return overlay;
}

void test_final_overlaid_runtime_is_meshed() {
    TestField base = make_field(
        "const 6.3\nconst 0.5\nheight r0\nmoisture r1\nrelief r1\n"
        "seaLevel -100\nbiome 0.65 0.35\n");
    TestField overlaid = make_field(
        "const 6.3\nconst 0.5\nheight r0\nmoisture r1\nrelief r1\n"
        "seaLevel -100\nbiome 0.65 0.35\n", make_river_overlay());
    const CanonicalDefinition base_definition = definition_for(base);
    const CanonicalDefinition overlaid_definition = definition_for(overlaid);
    TempRoot base_root("river-base");
    TempRoot overlay_root("river-overlay");
    TerrainCollisionCandidate base_candidate{};
    TerrainCollisionCandidate overlay_candidate{};
    std::string error;
    const bool base_ok = matter::terrain_collision::load_or_build_candidate(
        base.runtime, base_definition, base_root.path, {}, base_candidate, error);
    CHECK(base_ok, error.c_str());
    const bool overlay_ok = matter::terrain_collision::load_or_build_candidate(
        overlaid.runtime, overlaid_definition, overlay_root.path, {},
        overlay_candidate, error);
    CHECK(overlay_ok, error.c_str());
    if (!base_ok || !overlay_ok) return;
    CHECK(base_candidate.tiles[0].digest != overlay_candidate.tiles[0].digest,
          "a RiverHeightOverlay changes the collision tile digest");
    float minimum_world_y = std::numeric_limits<float>::infinity();
    std::size_t matching_probes = 0u;
    for (const Float3& local : overlay_candidate.tiles[0].vertices) {
        const float world_x = overlay_candidate.tiles[0].origin_m.x + local.x;
        const float world_y = overlay_candidate.tiles[0].origin_m.y + local.y;
        const float world_z = overlay_candidate.tiles[0].origin_m.z + local.z;
        minimum_world_y = std::min(minimum_world_y, world_y);
        if (std::fabs(world_y - overlaid.runtime.height_at(world_x, world_z)) <=
            kCellSize)
            ++matching_probes;
    }
    CHECK(minimum_world_y < 5.0f,
          "the collision surface follows the carved runtime below the base plane");
    CHECK(!overlay_candidate.tiles[0].vertices.empty() &&
              matching_probes * 4u >= overlay_candidate.tiles[0].vertices.size() * 3u,
          "collision vertices match direct probes of the final overlaid runtime");
}

using VertexBits = std::array<std::uint32_t, 3>;
using EdgeBits = std::pair<VertexBits, VertexBits>;

struct ExpectedInterfaceContour {
    std::size_t interface_axis = 0u;
    float interface_coordinate = 0.0f;
    std::size_t varying_axis = 0u;
    float varying_min = 0.0f;
    float varying_max = 0.0f;
    std::size_t fixed_axis = 0u;
    float fixed_coordinate = 0.0f;
};

std::uint32_t float_bits(float value) {
    std::uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

VertexBits world_vertex_bits(const TileCandidate& tile, std::uint32_t index) {
    const Float3& local = tile.vertices[index];
    return {float_bits(tile.origin_m.x + local.x),
            float_bits(tile.origin_m.y + local.y),
            float_bits(tile.origin_m.z + local.z)};
}

std::set<EdgeBits> interface_edges(const TileCandidate& tile,
                                   std::size_t axis,
                                   float plane) {
    std::set<EdgeBits> result;
    const std::uint32_t plane_bits = float_bits(plane);
    for (std::size_t offset = 0u; offset != tile.indices.size(); offset += 3u) {
        const std::uint32_t triangle[] = {
            tile.indices[offset], tile.indices[offset + 1u],
            tile.indices[offset + 2u]};
        for (std::size_t edge = 0u; edge != 3u; ++edge) {
            VertexBits a = world_vertex_bits(tile, triangle[edge]);
            VertexBits b = world_vertex_bits(tile, triangle[(edge + 1u) % 3u]);
            if (a[axis] != plane_bits || b[axis] != plane_bits || a == b)
                continue;
            if (b < a) std::swap(a, b);
            result.emplace(a, b);
        }
    }
    return result;
}

bool interface_edges_cover_expected_contour(
    const std::set<EdgeBits>& first,
    const std::set<EdgeBits>& second,
    const ExpectedInterfaceContour& expected) {
    std::set<EdgeBits> edge_union = first;
    edge_union.insert(second.begin(), second.end());
    if (edge_union.empty() || expected.interface_axis >= 3u ||
        expected.varying_axis >= 3u ||
        expected.fixed_axis >= 3u ||
        expected.interface_axis == expected.varying_axis ||
        expected.interface_axis == expected.fixed_axis ||
        expected.varying_axis == expected.fixed_axis)
        return false;

    const std::uint32_t interface_bits =
        float_bits(expected.interface_coordinate);
    const std::uint32_t fixed_bits = float_bits(expected.fixed_coordinate);
    const std::uint32_t minimum_bits = float_bits(expected.varying_min);
    const std::uint32_t maximum_bits = float_bits(expected.varying_max);
    struct ProjectedInterval {
        float minimum = 0.0f;
        float maximum = 0.0f;
        std::uint32_t minimum_bits = 0u;
        std::uint32_t maximum_bits = 0u;
    };
    std::vector<ProjectedInterval> intervals;
    intervals.reserve(edge_union.size());
    for (const EdgeBits& edge : edge_union) {
        if (edge.first[expected.interface_axis] != interface_bits ||
            edge.second[expected.interface_axis] != interface_bits ||
            edge.first[expected.fixed_axis] != fixed_bits ||
            edge.second[expected.fixed_axis] != fixed_bits)
            return false;
        float a = 0.0f;
        float b = 0.0f;
        const std::uint32_t a_bits = edge.first[expected.varying_axis];
        const std::uint32_t b_bits = edge.second[expected.varying_axis];
        std::memcpy(&a, &a_bits, sizeof(a));
        std::memcpy(&b, &b_bits, sizeof(b));
        if (!std::isfinite(a) || !std::isfinite(b) || a == b) return false;
        if (b < a)
            intervals.push_back({b, a, b_bits, a_bits});
        else
            intervals.push_back({a, b, a_bits, b_bits});
    }
    std::sort(intervals.begin(), intervals.end(),
              [](const ProjectedInterval& a, const ProjectedInterval& b) {
                  if (a.minimum != b.minimum) return a.minimum < b.minimum;
                  return a.maximum < b.maximum;
              });
    if (intervals.front().minimum_bits != minimum_bits ||
        intervals.back().maximum_bits != maximum_bits)
        return false;
    for (std::size_t index = 1u; index != intervals.size(); ++index)
        if (intervals[index - 1u].maximum_bits !=
            intervals[index].minimum_bits)
            return false;
    return true;
}

VertexBits vertex_bits(float x, float y, float z) {
    return {float_bits(x), float_bits(y), float_bits(z)};
}

void test_contour_coverage_validator_rejects_middle_gap() {
    std::set<EdgeBits> gapped{
        {vertex_bits(8.0f, 4.0f, 0.0f), vertex_bits(8.0f, 4.0f, 2.0f)},
        {vertex_bits(8.0f, 4.0f, 4.0f), vertex_bits(8.0f, 4.0f, 8.0f)},
    };
    CHECK(!interface_edges_cover_expected_contour(
              gapped, gapped, {0u, 8.0f, 2u, 0.0f, 8.0f, 1u, 4.0f}),
          "matching interface edge sets with a missing middle segment are rejected");
}

void assert_equal_rung_pair(const char* label, const char* field_text,
                            Float3 max_m,
                            const ExpectedInterfaceContour& expected) {
    TestField field = make_field(field_text);
    const CanonicalDefinition definition = definition_for(
        field, {0.0f, 0.0f, 0.0f}, max_m);
    TempRoot root(label);
    TerrainCollisionCandidate candidate{};
    std::string error;
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, definition, root.path, {}, candidate, error),
          error.c_str());
    CHECK(candidate.tiles.size() == 2u,
          "the neighbor fixture canonicalizes to exactly two tiles");
    if (candidate.tiles.size() != 2u) return;
    const std::set<EdgeBits> first =
        interface_edges(candidate.tiles[0], expected.interface_axis,
                        expected.interface_coordinate);
    const std::set<EdgeBits> second =
        interface_edges(candidate.tiles[1], expected.interface_axis,
                        expected.interface_coordinate);
    CHECK(!first.empty(),
          "the fixture surface has explicit triangle edges on the interface plane");
    CHECK(first == second,
          "both tiles own a bit-identical interface-plane edge set");
    CHECK(interface_edges_cover_expected_contour(first, second, expected),
          "the shared interface contour is covered exactly from face minimum to maximum");
}

void test_equal_rung_neighbors_share_vertices_on_every_axis() {
    assert_equal_rung_pair("neighbor-x", kPlaneField,
                           {2.0f * kSectorSize, kSectorSize, kSectorSize},
                           {0u, 8.0f, 2u, 0.0f, 8.0f, 1u, 4.0f});
    assert_equal_rung_pair("neighbor-y", kVerticalPlaneField,
                           {kSectorSize, 2.0f * kSectorSize, kSectorSize},
                           {1u, 8.0f, 2u, 0.0f, 8.0f, 0u, 4.0f});
    assert_equal_rung_pair("neighbor-z", kPlaneField,
                           {kSectorSize, kSectorSize, 2.0f * kSectorSize},
                           {2u, 8.0f, 0u, 0.0f, 8.0f, 1u, 4.0f});
}

terrain_mesher::SectorMesh one_triangle_mesh() {
    terrain_mesher::SectorMesh mesh{};
    terrain_mesher::MaterialBucket bucket{};
    bucket.positions = {0.0f, 0.0f, 0.0f,
                        1.0f, 0.0f, 0.0f,
                        0.0f, 1.0f, 0.0f};
    bucket.normals = {0.0f, 0.0f, 1.0f,
                      0.0f, 0.0f, 1.0f,
                      0.0f, 0.0f, 1.0f};
    mesh.buckets.push_back(std::move(bucket));
    return mesh;
}

void test_validators_reject_invalid_geometry_and_headers() {
    TestField field = make_field(kPlaneField);
    const CanonicalDefinition definition = definition_for(field);
    const SectorCoordinate coordinate{};
    terrain_mesher::SectorMesh mesh = one_triangle_mesh();
    TileCandidate valid{};
    std::string error;
    CHECK(matter::terrain_collision::detail::convert_mesh_to_tile(
              mesh, definition, coordinate, valid, error), error.c_str());

    const auto rejects_tile = [&](TileCandidate invalid, const char* message) {
        error.clear();
        CHECK(!matter::terrain_collision::detail::validate_tile_candidate(
                  invalid, definition, coordinate, error), message);
    };
    TileCandidate invalid = valid;
    invalid.vertices[0].x = std::numeric_limits<float>::quiet_NaN();
    rejects_tile(invalid, "tile validator rejects NaN vertices");
    invalid = valid;
    invalid.vertices[0].y = std::numeric_limits<float>::infinity();
    rejects_tile(invalid, "tile validator rejects infinite vertices");
    invalid = valid;
    invalid.indices[0] = static_cast<std::uint32_t>(invalid.vertices.size());
    rejects_tile(invalid, "tile validator rejects index overflow");
    invalid = valid;
    invalid.indices.push_back(0u);
    rejects_tile(invalid, "tile validator rejects non-triangle index counts");
    invalid = valid;
    invalid.indices[1] = invalid.indices[0];
    rejects_tile(invalid, "tile validator rejects repeated triangle indices");
    invalid = valid;
    invalid.vertices[2] = invalid.vertices[1];
    rejects_tile(invalid, "tile validator rejects exactly zero-area triangles");
    invalid = valid;
    invalid.coordinate.x = 1;
    rejects_tile(invalid, "tile validator rejects the wrong coordinate");
    invalid = valid;
    invalid.origin_m.x = 1.0f;
    rejects_tile(invalid, "tile validator rejects the wrong origin");
    invalid = valid;
    invalid.digest ^= 1u;
    rejects_tile(invalid, "tile validator rejects a digest mismatch");

    terrain_mesher::SectorMesh reversed = one_triangle_mesh();
    for (float& normal : reversed.buckets[0].normals) normal = -normal;
    TileCandidate ignored{};
    CHECK(!matter::terrain_collision::detail::convert_mesh_to_tile(
              reversed, definition, coordinate, ignored, error),
          "conversion rejects winding opposite emitted terrain normals");

    TempRoot root("count-overflow");
    TerrainCollisionCandidate candidate{};
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, definition, root.path, {}, candidate, error),
          error.c_str());
    std::vector<std::uint8_t> bytes =
        read_bytes(tile_path(root.path, candidate.tiles[0].tile_key));
    CHECK(bytes.size() >= 144u, "MTCT fixture contains its fixed header");
    for (std::size_t index = 96u; index != 104u; ++index) bytes[index] = 0xffu;
    TileCandidate parsed{};
    std::uint64_t artifact_bytes = 0u;
    CHECK(!matter::terrain_collision::detail::validate_tile_artifact_bytes(
              bytes, definition, coordinate, parsed, artifact_bytes, error),
          "tile artifact validator rejects overflowing count arithmetic");
}

void test_cache_hits_material_reuse_and_region_reuse() {
    TestField field = make_field(kPlaneField);
    const CanonicalDefinition base_definition = definition_for(field);
    TempRoot root("reuse");
    TerrainCollisionCandidate cold{};
    TerrainCollisionCandidate warm{};
    std::string error;
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, base_definition, root.path, {}, cold, error),
          error.c_str());
    const std::filesystem::path base_tile =
        tile_path(root.path, cold.tiles[0].tile_key);
    const std::vector<std::uint8_t> original_bytes = read_bytes(base_tile);
    const auto original_time = std::filesystem::last_write_time(base_tile);
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, base_definition, root.path, {}, warm, error),
          error.c_str());
    CHECK(same_tiles(cold, warm),
          "a warm load is byte-equivalent to the cold candidate");
    CHECK(warm.stats.cache_hit_tiles == base_definition.sectors.size() &&
              warm.stats.built_tiles == 0u,
          "the second load validates every tile from cache without remeshing");
    CHECK(matter::terrain_collision::detail::validate_generation_manifest(
              manifest_path(root.path, warm.installation_key), base_definition,
              warm, error), error.c_str());

    const CanonicalDefinition material_definition = definition_for(
        field, {0.0f, 0.0f, 0.0f},
        {kSectorSize, kSectorSize, kSectorSize}, 0.25f, 0.4f);
    TerrainCollisionCandidate material{};
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, material_definition, root.path, {}, material, error),
          error.c_str());
    CHECK(material.geometry_key == cold.geometry_key &&
              material.installation_key != cold.installation_key,
          "material-only changes preserve geometry identity and change installation identity");
    CHECK(material.tiles[0].tile_key == cold.tiles[0].tile_key &&
              material.stats.cache_hit_tiles == 1u && material.stats.built_tiles == 0u,
          "material-only changes reuse the same immutable MTCT tile");
    CHECK(read_bytes(base_tile) == original_bytes &&
              std::filesystem::last_write_time(base_tile) == original_time,
          "material-only changes do not rewrite tile bytes or timestamps");
    CHECK(std::filesystem::exists(
              manifest_path(root.path, material.installation_key)),
          "material-only changes publish a distinct installation manifest");

    const CanonicalDefinition expanded_definition = definition_for(
        field, {0.0f, 0.0f, 0.0f},
        {2.0f * kSectorSize, kSectorSize, kSectorSize});
    TerrainCollisionCandidate expanded{};
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, expanded_definition, root.path, {}, expanded, error),
          error.c_str());
    CHECK(expanded.tiles[0].tile_key == cold.tiles[0].tile_key &&
              expanded.stats.cache_hit_tiles == 1u && expanded.stats.built_tiles == 1u,
          "expanding a region reuses unchanged sector tiles and builds only new tiles");
    TerrainCollisionCandidate shrunk{};
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, base_definition, root.path, {}, shrunk, error),
          error.c_str());
    CHECK(shrunk.tiles[0].tile_key == cold.tiles[0].tile_key &&
              shrunk.stats.cache_hit_tiles == 1u && shrunk.stats.built_tiles == 0u,
          "shrinking a region reuses the original tile key");
}

void test_bake_modes_have_distinct_tile_and_generation_identities() {
    BakeModeGuard mode;
    TestField field = make_field(kPlaneField);
    TempRoot root("bake-mode");
    std::string error;

    mode.contour_seams(true);
    const CanonicalDefinition default_definition = definition_for(field);
    TerrainCollisionCandidate default_candidate{};
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, default_definition, root.path, {},
              default_candidate, error),
          error.c_str());

    mode.contour_seams(false);
    TerrainCollisionCandidate stale_candidate{};
    CHECK(!matter::terrain_collision::load_or_build_candidate(
              field.runtime, default_definition, root.path, {},
              stale_candidate, error),
          "a canonical definition from another bake mode is rejected");
    CHECK(error.find("bake mode") != std::string::npos,
          "stale bake-mode validation reports the identity mismatch");

    const CanonicalDefinition rollback_definition = definition_for(field);
    TerrainCollisionCandidate rollback_candidate{};
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, rollback_definition, root.path, {},
              rollback_candidate, error),
          error.c_str());
    CHECK(default_definition.geometry_key != rollback_definition.geometry_key &&
              default_definition.installation_key !=
                  rollback_definition.installation_key,
          "default and rollback modes have distinct geometry and installation identities");
    CHECK(default_candidate.tiles[0].tile_key !=
              rollback_candidate.tiles[0].tile_key,
          "default and rollback modes have distinct immutable MTCT paths");
    const std::filesystem::path default_tile =
        tile_path(root.path, default_candidate.tiles[0].tile_key);
    const std::filesystem::path rollback_tile =
        tile_path(root.path, rollback_candidate.tiles[0].tile_key);
    CHECK(std::filesystem::exists(default_tile) &&
              std::filesystem::exists(rollback_tile) &&
              std::filesystem::exists(manifest_path(
                  root.path, default_candidate.installation_key)) &&
              std::filesystem::exists(manifest_path(
                  root.path, rollback_candidate.installation_key)),
          "both bake modes remain independently addressable in one cache root");
    CHECK(read_bytes(default_tile) != read_bytes(rollback_tile),
          "the supported bake modes retain their distinct mesher bytes");

    TileCandidate wrong_mode_tile{};
    std::uint64_t artifact_bytes = 0u;
    CHECK(!matter::terrain_collision::detail::validate_tile_artifact_bytes(
              read_bytes(rollback_tile), default_definition,
              default_definition.sectors[0], wrong_mode_tile, artifact_bytes,
              error),
          "MTCT validation rejects bytes named by another bake-mode identity");
}

void flip_byte(const std::filesystem::path& path, std::size_t offset) {
    std::vector<std::uint8_t> bytes = read_bytes(path);
    CHECK(offset < bytes.size(), "corruption offset lies within fixture file");
    if (offset < bytes.size()) bytes[offset] ^= 0x5au;
    write_bytes(path, bytes);
}

void test_corrupt_artifacts_are_rebuilt_once_and_fail_closed() {
    TestField field = make_field(kPlaneField);
    const CanonicalDefinition definition = definition_for(field);
    TempRoot root("corruption");
    TerrainCollisionCandidate baseline{};
    std::string error;
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, definition, root.path, {}, baseline, error),
          error.c_str());
    const std::filesystem::path tile =
        tile_path(root.path, baseline.tiles[0].tile_key);
    const std::filesystem::path manifest =
        manifest_path(root.path, baseline.installation_key);
    const std::vector<std::uint8_t> pristine_tile = read_bytes(tile);
    std::atomic<unsigned> mesher_calls{0u};
    const auto counting_hooks = counting_mesher_hooks(mesher_calls);

    flip_byte(tile, 0u);
    TerrainCollisionCandidate repaired{};
    mesher_calls.store(0u, std::memory_order_relaxed);
    CHECK(load_with_hooks(field, definition, root.path, {}, counting_hooks,
                          repaired, error),
          error.c_str());
    CHECK(mesher_calls.load(std::memory_order_relaxed) == 1u &&
              read_bytes(tile) == pristine_tile,
          "a corrupt MTCT header gets exactly one clean deterministic rebuild");

    std::vector<std::uint8_t> truncated = pristine_tile;
    truncated.pop_back();
    write_bytes(tile, truncated);
    mesher_calls.store(0u, std::memory_order_relaxed);
    CHECK(load_with_hooks(field, definition, root.path, {}, counting_hooks,
                          repaired, error),
          error.c_str());
    CHECK(mesher_calls.load(std::memory_order_relaxed) == 1u &&
              read_bytes(tile) == pristine_tile,
          "a truncated MTCT payload gets exactly one clean rebuild");

    const std::uint64_t vertex_count = repaired.tiles[0].vertices.size();
    const std::size_t first_index_offset =
        144u + static_cast<std::size_t>(vertex_count) * sizeof(Float3);
    flip_byte(tile, first_index_offset);
    mesher_calls.store(0u, std::memory_order_relaxed);
    CHECK(load_with_hooks(field, definition, root.path, {}, counting_hooks,
                          repaired, error),
          error.c_str());
    CHECK(mesher_calls.load(std::memory_order_relaxed) == 1u &&
              read_bytes(tile) == pristine_tile,
          "an altered MTCT index is detected and rebuilt");

    flip_byte(manifest, 80u);
    mesher_calls.store(0u, std::memory_order_relaxed);
    CHECK(load_with_hooks(field, definition, root.path, {}, counting_hooks,
                          repaired, error),
          error.c_str());
    CHECK(mesher_calls.load(std::memory_order_relaxed) == 0u &&
              matter::terrain_collision::detail::validate_generation_manifest(
                  manifest, definition, repaired, error),
          "a corrupt manifest is republished from validated tile artifacts");

    std::error_code filesystem_error;
    CHECK(std::filesystem::remove(tile, filesystem_error) && !filesystem_error,
          "the referenced tile is removed for the missing-artifact fixture");
    mesher_calls.store(0u, std::memory_order_relaxed);
    CHECK(load_with_hooks(field, definition, root.path, {}, counting_hooks,
                          repaired, error),
          error.c_str());
    CHECK(mesher_calls.load(std::memory_order_relaxed) == 1u &&
              read_bytes(tile) == pristine_tile,
          "a missing referenced tile is rebuilt from the canonical field");

    TempRoot failed_root("rebuild-failure");
    const std::filesystem::path failed_tile =
        tile_path(failed_root.path, baseline.tiles[0].tile_key);
    std::filesystem::create_directories(failed_tile.parent_path());
    std::vector<std::uint8_t> corrupt = pristine_tile;
    corrupt[0] ^= 0xffu;
    write_bytes(failed_tile, corrupt);
    TerrainCollisionCandidate not_published{};
    std::atomic<unsigned> failed_mesher_calls{0u};
    matter::terrain_collision::detail::BuildTestHooks failing_hooks{};
    failing_hooks.mesh_tile = [&failed_mesher_calls](
        const terrain_field::FieldRuntime&, const SectorCoordinate&,
        std::int8_t, float, terrain_mesher::SectorMesh&, std::string& mesh_error) {
        failed_mesher_calls.fetch_add(1u, std::memory_order_relaxed);
        mesh_error = "injected canonical mesher failure";
        return false;
    };
    CHECK(!load_with_hooks(field, definition, failed_root.path, {}, failing_hooks,
                           not_published, error),
          "an injected mesher failure during corrupt-tile rebuild fails closed");
    CHECK(failed_mesher_calls.load(std::memory_order_relaxed) == 1u,
          "a failed corrupt-tile rebuild invokes the mesher exactly once");
    CHECK(error.find("mesher failed") != std::string::npos,
          "the rebuild failure propagates as an actual mesher failure");
    CHECK(not_published.tiles.empty() &&
              read_bytes(failed_tile) == corrupt &&
              !std::filesystem::exists(manifest_path(
                  failed_root.path, definition.installation_key)),
          "a failed rebuild exposes no candidate and publishes no generation manifest");
}

using PublicationCommitPoint =
    matter::terrain_collision::detail::PublicationCommitPoint;

class TwoPublisherBarrier {
public:
    TwoPublisherBarrier(bool wait_for_tile, bool wait_for_manifest)
        : wait_for_tile_(wait_for_tile), wait_for_manifest_(wait_for_manifest) {}

    void arrive(PublicationCommitPoint point) {
        if (point == PublicationCommitPoint::TileNoReplace && wait_for_tile_) {
            wait(tile_arrivals_);
        } else if (point == PublicationCommitPoint::ManifestNoReplace &&
                   wait_for_manifest_) {
            wait(manifest_arrivals_);
        }
    }

    bool timed_out() const noexcept {
        return timed_out_.load(std::memory_order_relaxed);
    }

private:
    void wait(unsigned& arrivals) {
        std::unique_lock<std::mutex> lock(mutex_);
        ++arrivals;
        if (arrivals == 2u) {
            condition_.notify_all();
            return;
        }
        if (!condition_.wait_for(lock, std::chrono::seconds(10),
                                 [&arrivals] { return arrivals >= 2u; })) {
            timed_out_.store(true, std::memory_order_relaxed);
            arrivals = 2u;
            condition_.notify_all();
        }
    }

    bool wait_for_tile_ = false;
    bool wait_for_manifest_ = false;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    unsigned tile_arrivals_ = 0u;
    unsigned manifest_arrivals_ = 0u;
    std::atomic<bool> timed_out_{false};
};

std::size_t count_files_with_extension(const std::filesystem::path& directory,
                                       const char* extension) {
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error) return 0u;
    std::size_t count = 0u;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.is_regular_file() && entry.path().extension() == extension)
            ++count;
    return count;
}

void test_cancellation_is_polled_at_atomic_publication_boundaries() {
    TestField field = make_field(kPlaneField);
    const CanonicalDefinition definition = definition_for(field);
    TempRoot reference_root("publication-reference");
    TerrainCollisionCandidate reference{};
    std::string error;
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, definition, reference_root.path, {}, reference,
              error),
          error.c_str());
    const std::filesystem::path reference_tile =
        tile_path(reference_root.path, reference.tiles[0].tile_key);
    const std::vector<std::uint8_t> pristine_tile = read_bytes(reference_tile);

    const auto cancel_at = [&](const char* label, PublicationCommitPoint target,
                               const std::function<void(const TempRoot&)>& seed,
                               const std::function<void(const TempRoot&)>& verify) {
        TempRoot root(label);
        seed(root);
        std::atomic<bool> cancelled{false};
        matter::terrain_collision::detail::BuildTestHooks hooks{};
        hooks.before_publication_commit =
            [&cancelled, target](PublicationCommitPoint point) {
                if (point == target)
                    cancelled.store(true, std::memory_order_release);
            };
        TerrainCollisionCandidate candidate{};
        error.clear();
        CHECK(!load_with_hooks(
                  field, definition, root.path,
                  [&cancelled] {
                      return cancelled.load(std::memory_order_acquire);
                  },
                  hooks, candidate, error),
              "cancellation at an atomic publication boundary fails the build");
        CHECK(error.find("cancel") != std::string::npos &&
                  candidate.tiles.empty(),
              "publication-boundary cancellation is precise and exposes no candidate");
        verify(root);
    };

    const auto no_seed = [](const TempRoot&) {};
    cancel_at(
        "cancel-tile-no-replace", PublicationCommitPoint::TileNoReplace,
        no_seed, [&](const TempRoot& root) {
            CHECK(count_files_with_extension(
                      root.path / "terrain_collision" / "v1" / "tiles",
                      ".mtct") == 0u &&
                      count_files_with_extension(
                          root.path / "terrain_collision" / "v1" /
                              "generations",
                          ".mtcm") == 0u,
                  "tile no-replace cancellation publishes neither tile nor manifest");
        });
    cancel_at(
        "cancel-manifest-no-replace", PublicationCommitPoint::ManifestNoReplace,
        no_seed, [&](const TempRoot& root) {
            CHECK(std::filesystem::exists(tile_path(
                      root.path, reference.tiles[0].tile_key)) &&
                      !std::filesystem::exists(manifest_path(
                          root.path, definition.installation_key)),
                  "manifest no-replace cancellation leaves only the committed tile");
        });

    std::vector<std::uint8_t> corrupt_tile = pristine_tile;
    corrupt_tile[0] ^= 0x5au;
    cancel_at(
        "cancel-tile-replace", PublicationCommitPoint::TileReplace,
        [&](const TempRoot& root) {
            const std::filesystem::path destination =
                tile_path(root.path, reference.tiles[0].tile_key);
            std::filesystem::create_directories(destination.parent_path());
            write_bytes(destination, corrupt_tile);
        },
        [&](const TempRoot& root) {
            CHECK(read_bytes(tile_path(root.path, reference.tiles[0].tile_key)) ==
                          corrupt_tile &&
                      !std::filesystem::exists(manifest_path(
                          root.path, definition.installation_key)),
                  "tile replacement cancellation preserves the prior corrupt file");
        });

    cancel_at(
        "cancel-manifest-replace", PublicationCommitPoint::ManifestReplace,
        [&](const TempRoot& root) {
            TerrainCollisionCandidate seeded{};
            std::string seed_error;
            CHECK(matter::terrain_collision::load_or_build_candidate(
                      field.runtime, definition, root.path, {}, seeded,
                      seed_error),
                  seed_error.c_str());
            flip_byte(manifest_path(root.path, definition.installation_key), 80u);
        },
        [&](const TempRoot& root) {
            std::string validation_error;
            CHECK(!matter::terrain_collision::detail::validate_generation_manifest(
                      manifest_path(root.path, definition.installation_key),
                      definition, reference, validation_error),
                  "manifest replacement cancellation preserves the corrupt manifest");
        });
}

struct ConcurrentBuildResult {
    bool ok = false;
    TerrainCollisionCandidate candidate{};
    std::string error;
};

void test_concurrent_publishers_validate_winners_and_converge() {
    TestField field = make_field(kPlaneField);
    const CanonicalDefinition definition = definition_for(field);
    TempRoot root("concurrent-publishers");
    TwoPublisherBarrier barrier(true, true);
    std::atomic<unsigned> mesher_calls{0u};
    std::atomic<unsigned> tile_winners{0u};
    std::atomic<unsigned> manifest_winners{0u};
    auto hooks = counting_mesher_hooks(mesher_calls);
    hooks.before_publication_commit = [&](PublicationCommitPoint point) {
        barrier.arrive(point);
        if (point == PublicationCommitPoint::TileExistingWinner)
            tile_winners.fetch_add(1u, std::memory_order_relaxed);
        if (point == PublicationCommitPoint::ManifestExistingWinner)
            manifest_winners.fetch_add(1u, std::memory_order_relaxed);
    };

    ConcurrentBuildResult results[2];
    std::thread first([&] {
        results[0].ok = load_with_hooks(
            field, definition, root.path, {}, hooks, results[0].candidate,
            results[0].error);
    });
    std::thread second([&] {
        results[1].ok = load_with_hooks(
            field, definition, root.path, {}, hooks, results[1].candidate,
            results[1].error);
    });
    first.join();
    second.join();

    CHECK(!barrier.timed_out(),
          "both writers deterministically reach tile and manifest commit races");
    CHECK(results[0].ok && results[1].ok,
          "both concurrent publishers accept a validated immutable winner");
    CHECK(mesher_calls.load(std::memory_order_relaxed) == 2u &&
              tile_winners.load(std::memory_order_relaxed) == 1u &&
              manifest_winners.load(std::memory_order_relaxed) == 1u,
          "one loser validates each concurrently published tile and manifest winner");
    if (!results[0].ok || !results[1].ok) return;
    CHECK(same_tiles(results[0].candidate, results[1].candidate),
          "concurrent publishers converge to byte-identical candidate geometry");
    CHECK(count_files_with_extension(
              root.path / "terrain_collision" / "v1" / "tiles", ".mtct") ==
              1u &&
              count_files_with_extension(
                  root.path / "terrain_collision" / "v1" / "generations",
                  ".mtcm") == 1u,
          "concurrent publication leaves one canonical tile and generation file");
    std::string validation_error;
    CHECK(matter::terrain_collision::detail::validate_generation_manifest(
              manifest_path(root.path, results[0].candidate.installation_key),
              definition, results[0].candidate, validation_error),
          validation_error.c_str());
}

void assert_cancelled_existing_winner(PublicationCommitPoint race_point,
                                      PublicationCommitPoint cancel_point,
                                      const char* label) {
    TestField field = make_field(kPlaneField);
    const CanonicalDefinition definition = definition_for(field);
    TempRoot root(label);
    TwoPublisherBarrier barrier(
        race_point == PublicationCommitPoint::TileNoReplace,
        race_point == PublicationCommitPoint::ManifestNoReplace);
    ConcurrentBuildResult results[2];
    std::atomic<unsigned> cancelled_winners{0u};

    const auto run = [&](ConcurrentBuildResult& result) {
        std::atomic<bool> cancelled{false};
        matter::terrain_collision::detail::BuildTestHooks hooks{};
        hooks.before_publication_commit = [&](PublicationCommitPoint point) {
            barrier.arrive(point);
            if (point == cancel_point) {
                cancelled_winners.fetch_add(1u, std::memory_order_relaxed);
                cancelled.store(true, std::memory_order_release);
            }
        };
        result.ok = load_with_hooks(
            field, definition, root.path,
            [&cancelled] {
                return cancelled.load(std::memory_order_acquire);
            },
            hooks, result.candidate, result.error);
    };
    std::thread first([&] { run(results[0]); });
    std::thread second([&] { run(results[1]); });
    first.join();
    second.join();

    const unsigned success_count =
        (results[0].ok ? 1u : 0u) + (results[1].ok ? 1u : 0u);
    const ConcurrentBuildResult& failed = results[0].ok ? results[1] : results[0];
    const ConcurrentBuildResult& winner = results[0].ok ? results[0] : results[1];
    CHECK(!barrier.timed_out() && success_count == 1u &&
              cancelled_winners.load(std::memory_order_relaxed) == 1u,
          "one concurrent loser is cancelled at its validated-winner boundary");
    CHECK(!failed.ok && failed.candidate.tiles.empty() &&
              failed.error.find("cancel") != std::string::npos,
          "a cancelled validated-winner path cannot return a candidate");
    if (!winner.ok) return;
    std::string validation_error;
    CHECK(matter::terrain_collision::detail::validate_generation_manifest(
              manifest_path(root.path, winner.candidate.installation_key),
              definition, winner.candidate, validation_error),
          "the non-cancelled publisher leaves one valid generation");
}

void test_cancellation_precedes_validated_winner_acceptance() {
    assert_cancelled_existing_winner(
        PublicationCommitPoint::TileNoReplace,
        PublicationCommitPoint::TileExistingWinner,
        "cancel-tile-existing-winner");
    assert_cancelled_existing_winner(
        PublicationCommitPoint::ManifestNoReplace,
        PublicationCommitPoint::ManifestExistingWinner,
        "cancel-manifest-existing-winner");
}

void test_interrupted_temporary_file_never_becomes_addressable() {
    TestField field = make_field(kPlaneField);
    const CanonicalDefinition definition = definition_for(field);
    TempRoot root("interrupted");
    const std::filesystem::path manifest =
        manifest_path(root.path, definition.installation_key);
    std::filesystem::create_directories(manifest.parent_path());
    const std::filesystem::path interrupted =
        manifest.parent_path() / (manifest.filename().string() + ".tmp-interrupted");
    write_bytes(interrupted, {0x4du, 0x54u, 0x43u});
    TerrainCollisionCandidate candidate{};
    std::string error;
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, definition, root.path, {}, candidate, error),
          error.c_str());
    CHECK(std::filesystem::exists(interrupted),
          "an unrelated interrupted sibling remains quarantined and unaddressable");
    CHECK(std::filesystem::exists(manifest) &&
              matter::terrain_collision::detail::validate_generation_manifest(
                  manifest, definition, candidate, error),
          "only a complete validated generation receives the canonical manifest path");
}

}  // namespace

int main() {
    BakeModeGuard mode;
    mode.contour_seams(true);
    test_fixture_conversion_is_valid_and_repeatable();
    test_all_material_buckets_are_flattened();
    test_uniform_density_tiles_succeed_empty();
    test_final_overlaid_runtime_is_meshed();
    test_contour_coverage_validator_rejects_middle_gap();
    test_equal_rung_neighbors_share_vertices_on_every_axis();
    test_validators_reject_invalid_geometry_and_headers();
    test_cache_hits_material_reuse_and_region_reuse();
    test_bake_modes_have_distinct_tile_and_generation_identities();
    test_corrupt_artifacts_are_rebuilt_once_and_fail_closed();
    test_cancellation_is_polled_at_atomic_publication_boundaries();
    test_concurrent_publishers_validate_winners_and_converge();
    test_cancellation_precedes_validated_winner_acceptance();
    test_interrupted_temporary_file_never_becomes_addressable();
    return check_summary();
}
