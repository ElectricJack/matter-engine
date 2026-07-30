#pragma once

// vt_compositor.h — WP-D tier-1 chart-page compositor + GPU BC encode.
//
// Implements the vt::VtPageFiller seam (vt_types.h, contract C2) with the
// real compositor: per requested page, a compute pass (vt_composite.comp)
// analytically rasterizes the page's chart region — part-local position +
// interpolated normal per texel via the chart-grouped triangle ranges, with
// nearest-triangle dilation for gutters/borders — samples the material's
// Wang detail tileset triplanar in part-local space, height-blends the
// top-2 materials, then a second compute pass (vt_bc_encode.comp) BC7/BC5
// compresses the result and the recorder copies the blocks (and the
// uncompressed aux channel) into the physical pool's page slot.
//
// STANDALONE by design: constructed from plain Vulkan handles (no
// VkSceneRenderer coupling) so it composes anywhere — the engine's renderer,
// the residency layer, or a headless test harness. The residency layer
// (WP-E) instantiates it and passes it wherever the stub filler went:
//
//   auto filler = vt::VtCompositor::create(device, physical_device,
//                                          pipeline_cache, err);
//   filler->set_tilesets(...);            // bound detail tileset views
//   filler->set_materials(...);           // materialId -> slot/fallbacks
//
// CALLER CONTRACT (the residency layer / test harness):
//   * fill() records into the provided command buffer only — no submits, no
//     waits; the caller owns submission and the pool images' lifetimes.
//   * Destination pool images arrive per request via VtFillRequest::pool
//     (vt_types.h VtPoolBinding); they must be in TRANSFER_DST_OPTIMAL when
//     pool->transfer_dst_layout is true, GENERAL otherwise, for the duration
//     of the recorded work. The compositor copies into slots but never
//     transitions the pool.
//   * At most kMaxBatchesInFlight fill() batches may be unretired on the
//     GPU at once (internal per-batch transient resources rotate through a
//     fixed ring); batches must be submitted to one queue in record order.
//   * set_tilesets / set_materials only while the device is idle with
//     respect to this compositor's prior fills.
//   * The first recorded fill() also records one-time image initialization
//     (intermediate layout transitions + dummy-view clears); the caller must
//     submit command buffers in record order.
//
// DETERMINISM: fills are pure functions of (chart table, mesh, tileset
// content, material table, request). No time, no random, fixed iteration
// orders on both CPU and GPU (see the shader headers). Same inputs =>
// byte-identical page blocks — asserted by tests/vt_compositor_tests.cpp.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "vt_types.h"

namespace vt {

// part_context reconciliation: this module consumes WP-E's pinned
// vt::VtPartContext (vt_types.h) — the indexed rung mesh streams
// (positions/normals/indices/material_ids) plus the chart table — and the
// pinned vt::VtPoolBinding carried on every VtFillRequest for the
// destination pool images (slot mapping via vt_slot_origin).

// One bound detail tileset slot (mirrors the runtime's per-slot channels the
// compositor consumes: albedo, normal RG, ORM, height). Null views fall back
// to the compositor's internal neutral dummy.
struct VtTilesetSlotViews {
    VkImageView albedo = VK_NULL_HANDLE;
    VkImageView normal = VK_NULL_HANDLE;
    VkImageView orm    = VK_NULL_HANDLE;
    VkImageView height = VK_NULL_HANDLE;
    float tile_size_m = 1.0f;        // Wang cell edge, meters
    float texels_per_meter = 1024.0f;  // finest-mip tileset texel density
};

// materialId -> compositor inputs. Index into set_materials' array is the
// TriEx materialId (and, later, the surfaces() tape's material handle).
struct VtCompositorMaterial {
    float albedo[4] = {0.5f, 0.5f, 0.5f, 1.0f};  // scalar fallback albedo
    float orm[3] = {1.0f, 0.8f, 0.0f};            // scalar fallback ORM
    int detail_slot = -1;                          // -1 = no detail tileset
};

class VtCompositor final : public VtPageFiller {
  public:
    // 128 payload + 4 border on each side.
    static constexpr uint32_t kPageStore =
        chart_atlas::kVtPagePayload + 2u * chart_atlas::kVtPageBorder;
    static constexpr uint32_t kBlocksPerAxis = kPageStore / 4u;   // 34
    static constexpr uint32_t kBlocksPerPage = kBlocksPerAxis * kBlocksPerAxis;
    static constexpr uint32_t kMaxDetailSlots = 8;   // == shader VT_MAX_SLOTS
    static constexpr uint32_t kMaxMaterials = 256;
    // Intermediate-image ring within one batch (requests per barrier group).
    static constexpr uint32_t kBatchStride = 8;
    // Distinct fill() batches that may be in flight on the GPU at once.
    static constexpr uint32_t kMaxBatchesInFlight = 4;

    // Material-weight source for vt_composite.comp's seam function.
    // kTriangleMaterial is the resting default; requests against a part whose
    // VtPartContext carries surfaces()-tape weights (surface_material_count
    // > 0) are promoted per-request to kSurfaceTape — barycentrically
    // interpolated per-vertex weight columns, top-2 kept per texel (WP-F,
    // contract C4) — or, when the part ALSO carries the canonical tape text
    // (surface_tape_text) and vt_tape_gpu_enabled(), to kSurfaceTapeGpu:
    // the packed tape evaluated PER TEXEL by the GPU interpreter (P2,
    // weight-seam mode 3; MATTER_VT_TAPE_GPU=0 is the escape hatch back to
    // mode 2). The debug ramp remains a test-only override. NOTE: a part the
    // compositor promoted to mode 3 packs f16 field lanes (not u8 weight
    // columns) into its cached triangle stream, so forcing kSurfaceTape via
    // set_weight_mode on such a part is unsupported — use the env gate.
    enum class WeightMode : uint32_t {
        kTriangleMaterial = 0,   // Phase-2 stub: TriEx materialId, weight 1
        kDebugRampBlend = 1,     // test hook: 2-material ramp along plane U
        kSurfaceTape = 2,        // WP-F: per-vertex tape weights (auto-selected)
        kSurfaceTapeGpu = 3,     // P2: per-texel GPU tape (auto-selected)
    };
    // Per-vertex tape weights packed into the GPU triangle stream, 8 u8
    // columns per vertex — must equal terrain_field::kMaxSurfaceMaterials.
    static constexpr uint32_t kMaxSurfaceMaterials = 8;

    // pipeline_cache may be VK_NULL_HANDLE. Fail-closed: nullptr + err.
    static std::unique_ptr<VtCompositor> create(VkDevice device,
                                                VkPhysicalDevice physical_device,
                                                VkPipelineCache pipeline_cache,
                                                std::string& err);
    ~VtCompositor() override;
    VtCompositor(const VtCompositor&) = delete;
    VtCompositor& operator=(const VtCompositor&) = delete;

    // slots beyond `count` (up to kMaxDetailSlots) unbind to the dummy.
    bool set_tilesets(const VtTilesetSlotViews* slots, uint32_t count,
                      std::string& err);
    // materials beyond `count` (up to kMaxMaterials) reset to defaults.
    void set_materials(const VtCompositorMaterial* materials, uint32_t count);
    // Test hook feeding the shader's weight seam (see WeightMode).
    void set_weight_mode(WeightMode mode, uint32_t debug_mat_a = 0,
                         uint32_t debug_mat_b = 0,
                         float debug_blend_start_m = 0.0f,
                         float debug_blend_width_m = 1.0f);

    // Drop the cached GPU chart/mesh buffers for a variant (all rungs). Call
    // on part unload / content-key change. Device must be idle w.r.t. fills.
    void invalidate_part(uint64_t variant_hash);

    // VtPageFiller. Requests with a null atlas/part_context or an
    // out-of-range destination are skipped (fail-closed, counted in stats).
    void fill(VkCommandBuffer cmd, const VtFillRequest* batch,
              size_t count) override;

    struct Stats {
        uint64_t pages_filled = 0;
        uint64_t requests_skipped = 0;
        uint64_t mesh_cache_builds = 0;
        uint64_t mesh_cache_evictions = 0;
        // --- P2 appends (texel-rate tape) ---
        uint64_t tape_mode3_entries = 0;   // mesh entries packed for mode 3
        uint64_t tape_lane_overflows = 0;  // tapes past the 8-lane cap (mode-2
                                           // fallback, warn-once logged)
        uint64_t tape_pack_failures = 0;   // parse/pack/arena failures (mode-2
                                           // fallback)
    };
    const Stats& stats() const { return stats_; }
    // P2: whether this compositor instance packs mode-3 tapes (the
    // MATTER_VT_TAPE_GPU gate as read at create time; see vt_types.h).
    bool tape_gpu_enabled() const;

  private:
    struct Impl;
    explicit VtCompositor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    Stats stats_;
};

}  // namespace vt
