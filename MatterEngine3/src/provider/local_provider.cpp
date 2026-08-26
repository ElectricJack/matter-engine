#include "local_provider.h"
#include "script/world_definition_loader.h"

#include "part_graph.h"        // -DMATTER_HAVE_SCRIPT_HOST pulls in script_host.h
#include "part_asset_v2.h"     // cache_path_resolved, cache_path_flat, FlatInstanceRef
#include "part_flatten.h"      // bake-time subtree flattening
#include "world_lights.h"
#include "world_tracer.h"
#include "part_asset.h"   // fnv1a64
#include "tileset_gtex.h" // gtex_content_hash, gtex_cache_hit (headless cache-hit load)
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "tileset_phase.h"
#include "bake_trace.h"        // Bake Lab task 1.3: tileset span split
#include "bake_trace_names.h"  // kSpanTileset
#include "material_registry.h"
#include "matter/log.h"
#include "hydrology/hydrology_settings.h"
#include "hydrology/physx_collision_input.h"
#include "hydrology/river_geometry.h"
#include "hydrology/river_section_coordinator.h"
#include "hydrology/river_section_graph.h"
#include "hydrology/hydrology_handoff_products.h"
#include "hydrology/water_visual_products.h"
#include "terrain_river_overlay.h"

#include <algorithm>

#if defined(MATTER_HAVE_AUTOREMESHER)
#include "mesh_retopo.hpp"     // retopo() TBB warm-up (see install_graph() below)
#include "mesh_indexed.hpp"
#include "mesh_transform.hpp"  // from_tri
#include "tri.h"               // Tri / TriEx / float3
#include "retopo_blacklist.h"  // load persistent crash-recovery journal
#include <cmath>               // std::sqrt for warm-up mesh construction
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <new>         // std::bad_alloc (Task 7 fix: fetch_parts skip-and-continue)
#include <regex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>   // std::exception (Task 7 fix: fetch_parts skip-and-continue)
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#ifdef _WIN32
#include <direct.h>      // _mkdir
#include <stdlib.h>      // _fullpath, _MAX_PATH
#ifndef PATH_MAX
#define PATH_MAX _MAX_PATH
#endif
#else
#include <limits.h>
#include <unistd.h>
#endif

using namespace part_graph;

namespace viewer {

// Deterministic splitmix64 (matches example_world's scatter exactly).
namespace {
// Filesystem portability shim: MinGW mkdir and realpath have different names.
#ifdef _WIN32
bool fs_realpath(const char* in, char* out)        { return _fullpath(out, in, PATH_MAX) != nullptr; }
#else
bool fs_realpath(const char* in, char* out)        { return realpath(in, out) != nullptr; }
#endif
struct Rng64 {
    uint64_t s;
    explicit Rng64(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s += 0x9e3779b97f4a7c15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    float range(float a, float b) {
        return a + (float)((next() >> 11) * (1.0 / 9007199254740992.0)) * (b - a);
    }
};
matter::Mat4f identity_transform() {
    matter::Mat4f result{};
    result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
    return result;
}
matter::Mat4f multiply_transform(const matter::Mat4f& a,
                                 const matter::Mat4f& b) {
    matter::Mat4f result{};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            for (int k = 0; k < 4; ++k)
                result.m[row * 4 + col] +=
                    a.m[row * 4 + k] * b.m[k * 4 + col];
    return result;
}
// Resolve a path (possibly relative to cwd) to an absolute path.
std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (fs_realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
}

#if defined(MATTER_HAVE_AUTOREMESHER)
// Phase 5 autoremesher (Task 15): one-shot TBB / geogram warm-up.
// See MatterEngine3/tests/retopo_integration_tests.cpp for the empirical
// motivation: on WSL2, the first retopo() call segfaults during TBB
// "Multithreading enabled" init if it happens AFTER any heap-heavy work like
// part_asset::save_v2 (which HostBaker::bake calls for every part). Calling
// retopo() FIRST on a small valid mesh initializes the TBB scheduler in a
// clean state and every subsequent retopo() succeeds. The N=4 spherified cube
// is the same fixture used by the integration test — small (~96 tris) but
// non-degenerate for the cross-field parameterizer.
void tbb_warmup_retopo() {
    std::vector<Tri> tris;
    tris.reserve(6 * 4 * 4 * 2);
    struct Face { float o[3], u[3], v[3]; };
    const Face faces[6] = {
        { {-1,-1,-1}, {2,0,0}, {0,2,0} },
        { {-1,-1, 1}, {0,2,0}, {2,0,0} },
        { {-1,-1,-1}, {0,0,2}, {2,0,0} },
        { {-1, 1,-1}, {2,0,0}, {0,0,2} },
        { {-1,-1,-1}, {0,2,0}, {0,0,2} },
        { { 1,-1,-1}, {0,0,2}, {0,2,0} },
    };
    const int N = 4;
    auto project = [](float x, float y, float z) {
        float r = std::sqrt(x*x + y*y + z*z);
        return make_float3(x / r, y / r, z / r);
    };
    for (const auto& f : faces) {
        std::vector<float3> grid((N + 1) * (N + 1));
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) {
                float s = static_cast<float>(i) / N;
                float t = static_cast<float>(j) / N;
                grid[j * (N + 1) + i] = project(
                    f.o[0] + s * f.u[0] + t * f.v[0],
                    f.o[1] + s * f.u[1] + t * f.v[1],
                    f.o[2] + s * f.u[2] + t * f.v[2]);
            }
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) {
                float3 a = grid[j * (N + 1) + i];
                float3 b = grid[j * (N + 1) + i + 1];
                float3 c = grid[(j + 1) * (N + 1) + i];
                float3 d = grid[(j + 1) * (N + 1) + i + 1];
                Tri t1, t2;
                t1.vertex0 = a; t1.vertex1 = b; t1.vertex2 = d;
                t1.centroid = make_float3((a.x+b.x+d.x)/3, (a.y+b.y+d.y)/3, (a.z+b.z+d.z)/3);
                t2.vertex0 = a; t2.vertex1 = d; t2.vertex2 = c;
                t2.centroid = make_float3((a.x+d.x+c.x)/3, (a.y+d.y+c.y)/3, (a.z+d.z+c.z)/3);
                tris.push_back(t1);
                tris.push_back(t2);
            }
    }
    MeshIndexed warm = from_tri(tris, nullptr);
    RetopoOptions opts;
    opts.threads = 1;
    RetopoResult wr = retopo(warm, opts);
    MATTER_LOGI("local_provider", "LocalProvider: TBB warm-up retopo ok=%d elapsed=%.3fs\n",
           (int)wr.ok, wr.elapsed_seconds);
}
#endif

// Extract the class name from a JS schema source so install-phase on_part
// callbacks can report a human-readable module name. Schemas follow the pattern:
//   class ClassName extends Part { ... }
// Scan for "class " followed by an identifier, stopping at whitespace or '{'.
// Returns empty string on parse failure (callback receives null module).
std::string class_name_from_source(const std::string& source) {
    const char* kw = "class ";
    const size_t kwlen = 6;
    size_t pos = source.find(kw);
    while (pos != std::string::npos) {
        size_t name_start = pos + kwlen;
        // Skip whitespace between 'class' and the identifier
        while (name_start < source.size() &&
               (source[name_start] == ' ' || source[name_start] == '\t'))
            ++name_start;
        if (name_start >= source.size()) break;
        // Read the identifier
        size_t name_end = name_start;
        while (name_end < source.size() &&
               (std::isalnum((unsigned char)source[name_end]) || source[name_end] == '_' || source[name_end] == '$'))
            ++name_end;
        if (name_end > name_start) {
            // Accept it if followed by whitespace or '{' (not a method/variable name collision)
            if (name_end < source.size() &&
                (source[name_end] == ' ' || source[name_end] == '\t' ||
                 source[name_end] == '\n' || source[name_end] == '\r' ||
                 source[name_end] == '{'))
                return source.substr(name_start, name_end - name_start);
        }
        pos = source.find(kw, pos + 1);
    }
    return {};
}

constexpr std::uint64_t kPhysxSdkVersion = 0x05060100u;
constexpr std::uint64_t kFluidAdapterVersion = 4u;
// v2: finite-to-empty coarse-grid interpolation and deterministic CPU normal
// repair change authority-mesh bytes, so v1 artifacts must not be reused.
constexpr std::uint64_t kFluidMesherContractVersion = 3u;
constexpr std::uint32_t kNvidiaVendorId = 0x10deu;

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
}

template <typename Value>
void hash_value(std::uint64_t& hash, const Value& value) {
    hash_bytes(hash, &value, sizeof(value));
}

void hash_string(std::uint64_t& hash, const std::string& value) {
    const std::uint64_t size = value.size();
    hash_value(hash, size);
    hash_bytes(hash, value.data(), value.size());
}

std::uint64_t nonzero_hash(std::uint64_t hash) {
    return hash == 0u ? 1u : hash;
}

std::string hex64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::uint32_t emitter_id(const std::string& text) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash == 0u ? 1u : hash;
}

std::string safe_cache_id(const std::string& id) {
    std::string result;
    result.reserve(id.size());
    for (const unsigned char byte : id) {
        result.push_back(std::isalnum(byte) || byte == '-' || byte == '_'
                             ? static_cast<char>(byte)
                             : '_');
    }
    return result.empty() ? "section" : result;
}

const matter::RiverSectionDefinition* initial_section(
    const matter::RiverNetworkDefinition& network) {
    const matter::RiverSectionDefinition* result = nullptr;
    for (const auto& section : network.sections) {
        if (!section.after_section_ids.empty() ||
            !section.upstream_spillway_section_ids.empty())
            continue;
        if (!result || section.id < result->id) result = &section;
    }
    return result;
}

const matter::RiverDefinition* section_river(
    const matter::RiverNetworkDefinition& network,
    const matter::RiverSectionDefinition& section) {
    const auto found = std::find_if(
        network.rivers.begin(), network.rivers.end(),
        [&](const matter::RiverDefinition& river) {
            return river.name == section.river;
        });
    return found == network.rivers.end() ? nullptr : &*found;
}

hydrology::RiverCentrelineSample sample_at_distance(
    const hydrology::RiverGeometry& geometry, float distance_m) {
    if (distance_m <= geometry.centreline.front().distance_m)
        return geometry.centreline.front();
    for (std::size_t index = 1; index < geometry.centreline.size(); ++index) {
        const auto& next = geometry.centreline[index];
        if (distance_m > next.distance_m) continue;
        const auto& previous = geometry.centreline[index - 1u];
        const float span = next.distance_m - previous.distance_m;
        const float t = span > 0.0f
            ? (distance_m - previous.distance_m) / span : 0.0f;
        hydrology::RiverCentrelineSample result = previous;
        const auto blend = [t](float a, float b) { return a + (b - a) * t; };
        result.position_m = {
            blend(previous.position_m.x, next.position_m.x),
            blend(previous.position_m.y, next.position_m.y),
            blend(previous.position_m.z, next.position_m.z)};
        result.tangent = {
            blend(previous.tangent.x, next.tangent.x),
            blend(previous.tangent.y, next.tangent.y),
            blend(previous.tangent.z, next.tangent.z)};
        result.lateral = {
            blend(previous.lateral.x, next.lateral.x), 0.0f,
            blend(previous.lateral.z, next.lateral.z)};
        result.distance_m = distance_m;
        result.width_m = blend(previous.width_m, next.width_m);
        result.depth_m = blend(previous.depth_m, next.depth_m);
        result.asymmetry = blend(previous.asymmetry, next.asymmetry);
        return result;
    }
    return geometry.centreline.back();
}

bool sample_terrain(const hydrology::TerrainHeightSampler& terrain,
                    float fallback, float x, float z, float& height) {
    if (terrain && terrain(x, z, height) && std::isfinite(height)) return true;
    height = fallback;
    return std::isfinite(height);
}

bool build_terrain_surface(
    const hydrology::RiverGeometry& geometry,
    const matter::HydrologyFluidRequest& fluid,
    float from_m,
    float to_m,
    float cell_size,
    float dry_margin,
    const hydrology::TerrainHeightSampler& terrain,
    hydrology::FluidCollisionSurface& surface,
    matter::Aabb& section_bounds,
    hydrology::FluidBakeError& error) {
    from_m = std::max(geometry.centreline.front().distance_m, from_m);
    to_m = std::min(geometry.centreline.back().distance_m,
                    to_m + fluid.virtual_dam.thickness_m);
    float minimum_x = std::numeric_limits<float>::infinity();
    float minimum_z = std::numeric_limits<float>::infinity();
    float maximum_x = -std::numeric_limits<float>::infinity();
    float maximum_z = -std::numeric_limits<float>::infinity();
    const auto include_sample = [&](const auto& sample) {
        const float half_width = sample.width_m * 0.5f + cell_size;
        minimum_x = std::min(minimum_x, sample.position_m.x - half_width);
        minimum_z = std::min(minimum_z, sample.position_m.z - half_width);
        maximum_x = std::max(maximum_x, sample.position_m.x + half_width);
        maximum_z = std::max(maximum_z, sample.position_m.z + half_width);
    };
    include_sample(sample_at_distance(geometry, from_m));
    for (const auto& sample : geometry.centreline) {
        if (sample.distance_m <= from_m) continue;
        if (sample.distance_m >= to_m) break;
        include_sample(sample);
    }
    include_sample(sample_at_distance(geometry, to_m));
    if (!(to_m > from_m) || !(maximum_x > minimum_x) ||
        !(maximum_z > minimum_z)) {
        error = {hydrology::FluidBakeCode::InvalidInput,
                 "authored fluid section has no finite terrain extent"};
        return false;
    }
    // The section bounds describe the authored wet domain.  The physical
    // terrain must continue beyond its validation-only dry collar so a
    // particle is always supported before the collar can report an escape.
    // This is sampled terrain, not an artificial wall.
    const float collision_minimum_x = minimum_x - dry_margin - cell_size;
    const float collision_minimum_z = minimum_z - dry_margin - cell_size;
    const float collision_maximum_x = maximum_x + dry_margin + cell_size;
    const float collision_maximum_z = maximum_z + dry_margin + cell_size;
    const auto dimension = [cell_size](float extent) {
        return static_cast<std::uint32_t>(
            std::max(2.0f, std::ceil(extent / cell_size) + 1.0f));
    };
    const std::uint32_t nx = dimension(collision_maximum_x - collision_minimum_x);
    const std::uint32_t nz = dimension(collision_maximum_z - collision_minimum_z);
    const std::uint64_t vertex_count = static_cast<std::uint64_t>(nx) * nz;
    if (vertex_count > 4000000u) {
        error = {hydrology::FluidBakeCode::CapacityExceeded,
                 "authored fluid terrain collision exceeds the bounded grid capacity"};
        return false;
    }
    surface = {};
    surface.kind = hydrology::FluidCollisionSurfaceKind::Terrain;
    surface.mesh.vertices.reserve(static_cast<std::size_t>(vertex_count));
    float minimum_y = std::numeric_limits<float>::infinity();
    float maximum_y = -std::numeric_limits<float>::infinity();
    const float fallback_y = geometry.bounds_m.minimum.y;
    for (std::uint32_t z_index = 0; z_index < nz; ++z_index) {
        const float z = z_index + 1u == nz ? collision_maximum_z
            : collision_minimum_z + static_cast<float>(z_index) * cell_size;
        for (std::uint32_t x_index = 0; x_index < nx; ++x_index) {
            const float x = x_index + 1u == nx ? collision_maximum_x
                : collision_minimum_x + static_cast<float>(x_index) * cell_size;
            float y = 0.0f;
            if (!sample_terrain(terrain, fallback_y, x, z, y)) {
                error = {hydrology::FluidBakeCode::InvalidInput,
                         "terrain height sampling failed for the authored fluid section"};
                return false;
            }
            minimum_y = std::min(minimum_y, y);
            maximum_y = std::max(maximum_y, y);
            surface.mesh.vertices.push_back({x, y, z});
        }
    }
    surface.mesh.indices.reserve(
        static_cast<std::size_t>(nx - 1u) * (nz - 1u) * 6u);
    for (std::uint32_t z = 0; z + 1u < nz; ++z) {
        for (std::uint32_t x = 0; x + 1u < nx; ++x) {
            const std::uint32_t a = z * nx + x;
            const std::uint32_t b = a + 1u;
            const std::uint32_t c = a + nx;
            const std::uint32_t d = c + 1u;
            surface.mesh.indices.insert(surface.mesh.indices.end(),
                                        {a, d, b, a, c, d});
        }
    }
    section_bounds = {{minimum_x, minimum_y, minimum_z},
                      {maximum_x,
                       maximum_y + fluid.virtual_dam.height_m,
                       maximum_z}};
    return true;
}

hydrology::FluidCollisionSurface build_dam_surface(
    const hydrology::RiverCentrelineSample& sample,
    const matter::HydrologyVirtualDam& dam,
    float bed_y) {
    float tx = sample.tangent.x;
    float tz = sample.tangent.z;
    const float tangent_length = std::hypot(tx, tz);
    if (tangent_length > 1.0e-6f) {
        tx /= tangent_length;
        tz /= tangent_length;
    } else {
        tx = 1.0f;
        tz = 0.0f;
    }
    float lx = -tz;
    float lz = tx;
    const float half_thickness = dam.thickness_m * 0.5f;
    const float half_width = sample.width_m * 0.6f;
    const auto point = [&](float along, float across, float y) {
        return matter::Float3{sample.position_m.x + tx * along + lx * across,
                              y,
                              sample.position_m.z + tz * along + lz * across};
    };
    hydrology::FluidCollisionSurface surface{};
    surface.kind = hydrology::FluidCollisionSurfaceKind::VirtualDam;
    surface.mesh.vertices = {
        point(-half_thickness, -half_width, bed_y),
        point( half_thickness, -half_width, bed_y),
        point( half_thickness,  half_width, bed_y),
        point(-half_thickness,  half_width, bed_y),
        point(-half_thickness, -half_width, bed_y + dam.height_m),
        point( half_thickness, -half_width, bed_y + dam.height_m),
        point( half_thickness,  half_width, bed_y + dam.height_m),
        point(-half_thickness,  half_width, bed_y + dam.height_m),
    };
    surface.mesh.indices = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5,
        2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7,
    };
    return surface;
}

matter::Aabb mesh_bounds(const hydrology::FluidCollisionMesh& mesh) {
    matter::Aabb bounds{{std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::infinity()},
                         {-std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}};
    for (const auto vertex : mesh.vertices) {
        bounds.minimum.x = std::min(bounds.minimum.x, vertex.x);
        bounds.minimum.y = std::min(bounds.minimum.y, vertex.y);
        bounds.minimum.z = std::min(bounds.minimum.z, vertex.z);
        bounds.maximum.x = std::max(bounds.maximum.x, vertex.x);
        bounds.maximum.y = std::max(bounds.maximum.y, vertex.y);
        bounds.maximum.z = std::max(bounds.maximum.z, vertex.z);
    }
    return bounds;
}

std::uint64_t pbd_revision(
    const matter::HydrologyFluidRequest& fluid,
    const std::vector<hydrology::FluidEmitter>& emitters) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    hash_value(hash, fluid.pbd);
    hash_value(hash, fluid.limits);
    for (const auto& emitter : emitters) {
        hash_value(hash, emitter.id);
        hash_value(hash, emitter.shape);
        hash_value(hash, emitter.position_m);
        hash_value(hash, emitter.direction);
        hash_value(hash, emitter.lateral_axis);
        hash_value(hash, emitter.up_axis);
        hash_value(hash, emitter.initial_velocity_mps);
        hash_value(hash, emitter.flow_m3s);
        hash_value(hash, emitter.radius_m);
        hash_value(hash, emitter.half_extent_m);
        hash_value(hash, emitter.channel_depth_m);
        hash_value(hash, emitter.channel_asymmetry);
        hash_value(hash, emitter.start_step);
        hash_value(hash, emitter.stop_step);
    }
    return nonzero_hash(hash);
}

std::uint64_t dam_revision(const matter::HydrologyVirtualDam& dam,
                           float distance_m,
                           const matter::Aabb& bounds) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    hash_value(hash, dam);
    hash_value(hash, distance_m);
    hash_value(hash, bounds);
    return nonzero_hash(hash);
}

std::uint64_t section_revision(
    const matter::RiverSectionDefinition& section,
    const std::vector<std::uint64_t>& upstream_handoff_keys) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    hash_string(hash, section.id);
    hash_string(hash, section.river);
    hash_value(hash, section.from_m);
    hash_value(hash, section.to_m);
    hash_value(hash, section.dry_margin_m);
    for (const auto& id : section.emitter_ids) hash_string(hash, id);
    for (const auto& waterfall : section.waterfalls) hash_value(hash, waterfall);
    if (section.terminal_pool) hash_value(hash, *section.terminal_pool);
    if (section.terminal_spillway) {
        hash_string(hash, section.terminal_spillway->id);
        hash_value(hash, section.terminal_spillway->distance_m);
        hash_value(hash, section.terminal_spillway->width_m);
        hash_value(hash, section.terminal_spillway->effective_depth_m);
        hash_value(hash, section.terminal_spillway->overlap_m);
        hash_value(hash, section.terminal_spillway->dam_offset_m);
    }
    for (const auto key : upstream_handoff_keys) hash_value(hash, key);
    return nonzero_hash(hash);
}

std::uint64_t section_water_animation_semantic_key(
    std::uint64_t section_semantic_key,
    std::uint64_t section_payload_digest,
    const matter::HydrologyMeshAnimationProfile& profile,
    const std::vector<hydrology::SpillwayHandoffRecord>& ownership) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    hash_string(hash, "water-mesh-animation-v1");
    hash_value(hash, section_semantic_key);
    hash_value(hash, section_payload_digest);
    hash_value(hash, profile.frames_per_second);
    hash_value(hash, profile.duration_seconds);
    hash_value(hash, profile.phase_offset_seconds);
    hash_value(hash, profile.frame_count);
    hash_value(hash, profile.sample_step_stride);
    hash_value(hash, profile.phase_offset_frames);
    for (const auto& handoff : ownership)
        hash_value(hash, handoff.semantic_key);
    return nonzero_hash(hash);
}

std::filesystem::path section_water_animation_path(
    const std::filesystem::path& cache_root,
    const std::string& section_id,
    std::uint64_t semantic_key) {
    return cache_root / "hydrology" / "animations" /
        (safe_cache_id(section_id) + "-" + hex64(semantic_key) + ".mhwa");
}

std::uint64_t collision_mesh_revision(
    const hydrology::FluidCollisionMesh& mesh) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const auto vertex : mesh.vertices) hash_value(hash, vertex);
    for (const auto index : mesh.indices) hash_value(hash, index);
    return nonzero_hash(hash);
}

std::uint64_t sensor_revision(
    const matter::HydrologyFillSensor& sensor,
    const hydrology::FluidFillSensor& resolved) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    hash_value(hash, sensor);
    hash_value(hash, resolved.frame_origin_m);
    hash_value(hash, resolved.longitudinal_axis_xz);
    hash_value(hash, resolved.lateral_axis_xz);
    hash_value(hash, resolved.frame_extent_m);
    return nonzero_hash(hash);
}

bool write_obj(const std::filesystem::path& path,
               const std::vector<float>& positions,
               const std::vector<std::uint32_t>& indices,
               std::string& error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "could not open " + path.string();
        return false;
    }
    stream << std::setprecision(9);
    for (std::size_t offset = 0; offset + 2u < positions.size(); offset += 3u)
        stream << "v " << positions[offset] << ' ' << positions[offset + 1u]
               << ' ' << positions[offset + 2u] << '\n';
    for (std::size_t offset = 0; offset + 2u < indices.size(); offset += 3u)
        stream << "f " << indices[offset] + 1u << ' '
               << indices[offset + 1u] + 1u << ' '
               << indices[offset + 2u] + 1u << '\n';
    if (!stream) {
        error = "could not write " + path.string();
        return false;
    }
    return true;
}

bool write_obj(const std::filesystem::path& path,
               const hydrology::FluidCollisionMesh& mesh,
               std::string& error) {
    std::vector<float> positions;
    positions.reserve(mesh.vertices.size() * 3u);
    for (const matter::Float3 vertex : mesh.vertices) {
        positions.push_back(vertex.x);
        positions.push_back(vertex.y);
        positions.push_back(vertex.z);
    }
    return write_obj(path, positions, mesh.indices, error);
}

bool write_particle_trace(
    const FluidBakeRequest& request,
    const hydrology::FluidBakeOutput& output,
    hydrology::FluidBakeCode terminal_code,
    const gpu_meshing::MeshResult& visual,
    bool accepted,
    std::string& error) {
    const char* configured = std::getenv("MATTER_HYDROLOGY_TRACE_DIR");
    if (!configured || configured[0] == '\0') return true;

    const std::filesystem::path configured_root(configured);
    if (!configured_root.is_absolute()) {
        error = "MATTER_HYDROLOGY_TRACE_DIR must be an absolute path";
        return false;
    }
    const std::filesystem::path root =
        request.input.network.sections.size() > 1u
            ? configured_root / request.section_id
            : configured_root;
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if (filesystem_error) {
        error = "could not create particle trace directory " + root.string() +
                ": " + filesystem_error.message();
        return false;
    }

    {
        std::ofstream stream(root / "particles.csv",
                             std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "could not open particle trace CSV";
            return false;
        }
        stream << "id,x_m,y_m,z_m,vx_mps,vy_mps,vz_mps\n"
               << std::setprecision(9);
        for (const hydrology::FluidParticle& particle : output.particles) {
            stream << particle.id << ',' << particle.position_m.x << ','
                   << particle.position_m.y << ',' << particle.position_m.z
                   << ',' << particle.velocity_mps.x << ','
                   << particle.velocity_mps.y << ','
                   << particle.velocity_mps.z << '\n';
        }
        if (!stream) {
            error = "could not write particle trace CSV";
            return false;
        }
    }
    {
        std::ofstream stream(root / "quarantined.csv",
                             std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "could not open quarantined-particle trace CSV";
            return false;
        }
        stream << "id,x_m,y_m,z_m\n" << std::setprecision(9);
        for (const hydrology::FluidQuarantinedParticle& particle :
             output.quarantined_particles) {
            stream << particle.id << ',' << particle.position_m.x << ','
                   << particle.position_m.y << ',' << particle.position_m.z
                   << '\n';
        }
        if (!stream) {
            error = "could not write quarantined-particle trace CSV";
            return false;
        }
    }
    {
        std::ofstream stream(root / "centreline.csv",
                             std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "could not open centreline trace CSV";
            return false;
        }
        stream << "distance_m,x_m,y_m,z_m,tx,ty,tz,lx,ly,lz,width_m,depth_m,asymmetry\n"
               << std::setprecision(9);
        for (const hydrology::RiverCentrelineSample& sample :
             request.input.geometry.centreline) {
            stream << sample.distance_m << ',' << sample.position_m.x << ','
                   << sample.position_m.y << ',' << sample.position_m.z << ','
                   << sample.tangent.x << ',' << sample.tangent.y << ','
                   << sample.tangent.z << ',' << sample.lateral.x << ','
                   << sample.lateral.y << ',' << sample.lateral.z << ','
                   << sample.width_m << ',' << sample.depth_m << ','
                   << sample.asymmetry << '\n';
        }
        if (!stream) {
            error = "could not write centreline trace CSV";
            return false;
        }
    }
    {
        std::ofstream stream(root / "metadata.txt",
                             std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "could not open particle trace metadata";
            return false;
        }
        stream << std::boolalpha << std::setprecision(9)
               << "accepted=" << accepted << '\n'
               << "terminal_code=" << static_cast<unsigned>(terminal_code)
               << "\nsteps=" << output.stats.simulated_steps
               << "\nparticles=" << output.particles.size()
               << "\nemitted=" << output.stats.emitted_particles
               << "\nescaped=" << output.stats.escaped_particles
               << "\nescape_budget=" << output.stats.escape_budget
               << "\nwall_seconds=" << output.stats.wall_seconds
               << "\nparticle_spacing_m="
               << request.input.settings.particle_spacing_m
               << "\nproduct_particle_radius_m="
               << request.product_settings.particle_radius_m
               << "\nsensor_max=" << output.sensor.maximum_wet_fraction
               << "\nsensor_final=" << output.sensor.final_wet_fraction
               << "\nvisual_vertices=" << visual.positions.size() / 3u
               << "\nvisual_triangles=" << visual.indices.size() / 3u
               << '\n';
        if (!stream) {
            error = "could not write particle trace metadata";
            return false;
        }
    }
    if (!write_obj(root / "visual.obj", visual.positions, visual.indices,
                   error) ||
        !write_obj(root / "collision.obj", request.input.collision, error))
        return false;

    std::fprintf(stderr, "[hydrology] particle trace written to %s\n",
                 root.string().c_str());
    error.clear();
    return true;
}

bool write_network_timing_trace(
    const hydrology::HydrologyNetworkBakeResult& result,
    std::string& error) {
    const char* configured = std::getenv("MATTER_HYDROLOGY_TRACE_DIR");
    if (!configured || configured[0] == '\0') return true;
    const std::filesystem::path root(configured);
    if (!root.is_absolute()) {
        error = "MATTER_HYDROLOGY_TRACE_DIR must be an absolute path";
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if (filesystem_error) {
        error = "could not create network timing trace directory";
        return false;
    }
    std::ofstream stream(root / "timings.json",
                         std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "could not open network timing trace";
        return false;
    }
    stream << std::fixed << std::setprecision(3)
           << "{\n  \"networkState\": \"Ready\",\n  \"sections\": [\n";
    for (std::size_t index = 0u;
         index != result.timings.sections.size(); ++index) {
        const auto& timing = result.timings.sections[index];
        const auto section = std::find_if(
            result.sections.begin(), result.sections.end(),
            [&](const auto& value) {
                return value.section.section_id == timing.id;
            });
        const std::uint32_t particles = section == result.sections.end()
            ? 0u : section->stats.active_particles;
        const std::uint32_t escaped = section == result.sections.end()
            ? 0u : section->stats.escaped_particles;
        stream << "    {\"id\":" << std::quoted(timing.id)
               << ",\"cacheHit\":" << std::boolalpha << timing.cache_hit
               << ",\"setupMs\":" << timing.setup_ms
               << ",\"physxInitMs\":" << timing.physx_init_ms
               << ",\"simulateMs\":" << timing.simulate_ms
               << ",\"gpuMeshMs\":" << timing.gpu_mesh_ms
               << ",\"cpuMeshMs\":" << timing.cpu_mesh_ms
               << ",\"particles\":" << particles
               << ",\"escaped\":" << escaped << "}"
               << (index + 1u == result.timings.sections.size() ? "\n" : ",\n");
    }
    stream << "  ],\n  \"handoffMeshMs\":"
           << result.timings.handoff_mesh_ms
           << ",\n  \"serializeMs\":" << result.timings.serialize_ms
           << ",\n  \"totalWallMs\":" << result.timings.total_wall_ms
           << ",\n  \"visualVertices\":"
           << result.products.visual_mesh.positions.size() / 3u
           << ",\n  \"visualTriangles\":"
           << result.products.visual_mesh.indices.size() / 3u
           << "\n}\n";
    if (!stream) {
        error = "could not write network timing trace";
        return false;
    }
    std::fprintf(stderr, "[hydrology] network timing trace written to %s\n",
                 (root / "timings.json").string().c_str());
    error.clear();
    return true;
}

} // namespace

bool assemble_authored_fluid_section_request(
    const matter::RiverNetworkDefinition& network,
    const hydrology::RiverGeometry& geometry,
    const matter::RiverSectionDefinition& section,
    const std::vector<hydrology::SpillwayHandoffRecord>& upstream,
    const std::vector<hydrology::AuthoredFluidCollider>& colliders,
    const FluidBakeRunContext& context,
    const std::string& cache_root,
    FluidBakeRequest& request,
    hydrology::FluidBakeError& error) {
    request = {};
    error = {};
    const matter::RiverDefinition* river = section_river(network, section);
    if (!river || !section.terminal_spillway ||
        !section.terminal_pool || geometry.centreline.size() < 2u) {
        error = {hydrology::FluidBakeCode::InvalidInput,
                 "authored river section is incomplete"};
        return false;
    }
    if (upstream.size() > 1u) {
        error = {hydrology::FluidBakeCode::InvalidInput,
                 "tributary fan-in execution is outside the two-section milestone"};
        return false;
    }
    if (!upstream.empty() && !section.emitter_ids.empty()) {
        error = {hydrology::FluidBakeCode::InvalidInput,
                 "inherited river section cannot also name authored emitters"};
        return false;
    }
    request.section_id = section.id;
    request.river_id = section.river;
    request.from_m = section.from_m;
    request.to_m = section.to_m;
    request.visual_from_m = section.from_m -
        (upstream.empty() ? 0.0f : upstream.front().overlap_m * 0.5f);
    request.visual_to_m = section.to_m +
        section.terminal_spillway->overlap_m * 0.5f;
    for (const auto& handoff : upstream) {
        if (handoff.semantic_key == 0u ||
            handoff.downstream_section_id != section.id) {
            error = {hydrology::FluidBakeCode::InvalidInput,
                     "upstream spillway handoff does not target this section"};
            request = {};
            return false;
        }
        request.upstream_handoff_keys.push_back(handoff.semantic_key);
    }
    std::sort(request.upstream_handoff_keys.begin(),
              request.upstream_handoff_keys.end());
    request.input.network = network;
    request.input.geometry = geometry;
    const auto& fluid = network.fluid;
    const float spillway_distance = section.terminal_spillway->distance_m;
    const float dam_distance = spillway_distance +
                               section.terminal_spillway->dam_offset_m;
    const float collision_from_m = section.from_m -
        (upstream.empty() ? 0.0f : upstream.front().overlap_m);
    hydrology::FluidCollisionSurface terrain_surface{};
    matter::Aabb section_bounds{};
    if (!build_terrain_surface(request.input.geometry, fluid,
                               collision_from_m, dam_distance,
                               network.cell_size_m,
                               section.dry_margin_m,
                               context.terrain,
                               terrain_surface, section_bounds, error))
        return false;
    const auto dam_sample = sample_at_distance(
        request.input.geometry, dam_distance);
    float dam_bed = section_bounds.minimum.y;
    sample_terrain(context.terrain, dam_bed, dam_sample.position_m.x,
                   dam_sample.position_m.z, dam_bed);
    hydrology::FluidCollisionBuildInput collision_input{};
    collision_input.surfaces.push_back(std::move(terrain_surface));
    std::vector<hydrology::FluidCollisionSurface> authored_surfaces;
    std::uint64_t authored_collider_revision = 0;
    if (!hydrology::build_authored_fluid_collision_surfaces(
            colliders, section_bounds, authored_surfaces,
            authored_collider_revision, error))
        return false;
    collision_input.surfaces.insert(
        collision_input.surfaces.end(),
        std::make_move_iterator(authored_surfaces.begin()),
        std::make_move_iterator(authored_surfaces.end()));
    auto dam_surface = build_dam_surface(
        dam_sample, fluid.virtual_dam, dam_bed);
    request.temporary_dam_bounds_m = mesh_bounds(dam_surface.mesh);
    collision_input.surfaces.push_back(std::move(dam_surface));
    collision_input.section_bounds_m = section_bounds;
    collision_input.dry_margin_m = section.dry_margin_m;
    hydrology::FluidCollisionBuildOutput collision_output{};
    if (!hydrology::build_physx_collision_input(
            collision_input, collision_output, error))
        return false;
    request.input.collision = std::move(collision_output.mesh);
    request.input.dry_collar_bounds_m = collision_output.dry_collar_bounds_m;

    request.input.settings = {
        fluid.pbd.particle_spacing_m, fluid.pbd.rest_density_kg_m3,
        fluid.pbd.fixed_step_seconds, fluid.pbd.solver_iterations,
        fluid.pbd.max_neighbors, fluid.limits.batch_steps,
        fluid.limits.max_steps, fluid.limits.max_particles,
        fluid.limits.escape_policy};

    const auto convert_authored = [&](const matter::HydrologyEmitter& authored,
                                      hydrology::FluidEmitter& emitter) {
        const auto step = [&](float seconds) -> std::uint64_t {
            return static_cast<std::uint64_t>(std::llround(
                static_cast<double>(seconds) / fluid.pbd.fixed_step_seconds));
        };
        const std::uint64_t start = step(authored.start_time_s);
        const std::uint64_t stop = step(authored.stop_time_s);
        if (start >= stop || stop > fluid.limits.max_steps ||
            stop > std::numeric_limits<std::uint32_t>::max()) {
            error = {hydrology::FluidBakeCode::InvalidInput,
                     "authored emitter time range exceeds the bake step budget"};
            return false;
        }
        emitter = {};
        emitter.id = emitter_id(authored.id);
        emitter.shape = hydrology::FluidEmitterShape::Disc;
        emitter.position_m = authored.position_m;
        emitter.direction = authored.direction;
        emitter.initial_velocity_mps = authored.initial_velocity_mps;
        emitter.flow_m3s = authored.flow_m3s;
        emitter.radius_m = authored.radius_m;
        emitter.start_step = static_cast<std::uint32_t>(start);
        emitter.stop_step = static_cast<std::uint32_t>(stop);
        return true;
    };
    if (!upstream.empty()) {
        hydrology::FluidEmitter emitter{};
        if (!hydrology::make_spillway_emitter(
                upstream.front(), request.input.settings, emitter, error))
            return false;
        request.input.emitters.push_back(emitter);
    } else {
        if (section.emitter_ids.empty()) {
            error = {hydrology::FluidBakeCode::InvalidInput,
                     "initial river section requires authored emitter ids"};
            return false;
        }
        std::unordered_set<std::uint32_t> ids;
        for (const auto& authored_id : section.emitter_ids) {
            const auto found = std::find_if(
                fluid.emitters.begin(), fluid.emitters.end(),
                [&](const matter::HydrologyEmitter& emitter) {
                    return emitter.id == authored_id;
                });
            if (found == fluid.emitters.end()) {
                error = {hydrology::FluidBakeCode::InvalidInput,
                         "section names an unknown authored emitter"};
                return false;
            }
            hydrology::FluidEmitter emitter{};
            if (!convert_authored(*found, emitter)) return false;
            if (!ids.insert(emitter.id).second) {
                error = {hydrology::FluidBakeCode::InvalidInput,
                         "authored emitter stable ids collide after canonical hashing"};
                return false;
            }
            request.input.emitters.push_back(emitter);
        }
    }

    const float sensor_distance = spillway_distance -
        fluid.fill_sensor.upstream_offset_m - fluid.fill_sensor.length_m * 0.5f;
    const auto sensor_sample = sample_at_distance(
        request.input.geometry, std::max(0.0f, sensor_distance));
    float tx = sensor_sample.tangent.x;
    float tz = sensor_sample.tangent.z;
    const float tangent_length = std::hypot(tx, tz);
    if (tangent_length > 1.0e-6f) { tx /= tangent_length; tz /= tangent_length; }
    else { tx = 1.0f; tz = 0.0f; }
    const float lx = -tz;
    const float lz = tx;
    const float half_length = fluid.fill_sensor.length_m * 0.5f;
    const float half_width = section.terminal_spillway->width_m * 0.5f;
    float sensor_min_x = std::numeric_limits<float>::infinity();
    float sensor_min_z = std::numeric_limits<float>::infinity();
    float sensor_max_x = -std::numeric_limits<float>::infinity();
    float sensor_max_z = -std::numeric_limits<float>::infinity();
    for (const float along : {-half_length, half_length}) {
        for (const float across : {-half_width, half_width}) {
            const float x = sensor_sample.position_m.x + tx * along + lx * across;
            const float z = sensor_sample.position_m.z + tz * along + lz * across;
            sensor_min_x = std::min(sensor_min_x, x);
            sensor_min_z = std::min(sensor_min_z, z);
            sensor_max_x = std::max(sensor_max_x, x);
            sensor_max_z = std::max(sensor_max_z, z);
        }
    }
    const float sensor_top = section.terminal_pool->fill_level_m;
    const float sensor_bottom = sensor_top - fluid.fill_sensor.height_m;
    request.input.sensor.bounds_m = {
        {sensor_min_x, sensor_bottom, sensor_min_z},
        {sensor_max_x, sensor_top, sensor_max_z}};
    request.input.sensor.resolution = {
        fluid.fill_sensor.resolution_x, fluid.fill_sensor.resolution_y,
        fluid.fill_sensor.resolution_z};
    request.input.sensor.required_wet_fraction =
        fluid.fill_sensor.crest_wet_fraction;
    request.input.sensor.stable_steps =
        fluid.fill_sensor.stable_wet_steps;
    request.input.sensor.minimum_particles_per_cell =
        fluid.fill_sensor.minimum_particles_per_cell;
    request.input.sensor.frame_origin_m = {
        sensor_sample.position_m.x - tx * half_length - lx * half_width,
        sensor_bottom,
        sensor_sample.position_m.z - tz * half_length - lz * half_width};
    request.input.sensor.longitudinal_axis_xz = {tx, tz};
    request.input.sensor.lateral_axis_xz = {lx, lz};
    request.input.sensor.frame_extent_m = {
        fluid.fill_sensor.length_m, fluid.fill_sensor.height_m,
        half_width * 2.0f};

    const auto& quality = fluid.quality;
    auto& products = request.product_settings;
    products.section = {request.section_id, request.river_id,
                        request.from_m, request.to_m,
                        request.visual_from_m, request.visual_to_m};
    products.particle_radius_m = quality.particle_radius_m;
    products.coarse_voxel_m = quality.coarse_voxel_m;
    products.visual_job.bounds_m = {
        request.input.dry_collar_bounds_m.minimum,
        request.input.dry_collar_bounds_m.maximum};
    products.visual_job.voxel_m = quality.visual_voxel_m;
    products.visual_job.blend_width_m = quality.visual_blend_width_m;
    products.visual_job.iso_value = 0.0f;
    products.visual_job.material = 4u;
    products.visual_job.limits = {
        quality.max_visual_particles, quality.max_grid_vertices,
        quality.max_mesh_vertices, quality.max_mesh_indices};
    const float gameplay_cell = quality.gameplay_cell_m;
    const auto gameplay_dimension = [gameplay_cell](float extent) {
        return static_cast<std::uint32_t>(
            std::max(1.0f, std::ceil(extent / gameplay_cell)));
    };
    products.gameplay_layout = {
        request.input.dry_collar_bounds_m.minimum, gameplay_cell,
        gameplay_dimension(request.input.dry_collar_bounds_m.maximum.x -
                           request.input.dry_collar_bounds_m.minimum.x),
        gameplay_dimension(request.input.dry_collar_bounds_m.maximum.z -
                           request.input.dry_collar_bounds_m.minimum.z)};

    const std::uint64_t terrain_revision = context.terrain_revision != 0u
        ? context.terrain_revision : request.input.geometry.revision;
    const std::uint64_t dam_key = dam_revision(
        fluid.virtual_dam, dam_distance, request.temporary_dam_bounds_m);
    const std::uint64_t section_key = section_revision(
        section, request.upstream_handoff_keys);
    std::uint64_t collision_revision = collision_mesh_revision(
        request.input.collision);
    hash_value(collision_revision, request.input.geometry.revision);
    hash_value(collision_revision, terrain_revision);
    hash_value(collision_revision, authored_collider_revision);
    hash_value(collision_revision, dam_key);
    hash_value(collision_revision, section_key);
    products.semantic = {
        kPhysxSdkVersion, kFluidAdapterVersion,
        pbd_revision(fluid, request.input.emitters),
        nonzero_hash(collision_revision),
        // Keep section artifacts independently reusable. The collision key
        // already carries the sampled river geometry/terrain and section
        // ownership; hashing the complete network here would invalidate an
        // accepted upstream section for a downstream-only pool edit.
        section_key,
        terrain_revision, dam_key,
        sensor_revision(fluid.fill_sensor, request.input.sensor),
        kFluidMesherContractVersion};
    request.semantic_key = hydrology::derive_hydrology_semantic_key(
        products.semantic);
    products.identity.semantic_key = request.semantic_key;
    products.provenance = {kNvidiaVendorId, 1u, 1u,
                           kPhysxSdkVersion, kFluidAdapterVersion};
    request.terrain = context.terrain;
    if (!request.terrain) {
        const float fallback = section_bounds.minimum.y;
        request.terrain = [fallback](float, float, float& height) {
            height = fallback;
            return true;
        };
    }
    request.cache_path = std::filesystem::path(cache_root) / "hydrology" /
                         "sections" /
                         (safe_cache_id(section.id) + "-" +
                          hex64(request.semantic_key) + ".mhyd");
    return true;
}

namespace {

bool cache_matches_request(
    const hydrology::HydrologyArtifact& artifact,
    const FluidBakeRequest& request) {
    if (!artifact.accepted || artifact.semantic_key != request.semantic_key)
        return false;
    if (artifact.section.section_id != request.section_id ||
        artifact.section.river_id != request.river_id ||
        artifact.section.from_m != request.from_m ||
        artifact.section.to_m != request.to_m ||
        artifact.section.visual_from_m != request.visual_from_m ||
        artifact.section.visual_to_m != request.visual_to_m)
        return false;
    const auto& products = request.product_settings;
    const std::uint64_t snapshot = hydrology::fluid_particle_snapshot_digest(
        artifact.particles, products.particle_radius_m);
    hydrology::ProductIdentitySettings identity = products.identity;
    identity.semantic_key = request.semantic_key;
    const gpu_meshing::ParticleJob visual_job =
        hydrology::PhysxFluidBake::resolved_visual_job(
            artifact.particles, products.particle_radius_m,
            products.visual_job);
    const hydrology::ProductKeys expected = hydrology::derive_product_keys(
        visual_job, snapshot, identity, products.coarse_voxel_m,
        products.gameplay_layout);
    return snapshot == artifact.particle_snapshot_digest &&
           artifact.product_keys == expected &&
           artifact.particles.size() <= products.visual_job.limits.max_particles &&
           artifact.visual_mesh.positions.size() / 3u <=
               products.visual_job.limits.max_mesh_vertices &&
           artifact.visual_mesh.indices.size() <=
               products.visual_job.limits.max_mesh_indices;
}

bool load_semantic_cache(const FluidBakeRequest& request,
                         hydrology::HydrologyArtifact& artifact) {
    artifact = {};
    std::error_code filesystem_error;
    const std::uintmax_t size = std::filesystem::file_size(
        request.cache_path, filesystem_error);
    constexpr std::uintmax_t kMaximumBytes = 513ull * 1024ull * 1024ull;
    if (filesystem_error || size == 0u || size > kMaximumBytes) return false;
    std::ifstream stream(request.cache_path, std::ios::binary);
    if (!stream) return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!stream) return false;
    gpu_meshing::Error load_error{};
    return hydrology::deserialize_artifact(bytes, artifact, load_error) &&
           cache_matches_request(artifact, request);
}

bool load_water_animation_cache(
    const std::filesystem::path& path,
    const std::string& section_id,
    std::uint64_t semantic_key,
    std::uint64_t source_payload_digest,
    const matter::HydrologyMeshAnimationProfile& profile,
    hydrology::WaterMeshAnimationArtifact& artifact) {
    artifact = {};
    gpu_meshing::Error error{};
    hydrology::WaterMeshAnimationArtifact candidate{};
    if (!hydrology::load_water_mesh_animation_artifact(
            path, candidate, error))
        return false;
    if (candidate.identity != section_id ||
        candidate.semantic_key != semantic_key ||
        candidate.source_primary_payload_digest != source_payload_digest ||
        candidate.source_secondary_payload_digest != 0u ||
        candidate.frames_per_second != profile.frames_per_second ||
        candidate.frames.size() != profile.frame_count ||
        candidate.phase_offset_frames != profile.phase_offset_frames ||
        candidate.duration_seconds != profile.duration_seconds ||
        candidate.payload_digest == 0u)
        return false;
    artifact = std::move(candidate);
    return true;
}

} // namespace

LocalProviderConfig make_engine_local_provider_config(
    const std::string& project_dir, const std::string& world_name,
    const std::string& engine_shared_lib_dir,
    FluidBakeBackendFactory backend_factory) {
    LocalProviderConfig config = LocalProviderConfig::for_project(
        project_dir, world_name, engine_shared_lib_dir);
    config.fluid_bake_backend_factory = std::move(backend_factory);
    return config;
}

LocalProvider::LocalProvider(LocalProviderConfig cfg) : cfg_(std::move(cfg)) {}

bool LocalProvider::build_accepted_fluid_artifact(
    const hydrology::FluidBakeOutput& output,
    const hydrology::PhysxFluidBake::ProductBuildSettings& settings,
    const hydrology::TerrainHeightSampler& terrain,
    hydrology::HydrologyArtifact& artifact,
    hydrology::FluidBakeError& error,
    hydrology::PhysxFluidBake::ProductBuildTimings* timings) const {
    return hydrology::PhysxFluidBake::build_accepted_artifact_on_renderer(
        output, settings, terrain, cfg_.gpu_run, cfg_.vk_particle_visual_bake,
        artifact, error, timings);
}

bool LocalProvider::authored_fluid_requested() const {
    return river_network_ &&
           river_network_->fluid.backend == matter::HydrologyBackend::Physx;
}

bool LocalProvider::run_authored_fluid_bake(
    const FluidBakeRunContext& context,
    matter::HydrologyStatus& status,
    hydrology::FluidBakeError& fluid_error,
    hydrology::HydrologyArtifact& artifact,
    gpu_meshing::MeshResult& failed_debug_visual) {
    accepted_fluid_artifact_.reset();
    artifact = {};
    failed_debug_visual = {};
    fluid_error = {};
    status = {};
    if (!authored_fluid_requested()) return false;

    FluidBakeRequest request{};
    hydrology::HydrologyArtifact candidate{};
    status.state = matter::HydrologyState::Baking;
    try {
        const matter::RiverSectionDefinition* section =
            initial_section(*river_network_);
        hydrology::RiverGeometry geometry{};
        std::string geometry_error;
        if (!section || !hydrology::build_river_geometry(
                *river_network_, section->river, geometry, geometry_error)) {
            fluid_error = {hydrology::FluidBakeCode::InvalidInput,
                           section ? geometry_error
                                   : "river network has no initial section"};
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        if (!assemble_authored_fluid_section_request(
                *river_network_, geometry, *section, {},
                authored_fluid_colliders_, context, abs_cache_root_, request,
                fluid_error)) {
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        status.input_key = hex64(request.semantic_key);
        if (context.callbacks.cancelled && context.callbacks.cancelled()) {
            fluid_error = {hydrology::FluidBakeCode::Cancelled,
                           "authored fluid bake was superseded before cache lookup"};
            status.state = matter::HydrologyState::Stale;
            status.failure_reason = fluid_error.message;
            return false;
        }
        if (load_semantic_cache(request, candidate)) {
            if (context.callbacks.cancelled && context.callbacks.cancelled()) {
                fluid_error = {hydrology::FluidBakeCode::Cancelled,
                               "authored fluid cache hit was superseded before publication"};
                status.state = matter::HydrologyState::Stale;
                status.failure_reason = fluid_error.message;
                return false;
            }
            status.state = matter::HydrologyState::Ready;
            status.cache_hit = true;
            status.progress = 1.0f;
            status.completed_steps = candidate.stats.simulated_steps;
            status.wet_cells = static_cast<std::uint32_t>(std::count_if(
                candidate.gameplay_field.begin(), candidate.gameplay_field.end(),
                [](const hydrology::GameplaySample& sample) {
                    return sample.wet_valid;
                }));
            status.mesh_triangles = static_cast<std::uint32_t>(
                candidate.visual_mesh.indices.size() / 3u);
            status.simulated_time_s =
                static_cast<double>(candidate.stats.simulated_steps) *
                request.input.settings.fixed_step_seconds;
            status.payload_digest = hex64(candidate.payload_digest);
            std::fprintf(
                stderr,
                "[hydrology] accepted cache_hit=true steps=%u active=%u peak=%u escaped=%u emitted=%u budget=%u sensor_max=%.6f sensor_final=%.6f sensor_stable=%.6f visual_vertices=%zu visual_triangles=%zu gameplay_cells=%zu payload=%s\n",
                candidate.stats.simulated_steps,
                candidate.stats.active_particles,
                candidate.stats.peak_particles,
                candidate.stats.escaped_particles,
                candidate.stats.emitted_particles,
                candidate.stats.escape_budget,
                static_cast<double>(candidate.sensor.maximum_wet_fraction),
                static_cast<double>(candidate.sensor.final_wet_fraction),
                static_cast<double>(
                    candidate.sensor.stable_window_wet_fraction),
                candidate.visual_mesh.positions.size() / 3u,
                candidate.visual_mesh.indices.size() / 3u,
                candidate.gameplay_field.size(),
                status.payload_digest.c_str());
            artifact = std::move(candidate);
            return true;
        }

        if (!cfg_.fluid_renderer_device.luid_valid) {
            fluid_error = {
                hydrology::FluidBakeCode::BackendUnavailable,
                "Vulkan render adapter identity is unavailable for the PhysX fluid bake"};
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }

        if (!cfg_.fluid_bake_backend_factory) {
            fluid_error = {hydrology::FluidBakeCode::BackendUnavailable,
                           "PhysX fluid support is disabled in this build"};
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        std::shared_ptr<hydrology::IFluidBakeBackend> backend =
            cfg_.fluid_bake_backend_factory();
        if (!backend) {
            fluid_error = {hydrology::FluidBakeCode::BackendUnavailable,
                           "PhysX fluid backend factory returned no runtime"};
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        const hydrology::FluidBackendProbe probe = backend->probe();
        if (!probe.available) {
            fluid_error = {probe.code, probe.message.empty()
                ? "PhysX fluid backend probe failed" : probe.message};
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        if (!probe.device_luid_valid) {
            fluid_error = {
                hydrology::FluidBakeCode::BackendUnavailable,
                "CUDA device identity is unavailable for the Vulkan render adapter comparison"};
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        if (probe.device_luid != cfg_.fluid_renderer_device.luid) {
            fluid_error = {
                hydrology::FluidBakeCode::BackendUnavailable,
                "CUDA device identity does not match the Vulkan render adapter"};
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        request.product_settings.provenance = {
            cfg_.fluid_renderer_device.vendor_id != 0u
                ? cfg_.fluid_renderer_device.vendor_id : kNvidiaVendorId,
            cfg_.fluid_renderer_device.device_id != 0u
                ? cfg_.fluid_renderer_device.device_id : 1u,
            probe.cuda_driver_version > 0
                ? static_cast<std::uint32_t>(probe.cuda_driver_version)
                : (cfg_.fluid_renderer_device.driver_version != 0u
                       ? cfg_.fluid_renderer_device.driver_version : 1u),
            probe.sdk_version_hex != 0u ? probe.sdk_version_hex
                                        : kPhysxSdkVersion,
            kFluidAdapterVersion};

        hydrology::FluidBakeOutput output{};
        const bool simulation_accepted = hydrology::PhysxFluidBake::run(
            request.input, *backend, context.callbacks, output, fluid_error);
        // The finite diagnostic snapshot is deliberately consumed only after
        // the backend has released every PhysX/CUDA scene resource.
        backend.reset();
        if (!simulation_accepted) {
            status.state = fluid_error.code == hydrology::FluidBakeCode::Cancelled
                ? matter::HydrologyState::Stale
                : matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            if (!output.particles.empty()) {
                matter::Float3 particle_min = output.particles.front().position_m;
                matter::Float3 particle_max = particle_min;
                std::uint32_t final_sensor_particles = 0u;
                for (const hydrology::FluidParticle& particle :
                     output.particles) {
                    particle_min.x = std::min(particle_min.x,
                                              particle.position_m.x);
                    particle_min.y = std::min(particle_min.y,
                                              particle.position_m.y);
                    particle_min.z = std::min(particle_min.z,
                                              particle.position_m.z);
                    particle_max.x = std::max(particle_max.x,
                                              particle.position_m.x);
                    particle_max.y = std::max(particle_max.y,
                                              particle.position_m.y);
                    particle_max.z = std::max(particle_max.z,
                                              particle.position_m.z);
                    const matter::Float3 local =
                        hydrology::fluid_fill_sensor_local_position(
                            request.input.sensor, particle.position_m);
                    if (local.x >= 0.0f &&
                        local.x <= request.input.sensor.frame_extent_m.x &&
                        local.y >= 0.0f &&
                        local.y <= request.input.sensor.frame_extent_m.y &&
                        local.z >= 0.0f &&
                        local.z <= request.input.sensor.frame_extent_m.z)
                        ++final_sensor_particles;
                }
                std::fprintf(
                    stderr,
                    "[hydrology] finite failed snapshot code=%u steps=%u active=%u peak=%u escaped=%u emitted=%u budget=%u sensor_max=%.6f sensor_final=%.6f wall_seconds=%.6f particle_bounds=[%.6f,%.6f,%.6f]-[%.6f,%.6f,%.6f] sensor_bounds=[%.6f,%.6f,%.6f]-[%.6f,%.6f,%.6f] sensor_particles=%u\n",
                    static_cast<unsigned>(fluid_error.code),
                    output.stats.simulated_steps,
                    output.stats.active_particles,
                    output.stats.peak_particles,
                    output.stats.escaped_particles,
                    output.stats.emitted_particles,
                    output.stats.escape_budget,
                    static_cast<double>(
                        output.sensor.maximum_wet_fraction),
                    static_cast<double>(output.sensor.final_wet_fraction),
                    output.stats.wall_seconds,
                    static_cast<double>(particle_min.x),
                    static_cast<double>(particle_min.y),
                    static_cast<double>(particle_min.z),
                    static_cast<double>(particle_max.x),
                    static_cast<double>(particle_max.y),
                    static_cast<double>(particle_max.z),
                    static_cast<double>(request.input.sensor.bounds_m.minimum.x),
                    static_cast<double>(request.input.sensor.bounds_m.minimum.y),
                    static_cast<double>(request.input.sensor.bounds_m.minimum.z),
                    static_cast<double>(request.input.sensor.bounds_m.maximum.x),
                    static_cast<double>(request.input.sensor.bounds_m.maximum.y),
                    static_cast<double>(request.input.sensor.bounds_m.maximum.z),
                    final_sensor_particles);
            }
            if (fluid_error.code != hydrology::FluidBakeCode::Cancelled &&
                (!context.callbacks.cancelled ||
                 !context.callbacks.cancelled()) &&
                (fluid_error.code ==
                     hydrology::FluidBakeCode::SensorNotReached ||
                 fluid_error.code == hydrology::FluidBakeCode::Escaped)) {
                hydrology::FluidBakeError debug_error{};
                if (!hydrology::PhysxFluidBake::
                        build_failed_debug_visual_on_renderer(
                            output, fluid_error.code,
                            request.product_settings, cfg_.gpu_run,
                            cfg_.vk_particle_visual_bake,
                            failed_debug_visual, debug_error)) {
                    std::fprintf(
                        stderr,
                        "[hydrology] UNACCEPTED DEBUG WATER visual rejected: code=%u message=%s\n",
                        static_cast<unsigned>(debug_error.code),
                        debug_error.message.c_str());
                    failed_debug_visual = {};
                }
            }
            std::string trace_error;
            if (!write_particle_trace(
                    request, output, fluid_error.code, failed_debug_visual,
                    false,
                    trace_error)) {
                std::fprintf(stderr,
                             "[hydrology] particle trace rejected: %s\n",
                             trace_error.c_str());
            }
            return false;
        }
        if (context.callbacks.cancelled && context.callbacks.cancelled()) {
            fluid_error = {hydrology::FluidBakeCode::Cancelled,
                           "authored fluid bake was superseded before visual meshing"};
            status.state = matter::HydrologyState::Stale;
            status.failure_reason = fluid_error.message;
            return false;
        }
        if (!build_accepted_fluid_artifact(
                output, request.product_settings, request.terrain, candidate,
                fluid_error)) {
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        std::string trace_error;
        if (!write_particle_trace(
                request, output, hydrology::FluidBakeCode::Ready,
                candidate.visual_mesh, true, trace_error)) {
            std::fprintf(stderr,
                         "[hydrology] particle trace rejected: %s\n",
                         trace_error.c_str());
        }
        if (context.callbacks.cancelled && context.callbacks.cancelled()) {
            fluid_error = {hydrology::FluidBakeCode::Cancelled,
                           "authored fluid bake was superseded before artifact save"};
            status.state = matter::HydrologyState::Stale;
            status.failure_reason = fluid_error.message;
            return false;
        }
        gpu_meshing::Error artifact_error{};
        if (!hydrology::save_artifact_atomic(
                request.cache_path, candidate, artifact_error)) {
            fluid_error = {hydrology::FluidBakeCode::ProductFailure,
                           artifact_error.message};
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        hydrology::HydrologyArtifact validated{};
        if (!hydrology::load_artifact_validated(
                request.cache_path, candidate.product_keys.visual, validated,
                artifact_error, request.semantic_key)) {
            fluid_error = {hydrology::FluidBakeCode::ProductFailure,
                           artifact_error.message};
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        if (context.callbacks.cancelled && context.callbacks.cancelled()) {
            fluid_error = {hydrology::FluidBakeCode::Cancelled,
                           "authored fluid bake was superseded before publication"};
            status.state = matter::HydrologyState::Stale;
            status.failure_reason = fluid_error.message;
            return false;
        }
        status.state = matter::HydrologyState::Ready;
        status.cache_hit = false;
        status.progress = 1.0f;
        status.completed_steps = validated.stats.simulated_steps;
        status.wet_cells = static_cast<std::uint32_t>(std::count_if(
            validated.gameplay_field.begin(), validated.gameplay_field.end(),
            [](const hydrology::GameplaySample& sample) {
                return sample.wet_valid;
            }));
        status.mesh_triangles = static_cast<std::uint32_t>(
            validated.visual_mesh.indices.size() / 3u);
        status.simulated_time_s =
            static_cast<double>(validated.stats.simulated_steps) *
            request.input.settings.fixed_step_seconds;
        status.payload_digest = hex64(validated.payload_digest);
        std::fprintf(
            stderr,
            "[hydrology] accepted cache_hit=false steps=%u active=%u peak=%u escaped=%u emitted=%u budget=%u wall_seconds=%.6f sensor_max=%.6f sensor_final=%.6f sensor_stable=%.6f visual_vertices=%zu visual_triangles=%zu coarse_vertices=%zu coarse_triangles=%zu gameplay_cells=%zu payload=%s\n",
            validated.stats.simulated_steps,
            validated.stats.active_particles,
            validated.stats.peak_particles,
            validated.stats.escaped_particles,
            validated.stats.emitted_particles,
            validated.stats.escape_budget,
            validated.stats.wall_seconds,
            static_cast<double>(validated.sensor.maximum_wet_fraction),
            static_cast<double>(validated.sensor.final_wet_fraction),
            static_cast<double>(
                validated.sensor.stable_window_wet_fraction),
            validated.visual_mesh.positions.size() / 3u,
            validated.visual_mesh.indices.size() / 3u,
            validated.coarse_cpu_mesh.positions.size() / 3u,
            validated.coarse_cpu_mesh.indices.size() / 3u,
            validated.gameplay_field.size(),
            status.payload_digest.c_str());
        artifact = std::move(validated);
        return true;
    } catch (const std::exception& exception) {
        fluid_error = {hydrology::FluidBakeCode::BackendFailure,
                       exception.what()};
    } catch (...) {
        fluid_error = {hydrology::FluidBakeCode::BackendFailure,
                       "fluid request assembly raised an unknown exception"};
    }
    status.state = matter::HydrologyState::Invalid;
    status.failure_reason = fluid_error.message;
    return false;
}

bool LocalProvider::run_authored_fluid_bake(
    const FluidBakeRunContext& context,
    matter::HydrologyStatus& status,
    hydrology::FluidBakeError& fluid_error,
    hydrology::HydrologyNetworkBakeResult& network_result) {
    accepted_fluid_network_.reset();
    network_result = {};
    fluid_error = {};
    status = {};
    if (!authored_fluid_requested()) return false;
    const auto network_wall_start = std::chrono::steady_clock::now();
    status.state = matter::HydrologyState::Baking;
    status.total_sections = static_cast<std::uint32_t>(
        river_network_->sections.size());

    std::vector<hydrology::RiverGeometry> geometries;
    geometries.reserve(river_network_->rivers.size());
    std::unordered_map<std::string, std::size_t> geometry_by_river;
    std::string graph_error;
    for (std::size_t index = 0u; index < river_network_->rivers.size(); ++index) {
        hydrology::RiverGeometry geometry{};
        if (!hydrology::build_river_geometry(
                *river_network_, river_network_->rivers[index].name,
                geometry, graph_error)) {
            fluid_error = {hydrology::FluidBakeCode::InvalidInput, graph_error};
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        geometry_by_river.emplace(river_network_->rivers[index].name, index);
        geometries.push_back(std::move(geometry));
    }
    hydrology::RiverSectionGraph graph{};
    if (!hydrology::build_river_section_graph(
            *river_network_, geometries, graph, graph_error)) {
        fluid_error = {hydrology::FluidBakeCode::InvalidInput, graph_error};
        status.state = matter::HydrologyState::Invalid;
        status.failure_reason = fluid_error.message;
        return false;
    }

    const auto combined_bounds = [&]() {
        matter::Aabb bounds = geometries.front().bounds_m;
        for (const auto& geometry : geometries) {
            bounds.minimum.x = std::min(bounds.minimum.x,
                                        geometry.bounds_m.minimum.x);
            bounds.minimum.y = std::min(bounds.minimum.y,
                                        geometry.bounds_m.minimum.y);
            bounds.minimum.z = std::min(bounds.minimum.z,
                                        geometry.bounds_m.minimum.z);
            bounds.maximum.x = std::max(bounds.maximum.x,
                                        geometry.bounds_m.maximum.x);
            bounds.maximum.y = std::max(bounds.maximum.y,
                                        geometry.bounds_m.maximum.y);
            bounds.maximum.z = std::max(bounds.maximum.z,
                                        geometry.bounds_m.maximum.z);
        }
        return bounds;
    }();
    const std::uint64_t terrain_revision = context.terrain_revision != 0u
        ? context.terrain_revision : geometries.front().revision;

    const auto finalize_manifest = [&](hydrology::HydrologyNetworkBakeResult& out) {
        hydrology::HydrologyNetworkArtifact manifest{};
        manifest.state = hydrology::HydrologyNetworkState::Ready;
        manifest.network_key = river_network_->canonical_hash;
        manifest.terrain_revision = terrain_revision;
        manifest.bounds_m = combined_bounds;
        manifest.runtime_field_digest =
            hydrology::hydrology_runtime_field_digest(
                out.products.gameplay_layout,
                out.products.gameplay_field);
        manifest.presentation_field_digest =
            hydrology::hydrology_presentation_field_digest(
                out.products.gameplay_layout,
                out.products.presentation_field);
        if (manifest.runtime_field_digest == 0u ||
            manifest.presentation_field_digest == 0u) {
            fluid_error = {hydrology::FluidBakeCode::ProductFailure,
                           "ready network contains an invalid field product"};
            return false;
        }
        manifest.field_products = {
            {hydrology::HydrologyFieldProductKind::Runtime,
             hydrology::hydrology_field_product_relative_path(
                 hydrology::HydrologyFieldProductKind::Runtime,
                 manifest.runtime_field_digest),
             manifest.runtime_field_digest},
            {hydrology::HydrologyFieldProductKind::Presentation,
             hydrology::hydrology_field_product_relative_path(
                 hydrology::HydrologyFieldProductKind::Presentation,
                 manifest.presentation_field_digest),
             manifest.presentation_field_digest},
        };
        for (const auto index : graph.topological_order) {
            const auto& section = river_network_->sections[index];
            const auto found = std::find_if(
                out.sections.begin(), out.sections.end(),
                [&](const auto& artifact) {
                    return artifact.section.section_id == section.id;
                });
            if (found == out.sections.end()) {
                fluid_error = {hydrology::FluidBakeCode::ProductFailure,
                               "ready network is missing a section artifact"};
                return false;
            }
            manifest.topological_order.push_back(section.id);
            manifest.sections.push_back({
                section.id,
                "hydrology/sections/" + safe_cache_id(section.id) + "-" +
                    hex64(found->semantic_key) + ".mhyd",
                section.after_section_ids,
                found->semantic_key,
                found->payload_digest});
            if (river_network_->fluid.mesh_animation.enabled) {
                const auto animation = std::find_if(
                    out.section_animations.begin(),
                    out.section_animations.end(),
                    [&](const auto& value) {
                        return value.identity == section.id;
                    });
                if (animation == out.section_animations.end() ||
                    animation->source_primary_payload_digest !=
                        found->payload_digest) {
                    fluid_error = {
                        hydrology::FluidBakeCode::ProductFailure,
                        "ready network is missing a matching section animation"};
                    return false;
                }
                manifest.section_animations.push_back({
                    section.id,
                    "hydrology/animations/" + safe_cache_id(section.id) +
                        "-" + hex64(animation->semantic_key) + ".mhwa",
                    animation->semantic_key,
                    animation->source_primary_payload_digest,
                    0u,
                    static_cast<std::uint32_t>(animation->frames.size()),
                    animation->frames_per_second,
                    animation->payload_digest});
            }
        }
        for (const auto& handoff : out.handoffs) {
            manifest.handoffs.push_back({
                handoff.id,
                "hydrology/handoffs/" + safe_cache_id(handoff.id) + "-" +
                    hex64(handoff.semantic_key) + ".mhyd",
                {handoff.handoff.upstream_section_id,
                 handoff.handoff.downstream_section_id},
                handoff.semantic_key,
                handoff.payload_digest});
            if (river_network_->fluid.mesh_animation.enabled) {
                const auto animation = std::find_if(
                    out.handoff_animations.begin(),
                    out.handoff_animations.end(),
                    [&](const auto& value) {
                        return value.identity == handoff.id;
                    });
                if (animation == out.handoff_animations.end() ||
                    animation->source_primary_payload_digest !=
                        handoff.upstream_payload_digest ||
                    animation->source_secondary_payload_digest !=
                        handoff.downstream_payload_digest) {
                    fluid_error = {
                        hydrology::FluidBakeCode::ProductFailure,
                        "ready network is missing a matching handoff animation"};
                    return false;
                }
                manifest.handoff_animations.push_back({
                    handoff.id,
                    "hydrology/animations/handoffs/" +
                        safe_cache_id(handoff.id) + "-" +
                        hex64(animation->semantic_key) + ".mhwa",
                    animation->semantic_key,
                    animation->source_primary_payload_digest,
                    animation->source_secondary_payload_digest,
                    static_cast<std::uint32_t>(animation->frames.size()),
                    animation->frames_per_second,
                    animation->payload_digest});
            }
        }
        if (context.callbacks.cancelled && context.callbacks.cancelled()) {
            fluid_error = {hydrology::FluidBakeCode::Cancelled,
                           "authored fluid network was superseded before manifest publication"};
            return false;
        }
        const auto manifest_path = std::filesystem::path(abs_cache_root_) /
            "hydrology" /
            ("network-" + hex64(manifest.network_key) + "-" +
             hex64(manifest.terrain_revision) + ".mhyn");
        gpu_meshing::Error artifact_error{};
        const auto serialize_start = std::chrono::steady_clock::now();
        hydrology::HydrologyFieldProduct runtime_product{};
        runtime_product.kind =
            hydrology::HydrologyFieldProductKind::Runtime;
        runtime_product.layout = out.products.gameplay_layout;
        runtime_product.gameplay = out.products.gameplay_field;
        runtime_product.payload_digest = manifest.runtime_field_digest;
        hydrology::HydrologyFieldProduct presentation_product{};
        presentation_product.kind =
            hydrology::HydrologyFieldProductKind::Presentation;
        presentation_product.layout = out.products.gameplay_layout;
        presentation_product.presentation = out.products.presentation_field;
        presentation_product.payload_digest =
            manifest.presentation_field_digest;
        const auto cache_root = std::filesystem::path(abs_cache_root_);
        if (!hydrology::save_hydrology_field_product_atomic(
                cache_root / manifest.field_products[0].relative_path,
                runtime_product, artifact_error) ||
            !hydrology::save_hydrology_field_product_atomic(
                cache_root / manifest.field_products[1].relative_path,
                presentation_product, artifact_error)) {
            fluid_error = {hydrology::FluidBakeCode::ProductFailure,
                           artifact_error.message};
            return false;
        }
        if (context.callbacks.cancelled && context.callbacks.cancelled()) {
            fluid_error = {hydrology::FluidBakeCode::Cancelled,
                           "authored fluid network was superseded before manifest publication"};
            return false;
        }
        if (!hydrology::save_network_artifact_atomic(
                manifest_path, manifest, artifact_error) ||
            !hydrology::load_network_artifact_validated(
                manifest_path, manifest.network_key,
                manifest.terrain_revision, out.manifest, artifact_error)) {
            fluid_error = {hydrology::FluidBakeCode::ProductFailure,
                           artifact_error.message};
            return false;
        }
        out.timings.serialize_ms +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - serialize_start).count();
        return true;
    };

    // Preserve the established one-section lifecycle and cache behavior for
    // existing worlds while publishing it through the network contract.
    if (river_network_->sections.size() == 1u &&
        !river_network_->fluid.mesh_animation.enabled) {
        hydrology::HydrologyArtifact section{};
        gpu_meshing::MeshResult debug{};
        const bool accepted = run_authored_fluid_bake(
            context, status, fluid_error, section, debug);
        status.total_sections = 1u;
        status.current_section = accepted ? 1u : 0u;
        status.completed_sections = accepted ? 1u : 0u;
        status.current_section_id = river_network_->sections.front().id;
        if (!accepted) {
            network_result.failed_debug_visual = std::move(debug);
            return false;
        }
        network_result.sections.push_back(section);
        network_result.products.visual_mesh = section.visual_mesh;
        network_result.products.coarse_cpu_mesh = section.coarse_cpu_mesh;
        network_result.products.gameplay_layout = section.gameplay_layout;
        network_result.products.gameplay_field = section.gameplay_field;
        network_result.products.presentation_field =
            section.presentation_field;
        if (!finalize_manifest(network_result)) {
            status.state = fluid_error.code == hydrology::FluidBakeCode::Cancelled
                ? matter::HydrologyState::Stale
                : matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        status.state = matter::HydrologyState::Ready;
        status.payload_digest = hex64(network_result.manifest.payload_digest);
        return true;
    }

    std::unordered_map<std::string, FluidBakeRequest> requests;
    gpu_meshing::MeshResult failed_section_debug{};
    std::size_t executing_order = 0u;
    bool all_cache_hits = true;
    const auto append_mesh = [](gpu_meshing::MeshResult& destination,
                                const gpu_meshing::MeshResult& source) {
        if (source.positions.empty() || source.indices.empty()) return;
        const auto base = static_cast<std::uint32_t>(
            destination.positions.size() / 3u);
        destination.positions.insert(destination.positions.end(),
                                     source.positions.begin(),
                                     source.positions.end());
        destination.normals.insert(destination.normals.end(),
                                   source.normals.begin(), source.normals.end());
        for (const auto index : source.indices)
            destination.indices.push_back(base + index);
        destination.material = 4u;
        destination.content_digest =
            gpu_meshing::mesh_content_digest(destination);
    };

    hydrology::SectionBakeExecutor executor =
        [&](const matter::RiverSectionDefinition& section,
            const std::vector<hydrology::SpillwayHandoffRecord>& upstream,
            hydrology::SectionBakeResult& section_result,
            hydrology::FluidBakeError& section_error) {
        const std::size_t order = executing_order++;
        hydrology::HydrologySectionTimings section_timings{};
        section_timings.id = section.id;
        const auto setup_start = std::chrono::steady_clock::now();
        status.current_section = static_cast<std::uint32_t>(order + 1u);
        status.current_section_id = section.id;
        const auto geometry_index = geometry_by_river.find(section.river);
        if (geometry_index == geometry_by_river.end()) {
            section_error = {hydrology::FluidBakeCode::InvalidInput,
                             "section river geometry is unavailable"};
            return false;
        }
        FluidBakeRunContext section_context = context;
        section_context.callbacks.progress =
            [&, order](const hydrology::FluidBakeProgress& progress) {
                const float local = progress.total_steps == 0u ? 0.0f
                    : static_cast<float>(progress.completed_steps) /
                      static_cast<float>(progress.total_steps);
                status.progress = (static_cast<float>(order) + local) /
                    static_cast<float>(river_network_->sections.size());
                status.completed_steps = progress.completed_steps;
                if (context.callbacks.progress) {
                    const std::uint64_t total =
                        static_cast<std::uint64_t>(progress.total_steps) *
                        river_network_->sections.size();
                    const std::uint64_t completed =
                        static_cast<std::uint64_t>(order) *
                            progress.total_steps + progress.completed_steps;
                    context.callbacks.progress({
                        static_cast<std::uint32_t>(std::min<std::uint64_t>(
                            completed, std::numeric_limits<std::uint32_t>::max())),
                        static_cast<std::uint32_t>(std::min<std::uint64_t>(
                            total, std::numeric_limits<std::uint32_t>::max())),
                        progress.active_particles,
                        progress.sensor_wet_fraction});
                }
            };
        FluidBakeRequest request{};
        if (!assemble_authored_fluid_section_request(
                *river_network_, geometries[geometry_index->second], section,
                upstream, authored_fluid_colliders_, section_context,
                abs_cache_root_, request, section_error))
            return false;
        requests[section.id] = request;
        const matter::RiverSectionDefinition* downstream_section = nullptr;
        for (const auto& possible : river_network_->sections) {
            if (std::find(possible.after_section_ids.begin(),
                          possible.after_section_ids.end(), section.id) ==
                possible.after_section_ids.end())
                continue;
            if (downstream_section) {
                section_error = {
                    hydrology::FluidBakeCode::InvalidInput,
                    "river section fan-out is outside the two-section milestone"};
                return false;
            }
            downstream_section = &possible;
        }
        std::optional<hydrology::SpillwayHandoffRecord> downstream_handoff;
        if (downstream_section) {
            const auto river = std::find_if(
                river_network_->rivers.begin(), river_network_->rivers.end(),
                [&](const auto& value) { return value.name == section.river; });
            hydrology::SpillwayHandoffRecord handoff{};
            if (river == river_network_->rivers.end() ||
                !hydrology::resolve_spillway_handoff(
                    section, *downstream_section,
                    geometries[geometry_index->second],
                    river->inlet.flow_m3s, handoff, section_error))
                return false;
            handoff.temporary_dam_exclusion_bounds_m =
                request.temporary_dam_bounds_m;
            handoff.semantic_key =
                hydrology::spillway_handoff_semantic_key(handoff);
            downstream_handoff = handoff;
        }
        std::vector<hydrology::SpillwayHandoffRecord> animation_ownership =
            upstream;
        if (downstream_handoff)
            animation_ownership.push_back(*downstream_handoff);
        status.input_key = hex64(request.semantic_key);
        hydrology::HydrologyArtifact candidate{};
        hydrology::WaterMeshAnimationArtifact animation_candidate{};
        const bool static_cache_hit = load_semantic_cache(request, candidate);
        const bool animation_enabled =
            river_network_->fluid.mesh_animation.enabled;
        std::uint64_t animation_semantic_key = 0u;
        std::filesystem::path animation_path;
        bool animation_cache_hit = !animation_enabled;
        if (static_cache_hit && animation_enabled) {
            animation_semantic_key = section_water_animation_semantic_key(
                request.semantic_key, candidate.payload_digest,
                river_network_->fluid.mesh_animation, animation_ownership);
            animation_path = section_water_animation_path(
                abs_cache_root_, section.id, animation_semantic_key);
            animation_cache_hit = load_water_animation_cache(
                animation_path, section.id, animation_semantic_key,
                candidate.payload_digest,
                river_network_->fluid.mesh_animation, animation_candidate);
        }
        const bool cache_hit = static_cache_hit && animation_cache_hit;
        section_timings.cache_hit = cache_hit;
        section_timings.animation_cache_hit = animation_cache_hit;
        section_timings.setup_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - setup_start).count();
        if (cache_hit) {
            section_result.cache_hit = true;
        } else {
            all_cache_hits = false;
            if (!cfg_.fluid_renderer_device.luid_valid) {
                section_error = {
                    hydrology::FluidBakeCode::BackendUnavailable,
                    "Vulkan render adapter identity is unavailable for the PhysX fluid bake"};
                return false;
            }
            if (!cfg_.fluid_bake_backend_factory) {
                section_error = {hydrology::FluidBakeCode::BackendUnavailable,
                                 "PhysX fluid support is disabled in this build"};
                return false;
            }
            const auto physx_init_start = std::chrono::steady_clock::now();
            auto backend = cfg_.fluid_bake_backend_factory();
            if (!backend) {
                section_error = {hydrology::FluidBakeCode::BackendUnavailable,
                                 "PhysX fluid backend factory returned no runtime"};
                return false;
            }
            const auto probe = backend->probe();
            section_timings.physx_init_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - physx_init_start).count();
            if (!probe.available || !probe.device_luid_valid ||
                probe.device_luid != cfg_.fluid_renderer_device.luid) {
                section_error = {
                    hydrology::FluidBakeCode::BackendUnavailable,
                    !probe.available
                        ? (probe.message.empty()
                               ? "PhysX fluid backend probe failed"
                               : probe.message)
                        : (!probe.device_luid_valid
                               ? "CUDA device identity is unavailable for the Vulkan render adapter comparison"
                               : "CUDA device identity does not match the Vulkan render adapter")};
                return false;
            }
            request.product_settings.provenance = {
                cfg_.fluid_renderer_device.vendor_id != 0u
                    ? cfg_.fluid_renderer_device.vendor_id : kNvidiaVendorId,
                cfg_.fluid_renderer_device.device_id != 0u
                    ? cfg_.fluid_renderer_device.device_id : 1u,
                probe.cuda_driver_version > 0
                    ? static_cast<std::uint32_t>(probe.cuda_driver_version)
                    : (cfg_.fluid_renderer_device.driver_version != 0u
                           ? cfg_.fluid_renderer_device.driver_version : 1u),
                probe.sdk_version_hex != 0u ? probe.sdk_version_hex
                                            : kPhysxSdkVersion,
                kFluidAdapterVersion};
            hydrology::FluidBakeOutput output{};
            const auto simulate_start = std::chrono::steady_clock::now();
            const bool accepted = hydrology::PhysxFluidBake::run(
                request.input, *backend, section_context.callbacks,
                output, section_error);
            section_timings.simulate_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - simulate_start).count();
            backend.reset();
            if (!accepted) {
                if (section_error.code != hydrology::FluidBakeCode::Cancelled &&
                    (section_error.code ==
                         hydrology::FluidBakeCode::SensorNotReached ||
                     section_error.code == hydrology::FluidBakeCode::Escaped)) {
                    hydrology::FluidBakeError debug_error{};
                    (void)hydrology::PhysxFluidBake::
                        build_failed_debug_visual_on_renderer(
                            output, section_error.code,
                            request.product_settings, cfg_.gpu_run,
                            cfg_.vk_particle_visual_bake,
                            failed_section_debug, debug_error);
                }
                std::string trace_error;
                (void)write_particle_trace(
                    request, output, section_error.code,
                    failed_section_debug, false, trace_error);
                return false;
            }
            if (context.callbacks.cancelled && context.callbacks.cancelled()) {
                section_error = {hydrology::FluidBakeCode::Cancelled,
                                 "authored fluid section was superseded before visual meshing"};
                return false;
            }
            hydrology::PhysxFluidBake::ProductBuildTimings product_timings{};
            if (!build_accepted_fluid_artifact(
                    output, request.product_settings, request.terrain,
                    candidate, section_error, &product_timings))
                return false;
            section_timings.gpu_mesh_ms = product_timings.gpu_mesh_ms;
            section_timings.cpu_mesh_ms = product_timings.cpu_mesh_ms;
            if (animation_enabled) {
                if (!output.animation_capture) {
                    section_error = {
                        hydrology::FluidBakeCode::ProductFailure,
                        "accepted animated section has no particle capture"};
                    return false;
                }
                const auto animation_mesh_start =
                    std::chrono::steady_clock::now();
                const hydrology::WaterMeshAnimationMesher animation_mesher =
                    [&](const gpu_meshing::ParticleJob& job,
                        gpu_meshing::MeshResult& mesh,
                        gpu_meshing::Stats& stats,
                        gpu_meshing::Error& mesh_error) {
                        std::string run_error;
                        const auto invoke = [&](std::string&) {
                            return cfg_.vk_particle_visual_bake(
                                job, mesh, stats, mesh_error,
                                {context.callbacks.cancelled, {}});
                        };
                        const bool completed = cfg_.gpu_run
                            ? cfg_.gpu_run(
                                  "hydrology_section_animation", invoke,
                                  run_error)
                            : invoke(run_error);
                        if (!completed && mesh_error.message.empty())
                            mesh_error = {
                                gpu_meshing::ErrorCode::VulkanFailure,
                                run_error.empty()
                                    ? "Vulkan section animation meshing failed"
                                    : run_error};
                        return completed;
                    };
                hydrology::WaterMeshAnimation raw_animation{};
                gpu_meshing::Error animation_error{};
                if (!hydrology::build_water_mesh_animation(
                        *output.animation_capture,
                        request.product_settings.particle_radius_m,
                        request.product_settings.visual_job,
                        animation_mesher, raw_animation, animation_error)) {
                    section_error = {
                        hydrology::FluidBakeCode::ProductFailure,
                        animation_error.message.empty()
                            ? "section water animation meshing failed"
                            : animation_error.message};
                    return false;
                }
                hydrology::WaterMeshAnimation owned_animation{};
                if (!hydrology::clip_section_water_mesh_animation(
                        raw_animation, section.id, animation_ownership,
                        request.product_settings.visual_job.voxel_m,
                        owned_animation, section_error))
                    return false;
                animation_semantic_key =
                    section_water_animation_semantic_key(
                        request.semantic_key, candidate.payload_digest,
                        river_network_->fluid.mesh_animation,
                        animation_ownership);
                animation_path = section_water_animation_path(
                    abs_cache_root_, section.id, animation_semantic_key);
                if (!hydrology::pack_water_mesh_animation_artifact(
                        {section.id, animation_semantic_key,
                         candidate.payload_digest, 0u,
                         request.product_settings.visual_job.voxel_m},
                        owned_animation, animation_candidate,
                        animation_error)) {
                    section_error = {
                        hydrology::FluidBakeCode::ProductFailure,
                        animation_error.message.empty()
                            ? "section water animation packing failed"
                            : animation_error.message};
                    return false;
                }
                section_timings.animation_mesh_ms =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() -
                        animation_mesh_start).count();
            }
            std::string trace_error;
            (void)write_particle_trace(
                request, output, hydrology::FluidBakeCode::Ready,
                candidate.visual_mesh, true, trace_error);
            gpu_meshing::Error artifact_error{};
            const auto serialize_start = std::chrono::steady_clock::now();
            if (!hydrology::save_artifact_atomic(
                    request.cache_path, candidate, artifact_error) ||
                !hydrology::load_artifact_validated(
                    request.cache_path, candidate.product_keys.visual,
                    candidate, artifact_error, request.semantic_key) ||
                (animation_enabled &&
                 (!hydrology::save_water_mesh_animation_artifact_immutable(
                      animation_path, animation_candidate, artifact_error) ||
                  !load_water_animation_cache(
                      animation_path, section.id, animation_semantic_key,
                      candidate.payload_digest,
                      river_network_->fluid.mesh_animation,
                      animation_candidate)))) {
                section_error = {hydrology::FluidBakeCode::ProductFailure,
                                 artifact_error.message};
                return false;
            }
            network_result.timings.serialize_ms +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - serialize_start).count();
        }

        section_result.artifact = candidate;
        if (animation_enabled) {
            section_timings.animation_bytes =
                animation_candidate.frame_payload.size();
            section_result.animation = animation_candidate;
        }
        section_result.downstream_handoff = downstream_handoff;
        status.completed_sections = static_cast<std::uint32_t>(order + 1u);
        status.progress = static_cast<float>(order + 1u) /
            static_cast<float>(river_network_->sections.size());
        network_result.timings.sections.push_back(
            std::move(section_timings));
        return true;
    };

    hydrology::RiverSectionSequenceResult sequence{};
    hydrology::RiverSectionSequenceCallbacks sequence_callbacks{};
    sequence_callbacks.cancelled = context.callbacks.cancelled;
    sequence_callbacks.progress = [&](float fraction, const std::string& id) {
        status.progress = std::max(status.progress, fraction);
        if (!id.empty()) status.current_section_id = id;
    };
    if (!hydrology::run_river_section_sequence(
            *river_network_, graph, executor, sequence_callbacks,
            sequence, fluid_error)) {
        for (const auto& accepted : sequence.sections) {
            network_result.sections.push_back(accepted.artifact);
            append_mesh(network_result.failed_debug_visual,
                        accepted.artifact.visual_mesh);
        }
        append_mesh(network_result.failed_debug_visual, failed_section_debug);
        network_result.manifest = sequence.manifest;
        status.state = fluid_error.code == hydrology::FluidBakeCode::Cancelled
            ? matter::HydrologyState::Stale
            : matter::HydrologyState::Invalid;
        status.failure_reason = fluid_error.message;
        return false;
    }

    for (auto& accepted : sequence.sections) {
        network_result.sections.push_back(std::move(accepted.artifact));
        if (accepted.animation)
            network_result.section_animations.push_back(
                std::move(*accepted.animation));
    }
    if (network_result.sections.size() == 1u) {
        const auto& section = network_result.sections.front();
        network_result.products.visual_mesh = section.visual_mesh;
        network_result.products.coarse_cpu_mesh = section.coarse_cpu_mesh;
        network_result.products.gameplay_layout = section.gameplay_layout;
        network_result.products.gameplay_field = section.gameplay_field;
        network_result.products.presentation_field =
            section.presentation_field;
        if (!finalize_manifest(network_result)) {
            status.state = fluid_error.code ==
                    hydrology::FluidBakeCode::Cancelled
                ? matter::HydrologyState::Stale
                : matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        status.state = matter::HydrologyState::Ready;
        status.cache_hit = all_cache_hits;
        status.progress = 1.0f;
        status.completed_sections = 1u;
        status.current_section = 1u;
        status.payload_digest =
            hex64(network_result.manifest.payload_digest);
        return true;
    }
    std::vector<hydrology::SpillwayHandoffRecord> handoff_records;
    for (const auto& accepted : sequence.sections)
        if (accepted.downstream_handoff)
            handoff_records.push_back(*accepted.downstream_handoff);
    if (handoff_records.size() != 1u ||
        network_result.sections.size() != 2u) {
        fluid_error = {hydrology::FluidBakeCode::InvalidInput,
                       "the sequential provider milestone requires exactly two sections and one handoff"};
        status.state = matter::HydrologyState::Invalid;
        status.failure_reason = fluid_error.message;
        return false;
    }
    const auto& handoff = handoff_records.front();
    const auto upstream = std::find_if(
        network_result.sections.begin(), network_result.sections.end(),
        [&](const auto& value) {
            return value.section.section_id == handoff.upstream_section_id;
        });
    const auto downstream = std::find_if(
        network_result.sections.begin(), network_result.sections.end(),
        [&](const auto& value) {
            return value.section.section_id == handoff.downstream_section_id;
        });
    if (upstream == network_result.sections.end() ||
        downstream == network_result.sections.end()) {
        fluid_error = {hydrology::FluidBakeCode::ProductFailure,
                       "accepted handoff section artifacts are missing"};
        status.state = matter::HydrologyState::Invalid;
        status.failure_reason = fluid_error.message;
        return false;
    }
    const auto& upstream_request = requests.at(handoff.upstream_section_id);
    const auto& downstream_request = requests.at(handoff.downstream_section_id);
    hydrology::HandoffProductSettings handoff_settings{};
    handoff_settings.visual_job = upstream_request.product_settings.visual_job;
    handoff_settings.visual_job.bounds_m.min_m = {
        std::min(upstream_request.product_settings.visual_job.bounds_m.min_m.x,
                 downstream_request.product_settings.visual_job.bounds_m.min_m.x),
        std::min(upstream_request.product_settings.visual_job.bounds_m.min_m.y,
                 downstream_request.product_settings.visual_job.bounds_m.min_m.y),
        std::min(upstream_request.product_settings.visual_job.bounds_m.min_m.z,
                 downstream_request.product_settings.visual_job.bounds_m.min_m.z)};
    handoff_settings.visual_job.bounds_m.max_m = {
        std::max(upstream_request.product_settings.visual_job.bounds_m.max_m.x,
                 downstream_request.product_settings.visual_job.bounds_m.max_m.x),
        std::max(upstream_request.product_settings.visual_job.bounds_m.max_m.y,
                 downstream_request.product_settings.visual_job.bounds_m.max_m.y),
        std::max(upstream_request.product_settings.visual_job.bounds_m.max_m.z,
                 downstream_request.product_settings.visual_job.bounds_m.max_m.z)};
    handoff_settings.particle_radius_m =
        upstream_request.product_settings.particle_radius_m;
    const float gameplay_cell =
        upstream_request.product_settings.gameplay_layout.cell_size_m;
    const auto gameplay_min_x = std::min(
        upstream->gameplay_layout.origin_m.x,
        downstream->gameplay_layout.origin_m.x);
    const auto gameplay_min_z = std::min(
        upstream->gameplay_layout.origin_m.z,
        downstream->gameplay_layout.origin_m.z);
    const auto gameplay_max_x = std::max(
        upstream->gameplay_layout.origin_m.x +
            upstream->gameplay_layout.width * gameplay_cell,
        downstream->gameplay_layout.origin_m.x +
            downstream->gameplay_layout.width * gameplay_cell);
    const auto gameplay_max_z = std::max(
        upstream->gameplay_layout.origin_m.z +
            upstream->gameplay_layout.depth * gameplay_cell,
        downstream->gameplay_layout.origin_m.z +
            downstream->gameplay_layout.depth * gameplay_cell);
    handoff_settings.gameplay_layout = {
        {gameplay_min_x, 0.0f, gameplay_min_z}, gameplay_cell,
        static_cast<std::uint32_t>(std::ceil(
            (gameplay_max_x - gameplay_min_x) / gameplay_cell)),
        static_cast<std::uint32_t>(std::ceil(
            (gameplay_max_z - gameplay_min_z) / gameplay_cell))};
    const hydrology::PhysxFluidBake::VisualMesher handoff_mesher =
        [this](const gpu_meshing::ParticleJob& job,
               gpu_meshing::MeshResult& mesh, gpu_meshing::Stats& stats,
               gpu_meshing::Error& error,
               const gpu_meshing::BuildControl& control) {
            std::string run_error;
            const auto invoke = [&](std::string&) {
                return cfg_.vk_particle_visual_bake(
                    job, mesh, stats, error, control);
            };
            const bool completed = cfg_.gpu_run
                ? cfg_.gpu_run("hydrology_handoff_visual", invoke, run_error)
                : invoke(run_error);
            if (!completed && error.message.empty()) {
                error = {gpu_meshing::ErrorCode::VulkanFailure,
                         run_error.empty()
                             ? "Vulkan handoff meshing failed"
                             : run_error};
            }
            return completed;
    };
    hydrology::HydrologyHandoffArtifact handoff_artifact{};
    const auto handoff_mesh_start = std::chrono::steady_clock::now();
    if (!hydrology::build_handoff_artifact(
            *upstream, *downstream, handoff, handoff_settings,
            handoff_mesher, handoff_artifact, network_result.products,
            fluid_error)) {
        status.state = matter::HydrologyState::Invalid;
        status.failure_reason = fluid_error.message;
        return false;
    }
    network_result.timings.handoff_mesh_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - handoff_mesh_start).count();
    hydrology::WaterMeshAnimationArtifact handoff_animation{};
    std::filesystem::path handoff_animation_path;
    if (river_network_->fluid.mesh_animation.enabled) {
        const auto upstream_animation = std::find_if(
            network_result.section_animations.begin(),
            network_result.section_animations.end(),
            [&](const auto& value) {
                return value.identity == handoff.upstream_section_id;
            });
        const auto downstream_animation = std::find_if(
            network_result.section_animations.begin(),
            network_result.section_animations.end(),
            [&](const auto& value) {
                return value.identity == handoff.downstream_section_id;
            });
        if (upstream_animation == network_result.section_animations.end() ||
            downstream_animation == network_result.section_animations.end()) {
            fluid_error = {
                hydrology::FluidBakeCode::ProductFailure,
                "handoff animation is missing an adjacent section animation"};
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        const auto animation_start = std::chrono::steady_clock::now();
        if (!hydrology::build_handoff_water_animation_artifact(
                *upstream_animation, *downstream_animation, handoff,
                handoff_settings.visual_job.voxel_m,
                handoff_animation, fluid_error)) {
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        network_result.timings.handoff_animation_mesh_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - animation_start).count();
        handoff_animation_path =
            std::filesystem::path(abs_cache_root_) / "hydrology" /
            "animations" / "handoffs" /
            (safe_cache_id(handoff.id) + "-" +
             hex64(handoff_animation.semantic_key) + ".mhwa");
    }
    const auto handoff_path = std::filesystem::path(abs_cache_root_) /
        "hydrology" / "handoffs" /
        (safe_cache_id(handoff_artifact.id) + "-" +
         hex64(handoff_artifact.semantic_key) + ".mhyd");
    gpu_meshing::Error artifact_error{};
    hydrology::HydrologyHandoffArtifact reopened_handoff{};
    const auto handoff_serialize_start = std::chrono::steady_clock::now();
    if (!hydrology::save_handoff_artifact_atomic(
            handoff_path, handoff_artifact, artifact_error) ||
        !hydrology::load_handoff_artifact_validated(
            handoff_path, handoff_artifact.semantic_key,
            handoff_artifact.upstream_payload_digest,
            handoff_artifact.downstream_payload_digest,
            reopened_handoff, artifact_error) ||
        (river_network_->fluid.mesh_animation.enabled &&
         !hydrology::save_water_mesh_animation_artifact_immutable(
             handoff_animation_path, handoff_animation, artifact_error))) {
        fluid_error = {hydrology::FluidBakeCode::ProductFailure,
                       artifact_error.message};
        status.state = matter::HydrologyState::Invalid;
        status.failure_reason = fluid_error.message;
        return false;
    }
    network_result.timings.serialize_ms +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - handoff_serialize_start).count();
    network_result.handoffs.push_back(std::move(reopened_handoff));
    if (river_network_->fluid.mesh_animation.enabled) {
        hydrology::WaterMeshAnimationArtifact reopened_animation{};
        if (!hydrology::load_water_mesh_animation_artifact(
                handoff_animation_path, reopened_animation,
                artifact_error) ||
            reopened_animation.identity != handoff.id ||
            reopened_animation.semantic_key !=
                handoff_animation.semantic_key ||
            reopened_animation.source_primary_payload_digest !=
                handoff_animation.source_primary_payload_digest ||
            reopened_animation.source_secondary_payload_digest !=
                handoff_animation.source_secondary_payload_digest) {
            fluid_error = {hydrology::FluidBakeCode::ProductFailure,
                           artifact_error.message.empty()
                               ? "published handoff animation validation failed"
                               : artifact_error.message};
            status.state = matter::HydrologyState::Invalid;
            status.failure_reason = fluid_error.message;
            return false;
        }
        network_result.handoff_animations.push_back(
            std::move(reopened_animation));
    }
    if (!finalize_manifest(network_result)) {
        status.state = fluid_error.code == hydrology::FluidBakeCode::Cancelled
            ? matter::HydrologyState::Stale
            : matter::HydrologyState::Invalid;
        status.failure_reason = fluid_error.message;
        return false;
    }
    status.state = matter::HydrologyState::Ready;
    status.cache_hit = all_cache_hits;
    status.progress = 1.0f;
    status.completed_sections = status.total_sections;
    status.current_section = status.total_sections;
    status.wet_cells = static_cast<std::uint32_t>(std::count_if(
        network_result.products.gameplay_field.begin(),
        network_result.products.gameplay_field.end(),
        [](const auto& sample) { return sample.wet_valid; }));
    status.mesh_triangles = static_cast<std::uint32_t>(
        network_result.products.visual_mesh.indices.size() / 3u);
    status.completed_steps = 0u;
    status.simulated_time_s = 0.0;
    for (const auto& section : network_result.sections) {
        status.completed_steps += section.stats.simulated_steps;
        status.simulated_time_s +=
            static_cast<double>(section.stats.simulated_steps) *
            river_network_->fluid.pbd.fixed_step_seconds;
    }
    status.payload_digest = hex64(network_result.manifest.payload_digest);
    network_result.timings.total_wall_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - network_wall_start).count();
    std::string timing_trace_error;
    if (!write_network_timing_trace(network_result, timing_trace_error))
        std::fprintf(stderr, "[hydrology] network timing trace rejected: %s\n",
                     timing_trace_error.c_str());
    return true;
}

bool LocalProvider::build_river_height_overlay(
    hydrology::RiverGeometry& geometry,
    std::shared_ptr<const terrain_field::RiverHeightOverlay>& overlay,
    std::string& error) const {
    if (!river_network_) {
        geometry = {};
        overlay.reset();
        error.clear();
        return true;
    }
    const matter::RiverSectionDefinition* section =
        initial_section(*river_network_);
    if (!section) {
        error = "river network has no initial section";
        return false;
    }
    hydrology::RiverGeometry built_geometry{};
    if (!hydrology::build_river_geometry(
            *river_network_, section->river,
            built_geometry, error))
        return false;
    std::shared_ptr<const terrain_field::RiverHeightOverlay> built_overlay;
    if (!terrain_field::RiverHeightOverlay::build(
            built_geometry, built_overlay, error))
        return false;
    geometry = std::move(built_geometry);
    overlay = std::move(built_overlay);
    return true;
}

std::string LocalProvider::resolve_object_path(const std::string& module) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const std::string& root : abs_object_roots_) {
        const std::string path = (fs::path(root) / (module + ".js")).string();
        ec.clear();
        if (fs::is_regular_file(path, ec)) return path;
    }
    return {};
}

bool LocalProvider::prepare_paths(std::string& err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(cfg_.cache_root) / "parts", ec);
    if (ec) {
        err = "LocalProvider: cannot create cache root " + cfg_.cache_root +
              ": " + ec.message();
        return false;
    }

    abs_cache_root_ = abspath(cfg_.cache_root);
    abs_schemas_ = abspath(cfg_.object_sources_dir());
    abs_object_roots_.clear();
    for (const std::string& root : cfg_.object_roots())
        abs_object_roots_.push_back(abspath(root));
    abs_shared_lib_roots_.clear();
    abs_project_shared_lib_.clear();
    abs_engine_shared_lib_.clear();
    abs_world_path_ = abspath(cfg_.world_path);
    if (!cfg_.project_shared_lib_dir.empty())
        abs_project_shared_lib_ = abspath(cfg_.project_shared_lib_dir);
    if (!cfg_.engine_shared_lib_dir.empty())
        abs_engine_shared_lib_ = abspath(cfg_.engine_shared_lib_dir);
    for (const std::string& root : cfg_.shared_lib_roots())
        abs_shared_lib_roots_.push_back(abspath(root));
    return true;
}

bool LocalProvider::load_authored_world(std::string& err) {
    roots_.clear();
    root_transforms_.clear();
    expand_flags_.clear();
    tileset_flags_.clear();
    authored_fluid_colliders_.clear();
    world_module_.clear();
    authored_lights_ = world_lights::WorldLights{};
    world_settings_ = matter::WorldSettings{};
    hydrology_settings_.reset();
    river_network_.reset();
    terrain_collision_.reset();
    authored_entities_.clear();

#if defined(MATTER_HAVE_SCRIPT_HOST)
    matter::WorldLoadDesc load_desc;
    load_desc.world_path = abs_world_path_;
    load_desc.objects_dir = abs_schemas_;
    load_desc.project_shared_lib_dir = abs_project_shared_lib_;
    load_desc.engine_shared_lib_dir = abs_engine_shared_lib_;
    load_desc.canonical_params_json = cfg_.root_params_json.empty()
        ? "{}" : cfg_.root_params_json;
    const Params canonical_params =
        params_from_json(load_desc.canonical_params_json);
    const auto seed = canonical_params.find("worldSeed");
    if (seed != canonical_params.end() &&
        seed->second.kind == ParamValue::Kind::Number)
        load_desc.world_seed = static_cast<std::uint64_t>(seed->second.num);

    matter::WorldDefinition definition;
    matter::WorldLoadError load_error;
    if (!matter::load_world_definition(load_desc, definition, load_error)) {
        err = "load world definition " + abs_world_path_ + ": " +
              load_error.message;
        if (!load_error.property_path.empty())
            err += " [" + load_error.property_path + "]";
        return false;
    }
    ProviderWorldDefinition adapted = adapt_world_definition(definition);
    roots_ = std::move(adapted.roots);
    root_transforms_ = std::move(adapted.root_transforms);
    expand_flags_ = std::move(adapted.expand_flags);
    tileset_flags_ = std::move(adapted.tileset_flags);
    authored_fluid_colliders_ = std::move(adapted.fluid_colliders);
    authored_lights_ = std::move(adapted.lights);
    world_settings_ = adapted.settings;
    hydrology_settings_ = std::move(adapted.hydrology);
    river_network_ = std::move(adapted.river_network);
    terrain_collision_ = std::move(adapted.terrain_collision);
    authored_entities_ = definition.entities;
    // defineMaterial() already installed these in the global registry while the
    // world script evaluated (chart-VT contract C3); what we keep here is the
    // scheduling half — which materials want an automated detail-tileset bake.
    world_materials_ = definition.materials;
    // Script-declared runtime tunables. A top-level WorldDefinition member has
    // to be copied out explicitly — `definition` is a stack local that dies at
    // the end of this function, and only `settings` rides along through
    // adapt_world_definition.
    world_prop_specs_ = definition.props;

    // Field worlds retain the existing eval_world/streaming path. The statics
    // loader intentionally does not execute field(), so identify the authored
    // method lexically and let install_world perform the established evaluation.
    std::ifstream source_file(abs_world_path_, std::ios::binary);
    std::ostringstream source_stream;
    source_stream << source_file.rdbuf();
    static const std::regex field_method("\\bfield\\s*\\(");
    if (std::regex_search(source_stream.str(), field_method))
        world_module_ = cfg_.world_name;
    return true;
#else
    err = "project world loading requires MATTER_HAVE_SCRIPT_HOST";
    return false;
#endif
}

std::set<std::string> LocalProvider::collect_entity_part_modules(
    const std::vector<matter::RawEntityRecipe>& entities) {
    std::set<std::string> modules;
    for (const auto& ent : entities) {
        auto pi_pos = ent.components_json.find("\"PartInstance\"");
        if (pi_pos == std::string::npos) continue;
        auto colon = ent.components_json.find(':', pi_pos);
        if (colon == std::string::npos) continue;
        auto brace = ent.components_json.find('{', colon);
        if (brace == std::string::npos) continue;
        auto part_pos = ent.components_json.find("\"part\"", brace);
        if (part_pos == std::string::npos) continue;
        auto pcolon = ent.components_json.find(':', part_pos);
        if (pcolon == std::string::npos) continue;
        size_t p = pcolon + 1;
        while (p < ent.components_json.size() && ent.components_json[p] == ' ') ++p;
        if (p >= ent.components_json.size() || ent.components_json[p] != '"') continue;
        ++p;
        size_t start = p;
        while (p < ent.components_json.size() && ent.components_json[p] != '"') ++p;
        std::string mod = ent.components_json.substr(start, p - start);
        if (!mod.empty()) modules.insert(std::move(mod));
    }
    return modules;
}

void LocalProvider::append_entity_part_roots() {
    entity_part_root_start_ = roots_for_install_.size();
    std::set<std::string> seen_roots;
    for (const auto& root : roots_for_install_)
        seen_roots.insert(root.module);
    for (const auto& module : collect_entity_part_modules(authored_entities_)) {
        if (seen_roots.insert(module).second)
            roots_for_install_.push_back({module, {}});
    }
}

bool LocalProvider::install_graph(std::string& err, part_graph::BakePolicy policy) {
    // Reset all mutable state at entry so repeated install_graph() calls are
    // idempotent. Unload any previously-loaded tileset slots so a re-connect
    // for a different world doesn't inherit stale atlases. reset_tileset_bindings
    // unbinds every material this provider pointed at a detail slot (material 16
    // via the deprecated root path, plus any defineMaterial() detail), empties
    // the LRU pool, and drops the previous world's dynamic registry entries.
    reset_tileset_bindings();
    baked_tileset_count_ = 0;

    baked_count_ = 0;
    hit_count_   = 0;
    install_bake_count_ = 0;
    baked_hashes_.clear();

    // Clear cross-phase state
    roots_.clear();
    root_transforms_.clear();
    expand_flags_.clear();
    tileset_flags_.clear();
    authored_fluid_colliders_.clear();
    roots_for_install_.clear();
    install_to_orig_.clear();
    tileset_indices_.clear();
    ir_ = part_graph::InstallResult{};
    graph_snapshot_ = part_graph_snapshot::Snapshot{};  // Task 9: reset snapshot

    if (!prepare_paths(err)) return false;

#if defined(MATTER_HAVE_AUTOREMESHER)
    // Originally this existed because the vendored 2017 TBB segfaulted on WSL2
    // if the first retopo() call happened after heap-heavy work (install()
    // calls save_v2 for every fresh bake). That TBB is gone — replaced by a
    // header-only shim — so the crash hazard is gone with it.
    //
    // The call is kept because it still does useful work: the first retopo()
    // is what runs ensure_singletons_initialized(), which brings up geogram's
    // Logger / ProcessManager / attribute registry. Doing that here moves it
    // off the first real bake. Cost is ~10 ms on the tiny warm-up cube.
    tbb_warmup_retopo();

    // Load the retopo blacklist journal. Any hash present in the .retopo_pending
    // journal without a matching .retopo_success entry crashed autoremesher on
    // a previous run and will be skipped in this session. See
    // MatterEngine3/include/retopo_blacklist.h for the mechanism.
    matter_engine3::retopo_blacklist::init(abs_cache_root_);
#endif

#if defined(MATTER_HAVE_SCRIPT_HOST)
    // SP-2/SP-3/SP-7 wiring. HostBaker receives the absolute cache root; it passes
    // it through BakeOptions.parts_dir so bake_source writes artifacts to absolute
    // paths (Task 3 Phase B: no chdir required).
    host_ = std::make_unique<script_host::ScriptHost>();
    host_->set_shared_lib_roots(abs_shared_lib_roots_);
    resolver_ = std::make_unique<part_graph::FileModuleResolver>(*host_, abs_object_roots_);
    // Task 13 (Phase C): create a shared HostBaker that persists beyond install_graph()
    // so ensure_part_baked() can reuse it without reconstructing a ScriptHost.
    host_baker_ = std::make_unique<part_graph::HostBaker>(*host_, abs_cache_root_);
    // W3: thread the optional per-rung bake observer (null in production).
    host_baker_->set_bake_observer(cfg_.bake_observer);

    // Task 2: apply transient settings to the baker (if set_transient_modules was called)
    if (!transient_modules_.empty()) {
        host_baker_->set_transient(&transient_modules_, transient_dir_);
    }

    // Capture cfg_ pointer for the RecordingBaker lambda (install-phase on_part).
    LocalProviderConfig* cfg_ptr = &cfg_;
    int* install_bake_count_ptr  = &install_bake_count_;

    // Task 5 (Phase B): RecordingBaker::bake() fires cfg_.on_part for
    // each freshly-baked part during install, with total==0 (indeterminate).
    // Task 13: RecordingBaker delegates to *host_baker_ (the shared HostBaker
    // member) instead of an inline inner, so ensure_part_baked() reuses it.
    part_graph::HostBaker* shared_baker_ptr = host_baker_.get();
    struct RecordingBaker : public Baker {
        part_graph::HostBaker& inner;
        LocalProviderConfig* cfg;
        int* install_bake_count;
        RecordingBaker(part_graph::HostBaker& b,
                       LocalProviderConfig* c, int* ibc)
            : inner(b), cfg(c), install_bake_count(ibc) {}
        uint64_t resolve_hash(const std::string& source, const Params& params,
                              const std::vector<uint64_t>& child_hashes) override {
            return inner.resolve_hash(source, params, child_hashes);
        }
        bool cached(uint64_t resolved_hash) override { return inner.cached(resolved_hash); }
        bool bake(const std::string& source, const Params& params,
                  const std::vector<uint64_t>& child_hashes,
                  const std::vector<std::string>& child_modules,
                  const std::vector<std::string>& child_params,
                  uint64_t resolved_hash) override {
            // Task 7: fire test_fault_hook (0-based index = current count BEFORE increment).
            // Exceptions propagate to PartGraph::install's per-node try-catch if present,
            // or to the worker's top-level catch; they serve as the OOM injection point.
            if (cfg && cfg->test_fault_hook)
                cfg->test_fault_hook(*install_bake_count);
            // Task 5 (Phase B): fire install-phase on_part before delegating.
            // total == 0 signals indeterminate count (install phase).
            if (cfg && cfg->on_part) {
                std::string class_name = class_name_from_source(source);
                const char* mod = class_name.empty() ? nullptr : class_name.c_str();
                cfg->on_part(mod, ++(*install_bake_count), 0);
            }
            return inner.bake(source, params, child_hashes, child_modules,
                              child_params, resolved_hash);
        }
        bool bake_lod_variants(const std::string& source, const Params& params,
                               const std::vector<uint64_t>& child_hashes,
                               uint64_t resolved_hash) override {
            return inner.bake_lod_variants(source, params, child_hashes, resolved_hash);
        }
        // Task 2: forward module notification to HostBaker for transient routing.
        void set_baking_module(const std::string& module) override {
            inner.set_baking_module(module);
        }
    };
    RecordingBaker baker(*shared_baker_ptr, cfg_ptr, install_bake_count_ptr);
    PartGraph graph(*resolver_, baker);

    if (!load_authored_world(err)) return false;

    // Tileset roots are installed by run_tileset_phase (it calls install() itself
    // on the tileset script's `static requires` children). Split them out here so
    // PartGraph::install() only sees the non-tileset roots.
    for (size_t i = 0; i < roots_.size(); ++i) {
        if (tileset_flags_[i]) tileset_indices_.push_back(i);
        else { roots_for_install_.push_back(roots_[i]); install_to_orig_.push_back(i); }
    }

    // Entity-referenced modules: scan authored entities for PartInstance.part
    // names and add them as extra roots so they get baked alongside world roots.
    // compose_world() iterates roots_ (not roots_for_install_) so these extras
    // won't be placed as static instances.
    append_entity_part_roots();

    // Phase C Task 7: if a root_params_json override is set (e.g. {"worldSeed": 2}
    // from WorldSession::regenerate()), merge it into every root's params before
    // calling install() so merge_params_canonical (and hence the resolved hash)
    // reflects the override. Override keys win; keys absent from the override are
    // unchanged. Only manifest roots receive the override; child parts (scatter
    // schemas such as Rock/Grass) get their params exclusively from the parent's
    // `static requires` function, which intentionally does NOT forward worldSeed to
    // scatter children — so their hashes are seed-free and hit cache on a reroll.
    if (!cfg_.root_params_json.empty()) {
        Params override_params = params_from_json(cfg_.root_params_json);
        if (!override_params.empty()) {
            for (size_t ri = 0; ri < entity_part_root_start_; ++ri)
                for (const auto& kv : override_params)
                    roots_for_install_[ri].params[kv.first] = kv.second;
        }
    }

    ir_ = graph.install(roots_for_install_, &graph_snapshot_, policy);
    if (!ir_.ok) {
        err = ir_.error;
        return false;
    }
    // Task 9: source_path is set by FileModuleResolver::source_path_for (called
    // from install's snapshot recording). Re-build the by_file index now that
    // source_path entries are present (they were set during install for FileModuleResolver).
    // The by_import index is already built in install; by_file is also built there.
    // Nothing extra needed here: install fills both indices directly.
    baked_count_ = (int)ir_.baked.size();
    hit_count_   = ir_.hits;
    baked_hashes_.insert(ir_.baked.begin(), ir_.baked.end());

    // Build hash -> module name map for use in fetch_parts()'s on_part callback.
    // Populate from roots first, then augment with all graph snapshot nodes so
    // that expanded children (e.g. BoxA placed inside a World expand root) also
    // get a module name in BakePartDone events and publish sort operations.
    module_by_hash_.clear();
    for (size_t j = 0; j < ir_.root_hashes.size(); ++j)
        if (ir_.root_hashes[j] != 0)
            module_by_hash_[ir_.root_hashes[j]] = roots_for_install_[j].module;
    for (const auto& kv : graph_snapshot_.nodes)
        if (kv.second.resolved_hash != 0)
            module_by_hash_.emplace(kv.second.resolved_hash, kv.second.module);

    // Map each root module to the child-FOLDED resolved hash the graph baked it under.
    // install() returns root_hashes parallel to `roots_for_install`; using them (instead
    // of an unfolded resolve_hash recompute) keeps manifest instances pointing at the .part
    // that actually exists on disk — critical once a root has children (e.g. Tree->Leaf).
    if (ir_.root_hashes.size() != roots_for_install_.size()) {
        err = "install did not return a hash for every root";
        return false;
    }

    return true;
#else
    // Without MATTER_HAVE_SCRIPT_HOST, the install phase can't do real baking.
    err = "install_graph: MATTER_HAVE_SCRIPT_HOST not defined";
    return false;
#endif
}

bool LocalProvider::ensure_part_baked(uint64_t part_hash, std::string& err) {
#if defined(MATTER_HAVE_SCRIPT_HOST)
    if (!host_baker_) {
        err = "ensure_part_baked: install_graph() has not been called";
        return false;
    }

    // Top-level entry guard: bake_plan covers every node of the installed graph.
    // An absent hash at the top level means the caller passed a stale or garbage
    // hash — fail fast rather than silently succeeding with no bake output.
    if (ir_.bake_plan.find(part_hash) == ir_.bake_plan.end()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)part_hash);
        err = std::string("ensure_part_baked: hash ") + buf + " not in bake plan";
        return false;
    }

    // Post-order DFS over bake_plan children so children are baked before parents.
    // Each node: cached() short-circuits; otherwise bake + bake_lod_variants.
    // Visited set prevents double-visiting in a DAG (shared children).
    std::set<uint64_t> visited;
    std::string bake_err;

    std::function<bool(uint64_t)> bake_subtree = [&](uint64_t hash) -> bool {
        if (visited.count(hash)) return true;
        visited.insert(hash);

        auto it = ir_.bake_plan.find(hash);
        if (it == ir_.bake_plan.end()) {
            // Not in bake_plan — already baked by install (BakePolicy::All path)
            // or not a known node. Treat as cached/ok for recursive child lookups.
            return true;
        }
        const part_graph::BakeInputs& bi = it->second;

        // Recurse into children first (post-order).
        for (uint64_t child_hash : bi.child_hashes) {
            if (!bake_subtree(child_hash)) return false;
        }

        // cached() short-circuits — counts as a demand-phase cache hit.
        if (host_baker_->cached(hash)) {
            ++hit_count_;
            return true;
        }

        // Fire on_part callback (demand phase, total==0 signals indeterminate count).
        if (cfg_.on_part) {
            const char* mod = bi.module.empty() ? nullptr : bi.module.c_str();
            cfg_.on_part(mod, ++install_bake_count_, 0);
        }

        // Set the baking module for transient routing (must precede bake call)
        host_baker_->set_baking_module(bi.module);

        // Bake
        bool bake_ok = false;
        try {
            bake_ok = host_baker_->bake(bi.source, bi.params, bi.child_hashes,
                                        bi.child_modules, bi.child_params, hash);
        } catch (std::bad_alloc&) {
            bake_err = "out of memory baking part: " + bi.module;
            return false;
        } catch (std::exception& e) {
            bake_err = std::string("exception baking part: ") + bi.module + ": " + e.what();
            return false;
        } catch (...) {
            bake_err = "unknown exception baking part: " + bi.module;
            return false;
        }
        if (!bake_ok) {
            bake_err = "bake failed for part: " + bi.module;
            return false;
        }

        // bake_lod_variants (mirrors install's per-node call)
        if (!host_baker_->bake_lod_variants(bi.source, bi.params, bi.child_hashes, hash)) {
            bake_err = "lod-variant bake failed for part: " + bi.module;
            return false;
        }

        // Track freshly demand-baked parts in baked_count_ and baked_hashes_ so
        // frame_stats().parts_baked reflects demand-phase activity and future
        // reconcile() calls know this hash is freshly written to disk.
        ++baked_count_;
        baked_hashes_.insert(hash);

        return true;
    };

    bool ok = bake_subtree(part_hash);
    if (!ok) err = bake_err;
    return ok;
#else
    err = "ensure_part_baked: MATTER_HAVE_SCRIPT_HOST not defined";
    return false;
#endif
}

bool LocalProvider::ensure_part_flattened(uint64_t part_hash) {
    // Identical logic to compose_world's flatten_one lambda (moved here verbatim
    // as a member; compose_world delegates to this function).
    // Transient parts live in scratch; their flats belong there too (never the cache).
    const std::string root = artifact_root(part_hash);
    const std::string flat_abs_path =
        root + "/" + part_asset::cache_path_flat(part_hash);
    if (part_asset::is_cache_artifact_header_compatible(
            flat_abs_path, part_hash, part_asset::kFormatVersionFlat))
        return true;

    // An ANIMATED Part has no static flat representation and must not be
    // flattened. It renders through the ECS dynamic lane -- PartStore loads it
    // together with its committed animation bundle (see the ANLK branch in
    // PartStore's candidate loader) -- and part_flatten's load_v2 deliberately
    // refuses an ANLK-bearing Part, because a flat is a static snapshot and a
    // skinned Part has no single static pose to snapshot.
    //
    // So the flatten was never wanted here: reporting "flatten failed" for it
    // described a job that should not have been attempted. Returning true means
    // "nothing further is required of this Part", which is what every caller
    // actually asks. Callers that go on to read the .flat.part already handle
    // its absence (they re-check is_cache_artifact_header_compatible first).
    //
    // Reachable only since the editor gained the animation bake host: while
    // clip compilation failed closed, no animated Part ever reached a provider.
    {
        std::optional<part_asset::PartAnimationLink> animation_link;
        const std::string part_abs_path =
            root + "/" + part_asset::cache_path_resolved(part_hash);
        // A present-but-invalid ANLK returns false and falls through to the
        // flatten below, which fails and reports -- corruption must stay loud.
        if (part_asset::load_animation_link(part_abs_path, part_hash, animation_link) &&
            animation_link)
            return true;
    }

    part_flatten::FlattenResult fr =
        part_flatten::flatten_part(root, part_hash);
    if (fr.ok) {
        MATTER_LOGI("local_provider", "LocalProvider: flattened %016llx (%zu clusters, %zu levels, %zu -> %zu tris, %zu instance_refs)\n",
               (unsigned long long)part_hash, fr.clusters, fr.levels,
               fr.full_tris, fr.coarsest_tris, fr.instance_refs);
        return true;
    } else {
        MATTER_LOGE("local_provider", "LocalProvider: flatten failed for %016llx: %s\n",
               (unsigned long long)part_hash, fr.error.c_str());
        return false;
    }
}

std::string LocalProvider::artifact_root(uint64_t part_hash) const {
    if (!transient_dir_.empty()) {
        const std::string scratch_part =
            transient_dir_ + "/" + part_asset::cache_path_resolved(part_hash);
        if (part_asset::is_cache_artifact_header_compatible(
                scratch_part, part_hash, part_asset::kFormatVersionV2))
            return transient_dir_;
    }
    return abs_cache_root_;
}

bool LocalProvider::compose_world(WorldManifest& out, std::string& err) {
    // Requires install_graph() to have succeeded.
    // All absolute paths (abs_*_) are set by install_graph().

    // Reset tileset count (may be called again for cone rebake).
    baked_tileset_count_ = 0;

    out.world_root_hash = 1;
    out.instances.clear();
    uint32_t next_id = 1;
    auto place = [&](uint64_t h, const matter::Mat4f& transform,
                     const std::string& mod = {}) {
        WorldManifestEntry e;
        e.instance_id = next_id++;
        e.part_hash   = h;
        e.module      = mod;
        std::memcpy(e.transform, transform.m, sizeof(e.transform));
        out.instances.push_back(e);
    };

    // Note: flatten_placed() and append_instance_refs() have moved to the publish
    // pipeline (matter_engine.cpp::publish_pipeline per-part bake+flatten+streaming).
    // compose_world() now only places roots and runs the tileset phase; per-part
    // flatten and FlatInstanceRef expansion happen on-demand in the publish loop.
    // connect() (sync API) still runs flatten+refs eagerly via its own compose path.

    // Generic placement: every non-tileset manifest root is placed at the origin, except
    // roots flagged `expand`, whose baked child-instance table is promoted to
    // individual world instances (per-child LOD, culling, and instanced
    // batching downstream). Tileset roots are handled separately below.
    for (size_t i = 0; i < roots_.size(); ++i) {
        if (tileset_flags_[i]) {
            // Handled below via run_tileset_phase; not placed as a world instance.
            continue;
        }
        // Map back to the install index for this original root.
        size_t k = 0; bool found = false;
        for (size_t j = 0; j < install_to_orig_.size(); ++j)
            if (install_to_orig_[j] == i) { k = j; found = true; break; }
        if (!found) continue;  // (unreachable — every non-tileset root was installed)
        // Task 7: skip roots that failed during install (root_hash == 0 → failed).
        if (ir_.root_hashes[k] == 0) continue;
        if (expand_flags_[i]) {
            const size_t first_expanded = out.instances.size();
            if (!append_expanded_children(artifact_root(ir_.root_hashes[k]),
                                          ir_.root_hashes[k],
                                          next_id, out.instances, err))
                return false;
            for (size_t expanded = first_expanded;
                 expanded < out.instances.size(); ++expanded) {
                matter::Mat4f relative{};
                std::memcpy(relative.m, out.instances[expanded].transform,
                            sizeof(relative.m));
                const matter::Mat4f combined =
                    multiply_transform(root_transforms_[i], relative);
                std::memcpy(out.instances[expanded].transform, combined.m,
                            sizeof(combined.m));
            }
        } else {
            place(ir_.root_hashes[k], root_transforms_[i], roots_[i].module);
        }
    }
    // Backfill module names for expanded children: append_expanded_children does
    // not know the module name of each child hash, but module_by_hash_ (built at
    // install time from graph_snapshot_.nodes) covers the full graph. Fill in any
    // empty module fields so BakePartDone events and publish-sort operations have
    // correct labels for all placed parts (roots and expanded children alike).
    for (auto& e : out.instances) {
        if (e.module.empty()) {
            auto it = module_by_hash_.find(e.part_hash);
            if (it != module_by_hash_.end()) e.module = it->second;
        }
    }

    // ---- Tileset roots: deferred (Task 15) -----------------------------------
    // Tileset roots are no longer run in compose_world. They run after BakeFinished
    // in the deferred tileset phase (publish_pipeline tail in matter_engine.cpp)
    // via run_tileset_deferred(). This removes the ~350s box3d settle wall from
    // the silhouette critical path.
    //
    // connect() (synchronous API) still runs them eagerly via run_tileset_deferred
    // called immediately after compose_world().
    //
    // Guard: fail-closed BEFORE any GL/disk work if the manifest declares more
    // tileset roots than we have sampler-array slots.
    // tileset::kMaxTilesetSlots (tileset_gtex.h) is the single source of truth
    // for the sampler-array slot count; do not re-declare a local literal here.
    if ((int)tileset_indices_.size() > tileset::kMaxTilesetSlots) {
        err = "LocalProvider: manifest declares " +
              std::to_string(tileset_indices_.size()) +
              " tileset roots but only " +
              std::to_string(tileset::kMaxTilesetSlots) +
              " slots are available";
        return false;
    }
    // tileset roots placed/slotted later; baked_tileset_count_ stays 0 until deferred phase.

    // --- Publish world lights ---
    out.lights = authored_lights_;

    return true;
}

std::vector<LocalProvider::DetailBakeRequest>
LocalProvider::collect_detail_bake_requests() const {
    // Ordering and merging rules live in detail_bake_plan.h so they can be
    // unit-tested without a provider; this member only supplies the inputs.
    std::vector<tileset::DetailBakeRoot> roots;
    roots.reserve(tileset_indices_.size());
    for (const size_t ti : tileset_indices_) {
        if (ti >= roots_.size()) continue;
        roots.push_back({roots_[ti].module, params_to_json(roots_[ti].params)});
    }
    return tileset::plan_detail_bakes(roots, world_materials_);
}

void LocalProvider::reset_tileset_bindings() {
    // Every material this provider pointed at a detail slot goes back to
    // untextured (scalar albedo), so a re-connect for a different world never
    // samples the previous world's atlas. Material 16 is unbound
    // unconditionally: the deprecated root path bound it before this
    // bookkeeping existed, and a warm restore may not have recorded it.
    MaterialRegistrySetGroundTilesetSlot(tileset::kDeprecatedTilesetRootMaterial, -1);
    for (const int material : tileset_slots_.reset())
        MaterialRegistrySetGroundTilesetSlot(material, -1);
    tileset_evict_warned_ = false;
    world_materials_.clear();
    // Contract C3: the dynamic registry tail is per-world. load_world_definition
    // resets it too (it must, to keep handles deterministic); doing it here as
    // well makes the "on world (re)connect" guarantee hold even for paths that
    // reset the provider without reloading the world source.
    MaterialRegistryResetDynamic();
}

bool LocalProvider::run_tileset_deferred(
    std::function<void(int done, int total, const char* module)> on_tileset_part,
    std::function<bool()> is_cancelled,
    std::string& err)
{
#if defined(MATTER_HAVE_SCRIPT_HOST)
    // Chart-VT spec Phase 3: the loop is no longer "every `tileset: true` root,
    // hardwired to material 16". It is "every declared detail bake" — the
    // deprecated roots plus every defineMaterial() that named a `detail`
    // module — each binding its OWN materials to the slot its atlas lands in.
    const std::vector<DetailBakeRequest> requests = collect_detail_bake_requests();
    const int total = (int)requests.size();
    if (total == 0) return true;   // nothing to do; keep traces free of empty spans

    // Bake Lab task 1.3 (docs/bake-lab.md §II.1): the tileset phase is no
    // longer inside compose_world — Task 15 moved it here, reached from
    // publish_pipeline's tail (deferred, after BakeFinished) and eagerly from
    // connect(). Spanning this function covers both paths and splits tileset
    // time out of kSpanCompose on the Timeline. Observation-only: with no
    // collector current on this thread the span is a no-op. GPU work marshaled
    // via run_gpu below runs on the app/Vulkan thread and is intentionally not
    // spanned (single-writer discipline); its wall time still lands inside this
    // span because run_gpu blocks the calling thread.
    BAKE_SPAN(bake_trace::kSpanTileset);

    // Phase B: route GPU work through gpu_run when set; fall back to inline.
    // (Was run_gl: the GL bake it used to marshal is gone — the closure now
    // carries the Vulkan bake + slot upload, spec §I.7 "Render-thread half".)
    auto run_gpu = [&](const char* name, std::function<bool(std::string&)> fn,
                       std::string& e) -> bool {
        if (cfg_.gpu_run) return cfg_.gpu_run(name, std::move(fn), e);
        return fn(e);
    };

    // sorted_child_hashes is an OUT param: the phase already sorts the root's
    // resolved child hashes for the settle cache key, and the .gtex key needs
    // the same list (see gtex_script_identity_hash below).
    auto settle_tileset = [&](const std::string& root_module,
                              const std::string& root_params_json,
                              tileset::SettledTorus& settled,
                              std::vector<uint64_t>& sorted_child_hashes,
                              std::string& settle_err) -> bool {
        sorted_child_hashes.clear();
        return tileset::run_tileset_phase_from_object_roots(
            abs_object_roots_, root_module, root_params_json, abs_cache_root_,
            settled, settle_err, abs_shared_lib_roots_, &sorted_child_hashes);
    };

    // Slot load, shared by the post-bake and cache-hit paths (spec §I.7 "Slot
    // load"): the .gtex on disk is the same artifact either way, so a fresh
    // bake and a cache hit go through one identical MaterialRegistry +
    // vk_tileset_load pair. A load failure is non-fatal — that slot's ground
    // stays untextured (matches vk_tileset_load's documented contract).
    // `materials` is the request's binding list: the deprecated `tileset: true`
    // root supplies {16}, a defineMaterial() detail supplies its own id, and a
    // detail scene shared by several materials supplies all of them.
    auto load_slot = [&](int slot, const std::string& gtex_path,
                         const std::string& root_module,
                         const std::vector<int>& materials, const char* tag) {
        for (const int material : materials)
            MaterialRegistrySetGroundTilesetSlot(material, slot);
        std::string ve;
        if (cfg_.vk_tileset_load && !cfg_.vk_tileset_load(slot, gtex_path, ve)) {
            MATTER_LOGE("local_provider",
                    "Vulkan tileset slot %d load "
                    "failed (ground stays untextured): %s\n",
                    slot, ve.c_str());
            fflush(stderr);
            return;
        }
        std::string bound;
        for (const int material : materials) {
            if (!bound.empty()) bound += ",";
            bound += std::to_string(material);
        }
        MATTER_LOGI("local_provider", "LocalProvider: tileset '%s' -> slot %d, material(s) %s (%s) [%s]\n",
               root_module.c_str(), slot, bound.c_str(), gtex_path.c_str(), tag);
    };

    // LRU acquire, shared by both arms: slots are a cache keyed by `.gtex`
    // content hash. A world with no more detail scenes than slots gets exactly
    // the assignment the old monotonic counter produced (lowest free index
    // first); beyond that the least-recently-acquired atlas is displaced and
    // its materials fall back to scalar albedo rather than sampling a slot that
    // now holds someone else's texels.
    auto acquire_slot = [&](uint64_t key, const std::string& root_module) -> int {
        const tileset::DetailSlotBinder::Acquired r = tileset_slots_.acquire(key);
        if (r.slot < 0) return -1;
        if (r.evicted) {
            for (const int material : r.unbound)
                MaterialRegistrySetGroundTilesetSlot(material, -1);
            if (!tileset_evict_warned_) {
                tileset_evict_warned_ = true;
                MATTER_LOGW("local_provider",
                        "detail-tileset slots exhausted (%d): "
                        "evicting the least-recently-used atlas for '%s'. The "
                        "displaced materials fall back to their scalar albedo. "
                        "Declare fewer detail tilesets, or share one detail "
                        "scene between materials. (warned once)\n",
                        tileset_slots_.capacity(), root_module.c_str());
                fflush(stderr);
            }
        }
        return r.slot;
    };

    for (int idx = 0; idx < total; ++idx) {
        if (is_cancelled && is_cancelled()) {
            err = "tileset deferred phase cancelled";
            return false;
        }

        const DetailBakeRequest& request = requests[(size_t)idx];
        const std::string root_module = request.module;
        const std::string root_params_json = request.params_json;

        if (on_tileset_part)
            on_tileset_part(idx, total, root_module.c_str());

        // Two arms, no preprocessor gate (spec §I.7): bake-capable when a
        // Vulkan bake callback is bound, load-only when it is not.
        const bool can_bake = static_cast<bool>(cfg_.vk_tileset_bake);

        // ---- Shared prologue (worker thread; spec §I.7 "Worker half") ------
        // Settle is required on BOTH arms: pose_hash is half the .gtex cache
        // key, so even a load-only probe has to settle first.
        if (!can_bake) {
            MATTER_LOGI("local_provider", "headless deferred tileset: '%s'\n",
                    root_module.c_str());
            fflush(stderr);
        }
        tileset::SettledTorus settled;
        std::vector<uint64_t> sorted_child_hashes;
        {
            std::string se;
            if (!settle_tileset(root_module, root_params_json, settled,
                                sorted_child_hashes, se)) {
                err = "LocalProvider: tileset '" + root_module + "' settle failed: " + se;
                return false;
            }
        }
        if (!can_bake)
            MATTER_LOGI("local_provider", "LocalProvider: tileset '%s' settle ok (deferred headless)\n",
                   root_module.c_str());

        // Script identity — the other half of the .gtex cache key.
        //
        // The root .js source bytes alone are NOT enough: pose_hash covers
        // where the children settled, so a child edit that MOVES geometry
        // invalidates the atlas, but an appearance-only child edit (recolour a
        // pebble, retune its roughness) leaves every pose bit-identical and
        // never touches the root source — the stale atlas kept being served
        // until someone wiped .cache by hand. Folding in the sorted child
        // resolved_hash list (the same list the settle cache key already
        // folds) closes that gap. See gtex_script_identity_hash's comment in
        // tileset_gtex.h.
        //
        // This value flows on to cfg_.vk_tileset_bake as well, so the bake's
        // own `expected` recomputation (tileset_bake_vk.cpp) stays in lockstep
        // with the probe below without a second signature change.
        const std::string root_js_path = resolve_object_path(root_module);
        uint64_t script_source_hash = 0;
        {
            std::ifstream jf(root_js_path, std::ios::binary);
            if (jf) {
                std::ostringstream ss; ss << jf.rdbuf();
                const std::string src = ss.str();
                script_source_hash = part_asset::fnv1a64(src.data(), src.size());
            }
            // Unreadable script -> hash 0 -> the load-only probe fails closed;
            // the bake arm simply rebakes.
        }
        script_source_hash = tileset::gtex_script_identity_hash(
            script_source_hash, sorted_child_hashes);

        // detailDensity: an authoring override on the atlas raster density. It
        // changes only how finely the settled scene is sampled, never the
        // settle itself, so it is applied after settling and folded into the
        // cache key (via the same script-identity slot) plus the artifact name
        // — two materials pointing one detail scene at different densities are
        // two different atlases and must not share a `.gtex`.
        std::string gtex_name = root_module;
        if (request.texels_per_meter > 0 &&
            request.texels_per_meter != settled.cfg.texels_per_meter) {
            settled.cfg.texels_per_meter = request.texels_per_meter;
            const std::vector<uint64_t> density_fold{
                static_cast<uint64_t>(request.texels_per_meter)};
            script_source_hash =
                tileset::gtex_script_identity_hash(script_source_hash, density_fold);
            gtex_name += "@tpm" + std::to_string(request.texels_per_meter);
        }

        const std::string gtex_path = abs_cache_root_ + "/" + gtex_name + ".gtex";
        const uint64_t expected = tileset::gtex_content_hash(
            settled.report.pose_hash, script_source_hash);

        const int slot_idx = acquire_slot(expected, root_module);
        if (slot_idx < 0) {
            err = "LocalProvider: tileset '" + root_module +
                  "': no detail-tileset slots are configured";
            return false;
        }
        tileset_slots_.bind(expected, request.materials);

        if (can_bake) {
            // ---- Bake-capable arm: Vulkan hardware-RT .gtex bake ------------
            // Marshaled to the app/Vulkan thread through gpu_run; the worker
            // blocks here, so the wall time still lands inside kSpanTileset.
            const bool dump_png = std::getenv("MATTER_TILESET_DUMP_PNG") != nullptr;
            tileset::BakeInputs bi; bi.parts_cache_dir = abs_cache_root_;
            std::string te;
            const bool ok = run_gpu(root_module.c_str(), [&](std::string& ge) -> bool {
                // No cache probe here: bake_tileset_vk probes the very same
                // `expected` content hash itself and returns true without
                // doing any work on a hit (spec §I.7), and load_slot below
                // runs either way — so a hit still loads the slot.
                std::string be;
                if (!cfg_.vk_tileset_bake(settled, script_source_hash, gtex_path,
                                          bi, dump_png, be)) {
                    ge = "vk_tileset_bake(" + root_module + "): " + be;
                    return false;
                }
                load_slot(slot_idx, gtex_path, root_module, request.materials,
                          "deferred");
                return true;
            }, te);

            if (!ok) {
                // Fatal to the tileset phase: propagates as
                // BakeError{phase="tileset"} (deferred) / fails connect() (sync).
                err = "LocalProvider: tileset '" + root_module + "': " + te;
                return false;
            }
            ++baked_tileset_count_;
        } else if (tileset::gtex_cache_hit(gtex_path, expected)) {
            // ---- Load-only arm: serve a cached atlas ------------------------
            load_slot(slot_idx, gtex_path, root_module, request.materials,
                      "headless cache hit");
            ++baked_tileset_count_;
        } else {
            // ---- Load-only arm: cache miss, non-fatal -----------------------
            // The slot stays reserved for this key (the scheduling bookkeeping
            // is what a headless run asserts) but no material is bound, so the
            // materials keep their scalar albedo.
            tileset_slots_.forget(expected);
            // Diagnostic detail: distinguish "no file" from "stale hash"
            // (a stale hash names the half that moved via the stored header).
            tileset::GTexHeader stale_hdr;
            std::vector<uint8_t> ta, tn, to_;
            std::vector<uint16_t> th;
            std::string le;
            const bool file_readable =
                tileset::load_gtex(gtex_path, stale_hdr, ta, tn, to_, th, le);
            MATTER_LOGE("local_provider",
                    "tileset '%s': no cached .gtex matches "
                    "(headless build cannot bake; ground stays untextured)\n"
                    "  probed: %s\n  expected content_hash %016llx; %s\n",
                    root_module.c_str(), gtex_path.c_str(),
                    (unsigned long long)expected,
                    file_readable
                        ? ("file has " + [&]{ char b[32]; snprintf(b, sizeof b, "%016llx",
                              (unsigned long long)stale_hdr.content_hash); return std::string(b); }() +
                           " (bake_ver " + std::to_string(stale_hdr.engine_bake_version) +
                           " box3d_ver " + std::to_string(stale_hdr.box3d_version) + ")").c_str()
                        : ("file missing/unreadable: " + le).c_str());
            fflush(stderr);
        }

        if (on_tileset_part)
            on_tileset_part(idx + 1, total, root_module.c_str());
    }
    return true;
#else
    (void)on_tileset_part;
    (void)is_cancelled;
    err = "run_tileset_deferred: MATTER_HAVE_SCRIPT_HOST not defined";
    return false;
#endif
}

bool LocalProvider::connect(WorldManifest& out, std::string& err) {
    // Sync API: keep eager behavior — BakePolicy::All bakes every node at install.
    // After compose_world (which now skips flatten/refs in the async path),
    // eagerly flatten all placed roots and expand FlatInstanceRefs here.
    if (!install_graph(err, part_graph::BakePolicy::All)) return false;
    if (!compose_world(out, err)) return false;

    // Eager flatten: every placed root gets a .flat.part so synchronous callers
    // (tests, gallery_bake, viewer_logic_tests) see the flat artifacts immediately.
    {
        std::set<uint64_t> done;
        for (const auto& e : out.instances) {
            if (!done.insert(e.part_hash).second) continue;
            ensure_part_flattened(e.part_hash);
        }
    }

    // Eager FlatInstanceRef expansion: read each placed root's .flat.part and
    // append world entries for any instance refs (BOUNDARY-path roots like
    // StressForest). Iterates to a fixed point so nested boundaries expand too.
    {
        // Order-independent max-scan, matching the sibling allocator in
        // matter_engine.cpp's publish pipeline. This read `back().instance_id
        // + 1u`, which is only the maximum while every id in `out.instances`
        // was allocated by one ascending counter in vector order -- an
        // assumption nothing states and nothing checks, and the one place in
        // the engine that would break if any id here ever stopped being an
        // allocation counter (streamed sector ids already have; they are
        // content-derived and live in the high half of the range, and they
        // reach WorldState through a WorldDelta rather than this manifest,
        // which is the only reason this line has not already produced
        // duplicate ids).
        uint32_t next_id = 1u;
        for (const auto& e : out.instances)
            if (e.instance_id >= next_id) next_id = e.instance_id + 1u;
        std::set<uint64_t> visited;
        while (true) {
            std::vector<uint64_t> to_process;
            std::set<uint64_t> seen;
            for (const auto& e : out.instances) {
                if (!seen.insert(e.part_hash).second) continue;
                if (visited.count(e.part_hash)) continue;
                to_process.push_back(e.part_hash);
            }
            if (to_process.empty()) break;
            const size_t before = out.instances.size();
            for (uint64_t ph : to_process) {
                visited.insert(ph);
                ensure_part_flattened(ph);
                const std::string flat_abs_path = artifact_root(ph) + "/" +
                    part_asset::cache_path_flat(ph);
                if (!part_asset::is_cache_artifact_header_compatible(
                        flat_abs_path, ph, part_asset::kFormatVersionFlat))
                    continue;
                BLASManager scratch_blas;
                TLASManager scratch_tlas(4);
                std::vector<part_asset::FlatCluster> clusters_ignored;
                std::vector<part_asset::FlatInstanceRef> refs;
                if (!part_asset::load_flat_v3(flat_abs_path, ph, scratch_blas,
                                              scratch_tlas, clusters_ignored, refs))
                    continue;
                if (refs.empty()) continue;
                out.instances.reserve(out.instances.size() + refs.size());
                for (const auto& r : refs) {
                    if (r.inline_cutover > 0.0f) continue;
                    WorldManifestEntry we;
                    we.instance_id = next_id++;
                    we.part_hash   = r.child_resolved_hash;
                    std::memcpy(we.transform, r.transform, sizeof(we.transform));
                    out.instances.push_back(we);
                }
            }
            if (out.instances.size() == before) break;  // fixed point
        }
    }

    // Task 15: sync API eagerly runs the deferred tileset phase (headless or GL).
    // Async path runs this after BakeFinished via publish_pipeline step 9.
    // Null callbacks: no progress reporting, no cancellation on the sync path.
    // FATAL on this sync path (pre-Task-15 behavior): connect() callers (tests,
    // gallery_bake, viewer_logic_tests) expect a fully prepared world on success.
    if (!collect_detail_bake_requests().empty()) {
        std::string te;
        if (!run_tileset_deferred(nullptr, nullptr, te)) {
            err = "LocalProvider::connect: tileset phase failed: " + te;
            return false;
        }
    }
    return true;
}

std::vector<uint64_t>
LocalProvider::reconcile(const WorldManifest& manifest, const PartStore& store) {
    // Return unique hashes that need to be fetched/loaded:
    //  - Newly baked this session (baked_hashes_): just written to disk, not yet
    //    loaded into the store's memory.
    //  - Not found on disk at all (store.has() covers both in-memory and disk):
    //    handles the case of a partially populated cache.
    std::vector<uint64_t> want;
    std::set<uint64_t> seen;
    for (const auto& e : manifest.instances) {
        if (!seen.insert(e.part_hash).second) continue;
        if (baked_hashes_.count(e.part_hash) || !store.has(e.part_hash))
            want.push_back(e.part_hash);
    }
    return want;
}

bool LocalProvider::fetch_parts(const std::vector<uint64_t>& want,
                                PartStore& store, std::string& err) {
    // LocalProvider already wrote the .part blobs to the shared cache during
    // connect()'s install; "fetching" is just loading them into the store.
    //
    // Task 7 fix: null get_or_load no longer hard-aborts (old: return false).
    // Instead, record the failure into fetch_failed_ and continue to the next
    // part. Return true if the loop completed, even with failures; callers that
    // care about partial failures inspect fetch_failed(). This matches the plan:
    // "a get_or_load null return appends to a failed list instead of returning false".
    //
    // Remaining callers of fetch_parts():
    //   - viewer_logic_tests.cpp:182  — direct unit test; now gets true-with-failures
    //     and can inspect fetch_failed() for assertions (expected by plan).
    //   - viewer_logic_tests.cpp:1268 — install_phase_progress test; same behavior.
    // The async path (execute_bake) does NOT call fetch_parts; it calls
    // store->get_or_load() directly in publish jobs. Both callers now see
    // skip-and-continue, which is the intended behavior per the plan.
    fetch_failed_.clear();
    for (size_t i = 0; i < want.size(); ++i) {
        uint64_t h = want[i];
        // Task 7: fire test_fault_hook (0-based index) with per-part exception catch.
        std::string module_name;
        {
            auto it = module_by_hash_.find(h);
            if (it != module_by_hash_.end()) module_name = it->second;
        }

        bool part_failed = false;
        try {
            if (cfg_.test_fault_hook) cfg_.test_fault_hook((int)i);
            if (!store.get_or_load(h)) {
                FetchFailed ff;
                ff.module = module_name;
                ff.error  = "load failed for part " + std::to_string(h);
                fetch_failed_.push_back(std::move(ff));
                part_failed = true;
            }
        } catch (std::bad_alloc&) {
            FetchFailed ff;
            ff.module = module_name;
            ff.error  = "std::bad_alloc loading part " + std::to_string(h);
            fetch_failed_.push_back(std::move(ff));
            part_failed = true;
        } catch (std::exception& ex) {
            FetchFailed ff;
            ff.module = module_name;
            ff.error  = ex.what();
            fetch_failed_.push_back(std::move(ff));
            part_failed = true;
        }

        if (part_failed) continue;  // skip-and-continue

        if (cfg_.on_part) {
            const char* mod = module_name.empty() ? nullptr : module_name.c_str();
            cfg_.on_part(mod, (int)(i + 1), (int)want.size());
        }
    }
    // Return true (loop completed) even with partial failures. Report empty err
    // string on partial failure — callers inspect fetch_failed() for details.
    // Return false only if there is a fatal structural failure (none currently
    // possible in this implementation).
    (void)err;  // no fatal error path in this implementation
    return true;
}

bool LocalProvider::poll_deltas(WorldDelta&) { return false; }  // static world

bool append_expanded_children(const std::string& cache_root, uint64_t root_hash,
                              uint32_t& next_id,
                              std::vector<WorldManifestEntry>& out_instances,
                              std::string& err) {
    const std::string path = cache_root + "/" + part_asset::cache_path_resolved(root_hash);
    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    if (!part_asset::load_v2(path, root_hash, blas, tlas, children, lods)) {
        err = "expand: failed to load root part " + path;
        return false;
    }
    if (children.empty()) {
        err = "expand: root has no children (nothing to expand)";
        return false;
    }
    out_instances.reserve(out_instances.size() + children.size());
    for (const auto& c : children) {
        WorldManifestEntry e;
        e.instance_id = next_id++;
        e.part_hash   = c.child_resolved_hash;
        std::memcpy(e.transform, c.transform, sizeof(e.transform));
        out_instances.push_back(e);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Phase C Task 17 — resolve cache restore hook
// ---------------------------------------------------------------------------

bool LocalProvider::restore_from_cache(
    const part_graph_snapshot::Snapshot&              snapshot,
    const std::unordered_map<uint64_t, part_graph::BakeInputs>& bake_plan,
    const std::vector<uint64_t>&                      root_hashes,
    std::string& err)
{
#if defined(MATTER_HAVE_SCRIPT_HOST)
    // Reset mutable state (mirrors the preamble of install_graph()).
    reset_tileset_bindings();
    baked_tileset_count_ = 0;
    baked_count_  = 0;
    hit_count_    = 0;
    install_bake_count_ = 0;
    baked_hashes_.clear();
    roots_.clear();
    root_transforms_.clear();
    expand_flags_.clear();
    tileset_flags_.clear();
    authored_fluid_colliders_.clear();
    roots_for_install_.clear();
    install_to_orig_.clear();
    tileset_indices_.clear();
    ir_ = part_graph::InstallResult{};
    graph_snapshot_ = part_graph_snapshot::Snapshot{};

    if (!prepare_paths(err)) return false;

    // Initialise ScriptHost + HostBaker so ensure_part_baked() can re-bake
    // individual cache-miss parts without running a global resolve.
    host_ = std::make_unique<script_host::ScriptHost>();
    host_->set_shared_lib_roots(abs_shared_lib_roots_);
    resolver_ = std::make_unique<part_graph::FileModuleResolver>(*host_, abs_object_roots_);
    host_baker_ = std::make_unique<part_graph::HostBaker>(*host_, abs_cache_root_);
    // W3: thread the optional per-rung bake observer (null in production).
    host_baker_->set_bake_observer(cfg_.bake_observer);

    // Task 2: apply transient settings to the baker (if set_transient_modules was called)
    if (!transient_modules_.empty()) {
        host_baker_->set_transient(&transient_modules_, transient_dir_);
    }

    // Reload the authored world input to populate roots/flags for the cached
    // compose and deferred-tileset paths. The cache key already validated the
    // same world source and script tiers.
    {
        std::string merr;
        if (!load_authored_world(merr)) {
            err = "restore_from_cache: failed to load world: " + merr;
            return false;
        }
        for (size_t i = 0; i < roots_.size(); ++i) {
            if (tileset_flags_[i]) tileset_indices_.push_back(i);
            else { roots_for_install_.push_back(roots_[i]); install_to_orig_.push_back(i); }
        }
        // Keep the install-root layout byte-for-byte compatible with the cold
        // path. Cached root_hashes are parallel to this complete list, including
        // entity-only PartInstance modules that are absent from World.roots.
        append_entity_part_roots();
    }

    // Apply root_params_json override (mirrors install_graph's merge step).
    if (!cfg_.root_params_json.empty()) {
        // We don't actually need to merge params into roots_ here because we're
        // not re-resolving the graph — the cached root_hashes already reflect the
        // override. We still populate roots_ for tileset phase which will re-eval
        // its own script; tileset scripts don't use root_params_json.
        (void)cfg_.root_params_json;
    }

    // Restore cache payload into ir_.
    ir_.ok          = true;
    ir_.root_hashes = root_hashes;
    ir_.bake_plan   = bake_plan;
    if (ir_.root_hashes.size() != roots_for_install_.size()) {
        err = "resolve cache stale: cached root count does not match authored roots";
        return false;
    }

    // Restore graph snapshot.
    graph_snapshot_ = snapshot;

    // Verify that all entity-referenced modules are in the cached snapshot.
    // If any are missing, the cache predates entity-part baking and must be
    // invalidated so install_graph can bake them.
    for (const auto& mod : collect_entity_part_modules(authored_entities_)) {
        if (graph_snapshot_.nodes.find(mod) == graph_snapshot_.nodes.end()) {
            err = "resolve cache stale: entity part '" + mod + "' not in snapshot";
            return false;
        }
    }

    // Build module_by_hash_ from snapshot nodes (for diagnostics / module labels).
    module_by_hash_.clear();
    for (const auto& kv : graph_snapshot_.nodes)
        module_by_hash_[kv.second.resolved_hash] = kv.second.module;

    return true;
#else
    err = "restore_from_cache: MATTER_HAVE_SCRIPT_HOST not defined";
    return false;
#endif
}

// Task 2: transient artifact routing
void LocalProvider::set_transient_modules(std::set<std::string> modules) {
    transient_modules_ = std::move(modules);

#ifdef _WIN32
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = ".";
    transient_dir_ = std::string(tmp) + "/matter_transient/" + std::to_string(::_getpid());
    std::string win_path = transient_dir_;
    for (char& c : win_path) if (c == '/') c = '\\';
    std::system(("mkdir \"" + win_path + "\" 2>nul").c_str());
#else
    pid_t pid = ::getpid();
    transient_dir_ = "/tmp/matter_transient/" + std::to_string(pid);
    std::system(("mkdir -p " + transient_dir_).c_str());
#endif

    // Note: baker and store configuration happens in install_graph() once they're created.
    // PartStore is owned at a higher level, so callers must call
    // store.set_scratch_dir(prov->transient_dir()) independently.
}

void LocalProvider::release_transient(uint64_t hash) {
    if (transient_dir_.empty()) return;  // not configured

    // Unlink .part and .flat.part from scratch
    const std::string part_path = transient_dir_ + "/" + part_asset::cache_path_resolved(hash);
    const std::string flat_path = transient_dir_ + "/" + part_asset::cache_path_flat(hash);

    std::remove(part_path.c_str());
    std::remove(flat_path.c_str());
}

} // namespace viewer
