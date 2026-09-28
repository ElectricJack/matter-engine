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
//   * set_tilesets / set_materials update desired CPU inputs only. Each fill
//     captures its own immutable material/parameter buffers and descriptors.
//     Supplied source lifetime tokens are retained by those batches. Channels
//     without tokens must stay alive under caller ownership until retirement.
//     Source contents must remain immutable for every batch that captured them.
//   * The first recorded fill() also records one-time image initialization
//     (intermediate layout transitions + dummy-view clears); the caller must
//     submit command buffers in record order.
//
// DETERMINISM: fills are pure functions of (chart table, mesh, tileset
// content, material table, request). No time, no random, fixed iteration
// orders on both CPU and GPU (see the shader headers). Same inputs =>
// byte-identical page blocks — asserted by tests/vt_compositor_tests.cpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "terrain_field.h"   // kMaxSurfaceMaterials (the source of truth)
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
    // Optional image/view ownership, in albedo/normal/ORM/height order.
    // A null token leaves that channel under the caller's lifetime contract.
    std::shared_ptr<void> lifetimes[4]{};
    // Physical decode retained for offline height export. Live BC composition
    // continues using normalized tileset height for its existing blend rule.
    float height_min_m=0, height_max_m=0;
    // Stable digest of the four uploaded compressed channels, every layer/mip.
    // Zero means unknown: such external images cannot authorize disk cache hits.
    uint64_t pixel_hash[2]{};
};

// materialId -> compositor inputs. Index into set_materials' array is the
// TriEx materialId (and, later, the surfaces() tape's material handle).
struct VtCompositorMaterial {
    float albedo[4] = {0.5f, 0.5f, 0.5f, 1.0f};  // scalar fallback albedo
    float orm[3] = {1.0f, 0.8f, 0.0f};            // scalar fallback ORM
    int detail_slot = -1;                          // -1 = no detail tileset
    bool height_from_top = false;                  // offline finished-surface shell datum
};

// The tier-1 page filler itself. Behind its pimpl it owns every Vulkan object
// the two compute passes need: both pipelines and their layouts, the
// descriptor pool and sampler, the neutral dummy tileset image, the global
// shared tape-op arena, the ring of per-batch material/params buffers and
// transient resources, and the cache of per-(variant, rung) chart/triangle
// buffers.
//
// Lifetime: instances come only from create() — the constructor is private
// and the type is neither copyable nor assignable. Everything it owns is
// destroyed in ~VtCompositor, and nothing internally tracks GPU completion,
// so the device must be idle with respect to work this compositor recorded
// before the destructor runs. The VkDevice/VkPhysicalDevice handles passed to
// create() are BORROWED and must outlive the compositor.
//
// Threading: all public calls belong to the thread recording fill(). A private
// CPU worker receives immutable inputs over a bounded channel; it never reads
// mutable compositor state or owns GPU handles. Shutdown joins that worker.
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
    // Per-vertex tape weights packed into the GPU triangle stream, one u8
    // column per material. DERIVED from terrain_field's constant rather than
    // restated: the two silently disagreeing mis-decodes every tape weight.
    static constexpr uint32_t kMaxSurfaceMaterials =
        static_cast<uint32_t>(terrain_field::kMaxSurfaceMaterials);

    // pipeline_cache may be VK_NULL_HANDLE. Fail-closed: nullptr + err.
    static std::unique_ptr<VtCompositor> create(VkDevice device,
                                                VkPhysicalDevice physical_device,
                                                VkPipelineCache pipeline_cache,
                                                std::string& err);
    // Destroys every Vulkan object the compositor owns. Nothing here waits on
    // a fence, so the device must already be idle with respect to fills this
    // compositor recorded.
    ~VtCompositor() override;
    VtCompositor(const VtCompositor&) = delete;
    VtCompositor& operator=(const VtCompositor&) = delete;

    // slots beyond `count` (up to kMaxDetailSlots) unbind to the dummy.
    // Stages source parameters (tile_size_m and texels_per_meter, clamped to
    // >= 1e-4) and views for subsequent fills; recorded batches are unchanged.
    // No device idle is needed. Copies the supplied lifetime tokens into desired
    // inputs and each batch that captures them; reuse of a retired batch releases
    // its previous tokens. Channels without tokens remain borrowed.
    // Returns false if count exceeds kMaxDetailSlots or nonzero count has no slots.
    bool set_tilesets(const VtTilesetSlotViews* slots, uint32_t count,
                      std::string& err);
    // materials beyond `count` (up to kMaxMaterials) reset to defaults.
    // Copies the whole desired CPU material table (all kMaxMaterials rows).
    // The next fill captures it without changing earlier GPU batches; no idle
    // is needed. A detail_slot outside [0, kMaxDetailSlots) is stored as
    // -1, i.e. "no detail tileset, use the scalar albedo/ORM fallback".
    // Stable material/source/producer identity for finished-page persistence.
    // Zero means at least one bound external image lacks a pixel fingerprint.
    // Compute at input changes, not per page. Does not contain session revisions.
    std::array<uint64_t, 2> encoded_input_identity() const;

    void set_materials(const VtCompositorMaterial* materials, uint32_t count);
    // Test hook feeding the shader's weight seam (see WeightMode).
    void set_weight_mode(WeightMode mode, uint32_t debug_mat_a = 0,
                         uint32_t debug_mat_b = 0,
                         float debug_blend_start_m = 0.0f,
                         float debug_blend_width_m = 1.0f);

    // Explicit standalone whole-part invalidation. Residency uses the exact
    // owner lifetime in release_preparation instead.
    //
    // The device does NOT have to be idle, and that is the point. Nothing is
    // destroyed in place: matching entries are moved to the retire list of the
    // most recent batch's ring and freed only when that ring comes round
    // again, which buys the same kMaxBatchesInFlight window the one-shot mesh
    // entries use. So the call is safe with earlier frames' command buffers
    // still executing, on the render thread. Freeing in place is what
    // produced VUID-vkFreeDescriptorSets-pDescriptorSets-00309 and
    // VK_ERROR_DEVICE_LOST in the former immediate-destruction path.
    void invalidate_part(uint64_t variant_hash);
    void release_preparation(const VtPreparationKey& key) override;

    // Refresh material weights/field lanes/tape while retaining compatible
    // chart geometry. Earlier batches keep their captured surface streams.
    void invalidate_surface(uint64_t variant_hash);
    void invalidate_surface(const VtPreparationKey& key) override;

    // VtPageFiller. Requests with a null atlas/part_context or an
    // out-of-range destination are skipped (fail-closed, counted in stats).
    //
    // Records only — no submit, no wait, no fence. Each call takes the next
    // slot of the internal kMaxBatchesInFlight ring and, in taking it,
    // destroys the resources retired the last time that slot was used. That
    // is what turns the "submit in record order, at most kMaxBatchesInFlight
    // unretired" contract above into a correctness requirement rather than a
    // suggestion.
    //
    // At most 256 requests are recorded per call; requests past that, and
    // pages whose candidate list would overrun the internal candidate buffer,
    // are skipped and counted in Stats::requests_skipped like every other
    // fail-closed path. A recorded request gets VtFillRequest::mark_filled()
    // called on it while the copies are recorded; a skipped one does not, and
    // the residency layer uses that flag to decide whether to map the page's
    // indirection entry — an unmapped entry beats one pointing at
    // never-written pool memory, which decodes to black.
    void fill(VkCommandBuffer cmd, const VtFillRequest* batch,
              size_t count) override;
    // Owned-snapshot requests must reach readiness here before fill(). This
    // queues/polls CPU work and advances bounded GPU buffer preparation without
    // waiting. fill() publishes completed preparation. Borrowed contexts retain the
    // synchronous seam. Call begin_preparation_frame once per recording frame.
    bool prepare(const VtPreparationKey& key,
                 const std::shared_ptr<const VtPartSnapshot>& inputs) override;
    void begin_preparation_frame() override;

    struct CpuPreparationStats {
        uint64_t submitted, completed, cancelled, failed, deferred, oversized;
        size_t retained_jobs, reserved_bytes, peak_bytes;
    };
    CpuPreparationStats cpu_preparation_stats() const;

    struct PreparationLimits {
        uint32_t allocations_per_frame = 2; // buffers and descriptor sets
        size_t upload_bytes_per_frame = 1024u * 1024u;
        double cpu_budget_ms = 0.35; // checked between operations / 64 KiB copies
    };
    // Zero allocations/bytes pause that work; zero time disables only the time
    // bound. Clamps to 64 allocations / 16 MiB per frame. A driver call cannot
    // be preempted. Takes effect next begin frame.
    void set_preparation_limits(PreparationLimits limits);
    struct GpuPreparationStats {
        uint64_t allocations = 0, uploaded_bytes = 0, completed = 0;
        uint64_t cancelled = 0, allocation_failures = 0;
        uint32_t allocations_this_frame = 0, peak_allocations_per_frame = 0;
        size_t uploaded_bytes_this_frame = 0, peak_uploaded_bytes_per_frame = 0;
        size_t pending_jobs = 0;
        double cpu_ms_this_frame = 0, peak_cpu_ms = 0;
    };
    GpuPreparationStats gpu_preparation_stats() const;

    // Monotonic lifetime counters; nothing resets them. They are incremented
    // during prepare()/fill(), so pages_filled counts pages whose copies were
    // recorded, not pages the GPU has finished writing.
    struct Stats {
        uint64_t pages_filled = 0;
        uint64_t coverage_pages_filled = 0; // no material evaluation/BC encode/copy
        uint64_t requests_skipped = 0;
        uint64_t mesh_cache_builds = 0;
        uint64_t mesh_cache_evictions = 0;
        uint64_t geometry_builds = 0;
        uint64_t geometry_reuses = 0; // refreshed surface streams using retained geometry
        // --- P2 appends (texel-rate tape) ---
        uint64_t tape_mode3_entries = 0;   // mesh entries packed for mode 3
        uint64_t tape_lane_overflows = 0;  // tapes past the 8-lane cap (mode-2
                                           // fallback, warn-once logged)
        uint64_t tape_pack_failures = 0;   // parse/pack/arena failures (mode-2
                                           // fallback)
    };
    const Stats& stats() const { return stats_; }
    // Diagnostic census of requested buffer bytes and retained corner-vector
    // capacity, including pending uploads and retired versions. Shared geometry is counted once.
    // Excludes driver allocation padding, descriptors and batch intermediates;
    // walks the bounded cache/retirement lists, so keep it off the frame path.
    struct PreparationMemory {
        uint64_t geometry_gpu_bytes = 0;
        uint64_t surface_gpu_bytes = 0;
        uint64_t corner_cpu_bytes = 0;
        uint64_t boundary_cpu_bytes = 0;
        uint32_t geometries = 0;
        uint32_t surface_versions = 0;
    };
    PreparationMemory preparation_memory() const;
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
