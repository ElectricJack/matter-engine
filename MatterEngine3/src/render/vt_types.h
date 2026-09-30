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
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "chart_atlas.h"

namespace vt {

struct VtPartSnapshot;
struct VtFiniteSources;

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
// One horizontal warp is the minimum independently scheduled GPU slice.
constexpr uint32_t kVtPageTileWidth = 32u;
constexpr uint32_t kVtPageTilesPerRow = (kVtPageStride + kVtPageTileWidth - 1u) / kVtPageTileWidth;
constexpr uint32_t kVtPageTiles = kVtPageTilesPerRow * kVtPageStride; // 680
constexpr uint32_t kVtPagesPerLayerEdge = 16u;
constexpr uint32_t kVtPagesPerLayer = kVtPagesPerLayerEdge * kVtPagesPerLayerEdge;
constexpr uint32_t kVtPoolLayerEdgeTexels = kVtPagesPerLayerEdge * kVtPageStride;  // 2176

// Channels, in pool-image order. Formats are the residency layer's choice and
// are also reported in VtPoolBinding::format.
enum VtChannel : uint32_t {
    kVtChannelAlbedo = 0,   // BC7_UNORM_BLOCK
    kVtChannelNormal = 1,   // BC5_UNORM_BLOCK  (tangent-space XY)
    kVtChannelOrm    = 2,   // BC7_UNORM_BLOCK  (occlusion/roughness/metal)
    kVtChannelAux    = 3,   // R8G8B8A8_UNORM   (tagged material IDs or direct chart coverage)
    kVtChannelHeight = 4,   // R16_UNORM, decoded with the published page metadata
    kVtChannelCount  = 5,
};
constexpr uint64_t kVtPoolBytesPerTexel = 1u + 1u + 1u + 4u + 2u;

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
    // Authoring/export only: copy the original RGBA8 intermediate channels
    // instead of BC blocks. All color/normal/ORM destinations must be RGBA8.
    bool uncompressed = false;
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
// Pointer fields are borrowed views. Residency requests retain a VtPartSnapshot
// that owns them, so edits, LOD promotion and owner release cannot mutate a
// captured context. CPU jobs must retain that snapshot for their whole use;
// standalone callers must keep their own borrowed arrays alive.
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
// An independently generated periodic material, in physical local metres.
// Version 0 is an ordinary finite receiver. Version 1 owns a complete logical
// rectangle, including wrapped filter borders at every mip. It has no instance
// transform or receiver/weathering identity. The module's canonical support
// quad is preparation metadata, never a substitute for a drawn receiver mesh.
struct VtPeriodicDomain {
    uint32_t version = 0, width = 0, height = 0;
    float origin[3]{};
    float u[3] = {1,0,0}, v[3] = {0,1,0}, n[3] = {0,0,1};
    float period[2]{};
};

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

    // P2 (texel-rate tape, weight-seam mode 3). The compositor now evaluates
    // the tape PER TEXEL on the GPU when the part also carries the canonical
    // tape text below; the per-vertex weight columns above remain the mode-2
    // fallback and the legacy-path argmax source. All appends follow the
    // null/zero-means-unavailable rule: an older producer that leaves them
    // default keeps the part on mode 2 exactly as before.
    //
    // surface_tape_text: the canonical surfaces() program text (the same
    // bytes surface_tape_hash hashes). Borrowed like every pointer here; the
    // residency layer owns a copy. Null => no GPU tape, mode 2.
    const char* surface_tape_text = nullptr;
    // 1 when the exactly-one-instance world-anchored rule holds for this
    // variant (terrain sectors). Non-anchored parts have their world ops
    // PRE-RESOLVED to the CPU fallback constants at pack time, so the shader
    // never executes a world op for them (mirrors the warn-once CPU rule).
    uint32_t surface_world_anchored = 0;
    // Row-major 4x3 local->world rows ([m00 m01 m02 m03], [m10..], [m20..])
    // — the top three rows of the SurfaceWorldContext 4x4. Meaningful only
    // when surface_world_anchored; identity otherwise (harmless: no packed op
    // reads world inputs for non-anchored parts).
    float surface_local_to_world[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    // Per-vertex f16 field-derived lane values (vertex_count *
    // surface_lane_count, lane-major per vertex), computed by the producer in
    // vt_surface_tape.h's canonical lane-scan order (vt_scan_surface_lanes —
    // the compositor re-runs the same scan on the same text, which is what
    // keeps producer and packer lane indices identical by construction).
    // Null/0 when the tape reads no field-derived inputs (or is absent).
    const uint16_t* surface_lanes = nullptr;
    uint32_t        surface_lane_count = 0;

    // Optional immutable finite-source catalog. One 1-based source selector
    // per vertex; all three corners of a receiver triangle must agree. Zero
    // keeps the base source. IDs are categorical and never interpolated.
    std::shared_ptr<const VtFiniteSources> finite_sources;
    const uint32_t* finite_source_ids = nullptr;

    // Value-owned by every captured context; zero version preserves finite
    // chart composition. vt_make_periodic_material builds the complete source
    // identity, wrapped source catalog and canonical preparation geometry.
    VtPeriodicDomain periodic;

};

// ---------------------------------------------------------------------------
// P2: VT page content identity.
// ---------------------------------------------------------------------------
// Pages/tails are pure functions of (variant content, chart table, tape hash,
// material + tileset content, weight-seam mode, kVtBakeVersion). There is no
// on-disk page cache — the "content key" is the per-variant surface_tape_hash
// the residency layer stores — so folding the weight-seam mode and the bake
// version into that stored hash is what keeps the identity honest: flipping
// MATTER_VT_TAPE_GPU (or bumping the version) yields a different stored key,
// never a silent mix of modes behind one hash.
//
// v2: GPU tape interpreter (weight-seam mode 3) — bumped so any cache keyed
// on the old implicit v1 identity invalidates once.
constexpr uint32_t kVtBakeVersion = 3u; // robust object-local normal frame

// Process-wide gate for the GPU tape interpreter. MATTER_VT_TAPE_GPU=0 forces
// weight-seam mode 2 everywhere (the escape hatch); anything else (including
// unset) enables mode-3 promotion for tape-text-carrying parts. Read once —
// the mode cannot flip mid-session, so resident pages never mix modes.
inline bool vt_tape_gpu_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("MATTER_VT_TAPE_GPU");
        return !(v != nullptr && v[0] == '0' && v[1] == '\0');
    }();
    return enabled;
}

// Folds the weight-seam mode + kVtBakeVersion into a tape content hash
// (FNV-1a-style fold, deterministic). The residency layer applies this to
// every surface_tape_hash it stores; tests assert mode 2 and mode 3 salt to
// different keys so no stale page can survive a mode flip.
inline uint64_t vt_page_content_salt(uint64_t tape_hash,
                                     uint32_t weight_seam_mode) {
    constexpr uint64_t prime = 1099511628211ULL;
    uint64_t h = tape_hash;
    h ^= 0x9E3779B97F4A7C15ull + weight_seam_mode;
    h *= prime;
    h ^= kVtBakeVersion;
    h *= prime;
    return h;
}

// Preparation belongs to the canonical parameterization owner, which may
// serve several geometry-rung aliases. Generation distinguishes successive
// lifetimes of the same owner; a delayed release must never erase a new one.
// Standalone producers may leave owner fields zero and use (variant, rung).
// Material edits keep this key and refresh only the surface preparation.
struct VtPreparationKey {
    uint64_t variant_hash = 0;
    uint32_t rung = 0;
    uint64_t owner_key = 0;
    uint64_t owner_generation = 0;
    bool operator<(const VtPreparationKey& other) const {
        return std::tie(variant_hash, rung, owner_key, owner_generation) <
               std::tie(other.variant_hash, other.rung, other.owner_key, other.owner_generation);
    }
    bool operator==(const VtPreparationKey& other) const {
        return std::tie(variant_hash, rung, owner_key, owner_generation) ==
               std::tie(other.variant_hash, other.rung, other.owner_key, other.owner_generation);
    }
};

// Bounded draw-input table identity. The renderer's immutable binding bundle
// owns source images/material bytes through lifetime; residency keeps it alive
// for displayed pages and their retiring readers. UINT32_MAX is the existing
// unversioned producer path. These ids do not change texture formats.
constexpr uint32_t kVtMaxInputSnapshots = 8;
constexpr uint32_t kVtNoInputSnapshot = UINT32_MAX;
struct VtInputSnapshot {
    const uint32_t index;
    const std::shared_ptr<void> lifetime;
    // Bank indices are recycled; pixel-cache identities must not be. This
    // token needs no GPU storage and retains no otherwise unused input bank.
    const uint64_t identity;
    VtInputSnapshot(uint32_t table_index, std::shared_ptr<void> bindings)
        : index(table_index), lifetime(std::move(bindings)), identity(next_identity()) {}
private:
    static uint64_t next_identity() {
        static std::atomic<uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
};

// Height is normalized over one immutable source's metre range. Every page
// retains its own decode through edits, including pages from older snapshots.
// Version 0 is the legacy/neutral route; version 1 stores composed source height.
struct VtPageHeight {
    float min_m = 0;
    float range_m = 0;
    uint32_t version = 0;
};
// Immutable chart/triangle buffers used to reconstruct a connected surface.
// Addresses alone never confer ownership: publication must retain lifetime
// until every resident page and every earlier GPU reader has retired.
struct VtDrawGeometryGpu {
    uint64_t charts = 0, triangles = 0;
    uint32_t chart_count = 0, triangle_count = 0;
    uint32_t page_flags = 0, seed_node_count = 0;
};
constexpr uint32_t kVtCoverageOnly = 1u;
struct VtSurfaceBoundary;
struct VtDrawGeometry {
    VtDrawGeometryGpu gpu;
    std::shared_ptr<const void> lifetime;
    std::shared_ptr<const VtSurfaceBoundary> boundary;
};
// Optional producer identity for identical encoded albedo/normal/ORM/height
// pages, INCLUDING all filter gutters. Zero means private. Geometry, chart IDs
// and coverage are deliberately excluded: AUX and traversal belong to each
// receiver. Producers must include mapping, phase, mip and material dependencies
// in this identity; sharing a source image alone does not establish equality.
struct VtMaterialPixelKey { uint64_t low = 0, high = 0; };
struct VtPageMetadata {
    uint32_t input_snapshot = kVtNoInputSnapshot;
    VtPageHeight height;
    VtDrawGeometryGpu geometry;
    uint32_t material_slot = 0;
    uint32_t mapping_address[2]{}; // immutable receiver material table BDA
    uint32_t mapping_count = 0;
    uint64_t occlusion_address = 0; // immutable packed R16 receiver factor, zero means identity
    uint64_t surface_revision = 0; // immutable owner content revision of this page
};
static_assert(sizeof(VtPageHeight) == 12 && sizeof(VtDrawGeometryGpu) == 32 &&
              sizeof(VtPageMetadata) == 80 && offsetof(VtPageMetadata, geometry) == 16 &&
              offsetof(VtPageMetadata, material_slot) == 48 && offsetof(VtPageMetadata, occlusion_address)==64,
              "vt_common.glsl page metadata is five uvec4s per receiver page");

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
    // flag is true AND whose identity still matches. Fillers target isolated
    // scratch slots, so even partial writes followed by refusal cannot corrupt
    // resident pixels. A false flag rolls back a fresh final-slot acquisition;
    // an existing page retains its bytes and durable retry state. A filler MUST set *out_filled =
    // true for every request whose page it actually wrote, and leave it alone
    // otherwise. Deterministic: it is a pure function of what the filler did.
    //
    // Null means "legacy filler with no signal", and the residency layer then
    // assumes success exactly as it did before — so an older filler keeps
    // working, at the cost of the old hazard.
    bool* out_filled = nullptr;
    // Bounded production may retain private scratch across calls. Pending
    // requests keep their queue age and priority without publishing any bytes.
    bool* out_pending = nullptr;
    uint32_t work_tiles = 0; // 0: complete synchronous page; otherwise a slice
    uint32_t* out_work_tiles = nullptr;
    bool linear_resolve = false; // exact linear reference for GPU comparisons
    void mark_pending(uint32_t tiles = 0) const {
        if (out_pending) *out_pending = true;
        if (out_work_tiles) *out_work_tiles = tiles ? tiles : work_tiles;
    }

    // Residency publication identity. A producer records into isolated scratch
    // storage; residency checks these generations before copying to a resident
    // slot. These identify publication; part_snapshot below owns CPU inputs.
    // Neither grants a worker access to the borrowed pool or output flag.
    uint64_t owner_generation = 0;
    uint64_t content_revision = 0;
    uint64_t owner_key = 0;

    // Captured draw bindings for the page candidate. Successful publication
    // commits this identity with the page; failure retains its predecessor.
    std::shared_ptr<const VtInputSnapshot> input_snapshot;

    VtPreparationKey preparation_key() const {
        return {variant_hash, rung, owner_key, owner_generation};
    }

    // Retain these immutable inputs when dispatching asynchronous CPU work.
    // Null for standalone/legacy producers that only supply borrowed context.
    std::shared_ptr<const VtPartSnapshot> part_snapshot;

    // Recorder-only output, initialized by residency. Publish with the page's
    // pixels only after success and generation checks; never retain on a worker.
    VtPageHeight* out_height = nullptr;
    VtDrawGeometry* out_geometry = nullptr;
    VtMaterialPixelKey* out_material_key = nullptr;

    // Optional authoring readback: one uvec4 per 136x136 page texel, containing
    // chart-grouped triangle index and float-bit barycentrics. UINT_MAX marks
    // empty atlas space. Caller owns this writable storage/BDA allocation and
    // retains it until submission completes. Zero has no shader side effects.
    VkDeviceAddress export_points = 0;

    // The recorder has proved this page is wholly inside a published material
    // mapping. A supporting filler may produce only AUX/geometry and report
    // kVtCoverageOnly in out_geometry. Other fillers may still produce a full
    // page. Tails and finite/mixed boundary pages always keep full materials.
    bool coverage_only = false;

    // Convenience accessor; never null for a request the residency layer
    // produced.
    const VtPartContext* part() const {
        return static_cast<const VtPartContext*>(part_context);
    }
    // Fillers call this on the path that actually wrote the page.
    void mark_filled(VtPageHeight height = {}, VtDrawGeometry geometry = {},
                     VtMaterialPixelKey material_key = {}) const {
        if (out_height != nullptr) *out_height = height;
        if (out_geometry != nullptr) *out_geometry = std::move(geometry);
        if (out_material_key != nullptr) *out_material_key = material_key;
        if (out_filled != nullptr) *out_filled = true;
    }
};

// The tier-1 page-fill seam. Exactly one implementation is installed on the
// residency layer (VtResidency::set_filler, which takes ownership) and lives
// for the process; today that is vt_stub_filler.cpp (CPU BC encode of a flat
// material colour) or vt_compositor.{h,cpp} (the real GPU compositor).
//
// The residency layer fully resolves every request before calling — destination
// slot, chart table, part context, pool binding — so an implementation needs no
// state beyond its own staging/pipeline resources. fill() is invoked from
// VtResidency::record_frame, between the pool's layout transitions, so an
// implementation may only RECORD into `cmd`: never submit, never wait, never
// transition the pool images, and never touch indirection state (mapping is the
// residency layer's job, after the fill, and only for requests that reported
// success).
class VtPageFiller {
  public:
    virtual ~VtPageFiller() = default;
    virtual bool supports_incremental_fill() const { return false; }
    // Called only after the caller's fence retired this frame slot's previous
    // submission. Cache producers may now consume readbacks and recycle staging.
    virtual void begin_residency_frame(uint64_t, uint32_t) {}
    virtual void begin_preparation_frame() {}
    enum class PageReadiness { NeedsPreparation, Pending, Ready };
    // Optional finished-page lookup BEFORE owner preparation or slot admission.
    // The request has immutable owner/input identity and page coordinates, but
    // no pool, destination slot, or output pointers. Ready bypasses prepare();
    // Pending preserves demand without evicting another page. A cache miss (or
    // unsupported producer) returns NeedsPreparation and uses the original path.
    // Never retain the request itself: asynchronous work owns only snapshots,
    // content keys and its own completion data. fill() still reports success.
    virtual PageReadiness probe_page(const VtFillRequest&) {
        return PageReadiness::NeedsPreparation;
    }
    // Recorder-thread admission before a refinement slot is acquired. False
    // means preparation is deferred; residency retains the request and its
    // original age. Implementations must not wait, submit, or touch mappings.
    // CPU jobs retain only these immutable inputs, never a fill request's
    // borrowed pool pointer or output flag. Stateless fillers are ready now.
    virtual bool prepare(const VtPreparationKey&,
                         const std::shared_ptr<const VtPartSnapshot>&) { return true; }
    // Record fills for `count` requests into `cmd`. Deterministic given
    // identical inputs (no time/random); must not submit or wait. Every
    // request whose page is actually written must be reported through
    // VtFillRequest::mark_filled() (see the contract above).
    virtual void fill(VkCommandBuffer cmd, const VtFillRequest* batch, size_t count) = 0;
    // Render-thread owner lifecycle notifications, outside ordinary recording.
    // Cached producers must override these and defer resource destruction
    // through their existing in-flight retirement mechanism. Stateless fillers
    // need no action. Releasing one of several aliases sends no notification.
    virtual void release_preparation(const VtPreparationKey&) {}
    virtual void invalidate_surface(const VtPreparationKey&) {}
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
    // `const VtPartContext*`, exactly as VtFillRequest::part_context. Retain
    // part_snapshot for CPU work that outlives this recording call.
    const void* part_context = nullptr;
    // Destination/source pool images (borrowed, same for every request in one
    // enrich() call). `sampled_view[kVtChannelOrm]` must be non-null.
    const VtPoolBinding* pool = nullptr;
    // Monotonic frame counter, used only for the enricher's own cache ageing
    // and deferred destruction. Never feeds the bake itself — enrichment must
    // stay a pure function of geometry.
    uint64_t frame_index = 0;
    uint64_t owner_key = 0;
    uint64_t owner_generation = 0;

    // Optional immutable factor output, instead of modifying ORM. The caller
    // owns/retains this aligned 136x136 R16 subrange through GPU completion,
    // including when a producer records work but declines publication.
    VkBuffer occlusion_buffer = VK_NULL_HANDLE;
    VkDeviceSize occlusion_offset = 0;
    VkDeviceAddress occlusion_address = 0;
    bool* out_enriched = nullptr;
    // A separate factor producer reports a recorded slice through out_enriched.
    // The caller retains its lease/cursor and publishes only after all tiles.
    uint32_t tile_begin = 0, tile_count = kVtPageTiles;
    void mark_enriched() const {if(out_enriched)*out_enriched=true;}

    VtPreparationKey preparation_key() const {
        return {variant_hash, rung, owner_key, owner_generation};
    }

    std::shared_ptr<const VtPartSnapshot> part_snapshot;

    const VtPartContext* part() const {
        return static_cast<const VtPartContext*>(part_context);
    }
};

// The tier-2 enrichment seam. OPTIONAL, unlike the filler: the renderer
// installs one (VtResidency::set_enricher, takes ownership) only when hardware
// ray tracing is available, and with none installed nothing is ever queued and
// every page simply stays tier-1.
//
// Same recording discipline as VtPageFiller — record into `cmd`, do not submit
// or wait, do not transition the pool. The difference is that enrichment is a
// read-modify-write of a page that is ALREADY mapped and samplable: the
// indirection is never touched, so a page being enriched keeps rendering its
// tier-1 content. Because the refinement multiplies into the page in place,
// running it twice on one fill darkens the page twice; the residency layer
// prevents that by tracking tier per PHYSICAL SLOT and resetting it wherever a
// slot changes hands (VtResidency::slot_reset_tier).
class VtPageEnricher {
  public:
    virtual ~VtPageEnricher() = default;
    // Legacy producers still refine private ORM. A separate-factor producer
    // must call mark_enriched only after recording the requested factor rows.
    // The default request covers a complete page; incremental callers retain
    // the private factor until all rows have been recorded.
    virtual bool supports_separate_occlusion() const {return false;}
    virtual bool supports_incremental_enrichment() const {return false;}
    // Record enrichment for `count` requests into `cmd`. Deterministic given
    // identical inputs (no time, no random, no frame index in the bake); must
    // not submit or wait. See vt_enrich.h for the pool-layout contract.
    virtual void enrich(VkCommandBuffer cmd, const VtEnrichRequest* batch,
                        size_t count) = 0;
    // Drop cached per-variant state (geometry streams, acceleration
    // structures). Device must be idle with respect to prior enrichments.
    virtual void invalidate_part(uint64_t variant_hash) = 0;
    // Last alias released or incompatible owner replaced. Cached producers
    // retire exactly this lifetime, retaining resources until prior GPU use ends.
    virtual void release_preparation(const VtPreparationKey&) {}
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
