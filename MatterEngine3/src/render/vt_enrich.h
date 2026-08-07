#pragma once

// vt_enrich.h — WP-H tier-2 hemisphere AO page enrichment (spec Phase 6).
//
// Implements the vt::VtPageEnricher seam (vt_types.h): given a page the tier-1
// compositor has already filled, trace per-texel cosine-hemisphere occlusion
// rays against the VARIANT'S OWN acceleration structure and multiply the result
// into the page's ORM occlusion channel IN PLACE. The indirection is never
// touched — a page is tier-1 correct the moment it is filled and simply darkens
// into its own crevices later.
//
// WHAT IS BAKED, AND WHY THAT IS THE LINE: part-local self-occlusion only.
// Pages are keyed per part variant (resolved_hash), not per instance, so every
// placement of the variant shares them; anything derived from world context
// would be correct for at most one placement. Occlusion from other parts, from
// terrain, and from the sky stays the live RT lighting's job. See
// shaders_vk/vt_enrich_ao.comp's header for the same statement shader-side.
//
// THE ACCELERATION STRUCTURE IS THIS MODULE'S OWN. The spec says "the variant's
// own BLAS", and this builds exactly that: a single-BLAS TLAS over the rung mesh
// carried by VtPartContext, in part-local space, identity instance transform.
// It deliberately does NOT borrow VkSceneRenderer's rt_lods BLASes, because
// those are keyed per (cluster, LOD) over the renderer's own index stream and
// are built asynchronously, whereas the chart table's tri_order indexes THIS
// mesh — matching the two is a correctness requirement, not an optimisation.
// The structures are cached per (variant, rung) and LRU-evicted under
// MATTER_VT_ENRICH_AS_CACHE (default 8 entries) with a deferred-destroy
// graveyard, so streaming a world does not accumulate acceleration structures.
//
// THE READ-MODIFY-WRITE. The physical pool's ORM channel is BC7 and carries no
// STORAGE usage, so it cannot be written by a compute shader and cannot be
// copied out of (no TRANSFER_SRC). The pass therefore:
//   1. SAMPLES the current page texel out of the pool (hardware BC7 decode,
//      NEAREST sampler at texel centres = an exact fetch),
//   2. multiplies channel R by the traced occlusion into an RGBA8 intermediate,
//   3. re-encodes with the existing vt_bc_encode.comp (the same fast mode-6
//      BC7 tier tier-1 pages already went through), and
//   4. copies the ORM blocks back over the same page slot.
// Cost of that choice: ONE extra BC7 generation on roughness/metallic. Those
// two channels are unchanged by the pass, so after the first re-encode they sit
// on the representable lattice and further generations are fixed points — but
// the enrichment must still run at most once per fill, which is the residency
// layer's bookkeeping job (a second pass would multiply the occlusion twice).
//
// NO TEMPORAL FADE. The spec asks for a ~250 ms fade-in; this ships a straight
// apply, deliberately. Fading a read-modify-write page means either re-tracing
// the hemisphere once per fade step (4x the dominant cost, against a "< 10% GPU
// on the flight path" budget) or carrying a per-page AO scratch plus a pristine
// tier-1 ORM snapshot across eviction/re-fill/invalidation for the fade's
// lifetime — new state on the residency layer's hottest path, and a final page
// whose bytes depend on how many fade steps completed, which would make the
// determinism gate timing-dependent. What reaches the screen is already
// gradual: MATTER_VT_ENRICH_PER_FRAME (default 2) bounds enrichment to a couple
// of pages a frame, so a screenful darkens over many frames rather than in one.
// The strength is a push constant, so a future fade has its seam here.
//
// STANDALONE-ish: unlike vt_compositor this module takes a matter::VulkanDevice
// (it needs ray_tracing_available(), ray_tracing_properties() and the
// acceleration-structure helpers in vk_resources.h). Everything else about the
// contract matches the compositor's:
//
//   * enrich() records into the provided command buffer only — no submits, no
//     waits; the caller owns submission and the pool images' lifetimes.
//   * ON ENTRY the pool's ORM image must be in VK_IMAGE_LAYOUT_
//     SHADER_READ_ONLY_OPTIMAL; enrich() transitions it to TRANSFER_DST for the
//     write-back and RESTORES SHADER_READ_ONLY_OPTIMAL before returning, so the
//     residency layer's own layout tracking stays true.
//   * At most kMaxBatchesInFlight enrich() batches may be unretired at once;
//     batches must be submitted to one queue in record order.
//   * invalidate_part() is safe outside a recording: it defers destruction
//     through the same graveyards evict_lru uses, so an entry built only a
//     frame ago outlives the batch that referenced it.

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan_core.h>

#include "vt_types.h"

namespace matter { class VulkanDevice; }

namespace vt {

class VtEnricher final : public VtPageEnricher {
  public:
    // 128 payload + 4 border on each side (chart_atlas.h).
    static constexpr uint32_t kPageStore =
        chart_atlas::kVtPagePayload + 2u * chart_atlas::kVtPageBorder;   // 136
    static constexpr uint32_t kBlocksPerAxis = kPageStore / 4u;          // 34
    static constexpr uint32_t kBlocksPerPage = kBlocksPerAxis * kBlocksPerAxis;
    // Requests per enrich() call, and therefore intermediate layers per ring.
    // One group per call keeps the pool-ORM layout dance to a single
    // read-phase / write-phase pair; the residency layer's per-frame budget is
    // clamped to this.
    static constexpr uint32_t kMaxRequestsPerBatch = 16;
    // Distinct enrich() batches that may be in flight on the GPU at once.
    static constexpr uint32_t kMaxBatchesInFlight = 4;
    // nullptr + err when ray tracing is unavailable, the embedded SPIR-V is
    // missing, or any resource creation fails. Callers treat that as "tier-2 is
    // off": tier-1 pages are already correct, enrichment is purely additive.
    static std::unique_ptr<VtEnricher> create(matter::VulkanDevice& vulkan,
                                              VkPipelineCache pipeline_cache,
                                              std::string& err);
    ~VtEnricher() override;
    VtEnricher(const VtEnricher&) = delete;
    VtEnricher& operator=(const VtEnricher&) = delete;

    // VtPageEnricher. Requests with a null atlas/part_context/pool, a pool
    // without a sampled ORM view, an out-of-range slot, or a mesh the
    // acceleration structure cannot be built from are skipped (fail-closed,
    // counted in stats). Requests past kMaxRequestsPerBatch are skipped too.
    void enrich(VkCommandBuffer cmd, const VtEnrichRequest* batch,
                size_t count) override;

    // Drop the cached chart/triangle streams AND acceleration structures for a
    // variant (all rungs). Call on part unload / content-key change. Device
    // must be idle w.r.t. prior enrichments.
    void invalidate_part(uint64_t variant_hash) override;

    // Rays per texel (MATTER_VT_ENRICH_SAMPLES, default 32, clamped 8..64).
    uint32_t sample_count() const override;
    // kEnrichFadeSpan x MATTER_VT_ENRICH_CAP_METERS (default 4 x 0.5 = 2.0 m).
    float max_footprint_meters() const override;

    struct Stats {
        uint64_t pages_enriched = 0;
        uint64_t requests_skipped = 0;
        uint64_t as_builds = 0;
        uint64_t as_evictions = 0;
        uint32_t as_cached = 0;
        uint64_t as_bytes = 0;
    };
    const Stats& stats() const { return stats_; }

  private:
    struct Impl;
    explicit VtEnricher(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    Stats stats_;
};

}  // namespace vt
