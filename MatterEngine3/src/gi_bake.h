#pragma once

// gi_bake.h — fully baked GI lighting: per-Part-instance lightmaps.
//
// Bakes the engine's lighting contract — one directional sun with a finite
// angular diameter, a flat sky, multi-bounce diffuse interreflection over the
// material albedo (with the TriEx tint blend of material_common.glsl) and
// emission — into a texture per placed Part instance, in a chart UV set built
// with the same MeshChartingLib pipeline the renderer's chart-space VT uses
// (lod_bake::build_chart_rung). The consumer is a renderer with no GI of its
// own (three.js lightMap + aoMap), so the output is a plain HDR texture plus
// the UV set that addresses it. Full description: docs/bake-gi.md.
//
// UNITS. A texel stores irradiance / pi in linear RGB, i.e. the outgoing
// radiance of a white Lambertian receiver. Under the engine defaults a fully
// open horizontal floor reads `sun_color * cos(theta_sun) + sky_color`. That
// is what three.js multiplies the diffuse colour by, and it matches the
// engine composite (`ambient = diffuse * sky_irradiance`, `sun * ndotl`).
//
// ESTIMATOR (per texel, N samples):
//   value = 1/N * sum [ sun(x, n) + path(x, n) ]
//   sun(x, n)  = sun_color * max(0, n.l) * V(x, l),  l jittered inside the disc
//   path       : T = 1; for d = 0..bounces:
//                  w ~ cosine hemisphere(n); hit = trace(x, w)
//                  miss -> value += T * sky_color; stop
//                  hit  -> if d == bounces stop;  value += T * emission(hit);
//                          T *= albedo(hit); value += T * sun(hit); x = hit
// (emission is the hit's own radiance and is not scaled by its albedo; both
// albedo and emission colour take the TriEx tint blend the renderer uses)
// so `bounces = 0` is direct sun plus sky visibility (the occlusion term), and
// every extra bounce adds one diffuse interreflection.
//
// POST PASSES, in order, each per instance lightmap: a firefly ceiling that
// ports the gi-firefly-filtering policy (center-excluded 3x3 median/MAD,
// ceiling max(0.25, 4*median, median + 6*MAD)); an edge-aware a-trous denoise
// guided by normal, position and chart identity; and chart-bounded dilation
// that pads every chart's content into its gutter so bilinear fetches across
// chart edges never blend against empty texels.
//
// DETERMINISM. Every texel owns a splitmix64-seeded PCG stream keyed on
// (seed, part hash, instance placement, texel), and the filters are pure
// functions of the raw image, so the bytes do not depend on thread count or
// scheduling. `content_hash` over the final texels is what the determinism
// test compares, and the content-addressed cache key folds
// hash_settings/hash_lighting/hash_scene (below) with the part and placement.
//
// THREADING. bake_scene runs the tracer over Settings::threads worker threads;
// the WorldTracer is query-safe once built. The caller's BLAS entries are
// borrowed for the whole call and must outlive it.

#include "blas_manager.hpp"          // BLASManager::BLASEntry (borrowed geometry)
#include "precomp.h"                 // float3
#include "render/chart_atlas.h"      // ChartAtlasRung (chart UV mapping contract)
#include "tri.h"                     // Tri, TriEx

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gi_bake {

// Bumped whenever the estimator, filters or serialized lightmap layout change
// meaning: it is folded into every cache key, so a stale blob can never be
// mistaken for a current bake.
constexpr uint32_t kGiBakeVersion = 1;

struct Settings {
    uint32_t samples = 64;            // paths per texel (1..4096)
    uint32_t bounces = 2;             // diffuse interreflections (0..8)
    float    texel_density = 8.0f;    // lightmap texels per metre
    uint32_t seed = 0x5EEDu;
    bool     firefly_filter = true;
    bool     denoise = true;
    uint32_t dilate_texels = 4;       // seam padding rounds (chart gutter is 4)
    bool     prelit = false;          // also produce albedo x lightmap
    uint32_t threads = 0;             // 0 = std::thread::hardware_concurrency()
    // The chart density halves until a part's atlas fits this many texels.
    uint64_t max_atlas_texels = 2048ull * 2048ull;
    float    cone_deg = chart_atlas::kChartNormalConeDeg;   // chart segmentation
};

struct Lighting {
    float sun_direction[3] = {-0.45f, -0.80f, -0.35f};   // FROM the sun toward the scene
    float sun_color[3]     = {2.2f, 2.05f, 1.8f};
    float sky_color[3]     = {0.38f, 0.43f, 0.52f};      // uniform sky radiance
    float sun_angular_diameter_deg = 0.53f;              // matter/sun_angles.h default
};

// One unique part's LOD0 geometry. `entries` are BORROWED BLAS entries with
// built BVHs (the tracer intersects them); `tris`/`triex` are the
// concatenation of those entries' triangles in entry order and receive the
// chart UVs (uv0/1/2 are overwritten by bake_scene).
struct Part {
    uint64_t    hash = 0;     // resolved hash; 0 = synthetic, content-hashed instead
    std::string name;
    std::vector<const BLASManager::BLASEntry*> entries;
    std::vector<Tri>   tris;
    std::vector<TriEx> triex;
};

struct Instance {
    uint32_t    part = 0;          // index into Scene::parts
    float       transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};   // row-major, ChildInstance convention
    std::string name;
    uint32_t    manifest_id = 0;   // WorldManifestEntry::instance_id (0 for fixtures)
};

struct Scene {
    std::vector<Part>     parts;
    std::vector<Instance> instances;
};

// The chart table for one part: texel <-> part-local point mapping per
// chart_atlas.h, plus a per-triangle chart id in `Part::tris` order.
struct PartAtlas {
    chart_atlas::ChartAtlasRung rung;
    uint32_t width = 0, height = 0;
    float    texels_per_meter = 0.0f;   // effective (after the fit-halving)
    std::vector<uint32_t> tri_chart;
    bool ok = false;
};

// Coverage classes per texel.
enum : uint8_t { kTexelEmpty = 0, kTexelCovered = 1, kTexelDilated = 2 };

struct Lightmap {
    uint32_t width = 0, height = 0;
    std::vector<float>   rgb;        // 3 per texel, linear, irradiance / pi
    std::vector<uint8_t> coverage;   // kTexel* per texel
    std::vector<float>   albedo;     // 3 per texel; empty unless Settings::prelit
    uint32_t covered_texels = 0;     // kTexelCovered count (before dilation)
    uint64_t rays = 0;
    double   trace_ms = 0.0;
    bool     cache_hit = false;
    uint64_t content_hash = 0;       // fnv1a64 over rgb bytes after all passes
};

struct Progress {
    uint32_t    instance = 0;
    const char* phase = "";          // "atlas" | "raster" | "trace" | "filter"
    uint64_t    done = 0, total = 0;
};
using ProgressFn = std::function<void(const Progress&)>;

// Optional content-addressed cache. `lookup` returns true and fills `out`
// when the key is known; `store` is called once per freshly traced lightmap.
struct CacheHooks {
    std::function<bool(const std::string& key, Lightmap& out)> lookup;
    std::function<void(const std::string& key, const Lightmap& map)> store;
};

struct BakeResult {
    std::vector<PartAtlas> atlases;    // parallel to Scene::parts
    std::vector<Lightmap>  lightmaps;  // parallel to Scene::instances
    uint64_t scene_hash = 0, lighting_hash = 0, settings_hash = 0;
};

// ---- hashing (public so the cache and the tests use exactly these) --------
uint64_t fnv1a64(const void* data, size_t len, uint64_t seed = 0xcbf29ce484222325ull);
uint64_t hash_settings(const Settings& s);     // every field that changes texel values
uint64_t hash_lighting(const Lighting& l);
uint64_t hash_part(const Part& p);             // resolved hash, or content for synthetic parts
uint64_t hash_placement(const Instance& i);    // transform bits
uint64_t hash_scene(const Scene& s);           // every (part, placement): moving one moves all
// "gi<version>|part=<hex>|inst=<hex>|scene=<hex>|light=<hex>|set=<hex>";
// bake_scene appends "|atlas=<w>x<h>" (the packed layout) before using it.
std::string cache_key(const Scene& s, uint32_t instance, const Settings& st, const Lighting& l);

// ---- pipeline ---------------------------------------------------------------

// Build a part's chart atlas at the requested density (halving until
// Settings::max_atlas_texels fits) and write chart UVs into part.triex.
bool build_atlas(Part& part, const Settings& s, PartAtlas& out, std::string& err);

// The whole bake. Writes chart UVs into every part's triex, traces every
// instance, runs the post passes, and fills `out`. Returns false with `err`
// on a structural failure (no instances, an atlas that cannot be built, a
// tracer that cannot be built); per-instance problems are reported through
// the lightmap (zero coverage) rather than failing the scene.
bool bake_scene(Scene& scene, const Settings& s, const Lighting& l, BakeResult& out,
                std::string& err, const ProgressFn& progress = {},
                const CacheHooks* cache = nullptr);

// Bilinear lightmap value at a world point on an instance's surface: finds the
// part-local triangle whose chart projection contains the point and samples
// through its UVs. `out_chart` receives the chart id. False when the point is
// not on the instance's charted surface.
bool sample_at(const Scene& scene, const BakeResult& r, uint32_t instance,
               const float world_point[3], float out_rgb[3], uint32_t* out_chart = nullptr);

// Serialized lightmap (the cache blob): a versioned little-endian header, the
// rgb floats, the coverage bytes and the optional albedo floats.
std::vector<uint8_t> serialize_lightmap(const Lightmap& map);
bool deserialize_lightmap(const uint8_t* data, size_t len, Lightmap& out);

// Row-major 4x4 helpers shared with the scene loader (ChildInstance layout).
void mul_transform(const float a[16], const float b[16], float out[16]);

} // namespace gi_bake
