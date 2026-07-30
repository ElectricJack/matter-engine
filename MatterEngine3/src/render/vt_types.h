#pragma once

// Chart-space virtual texturing — runtime seam (contract C2).
// Owner of the residency side: vt_residency.{h,cpp} (WP-E).
// Implementations of the filler seam: vt_stub_filler.cpp (WP-E, flat
// material fill) and vt_compositor.{h,cpp} (WP-D, the real tier-1
// compositor + GPU BC encode). The residency layer resolves each request
// (destination slot, chart table) before invoking the filler; the filler
// records GPU work into the provided command buffer and never touches
// indirection state — mapping updates are the residency layer's job after
// the fill is submitted.
//
// C++17: the batch is (pointer, count), not std::span (plan C2 wrote span;
// this header is the binding form).

#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "chart_atlas.h"

namespace vt {

// ---------------------------------------------------------------------------
// Physical page pool geometry (contract C2). The residency layer owns the
// images; fillers write payload+border texels into the slot rect below.
//
// A page slot stores kVtPageStride^2 texels: kVtPagePayload payload texels
// surrounded by kVtPageBorder texels of dilated neighbour content, so a
// bilinear fetch anywhere in the payload stays inside the slot. Slots are laid
// out as a kVtPagesPerLayerEdge^2 grid per array layer.
constexpr uint32_t kVtPageStride =
    chart_atlas::kVtPagePayload + 2u * chart_atlas::kVtPageBorder;   // 136
constexpr uint32_t kVtPagesPerLayerEdge = 16u;
constexpr uint32_t kVtPagesPerLayer = kVtPagesPerLayerEdge * kVtPagesPerLayerEdge;
constexpr uint32_t kVtPoolLayerEdgeTexels = kVtPagesPerLayerEdge * kVtPageStride;  // 2176

// Channels, in pool-image order. Formats are the residency layer's choice and
// are also reported in VtPoolBinding::format.
enum VtChannel : uint32_t {
    kVtChannelAlbedo = 0,   // BC7_UNORM_BLOCK
    kVtChannelNormal = 1,   // BC5_UNORM_BLOCK  (tangent-space XY)
    kVtChannelOrm    = 2,   // BC7_UNORM_BLOCK  (occlusion/roughness/metal)
    kVtChannelAux    = 3,   // R8G8B8A8_UNORM   (dominant/secondary mat + blend)
    kVtChannelCount  = 4,
};

// Top-left texel of a page slot inside its array layer, border included.
inline void vt_slot_origin(uint32_t slot, uint32_t& layer, uint32_t& x,
                           uint32_t& y) {
    layer = slot / kVtPagesPerLayer;
    const uint32_t local = slot % kVtPagesPerLayer;
    x = (local % kVtPagesPerLayerEdge) * kVtPageStride;
    y = (local / kVtPagesPerLayerEdge) * kVtPageStride;
}

// The live pool images, handed to the filler on every request so a filler can
// stay stateless. Borrowed; valid for the duration of the fill() call.
struct VtPoolBinding {
    VkImage     image[kVtChannelCount]{};
    VkImageView storage_view[kVtChannelCount]{};   // may be VK_NULL_HANDLE for BC images
    VkFormat    format[kVtChannelCount]{};
    // WP-H append: SAMPLED views of the same images. The pool is BC-compressed
    // and carries no STORAGE usage, so a pass that must READ resident page
    // content (tier-2 enrichment's read-modify-write of the ORM channel) goes
    // through a sampler and lets the hardware decode the block. Always
    // populated by the residency layer; VK_NULL_HANDLE means "not available"
    // and such a pass must fail closed.
    VkImageView sampled_view[kVtChannelCount]{};
    uint32_t    layer_edge_texels = kVtPoolLayerEdgeTexels;
    uint32_t    layer_count = 0;
    // Images are in VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL for the whole batch
    // when `transfer_dst_layout` is true (the CPU-staging stub path), and in
    // VK_IMAGE_LAYOUT_GENERAL otherwise (the compute-encode path). The
    // residency layer records the transitions around fill(); the filler never
    // transitions the pool itself.
    bool transfer_dst_layout = true;
};

// ---------------------------------------------------------------------------
// VtPartContext — the concrete type behind VtFillRequest::part_context.
//
// PINNED (WP-E, 2026-07-29). Both fillers (vt_stub_filler.cpp and WP-D's
// vt_compositor.cpp) reinterpret_cast `VtFillRequest::part_context` to
// `const VtPartContext*`. Fields are APPEND-ONLY: never reorder, never
// repurpose, never delete. New inputs go at the end with a documented
// "null/zero means unavailable" fallback so an older filler keeps compiling
// and an older producer keeps working.
//
// Everything here is BORROWED and owned by the residency layer's per-variant
// record. The residency layer guarantees the pointed-to storage outlives every
// fill request it has queued for that variant (a variant is not released while
// fills referencing it are in flight).
//
// The mesh is the rung's INDEXED CPU geometry (viewer::IndexedPartGeometry /
// RasterMeshData), i.e. exactly the stream that produced the render vertices:
//   triangle t has corners indices[3t+0..2]; corner c has
//   position  = positions[3c+0..2]     (part-local metres)
//   normal    = normals[3c+0..2]       (part-local, unit)
//   chart UV  = surface_uvs[2c+0..1]   (normalized [0,1] over the rung atlas)
//   material  = material_ids[c]        (registry index, UINT32_MAX = none)
//   tint      = tint_rgba[4c+0..3]     (sRGB-ish bytes, a = tint strength)
// `atlas->tri_order` indexes TRIANGLES of this same mesh, so a chart's
// triangle range is atlas->tri_order[first_tri .. first_tri+tri_count).
struct VtPartContext {
    uint64_t variant_hash = 0;   // resolved_hash of the part variant
    uint32_t rung = 0;           // LOD rung this mesh/chart table belongs to
    uint32_t rung_count = 0;     // number of rungs the variant has

    // Same table as VtFillRequest::atlas; repeated here so a filler that only
    // holds a context still has it.
    const chart_atlas::ChartAtlasRung* atlas = nullptr;

    // Indexed rung mesh (borrowed). positions/indices are always non-null when
    // triangle_count > 0; the optional streams may be null.
    const float*    positions    = nullptr;   // 3 * vertex_count
    const float*    normals      = nullptr;   // 3 * vertex_count, may be null
    const float*    surface_uvs  = nullptr;   // 2 * vertex_count, may be null
    const uint32_t* material_ids = nullptr;   // vertex_count, may be null
    const uint8_t*  tint_rgba    = nullptr;   // 4 * vertex_count, may be null
    uint32_t        vertex_count = 0;
    const uint32_t* indices        = nullptr; // 3 * triangle_count
    uint32_t        triangle_count = 0;

    // Material registry snapshot, packed exactly as MaterialRegistryPackForGPU
    // writes it: `material_count` records of `material_stride` floats. Null
    // when the residency layer had no registry snapshot (fillers must fall
    // back to a neutral albedo). Record layout is material_registry.h's.
    const float* material_table  = nullptr;
    uint32_t     material_count  = 0;
    uint32_t     material_stride = 0;

    // Per-rung dominant material id (the value build_raster_mesh_data used as
    // its default), UINT32_MAX when unknown. Cheap hint for whole-page fills.
    uint32_t dominant_material = 0xFFFFFFFFu;
    // --- append new fields below this line only ---

    // WP-F (surfaces() tape, contract C4). Per-vertex material weights the
    // world's compiled surfaces() tape produced (CPU-evaluated at part
    // registration; terrain_field::SurfaceRuntime::classify_vertices):
    //   surface_weights[v * surface_material_count + k] = u8 weight of
    //   surface_materials[k] (a material registry index) at vertex v,
    //   normalized so a vertex's weights sum to ~255.
    // The compositor interpolates the weight columns barycentrically per
    // texel and keeps the top-2 (vt_composite.comp weight-seam mode 2).
    // surface_material_count == 0 (or null pointers) means "no tape" — the
    // filler falls back to the TriEx materialId stub, so an older producer
    // keeps working unchanged. surface_material_count is capped at 8
    // (terrain_field::kMaxSurfaceMaterials == the shader packing width).
    // surface_tape_hash is the tape's content hash: it folds into the page/
    // tail content key, so an edited tape invalidates resident pages (the
    // renderer's vt-surface update path drops mesh caches + pool content
    // whenever it changes).
    const uint8_t*  surface_weights = nullptr;   // vertex_count * surface_material_count
    const uint32_t* surface_materials = nullptr; // surface_material_count registry ids
    uint32_t        surface_material_count = 0;
    uint64_t        surface_tape_hash = 0;
};

// One page fill, fully resolved by the residency layer.
struct VtFillRequest {
    uint64_t variant_hash = 0;    // resolved_hash of the part variant
    uint16_t rung = 0;            // LOD rung whose chart table applies
    uint16_t mip = 0;             // virtual mip the page belongs to
    uint16_t page_x = 0;          // page coords at `mip`
    uint16_t page_y = 0;
    uint32_t physical_slot = 0;   // destination page slot in the pool
    const chart_atlas::ChartAtlasRung* atlas = nullptr;  // borrowed, non-null
    // Residency-layer handle giving the filler access to the variant's mesh
    // (positions/normals/TriEx material ids) and material bindings. Always a
    // `const VtPartContext*` (see above) — kept as void* so the seam stays
    // stable if a future filler family needs a different context type.
    const void* part_context = nullptr;

    // Destination pool images for this batch (borrowed, same for every request
    // in one fill() call). Appended after the original C2 sketch.
    const VtPoolBinding* pool = nullptr;

    // --- WP-H append: PER-REQUEST SUCCESS SIGNAL -------------------------
    // fill() returns void and a page slot is USELESS until something writes it,
    // so without this the seam had no way to report a skipped request. The
    // residency layer used to map the indirection entry BEFORE calling the
    // filler, which turned every skip into a page permanently pointing at
    // never-written pool memory — BC7-decodes to black, and the pinned tail
    // variant of that bug blackens an entire variant at every distance.
    //
    // CONTRACT. The residency layer points this at one bool per request,
    // pre-set to false, and after fill() returns maps ONLY the requests whose
    // flag is true; a false flag rolls the slot back (freshly acquired slots go
    // straight back to the free list, a pinned tail keeps its slot but stays
    // unfilled and is re-queued). A filler MUST therefore set *out_filled =
    // true for every request whose page it actually wrote, and leave it alone
    // otherwise. Deterministic: it is a pure function of what the filler did.
    //
    // Null means "legacy filler with no signal", and the residency layer then
    // assumes success exactly as it did before — so an older filler keeps
    // working, at the cost of the old hazard.
    bool* out_filled = nullptr;

    // Convenience accessor; never null for a request the residency layer
    // produced.
    const VtPartContext* part() const {
        return static_cast<const VtPartContext*>(part_context);
    }
    // Fillers call this on the path that actually wrote the page.
    void mark_filled() const {
        if (out_filled != nullptr) *out_filled = true;
    }
};

class VtPageFiller {
  public:
    virtual ~VtPageFiller() = default;
    // Record fills for `count` requests into `cmd`. Deterministic given
    // identical inputs (no time/random); must not submit or wait. Every
    // request whose page is actually written must be reported through
    // VtFillRequest::mark_filled() (see the contract above).
    virtual void fill(VkCommandBuffer cmd, const VtFillRequest* batch, size_t count) = 0;
};

// ---------------------------------------------------------------------------
// WP-H — tier-2 page enrichment seam (spec Phase 6).
// ---------------------------------------------------------------------------
// A page becomes an enrichment candidate the moment tier-1 has filled it. The
// enricher REFINES resident page content in place: it reads the page's current
// ORM texels back out of the pool, multiplies baked hemisphere occlusion into
// the occlusion channel, and writes the same slot again. The indirection is
// never touched, so a page is always samplable and always correct — tier 2 only
// ever darkens crevices that tier 1 left flat.
//
// The whole tier is OPTIONAL and additive: with no enricher installed (no
// hardware ray tracing, or a device/build without it) the residency layer never
// queues anything and pages stay tier-1, which is the shipping behaviour of
// every wave before this one.
struct VtEnrichRequest {
    uint64_t variant_hash = 0;    // resolved_hash of the part variant
    uint16_t rung = 0;            // LOD rung whose chart table applies
    uint16_t mip = 0;             // virtual mip the page belongs to
    uint16_t page_x = 0;          // page coords at `mip`
    uint16_t page_y = 0;
    uint32_t physical_slot = 0;   // the page slot to refine, in place
    const chart_atlas::ChartAtlasRung* atlas = nullptr;  // borrowed, non-null
    // `const VtPartContext*`, exactly as VtFillRequest::part_context (same
    // lifetime guarantee: the residency layer's owned copies outlive every
    // queued enrichment for that variant).
    const void* part_context = nullptr;
    // Destination/source pool images (borrowed, same for every request in one
    // enrich() call). `sampled_view[kVtChannelOrm]` must be non-null.
    const VtPoolBinding* pool = nullptr;
    // Monotonic frame counter, used only for the enricher's own cache ageing
    // and deferred destruction. Never feeds the bake itself — enrichment must
    // stay a pure function of geometry.
    uint64_t frame_index = 0;

    const VtPartContext* part() const {
        return static_cast<const VtPartContext*>(part_context);
    }
};

class VtPageEnricher {
  public:
    virtual ~VtPageEnricher() = default;
    // Record enrichment for `count` requests into `cmd`. Deterministic given
    // identical inputs (no time, no random, no frame index in the bake); must
    // not submit or wait. See vt_enrich.h for the pool-layout contract.
    virtual void enrich(VkCommandBuffer cmd, const VtEnrichRequest* batch,
                        size_t count) = 0;
    // Drop cached per-variant state (geometry streams, acceleration
    // structures). Device must be idle with respect to prior enrichments.
    virtual void invalidate_part(uint64_t variant_hash) = 0;
    // Rays per texel this enricher traces; reported in the residency stats.
    virtual uint32_t sample_count() const = 0;
    // Largest page-texel size (metres) at which enrichment still contributes
    // anything. Past it the enricher's strength has faded to zero — see the MIP
    // FADE note in vt_enrich_ao.comp — so the residency layer must not queue
    // those pages at all, or every coarse page in a streamed world pays for a
    // full hemisphere trace that is then multiplied by 0. Return a huge value
    // to opt out of the skip.
    virtual float max_footprint_meters() const = 0;
};

}  // namespace vt
