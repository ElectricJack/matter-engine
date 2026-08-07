#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <map>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "chart_atlas.h"   // WP-E: per-rung chart tables travel with a part
#include "frame_matrices.h"
#include "matter/draw_overrides.h"  // per-module draw overrides (GPU lane)
#include "matter/render_debug.h"
#include "gpu_matrix_pack.h"
#include "matter/lod_contract.h"
#include "matter/math_types.h"
#include "matter/world_definition.h"
#include "material_registry.h"
#include "render/dynamic_instance_slots.h"
#include "tileset_gtex.h"  // tileset::kMaxTilesetSlots (slot-count source of truth)
#include "vk_gi_contract.h"
#include "vk_animation_skinning.h"
#include "vk_animation_bounds.h"
#include "vk_draw_command.h"
#include "vk_resources.h"
#include "vk_temporal.h"
#include "vt_compositor.h"  // WP-D: tier-1 page compositor (the filler)
#include "vt_enrich.h"      // WP-H: tier-2 hemisphere AO page enrichment
#include "vt_residency.h"  // WP-E: chart-space virtual texturing runtime

namespace matter {
class VulkanDevice;
class StreamlineBridge;
enum class DlssMode : uint8_t;
struct VulkanFrame;
struct VulkanVolumetricsSettings;
struct FogSettings;
struct TilesetPomSettings;
}

namespace tileset {
struct SettledTorus;
struct BakeInputs;
}

namespace viewer {

class VkVolumetrics;

constexpr uint32_t kVkMaxLod =
    static_cast<uint32_t>(matter::kMaxSerializedLodLevels);
static_assert(kVkMaxLod == 9u,
              "update shaders_vk/cull.comp MAX_LOD with the shared capacity");

// CHART RUNGS ARE NOT LOD INDICES, and conflating the two is what issue
// 6e1b444f turned out to be.
//
// kVkMaxLod bounds the LOD index WITHIN ONE CLUSTER -- it is cull.comp's
// MAX_LOD and the stride of the command/vt_draw_slot tables. A part's chart
// rungs, by contrast, are numbered across ALL of its clusters end to end:
// VkSceneLod::chart_rung indexes lod_charts, which part_store builds one
// entry per (cluster, rung). A two-cluster tree with six rungs each has
// chart rungs 0..11.
//
// vt_slots and the demand bookkeeping were sized kVkMaxLod, so every chart
// rung >= 9 was unreachable: vt_rung_mask (a uint32, filled to
// min(lod_charts.size(), 32)) marked it ELIGIBLE, register_vt_rung then
// refused it on its own `rung >= kVkMaxLod` guard, and vt_slot_for_lod
// rejected it as out of range -- so the draw fell to the legacy flat-tint
// path permanently and silently. That was 23.2% of draw-slot lookups in
// StreamMountain, and on screen it was the pale canopies.
//
// 32 because that is the mask's width; the mask is what declares a rung
// registrable, so nothing downstream of it may be narrower.
constexpr uint32_t kVkMaxChartRung = 32u;
static_assert(kVkMaxChartRung >= kVkMaxLod,
              "a chart rung numbering is at least as wide as one cluster's");

inline uint32_t vulkan_history_token(uint64_t instance_id) {
    const uint32_t folded = static_cast<uint32_t>(instance_id) ^
                            static_cast<uint32_t>(instance_id >> 32);
    return folded != 0 ? folded : 1u;
}

// The graphics push contract shared by raster.vert and gbuffer.frag. Indirect
// mesh draws leave the override off and read DrawTransform::selected_lod, which
// cull.comp wrote. Direct draws (the skin tail) have no cull-written transform
// tail of their own, so they carry their exact selected rung in the first two
// words -- that separation is what lets two skin clusters of one instance keep
// different rungs.
//
// `wireframe_enabled` is set only when the frame is genuinely being recorded
// with the VK_POLYGON_MODE_LINE pipelines. It is what tells gbuffer.frag to
// stop shading and emit a flat, self-lit line colour, so it must never be
// raised on a device that fell back to fill -- see select_raster_pipelines,
// which is the single place that decides both at once.
struct RasterDebugPushConstants {
    uint32_t direct_lod = 0;
    uint32_t direct_lod_valid = 0;
    uint32_t lod_tint_enabled = 0;
    uint32_t wireframe_enabled = 0;
};
static_assert(sizeof(RasterDebugPushConstants) == 16,
              "raster debug push constants must remain four uint32_t words");

// Keep raster-pipeline choice atomic: an unavailable or partially created line
// variant must never produce a mixed fill/line frame, and must never leave the
// push constant claiming wireframe while filled triangles are drawn.
//
// The reference branch carried a third member here for the far-field impostor
// sidecar (a five-vertex LINE_STRIP perimeter rather than polygon-line over
// the fill quad's diagonal). There is no impostor system on this base, so that
// member and its perimeter contract are deliberately absent; add them back
// with the impostor pipeline, not before.
struct RasterPipelineSet {
    VkPipeline static_mesh = VK_NULL_HANDLE;
    VkPipeline skinned_mesh = VK_NULL_HANDLE;
};

struct RasterPipelineSelection : RasterPipelineSet {
    bool wireframe_enabled = false;
};

inline RasterPipelineSelection select_raster_pipelines(
    bool wireframe_requested, bool wireframe_available,
    const RasterPipelineSet& fill, const RasterPipelineSet& line) noexcept {
    const bool line_complete = line.static_mesh != VK_NULL_HANDLE &&
                               line.skinned_mesh != VK_NULL_HANDLE;
    if (!wireframe_requested || !wireframe_available || !line_complete)
        return {{fill.static_mesh, fill.skinned_mesh}, false};
    return {{line.static_mesh, line.skinned_mesh}, true};
}

// A line-mode copy of the fill raster state. Copying rather than mutating is
// the point: the fill pipelines' create-info aliases one rasterization struct,
// and the composite pass historically aliased the input-assembly struct next
// to it, so an in-place polygonMode/topology edit for the wireframe variants
// silently changes pipelines built afterwards.
//
// Backface culling is dropped for the line pass on purpose. Wireframe exists
// to show triangle DENSITY; hiding the far side of a shell halves the edge
// count the view is supposed to be measuring.
inline VkPipelineRasterizationStateCreateInfo make_wireframe_rasterization(
    const VkPipelineRasterizationStateCreateInfo& fill) noexcept {
    VkPipelineRasterizationStateCreateInfo line = fill;
    line.polygonMode = VK_POLYGON_MODE_LINE;
    line.cullMode = VK_CULL_MODE_NONE;
    line.lineWidth = 1.0f;
    return line;
}

inline RasterDebugPushConstants make_raster_debug_push_constants(
    uint32_t direct_lod, bool direct_lod_valid,
    matter::GeometryDebugView geometry_debug_view,
    bool wireframe_enabled) noexcept {
    return {direct_lod, direct_lod_valid ? 1u : 0u,
            geometry_debug_view == matter::GeometryDebugView::LodTint ? 1u : 0u,
            wireframe_enabled ? 1u : 0u};
}

// Emission is stored as log2(1 + strength) in the alpha channel of the
// R16G16B16A16_SFLOAT normal attachment. 15.875 is exactly representable in
// binary16 and decodes to about 60096, leaving headroom below binary16's 65504
// maximum for the remaining composite terms. Larger finite authored values
// saturate monotonically at that finite HDR strength; invalid/non-positive
// values are defined as zero.
constexpr float kVkMaxEncodedEmission = 15.875f;
inline float vulkan_encode_emission(float emission) {
    if (!(emission > 0.0f) || !std::isfinite(emission)) return 0.0f;
    return std::fmin(std::log2(1.0f + emission), kVkMaxEncodedEmission);
}

inline bool vulkan_material_uses_unsupported_texture(float packed_slot) {
    return std::isfinite(packed_slot) && packed_slot >= 0.0f;
}

namespace vk_scene_detail {
VkShaderStageFlags scene_binding_stage_flags(uint32_t binding) noexcept;
bool scene_storage_limits_supported(uint32_t max_per_stage,
                                    uint32_t max_per_set) noexcept;
bool checked_mul_to_device_size(size_t count, size_t element_size,
                                VkDeviceSize& result, const char* label,
                                std::string& error);
bool checked_grown_capacity(VkDeviceSize current, VkDeviceSize required,
                            VkDeviceSize limit, VkDeviceSize& result,
                            const char* label, std::string& error);
bool checked_dispatch_groups(uint32_t instance_count,
                             uint32_t max_clusters_per_instance,
                             uint32_t max_group_count_x, uint32_t& groups,
                             std::string& error);
bool checked_size_to_int(size_t count, int& result, const char* label,
                         std::string& error);
size_t frame_constants_size_for_test() noexcept;
VkPipelineStageFlags2 ray_depth_destination_stages(
    bool native_ray_tracing_available) noexcept;
VkPipelineStageFlags2 gbuffer_sampled_stages_for_test(
    uint32_t attachment_index, bool native_ray_tracing_available) noexcept;
}  // namespace vk_scene_detail

static_assert(sizeof(DrawCommand) == sizeof(VkDrawIndexedIndirectCommand),
              "DrawCommand must match VkDrawIndexedIndirectCommand");
static_assert(offsetof(DrawCommand, index_count) ==
              offsetof(VkDrawIndexedIndirectCommand, indexCount));
static_assert(offsetof(DrawCommand, instance_count) ==
              offsetof(VkDrawIndexedIndirectCommand, instanceCount));
static_assert(offsetof(DrawCommand, first_index) ==
              offsetof(VkDrawIndexedIndirectCommand, firstIndex));
static_assert(offsetof(DrawCommand, vertex_offset) ==
              offsetof(VkDrawIndexedIndirectCommand, vertexOffset));
static_assert(offsetof(DrawCommand, first_instance) ==
              offsetof(VkDrawIndexedIndirectCommand, firstInstance));

struct VkSceneLod {
    // first_index/index_count are part-local (into VkScenePart::indices).
    // ensure_part rebases first_index to the global index_staging_ offset.
    uint32_t first_index = 0;   // into VkScenePart::indices (part-local until ensure_part rebases)
    uint32_t index_count = 0;   // 3 × triangle count
    float threshold = 0.0f;
    // WP-E: which of the part's LOD rungs (index into VkScenePart::lod_charts
    // / lod_chart_meshes) this ladder step actually draws. A cluster's ladder
    // position is NOT the rung index in general — clusters pick their own
    // subset of meshes — so the chart table has to be named explicitly.
    // UINT32_MAX means "unknown rung", which resolves to no VT.
    uint32_t chart_rung = UINT32_MAX;
};

struct VkSceneCluster {
    matter::Float3 aabb_min{};
    matter::Float3 aabb_max{};
    float radius = 0.0f;
    std::vector<VkSceneLod> lods;
};

struct VkRasterVertex {
    matter::Float3 position{};
    matter::Float3 normal{};
    matter::Float4 tint{};
    matter::Float4 surface{};
    uint32_t material_index = UINT32_MAX;
    uint32_t pad[3]{};
    // Warp field (VT Phase 2), appended AFTER the historical 72-byte layout
    // so every pre-existing word offset (RT's manual decode reads material
    // at word 14) is unchanged; only the stride grew 72 -> 88. warp_uv is
    // the warped ground coordinate in world-anchored metres; warp_tangent
    // is the octahedral-encoded frame tangent (2 x f16); warp_scales packs
    // (su, sv) as 2 x f16 — grad u = su * T, grad v = sv * (N x T). All
    // zero (su == 0) means "no warp": the fragment shader falls back to the
    // shipped world-XZ march addressing.
    matter::Float2 warp_uv{};
    uint32_t warp_tangent = 0;
    uint32_t warp_scales = 0;
};

// WP-E (chart-space VT): the CPU-side rung mesh a VT page filler reads.
// De-interleaved on purpose — it is exactly the shape vt::VtPartContext pins,
// so the residency layer can adopt these arrays without a repack. Supplied
// only for rungs that actually have a chart table; anything missing here makes
// that rung chartless (legacy path, fail-closed).
struct VkScenePartChartMesh {
    std::vector<float> positions;        // 3 per vertex, part-local metres
    std::vector<float> normals;          // 3 per vertex
    std::vector<float> surface_uvs;      // 2 per vertex, chart UV in [0,1]
    std::vector<uint32_t> material_ids;  // 1 per vertex
    std::vector<uint32_t> indices;       // 3 per triangle, mesh-local
    uint32_t vertex_count = 0;
    uint32_t dominant_material = 0xFFFFFFFFu;
    // WP-F (surfaces() tape): per-vertex u8 weight columns over the part's
    // VkScenePart::surface_materials — vertex_count * material-count bytes,
    // CPU-evaluated by the engine's compiled tape (see vt_types.h appends).
    // Empty = no tape for this rung (TriEx materialId weights).
    std::vector<uint8_t> surface_weights;
    // P2 (texel-rate tape): per-vertex f16 field-derived lane values for the
    // GPU interpreter (vertex_count * surface_lane_count halves, in
    // vt_surface_tape.h's canonical lane-scan order). Empty/0 = the tape
    // reads no field inputs (or the part carries no tape text).
    std::vector<uint16_t> surface_lanes;
    uint32_t surface_lane_count = 0;
};

// M2.5: one cluster's terminal impostor, carried from the part store to the
// renderer. `mesh_index` is the part-local rung mesh whose two triangles are
// the billboard -- the renderer patches their vertex tint with the atlas slot
// it assigns, which is the only thing about the impostor that cannot be known
// until upload time.
struct VkScenePartImpostor {
    uint32_t cluster = 0;
    uint32_t ordinal = 0;   // this impostor's index within the part
    std::vector<uint8_t> atlas;   // impostor::kAtlasBytes: shade layer, tint layer
};

struct VkScenePart {
    uint64_t part_hash = 0;
    std::vector<VkSceneCluster> clusters;
    std::vector<VkRasterVertex> vertices;   // unique vertices, all LOD meshes concatenated
    // Index buffer: values are PART-LOCAL (already include each mesh's vertex
    // offset within the part).  Global vertex rebase is done by vertexOffset /
    // base addresses (Tasks 4-5); ensure_part never rewrites these values.
    std::vector<uint32_t> indices;
    // WP-E: per-rung chart tables and their meshes, parallel to the part's LOD
    // ladder (index = rung). Empty (or an empty entry) means charts = 0 for
    // that rung, which is the legacy chartless render path.
    std::vector<chart_atlas::ChartAtlasRung> lod_charts;
    std::vector<VkScenePartChartMesh> lod_chart_meshes;
    // Packed material table the VT page filler shades from, exactly as
    // MaterialRegistryPackForGPU writes it (chart_material_stride floats per
    // record). Supplied by the caller rather than pulled from the global
    // registry so the renderer keeps no hidden dependency on it — and so a
    // test fixture can hand over a synthetic table. Empty = neutral albedo.
    std::vector<float> chart_material_table;
    uint32_t chart_material_stride = 0;
    // WP-F (surfaces() tape): the tape's declared material registry ids —
    // the columns of every rung's VkScenePartChartMesh::surface_weights —
    // and the compiled tape's content hash (folds into VT page invalidation:
    // update_vt_part_surface with a different hash re-fills page content).
    // Empty = the part carries no tape classification.
    std::vector<uint32_t> surface_materials;
    uint64_t surface_tape_hash = 0;
    // P2 (texel-rate tape): the canonical surfaces() program text (the bytes
    // surface_tape_hash hashes), the world-anchored verdict for this part and
    // its row-major 4x3 local->world. Threaded into vt::VtPartContext so the
    // compositor can pack the GPU tape (weight-seam mode 3). Empty text keeps
    // every rung on mode 2 exactly as before.
    std::string surface_tape_text;
    uint32_t surface_world_anchored = 0;
    float surface_local_to_world[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    // Demand-driven VT (the streamed-world default): bit r set = rung r has a
    // chart table, but the payload above (lod_charts / lod_chart_meshes /
    // chart_material_table) is deliberately NOT populated. register_vt_part
    // records the mask and registers nothing; variants materialize later
    // through register_vt_rung() when the demand pass wants a rung on screen,
    // rebuilt from data the part store still owns. Zero (with populated
    // payloads) is the eager path: every chart rung registers at ensure_part,
    // exactly the pre-demand behaviour the smoke fixtures drive.
    uint32_t vt_deferred_rung_mask = 0;
    // M2.5 terminal impostors, one per cluster that earned one. Empty for
    // every part whose ladder bottoms out above the impostor tier. LAST in the
    // struct on purpose: the Vulkan smoke fixtures build VkScenePart with
    // positional aggregate initialisers, so a field inserted anywhere earlier
    // silently re-binds their arguments.
    std::vector<VkScenePartImpostor> impostors;
};

// Demand-driven VT: one wanted-but-unregistered (part, rung), surfaced by the
// renderer's per-frame demand pass and drained by the engine, which rebuilds
// the atlas + context from the part store and answers with register_vt_rung().
// `priority` is the largest projected size that wanted the rung this frame —
// the drained list arrives sorted by it, biggest on screen first.
struct VtRungRequest {
    uint64_t part_hash = 0;
    uint32_t rung = 0;
    float priority = 0.0f;
};

namespace vk_scene_detail {
struct RtGeometrySelection {
    uint32_t cluster_index = 0;
    uint32_t lod_index = 0;
    uint32_t first_index = 0;   // part-local index into VkScenePart::indices
    uint32_t index_count = 0;   // 3 × triangle count
};

uint32_t select_scene_cluster_lod(const VkSceneCluster& cluster,
                                  const matter::Mat4f& object_to_world,
                                  matter::Float3 camera_eye,
                                  float pixel_budget) noexcept;
// cull.comp's `distance_to_eye`: camera to the cluster AABB's world centre,
// floored at 1 cm. Exposed because the shader asks TWO questions of that one
// value -- the max_draw_distance reject and the rung choice -- and the CPU
// mirrors must not grow a second way to compute it.
float cluster_distance_to_eye(const matter::Float3& aabb_min,
                              const matter::Float3& aabb_max,
                              const matter::Mat4f& object_to_world,
                              matter::Float3 camera_eye) noexcept;
// `override_lod_bias` is PartDrawOverride::lod_bias for the owning part slot
// (1 = neutral). It multiplies the finished reach, exactly where and how
// cull.comp applies it -- see the note in the definition for why it is not
// folded into pixel_budget instead.
uint32_t select_cluster_lod_view(const matter::Float3& aabb_min,
                                 const matter::Float3& aabb_max,
                                 float radius, const float* switch_distances,
                                 uint32_t lod_count,
                                 const matter::Mat4f& object_to_world,
                                 matter::Float3 camera_eye,
                                 float pixel_budget,
                                 float override_lod_bias = 1.0f) noexcept;
std::vector<uint32_t> dense_rt_lod_offsets(const VkScenePart& part);
bool dense_rt_lod_index(const std::vector<uint32_t>& offsets,
                        uint32_t cluster_index, uint32_t lod_index,
                        uint32_t& record_index) noexcept;

// M2.5 — is this ladder rung the terminal billboard rather than a mesh?
// The two triangles carry impostor::kQuadMarker in surface.x (the same
// sentinel raster.vert and gbuffer.frag branch on, and the same one
// adopt_part_impostors patches by), so the answer is read off the geometry
// the renderer already holds rather than a parallel table that could drift.
bool lod_is_billboard(const VkScenePart& part, const VkSceneLod& lod) noexcept;

// How many LEADING rungs of a cluster's ladder are real meshes -- the ladder
// length with any trailing billboard rung removed.
//
// This is the RT lane's CUTOFF. The billboard is oriented in the VERTEX stage,
// so its BLAS holds the quad UNROTATED: raster draws a camera-facing card
// while a ray hits a fixed rectangle, and every traced consumer (shadows, GI,
// reflections) sees a shape the object does not have. No amount of alpha
// testing repairs that, because a camera-facing card cannot present the
// light's silhouette.
//
// A cluster whose selected rung is at or past this count contributes NO traced
// geometry -- see the measurement in build_ray_geometry for why the traced rung
// is not merely clamped down to the last mesh rung instead.
//
// Returns 0 for a cluster with no mesh rung at all, which the same rule
// covers: nothing is traced.
uint32_t cluster_mesh_lod_count(const VkScenePart& part,
                                uint32_t cluster_index) noexcept;
// TEST-ONLY mirror of build_ray_geometry's per-cluster rule, over a
// VkScenePart rather than the renderer's staged GPU tables. It has no owner
// and therefore no part slot, so it cannot see the per-module draw overrides
// (max_draw_distance / lod_bias) that build_ray_geometry now applies: it
// models the NEUTRAL case only, which is the case its assertions are about.
// The production RT lane is build_ray_geometry; do not grow a second one here.
std::vector<RtGeometrySelection> select_rt_instance_geometry(
    const VkScenePart& part, const matter::Mat4f& object_to_world,
    matter::Float3 camera_eye, float pixel_budget);
}  // namespace vk_scene_detail

struct VkSceneInstance {
    uint64_t part_hash = 0;
    matter::Mat4f object_to_world{};
    // Stable identity within the owning world session. Zero selects the
    // renderer's deterministic input-order fallback for legacy callers.
    uint64_t instance_id = 0;
    // C3: dynamic animation bounds are keyed by the generational dynamic
    // instance slot. UINT32_MAX retains the immutable/static culling path.
    uint32_t animation_instance_slot = UINT32_MAX;
};

struct VkCullStats {
    uint32_t frustum_culled = 0;
    uint32_t hiz_culled = 0;
    uint32_t emitted = 0;
    uint32_t overflowed = 0;
    uint32_t triangles = 0;
    uint32_t batches = 0;
};

struct VkRasterAttachment {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

struct VkRasterAttachments {
    VkRasterAttachment albedo{};
    VkRasterAttachment normal{};
    VkRasterAttachment orm{};
    VkRasterAttachment velocity{};
    VkRasterAttachment material_instance{};
    VkRasterAttachment depth{};
    // R16G16B16A16_SFLOAT is the explicit linear HDR composite format.
    VkRasterAttachment hdr{};
    VkExtent2D extent{};
};

struct VkRasterPixel {
    matter::Float4 albedo{};
    matter::Float4 normal{};
    matter::Float4 orm{};
    matter::Float3 velocity{};
    // The identity attachment's .x, with gbuffer.frag's impostor bit already
    // MASKED OFF -- the same contract every GPU reader honours (see
    // shaders_vk/impostor_common.glsl). Every existing assertion of the form
    // `pixel.material_index == kMaterial` therefore keeps meaning what it
    // said, and an impostor pixel is identified by `impostor` below rather
    // than by a material index that suddenly compares unequal to itself.
    uint32_t material_index = UINT32_MAX;
    // Bit 31 of that same word: this fragment is an impostor card, not a
    // surface. rt_shadow.rgen forces its sun visibility to 1.
    bool impostor = false;
    uint32_t instance_token = UINT32_MAX;
    matter::Float4 hdr{};
    float depth = 1.0f;
    matter::Float3 visibility{1.0f, 1.0f, 1.0f};
    matter::Float4 raw_diffuse{};
    matter::Float4 accumulated_diffuse{};
    matter::Float4 raw_specular{};
    matter::Float4 accumulated_specular{};
    // RT PBR Phase 1: raw + denoised transmission (rgb radiance, a coverage)
    // and the transmission aux lane (x = hit_t, y = roughness).
    matter::Float4 raw_transmission{};
    matter::Float4 accumulated_transmission{};
    matter::Float3 transmission_aux{};
};

#ifdef MATTER_VK_TEST_FAULT_INJECTION
    // What the last recorded raster pass ACTUALLY bound, as distinct from
    // what was requested. `wireframe_enabled` here is the push-constant word,
    // so a test can prove the shader flag and the polygon mode agree.
    struct RasterPipelineDrawDebug {
        bool wireframe_enabled = false;
        bool static_mesh_wireframe = false;
        bool skinned_mesh_wireframe = false;
    };
    struct RtGeometryDebugRecord {
        uint64_t part_hash = 0;
        uint32_t cluster_index = 0;
        uint32_t lod_index = 0;
        uint32_t custom_index = 0;
        uint32_t first_index = 0;
        uint32_t index_count = 0;
        VkDeviceAddress vertex_address = 0;
        VkDeviceAddress blas_address = 0;
        bool opaque = false;
        bool built_this_frame = false;
    };
struct RtTraceCounters {
    uint32_t invalid_part_records = 0;
    uint32_t any_hit_invocations = 0;
    uint32_t any_hit_layers = 0;
    uint32_t capped_rays = 0;
};
static_assert(sizeof(RtTraceCounters) == 16);

constexpr uint32_t kRtSurfaceValid = 1u;
constexpr uint32_t kRtSurfaceFrontFace = 2u;
struct RtSurfaceHit {
    bool valid = false;
    uint32_t part_slot = UINT32_MAX;
    uint32_t primitive = UINT32_MAX;
    uint32_t material_index = UINT32_MAX;
    matter::Float3 position{};
    matter::Float3 normal{};
    matter::Float4 tint{};
    float uv[2]{};
    float baked_ao = 1.0f;
    float hit_t = 0.0f;
    uint32_t flags = 0;
    // --- WP-G (chart VT + ray cones), from rt_surface_test.rgen words 20-28 -
    uint32_t vt_slot = 0;            // 0 = the hit rung carries no chart table
    bool vt_applied = false;         // a VT page actually resolved and sampled
    matter::Float3 vt_albedo{};      // tinted VT albedo at the hit
    float vt_desired_mip = 0.0f;     // virtual mip the ray cone asked for
    float vt_mapped_mip = 0.0f;      // mip that was actually resident
    float cone_width = 0.0f;         // world-space cone footprint at the hit
    float uv_density = 0.0f;         // atlas UV per metre across the hit triangle
};

struct GiTemporalGpuFixture {
    uint32_t signal_mode = 0;
    matter::Float4 raw{0.25f, 0.5f, 0.75f, 1.0f};
    matter::Float3 raw_aux{1.0f, 0.5f, 0.0f};
    matter::Float3 velocity{};
    float depth = 0.5f;
    matter::Float4 normal{0.0f, 0.0f, 1.0f, 0.0f};
    uint32_t material_index = 7;
    uint32_t instance_token = 41;
    matter::Float4 previous_radiance{0.25f, 0.5f, 0.75f, 1.0f};
    matter::Float3 previous_moments{};
    uint32_t previous_history_length = 3;
    float previous_depth = 0.5f;
    matter::Float4 previous_normal{0.0f, 0.0f, 1.0f, 0.0f};
    uint32_t previous_material_index = 7;
    uint32_t previous_instance_token = 41;
    matter::Float3 previous_aux{1.0f, 0.5f, 0.0f};
    GiPixelCoord output_pixel{3, 3};
    GiPixelCoord history_patch_pixel{3, 3};
    bool reset = false;
};

struct GiTemporalGpuResult {
    matter::Float4 radiance{};
    matter::Float3 moments{};
    uint32_t history_length = 0;
    uint32_t rejection_bits = 0;
    matter::Float3 aux{};
};

struct GiAtrousGpuFixture {
    VkExtent2D extent{9, 9};
    std::vector<matter::Float4> signal;
    std::vector<matter::Float3> moments;
    std::vector<float> depth;
    std::vector<matter::Float4> normal;
    std::vector<uint32_t> material_index;
    std::vector<uint32_t> history_length;
};

struct GiAtrousGpuResult {
    std::vector<matter::Float4> filtered;
    std::vector<matter::Float4> penultimate;
    std::array<uint32_t, 5> gpu_step_widths{};
};

// Test-only direct execution fixture for the production C2 skin pipeline.
// Unlike the CPU oracle it binds VkSceneRenderer's actual descriptor layout
// and compute pipeline, then reads both deformation streams back after the
// same compute-to-consumer barrier the raster path records.
struct VkAnimationSkinGpuFixture {
    std::vector<VkSkinSourceVertex> source;
    std::vector<VkSkinInfluence> influences;
    std::vector<VkSkinJoint> current_palette;
    std::vector<VkSkinJoint> previous_palette;
    std::vector<VkSkinWorkItem> work;
};

struct VkAnimationSkinGpuResult {
    std::vector<VkSkinVertex> current;
    std::vector<VkSkinVertex> previous;
};
#endif

struct VkSceneLighting {
    // Direction from the sun toward the scene, matching WorldLights.
    matter::Float3 sun_direction{-0.45f, -0.80f, -0.35f};
    float sun_intensity = 1.0f;
    matter::Float3 sun_color{2.2f, 2.05f, 1.8f};
    float diffuse_rt_multiplier = 0.0f;
    matter::Float3 sky_color{0.38f, 0.43f, 0.52f};
    float emission_multiplier = 1.0f;
    float debug_view = 0.0f;
    float camera_fwd_x = 0.0f;
    float camera_fwd_y = 0.0f;
    float camera_fwd_z = -1.0f;
    float tan_half_fov = 1.0f;
    float aspect_ratio = 1.0f;
    float jitter_offset_u = 0.0f;
    float jitter_offset_v = 0.0f;
    float vol_enabled = 0.0f;
    float vol_debug_view = 0.0f;
    float camera_near = 1.0f;
    float camera_far = 5000.0f;
    float camera_y = 0.0f;
    float vol_cloud_top = 0.0f;
    float vol_height_layer = 0.0f;
    // Angular DIAMETER of the sun in degrees — the one authored/edited number
    // behind the sky disc, the RT reflection prefilter and the shadow cone.
    // matter/sun_angles.h owns the calibration; the default reproduces the
    // magic constants those three used to carry, exactly.
    float sun_angular_diameter_deg = matter::kSunAngularDiameterDefaultDeg;
    // DERIVED from the field above by set_lighting — do not assign by hand.
    // cos of the disc's outer (fully faded) and inner (full brightness) radii,
    // in the order smoothstep wants: smoothstep(cos_edge, cos_core, dot()).
    // Computed on the CPU because a GPU cos() is only good to a few ULP and a
    // few ULP at 0.9997 moves the visible edge of the disc.
    float sun_disc_cos_edge = 0.99975f;
    float sun_disc_cos_core = 0.99995f;
};
// Two floats longer than the 112 this was (vol_pad became a real field and two
// more followed): the composite push constant grew with the sun-size fields
// above. composite.frag's SceneLighting block must match member for member.
static_assert(sizeof(VkSceneLighting) == 120);

struct VkSceneUploadCounters {
    uint64_t vertex_uploads = 0;
    uint64_t cluster_uploads = 0;
    uint64_t instance_uploads = 0;
    uint64_t command_uploads = 0;
    uint64_t command_layout_rebuilds = 0;
    // PER-FRAME, unlike every cumulative counter around them: what the M6.5
    // caster harvest cost last frame and how many receivers it serviced.
    // Cumulative totals cannot show a spike, which is the only thing these
    // two exist to show.
    uint64_t caster_harvest_us = 0;
    uint32_t caster_harvest_receivers = 0;
    // How the cluster/vertex/index staging reached the GPU: a full pass
    // recreates the buffers and rewrites every byte (O(world)); an append
    // writes only the staging tail past the already-uploaded counts
    // (O(new part)). Streaming publishes must take the append path — a full
    // count that climbs with resident parts is the O(N^2) load regression.
    uint64_t static_full_uploads = 0;
    uint64_t static_append_uploads = 0;
};

struct PartCommandRange {
    uint32_t first_command = 0;
    uint32_t command_count = 0;
    uint32_t part_slot = 0;
};

class VkSceneRenderer {
public:
    struct RtInstance {
        uint64_t part_hash = 0;
        float transform[16]{};
        // Animated instances must pick their LOD from the same dynamic joint
        // bounds the raster lanes use, not from the part's static cluster
        // AABB — those two metrics disagree for a deforming mesh, and the
        // disagreement is directly visible: the gbuffer draws one rung while
        // the tracer traces another, and the two surfaces interpenetrate.
        // UINT32_MAX means "static instance, use the cluster AABB".
        uint32_t animation_instance_slot = UINT32_MAX;
        uint32_t animation_instance_generation = 0;
    };

    explicit VkSceneRenderer(matter::VulkanDevice& vulkan);
    ~VkSceneRenderer();
    VkSceneRenderer(const VkSceneRenderer&) = delete;
    VkSceneRenderer& operator=(const VkSceneRenderer&) = delete;

    bool init(std::string& error);
    int ensure_part(const VkScenePart& part, std::string& error);

    // M2.5: how many terminal impostors currently hold an atlas slot. On the
    // editor stats overlay so "are any drawing?" is answerable at a glance --
    // the single question the abandoned branch could not answer for a whole
    // generation of artifacts.
    uint32_t resident_impostor_count() const { return impostor_resident_; }
    // Immutable renderer-global raster range for an already registered part.
    // Animation uses this after ensure_part() so skin work references the same
    // source vertex/index arenas as the conservative bind-pose draw.
    bool part_raster_range(uint64_t part_hash,
                           uint32_t& vertex_start,
                           uint32_t& vertex_count,
                           uint32_t& index_start,
                           uint32_t& index_count) const noexcept;

    // Mirrors ensure_part()'s early-out so callers can skip the (expensive)
    // CPU-side VkScenePart construction for parts the renderer already holds.
    // Returns the registered slot, or -1 when the hash is unknown *or* the
    // renderer is poisoned — a poisoned renderer must keep failing closed
    // through ensure_part(), so -1 sends the caller down the normal path.
    // slot_of_ is the authoritative registration map (release_part erases from
    // it), so this query can never go stale.
    int registered_part_slot(uint64_t part_hash) const {
        if (poisoned()) return -1;
        const auto found = slot_of_.find(part_hash);
        return found != slot_of_.end() ? found->second : -1;
    }
    bool update_materials(const std::vector<MaterialGpuRecord>& records,
                          uint64_t shading_revision,
                          uint64_t geometry_revision, std::string& error);
    // Phase 1 tileset Vulkan port (Task 6): loads a .gtex ground tileset atlas
    // (tileset_gtex.h) into slot [0,4), CPU-slices it into 16-layer texture
    // arrays with full per-layer mip chains (tileset_slicer.h), uploads them,
    // and (re)writes the shared TilesetParams UBO plus both descriptor sets
    // (raster set 1 bindings 6/7, RT set 0 bindings 15/16). Fails closed: on
    // any error the slot's previous contents (if any) are left untouched and
    // false is returned with error set — ground rendering for that slot
    // degrades to untextured, it never crashes and never leaves a
    // partially-uploaded image bound. Safe to call before/without a prior
    // init(); returns false if the renderer has not initialized Vulkan
    // resources yet.
    bool load_tileset_slot(int slot, const std::string& gtex_path,
                           std::string& error);
    // Frees slot's GPU images (if loaded) and rewrites descriptors across all
    // frame slots to point back at the shared dummy images. No-op if the slot
    // was never loaded or slot is out of range.
    void unload_tileset_slot(int slot);
    // Vulkan hardware-RT .gtex bake (spec vulkan-rt-gtex-bake.md, milestone V1).
    // Thin passthrough to tileset::bake_tileset_vk (Q1): hands the bake the
    // renderer's Vulkan device so it can build a bake-only acceleration
    // structure and dispatch the primary compute pass. (The V1 milestone
    // emitted only the four core channels with an AO=1.0 placeholder and no
    // horizon; V2 added the AO pass and V3 the horizon channels, so the bake
    // now writes the full six-channel atlas. kEngineBakeVersion is 4 — see
    // tileset_gtex.h.) Fails closed: false + err on any error or when the
    // device lacks ray-tracing support.
    bool bake_tileset(const tileset::SettledTorus& settled,
                      uint64_t script_source_hash,
                      const std::string& gtex_path,
                      const tileset::BakeInputs& inputs, bool force_rebake,
                      bool dump_png, std::string& error);
    bool consume_gi_history_reset();
    // WP-E: pool occupancy / fills / evictions for the FrameStats channel.
    // All-zero when the VT runtime never started (no chart-bearing part).
    vt::VtResidency::Stats vt_stats() const {
        return vt_ ? vt_->stats() : vt::VtResidency::Stats{};
    }
    bool vt_active() const { return vt_ && vt_->available(); }
    // WP-E frame hooks. Public only so the file-local raster recorder in
    // vk_scene_renderer.cpp can reach them; not part of the app-facing API.
    // pre  = pool/indirection transitions, bounded page fills, feedback clear
    //        (recorded BEFORE vkCmdBeginRendering — they are transfers).
    // post = feedback image -> host-visible readback (AFTER vkCmdEndRendering).
    void vt_record_pre_pass(VkCommandBuffer command_buffer);
    void vt_record_post_pass(VkCommandBuffer command_buffer);
    bool rt_geometry_classification_dirty(uint64_t part_hash) const;
    void release_part(uint64_t part_hash);
    bool update_instances(const std::vector<VkSceneInstance>& instances,
                          std::string& error);
    // Dynamic lane (Task 7): consumes CPU-side slot changes produced by
    // matter::render::DynamicInstanceSlots. submit_serial identifies the GPU
    // frame this batch of changes will be submitted with; finish_dynamic_frame
    // reports back the highest completed serial so callers can safely reuse
    // retired slots.
    bool update_dynamic_instances(const matter::render::DynamicSlotChange* changes,
                                  uint32_t count,
                                  uint64_t submit_serial,
                                  std::string& error);
    void finish_dynamic_frame(uint64_t completed_serial);
    // Phase C1 CPU staging seam. C2 consumes these exact arenas for the
    // compute dispatch; no submission reaches the Vulkan command stream yet.
    bool register_animation_skin_asset(
        uint64_t asset_key, const std::vector<VkSkinInfluence>& influences);
    bool begin_animation_skinning_frame(uint32_t frame_slot,
                                        uint64_t completed_fence);
    bool submit_visible_animation_skinning(
        uint32_t frame_slot, const std::vector<VkSkinSubmission>& visible,
        const FrameMatrices& matrices, matter::Float3 camera_eye,
        float pixel_budget,
        const std::vector<VkAnimationBoundsInstance>& rejected_bounds = {});
    bool finish_animation_skinning_frame(uint32_t frame_slot, uint64_t fence);
    const VkAnimationSkinning& animation_skinning() const noexcept {
        return animation_skinning_;
    }
    // Renderer diagnostics are a view of the central animation budget; no
    // duplicate counters or renderer-specific limits are maintained here.
    matter::animation::AnimationBudgetRuntimeStats animation_runtime_stats(
        const matter::animation::AnimationBudgetRuntimeStats& runtime = {}) const noexcept {
        auto result = runtime;
        result.merge(animation_skinning_.stats());
        return result;
    }
    bool skinned_rt_uses_bind_pose_blas() const noexcept;
    const std::vector<VkSkinFallback>& consumed_animation_skin_fallbacks() const noexcept {
        return consumed_animation_skin_fallbacks_;
    }
    bool register_animation_bounds_asset(const VkAnimationBoundsAsset& asset);
    bool update_animation_bounds(uint32_t instance_slot,
                                 uint32_t instance_generation,
                                 uint64_t asset_key,
                                 const VkSkinPose& pose, bool history_valid);
    bool unregister_animation_bounds_asset(uint64_t asset_key);
    const VkAnimationBounds& animation_bounds() const noexcept {
        return animation_bounds_;
    }
    // Also folds the incoming history into the unchanged-input fast path of
    // update_instances(): see temporal_history_changed_.
    void set_temporal_frame(const TemporalFrame& frame);
    void set_dlss_mode(matter::DlssMode mode);
    VkExtent2D dlss_internal_extent(VkExtent2D output_extent) const;
    matter::DlssMode selected_dlss_mode() const { return selected_dlss_mode_; }
    matter::DlssMode active_dlss_mode() const;
    const std::string& dlss_reason() const;
    uint64_t dlss_reset_count() const { return dlss_reset_count_; }
    bool rt_available_observed() const { return last_rt_available_; }
    bool rt_effective_observed() const { return last_rt_effective_; }
    uint32_t rt_trace_dispatches_observed() const {
        return last_rt_trace_dispatches_;
    }
    uint32_t rt_samples_observed() const { return last_rt_samples_; }
    bool rt_debug_view_observed() const { return last_rt_debug_view_; }
    const std::string& rt_fallback_reason_observed() const {
        return last_rt_fallback_reason_;
    }
    // Lifetime totals of full TLAS rebuilds vs. frames that reused a slot's
    // already-built TLAS. Observation only — nothing branches on these.
    uint64_t rt_tlas_build_count() const { return rt_tlas_builds_; }
    uint64_t rt_tlas_reuse_count() const { return rt_tlas_reuses_; }
    bool consume_dlss_history_reset();
    bool prepare_frame(const matter::VulkanFrame& frame,
                       const FrameMatrices& matrices,
                       matter::Float3 camera_eye, float pixel_budget,
                       std::string& error);
    bool record_cull_and_render(const matter::VulkanFrame& frame,
                                const FrameMatrices& matrices,
                                matter::Float3 camera_eye,
                                float pixel_budget, std::string& error);
    bool record_ray_traced_shadows(const matter::VulkanFrame& frame,
                                   const FrameMatrices& matrices,
                                   matter::Float3 camera_eye,
                                   float pixel_budget,
                                   VkExtent2D trace_extent,
                                   std::string& error);
    void finish_ray_tracing_frame(uint64_t frame_serial, bool succeeded);
    const std::vector<PartCommandRange>& test_recorded_draw_ranges() const {
        return recorded_draw_ranges_;
    }
    VkSceneUploadCounters upload_counters() const noexcept {
        return upload_counters_;
    }
    VkCullStats cached_cull_stats() const noexcept { return cached_stats_; }
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    const std::vector<RtGeometryDebugRecord>&
    test_last_rt_geometry_records() const {
        return test_last_rt_geometry_records_;
    }
    uint32_t test_last_rt_blas_build_count() const {
        return test_last_rt_blas_build_count_;
    }
    RasterPipelineDrawDebug test_last_raster_pipeline_draw() const noexcept {
        return test_last_raster_pipeline_draw_;
    }
    void set_test_dlss_bridge(matter::StreamlineBridge bridge);
    bool test_uses_device_streamline_bridge() const;
    VkImage test_dlss_output_image(uint32_t frame_slot) const {
        return frame_slot < frames_.size()
                   ? frames_[frame_slot].dlss_output.image
                   : VK_NULL_HANDLE;
    }
    std::weak_ptr<void> test_dlss_output_lifetime(uint32_t frame_slot) const;
    bool test_replace_dlss_output(uint32_t frame_slot, VkExtent2D extent,
                                  std::string& error);
    // Immediate submit/readback diagnostics are intentionally test-only. The
    // production path records through record_cull_and_render above.
    bool dispatch_culling(const FrameMatrices& frame, matter::Float3 camera_eye,
                          float pixel_budget, std::string& error);
    bool cull_stats(VkCullStats& stats, std::string& error);
    bool readback_commands(std::vector<DrawCommand>& commands,
                           std::string& error);
    bool readback_draw_transforms(std::vector<GpuMat4>& transforms,
                                  std::string& error);
    bool render_gbuffer_and_composite(uint32_t width, uint32_t height,
                                      std::string& error);
#endif
    // Per-module draw overrides, already resolved to part CONTENT HASHES by
    // DrawOverrideResolver (matter/draw_overrides.h). The renderer's own job is
    // the last hop: hash -> dense part_slot, which is the only part identity
    // cull.comp has. `entries` must be sorted ascending by part_hash.
    //
    // An EMPTY vector is the default and restores the neutral one-entry table,
    // which the shader treats as a no-op — so a process that never calls this
    // (headless tests, replay, the Part Workbench isolation session) produces
    // byte-identical culling to the pre-override renderer.
    //
    // `hide` is NOT part of this lane: it is enforced upstream, where the
    // instance list is expanded, so a hidden module leaves the ray-tracing TLAS
    // as well as the raster instance buffer.
    void set_part_draw_overrides(
        const std::vector<matter::PartDrawOverrideEntry>& entries);
    void set_lighting(const VkSceneLighting& lighting);
    void set_display_exposure(float exposure_ev);
    void set_composite_debug_view(float mode) { composite_debug_override_ = mode; }
    void set_geometry_debug_view(matter::GeometryDebugView view);
    void set_wireframe(bool enabled);
    // True only when the device enabled fillModeNonSolid AND both line
    // pipelines were created. Callers that surface a wireframe control must
    // ask this rather than assume; a false here is the honest "cannot".
    bool wireframe_available() const noexcept;
    void set_ray_tracing_settings(
        const matter::VulkanRayTracingSettings& settings);
    void set_gi_settings(const matter::VulkanGiSettings& settings) {
        gi_settings_ = settings;
        gi_settings_.max_bounces = 1u;
        gi_settings_.samples_per_pixel = 1u;
        gi_settings_.trace_scale = std::max(0.125f, std::min(settings.trace_scale, 1.0f));
    }
    void set_volumetrics_settings(const matter::VulkanVolumetricsSettings& s,
                                  const matter::FogSettings& fog);
    // Ground POM live-tunables (viewer "Ground POM" UI). Stores the settings
    // and immediately re-writes the tileset params UBO (cheap -- see
    // write_tileset_params_buffer) so the next frame's gbuffer/RT tileset
    // sampling picks up the change; slot-derived fields (height ranges,
    // mean albedo, tile sizes) stay renderer-owned and are re-derived from
    // tileset_slots_ on every call, same as the sun_dir_intensity mirror.
    void set_tileset_pom_settings(const matter::TilesetPomSettings& s);
    // Chart-VT near band (Phase 0). Same shape and same UBO as the POM
    // setter above -- separate call because the two are now separate
    // policies; see matter::VtNearBandSettings for why they were split.
    void set_vt_near_band_settings(const matter::VtNearBandSettings& s);
    // Blit the real HDR world composite into the currently acquired swapchain
    // image, leaving it ready for UI dynamic rendering.
    bool record_composite_to_swapchain(const matter::VulkanFrame& frame,
                                       std::string& error);
    VkRasterAttachments raster_attachments() const;
    // Logs when the RT hit shader rejects a part record. A rejection means a
    // stale or unwritten rt_parts slot was traced, which is the signal that
    // confirms (or refutes) the stale-tail clear in emit_ray_instances. The
    // shader has always counted these, but the only reader was a test-only
    // readback compiled out of shipping builds, so in a real session the
    // count was invisible. Deliberately NOT inside the test guard.
    void report_rt_trace_counters(uint32_t frame_slot);
    uint32_t rt_invalid_part_records_last_ = 0;
    uint64_t rt_invalid_part_records_total_ = 0;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    void test_force_rt_unavailable(bool unavailable) {
        test_force_rt_unavailable_ = unavailable;
    }
    void test_skip_volumetrics(bool skip) { test_skip_volumetrics_ = skip; }
    bool readback_raster_pixel(uint32_t x, uint32_t y,
                               VkRasterPixel& pixel, std::string& error);
    bool readback_materials(std::vector<MaterialGpuRecord>& records,
                            std::string& error);
    uint64_t test_material_upload_record_count(uint32_t frame_slot) const {
        return frame_slot < frames_.size()
                   ? frames_[frame_slot].material_upload_record_count
                   : 0;
    }
    VkMemoryPropertyFlags test_material_buffer_memory(
        uint32_t frame_slot) const {
        return frame_slot < frames_.size()
                   ? frames_[frame_slot].materials.memory_properties
                   : 0;
    }
    VkMemoryPropertyFlags test_material_staging_memory(
        uint32_t frame_slot) const {
        return frame_slot < frames_.size()
                   ? frames_[frame_slot].material_upload.memory_properties
                   : 0;
    }
    VkDeviceAddress test_rt_geometry_address(uint64_t part_hash) const;
    // WP-G: cone_width/cone_spread drive the ray cone the test ray carries
    // (footprint at the origin, spread in radians). The 3-argument overload
    // keeps the pre-WP-G call sites tracing a degenerate (mip-0) cone.
    bool record_test_surface_ray(const matter::VulkanFrame& frame,
                                 matter::Float3 origin,
                                 matter::Float3 direction,
                                 uint32_t invalid_part_slot, float cone_width,
                                 float cone_spread, std::string& error);
    bool record_test_surface_ray(const matter::VulkanFrame& frame,
                                 matter::Float3 origin,
                                 matter::Float3 direction,
                                 uint32_t invalid_part_slot,
                                 std::string& error);
    bool readback_test_surface_hit(uint32_t frame_slot, RtSurfaceHit& hit,
                                   uint32_t& invalid_count,
                                   std::string& error);
    bool readback_rt_trace_counters(uint32_t frame_slot,
                                    RtTraceCounters& counters,
                                    std::string& error);
    bool test_readback_reflection_sample_counts(uint32_t& base_samples,
                                                uint32_t& coat_samples,
                                                std::string& error);
    bool test_dispatch_gi_temporal_fixture(
        const GiTemporalGpuFixture& fixture, GiTemporalGpuResult& result,
        std::string& error);
    bool test_dispatch_gi_atrous_fixture(
        const GiAtrousGpuFixture& fixture, GiAtrousGpuResult& result,
        std::string& error);
    bool test_dispatch_animation_skin_fixture(
        const VkAnimationSkinGpuFixture& fixture,
        VkAnimationSkinGpuResult& result, std::string& error);
    bool test_readback_animation_skin_output(
        uint32_t frame_slot, uint32_t vertex_count,
        std::vector<VkSkinVertex>& output, std::string& error);
    bool test_record_hdr_constant(const matter::VulkanFrame& frame,
                                  matter::Float3 color,
                                  std::string& error);
    bool test_rt_blas_built(uint64_t part_hash) const;
    uint64_t test_rt_blas_candidate_serial(uint64_t part_hash) const;
    std::weak_ptr<void> test_rt_blas_lifetime(uint64_t part_hash) const;
    VkDeviceAddress test_rt_sbt_address() const { return rt_sbt_address_; }
    VkDeviceAddress test_rt_test_raygen_address() const {
        return rt_sbt_test_raygen_address_;
    }
    VkDeviceSize test_rt_sbt_stride() const { return rt_sbt_stride_; }
    VkDeviceAddress test_rt_miss_address() const { return rt_sbt_miss_address_; }
    VkDeviceAddress test_rt_hit_address() const { return rt_sbt_hit_address_; }
    VkDeviceSize test_rt_miss_region_size() const { return rt_sbt_miss_size_; }
    VkDeviceSize test_rt_hit_region_size() const { return rt_sbt_hit_size_; }
    uint32_t test_surface_trace_dispatches() const {
        return test_surface_trace_dispatches_;
    }
    VkDeviceAddress test_rt_scratch_address(uint32_t frame_slot) const;
    uint32_t test_last_rt_samples() const { return last_rt_samples_; }
    bool test_last_rt_debug_view() const { return last_rt_debug_view_; }
    VkImageUsageFlags test_visibility_usage() const {
        return visibility_usage_;
    }
    VkFormat test_visibility_format() const { return visibility_.format; }
    VkFormat test_raw_diffuse_format() const { return raw_diffuse_.format; }
    VkExtent2D test_raw_diffuse_extent() const { return raw_diffuse_extent_; }
    uint32_t test_gi_presented_history_index() const {
        return gi_presented_history_index_;
    }
    uint32_t test_gi_candidate_history_index() const {
        return gi_candidate_history_index_;
    }
    bool test_composite_uses_gi_temporal() const {
        return last_composite_used_gi_temporal_;
    }
    uint64_t test_gi_history_reset_count() const {
        return gi_history_reset_count_;
    }
    uint32_t test_gi_samples_per_pixel() const {
        return gi_settings_.samples_per_pixel;
    }
    float test_shadow_visibility_for_ray(bool occluded) const {
        return ray_tracing_settings_.enabled && occluded ? 0.0f : 1.0f;
    }
    // Returns the first_index of rt_lods[rt_lod_index] for the given part hash,
    // or UINT32_MAX if the part is not found or the index is out of range.
    uint32_t test_rt_lod_first_index(uint64_t part_hash,
                                     uint32_t rt_lod_index) const {
        const auto found = slot_of_.find(part_hash);
        if (found == slot_of_.end()) return UINT32_MAX;
        const PartRecord& part = parts_[static_cast<size_t>(found->second)];
        if (rt_lod_index >= part.rt_lods.size()) return UINT32_MAX;
        return part.rt_lods[rt_lod_index].first_index;
    }
#endif
    int fill_rt_instances(std::vector<RtInstance>& output) const;

    // GPU timer results (ms, EMA-smoothed). Zones are non-overlapping;
    // each begin is recorded after the previous zone's end.
    // 0=total 1=cull 2=gbuffer 3=blas 4=tlas 5=rt 6=denoise 7=dlss 8=composite
    static constexpr uint32_t kGpuZoneTotal        = 0;
    static constexpr uint32_t kGpuZoneCull         = 1;
    static constexpr uint32_t kGpuZoneGBuffer      = 2;
    static constexpr uint32_t kGpuZoneBlas         = 3;
    static constexpr uint32_t kGpuZoneTlas         = 4;
    static constexpr uint32_t kGpuZoneRt           = 5;
    static constexpr uint32_t kGpuZoneDenoise      = 6;
    static constexpr uint32_t kGpuZoneDlss         = 7;
    static constexpr uint32_t kGpuZoneComposite    = 8;
    static constexpr uint32_t kGpuZoneVolumetrics  = 9;
    // WP-E: the VT page-fill pass (residency uploads + the tier-1 compositor's
    // dispatches and pool copies), recorded just before the G-buffer zone.
    static constexpr uint32_t kGpuZoneVt           = 10;
    static constexpr uint32_t kGpuZoneCount        = 11;
    bool gpu_timers_supported() const { return gpu_timers_supported_; }
    float gpu_zone_ms(uint32_t zone) const {
        return zone < kGpuZoneCount ? gpu_smoothed_ms_[zone] : 0.0f;
    }
    // A poisoned renderer fails closed. reset() then performs a full GPU
    // resource/pipeline teardown, clears the poison, and requires re-init
    // (explicitly or through the next dispatch_culling call) before reuse.
    void reset();
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    void set_test_device_limits(VkDeviceSize max_storage_buffer_range,
                                VkDeviceSize max_uniform_buffer_range,
                                VkDeviceSize max_buffer_size,
                                uint32_t max_dispatch_group_count_x,
                                uint32_t max_draw_indirect_count);
    void clear_test_device_limits(std::string& error);
    bool set_test_command_first_instance(uint32_t command_index,
                                         uint32_t first_instance,
                                         std::string& error);
    void set_test_scene_failure(uint32_t fail_after_replacements,
                                uint32_t fail_after_uploads);
    // Escalate a pending append-mode static upload to the full recreate path,
    // so fault tests that target buffer *replacements* stay deterministic
    // instead of depending on whether the appended part happened to fit the
    // live buffers' capacity.
    void test_force_full_static_upload() {
        if (static_upload_dirty_ != StaticUpload::kClean)
            static_upload_dirty_ = StaticUpload::kFull;
    }
    void set_test_frame_resource_failure(uint32_t fail_after_allocations);
    // Fails one named animation-skin resource operation without poisoning the
    // renderer; the queue must degrade to retained/bind pose transactionally.
    void set_test_animation_skin_failure(uint32_t fail_after_allocations,
                                         uint32_t fail_after_uploads);
    void set_test_animation_bounds_upload_failure_once() {
        test_fail_animation_bounds_upload_once_ = true;
    }
#endif

    VkBuffer indirect_buffer() const {
        return poisoned() || frames_.empty()
                   ? VK_NULL_HANDLE
                   : frames_[active_frame_index_].commands.buffer;
    }
    VkBuffer draw_transform_buffer() const {
        return poisoned() || frames_.empty()
                   ? VK_NULL_HANDLE
                   : frames_[active_frame_index_].draw_transforms.buffer;
    }
    uint32_t draw_command_count() const {
        return poisoned() ? 0 : uploaded_command_count_;
    }
    uint32_t cluster_count() const {
        return poisoned() ? 0 : uploaded_cluster_count_;
    }
    VkDeviceSize cluster_buffer_size() const {
        return poisoned() ? 0 : clusters_.size;
    }
    VkDeviceSize command_buffer_size() const {
        return poisoned() || frames_.empty()
                   ? 0
                   : frames_[active_frame_index_].commands.size;
    }
    VkDeviceSize draw_transform_buffer_size() const {
        return poisoned() || frames_.empty()
                   ? 0
                   : frames_[active_frame_index_].draw_transforms.size;
    }
    uint32_t raster_vertex_count() const {
        return poisoned() ? 0
                          : static_cast<uint32_t>(vertex_staging_.size());
    }
    uint32_t raster_index_count() const {
        return poisoned() ? 0
                          : static_cast<uint32_t>(index_staging_.size());
    }
    VkDeviceSize raster_vertex_buffer_size() const {
        return poisoned() ? 0 : vertices_.size;
    }
    VkDeviceSize raster_index_buffer_size() const {
        return poisoned() ? 0 : indices_.size;
    }
    uint32_t raster_draw_command_count() const {
        return poisoned() ? 0 : raster_draw_command_count_;
    }
    uint32_t uploaded_raster_draw_command_count() const {
        return poisoned() ? 0 : uploaded_raster_draw_command_count_;
    }

    // Static cluster/vertex/index upload census (process-wide, monotonic).
    // kFull recreates the buffers and rewrites all of them; kAppend writes only
    // the dirty ranges. Which one runs, how often, and the worst single kFull
    // is what separates "the renderer is slow" from "the renderer occasionally
    // rewrites the whole world inside one frame".
    struct StaticUploadCensus {
        uint64_t full_count = 0, full_us = 0, full_max_us = 0;
        uint64_t append_count = 0, append_us = 0;
    };
    static StaticUploadCensus static_upload_census();

private:
    struct GpuCluster {
        float aabb_min[3];
        float radius;
        float aabb_max[3];
        float pad0;
        // Normalized LOD switch distances (lod_distance.h), fine -> coarse and
        // INCREASING. Mirrors ClusterMeta.switch_distances in shaders_vk/cull.comp.
        float switch_distances[kVkMaxLod];
        uint32_t lod_mesh_idx[kVkMaxLod];
        uint32_t lod_count;
        uint32_t part_slot;
        uint32_t cluster_index;
        uint32_t pad1[3];
    };
    struct GpuInstance {
        GpuMat4 object_to_world;
        GpuMat4 previous_object_to_world;
        uint32_t part_slot;
        uint32_t base_lod;
        uint32_t cluster_start;
        uint32_t cluster_count;
        uint32_t history_valid;
        uint32_t instance_token;
        uint32_t animation_instance_slot;
        uint32_t animation_instance_generation;
    };
    struct GpuDrawTransform {
        GpuMat4 current;
        GpuMat4 previous;
        uint32_t history_valid;
        uint32_t instance_token;
        // WP-E: the transported vt slot (indirection layer + 1; 0 = no VT).
        // cull.comp writes it from the per-(part, lod) vt_draw_slots table;
        // raster.vert forwards it flat to gbuffer.frag. Every other writer of
        // this struct (the skin tail, tests) leaves it zero, which is the
        // fail-closed legacy path.
        uint32_t vt_slot;
        // LOD debug view: the rung cull.comp selected for this draw. This is
        // the old trailing pad word renamed, NOT a new field -- the 144-byte
        // assert below is the guard. Direct writers of this struct (the skin
        // tail, tests) leave it zero and supply their rung by push constant.
        uint32_t selected_lod;
    };
    static_assert(sizeof(GpuCluster) == 128);
    static_assert(sizeof(GpuInstance) == 160);
    static_assert(sizeof(GpuDrawTransform) == 144);
    static_assert(offsetof(GpuDrawTransform, selected_lod) == 140);

    struct RtLodRecord {
        uint32_t cluster_index = 0;
        uint32_t lod_index = 0;
        // first_index is part-local (NOT rebased; stored this way so compaction
        // in release_part does not invalidate surviving parts' rt_lods).
        // Consumers address the per-part rt_index buffer directly via this offset.
        uint32_t first_index = 0;    // part-local index into rt_index buffer
        uint32_t index_count = 0;    // 3 × triangle count
        uint32_t primitive_count = 0;
        std::shared_ptr<matter::VkAccelerationStructureResource> blas;
        std::shared_ptr<matter::VkAccelerationStructureResource> candidate;
        bool built = false;
        bool geometry_opaque = false;
        bool candidate_opaque = false;
        uint64_t candidate_serial = 0;
        std::vector<uint32_t> material_ids;
    };

    struct PartRecord {
        uint64_t hash = 0;
        uint32_t cluster_start = 0;
        uint32_t cluster_count = 0;
        uint32_t vertex_start = 0;   // kept for Task 4 vertexOffset; NOT folded into lod offsets
        uint32_t vertex_count = 0;
        uint32_t index_start = 0;    // global offset into index_staging_
        uint32_t index_count = 0;
        bool live = false;
        std::shared_ptr<matter::VkBufferResource> rt_geometry;
        std::shared_ptr<matter::VkBufferResource> rt_index;
        std::vector<RtLodRecord> rt_lods;
        std::vector<uint32_t> rt_cluster_lod_offsets;
        // M2.5: per cluster, the number of leading MESH rungs
        // (vk_scene_detail::cluster_mesh_lod_count at registration time). A
        // cluster whose RT rung pick reaches this count is at its terminal
        // billboard and contributes NO traced geometry, so a ray never meets
        // the billboard's unrotated quad. Entry 0 (no mesh rung at all) is the
        // same case.
        std::vector<uint32_t> rt_cluster_mesh_lods;
        std::vector<uint32_t> material_ids;
        bool rt_geometry_classification_dirty = false;
        // WP-E: transported vt slot per LOD rung (0 = chartless rung). Feeds
        // the vt_draw_slots table cull.comp reads.
        std::vector<uint32_t> vt_slots;
        // Demand-driven VT bookkeeping. vt_rung_mask bit r = rung r has a
        // chart table in the part store and may be registered on demand; zero
        // means the part is eager (or chartless) and the demand pass leaves
        // it alone entirely — eager registrations are never auto-evicted.
        // vt_last_wanted[r] is the vt_demand_frame_ stamp of the last frame
        // whose CPU LOD mirror selected rung r for any of this part's
        // clusters; it is both the LRU key and the this-frame guard.
        // vt_last_requested[r] throttles duplicate registration requests
        // while one is already in flight to the engine.
        uint32_t vt_rung_mask = 0;
        // Indexed by CHART RUNG, not LOD index — see kVkMaxChartRung.
        std::array<uint64_t, kVkMaxChartRung> vt_last_wanted{};
        std::array<uint64_t, kVkMaxChartRung> vt_last_requested{};
        // M6.5: caster-harvest inputs, captured from the VtPartContext at VT
        // registration (both the eager and the demand paths). Reading them
        // from the CONTEXT rather than VkScenePart keeps the harvest's notion
        // of "anchored receiver" identical to the residency layer's — the
        // gate queue_dir_occ applies is context.surface_world_anchored, and a
        // receiver the harvest fed that the queue then refused (or vice
        // versa) would be a silent nothing-bakes bug.
        uint32_t vt_world_anchored = 0;
        float vt_local_to_world[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    };

    struct DeviceLimits {
        VkDeviceSize max_storage_buffer_range = 0;
        VkDeviceSize max_uniform_buffer_range = 0;
        VkDeviceSize max_buffer_size = 0;
        uint32_t max_dispatch_group_count_x = 0;
        uint32_t max_draw_indirect_count = 0;
    };

    // --- Phase 1 tileset Vulkan port (Task 6) ------------------------------
    // Channel order matches tileset_gtex.h's ChannelId and the descriptor
    // array index slot*6 + channel used by both descriptor sets.
    // Phase 2 (horizon-map lighting): kTilesetChannelHorizonA/B are the two
    // quarter-albedo-resolution RGBA8 horizon-map textures (.gtex v2
    // CHAN_HORIZON_A/B; see tileset_common.glsl's tileset_horizon_sin).
    // v1 .gtex files carry no horizon data -- load_tileset_slot binds the
    // shared RGBA8 dummy for these two channels and clears has_horizon.
    enum TilesetChannel {
        kTilesetChannelAlbedo = 0,
        kTilesetChannelNormal = 1,
        kTilesetChannelOrm = 2,
        kTilesetChannelHeight = 3,
        kTilesetChannelHorizonA = 4,
        kTilesetChannelHorizonB = 5,
        kTilesetChannelCount = 6,
    };

    // Raw-handle image, manually created/destroyed: matter::VkImageResource's
    // create_image() only supports a single mip level and array layer
    // (vk_resources.cpp), so the 16-layer, full-mip-chain images this feature
    // requires are built directly with vkCreateImage/vkCreateImageView here,
    // reusing the public matter::find_memory_type/create_buffer/submit_immediate
    // helpers for memory allocation and upload plumbing.
    struct TilesetImage {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;  // VK_IMAGE_VIEW_TYPE_2D_ARRAY
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    struct TilesetSlotGpu {
        bool loaded = false;
        // Phase 2 (horizon-map lighting): true when this slot's .gtex was a
        // v2 file carrying CHAN_HORIZON_A/B (tileset::load_gtex returned
        // non-empty horizon vectors). false for v1 files (or when unloaded)
        // -- channels[kTilesetChannelHorizonA/B] then hold the shared RGBA8
        // dummy and the horizon helpers in tileset_common.glsl must read as
        // fully unoccluded. Folded into TilesetParamsGpu.slot_mean_albedo's
        // valid flag rather than adding a new field (see
        // write_tileset_params_buffer).
        bool has_horizon = false;
        TilesetImage channels[kTilesetChannelCount];
        float tile_size_m = 0.0f;
        float texels_per_meter = 0.0f;
        float height_min = 0.0f;
        float height_max = 0.0f;
        float mean_albedo[3] = {0.0f, 0.0f, 0.0f};
        // Phase 0 (near-band modulate-not-replace): the same whole-atlas mean
        // as mean_albedo, over the ORM channel (occlusion, roughness,
        // metallic). gbuffer.frag divides the live detail's occlusion and
        // roughness by these to get a mean-preserving ratio, exactly as the
        // albedo path already does -- without them the near band can only
        // hard-replace the page's ORM, which is the defect Phase 0 fixes.
        // Computed at load from the decoded ORM atlas, same call as the
        // albedo mean.
        float mean_orm[3] = {0.0f, 0.0f, 0.0f};
    };

    // std140 UBO — MUST match the plan's Task 6 struct field-for-field
    // (MatterEngine3/shaders_vk/tileset_common.glsl's TilesetParams block
    // mirrors this: vec4 tile_size_m/texels_per_meter/height_min/height_max
    // each [kMaxTilesetSlots/4] (four slots per vec4 — the shader unpacks via
    // TILESET_SLOT_SCALAR),
    // vec4 mean_albedo[kMaxTilesetSlots], vec4 mean_orm[kMaxTilesetSlots],
    // vec4 pom_a{steps,refine,max_distance,fade_band},
    // vec4 pom_b{fade_center,fade_width,max_relief_m,max_march_m},
    // vec4 sun_dir_intensity{dir.xyz,intensity} (Phase 2 Task 11),
    // vec4 pom_c{datum_bias_m,ao_strength,horizon_ambient_strength,
    //            horizon_strength}
    // (Ground POM UI knobs; .w repurposed from "reserved" for the horizon-map
    // lighting feature -- see TilesetPomSettings::horizon_strength),
    // vec4 vt_near{near_band_m, near_fade_m, 0, 0} (Phase 0; see
    // matter::VtNearBandSettings). Every group below is exactly 16 bytes so
    // the natural C++ layout matches std140 with no manual padding.
    //
    // Horizon-map has_horizon flag: rather than adding a field,
    // slot_mean_albedo[slot][3] -- previously a plain 0/1 "slot loaded" bool
    // unused by any shader -- encodes both bits: 0.0 = not loaded,
    // 1.0 = loaded without horizon data (v1 .gtex), 2.0 = loaded with
    // horizon data (v2 .gtex). tileset_common.glsl's tileset_has_horizon
    // reads `mean_albedo[slot].w >= 1.5`.
    //
    // The per-slot scalar arrays are packed four-to-a-vec4 in std140 (a bare
    // float[8] would pad to 16 bytes per element, quadrupling them); the
    // shader unpacks with TILESET_SLOT_SCALAR(field, slot). The C++ side can
    // keep writing them as a flat float[kMaxTilesetSlots] because that is
    // byte-identical to vec4[kMaxTilesetSlots/4].
    struct alignas(16) TilesetParamsGpu {
        float slot_tile_size_m[tileset::kMaxTilesetSlots]{};
        float slot_texels_per_meter[tileset::kMaxTilesetSlots]{};
        float slot_height_min[tileset::kMaxTilesetSlots]{};
        float slot_height_max[tileset::kMaxTilesetSlots]{};
        // rgb + valid/has_horizon flag in [.][3]: 0 = not loaded, 1 = loaded
        // (no horizon), 2 = loaded (with horizon). See file comment above.
        float slot_mean_albedo[tileset::kMaxTilesetSlots][4]{};
        // Phase 0: whole-atlas mean of the slot's ORM channel, rgb + unused
        // .w. The near-band composite in gbuffer.frag divides the live
        // detail's occlusion/roughness by these so the page keeps its own
        // (macro) level and the detail contributes only its deviation.
        float slot_mean_orm[tileset::kMaxTilesetSlots][4]{};
        float pom_steps = 50.0f;
        float pom_refine_steps = 4.0f;
        float pom_max_distance_m = 50.4f;
        float pom_fade_band_m = 1.0f;
        float detail_fade_center_m = 40.0f;
        float detail_fade_width_m = 10.0f;
        // Max relief depth (meters) the POM march may carve. The baked
        // height range is dominated by sparse tall litter (rock tips), so
        // marching the full range sinks the whole dirt floor by ~h_range —
        // deep stepped canyons. Parallax only needs ~10 cm to sell relief;
        // anything deeper is real-geometry territory.
        float pom_max_relief_m = 0.178f;
        // Max total world-space march length (meters). Without this cap a
        // near-tangent view ray marches relief/0.08 (~2 m) laterally and the
        // grazing ground dissolves into a structureless smear of far-away
        // texels; capping the march bounds the lateral offset parallax can
        // ever produce. When the cap is exhausted without a relief crossing
        // the march returns the capped end point (continuous with neighbors
        // that crossed just before the cap).
        float pom_max_march_m = 0.73f;
        // Task 11 (height self-shadow): direction-to-sun (normalized, world
        // space, matches the `to_sun` convention used by the RT shadow push
        // constants -- i.e. normalize(-VkSceneLighting::sun_direction)) and
        // sun_intensity in .w. gbuffer.frag skips the self-shadow march when
        // dir.y <= 0 (sun below horizon) or intensity <= 0.
        float sun_dir_intensity[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        // Ground POM UI knobs (matter::TilesetPomSettings, mirrored into the
        // UBO by write_tileset_params_buffer): x = datum_bias_m -- subtracted
        // from the decoded relief height before the clamp in
        // tileset_relief_h, letting baked litter stand proud of a recessed
        // dirt floor instead of clamping flat at the datum. y = ao_strength,
        // z = horizon_ambient_strength -- how strongly the DIRECTION-FREE
        // mean horizon occlusion darkens ambient sky irradiance, read by
        // gbuffer.frag's ambient term AND by rt_lighting.rgen's hit-path
        // sky_scale (where it replaced a hardcoded 0.7), so one knob keeps
        // raster and RT in step. Replaced shadow_strength, which blended the
        // DIRECTIONAL toward-the-sun visibility into the same omnidirectional
        // channel and shipped at 1.40 in a mix() documented as 0-1.
        // w = horizon_strength (Phase 2 horizon-map lighting) -- blends the
        // per-direction baked horizon occlusion toward 0 (fully visible)
        // instead of always applying it at full strength; see
        // tileset_common.glsl's tileset_horizon_occlusion. Gates the sun term
        // and, multiplied by z, the ambient one.
        float pom_datum_bias_ao_shadow[4] = {0.105f, 0.63f, 0.7f, 1.0f};
        // Phase 0 (matter::VtNearBandSettings): x = near_band_m (full-strength
        // reach of the live-detail modulation), y = near_fade_m (fade width
        // beyond it), z/w unused. Defaults match the struct's compiled
        // defaults so a renderer whose setter is never called still runs the
        // intended band rather than the old POM-derived one.
        float vt_near_band[4] = {50.0f, 10.0f, 0.0f, 0.0f};
    };
    // The GLSL side declares TILESET_MAX_SLOTS 8; keep the two in step. The
    // /4 packing of the scalar arrays only works for a multiple of four.
    static_assert(tileset::kMaxTilesetSlots == 8,
                  "shaders_vk/tileset_common.glsl hardcodes TILESET_MAX_SLOTS 8 "
                  "(GLSL cannot import kMaxTilesetSlots) -- change both together");
    static_assert(tileset::kMaxTilesetSlots % 4 == 0,
                  "per-slot scalars are packed four to a std140 vec4");
    static_assert(MATERIAL_MAX_DETAIL_SLOTS == tileset::kMaxTilesetSlots,
                  "material_registry.h's slot-override validation bound must "
                  "match the renderer's descriptor-array slot count");
    // 4 scalar arrays x 8 floats (= 4 x two vec4) + 8 mean_albedo vec4
    // + 8 mean_orm vec4 + 5 trailing vec4 (pom_a, pom_b, sun_dir_intensity,
    // pom_c, vt_near).
    static_assert(sizeof(TilesetParamsGpu) == 464,
                  "TilesetParamsGpu must remain twenty-nine vec4 records "
                  "(std140)");

    struct FrameResources {
        matter::VkBufferResource frame_constants;
        matter::VkBufferResource instances;
        matter::VkBufferResource commands;
        matter::VkBufferResource draw_transforms;
        matter::VkBufferResource stats;
        matter::VkBufferResource animation_bounds;
        matter::VkBufferResource material_upload;
        matter::VkBufferResource materials;
        matter::VkImageResource dlss_output;
        matter::VkBufferResource rt_instances;
        matter::VkBufferResource rt_scratch;
        matter::VkBufferResource rt_tlas_scratch;
        matter::VkAccelerationStructureResource rt_tlas;
        matter::VkBufferResource rt_parts;
        matter::VkBufferResource rt_error_counter;
        matter::VkBufferResource rt_test_output;
        matter::VkBufferResource gi_atrous_markers;
        // C2 compute deformation resources.  Sources alias the immutable
        // raster vertex arena; all other streams are per-frame so C1 fences
        // continue to own palette/work/output lifetime.
        matter::VkBufferResource skin_influences;
        matter::VkBufferResource skin_sources;
        matter::VkBufferResource skin_palette_current;
        matter::VkBufferResource skin_palette_previous;
        matter::VkBufferResource skin_work;
        matter::VkBufferResource skin_current_output;
        matter::VkBufferResource skin_previous_output;
        // WP-E: per-(part_slot, lod) transported vt slots, read by cull.comp.
        matter::VkBufferResource vt_draw_slots;
        // Per-part_slot draw overrides (max distance / LOD bias), read by
        // cull.comp. One neutral entry unless a module carries an override.
        matter::VkBufferResource part_draw_overrides;
        std::vector<VkSkinRasterDraw> ready_skin_raster_draws;
        VkExtent2D dlss_output_extent{};
        VkDescriptorSet descriptor_sets[2]{};
        VkDescriptorSet skin_descriptor_set = VK_NULL_HANDLE;
        VkDescriptorSet composite_descriptor_set = VK_NULL_HANDLE;
        VkDescriptorSet display_descriptor_set = VK_NULL_HANDLE;
        // Three denoised signals (diffuse, specular, transmission), one
        // temporal set each and three a-trous ping-pong sets each.
        VkDescriptorSet gi_temporal_descriptor_sets[3]{};
        VkDescriptorSet gi_atrous_descriptor_sets[9]{};
        uint64_t static_generation = 0;
        uint64_t instance_generation = 0;
        uint64_t command_generation = 0;
        uint64_t material_generation = 0;
        uint64_t material_upload_record_count = 0;
        VkDeviceSize pending_material_bytes = 0;
        bool stats_valid = false;
        // M1d fly-through determinism trace (render/lod_trace.h). Published on
        // exactly the same contract as stats_valid: only a frame that recorded
        // culling successfully leaves a readable cull result behind in this
        // slot's `commands` / `draw_transforms`. The counts and serial are
        // snapshotted with the flag because the capture happens one full
        // frame-slot rotation later, by which time the global
        // uploaded_command_count_ may describe a different part set.
        bool lod_trace_valid = false;
        uint32_t lod_trace_command_count = 0;
        uint32_t lod_trace_transform_slots = 0;
        uint64_t lod_trace_serial = 0;
        // Global cluster slot -> the cluster's index WITHIN ITS PART, snapshot
        // at record time; UINT32_MAX where no registered part owns the slot.
        //
        // The bucket arithmetic recovers a global slot, which is
        // PartRecord::cluster_start plus a local offset -- and cluster_start
        // comes out of free_clusters_ in part REGISTRATION order. For a
        // streamed world that is bake-completion order, so it moves between
        // warm runs exactly as the sector instance ids used to: measured on
        // PomProofBrick, 42 of 48 cluster indices survived a second warm run,
        // and the six that moved were enough to fail the gate on their own
        // once instance_token had been made content-derived. The part-local
        // index does not move, and (instance_token, local index) is still a
        // unique key: a token names one instance, an instance names one part.
        //
        // Snapshotted rather than looked up in parts_ at capture time for the
        // same reason as the counts above -- capture runs a full frame-slot
        // rotation later, and a released part's cluster range can be reused by
        // a different part in between. Populated only while the trace is on.
        std::vector<uint32_t> lod_trace_local_cluster;
        // Published only after the compute dispatch and both vertex-input
        // barriers have been recorded.  A malformed source/mapping therefore
        // leaves the regular static/last-good draw path intact.
        bool skin_raster_ready = false;
        // TLAS reuse (perf): an acceleration structure is device-resident and
        // stays valid across frames, so a slot whose TLAS already encodes the
        // exact instance records we would build again can skip the rebuild.
        // `rt_tlas_instances` is the record set this slot's TLAS was last built
        // from and `rt_tlas_geometry_epoch` the geometry epoch at that build;
        // `rt_tlas_valid` only becomes true once the frame that recorded the
        // build reported success, so an abandoned frame can never leave the
        // cache claiming content the GPU never wrote.
        std::vector<VkAccelerationStructureInstanceKHR> rt_tlas_instances;
        uint64_t rt_tlas_geometry_epoch = 0;
        uint64_t rt_tlas_pending_serial = 0;
        uint64_t rt_tlas_pending_epoch = 0;
        bool rt_tlas_valid = false;
        // GPU timestamp query pool: 18 slots (9 zones × begin/end).
        VkQueryPool ts_pool = VK_NULL_HANDLE;
        // Per zone: bit 0 set when begin was written, bit 1 when end was.
        uint8_t ts_written[kGpuZoneCount]{};
        // True when the previous recording wrote at least the total zone.
        bool ts_valid = false;
    };

    // Intermediate state shared between the record_ray_traced_shadows phases.
    struct RtBuildSel {
        PartRecord* part = nullptr;
        RtLodRecord* lod = nullptr;
        const RtInstance* source = nullptr;
        bool opaque = false;
    };
    struct RtBlasPending {
        PartRecord* part = nullptr;
        RtLodRecord* lod = nullptr;
        VkAccelerationStructureGeometryKHR geometry{};
        VkAccelerationStructureBuildGeometryInfoKHR build{};
        VkAccelerationStructureBuildRangeInfoKHR range{};
        std::shared_ptr<matter::VkAccelerationStructureResource> target;
        VkDeviceSize scratch_size = 0;
        VkDeviceSize scratch_offset = 0;
    };

    bool build_ray_geometry(
        const matter::VulkanFrame& frame,
        matter::Float3 camera_eye, float pixel_budget,
        PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes,
        PFN_vkCmdBuildAccelerationStructuresKHR cmd_build,
        std::vector<RtBuildSel>& selected_geometry,
        std::vector<RtBlasPending>& pending,
        std::string& error);
    bool emit_ray_instances(
        const matter::VulkanFrame& frame,
        PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes,
        PFN_vkCmdBuildAccelerationStructuresKHR cmd_build,
        VkDeviceSize scratch_alignment,
        const std::vector<RtBuildSel>& selected_geometry,
        const std::vector<RtBlasPending>& pending,
        bool& instances_empty,
        std::string& error);
    bool record_ray_trace_dispatch(
        const matter::VulkanFrame& frame,
        const FrameMatrices& matrices,
        VkExtent2D trace_extent,
        PFN_vkCmdTraceRaysKHR cmd_trace,
        std::string& error);

    bool create_pipeline(std::string& error);
    bool create_raster_pipelines(std::string& error);
    bool create_display_pipeline(std::string& error);
    bool create_ray_tracing_pipeline(std::string& error);
    bool create_gi_temporal_pipeline(std::string& error);
    bool create_gi_atrous_pipeline(std::string& error);
    bool ensure_raster_targets(uint32_t width, uint32_t height,
                               std::string& error);
    bool ensure_dlss_output(FrameResources& frame, VkExtent2D output_extent,
                            std::string& error);
    bool record_gi_temporal(const matter::VulkanFrame& frame,
                            std::string& error, bool retain = true);
    bool record_gi_temporal_signal(const matter::VulkanFrame& frame,
                                   uint32_t signal_mode,
                                   std::string& error, bool retain);
    bool record_gi_atrous(const matter::VulkanFrame& frame,
                          std::string& error, bool retain = true);
    bool record_gi_atrous_signal(const matter::VulkanFrame& frame,
                                 uint32_t signal_mode,
                                 std::string& error, bool retain);
    bool ensure_vertex_buffer(VkDeviceSize required_size,
                              std::string& error, bool* replaced = nullptr);
    bool ensure_index_buffer(VkDeviceSize required_size,
                             std::string& error, bool* replaced = nullptr);
    bool ensure_buffer(matter::VkBufferResource& buffer,
                       VkDeviceSize required_size, VkBufferUsageFlags usage,
                       std::string& error, bool* replaced = nullptr);
    bool ensure_build_buffer(matter::VkBufferResource& buffer,
                             VkDeviceSize required_size,
                             VkBufferUsageFlags usage, std::string& error);
    void update_descriptor(VkDescriptorSet set, uint32_t binding,
                           VkDescriptorType type,
                           const matter::VkBufferResource& buffer);
    bool ensure_frame_resources(uint32_t frame_slot_count,
                                std::string& error);
    void update_frame_descriptors(FrameResources& frame);
    bool record_animation_skinning(const matter::VulkanFrame& frame,
                                   FrameResources& resources,
                                   std::string& error);
    // MATTER_SKIN_PROBE=1 diagnostic. Asserts, on the CPU, the invariant the
    // skinned draw depends on: every index it fetches lies inside the output
    // window the compute pass actually wrote. Silent unless it is violated.
    void probe_skin_raster_draws(
        const std::vector<VkSkinRasterDraw>& draws) const;
    void update_composite_descriptor(FrameResources& frame);
    void update_display_descriptor(VkDescriptorSet set, VkImageView view);
    bool upload_scene_buffers(FrameResources& frame,
                              VkCommandBuffer material_command_buffer,
                              bool reset_stats, std::string& error);
    void record_material_upload(VkCommandBuffer command_buffer,
                                FrameResources& frame);
    bool upload_frame_constants(FrameResources& frame,
                                const FrameMatrices& matrices,
                                matter::Float3 camera_eye,
                                float pixel_budget, std::string& error);
    bool validate_draw_command_regions(std::string& error) const;
    // M1d: read this slot's completed cull result back and hand it to
    // render/lod_trace.h. Called from prepare_frame BEFORE any upload touches
    // the slot, which is the one point where the slot's fence has been waited
    // (VulkanContext::begin_frame) and the GPU-written instance counts have not
    // yet been overwritten by upload_scene_buffers' unconditional restore of
    // command_template_. Inert unless MATTER_LOD_TRACE is set.
    void capture_lod_trace(FrameResources& frame);
    void note_command_layout_rebuild();
    bool rebuild_command_template(std::string& error);
    bool apply_dynamic_command_layout(std::string& error);
    bool load_device_limits(std::string& error);
    bool fail_if_poisoned(std::string& error) const;
    bool poison(std::string& error);
    bool poisoned() const { return !poison_reason_.empty(); }
    void destroy_pipeline();

    // --- Phase 1 tileset Vulkan port (Task 6) ------------------------------
    // Creates the shared sampler, the three format-family dummy images, and
    // the TilesetParams UBO on first use. Idempotent (tileset_infra_ready_
    // guards re-entry). Called from init().
    bool ensure_tileset_infra(std::string& error);
    bool create_tileset_image(VkFormat format, uint32_t edge_px,
                              uint32_t mip_levels, uint32_t array_layers,
                              TilesetImage& out, std::string& error);
    void destroy_tileset_image(TilesetImage& image);
    // Real slot view if loaded, else the format-matched dummy's view.
    VkImageView tileset_channel_view(int slot, int channel) const;
    void write_tileset_params_buffer();
    // Writes bindings 6 (16-entry sampler2DArray) and 7 (TilesetParams UBO)
    // for one raster (set 1) descriptor set, sourced from current renderer
    // state (loaded slots or dummies). Called once per frame at frame-resource
    // creation/growth (update_frame_descriptors) and again for every frame
    // slot whenever a tileset slot loads or unloads.
    void write_tileset_descriptors_for_frame(VkDescriptorSet set);

    // --- WP-E: chart-space virtual texturing -------------------------------
    // Creates the fallback (dummy) VT descriptor sources. Always available, so
    // scene-set bindings 9-13 are legal even when no part has charts and the
    // residency runtime was never started.
    bool ensure_vt_dummies(std::string& error);
    // Starts the residency runtime on first use (first chart-bearing part).
    // Returns false only when VT is genuinely unusable; the caller then keeps
    // every part on the legacy path.
    bool ensure_vt_runtime(std::string& error);
    // Registers a part's chart-bearing rungs with the residency layer and
    // records their transported slots into parts_[slot].vt_slots.
    void register_vt_part(int part_slot, const VkScenePart& part);
    // The single (cluster, lod) -> transported VT slot rule, shared by the
    // raster table below and WP-G's GpuRtPartRecord::vt_slot so a ray hit and
    // a raster fragment on the same rung resolve the same indirection layer.
    // `global_cluster` is an index into cluster_lods_ (NOT part-local).
    uint32_t vt_slot_for_lod(const PartRecord& record, uint32_t global_cluster,
                             uint32_t lod_index) const;
    // Rebuilds the per-(part, lod) vt slot table cull.comp reads.
    void rebuild_vt_draw_slots();
    // Rebuilds the per-part_slot draw-override table cull.comp reads. Sized
    // parts_.size() while any override is set, and exactly ONE neutral entry
    // otherwise (a zero-length storage buffer is not bindable, and a neutral
    // slot-0 entry costs the shader nothing).
    void rebuild_part_draw_overrides();
    // Feeds the tier-1 compositor the two inputs only the renderer knows: the
    // bound detail tileset slots and the materialId -> (detail slot, fallback
    // albedo/ORM) table. vt_compositor.h requires the device to be idle with
    // respect to prior fills for both setters, so this waits before pushing —
    // which is why it is driven by a dirty flag and not called per frame.
    // A push that is not the runtime's first also invalidates the residency
    // layer's resident pages: they were baked from the inputs being replaced.
    void push_vt_compositor_inputs();
    // Retires deferred VtCompositor::invalidate_part calls whose fills can no
    // longer be in flight (see vt_pending_invalidate_).
    void drain_vt_invalidations(uint64_t serial);
    // Writes scene-set bindings 9-13 (draw-slot table, pool, indirection,
    // variant records, feedback image) for one frame's descriptor set.
    void write_vt_descriptors_for_frame(FrameResources& frame);
    // Consumes the previous submission's feedback for this frame slot, resizes
    // the feedback target to the current raster extent, and refreshes this
    // frame's VT descriptors. Called once per frame before recording, when the
    // slot's fence has already been waited on (so updating its set is legal).
    // Drives its own monotonic counter rather than a VulkanFrame serial: the
    // legacy immediate path has no serial, and the LRU/retirement logic only
    // needs monotonicity.
    void vt_begin_frame(FrameResources& frame, uint32_t frame_slot);
    // Frame counter at which a compositor mesh cache dropped now can no longer
    // be read by an unretired fill. VtCompositor::kMaxBatchesInFlight is 4 and
    // one batch is recorded per frame, so twice that is a comfortable margin.
    uint64_t vt_invalidate_retire_serial() const {
        // Covers BOTH page passes' in-flight windows (WP-D's fills and WP-H's
        // enrichments), which is why the max of the two ring depths is used.
        static_assert(vt::VtEnricher::kMaxBatchesInFlight <=
                          vt::VtCompositor::kMaxBatchesInFlight,
                      "the retirement horizon must cover the deeper ring");
        return vt_frame_serial_ + 2u * vt::VtCompositor::kMaxBatchesInFlight;
    }

public:
    // --- WP-F: surfaces()-tape live update ----------------------------------
    // Replaces registered parts' per-vertex tape classification when the
    // world's surfaces() tape is edited (a new tape hash) without re-uploading
    // any geometry. Usage: begin (waits the device idle so no fill still
    // borrows the old weight arrays), any number of per-part updates, end
    // (drops every resident page so the next frames re-fill from the new
    // weights). All three are no-ops when the VT runtime never started.
    void begin_vt_surface_update();
    // rung_weights is indexed by rung; an empty entry leaves that rung
    // stripped of classification. `materials` are the tape's declared
    // registry ids (the weight columns); empty strips the whole part back to
    // TriEx materialId weights. Returns true when at least one registered
    // rung was updated.
    // P2 appends (defaulted; older callers keep compiling): the canonical
    // tape text plus per-rung f16 field lanes (rung_lanes indexed by rung,
    // lane_count halves per vertex) for the GPU interpreter (mode 3).
    bool update_vt_part_surface(
        uint64_t part_hash,
        const std::vector<std::vector<uint8_t>>& rung_weights,
        const std::vector<uint32_t>& materials, uint64_t tape_hash,
        const char* tape_text = nullptr,
        const std::vector<std::vector<uint16_t>>* rung_lanes = nullptr,
        uint32_t lane_count = 0);
    void end_vt_surface_update();

    // --- Demand-driven VT variant registration ------------------------------
    // Moves the wanted-but-unregistered (part, rung) list built by the last
    // frame's demand pass into `out` (sorted by priority, bounded by
    // MATTER_VT_REQUESTS_PER_FRAME). The engine services each entry from the
    // part store and answers with register_vt_rung(); an entry it cannot
    // service is simply dropped — the demand pass re-surfaces the rung next
    // frame for as long as it stays wanted.
    void take_vt_rung_requests(std::vector<VtRungRequest>& out);
    // Registers ONE deferred (part, rung) variant. Starts the VT runtime on
    // first use. When the layer pool or the CPU mesh budget is full, evicts
    // least-recently-wanted demand-managed variants (never eager ones, never
    // one wanted this frame) until the registration fits; if it still cannot
    // fit, the residency layer counts the rejection and the caller's request
    // simply retries on a later frame — over-budget is a working-set signal
    // now, not a permanent verdict. Returns true when the rung ends up
    // registered. `atlas` and every array `context` points at are borrowed
    // for the call only (the residency layer copies synchronously).
    bool register_vt_rung(uint64_t part_hash, uint32_t rung,
                          const chart_atlas::ChartAtlasRung& atlas,
                          const vt::VtPartContext& context);

private:
    // Per-frame working-set maintenance for demand-driven VT. Mirrors
    // cull.comp's LOD selection exactly (same clusters, same thresholds, same
    // projected-size formula) over the static instance set to stamp
    // vt_last_wanted per (part, rung), queue registration requests for wanted
    // rungs with no slot, and release variants that stayed unwanted for
    // MATTER_VT_LINGER_FRAMES. Cheap no-op while no deferred part is live.
    void update_vt_demand(matter::Float3 camera_eye, float pixel_budget);
    // M6.5: harvest impostored-caster geometry for every world-anchored VT
    // receiver (terrain sectors) and hand the sets to the residency layer.
    // Runs beside update_vt_demand but only when the static instance set (or
    // the receiver registrations) changed — the harvest is O(receivers x
    // instance clusters) and its OUTPUT is a pure function of that set, so
    // re-running it on an unchanged world could only reproduce the same
    // caster_set_hash the residency layer would then discard as a no-op.
    void update_vt_casters();
    // Releases one demand-managed (part, rung) variant: residency layer,
    // vt_slots entry, draw-slot table dirty, deferred filler-cache
    // invalidation (same retirement discipline as release_part).
    void evict_vt_rung(PartRecord& record, uint32_t rung);

    matter::VulkanDevice* vulkan_ = nullptr;
    VkAnimationSkinning animation_skinning_;
    std::vector<VkSkinFallback> consumed_animation_skin_fallbacks_;
    VkAnimationBounds animation_bounds_;
    std::set<uint64_t> visible_skin_instances_;
    std::set<uint64_t> pending_visible_skin_instances_;
    uint32_t pending_skin_visibility_frame_slot_ = UINT32_MAX;
    matter::StreamlineBridge* dlss_bridge_ = nullptr;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    std::unique_ptr<matter::StreamlineBridge> test_dlss_bridge_override_;
    bool test_force_rt_unavailable_ = false;
    bool test_skip_volumetrics_ = false;
    std::vector<RtGeometryDebugRecord> test_last_rt_geometry_records_;
    uint32_t test_last_rt_blas_build_count_ = 0;
    RasterPipelineDrawDebug test_last_raster_pipeline_draw_{};
#endif
    matter::DlssMode selected_dlss_mode_ = static_cast<matter::DlssMode>(0);
    bool dlss_history_reset_pending_ = false;
    uint64_t dlss_reset_count_ = 0;
    VkDescriptorSetLayout set_layouts_[2]{};
    VkDescriptorSetLayout skin_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout skin_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline skin_pipeline_ = VK_NULL_HANDLE;
    VkPipeline raster_pipeline_ = VK_NULL_HANDLE;
    // Same descriptors/fragment stage as raster_pipeline_, but a 96-byte
    // VkSkinVertex binding with an explicit previous-position attribute.
    VkPipeline skinned_raster_pipeline_ = VK_NULL_HANDLE;
    // VK_POLYGON_MODE_LINE twins of the two above, created only when the
    // device enabled fillModeNonSolid. Everything else about them -- shaders,
    // layout, attachments, depth state -- is identical, so the wireframe view
    // is the same frame with the interiors removed.
    VkPipeline wireframe_raster_pipeline_ = VK_NULL_HANDLE;
    VkPipeline wireframe_skinned_raster_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout composite_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline composite_pipeline_ = VK_NULL_HANDLE;
    VkSampler composite_sampler_ = VK_NULL_HANDLE;
    VkSampler vol_linear_sampler_ = VK_NULL_HANDLE;  // trilinear for froxel volume
    VkDescriptorSetLayout display_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout display_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline display_pipeline_ = VK_NULL_HANDLE;
    VkFormat display_pipeline_format_ = VK_FORMAT_UNDEFINED;
    VkDescriptorSetLayout gi_temporal_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout gi_temporal_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline gi_temporal_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout gi_atrous_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout gi_atrous_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline gi_atrous_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout rt_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout rt_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline rt_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool rt_descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> rt_descriptor_sets_;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    bool initialized_ = false;

    matter::VkBufferResource clusters_;
    matter::VkBufferResource vertices_;
    matter::VkBufferResource indices_;
    std::vector<FrameResources> frames_;
    uint32_t active_frame_index_ = 0;
    uint32_t frame_resource_slot_capacity_ = 0;

    matter::VkImageResource albedo_;
    matter::VkImageResource normal_;
    matter::VkImageResource orm_;
    matter::VkImageResource velocity_;
    matter::VkImageResource material_instance_;
    matter::VkImageResource depth_;
    matter::VkImageResource hdr_;
    matter::VkImageResource visibility_;
    matter::VkImageResource raw_diffuse_;
    matter::VkImageResource raw_specular_;
    matter::VkImageResource raw_specular_aux_;
    matter::VkImageResource raw_transmission_;
    // RT PBR Phase 1: (hit_t, roughness) sibling of raw_specular_aux_ for the
    // transmission denoiser lane.
    matter::VkImageResource raw_transmission_aux_;
    matter::VkImageResource vol_dummy_3d_;
    VkExtent2D raw_diffuse_extent_{};

    // --- Phase 1 tileset Vulkan port (Task 6) ------------------------------
    TilesetSlotGpu tileset_slots_[tileset::kMaxTilesetSlots]{};
    // One dummy per distinct format among the 4 channels (albedo and ORM
    // share R8G8B8A8_UNORM, so 3 dummies cover all 4 channel roles).
    TilesetImage tileset_dummy_rgba8_;  // albedo, orm
    TilesetImage tileset_dummy_rg8_;    // normal
    TilesetImage tileset_dummy_r16_;    // height
    VkSampler tileset_sampler_ = VK_NULL_HANDLE;

    // ---- M2.5 terminal impostors -----------------------------------------
    // One 2D array image, two layers per resident impostor (shade, tint), at
    // scene-set binding 15. Created once at tileset-infrastructure time and
    // never recreated, so the descriptor is valid from the first frame and a
    // part with no impostor costs nothing.
    static constexpr uint32_t kImpostorMaxSlots = 128;
    TilesetImage impostor_atlas_{};
    // What impostor_atlas_ was CREATED with. A Vulkan image cannot be resized
    // and impostor::cell_px() is a live env read, so these latch the
    // resolution at creation; every upload is validated against them.
    uint32_t impostor_layer_px_ = 0;
    size_t   impostor_atlas_bytes_ = 0;
    VkSampler    impostor_sampler_ = VK_NULL_HANDLE;
    // Bump allocator with a free list. Slots are returned on release_part, so
    // a streaming world recycles them; exhaustion is a LOGGED load failure
    // (once), never a silent absence.
    std::vector<uint32_t> impostor_free_slots_;
    uint32_t     impostor_next_slot_ = 0;
    uint32_t     impostor_resident_ = 0;
    bool         impostor_slots_exhausted_logged_ = false;
    // part slot -> the atlas slots it owns, so release_part can return them.
    std::map<uint32_t, std::vector<uint32_t>> impostor_slots_of_part_;

    bool create_impostor_atlas(std::string& error);
    void write_impostor_descriptor_for_frame(VkDescriptorSet set);
    // Assigns atlas slots for `part`, uploads its atlases, and patches the
    // billboard vertices already staged at `vertex_base`. Failure is reported
    // and the part still registers -- with its impostor rungs drawing as
    // untextured cards is NOT an option, so a failed upload logs and the
    // vertices keep slot 0, whose layers are cleared to zero coverage and
    // therefore discard.
    void adopt_part_impostors(const VkScenePart& part, uint32_t part_slot,
                              uint32_t vertex_base);
    matter::VkBufferResource tileset_params_;
    bool tileset_infra_ready_ = false;

    // --- WP-E: chart-space virtual texturing -------------------------------
    // The residency runtime is started lazily by the first chart-bearing part
    // (its physical pool is ~0.9 GB by default), so worlds with no charts pay
    // nothing. Until then — and forever, on a device where VT init failed —
    // scene-set bindings 9-13 point at the dummies below and every draw
    // carries vt_slot 0.
    std::unique_ptr<vt::VtResidency> vt_;
    bool vt_init_attempted_ = false;
    bool vt_unavailable_ = false;
    std::string vt_unavailable_reason_;
    TilesetImage vt_dummy_feedback_;      // R16G16B16A16_UINT 1x1x1, GENERAL
    // Dummy storage buffer standing in for the indirection buffer (11/18) and
    // the variant table (12/19) until the VT runtime is live.
    matter::VkBufferResource vt_dummy_storage_;
    bool vt_dummies_ready_ = false;
    // Per-(part_slot, lod) transported slots; rebuilt on part registration.
    std::vector<uint32_t> vt_draw_slot_table_;
    bool vt_draw_slots_dirty_ = true;
    // Per-module draw overrides resolved to part hashes (the host's half) and
    // then to part slots (this table, what cull.comp reads). Both are empty in
    // the default state and the table then holds one neutral entry.
    // Sorted by part_hash, exactly as handed over: set_part_draw_overrides is
    // safe to call EVERY FRAME because the equality check against this vector
    // allocates nothing and short-circuits on size in the (empty) default
    // state. Calling it unconditionally is what makes the lane survive a
    // renderer reset without the host having to notice.
    std::vector<matter::PartDrawOverrideEntry> part_draw_override_entries_;
    std::vector<matter::PartDrawOverrideGpu> part_draw_override_table_;
    bool part_draw_overrides_dirty_ = true;
    // Borrowed: the residency layer owns the filler. Null when the compositor
    // could not be created and the WP-E stub filler is standing in.
    vt::VtCompositor* vt_compositor_ = nullptr;
    // WP-H: borrowed likewise (the residency layer owns the enricher). Null
    // when hardware ray tracing is unavailable or the enricher failed to
    // create -- tier-2 is additive, so that is a quality loss, not a fault.
    vt::VtEnricher* vt_enricher_ = nullptr;
    bool vt_inputs_dirty_ = true;
    // Set by the first push_vt_compositor_inputs() that actually pushed. A
    // later push means the inputs CHANGED, which makes every resident page
    // stale (see the invalidate_all_content call there); the first one is the
    // runtime's own start, where nothing is resident yet.
    bool vt_inputs_pushed_ = false;
    // invalidate_part() frees GPU mesh caches a recorded fill may still read,
    // and its contract wants the device idle w.r.t. fills. Rather than stall a
    // streaming world on every sector unload, invalidations are held until the
    // frame serial is far enough past kMaxBatchesInFlight that no fill
    // referencing them can still be unretired.
    std::vector<std::pair<uint64_t, uint64_t>> vt_pending_invalidate_;
    // Monotonic VT frame counter (LRU timestamps + invalidation retirement).
    // Deliberately independent of VulkanFrame::serial, which the legacy
    // immediate render path does not have.
    uint64_t vt_frame_serial_ = 0;
    // WP-F: surfaces()-tape live-update bracket state (begin/update/end).
    bool vt_surface_update_open_ = false;
    uint32_t vt_surface_updates_applied_ = 0;
    // Demand-driven VT working-set state. vt_demand_frame_ is the demand
    // pass's own monotonic clock (one tick per update_vt_demand call);
    // vt_last_wanted/vt_last_requested stamps compare against it.
    // vt_deferred_parts_ counts live PartRecords with a nonzero vt_rung_mask
    // so the per-frame pass can no-op in scenes with no deferred parts.
    uint64_t vt_demand_frame_ = 0;
    uint32_t vt_deferred_parts_ = 0;
    std::vector<VtRungRequest> vt_rung_requests_;
    // M6.5 caster-harvest change detection: the instance generation the last
    // harvest ran against, plus a dirty latch for events that add receivers
    // WITHOUT touching the instance set (a VT rung registering on demand —
    // its fresh layer would otherwise wait for the next streaming event to
    // receive any casters).
    uint64_t vt_caster_harvest_generation_ = 0;
    bool vt_casters_dirty_ = false;
    // The harvest is BUDGETED: it walks `parts_` a few receivers per frame
    // rather than all of them, and this is where it resumes. A full sweep is
    // spread over ceil(receivers / budget) frames.
    //
    // WHY. The harvest is O(receivers x static instances). The gate above
    // re-runs it whenever instance_generation_ moves, and that generation
    // bumps on EVERY streaming publish -- so in a streaming world the "common
    // frame" is a change frame, and the whole quadratic sweep ran per frame.
    // A hitch capture measured 390 ms of CPU in render at 218 resident
    // sectors and 32 k instances, against 24 ms of GPU.
    //
    // `parts_` can shift under a persisted index when a part is released, so
    // the cursor can occasionally skip or repeat a receiver. That is benign:
    // it wraps continuously and a changed generation starts a fresh sweep, so
    // nothing is starved, and a receiver harvested twice re-derives the same
    // set hash and is discarded as a no-op.
    size_t vt_caster_cursor_ = 0;
    uint64_t vt_caster_sweep_generation_ = 0;
    // Working-set knobs, refreshed from matter::VtResidencyBudgets on every
    // demand pass: linger_frames (how long a variant survives unwanted before
    // its layer is reclaimed, MATTER_VT_LINGER_FRAMES) and requests_per_frame
    // (registration requests surfaced to the engine per frame,
    // MATTER_VT_REQUESTS_PER_FRAME). Re-read rather than latched so an editor
    // edit takes effect on the next frame.
    uint32_t vt_linger_frames_ = 0;
    uint32_t vt_max_requests_ = 0;
    struct GiHistorySet {
        matter::VkImageResource radiance;
        matter::VkImageResource moments;
        matter::VkImageResource history_length;
        matter::VkImageResource depth;
        matter::VkImageResource normal;
        matter::VkImageResource identity;
        matter::VkImageResource rejection;
        matter::VkImageResource aux;
    } gi_history_[2];
    GiHistorySet gi_spec_history_[2];
    // RT PBR Phase 1: transmission history mirrors the specular chain
    // (signal mode 2 in gi_temporal.comp / gi_atrous.comp).
    GiHistorySet gi_trans_history_[2];
    matter::VkImageResource gi_atrous_[2];
    matter::VkImageResource gi_spec_atrous_[2];
    matter::VkImageResource gi_trans_atrous_[2];
    uint32_t gi_filtered_index_ = 0;
    bool gi_filtered_valid_ = false;
    uint32_t gi_presented_history_index_ = 0;
    uint32_t gi_candidate_history_index_ = 1;
    uint32_t gi_composite_history_index_ = 1;
    uint64_t gi_candidate_frame_serial_ = 0;
    uint64_t gi_candidate_attempt_token_ = 0;
    uint64_t gi_presented_attempt_token_ = 0;
    uint64_t gi_history_reset_count_ = 0;
    bool gi_candidate_was_reset_ = false;
    bool last_composite_used_gi_temporal_ = false;
    VkImageUsageFlags visibility_usage_ = 0;
    matter::VkBufferResource rt_sbt_;
    VkDeviceAddress rt_sbt_address_ = 0;
    VkDeviceAddress rt_sbt_test_raygen_address_ = 0;
    VkDeviceAddress rt_sbt_lighting_raygen_address_ = 0;
    VkDeviceAddress rt_sbt_miss_address_ = 0;
    VkDeviceAddress rt_sbt_hit_address_ = 0;
    VkDeviceSize rt_sbt_stride_ = 0;
    VkDeviceSize rt_sbt_miss_size_ = 0;
    VkDeviceSize rt_sbt_hit_size_ = 0;
    VkExtent2D raster_extent_{};
    bool raster_attachments_ready_ = false;

    std::vector<PartRecord> parts_;
    std::map<uint64_t, int> slot_of_;
    // Monotonic version of the part table. Bumped by the ONLY three writers of
    // slot_of_ -- ensure_part's insert, release_part's erase, reset()'s clear --
    // each of which is also the only writer of the parts_ fields the per-frame
    // build loop reads (PartRecord::cluster_start / .cluster_count are assigned
    // exclusively in ensure_part, immediately before the matching insert, and
    // cleared by release_part's `parts_[slot] = {}` and reset()'s clear). So an
    // unchanged version proves both "the same hashes resolve to the same slots"
    // and "those slots carry the same cluster ranges".
    uint64_t slot_of_version_ = 1;
    // Flat open-addressed mirror of slot_of_, rebuilt lazily whenever the
    // version moves.
    //
    // Perf: slot_of_ is a std::map, and update_instances() does one find() per
    // resolved instance -- ~90k red-black-tree descents per frame during a
    // sector fill, each several pointer hops into a table of thousands of
    // nodes. That is the dominant term in build_ms. The mirror answers the same
    // question in one probe of a contiguous power-of-two table. It is derived
    // state with a single source of truth (slot_of_ itself), so it cannot
    // desynchronise: any mutation moves the version and the next lookup
    // rebuilds. Entries are -1 for empty, which keeps hash 0 a legal key.
    mutable std::vector<uint64_t> part_slot_keys_;
    mutable std::vector<int32_t> part_slot_entries_;
    mutable uint32_t part_slot_mask_ = 0;
    mutable uint64_t part_slot_index_version_ = 0;
    void refresh_part_slot_index() const;
    // slot_of_.find(hash)->second, or -1. Behaviourally identical to the map
    // lookup it replaces. MATTER_VK_SLOT_INDEX=0 forwards to slot_of_ directly.
    int part_slot_lookup(uint64_t part_hash) const;
    std::vector<GpuCluster> cluster_staging_;
    std::vector<std::vector<VkSceneLod>> cluster_lods_;
    std::vector<GpuInstance> instance_staging_;
    std::vector<uint32_t> instance_part_slots_;
    size_t static_instance_count_ = 0;

    // Dynamic lane state (Task 7)
    std::vector<GpuInstance> dynamic_instance_staging_;
    // Scratch for the skin transform tail; see skin_transform_base_.
    std::vector<GpuDrawTransform> skin_transform_staging_;
    std::vector<uint32_t> dynamic_instance_part_slots_;
    uint32_t dynamic_instance_count_ = 0;
    uint64_t dynamic_submit_serial_ = 0;
    uint64_t dynamic_completed_serial_ = 0;
    bool dynamic_dirty_ = false;
    // True while command_template_/part_command_ranges_/draw_transform_slots_
    // carry per-frame offsets that include dynamic instances (see
    // apply_dynamic_command_layout). Cleared once the static baseline is
    // restored after the last dynamic instance disappears.
    bool dynamic_command_layout_applied_ = false;

    std::vector<uint32_t> part_instance_counts_;
    std::vector<DrawCommand> command_template_;
    std::vector<PartCommandRange> part_command_ranges_;
    std::vector<PartCommandRange> recorded_draw_ranges_;
    std::vector<uint8_t> raster_command_enabled_;
    std::vector<uint8_t> uploaded_raster_command_enabled_;
    std::vector<RtInstance> rt_instances_;
    size_t static_rt_instance_count_ = 0;
    // Bumped by every event that can change or free the memory an already-built
    // TLAS points at: a recorded BLAS build (which also covers the later
    // promotion of that build's candidate), a part release, and reset(). A
    // cached TLAS is only reusable while this is unchanged, so no cached
    // structure can ever outlive a bottom-level structure it references. Starts
    // at 1 so a zero-initialised FrameResources never compares equal by
    // accident.
    uint64_t rt_geometry_epoch_ = 1;
    uint64_t rt_tlas_builds_ = 0;
    uint64_t rt_tlas_reuses_ = 0;
    std::vector<VkRasterVertex> vertex_staging_;
    std::vector<uint32_t> index_staging_;   // CPU-side mirror; Task 4 uploads to GPU
    std::vector<MaterialGpuRecord> material_staging_;
    uint64_t material_shading_revision_ = 0;
    uint64_t material_geometry_revision_ = 0;
    uint64_t material_generation_ = 1;
    bool gi_history_reset_pending_ = false;
    // MATTER_VT_CHART_LOG route census: which of vt_slot_for_lod's seven
    // early-outs sent a draw to the legacy flat-tint path. Mutable + atomic
    // because vt_slot_for_lod is const and reached from the record path.
    enum VtRouteReason {
        kVtRouteTotal = 0, kVtRouteOk, kVtRouteNoSlots, kVtRouteClusterOob,
        kVtRouteLodOob, kVtRouteRungOob, kVtRouteUnregistered,
        kVtRouteTailGate, kVtRouteCount
    };
    mutable std::atomic<uint64_t> vt_route_census_[kVtRouteCount] = {};
    void dump_vt_route_census() const;

    matter::GeometryDebugView geometry_debug_view_ =
        matter::GeometryDebugView::None;
    // Requested, not necessarily honoured: select_raster_pipelines is what
    // turns this into an actually-recorded line frame.
    bool wireframe_ = false;
    uint32_t uploaded_vertex_count_ = 0;
    uint32_t uploaded_index_count_ = 0;
    uint32_t raster_draw_command_count_ = 0;
    uint32_t uploaded_raster_draw_command_count_ = 0;
    uint32_t max_clusters_per_instance_ = 0;
    uint32_t draw_transform_slots_ = 0;
    // First slot of the SKIN transform tail. cull.comp reserves draw transforms
    // inside per-bucket regions [0, skin_transform_base_); explicit skinned
    // draws are not part of any bucket (cull.comp deliberately skips the
    // skin-owned cluster/LOD), so nothing there ever writes their transform.
    // They index this tail by dynamic-instance slot instead, and the CPU fills
    // it every frame from dynamic_instance_staging_.
    //
    // Before this existed the skinned draw passed its dynamic slot straight to
    // firstInstance, landing in BUCKET space -- slot 0, which in the gallery
    // world is the Crate floor slab. The creature rendered squashed to 10%
    // height and spread 4x, because that is literally the slab's matrix.
    uint32_t skin_transform_base_ = 0;
    uint32_t uploaded_command_count_ = 0;
    uint32_t uploaded_transform_slots_ = 0;
    uint32_t uploaded_cluster_count_ = 0;
    std::vector<RtInstance> uploaded_rt_instances_;
    std::string poison_reason_;
    DeviceLimits limits_{};
    DeviceLimits physical_limits_{};
    VkSceneLighting lighting_{};
    bool lighting_initialized_ = false;
    float display_exposure_ev_ = -2.0f;
    float composite_debug_override_ = 0.0f;
    matter::VulkanRayTracingSettings ray_tracing_settings_{};
    matter::VulkanGiSettings gi_settings_{};
    std::unique_ptr<VkVolumetrics> volumetrics_;
    bool volumetrics_enabled_ = false;
    float volumetrics_debug_view_ = 0.0f;
    bool volumetrics_height_layer_ = false;
    float volumetrics_cloud_top_ = 0.0f;
    matter::TilesetPomSettings tileset_pom_settings_{};
    matter::VtNearBandSettings vt_near_band_settings_{};
    uint32_t last_rt_samples_ = 1;
    bool last_rt_debug_view_ = false;
    bool last_rt_available_ = false;
    bool last_rt_effective_ = false;
    uint32_t last_rt_trace_dispatches_ = 0;
    std::string last_rt_fallback_reason_;
    TemporalFrame temporal_frame_{};

    // ---- update_instances() unchanged-input fast path --------------------
    // The candidate instance set update_instances() builds is a pure function
    // of five inputs; these snapshot all five as of the last call that
    // returned true, so an unchanged frame can take the same early-out
    // without materialising ~24 MB of candidate records first.
    //   (1) the `instances` span, verbatim
    std::vector<VkSceneInstance> instance_input_snapshot_;
    //   (2) slot_of_, flattened in key order (covers both "which hash
    //       resolves" and "to which slot")
    std::vector<std::pair<uint64_t, int>> slot_of_snapshot_;
    //   (3) parts_[slot].cluster_start / .cluster_count -- the only
    //       PartRecord fields the build loop reads
    std::vector<std::pair<uint32_t, uint32_t>> part_cluster_snapshot_;
    //   (2)+(3) collapse to one integer compare against slot_of_version_ (see
    //       its declaration for why that counter covers both). The flattened
    //       vectors above are the fallback, kept alive by
    //       MATTER_VK_SNAPSHOT_VERSION=0; the version is strictly more
    //       conservative -- it also reports a change for a mutation that
    //       happens to restore the previous contents, which only costs a
    //       redundant rebuild.
    uint64_t snapshot_slot_of_version_ = 0;
    //   (4) temporal_frame_: sticky, set by set_temporal_frame() whenever the
    //       history data the build loop reads differs from what the renderer
    //       already held. Starts true so the first call always builds.
    bool temporal_history_changed_ = true;
    //   (5) instance_staging_ / instance_part_slots_, the buffers the result
    //       is compared against. Every mutation outside update_instances()
    //       either bumps instance_generation_ or changes slot_of_/parts_.
    uint64_t instance_snapshot_generation_ = 0;
    bool instance_snapshot_valid_ = false;
    bool instance_inputs_match_snapshot(
        const std::vector<VkSceneInstance>& instances) const noexcept;
    void snapshot_instance_inputs(const std::vector<VkSceneInstance>& instances);
    // Candidate-build scratch reused across update_instances() calls. During
    // sector streaming the candidate set is rebuilt nearly every frame; these
    // keep that from allocating and releasing ~24 MB per frame. The commit
    // paths swap them with the staging vectors (or recycle the retired
    // staging), so capacity survives in both directions.
    std::vector<GpuInstance> candidate_instances_scratch_;
    std::vector<uint32_t> candidate_slots_scratch_;
    std::vector<RtInstance> candidate_rt_scratch_;
    // Generation the uploaded_rt_instances_ mirror was last copied at; an
    // unchanged generation means unchanged content, so the ~60k-record deep
    // copy per frame can be skipped.
    uint64_t uploaded_rt_instances_generation_ = 0;

    uint64_t instance_generation_ = 1;
    uint64_t static_generation_ = 1;
    uint64_t command_generation_ = 1;
    // What the next upload_scene_buffers() owes the static cluster/vertex/
    // index buffers. kAppend is only valid while every mutation since the
    // last upload was a pure tail-append (register_part); anything that
    // rewrites existing bytes (release_part compaction, reset) must escalate
    // to kFull, because in-flight frames read the live buffers and only a
    // disjoint tail write is safe in place.
    enum class StaticUpload : uint8_t { kClean, kAppend, kFull };
    StaticUpload static_upload_dirty_ = StaticUpload::kFull;
    // A registration appended clusters but the O(clusters x LODs) command
    // template fill was deferred, so several parts landing in one frame pay
    // for one rebuild. Set by register_part; cleared by any successful
    // rebuild_command_template (the update_instances layout rebuild or
    // flush_command_template before a frame consumes the template).
    bool command_template_dirty_ = false;
    bool flush_command_template(std::string& error);
    // Free-range recycling for the static cluster/vertex/index staging and
    // buffers. release_part() returns a part's ranges here (O(part) — no
    // compaction, no full re-upload) and register_part() reuses them, so a
    // streaming world's eviction/publish churn does bounded work per sector
    // and the buffers reach a steady-state watermark. A freed range is
    // quarantined in `pending` until every frame that could still read its
    // old bytes has retired — in-place reuse before that would put garbage
    // under an in-flight frame's draws.
    struct FreeRangeList {
        struct Range { uint32_t start = 0; uint32_t count = 0; };
        struct PendingRange { Range range; uint64_t freed_serial = 0; };
        std::vector<Range> free_ranges;     // sorted by start, coalesced
        std::vector<PendingRange> pending;  // awaiting in-flight retirement
        void release(uint32_t start, uint32_t count, uint64_t serial);
        void settle(uint64_t safe_serial);  // pending -> free_ranges
        uint32_t allocate(uint32_t count);  // UINT32_MAX = no fit (extend tail)
        void clear();
    };
    FreeRangeList free_clusters_;
    FreeRangeList free_vertices_;
    FreeRangeList free_indices_;
    // Frame serial the renderer last saw (prepare_frame/dispatch_culling) and
    // the in-flight window used to settle pending ranges.
    uint64_t static_frame_serial_ = 0;
    uint64_t static_frame_window_ = 3;
    void settle_free_ranges();
    uint32_t allocate_cluster_range(uint32_t count);
    uint32_t allocate_vertex_range(uint32_t count);
    uint32_t allocate_index_range(uint32_t count);
    // Element ranges of the staging arrays written since the last upload
    // (interior reuse or tail growth). Uploaded in place by the ranged path
    // in upload_scene_buffers; a full upload clears them.
    std::vector<std::pair<uint32_t, uint32_t>> dirty_cluster_ranges_;
    std::vector<std::pair<uint32_t, uint32_t>> dirty_vertex_ranges_;
    std::vector<std::pair<uint32_t, uint32_t>> dirty_index_ranges_;
    // Escalate-only: never lets an append downgrade an owed full rewrite.
    void mark_static_append() {
        if (static_upload_dirty_ == StaticUpload::kClean)
            static_upload_dirty_ = StaticUpload::kAppend;
    }
    VkSceneUploadCounters upload_counters_{};
    VkCullStats cached_stats_{};
    // GPU timestamp support. Cached at init time from device properties.
    bool gpu_timers_supported_ = false;
    float timestamp_period_ns_ = 0.0f;
    // EMA-smoothed per-zone GPU timings (ms). Updated each frame on readback.
    float gpu_smoothed_ms_[kGpuZoneCount]{};
    // Helper recorded per-frame to stamp the command buffer.
    void write_gpu_timestamp(VkCommandBuffer cmd, uint32_t zone_id,
                             bool is_end, FrameResources& frame);
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    uint32_t test_surface_trace_dispatches_ = 0;
    DeviceLimits test_limits_{};
    bool use_test_limits_ = false;
    uint32_t test_fail_after_replacements_ =
        std::numeric_limits<uint32_t>::max();
    uint32_t test_fail_after_uploads_ =
        std::numeric_limits<uint32_t>::max();
    uint32_t test_fail_after_frame_resource_allocations_ =
        std::numeric_limits<uint32_t>::max();
    uint32_t test_fail_after_skin_allocations_ =
        std::numeric_limits<uint32_t>::max();
    uint32_t test_fail_after_skin_uploads_ =
        std::numeric_limits<uint32_t>::max();
    bool test_fail_animation_bounds_upload_once_ = false;
#endif
};

}  // namespace viewer
