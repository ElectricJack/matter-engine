#include "check.h"
#include "../src/hydrology/river_geometry.h"
#include "../src/terrain_collision/terrain_collision_artifact.h"
#include "../src/terrain_mesher.h"
#include "../src/terrain_river_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
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

std::uint32_t float_bits(float value) {
    std::uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::set<VertexBits> world_vertices(const TileCandidate& tile) {
    std::set<VertexBits> result;
    for (const Float3& local : tile.vertices) {
        const Float3 world{tile.origin_m.x + local.x,
                           tile.origin_m.y + local.y,
                           tile.origin_m.z + local.z};
        result.insert({float_bits(world.x), float_bits(world.y), float_bits(world.z)});
    }
    return result;
}

void assert_equal_rung_pair(const char* label, const char* field_text,
                            Float3 max_m, std::size_t axis) {
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
    const std::set<VertexBits> first = world_vertices(candidate.tiles[0]);
    const std::set<VertexBits> second = world_vertices(candidate.tiles[1]);
    std::vector<VertexBits> shared;
    std::set_intersection(first.begin(), first.end(), second.begin(), second.end(),
                          std::back_inserter(shared));
    CHECK(!shared.empty(),
          "equal-rung neighbors retain bit-identical shared world vertices");
    float first_max = -std::numeric_limits<float>::infinity();
    float second_min = std::numeric_limits<float>::infinity();
    for (const Float3& local : candidate.tiles[0].vertices) {
        const float values[] = {
            candidate.tiles[0].origin_m.x + local.x,
            candidate.tiles[0].origin_m.y + local.y,
            candidate.tiles[0].origin_m.z + local.z,
        };
        first_max = std::max(first_max, values[axis]);
    }
    for (const Float3& local : candidate.tiles[1].vertices) {
        const float values[] = {
            candidate.tiles[1].origin_m.x + local.x,
            candidate.tiles[1].origin_m.y + local.y,
            candidate.tiles[1].origin_m.z + local.z,
        };
        second_min = std::min(second_min, values[axis]);
    }
    CHECK(second_min <= first_max,
          "equal-rung ownership bridge leaves no open gap at the shared plane");
}

void test_equal_rung_neighbors_share_vertices_on_every_axis() {
    assert_equal_rung_pair("neighbor-x", kPlaneField,
                           {2.0f * kSectorSize, kSectorSize, kSectorSize}, 0u);
    assert_equal_rung_pair("neighbor-y", kVerticalPlaneField,
                           {kSectorSize, 2.0f * kSectorSize, kSectorSize}, 1u);
    assert_equal_rung_pair("neighbor-z", kPlaneField,
                           {kSectorSize, kSectorSize, 2.0f * kSectorSize}, 2u);
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

    flip_byte(tile, 0u);
    TerrainCollisionCandidate repaired{};
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, definition, root.path, {}, repaired, error),
          error.c_str());
    CHECK(repaired.stats.built_tiles == 1u && read_bytes(tile) == pristine_tile,
          "a corrupt MTCT header gets exactly one clean deterministic rebuild");

    std::vector<std::uint8_t> truncated = pristine_tile;
    truncated.pop_back();
    write_bytes(tile, truncated);
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, definition, root.path, {}, repaired, error),
          error.c_str());
    CHECK(repaired.stats.built_tiles == 1u && read_bytes(tile) == pristine_tile,
          "a truncated MTCT payload gets exactly one clean rebuild");

    const std::uint64_t vertex_count = repaired.tiles[0].vertices.size();
    const std::size_t first_index_offset =
        144u + static_cast<std::size_t>(vertex_count) * sizeof(Float3);
    flip_byte(tile, first_index_offset);
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, definition, root.path, {}, repaired, error),
          error.c_str());
    CHECK(repaired.stats.built_tiles == 1u && read_bytes(tile) == pristine_tile,
          "an altered MTCT index is detected and rebuilt");

    flip_byte(manifest, 80u);
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, definition, root.path, {}, repaired, error),
          error.c_str());
    CHECK(repaired.stats.cache_hit_tiles == 1u && repaired.stats.built_tiles == 0u &&
              matter::terrain_collision::detail::validate_generation_manifest(
                  manifest, definition, repaired, error),
          "a corrupt manifest is republished from validated tile artifacts");

    std::error_code filesystem_error;
    CHECK(std::filesystem::remove(tile, filesystem_error) && !filesystem_error,
          "the referenced tile is removed for the missing-artifact fixture");
    CHECK(matter::terrain_collision::load_or_build_candidate(
              field.runtime, definition, root.path, {}, repaired, error),
          error.c_str());
    CHECK(repaired.stats.built_tiles == 1u && read_bytes(tile) == pristine_tile,
          "a missing referenced tile is rebuilt from the canonical field");

    TempRoot failed_root("rebuild-failure");
    const std::filesystem::path failed_tile =
        tile_path(failed_root.path, baseline.tiles[0].tile_key);
    std::filesystem::create_directories(failed_tile.parent_path());
    std::vector<std::uint8_t> corrupt = pristine_tile;
    corrupt[0] ^= 0xffu;
    write_bytes(failed_tile, corrupt);
    TerrainCollisionCandidate not_published{};
    unsigned cancel_checks = 0u;
    CHECK(!matter::terrain_collision::load_or_build_candidate(
              field.runtime, definition, failed_root.path,
              [&cancel_checks] { return ++cancel_checks >= 2u; },
              not_published, error),
          "an injected rebuild cancellation fails closed");
    CHECK(cancel_checks == 2u,
          "the injected failure occurs after the corrupt tile triggers remeshing");
    CHECK(error.find("cancel") != std::string::npos,
          "rebuild cancellation returns a precise cancellation error");
    CHECK(not_published.tiles.empty() &&
              !std::filesystem::exists(manifest_path(
                  failed_root.path, definition.installation_key)),
          "a failed rebuild exposes no candidate and publishes no generation manifest");
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
    test_fixture_conversion_is_valid_and_repeatable();
    test_all_material_buckets_are_flattened();
    test_uniform_density_tiles_succeed_empty();
    test_final_overlaid_runtime_is_meshed();
    test_equal_rung_neighbors_share_vertices_on_every_axis();
    test_validators_reject_invalid_geometry_and_headers();
    test_cache_hits_material_reuse_and_region_reuse();
    test_corrupt_artifacts_are_rebuilt_once_and_fail_closed();
    test_interrupted_temporary_file_never_becomes_addressable();
    return check_summary();
}
