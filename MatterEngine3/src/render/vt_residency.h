#pragma once
#include "vt_feedback_format.h"

// Chart-space virtual texturing — residency runtime (WP-E, contract C2).
//
// Two layers live in this header:
//
//   1. Pure CPU addressing/bookkeeping (VtVariantLayout, VtIndirectionMap,
//      VtTableAllocator, VtSlotPool). No Vulkan calls, no allocation of GPU
//      objects — this is what vt_residency_tests.cpp exercises headlessly.
//   2. VtResidency, the GPU-facing owner of the physical page pool, the
//      indirection buffer, the feedback readback ring and the fill queue.
//
// Sampling side lives in shaders_vk/vt_common.glsl; the two must agree on the
// packing documented below.
//
// ---------------------------------------------------------------------------
// Indirection layout (must match vt_common.glsl)
// ---------------------------------------------------------------------------
// The indirection is ONE storage buffer of 4-byte entries shared by every
// (variant, rung). Each registration owns an EXACT-SIZED table inside it:
// the virtual mip grids concatenated finest-first,
//
//   pw(m) = ceil(max(atlas_w >> m, 1) / 128)   (= ceil(atlas_w / (128 << m)))
//   ph(m) = ceil(max(atlas_h >> m, 1) / 128)
//   mip_offset[m] = sum_{k < m} pw(k) * ph(k)
//   entry(m, px, py) = table[mip_offset[m] + py * pw(m) + px]
//
// so a 512^2 atlas costs 16+4+1+1 = 22 entries (88 bytes) and the 16384^2
// worst case costs 21846 (85.3 KiB) — not the fixed 64x128 = 32 KiB layer the
// old image-array indirection burned per registration. The per-variant record
// (VariantRecordGpu / vt_common.glsl's VtVariantRecord) carries the table's
// word offset, the precomputed mip_offset prefix sums and the finest-mip page
// dims, so the shader does pure arithmetic plus ONE buffer load.
//
// This replaced the one-64x128-R16G16_UINT-array-layer-per-(variant,rung)
// image: NVIDIA caps that format at 2048 array layers (per-format
// maxArrayLayers, tighter than maxImageArrayLayers), which capped the
// simultaneously registered working set at 2048 variant-rungs. A buffer has
// no layer limit; capacity is now just MATTER_VT_INDIRECTION_MB of entries.
//
// Entry = (low 16 bits = physical slot, high 16 bits = the mip whose page
// that slot actually holds). EVERY entry is always valid: at minimum it
// points at the variant's pinned tail page, so a sample never faults and
// never needs a walk loop. A finer resident page overwrites its own entry AND
// every finer descendant entry it covers, so the shader does exactly one
// fetch.
//
// ---------------------------------------------------------------------------
// GPU-timeline recycling (the correctness contract)
// ---------------------------------------------------------------------------
// Frames in flight recorded draw records (cull.comp's vt_draw_slots table,
// GpuRtPartRecord::vt_slot) that name a variant slot. All GPU-side CONTENT
// updates (table uploads, pool page fills) are queue-ordered copies, so an
// in-flight frame always reads the bytes that were current when it was
// submitted. The one thing queue order cannot protect is INDEX REUSE backed
// by host-visible writes: rewriting the (host-visible) variant record of a
// released layer, or handing its table words / pool slots to a new owner,
// while an older frame's draw records still reference the layer.
//
// Therefore every release parks its resources in a GRAVEYARD stamped with a
// retire serial (release frame + kVtRetireHorizonFrames) and nothing —
// variant slot, indirection table block, physical page slot — is reusable
// until begin_frame's collect() sees that serial retired. Until then the dead
// registration's record, table words and page content stay byte-for-byte
// intact, so an in-flight frame that still samples it renders exactly what it
// always did. Generation counters ride every allocation; with
// MATTER_VT_DEBUG_GENERATIONS=1 the reuse discipline is asserted at every
// hand-out (see vt_residency.cpp's audit notes for what that can and cannot
// catch).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "chart_atlas.h"
#include "vt_feedback.h"
#include "vt_types.h"
#include "vt_snapshot.h"
#include "vt_surface_boundary.h"
#include "vt_surface_connections.h"
#include "vt_receiver_material.h"
#include "vt_occlusion_pages.h"
#include "vt_material_pages.h"
#include "vt_material_read.h"

namespace matter { class VulkanDevice; struct VulkanFrame; struct VkImageResource; }

namespace vt {

class VtResidency;
struct VtModuleOwner;

// Render-thread ownership of an independently resident immutable material.
// Copies share one registration. The last copy releases it through the normal
// GPU retirement horizon. A lease may safely outlive residency shutdown.
class VtMaterialModule {
public:
    ~VtMaterialModule();
    VtMaterialModule(const VtMaterialModule&) = delete;
    VtMaterialModule& operator=(const VtMaterialModule&) = delete;
    uint64_t content_hash() const { return hash_; }
private:
    friend class VtResidency;
    VtMaterialModule() = default;
    std::weak_ptr<VtModuleOwner> owner_;
    uint64_t hash_ = 0, generation_ = 0;
    uint32_t slot_ = 0;
};
using VtMaterialModuleLease = std::shared_ptr<const VtMaterialModule>;
struct VtMaterialModuleBinding { uint32_t slot = 0, generation = 0; };

struct VtMaterialReadPage {
    uint32_t x = 0, y = 0, material_slot = UINT32_MAX;
    VtPageHeight height;
};
// Explicit immutable page addresses for a composition read. Consumers do not
// sample the mutable live page table, which may not yet reflect CPU updates.
class VtMaterialRead {
public:
    const std::vector<VtMaterialReadPage>& pages() const { return pages_; }
    const VtPeriodicDomain& domain() const { return inputs_->context.periodic; }
    uint32_t mip() const { return mip_; }
private:
    friend class VtResidency;
    VtMaterialRead() = default;
    VtMaterialModuleLease module_;
    std::shared_ptr<const VtPartSnapshot> inputs_;
    std::shared_ptr<const VtInputSnapshot> snapshot_;
    uint64_t revision_ = 0;
    uint32_t mip_ = 0;
    std::vector<VtMaterialReadPage> pages_;
    std::vector<VtMaterialPages::Read> reads_;
};
using VtMaterialReadLease = std::shared_ptr<const VtMaterialRead>;
enum class VtMaterialReadStatus { Ready, Pending, Invalid };

enum class VtInvalidationReason : uint32_t {
    Explicit = 0, MaterialInputs = 1, SourceInputs = 2,
    MaterialAndSourceInputs = 3, Surface = 4, Geometry = 5,
};

// 16384 -> 8192 -> ... -> 64 is 9 levels; the last is the resident tail.
constexpr uint32_t kVtMaxMips = 9u;
// A vt slot index of 0 means "no VT" in the draw record, so slots are
// transported as (variant slot + 1). kVtNoSlot is that sentinel.
constexpr uint32_t kVtNoSlot = 0u;
// Frames a freed variant slot / indirection table block / physical page slot
// must age in the graveyard before reuse. Must cover every frame whose draw
// records could still name the freed index: the renderer's frame ring is 3
// deep and its established VT retirement margin is 2 *
// VtCompositor::kMaxBatchesInFlight = 8 frames (vk_scene_renderer.h,
// vt_invalidate_retire_serial) — ride the same horizon.
constexpr uint64_t kVtRetireHorizonFrames = 8u;
// Worst-case indirection table: 128x128 pages, down to the 64-texel tail.
constexpr uint32_t kVtMaxTableWords = 21846u;
static_assert((chart_atlas::kVtMaxAtlasDim >> (kVtMaxMips - 1u)) <=
              chart_atlas::kVtTailDim, "every atlas must reach its pinned tail");
static_assert(kVtMaxMips <= 16u, "visible feedback reserves four mip bits");
// VariantRung::tail_ready_serial value meaning "the pinned tail has never
// been written" — see the tail gate below.
constexpr uint64_t kVtTailNotReady = 0xFFFFFFFFFFFFFFFFull;

// ---------------------------------------------------------------------------
// TAIL-GATED ACTIVATION (the streaming black-flash fix).
// ---------------------------------------------------------------------------
// Registration maps every indirection entry to the pinned tail slot
// immediately, but the tail's FILL drains through the bounded fill queue —
// under a streaming burst, frames later. Until that fill has executed, the
// tail slot holds never-written pool memory, so a draw routed through the VT
// path samples deterministic-but-wrong bytes (the pool is zero-cleared at
// creation; before that clear existed it was undefined memory — the black
// flash). "Every entry is always valid" must mean "points at WRITTEN
// content", not merely "points at an allocated slot".
//
// The rule: the draw side may route a draw through the VT path (vt_slot != 0
// in cull.comp's table and in GpuRtPartRecord) only once the variant's tail
// fill has been recorded in a frame that is fully SUBMITTED — i.e. from the
// frame AFTER the fill's map (tail_ready_serial = fill frame + 1), at which
// point queue order + the pool barriers guarantee every subsequent sample
// reads the written texels. Until then the draw keeps vt_slot 0 and renders
// through the legacy classified-but-flat path — a brief flat window instead
// of a black one. Same violation class as the recycling audit: "sampled
// before first write" == "sampled after retirement".
inline bool vt_slot_activation_rule(bool live, uint64_t tail_ready_serial,
                                    uint64_t frame) {
    return live && tail_ready_serial <= frame;
}

// ---------------------------------------------------------------------------
// Virtual layout of one (variant, rung) atlas.
// ---------------------------------------------------------------------------
struct VtVariantLayout {
    uint32_t atlas_w = 0, atlas_h = 0;      // finest-mip texels
    uint32_t mip_count = 0;                 // 1..kVtMaxMips; last mip is the tail
    uint32_t page_w[kVtMaxMips]{};          // pages across at each mip
    uint32_t page_h[kVtMaxMips]{};
    uint32_t mip_offset[kVtMaxMips]{};      // word offset of mip m's grid
                                            // inside the variant's table
    uint32_t entry_count = 0;               // exact table size in words

    bool valid() const { return mip_count != 0; }
};

// Builds the layout for an atlas. Returns false (and leaves `out` invalid)
// for a zero-sized atlas or one exceeding kVtMaxAtlasDim.
inline bool vt_build_layout(uint32_t atlas_w, uint32_t atlas_h,
                            VtVariantLayout& out) {
    out = VtVariantLayout{};
    if (atlas_w == 0 || atlas_h == 0) return false;
    if (atlas_w > chart_atlas::kVtMaxAtlasDim ||
        atlas_h > chart_atlas::kVtMaxAtlasDim)
        return false;
    const uint32_t payload = chart_atlas::kVtPagePayload;
    uint32_t mips = 0;
    for (uint32_t m = 0; m < kVtMaxMips; ++m) {
        const uint32_t w = atlas_w >> m ? atlas_w >> m : 1u;
        const uint32_t h = atlas_h >> m ? atlas_h >> m : 1u;
        const uint32_t pw = (w + payload - 1u) / payload;
        const uint32_t ph = (h + payload - 1u) / payload;
        out.page_w[m] = pw;
        out.page_h[m] = ph;
        out.mip_offset[m] = out.entry_count;
        out.entry_count += pw * ph;
        ++mips;
        // The tail is the first mip whose atlas dimensions both fit the tail
        // budget; everything coarser than it is redundant (a single page
        // already covers the whole atlas).
        if (w <= chart_atlas::kVtTailDim && h <= chart_atlas::kVtTailDim) break;
    }
    out.atlas_w = atlas_w;
    out.atlas_h = atlas_h;
    out.mip_count = mips;
    return mips != 0;
}

// ---------------------------------------------------------------------------
// Registration cost + the fail-closed gate (pure CPU; VtResidency applies it)
// ---------------------------------------------------------------------------
// A usable surfaces()-tape classification needs both arrays and an exact
// per-vertex weight matrix; anything else fails closed to the TriEx materialId
// path. Shared by the cost model and register_variant so the two can never
// disagree about whether the tape columns are being copied.
inline bool vt_context_has_surface_tape(const VtPartContext& context) {
    return context.surface_material_count > 0 && context.surface_weights &&
           context.surface_materials && context.surface_material_count <= 8u &&
           context.vertex_count > 0;
}

// Exactly the CPU bytes register_variant will COPY for this registration (see
// its LIFETIME note): every stream VtPartContext points at, plus the chart
// table and triangle order of the atlas. This is the quantity
// MATTER_VT_MESH_BUDGET_MB budgets, so sizing that knob for a world means
// evaluating this function on one of its variants and multiplying.
//
// Per vertex, with everything present: 12 (positions) + 12 (normals) +
// 8 (surface_uvs) + 4 (material_ids) + 4 (tint_rgba) + surface_material_count
// (tape weight columns, one u8 each) = 40 + tape columns. Per triangle: 12.
inline size_t vt_variant_mesh_bytes(const chart_atlas::ChartAtlasRung& atlas,
                                    const VtPartContext& context) {
    const bool tape = vt_context_has_surface_tape(context);
    return static_cast<size_t>(context.vertex_count) *
               (sizeof(float) * 3 * (context.positions ? 1 : 0) +
                sizeof(float) * 3 * (context.normals ? 1 : 0) +
                sizeof(float) * 2 * (context.surface_uvs ? 1 : 0) +
                sizeof(uint32_t) * (context.material_ids ? 1 : 0) +
                4 * (context.tint_rgba ? 1 : 0) +
                (tape ? context.surface_material_count : 0) +
                // P2: per-vertex f16 field lanes (mode 3).
                (tape && context.surface_lanes
                     ? sizeof(uint16_t) * context.surface_lane_count
                     : 0)) +
           static_cast<size_t>(context.triangle_count) * 3 * sizeof(uint32_t) +
           static_cast<size_t>(context.material_count) *
               context.material_stride * sizeof(float) +
           (tape ? static_cast<size_t>(context.surface_material_count) *
                       sizeof(uint32_t)
                 : 0) +
           // P2: the canonical tape text copy (mode 3).
           (tape && context.surface_tape_text
                ? std::strlen(context.surface_tape_text)
                : 0) +
           (context.finite_source_ids ? size_t(context.vertex_count)*sizeof(uint32_t) : 0) +
           (context.finite_sources ? context.finite_sources->bytes() : 0) +
           atlas.charts.size() * sizeof(chart_atlas::ChartEntry) +
           atlas.tri_order.size() * sizeof(uint32_t);
}

// Why a registration was refused. Every non-Accept verdict means the part keeps
// rendering through the LEGACY per-material path: correct topology and (since
// the tape's per-vertex argmax is baked into the legacy vertex stream)
// correct classification, but flat per-material shading instead of composited
// pages — no detail tileset, no tier-2 AO. Before that argmax bake it was worse
// still: the refused far field rendered as one uniform material with a visible
// boundary against the VT'd near field.
enum class VtRejectReason {
    Accept = 0,
    NoLayer,      // every variant slot is taken (MATTER_VT_MAX_VARIANTS — a
                  // soft bookkeeping bound since the buffer indirection; the
                  // hardware layer wall is gone)
    MeshBudget,   // the CPU mesh copy would exceed MATTER_VT_MESH_BUDGET_MB
};

// The two capacity gates register_variant applies BEFORE it takes any slot, so
// a rejection can never leave a partial registration behind. Variant-slot
// exhaustion is checked first, matching register_variant's order. (A third
// gate — the indirection table arena, MATTER_VT_INDIRECTION_MB — lives inside
// register_variant; it is sized to be unreachable before the mesh budget.)
inline VtRejectReason vt_registration_verdict(uint32_t live_variants,
                                              uint32_t max_variants,
                                              size_t mesh_bytes_used,
                                              size_t mesh_budget_bytes,
                                              size_t mesh_bytes_wanted) {
    if (live_variants >= max_variants) return VtRejectReason::NoLayer;
    if (mesh_bytes_used + mesh_bytes_wanted > mesh_budget_bytes)
        return VtRejectReason::MeshBudget;
    return VtRejectReason::Accept;
}

// One indirection entry, as stored in the u32 entry word.
struct VtEntry {
    uint16_t slot = 0;        // physical page slot
    uint16_t mapped_mip = 0;  // the mip that slot's page actually holds
};

inline uint32_t vt_pack_entry(uint32_t slot, uint32_t mip) {
    return (slot & 0xFFFFu) | ((mip & 0xFFFFu) << 16);
}
inline VtEntry vt_unpack_entry(uint32_t packed) {
    return VtEntry{static_cast<uint16_t>(packed & 0xFFFFu),
                   static_cast<uint16_t>(packed >> 16)};
}

// Key for a virtual page within one variant rung.
struct VtPageKey {
    uint32_t mip = 0, px = 0, py = 0;
    bool operator<(const VtPageKey& o) const {
        if (mip != o.mip) return mip < o.mip;
        if (py != o.py) return py < o.py;
        return px < o.px;
    }
    bool operator==(const VtPageKey& o) const {
        return mip == o.mip && px == o.px && py == o.py;
    }
};

// ---------------------------------------------------------------------------
// Per-variant indirection mirror.
// ---------------------------------------------------------------------------
// Holds the CPU-side truth for one variant's exact-sized table. map()/unmap()
// only touch the small resident set; texels() rebuilds the whole table from
// that set, coarsest page first, so the result is a pure function of the
// resident set — no incremental-update aliasing bugs are possible. Rebuilds
// are bounded by the entry count (<= kVtMaxTableWords) and only happen when
// the table is dirty.
class VtIndirectionMap {
  public:
    void reset(const VtVariantLayout& layout, uint32_t tail_slot) {
        layout_ = layout;
        tail_slot_ = tail_slot;
        resident_.clear();
        texels_.assign(layout.entry_count, 0u);
        dirty_ = true;
    }

    const VtVariantLayout& layout() const { return layout_; }
    uint32_t tail_slot() const { return tail_slot_; }
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }
    size_t resident_count() const { return resident_.size(); }

    bool in_range(uint32_t mip, uint32_t px, uint32_t py) const {
        return layout_.valid() && mip < layout_.mip_count &&
               px < layout_.page_w[mip] && py < layout_.page_h[mip];
    }

    void map(uint32_t mip, uint32_t px, uint32_t py, uint32_t slot) {
        if (!in_range(mip, px, py)) return;
        resident_[VtPageKey{mip, px, py}] = slot;
        dirty_ = true;
    }
    void unmap(uint32_t mip, uint32_t px, uint32_t py) {
        if (resident_.erase(VtPageKey{mip, px, py}) != 0) dirty_ = true;
    }
    bool is_mapped(uint32_t mip, uint32_t px, uint32_t py) const {
        return resident_.find(VtPageKey{mip, px, py}) != resident_.end();
    }

    // Resolve exactly as vt_common.glsl does: one lookup, no walk.
    VtEntry resolve(uint32_t mip, uint32_t px, uint32_t py) const {
        const_cast<VtIndirectionMap*>(this)->rebuild_if_dirty();
        if (!in_range(mip, px, py))
            return VtEntry{static_cast<uint16_t>(tail_slot_),
                           static_cast<uint16_t>(layout_.mip_count
                                                     ? layout_.mip_count - 1u
                                                     : 0u)};
        return vt_unpack_entry(texels_[texel_index(mip, px, py)]);
    }

    // Packed u32 entries for the whole table (exactly layout().entry_count).
    const std::vector<uint32_t>& texels() {
        rebuild_if_dirty();
        return texels_;
    }

    static uint32_t texel_index_for(const VtVariantLayout& layout, uint32_t mip,
                                    uint32_t px, uint32_t py) {
        return layout.mip_offset[mip] + py * layout.page_w[mip] + px;
    }

  private:
    uint32_t texel_index(uint32_t mip, uint32_t px, uint32_t py) const {
        return texel_index_for(layout_, mip, px, py);
    }

    void rebuild_if_dirty() {
        if (!dirty_ || !layout_.valid()) return;
        const uint32_t tail_mip = layout_.mip_count - 1u;
        const uint32_t tail = vt_pack_entry(tail_slot_, tail_mip);
        texels_.assign(layout_.entry_count, tail);
        // Coarsest first, so a finer resident page always wins its region.
        for (auto it = resident_.rbegin(); it != resident_.rend(); ++it) {
            stamp(it->first, it->second);
        }
        dirty_ = false;
    }

    // Write (slot, key.mip) into key's own entry and every finer entry the
    // page covers.
    void stamp(const VtPageKey& key, uint32_t slot) {
        const uint32_t packed = vt_pack_entry(slot, key.mip);
        for (uint32_t f = 0; f <= key.mip; ++f) {
            const uint32_t shift = key.mip - f;
            const uint32_t x0 = key.px << shift;
            const uint32_t y0 = key.py << shift;
            const uint32_t x1 = std::min<uint32_t>((key.px + 1u) << shift,
                                                   layout_.page_w[f]);
            const uint32_t y1 = std::min<uint32_t>((key.py + 1u) << shift,
                                                   layout_.page_h[f]);
            for (uint32_t y = y0; y < y1; ++y) {
                uint32_t* row = texels_.data() + layout_.mip_offset[f] +
                                y * layout_.page_w[f];
                for (uint32_t x = x0; x < x1; ++x) row[x] = packed;
            }
        }
    }

    VtVariantLayout layout_{};
    uint32_t tail_slot_ = 0;
    std::map<VtPageKey, uint32_t> resident_;
    std::vector<uint32_t> texels_;
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// Indirection table sub-allocator (CPU bookkeeping for the GPU buffer).
// ---------------------------------------------------------------------------
// Pow-2 size classes with per-class free lists over a fixed word arena.
// Chosen over a general allocator because tables are SMALL (22 words for a
// 512^2 atlas, kVtMaxTableWords worst case) and never resize: rounding to the
// next power of two bounds internal fragmentation at 2x, blocks never split
// or merge so external fragmentation cannot exist, and a freed block is
// reusable by any later table of its class — the pathological case (a world
// of mixed table sizes churning for hours) converges to at most one free
// list per class, never to an unusable arena.
//
// GROWTH POLICY: none at runtime. The arena is pre-sized from
// MATTER_VT_INDIRECTION_MB (default 64 MiB = 16.7M words: thousands of
// worst-case tables, hundreds of thousands of typical ones — an order of
// magnitude past any working set the mesh budget admits). Growing live would
// mean a new VkBuffer + copy + re-pointing descriptors while in-flight
// frames still reference the old one, i.e. exactly the deferred-destroy
// hazard this redesign exists to remove. Exhaustion fails the registration
// closed (counted + warned) like every other capacity gate, and the demand
// pass simply retries later.
//
// RECYCLING: release() parks the block in a graveyard with a retire serial;
// collect(serial) moves matured blocks to their free list. Until collection
// the block's words (CPU and GPU copies) stay untouched, so an in-flight
// frame still sampling the dead variant reads exactly the bytes it was
// submitted against. Every acquire stamps a fresh generation; with debugging
// on, premature reuse aborts (see set_debug).
class VtTableAllocator {
  public:
    static constexpr uint32_t kMinBlockWords = 16u;
    static constexpr uint32_t kClassCount = 12u;   // 16 .. 32768 words
    static_assert((kMinBlockWords << (kClassCount - 1u)) >= kVtMaxTableWords,
                  "largest virtual table must fit an allocator class");

    void reset(uint32_t capacity_words) {
        capacity_ = capacity_words;
        bump_ = 0;
        used_words_ = 0;
        live_blocks_ = 0;
        generation_ = 0;
        for (auto& f : free_) f.clear();
        graveyard_.clear();
        debug_reuse_.clear();
    }

    void set_debug(bool debug) { debug_ = debug; }

    static uint32_t size_class(uint32_t words) {
        uint32_t c = 0;
        uint32_t size = kMinBlockWords;
        while (size < words && c + 1u < kClassCount) {
            size <<= 1u;
            ++c;
        }
        return c;
    }
    static uint32_t class_words(uint32_t c) { return kMinBlockWords << c; }

    // Acquires a block of at least `words` words. On success fills the block's
    // word offset, its (rounded) size and a fresh generation stamp. `frame` is
    // the caller's monotonic frame counter, used only for the debug audit.
    bool acquire(uint32_t words, uint64_t frame, uint32_t& offset_words,
                 uint32_t& block_words, uint64_t& generation) {
        if (words == 0 || words > class_words(kClassCount - 1u)) return false;
        const uint32_t c = size_class(words);
        block_words = class_words(c);
        if (!free_[c].empty()) {
            offset_words = free_[c].back();
            free_[c].pop_back();
        } else {
            if (bump_ + block_words > capacity_) return false;
            offset_words = bump_;
            bump_ += block_words;
        }
        if (debug_) {
            const auto found = debug_reuse_.find(offset_words);
            if (found != debug_reuse_.end() && frame < found->second)
                debug_fail("table block reused before its retire serial");
        }
        generation = ++generation_;
        used_words_ += block_words;
        ++live_blocks_;
        return true;
    }

    // Parks the block until `retire_serial` has passed on the frame clock.
    void release(uint32_t offset_words, uint32_t block_words,
                 uint64_t retire_serial) {
        graveyard_.push_back(Grave{offset_words, size_class(block_words),
                                   retire_serial});
        if (debug_) debug_reuse_[offset_words] = retire_serial;
        used_words_ -= std::min(used_words_, block_words);
        if (live_blocks_) --live_blocks_;
    }

    // Immediate free — legal ONLY for a block no frame ever referenced (a
    // registration rollback before any record/table upload named it).
    void release_now(uint32_t offset_words, uint32_t block_words) {
        free_[size_class(block_words)].push_back(offset_words);
        used_words_ -= std::min(used_words_, block_words);
        if (live_blocks_) --live_blocks_;
    }

    // Moves every graveyard block whose retire serial has passed to its free
    // list. `completed_serial` is the newest frame counter value that is known
    // retired ON THE GPU (the caller derives it from its frame fences).
    void collect(uint64_t completed_serial) {
        size_t keep = 0;
        for (size_t i = 0; i < graveyard_.size(); ++i) {
            if (graveyard_[i].retire_serial <= completed_serial) {
                free_[graveyard_[i].size_class].push_back(
                    graveyard_[i].offset_words);
                continue;
            }
            graveyard_[keep++] = graveyard_[i];
        }
        graveyard_.resize(keep);
    }

    uint32_t capacity_words() const { return capacity_; }
    uint32_t used_words() const { return used_words_; }
    uint32_t high_water_words() const { return bump_; }
    uint32_t live_blocks() const { return live_blocks_; }
    uint32_t graveyard_blocks() const {
        return static_cast<uint32_t>(graveyard_.size());
    }
    uint64_t generation() const { return generation_; }

  private:
    struct Grave {
        uint32_t offset_words = 0;
        uint32_t size_class = 0;
        uint64_t retire_serial = 0;
    };

    // Inline (not in the .cpp): the header-only CPU half must stay linkable
    // without the GPU translation unit — vt_residency_tests.cpp compiles
    // against this header alone.
    [[noreturn]] static void debug_fail(const char* what) {
        std::fprintf(stderr, "[vt] GENERATION AUDIT FAILED (table arena): %s\n",
                     what);
        std::fflush(stderr);
        std::abort();
    }

    uint32_t capacity_ = 0;
    uint32_t bump_ = 0;
    uint32_t used_words_ = 0;
    uint32_t live_blocks_ = 0;
    uint64_t generation_ = 0;
    bool debug_ = false;
    std::vector<uint32_t> free_[kClassCount];
    std::vector<Grave> graveyard_;
    std::map<uint32_t, uint64_t> debug_reuse_;   // offset -> earliest reuse
};

// ---------------------------------------------------------------------------
// Physical page slot pool with LRU eviction.
// ---------------------------------------------------------------------------
// Pinned slots (the per-variant resident tails) are never evicted. Unpinned
// slots evict strictly by ascending last_used, ties broken by ascending slot
// index, so eviction order is deterministic and testable.
//
// EVICTION HYSTERESIS: a slot used within the last protect_frames() frames
// is never an eviction candidate (default 1 = current frame only; the
// residency layer widens it via MATTER_VT_EVICT_PROTECT_FRAMES). Feedback-
// requested pages are touched when their request drains (queue_page), so
// everything recently asked for is ineligible; when nothing else is
// evictable, acquire() fails and the caller retries later while the
// indirection keeps serving the coarser resident coverage — degrade, never
// thrash. The window must be WIDER than one frame in practice: temporal
// jitter (DLSS) shifts which 8x8 feedback blocks sample which pages, so at a
// fixed camera a page can be requested only every few frames — with a
// one-frame window an oversubscribed pool ping-pongs pages forever instead
// of settling blurry-but-stable.
//
// RECYCLING: eviction hands a slot straight to its next owner — safe, because
// the new content arrives via queue-ordered copies that in-flight frames
// execute ahead of. release(slot, retire_serial) is for VARIANT DEATH, where
// the freed slot must stay untouched until no in-flight frame's draw records
// can still resolve into it; those slots age in a graveyard until collect().
// release_now() is the immediate form for slots provably unreferenced (a
// never-mapped rollback, or a caller that has wait_idle'd).
class VtSlotPool {
  public:
    struct Owner {
        bool     live = false;
        bool     pinned = false;
        uint64_t variant_key = 0;   // residency-defined; opaque here
        VtPageKey page{};
        uint64_t last_used = 0;
        uint64_t generation = 0;    // bumped on every acquire
    };

    void reset(uint32_t capacity) {
        owners_.assign(capacity, Owner{});
        free_.clear();
        free_.reserve(capacity);
        for (uint32_t i = capacity; i-- > 0;) free_.push_back(i);
        graveyard_.clear();
        reusable_at_.assign(capacity, 0u);
        used_ = 0;
        pinned_ = 0;
        evictions_ = 0;
        generation_ = 0;
        lru_scan_count_ = 0;
        lru_scan_ns_ = 0;
    }

    void set_debug(bool debug) { debug_ = debug; }
    // Hysteresis window width, in frames (>= 1). See EVICTION HYSTERESIS.
    void set_protect_frames(uint64_t frames) {
        protect_frames_ = frames ? frames : 1u;
    }
    uint64_t protect_frames() const { return protect_frames_; }

    uint32_t capacity() const { return static_cast<uint32_t>(owners_.size()); }
    uint32_t used() const { return used_; }
    uint32_t pinned() const { return pinned_; }
    uint64_t evictions() const { return evictions_; }
    uint64_t lru_scan_count() const { return lru_scan_count_; }
    uint64_t lru_scan_ns() const { return lru_scan_ns_; }
    uint32_t graveyard_slots() const {
        return static_cast<uint32_t>(graveyard_.size());
    }
    const Owner& owner(uint32_t slot) const { return owners_[slot]; }

    // Acquires a slot for (variant_key, page). On success `slot` receives the
    // index; when an occupied slot was recycled, `evicted` receives its old
    // owner (evicted.live == true) so the caller can unmap it. `frame` is the
    // current frame: slots last used at `frame` are never eviction victims
    // (see EVICTION HYSTERESIS above).
    bool acquire(uint64_t variant_key, VtPageKey page, bool pinned,
                 uint64_t frame, uint32_t& slot, Owner& evicted) {
        evicted = Owner{};
        if (owners_.empty()) return false;
        uint32_t chosen;
        if (!free_.empty()) {
            chosen = free_.back();
            free_.pop_back();
        } else {
            if (!pick_lru(frame, chosen)) return false;
            evicted = owners_[chosen];
            ++evictions_;
            --used_;
        }
        if (debug_ && chosen < reusable_at_.size() &&
            frame < reusable_at_[chosen]) {
            debug_fail("page slot reused before its retire serial");
        }
        Owner& o = owners_[chosen];
        o.live = true;
        o.pinned = pinned;
        o.variant_key = variant_key;
        o.page = page;
        o.last_used = frame;
        o.generation = ++generation_;
        ++used_;
        if (pinned) ++pinned_;
        slot = chosen;
        return true;
    }

    void touch(uint32_t slot, uint64_t frame) {
        if (slot < owners_.size() && owners_[slot].live)
            owners_[slot].last_used = frame;
    }

    // Variant-death release: the slot ages in the graveyard until
    // collect(serial >= retire_serial) and only then re-enters the free list.
    void release(uint32_t slot, uint64_t retire_serial) {
        if (slot >= owners_.size() || !owners_[slot].live) return;
        if (owners_[slot].pinned) --pinned_;
        owners_[slot] = Owner{};
        graveyard_.push_back(Grave{slot, retire_serial});
        if (slot < reusable_at_.size()) reusable_at_[slot] = retire_serial;
        --used_;
    }

    // Immediate release — legal ONLY when no in-flight frame can resolve into
    // this slot (a never-mapped rollback, or the caller has wait_idle'd, as
    // invalidate_all_content's contract requires).
    void release_now(uint32_t slot) {
        if (slot >= owners_.size() || !owners_[slot].live) return;
        if (owners_[slot].pinned) --pinned_;
        owners_[slot] = Owner{};
        free_.push_back(slot);
        --used_;
    }

    // Returns matured graveyard slots to the free list.
    void collect(uint64_t completed_serial) {
        size_t keep = 0;
        for (size_t i = 0; i < graveyard_.size(); ++i) {
            if (graveyard_[i].retire_serial <= completed_serial) {
                free_.push_back(graveyard_[i].slot);
                continue;
            }
            graveyard_[keep++] = graveyard_[i];
        }
        graveyard_.resize(keep);
    }

    // Evictable count = live, unpinned slots outside the hysteresis window.
    uint32_t evictable(uint64_t frame) const {
        uint32_t n = 0;
        for (const Owner& o : owners_)
            if (o.live && !o.pinned && o.last_used + protect_frames_ <= frame)
                ++n;
        return n;
    }

  private:
    struct Grave {
        uint32_t slot = 0;
        uint64_t retire_serial = 0;
    };

    // Inline for the same header-only-linkability reason as the allocator's.
    [[noreturn]] static void debug_fail(const char* what) {
        std::fprintf(stderr, "[vt] GENERATION AUDIT FAILED (slot pool): %s\n",
                     what);
        std::fflush(stderr);
        std::abort();
    }

    bool pick_lru(uint64_t frame, uint32_t& out) const {
        const auto start = std::chrono::steady_clock::now();
        ++lru_scan_count_;
        bool found = false;
        uint32_t best = 0;
        uint64_t best_used = 0;
        for (uint32_t i = 0; i < owners_.size(); ++i) {
            const Owner& o = owners_[i];
            if (!o.live || o.pinned) continue;
            // Hysteresis: never evict what was touched/requested inside the
            // protection window.
            if (o.last_used + protect_frames_ > frame) continue;
            if (!found || o.last_used < best_used) {
                found = true;
                best = i;
                best_used = o.last_used;
            }
        }
        lru_scan_ns_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
        if (found) out = best;
        return found;
    }

    std::vector<Owner> owners_;
    std::vector<uint32_t> free_;
    std::vector<Grave> graveyard_;
    std::vector<uint64_t> reusable_at_;   // debug: earliest legal reuse frame
    uint32_t used_ = 0;
    uint32_t pinned_ = 0;
    uint64_t evictions_ = 0;
    mutable uint64_t lru_scan_count_ = 0;
    mutable uint64_t lru_scan_ns_ = 0;
    uint64_t generation_ = 0;
    uint64_t protect_frames_ = 1;
    bool debug_ = false;
};

// ---------------------------------------------------------------------------
// Feedback packing (must match vt_feedback.comp).
// ---------------------------------------------------------------------------
// Two four-u16 requests per 8x8 screen block, in contiguous receiver/material
// planes extracted from the full-resolution RGBA32_UINT visibility image:
//   x = vt slot (variant index + 1, 0 = no request), y = page_x, z = page_y,
//   w = mip. The 16-bit transported slot is why MATTER_VT_MAX_VARIANTS tops out at
//   65534.
struct VtFeedbackRequest {
    uint32_t layer = 0;   // variant slot index
    uint32_t mip = 0;
    uint32_t px = 0, py = 0;
};

// ---------------------------------------------------------------------------
// The GPU-facing residency runtime.
// ---------------------------------------------------------------------------
// Owns every GPU resource the VT system has: the kVtChannelCount pool images
// and their samplers, the device-local indirection SSBO plus its per-frame
// staging ring, the host-visible per-variant record buffer, the feedback image
// and its readback ring, and the installed VtPageFiller / VtPageEnricher.
//
// LIFECYCLE. Constructed empty; init(device, error) allocates everything and
// is idempotent (no-op once ready). It fails closed — on error nothing is
// partially bound and available() stays false, and the renderer then runs with
// no VT at all rather than half of one. shutdown() releases the resources;
// the object is non-copyable and non-movable (deleted copy ops).
//
// PER-FRAME CALL ORDER, all of it required and order-dependent:
//   begin_frame(frame_index, frame_slot)   // consumes readback slot, collects
//                                          // the graveyards, refreshes budgets
//   ensure_feedback(raster_w, raster_h, error)
//   record_feedback_clear(cmd)             // before the G-buffer pass
//   record_frame(cmd, error)               // before the G-buffer pass
//   ... G-buffer pass ...
//   record_feedback_readback(cmd)          // after the G-buffer pass
// `frame_slot` must name a readback slot whose previous submission has already
// completed — the caller's frame fence is what guarantees that.
//
// THREADING. There is no internal locking anywhere in this class. Everything
// — registration, release, the per-frame calls — runs on the thread that
// records the frame.
//
// The correctness contracts that are easy to break live in the file header
// above: GPU-timeline recycling (nothing freed is reusable until its retire
// serial matures) and tail-gated activation (slot_active() must gate every
// routing of a draw through the VT path). Read both before touching release,
// eviction or fill mapping.
class VtResidency {
  public:
    struct Stats {
        uint32_t variants = 0;          // LAYERS live (M6: not (hash,rung)s)
        // M6: registrations that found an existing layer with the same
        // parameterisation and took a reference instead of building a second
        // one, and rebuilds a finer rung forced.
        //
        // shared_refs is the milestone's own measurement: it is how many
        // duplicate page sets are NOT being fetched. It is nonzero even with
        // MATTER_VT_UNIFY off, because part_store clamps a cluster with fewer
        // levels to its last one (use_li = min(li, levels-1)) and those rungs
        // gather identical triangles, hence an identical chart table. Those
        // were N separate layers holding N copies of one page set before the
        // parameterisation key collapsed them.
        uint64_t shared_refs_total = 0;
        uint64_t finer_rebuilds_total = 0;
        uint32_t max_variants = 0;      // MATTER_VT_MAX_VARIANTS (soft bound)
        uint64_t mesh_budget_bytes = 0; // MATTER_VT_MESH_BUDGET_MB, in bytes
        uint32_t pool_capacity = 0;
        uint32_t replacement_reserve_pages = 0; // included in pool_capacity/bytes
        uint32_t dirty_pages = 0; // durable resident work, independent of queue cap
        uint64_t fills_stale_total = 0;
        uint32_t pool_used = 0;
        uint32_t material_pages = 0; // unique color/normal/ORM/height allocations
        uint32_t coverage_only_pages = 0; // receiver AUX/geometry without private material pixels
        uint32_t occlusion_pages = 0; // published immutable receiver factors
        uint32_t occlusion_retained_pages = 0; // includes earlier GPU readers and unpublished writes
        uint64_t occlusion_allocated_bytes = 0; // driver allocations, including free space inside live slabs
        uint64_t enrich_deferred_total = 0;
        uint32_t module_variants = 0; // independent material owners, included in variants
        uint64_t module_reuses_total = 0;
        uint32_t shared_material_references = 0; // references saved by sharing
        uint64_t surface_pairs_compiled_total = 0;
        uint64_t surface_table_uploads_total = 0;
        uint32_t surface_link_tables = 0;
        uint64_t surface_link_table_bytes = 0; // live connection-table payload; same size on CPU/GPU
        uint32_t material_read_pages = 0; // distinct material allocations held by composition reads
        uint32_t pool_pinned = 0;
        uint32_t fills_last_frame = 0;
        uint32_t requests_last_frame = 0;
        uint32_t queue_depth = 0;
        // Sampled after frame selection/retries, once per record_frame.
        uint32_t mandatory_queue_depth = 0;
        uint32_t detail_queue_depth = 0;
        uint64_t oldest_mandatory_age_frames = 0;
        uint64_t oldest_detail_age_frames = 0;
        uint64_t fills_total = 0;
        uint64_t evictions_total = 0;
        uint64_t lru_scan_count = 0;    // full-pool victim selections attempted
        uint64_t lru_scan_ns = 0;       // cumulative CPU time inside pick_lru
        uint64_t pool_bytes = 0;
        uint64_t mesh_bytes = 0;        // CPU copies held for the filler
        uint32_t rejected_variants = 0; // fell back to legacy (budget/slots)
        uint64_t invalidations_total = 0;  // nonempty owner-invalidation batches
        uint64_t pages_dropped_total = 0;  // pages those calls dropped
        // Requests the filler dispatched but did not write (see the map-or-
        // rollback path in record_frame). Nonzero means pages are NOT going
        // black, but something upstream is refusing work -- watch it.
        uint64_t fills_failed_total = 0;
        // Page requests discarded by the queue_cap trim. Expected to be
        // non-zero -- that is the cap doing its job. A value that dwarfs
        // requests_last_frame means the cap is too tight for the working set.
        uint64_t requests_dropped_total = 0;
        // --- WP-H: tier-2 hemisphere enrichment ---
        uint32_t enrich_samples = 0;        // rays/texel; 0 = no enricher
        uint32_t enrich_last_frame = 0;     // pages enriched in the last frame
        uint32_t enrich_queue_depth = 0;    // pages waiting for tier 2
        uint32_t enriched_pages = 0;        // resident slots currently at tier 2
        uint64_t enrich_total = 0;          // pages enriched since startup
        uint64_t enrich_dropped_total = 0;  // candidates dropped before running
        // Pages never queued because their texel is coarser than the contact
        // scale tier 2 bakes (the coarse-page skip in queue_enrich).
        uint64_t enrich_skipped_coarse_total = 0;
        // --- Buffer indirection (the 2048-layer-wall replacement) ---
        uint64_t indirection_capacity_bytes = 0;  // MATTER_VT_INDIRECTION_MB
        uint64_t indirection_used_bytes = 0;      // live table blocks
        uint32_t tables_live = 0;                 // live table blocks
        // Fence-deferred recycling census: resources freed but not yet past
        // the retirement horizon.
        uint32_t graveyard_tables = 0;
        uint32_t graveyard_slots = 0;
        uint32_t graveyard_layers = 0;
        // Table uploads that missed this frame's staging window (they stay
        // queued; a persistent climb means the staging ring is undersized).
        uint64_t table_uploads_deferred_total = 0;
        // Renderer hook timings, filled by VkSceneRenderer::vt_stats(). These
        // are CPU wall times, including allocation/waits and command recording;
        // they are not GPU timestamps. Direct residency users leave them zero.
        uint64_t cpu_frame_serial = 0;
        double cpu_demand_ms = 0;
        double cpu_begin_ms = 0;
        double cpu_pre_pass_ms = 0;
        double cpu_post_pass_ms = 0;
    };

    VtResidency();
    ~VtResidency();
    VtResidency(const VtResidency&) = delete;
    VtResidency& operator=(const VtResidency&) = delete;

    // Allocates the pool, indirection buffer, sampler and readback ring. Safe
    // to call repeatedly (no-op once ready). Fails closed: on error nothing is
    // left partially bound and available() stays false.
    bool init(matter::VulkanDevice& vulkan, std::string& error);
    void shutdown();
    bool available() const { return ready_; }

    // Descriptor handles (valid only once available()).
    VkImageView pool_view(uint32_t channel) const;
    VkBuffer    indirection_buffer() const { return indirection_buffer_.buffer; }
    VkDeviceSize indirection_buffer_size() const {
        return indirection_buffer_.size;
    }
    VkSampler   pool_sampler() const { return pool_sampler_; }
    VkSampler   point_sampler() const { return point_sampler_; }
    VkBuffer    variant_buffer() const { return variant_buffer_.buffer; }
    // One VtPageMetadata (80 bytes) per receiver page slot.
    VkBuffer input_snapshot_buffer() const { return input_snapshot_buffer_.buffer; }
    VkDeviceSize input_snapshot_buffer_size() const { return input_snapshot_buffer_.size; }
    // changed_material_ids covers every effective draw/compositor input that
    // differs from the preceding desired snapshot. Staging also dirties those
    // dependencies; clean unaffected pages can reuse their bytes immediately.
    // A table index cannot be reused while pages or retiring readers own it.
    bool set_input_snapshot(std::shared_ptr<const VtInputSnapshot> snapshot,
                            const std::vector<uint32_t>& changed_material_ids,
                            VtInvalidationReason reason = VtInvalidationReason::MaterialInputs);
    // Only residency-owned versions are returned. Frame descriptor captures
    // must not perpetually keep one another's obsolete versions registered.
    std::array<std::shared_ptr<const VtInputSnapshot>, kVtMaxInputSnapshots>
        active_input_snapshots() const;
    void set_input_update_pending(bool pending) { input_update_pending_ = pending; }
    bool input_update_pending() const { return input_update_pending_; }
    VkDeviceSize variant_buffer_size() const { return variant_buffer_.size; }
    VkImageView feedback_view() const { return feedback_source_view_; }
    uint32_t    feedback_width() const { return feedback_w_; }
    uint32_t    feedback_height() const { return feedback_h_; }

    // Registers one (variant, rung). Returns the transport slot (index + 1),
    // or kVtNoSlot when the rung has no charts / the layout is unusable / no
    // variant slot or table block is free / the mesh budget is spent.
    // Idempotent per (hash, rung). A compatible finer mesh refreshes the same
    // owner in place, preserving aliases and coverage. Peak old + staged CPU
    // mesh bytes must fit the budget; rejection preserves the existing owner.
    //
    // LIFETIME: the chart table and every array `context` points at are COPIED
    // here. The caller may free its own storage the moment this returns; the
    // VtPartContext the filler receives points at this object's copies, which
    // live exactly as long as the registration. The copies are what makes the
    // "borrowed, outlives every queued fill" clause of vt_types.h true, and
    // they are budgeted: MATTER_VT_MESH_BUDGET_MB (default 1024) caps the
    // total, and a registration that would exceed it fails closed to the
    // legacy path rather than growing without bound across a streamed world.
    //
    // A rejection is NOT silent: it bumps Stats::rejected_variants and the
    // first one logs a warning naming MATTER_VT_MAX_VARIANTS and
    // MATTER_VT_MESH_BUDGET_MB. Rejections are how a streamed world ends up
    // half-VT'd (near field classified, far field uniform legacy shading with
    // a visible boundary), so they must be visible in the census, not inferred
    // from pixels.
    uint32_t register_variant(uint64_t variant_hash, uint32_t rung,
                              const chart_atlas::ChartAtlasRung& atlas,
                              const VtPartContext& context);
    // Inputs come from vt_make_periodic_material. Failure preserves out. A new
    // lease is a candidate: retain the previous published lease until this
    // one's binding becomes nonzero. Never publish its unfilled tail.
    bool acquire_material_module(const std::shared_ptr<const VtPartSnapshot>&,
        VtMaterialModuleLease& out, std::string& error);
    VtMaterialModuleBinding material_module_binding(const VtMaterialModuleLease&) const;
    // Render-thread composition dependency admission. Bounds include all
    // operator support in unwrapped normalized module coordinates. Missing or
    // dirty exact-mip pages are queued; Pending never substitutes a coarse base.
    // Invalid inputs are rejected. Both non-ready results clear out.
    VtMaterialReadStatus acquire_material_read(const VtMaterialModuleLease&,
        uint32_t mip, const VtMaterialReadBounds&, VtMaterialReadLease& out, std::string& error);
    bool material_read_current(const VtMaterialReadLease&) const;
    // Call immediately before recording each GPU use. Revalidates dependencies
    // and extends pixel retention through the normal GPU reader horizon. The
    // caller must separately arrange sampled-image layouts/barriers. This API
    // records no commands and introduces no GPU submit, wait or texture copy.
    bool retain_material_read(const VtMaterialReadLease&);
    // Bind every chart in one immutable publication. Pending module tails and
    // failures preserve displayed bindings. The descriptor retains its modules.
    // Applies to the canonical parameterization owner (including rung aliases).
    // Geometry/surface edits require rebinding; older pages retain their old
    // mapping, while new unmatched pages use their complete finite fallback.
    bool bind_receiver_materials(uint64_t variant_hash, uint32_t rung,
        const std::vector<VtReceiverMaterialChart>&, std::string& error);

    // Releases every rung of the variant. The CPU mesh copies die immediately
    // (every recorded fill has already staged what it reads); the variant
    // slot, its GPU record, its indirection table block and its page slots age
    // in the graveyard for kVtRetireHorizonFrames so no in-flight frame's
    // draw records can resolve into recycled state (see the header note).
    void release_variant(uint64_t variant_hash);
    // Per-rung release drops one alias. The mesh/pages retire only after the
    // last alias releases the owner, using the same graveyard discipline.
    void release_variant(uint64_t variant_hash, uint32_t rung);
    uint32_t slot_for(uint64_t variant_hash, uint32_t rung) const;
    // Read-only admission/edit queries. Aliases spend no new layer; only a
    // finer canonical mesh needs staged CPU capacity. Invalid slot -> UINT32_MAX.
    uint32_t compatible_owner_slot(uint64_t variant_hash,
        const chart_atlas::ChartAtlasRung& atlas) const;
    uint32_t canonical_rung_for_slot(uint32_t slot) const;
    // Only the current completed tail may admit new cross-owner connections.
    // Returned leases retain the exact CPU/device geometry through replacement;
    // callers must revalidate before publishing or recording a new use.
    std::shared_ptr<const VtSurfaceBoundarySource> surface_boundary_source(uint32_t slot) const;
    bool surface_boundary_source_current(const VtSurfaceBoundarySource&) const;
    bool set_surface_connections(const std::vector<VtSurfaceConnectionPair>&,std::string& error);
    // Only the POM surface walk consumes these cross-sector tables. Preserve
    // requests while disabled so live re-enabling can publish current inputs.
    void set_surface_walk_enabled(bool enabled) { surface_walk_enabled_ = enabled; }


    // WP-F: replace one registered (variant, rung)'s surfaces()-tape
    // classification in place (owned copies + repointed context fields).
    // weights must be vertex_count * material_count bytes; material_count of
    // 0 (or an empty tape) strips the classification, reverting the rung to
    // the TriEx materialId path. Returns false when the (hash, rung) is not
    // registered or the sizes disagree with the stored mesh.
    //
    // CALLER CONTRACT: render thread, outside fill/enrich recording. Supply
    // weights/lanes for canonical_rung_for_slot(slot_for(hash, rung)), since
    // an alias can share another rung's prepared mesh. This method refreshes
    // that owner's surface preparation; the caller must dirty its resident
    // content with invalidate_owners after completing an edit bracket.
    // content_changed optionally distinguishes an accepted identical update
    // from a changed input, so callers can avoid unnecessary invalidation.
    // P2 appends (all defaulted so older callers keep compiling): the
    // canonical tape text plus the per-vertex f16 field lanes for the GPU
    // interpreter (weight-seam mode 3). Null text keeps the rung on mode 2.
    // lanes must be vertex_count * lane_count halves when lane_count != 0.
    // The stored surface_tape_hash is SALTED with the weight-seam mode and
    // kVtBakeVersion (vt_page_content_salt) — the P2 content-key fold.
    // Optional row-major float[12] receiver frame updates anchoring atomically
    // with the source snapshot; null preserves the current frame.
    bool update_variant_surface(uint64_t variant_hash, uint32_t rung,
                                const uint8_t* weights, size_t weight_bytes,
                                const uint32_t* materials,
                                uint32_t material_count, uint64_t tape_hash,
                                const char* tape_text = nullptr,
                                const uint16_t* lanes = nullptr,
                                uint32_t lane_count = 0,
                                bool* content_changed = nullptr,
                                const float* local_to_world = nullptr,
                                uint32_t world_anchored = 0);
    // Same edit-bracket/invalidate_owners contract as update_variant_surface.
    // Catalogs are immutable/shared; selector IDs are copied into a new
    // snapshot. Passing an empty catalog and zero IDs removes the binding.
    bool update_variant_finite_sources(uint64_t variant_hash,uint32_t rung,
        std::shared_ptr<const VtFiniteSources> sources,const uint32_t* ids,
        size_t id_count,bool* content_changed=nullptr);

    // Declares every page currently in the pool stale, because what the
    // INSTALLED FILLER bakes from has changed (a detail tileset slot was
    // loaded/evicted, the material table was edited). Rebinding the filler's
    // inputs alone only fixes FUTURE fills; the pages already resident — and
    // in particular the pinned per-variant tails, which never expire — keep
    // the content they were baked from, forever.
    //
    // Keeps every resident page and marks it durably dirty. Candidates are
    // produced in a bounded scratch reserve and copied to final slots only
    // after successful recording and generation validation. Old content and
    // readiness survive refusals; ordinary invalidation releases no page.
    // Returns the number of resident DETAIL pages scheduled (tails excluded).
    // Invalidation itself requires no idle wait; the caller still owns input
    // resource lifetime and must bind compatible inputs before candidate work.
    uint32_t invalidate_all_content();
    // Owner-scoped equivalents of invalidate_all_content. Transport slots are deduplicated and dead or
    // absent owners are ignored. Material dependencies include scalar fallback
    // IDs and surfaces() IDs even when that material is not loaded yet.
    uint32_t invalidate_owners(const std::vector<uint32_t>& transport_slots,
        VtInvalidationReason reason = VtInvalidationReason::Explicit);
    uint32_t invalidate_material_content(const std::vector<uint32_t>& material_ids,
        VtInvalidationReason reason = VtInvalidationReason::MaterialInputs);

    // ---- per-frame ----
    // Called once per frame before recording. `frame_slot` selects the
    // readback buffer whose previous submission has already completed (the
    // caller's frame fence guarantees this), so its contents are consumed
    // here. Also collects the graveyards: anything freed at least
    // kVtRetireHorizonFrames ago becomes allocatable again.
    void begin_frame(uint64_t frame_index, uint32_t frame_slot);
    // Ensures paired readback matches the current kVtFeedbackFormat attachment.
    // Invalid attachments and allocation failures return false (feedback stops,
    // while resident tails still render).
    bool ensure_feedback(const matter::VkImageResource& visible_feedback,
                          std::string& error);
    // Records: pool/indirection transitions, bounded fills through the filler,
    // indirection + variant-table uploads. Call before the G-buffer pass.
    bool record_frame(VkCommandBuffer cmd, std::string& error);
    // Records the feedback readback copy. Call after the G-buffer pass.
    void record_feedback_readback(VkCommandBuffer cmd,
                                   matter::VkImageResource& visible_feedback);
    // Records the feedback clear. Call before the G-buffer pass.

    void set_filler(std::unique_ptr<VtPageFiller> filler);
    VtPageFiller* filler() const { return filler_.get(); }

    // WP-H: install the tier-2 enricher. OPTIONAL — with none installed no page
    // is ever queued for enrichment and every page stays tier-1 (which is
    // correct, just flatter). The renderer installs one only when
    // vulkan.ray_tracing_available(). Takes ownership; safe to call before or
    // after variants are registered, but only while no enrichment can be
    // unretired (the renderer does it at runtime startup).
    void set_enricher(std::unique_ptr<VtPageEnricher> enricher);
    VtPageEnricher* enricher() const { return enricher_.get(); }
    uint32_t max_enrich_per_frame() const { return max_enrich_per_frame_; }

    const Stats& stats() const { return stats_; }
    uint32_t max_fills_per_frame() const { return max_fills_per_frame_; }
    uint32_t max_tail_fills_per_frame() const {
        return max_tail_fills_per_frame_;
    }

    // TAIL GATE query for the draw side (see the header note): true once this
    // transported slot's variant is live AND its tail fill is guaranteed
    // visible to any draw recorded from here on. vt_slot_for_lod() must route
    // a draw through the VT path only when this returns true.
    bool slot_active(uint32_t transport_slot) const {
        if (transport_slot == kVtNoSlot || !ready_) return false;
        const uint32_t layer = transport_slot - 1u;
        if (layer >= variants_.size()) return false;
        const VariantRung& v = variants_[layer];
        return vt_slot_activation_rule(v.live, v.tail_ready_serial,
                                       frame_index_);
    }
    // True once when any variant's activation state changed since the last
    // call — the renderer's cue to rebuild its (cluster, lod) -> vt_slot
    // table (which is otherwise only rebuilt on registration/release).
    bool consume_activation_dirty() {
        const bool dirty = activation_dirty_;
        activation_dirty_ = false;
        return dirty;
    }


    // TEST SEAM: inject feedback requests without a GPU readback.
    void inject_feedback_for_test(const VtFeedbackRequest* requests,
                                  size_t count);
    // TEST SEAM: verifies that a registered context still borrows every
    // pointer from its own VariantRung after later registrations grow the
    // slot table. This is deliberately non-dereferencing so a failure reports
    // the lifetime break before a driver-facing fill or enrichment can use it.
    bool context_storage_owned_for_test(uint32_t transport_slot) const;
    // The address actually published in the draw-side variant record.
    uint64_t surface_link_address_for_test(uint32_t slot) const {
        if(!slot || slot>variant_records_.size())return 0;
        const auto& record=variant_records_[slot-1];
        return uint64_t(record.surface_links_low) | (uint64_t(record.surface_links_high)<<32);
    }
    // Validate indices before a forced refresh can dereference a stale queue
    // lookup. Used by the production-queue lifetime regression fixture.
    bool queued_requests_consistent_for_test() const;
    // TEST SEAM: shrink one live owner's indirection RANGE to its finest
    // mip_count mips, keeping its table size and generation, so pages it
    // already owns (dirty, or in flight) fall outside it and a forced
    // queue_page declines them. No shipped path narrows a live owner.
    void narrow_indirection_for_test(uint32_t transport_slot, uint32_t mip_count) {
        if (!transport_slot || transport_slot > variants_.size()) return;
        VariantRung& v = variants_[transport_slot - 1u];
        if (!v.live || mip_count == 0 || mip_count >= v.layout.mip_count) return;
        VtVariantLayout narrow = v.layout;
        narrow.mip_count = mip_count;
        v.indirection.reset(narrow, v.tail_slot);
    }
    // Keep production feedback, dirtiness and rendering active while a test
    // delays replacement work. Existing mappings must remain usable.
    void pause_page_fills_for_test(bool paused) { page_fills_paused_for_test_ = paused; }
    uint32_t resident_page_slot_for_test(uint32_t transport_slot, VtPageKey page) const {
        if (!transport_slot || transport_slot > variants_.size()) return UINT32_MAX;
        const auto& v = variants_[transport_slot - 1u];
        if (!v.live || !v.indirection.is_mapped(page.mip, page.px, page.py)) return UINT32_MAX;
        return v.indirection.resolve(page.mip, page.px, page.py).slot;
    }
    uint32_t material_page_slot_for_test(uint32_t receiver_page) const {
        return receiver_page < slot_page_metadata_.size()
            ? slot_page_metadata_[receiver_page].material_slot : UINT32_MAX;
    }
    bool coverage_only_page_for_test(uint32_t receiver_page) const {
        return receiver_page<slot_page_metadata_.size() &&
            (slot_page_metadata_[receiver_page].geometry.page_flags&kVtCoverageOnly)!=0;
    }
    uint64_t occlusion_address_for_test(uint32_t receiver_page) const {
        return receiver_page<slot_page_metadata_.size()?slot_page_metadata_[receiver_page].occlusion_address:0;
    }

  private:
    // One pool channel (or the feedback image): a layered 2D image plus the
    // memory backing it. `layout` MIRRORS the image's current Vulkan layout —
    // record_frame reads it to decide which transitions to emit, so anything
    // that transitions the image must update it. `edge` is the layer's square
    // edge in texels (kVtPoolLayerEdgeTexels for pool channels).
    struct PoolImage {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t layers = 0;
        uint32_t edge = 0;
        VkDeviceSize tracked_alloc_size = 0;
    };

    // One registered VT layer: the CPU truth for a (variant, rung) — or, since
    // M6, for the set of (hash, rung) aliases that share one parameterisation.
    // Indexed by variant slot in variants_; the transported slot the draw side
    // carries is (index + 1), because 0 means "no VT".
    //
    // It owns COPIES of everything the filler will read (the chart atlas plus
    // every mesh/tape stream VtPartContext points at) and `context` is
    // repointed at those copies, which is what makes VtPartContext's "borrowed
    // storage outlives every queued fill" guarantee true. Those copies are
    // what MATTER_VT_MESH_BUDGET_MB budgets, and mesh_bytes records this
    // layer's share of it.
    //
    // Teardown is refcounted (alias_refs) and fence-deferred: the mesh copies
    // die immediately at release, but the slot, its table block and its page
    // slots age in the graveyard for kVtRetireHorizonFrames first.
    struct VariantRung {
        uint64_t variant_hash = 0;
        uint32_t rung = 0;
        uint32_t layer = 0;               // variant slot index
        // M6: the key this layer is registered under, and how many (hash,rung)
        // ALIASES resolve to it.
        //
        // Before M6 these were implied: the key was variant_key(hash, rung) and
        // exactly one rung ever pointed at a layer. With one parameterisation
        // per part, every rung of a part resolves to the SAME layer, and the
        // renderer still releases per rung (VkSceneRenderer::evict_vt_rung off
        // record.vt_slots[rung]). Without the refcount, evicting one rung frees
        // a layer the others still name — a dangling variant slot, which is GPU
        // use-after-free, in the subsystem that already produced one
        // DEVICE_LOST investigation. The layer is torn down (graveyard and all)
        // only when the LAST alias goes.
        uint64_t param_key = 0;
        uint32_t alias_refs = 0;
        VtVariantLayout layout{};
        VtIndirectionMap indirection;
        // Immutable CPU inputs retained by any preparation jobs. Surface
        // edits replace only their snapshot and continue sharing geometry.
        std::shared_ptr<const VtPartSnapshot> inputs;
        std::shared_ptr<const VtSurfaceBoundarySource> boundary_source;
        std::shared_ptr<VtReceiverMaterialState> material_candidate, material_published;
        std::vector<uint32_t> material_dependencies;
        size_t mesh_bytes = 0;
        uint32_t tail_slot = 0;
        bool tail_filled = false;
        bool live = false;
        // First frame at which the draw side may route through the VT path
        // (kVtTailNotReady until the tail fill maps; see the tail gate note).
        // Deliberately NOT reset by in-place tail re-fills (invalidations):
        // the slot then still holds the previous VALID bytes until the
        // re-fill's queue-ordered copy lands, so there is no unwritten window.
        uint64_t tail_ready_serial = kVtTailNotReady;
        // Buffer-indirection bookkeeping: the variant's table block inside the
        // indirection SSBO, its allocation generation, and whether the GPU copy
        // has ever been written (a never-uploaded table outranks re-uploads in
        // record_frame — sampling it would read stale arena bytes).
        uint32_t table_offset_words = 0;
        uint32_t table_block_words = 0;
        uint64_t table_generation = 0;
        uint64_t content_revision = 1; // desired content; validated before candidate publication
        bool table_uploaded = false;
        // WP-H: the highest texels_per_meter over this rung's charts, i.e. the
        // SMALLEST page-texel size the rung has. Used to decide whether a page
        // is fine enough for tier-2 enrichment to contribute anything at all
        // (see queue_enrich); taking the max makes the skip conservative — a
        // page is only skipped when no chart on it could benefit, and the
        // shader's per-texel fade handles the coarser charts on a mixed page.
        float finest_texels_per_meter = 0.0f;
    };

    static void copy_variant_mesh(VariantRung& target,
        const chart_atlas::ChartAtlasRung& atlas, const VtPartContext& context);
    static void swap_variant_mesh(VariantRung& a, VariantRung& b);
    friend class VtMaterialModule;
    uint32_t register_variant_impl(uint64_t, uint32_t,
        const chart_atlas::ChartAtlasRung&, const VtPartContext&);
    void release_material_module(const VtMaterialModule&);
    void publish_receiver_materials();
    void set_slot_material_mapping(uint32_t, std::shared_ptr<VtReceiverMaterialState>);
    void retire_slot_material_mapping(uint32_t);
    std::shared_ptr<VtModuleOwner> module_owner_;
    std::map<uint64_t,std::weak_ptr<const VtMaterialModule>> modules_;

    bool create_pool_image(uint32_t channel, VkFormat format, uint32_t layers,
                           std::string& error);
    void destroy_pool_image(PoolImage& image);
    // Re-reads the live half of matter::vt_residency_budgets(). Called at init
    // and once per begin_frame.
    void refresh_budgets();
    void write_variant_record(const VariantRung& v);
    // Counts a fail-closed-to-legacy registration and warns ONCE, naming the
    // env knobs. `wanted_bytes` is the mesh copy the rejected registration
    // would have taken (0 when a slot/table, not the budget, was the limit).
    void note_rejection(const char* reason, size_t wanted_bytes);
    // Shared teardown for both release_variant overloads: parks the variant
    // slot, table block and page slots in the graveyard and frees the mesh
    // copy of one (variant, rung) key. Returns false when the key is not
    // registered. Does NOT refresh the pool-level stats lines — callers do,
    // once, after their sweep.
    bool release_variant_key(uint64_t key);
    // M6: drop one (hash, rung) reference; tears the layer down only when it
    // was the last. Returns true iff the layer was actually released.
    bool release_rung_alias(uint64_t alias);
    // `force` queues a page even when the indirection already maps it —
    // the pinned tail is mapped at registration but still needs its fill.
    void queue_page(VariantRung& v, VtPageKey page, bool force = false,
                    uint32_t preassigned_slot = 0xFFFFFFFFu);
    void drain_feedback(uint32_t frame_slot);
    void refresh_indirection_stats();
    void remove_material_dependencies(VariantRung& variant);
    void rebuild_material_dependencies(VariantRung& variant);
    void log_page_density() const;
    void queue_dirty_pages();

    // ---- WP-H tier-2 bookkeeping ----------------------------------------
    // Tier state rides the PHYSICAL SLOT, not the virtual page: eviction and
    // re-fill both hand a slot new content, and both must forget that the old
    // content was enriched. slot_reset_tier() is therefore called from every
    // place a slot changes hands, and it is also what makes double-application
    // impossible (enrichment multiplies into the page in place, so running it
    // twice on one fill would darken the page twice).
    void slot_reset_tier(uint32_t slot);
    void queue_enrich(uint32_t layer, VtPageKey page, uint32_t slot);
    void drain_enrich(VkCommandBuffer cmd);
    void queue_resident_enrichment();

    matter::VulkanDevice* vulkan_ = nullptr;
    bool ready_ = false;
    bool page_fills_paused_for_test_ = false;
    bool input_update_pending_ = false;
    std::shared_ptr<const VtInputSnapshot> input_snapshot_;
    std::array<uint32_t, kVtMaxInputSnapshots> slot_input_counts_{};
    std::weak_ptr<const VtInputSnapshot> input_snapshot_registry_[kVtMaxInputSnapshots];
    struct RetiredInputSnapshot {
        std::shared_ptr<const VtInputSnapshot> snapshot;
        uint64_t retire_serial = 0;
    };
    RetiredInputSnapshot retired_input_snapshots_[kVtMaxInputSnapshots];
    std::vector<std::shared_ptr<const VtInputSnapshot>> slot_input_snapshots_;
    std::vector<std::shared_ptr<const void>> slot_geometry_lifetimes_;
    std::unique_ptr<VtOcclusionPages> occlusion_pages_;
    std::vector<std::shared_ptr<VtOcclusionPages::Page>> slot_occlusion_pages_;
    std::vector<std::shared_ptr<VtReceiverMaterialState>> slot_material_mappings_;
    std::vector<uint64_t> slot_content_revisions_;
    struct RetiredGeometry {
        std::shared_ptr<const void> lifetime;
        uint64_t retire_serial = 0;
    };
    std::map<const void*, RetiredGeometry> retired_geometries_;
    std::vector<VtPageMetadata> slot_page_metadata_;
    uint32_t input_indices_dirty_begin_ = UINT32_MAX;
    uint32_t input_indices_dirty_end_ = 0;
    void retire_slot_input_snapshot(uint32_t slot);
    void retire_slot_geometry(uint32_t slot);
    void retire_slot_occlusion(uint32_t slot);
    void retire_occlusion(std::shared_ptr<VtOcclusionPages::Page> page);
    void set_slot_geometry(uint32_t slot, const VtDrawGeometry& geometry);
    void set_slot_input_snapshot(uint32_t slot,
                                std::shared_ptr<const VtInputSnapshot> snapshot);
    void rebind_compatible_input_snapshots(const std::vector<uint32_t>& changed_material_ids);
    void record_input_snapshot_indices(VkCommandBuffer cmd);

    PoolImage pool_[kVtChannelCount]{};
    VkImageView feedback_source_view_ = VK_NULL_HANDLE;
    std::shared_ptr<void> feedback_source_lifetime_;
    VkSampler pool_sampler_ = VK_NULL_HANDLE;
    VkSampler point_sampler_ = VK_NULL_HANDLE;

    // Per-variant GPU record (std430; mirrored in vt_common.glsl's
    // VtVariantRecord — keep the two in lockstep, and APPEND-ONLY: the shader
    // and this struct are versioned together, but debug tooling reads the
    // buffer by offset).
    struct VariantRecordGpu {
        uint32_t atlas_w = 0;
        uint32_t atlas_h = 0;
        uint32_t mip_count = 0;
        uint32_t flags = 0;            // bit0 = valid
        uint32_t mip_offset[8]{};   // original offsets: preserve following field offsets
        // --- buffer-indirection appends ---
        uint32_t table_offset = 0;     // word offset of the table in the SSBO
        uint32_t pages_w = 0;          // finest-mip page grid dims
        uint32_t pages_h = 0;
        uint32_t generation = 0;       // low 32 bits of the table generation;
                                       // material-domain binding token as well
                                       // as debug correlation. CPU retirement
                                       // still owns slot/table lifetime.
        // Append the ninth offset: table/pages/generation keep their old offsets.
        uint32_t mip_offset_high[kVtMaxMips - 8]{};
        uint32_t surface_links_low=0,surface_links_high=0;
    };
    static_assert(sizeof(VariantRecordGpu) == 16 + kVtMaxMips * 4 + 24,
                  "VariantRecordGpu must stay tightly packed for std430");

    std::vector<VariantRecordGpu> variant_records_;
    bool variant_records_dirty_ = true;
    std::vector<VtSurfaceConnectionPair> surface_connection_pairs_;
    std::shared_ptr<VtSurfaceConnectionState> surface_connections_;
    bool surface_connections_changed_=false;
    bool surface_walk_enabled_=true;
    void publish_surface_connections(std::string& error);

    // GPU buffers.
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
        VkDeviceSize tracked_alloc_size = 0;
        VkMemoryPropertyFlags tracked_mem_props = 0;
    };
    // `preferred` (0 = same as `properties`) is a soft request: find_memory_type
    // takes the first type that has it and falls back to any type meeting
    // `properties` alone. Used to ask for HOST_CACHED on buffers the CPU READS,
    // which is not optional for performance -- see ensure_feedback.
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags properties, Buffer& out,
                       std::string& error,
                       VkMemoryPropertyFlags preferred = 0);
    void destroy_buffer(Buffer& b);
    Buffer input_snapshot_buffer_;

    Buffer variant_buffer_{};        // host-visible storage buffer
    // Device-local indirection SSBO + a per-frame-slot staging ring. The ring
    // matters: a single staging buffer re-written every frame would clobber
    // bytes a still-executing previous frame's vkCmdCopyBuffer has not yet
    // consumed. Slot N's staging is only rewritten once slot N's fence has
    // been waited (the same guarantee the feedback readback ring rides).
    Buffer indirection_buffer_{};
    static constexpr uint32_t kFeedbackSlots = 3;
    Buffer indirection_staging_[kFeedbackSlots]{};
    bool indirection_cleared_ = false;   // one-time vkCmdFillBuffer(0)
    // One-time zero-clear of the pool images at creation, so any tail-gate
    // violation samples a deterministic flat color instead of undefined
    // memory. BC formats cannot vkCmdClearColorImage, so the compressed
    // channels are cleared by copying this zeroed staging buffer over every
    // layer (the uncompressed aux channel uses a plain clear). The buffer is
    // destroyed once the clearing frame has retired.
    Buffer pool_zero_staging_{};
    bool pool_cleared_ = false;
    uint64_t zero_staging_retire_ = 0;
    Buffer feedback_readback_[kFeedbackSlots]{};
    bool feedback_slot_written_[kFeedbackSlots]{};

    struct FeedbackGpu;
    std::unique_ptr<FeedbackGpu> feedback_gpu_;
    bool ensure_feedback_pipeline(std::string& error);

    uint32_t feedback_w_ = 0, feedback_h_ = 0;
    uint32_t feedback_raster_w_ = 0, feedback_raster_h_ = 0;
    uint32_t pool_pages_ = 0;
    uint32_t max_fills_per_frame_ = 8;
    // Dedicated tail-fill budget (MATTER_VT_TAIL_FILLS_PER_FRAME): a
    // streaming burst registers many variants per frame, and every one of
    // them renders legacy-flat until its single tail page is filled — so
    // tails must never queue behind feedback-driven sharpening fills.
    uint32_t max_tail_fills_per_frame_ = 16;
    // Ceiling on feedback-driven requests after selection. Mandatory tails
    // are separately bounded by live owners and never discarded by this cap.
    uint32_t max_queue_ = 256;
    bool activation_dirty_ = false;
    uint32_t max_variants_ = 0;
    size_t mesh_budget_bytes_ = 0;
    size_t mesh_bytes_used_ = 0;
    bool warned_rejection_ = false;
    bool debug_generations_ = false;
    bool event_log_ = false;
    uint64_t density_frame_ = 0; // opt-in one-shot CPU diagnostic; zero disables
    uint64_t frame_index_ = 0;
    uint32_t frame_slot_ = 0;

    VtSlotPool slots_;
    VtMaterialPages material_pages_;
    VtTableAllocator tables_;
    std::vector<VariantRung> variants_;      // indexed by variant slot; grows
                                             // lazily with the high-water mark
    std::vector<uint32_t> free_layers_;
    // Variant slots freed but not yet past the retirement horizon. Their GPU
    // records stay intact (flags still valid) until collection, so in-flight
    // frames referencing them keep resolving real data.
    struct LayerGrave {
        uint32_t layer = 0;
        uint64_t retire_serial = 0;
    };
    std::vector<LayerGrave> layer_graveyard_;
    // Debug audit: earliest frame each variant slot's record may be mutated
    // again (populated at release when MATTER_VT_DEBUG_GENERATIONS=1).
    std::map<uint32_t, uint64_t> debug_layer_reuse_;
    std::map<uint64_t, uint32_t> layer_of_;  // param key -> slot
    // M6: (hash, rung) -> param key. The rung-taking API (slot_for,
    // release_variant, update_variant_surface) has no chart table to derive a
    // param key from, so this is how a rung finds its layer. It is also what
    // keeps release_variant(hash) working: that walks rungs 0..31, and with a
    // content-derived key those rung keys no longer exist in layer_of_ — the
    // whole-part release would have silently freed nothing.
    std::map<uint64_t, uint64_t> param_key_of_rung_;
    std::map<uint32_t, std::set<uint32_t>> material_dependents_; // ID -> live owner layers

    // Pending fills, priority = mip distance from the currently mapped mip.
    struct PendingFill {
        uint32_t layer = 0;
        VtPageKey page{};
        uint32_t priority = 0;
        uint64_t requested_frame = 0;
        // Pinned tail's FINAL slot. Production targets scratch; successful
        // publication copies into this stable address. UINT32_MAX means a
        // detail page (reuse its resident slot or acquire a new one).
        uint32_t preassigned_slot = 0xFFFFFFFFu;
        uint64_t owner_generation = 0;
        uint64_t content_revision = 0;
    };
    std::vector<PendingFill> queue_;
    std::map<uint64_t, size_t> queued_keys_;   // dedup
    // At most one durable replacement per occupied physical slot. Execution
    // requests may be capacity-trimmed; this state survives until publication
    // or the page/owner is explicitly evicted/released.
    std::map<uint32_t, PendingFill> dirty_pages_;
    void reindex_pending_fills();
    void refresh_queue_stats();

    // WP-H: pages that tier-1 has filled and tier-2 has not run on yet.
    // Deduped by physical slot (a slot holds exactly one page), FIFO within a
    // frame, drained at low priority ahead of the frame's fills so a page
    // queued this frame is never enriched in the same command buffer that
    // wrote it.
    struct PendingEnrich {
        uint32_t layer = 0;
        VtPageKey page{};
        uint32_t slot = 0;
        uint64_t requested_frame = 0;
    };
    std::vector<PendingEnrich> enrich_queue_;
    std::map<uint32_t, size_t> enrich_queued_slot_;   // slot -> queue index
    // 0 = tier-1 (or unknown), 1 = tier-2 applied. Indexed by physical slot.
    std::vector<uint8_t> slot_tier_;
    uint32_t max_enrich_per_frame_ = 2;
    std::unique_ptr<VtPageEnricher> enricher_;
    std::vector<VtEnrichRequest> enrich_batch_;

    std::unique_ptr<VtPageFiller> filler_;
    std::vector<VtFillRequest> batch_;
    // What record_frame will map (or roll back) once the filler has reported
    // per-request success. `preassigned` distinguishes a pinned tail — which
    // keeps its slot on failure because every unmapped entry resolves to it —
    // from a freshly acquired slot, which goes back to the free list.
    struct PendingMap {
        uint32_t layer = 0;
        VtPageKey page{};
        uint32_t slot = 0;
        bool preassigned = false;
        uint64_t requested_frame = 0;
        uint64_t owner_generation = 0;
        uint64_t content_revision = 0;
        uint64_t slot_generation = 0;
        bool acquired = false;
        uint64_t owner_key = 0;
    };
    std::vector<PendingMap> pending_map_;
    // Per-request success flags handed to the filler. A real bool array (not
    // vector<bool>, which is bit-packed and has no addressable elements, and
    // not a uint8_t buffer reinterpreted as bool*, which would alias). Sized by
    // the hard env ceiling on MATTER_VT_FILLS_PER_FRAME so it never allocates.
    static constexpr uint32_t kMaxFillFlags = 64;
    bool fill_flags_[kMaxFillFlags]{};
    VtPageHeight fill_heights_[kMaxFillFlags]{};
    VtDrawGeometry fill_geometries_[kMaxFillFlags]{};
    VtMaterialPixelKey fill_material_keys_[kMaxFillFlags]{};
    std::vector<VtFeedbackRequest> injected_;
    VtFeedbackKeys feedback_keys_;
    VtPoolBinding pool_binding_{};
    Stats stats_{};
};

// The WP-E stub filler: flat material albedo, chart-tangent-neutral normal,
// default ORM, aux = dominant material id. CPU BC encode (bc_encode.h) +
// staging copy — deterministic, slow, and correct. WP-D replaces it at this
// same seam with the real compute compositor.
std::unique_ptr<VtPageFiller> make_vt_stub_filler(matter::VulkanDevice& vulkan,
                                                  uint32_t max_fills_per_frame,
                                                  std::string& error);

}  // namespace vt
