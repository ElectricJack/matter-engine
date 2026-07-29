#pragma once

// Chart-space virtual texturing — residency runtime (WP-E, contract C2).
//
// Two layers live in this header:
//
//   1. Pure CPU addressing/bookkeeping (VtVariantLayout, VtIndirectionMap,
//      VtSlotPool). No Vulkan calls, no allocation of GPU objects — this is
//      what vt_residency_tests.cpp exercises headlessly.
//   2. VtResidency, the GPU-facing owner of the physical page pool, the
//      indirection image, the feedback readback ring and the fill queue.
//
// Sampling side lives in shaders_vk/vt_common.glsl; the two must agree on the
// packing documented below.
//
// ---------------------------------------------------------------------------
// Indirection layout (must match vt_common.glsl)
// ---------------------------------------------------------------------------
// One array LAYER of the indirection image per (variant, rung), 64 x 128
// R16G16_UINT texels, single mip level. The virtual mip chain is stacked
// VERTICALLY inside that layer instead of using image mips, because different
// variants have different atlas sizes and image mips would force one shared
// ratio. Mip m's page grid occupies rows [mip_row[m], mip_row[m] + page_h[m]).
// 64 + 32 + 16 + 8 + 4 + 2 + 1 + 1 = 128 rows exactly covers the worst case
// (8192-texel atlas, mips down to the 64-texel tail).
//
// Entry = (R = physical slot, G = the mip whose page that slot actually holds).
// EVERY entry is always valid: at minimum it points at the variant's pinned
// tail page, so a sample never faults and never needs a walk loop. A finer
// resident page overwrites its own entry AND every finer descendant entry it
// covers, so the shader does exactly one texelFetch.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "chart_atlas.h"
#include "vt_types.h"

namespace matter { class VulkanDevice; struct VulkanFrame; }

namespace vt {

// 8192 -> 4096 -> ... -> 64 is 8 levels; the last is the resident tail.
constexpr uint32_t kVtMaxMips = 8u;
// Indirection layer dimensions (see the header comment).
constexpr uint32_t kVtIndirectionWidth  = 64u;
constexpr uint32_t kVtIndirectionHeight = 128u;
// A vt slot index of 0 means "no VT" in the draw record, so slots are
// transported as (layer + 1). kVtNoSlot is that sentinel.
constexpr uint32_t kVtNoSlot = 0u;

// ---------------------------------------------------------------------------
// Virtual layout of one (variant, rung) atlas.
// ---------------------------------------------------------------------------
struct VtVariantLayout {
    uint32_t atlas_w = 0, atlas_h = 0;      // finest-mip texels
    uint32_t mip_count = 0;                 // 1..kVtMaxMips; last mip is the tail
    uint32_t page_w[kVtMaxMips]{};          // pages across at each mip
    uint32_t page_h[kVtMaxMips]{};
    uint32_t mip_row[kVtMaxMips]{};         // row offset inside the indirection layer
    uint32_t entry_count = 0;               // sum of page_w[m] * page_h[m]

    bool valid() const { return mip_count != 0; }
};

// Builds the layout for an atlas. Returns false (and leaves `out` invalid) for
// a zero-sized atlas or one exceeding kVtMaxAtlasDim, or when the stacked mip
// rows would not fit the indirection layer.
inline bool vt_build_layout(uint32_t atlas_w, uint32_t atlas_h,
                            VtVariantLayout& out) {
    out = VtVariantLayout{};
    if (atlas_w == 0 || atlas_h == 0) return false;
    if (atlas_w > chart_atlas::kVtMaxAtlasDim ||
        atlas_h > chart_atlas::kVtMaxAtlasDim)
        return false;
    const uint32_t payload = chart_atlas::kVtPagePayload;
    uint32_t row = 0;
    uint32_t mips = 0;
    for (uint32_t m = 0; m < kVtMaxMips; ++m) {
        const uint32_t w = atlas_w >> m ? atlas_w >> m : 1u;
        const uint32_t h = atlas_h >> m ? atlas_h >> m : 1u;
        const uint32_t pw = (w + payload - 1u) / payload;
        const uint32_t ph = (h + payload - 1u) / payload;
        if (pw > kVtIndirectionWidth) return false;
        if (row + ph > kVtIndirectionHeight) return false;
        out.page_w[m] = pw;
        out.page_h[m] = ph;
        out.mip_row[m] = row;
        out.entry_count += pw * ph;
        row += ph;
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

// One indirection entry, as stored in R16G16_UINT.
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
// Holds the CPU-side truth for one indirection layer. map()/unmap() only touch
// the small resident set; texels() rebuilds the whole layer from that set,
// coarsest page first, so the result is a pure function of the resident set —
// no incremental-update aliasing bugs are possible. Rebuilds are bounded by
// the entry count (<= 5461) and only happen when the layer is dirty.
class VtIndirectionMap {
  public:
    void reset(const VtVariantLayout& layout, uint32_t tail_slot) {
        layout_ = layout;
        tail_slot_ = tail_slot;
        resident_.clear();
        texels_.assign(kVtIndirectionWidth * kVtIndirectionHeight, 0u);
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

    // Packed R16G16_UINT texels for the whole layer (row-major, 64 x 128).
    const std::vector<uint32_t>& texels() {
        rebuild_if_dirty();
        return texels_;
    }

    static uint32_t texel_index_for(const VtVariantLayout& layout, uint32_t mip,
                                    uint32_t px, uint32_t py) {
        return (layout.mip_row[mip] + py) * kVtIndirectionWidth + px;
    }

  private:
    uint32_t texel_index(uint32_t mip, uint32_t px, uint32_t py) const {
        return texel_index_for(layout_, mip, px, py);
    }

    void rebuild_if_dirty() {
        if (!dirty_ || !layout_.valid()) return;
        const uint32_t tail_mip = layout_.mip_count - 1u;
        const uint32_t tail = vt_pack_entry(tail_slot_, tail_mip);
        texels_.assign(kVtIndirectionWidth * kVtIndirectionHeight, tail);
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
                uint32_t* row =
                    texels_.data() + (layout_.mip_row[f] + y) * kVtIndirectionWidth;
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
// Physical page slot pool with LRU eviction.
// ---------------------------------------------------------------------------
// Pinned slots (the per-variant resident tails) are never evicted. Unpinned
// slots evict strictly by ascending last_used, ties broken by ascending slot
// index, so eviction order is deterministic and testable.
class VtSlotPool {
  public:
    struct Owner {
        bool     live = false;
        bool     pinned = false;
        uint64_t variant_key = 0;   // residency-defined; opaque here
        VtPageKey page{};
        uint64_t last_used = 0;
    };

    void reset(uint32_t capacity) {
        owners_.assign(capacity, Owner{});
        free_.clear();
        free_.reserve(capacity);
        for (uint32_t i = capacity; i-- > 0;) free_.push_back(i);
        used_ = 0;
        pinned_ = 0;
        evictions_ = 0;
    }

    uint32_t capacity() const { return static_cast<uint32_t>(owners_.size()); }
    uint32_t used() const { return used_; }
    uint32_t pinned() const { return pinned_; }
    uint64_t evictions() const { return evictions_; }
    const Owner& owner(uint32_t slot) const { return owners_[slot]; }

    // Acquires a slot for (variant_key, page). On success `slot` receives the
    // index; when an occupied slot was recycled, `evicted` receives its old
    // owner (evicted.live == true) so the caller can unmap it.
    bool acquire(uint64_t variant_key, VtPageKey page, bool pinned,
                 uint64_t frame, uint32_t& slot, Owner& evicted) {
        evicted = Owner{};
        if (owners_.empty()) return false;
        uint32_t chosen;
        if (!free_.empty()) {
            chosen = free_.back();
            free_.pop_back();
        } else {
            if (!pick_lru(chosen)) return false;
            evicted = owners_[chosen];
            ++evictions_;
            --used_;
        }
        Owner& o = owners_[chosen];
        o.live = true;
        o.pinned = pinned;
        o.variant_key = variant_key;
        o.page = page;
        o.last_used = frame;
        ++used_;
        if (pinned) ++pinned_;
        slot = chosen;
        return true;
    }

    void touch(uint32_t slot, uint64_t frame) {
        if (slot < owners_.size() && owners_[slot].live)
            owners_[slot].last_used = frame;
    }

    void release(uint32_t slot) {
        if (slot >= owners_.size() || !owners_[slot].live) return;
        if (owners_[slot].pinned) --pinned_;
        owners_[slot] = Owner{};
        free_.push_back(slot);
        --used_;
    }

    // Evictable count = live, unpinned slots.
    uint32_t evictable() const {
        uint32_t n = 0;
        for (const Owner& o : owners_) if (o.live && !o.pinned) ++n;
        return n;
    }

  private:
    bool pick_lru(uint32_t& out) const {
        bool found = false;
        uint32_t best = 0;
        uint64_t best_used = 0;
        for (uint32_t i = 0; i < owners_.size(); ++i) {
            const Owner& o = owners_[i];
            if (!o.live || o.pinned) continue;
            if (!found || o.last_used < best_used) {
                found = true;
                best = i;
                best_used = o.last_used;
            }
        }
        if (found) out = best;
        return found;
    }

    std::vector<Owner> owners_;
    std::vector<uint32_t> free_;
    uint32_t used_ = 0;
    uint32_t pinned_ = 0;
    uint64_t evictions_ = 0;
};

// ---------------------------------------------------------------------------
// Feedback packing (must match vt_common.glsl's vt_write_feedback).
// ---------------------------------------------------------------------------
// One RGBA16_UINT texel per 8x8 screen block:
//   x = vt layer + 1 (0 = no request), y = page_x, z = page_y, w = mip.
struct VtFeedbackRequest {
    uint32_t layer = 0;   // indirection array layer
    uint32_t mip = 0;
    uint32_t px = 0, py = 0;
};

// ---------------------------------------------------------------------------
// The GPU-facing residency runtime.
// ---------------------------------------------------------------------------
class VtResidency {
  public:
    struct Stats {
        uint32_t variants = 0;          // registered (variant, rung) layers
        uint32_t pool_capacity = 0;
        uint32_t pool_used = 0;
        uint32_t pool_pinned = 0;
        uint32_t fills_last_frame = 0;
        uint32_t requests_last_frame = 0;
        uint32_t queue_depth = 0;
        uint64_t fills_total = 0;
        uint64_t evictions_total = 0;
        uint64_t pool_bytes = 0;
        uint64_t mesh_bytes = 0;        // CPU copies held for the filler
        uint32_t rejected_variants = 0; // fell back to legacy (budget/layers)
        uint64_t invalidations_total = 0;  // invalidate_all_content() calls
        uint64_t pages_dropped_total = 0;  // pages those calls dropped
    };

    VtResidency();
    ~VtResidency();
    VtResidency(const VtResidency&) = delete;
    VtResidency& operator=(const VtResidency&) = delete;

    // Allocates the pool, indirection image, sampler and readback ring. Safe
    // to call repeatedly (no-op once ready). Fails closed: on error nothing is
    // left partially bound and available() stays false.
    bool init(matter::VulkanDevice& vulkan, std::string& error);
    void shutdown();
    bool available() const { return ready_; }

    // Descriptor handles (valid only once available()).
    VkImageView pool_view(uint32_t channel) const;
    VkImageView indirection_view() const { return indirection_.view; }
    VkSampler   pool_sampler() const { return pool_sampler_; }
    VkSampler   point_sampler() const { return point_sampler_; }
    VkBuffer    variant_buffer() const { return variant_buffer_.buffer; }
    VkDeviceSize variant_buffer_size() const { return variant_buffer_.size; }
    VkImageView feedback_view() const { return feedback_.view; }
    uint32_t    feedback_width() const { return feedback_w_; }
    uint32_t    feedback_height() const { return feedback_h_; }

    // Registers one (variant, rung). Returns the transport slot (layer + 1),
    // or kVtNoSlot when the rung has no charts / the layout is unusable / no
    // indirection layer is free / the mesh budget is spent. Idempotent per
    // (hash, rung).
    //
    // LIFETIME: the chart table and every array `context` points at are COPIED
    // here. The caller may free its own storage the moment this returns; the
    // VtPartContext the filler receives points at this object's copies, which
    // live exactly as long as the registration. The copies are what makes the
    // "borrowed, outlives every queued fill" clause of vt_types.h true, and
    // they are budgeted: MATTER_VT_MESH_BUDGET_MB (default 256) caps the
    // total, and a registration that would exceed it fails closed to the
    // legacy path rather than growing without bound across a streamed world.
    uint32_t register_variant(uint64_t variant_hash, uint32_t rung,
                              const chart_atlas::ChartAtlasRung& atlas,
                              const VtPartContext& context);
    void release_variant(uint64_t variant_hash);
    uint32_t slot_for(uint64_t variant_hash, uint32_t rung) const;

    // WP-F: replace one registered (variant, rung)'s surfaces()-tape
    // classification in place (owned copies + repointed context fields).
    // weights must be vertex_count * material_count bytes; material_count of
    // 0 (or an empty tape) strips the classification, reverting the rung to
    // the TriEx materialId path. Returns false when the (hash, rung) is not
    // registered or the sizes disagree with the stored mesh.
    //
    // CALLER CONTRACT: the device must be idle with respect to fills that
    // borrow this variant's context (the renderer's vt-surface update path
    // wait_idles first), and the caller must drop the filler's cached mesh
    // buffers (VtCompositor::invalidate_part) plus resident page content
    // (invalidate_all_content) afterwards — this method only swaps the CPU
    // inputs.
    bool update_variant_surface(uint64_t variant_hash, uint32_t rung,
                                const uint8_t* weights, size_t weight_bytes,
                                const uint32_t* materials,
                                uint32_t material_count, uint64_t tape_hash);

    // Declares every page currently in the pool stale, because what the
    // INSTALLED FILLER bakes from has changed (a detail tileset slot was
    // loaded/evicted, the material table was edited). Rebinding the filler's
    // inputs alone only fixes FUTURE fills; the pages already resident — and
    // in particular the pinned per-variant tails, which never expire — keep
    // the content they were baked from, forever.
    //
    // Drops every resident unpinned page (its indirection entry falls back to
    // the variant's tail, exactly as after an eviction) and returns its slot to
    // the free pool, then re-queues every registered variant's tail for an
    // IN-PLACE re-fill (PendingFill::preassigned_slot, so a tail rewrites the
    // pinned slot every unmapped entry resolves to instead of burning a second
    // one). Tails are queued above any feedback-derived priority: until a tail
    // is re-filled it is what most of the variant reads.
    //
    // Fills are queued, not recorded: they execute in the next record_frame(),
    // so the caller must have bound the new inputs before returning here (the
    // renderer's push_vt_compositor_inputs() does, under its wait_idle).
    // Returns the number of pages dropped.
    uint32_t invalidate_all_content();

    // ---- per-frame ----
    // Called once per frame before recording. `frame_slot` selects the
    // readback buffer whose previous submission has already completed (the
    // caller's frame fence guarantees this), so its contents are consumed here.
    void begin_frame(uint64_t frame_index, uint32_t frame_slot);
    // Ensures the feedback image matches the raster extent. Returns false only
    // on a hard allocation failure (VT then degrades: feedback stops, tails
    // still render).
    bool ensure_feedback(uint32_t raster_width, uint32_t raster_height,
                         std::string& error);
    // Records: pool/indirection transitions, bounded fills through the filler,
    // indirection + variant-table uploads. Call before the G-buffer pass.
    bool record_frame(VkCommandBuffer cmd, std::string& error);
    // Records the feedback readback copy. Call after the G-buffer pass.
    void record_feedback_readback(VkCommandBuffer cmd);
    // Records the feedback clear. Call before the G-buffer pass.
    void record_feedback_clear(VkCommandBuffer cmd);

    void set_filler(std::unique_ptr<VtPageFiller> filler);
    VtPageFiller* filler() const { return filler_.get(); }

    const Stats& stats() const { return stats_; }
    uint32_t max_fills_per_frame() const { return max_fills_per_frame_; }

    // TEST SEAM: inject feedback requests without a GPU readback.
    void inject_feedback_for_test(const VtFeedbackRequest* requests,
                                  size_t count);

  private:
    struct PoolImage {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t layers = 0;
        uint32_t edge = 0;
    };

    struct VariantRung {
        uint64_t variant_hash = 0;
        uint32_t rung = 0;
        uint32_t layer = 0;               // indirection array layer
        VtVariantLayout layout{};
        VtIndirectionMap indirection;
        chart_atlas::ChartAtlasRung atlas;   // owned copy (borrowed by the filler)
        // Owned copies of everything VtPartContext points at; `context` is
        // repointed at these after the copy (see register_variant).
        std::vector<float> positions, normals, surface_uvs, material_table;
        std::vector<uint32_t> material_ids, indices;
        std::vector<uint8_t> tint_rgba;
        // WP-F: owned copies of the surfaces()-tape classification (see
        // vt_types.h appends); replaced in place by update_variant_surface.
        std::vector<uint8_t> surface_weights;
        std::vector<uint32_t> surface_materials;
        size_t mesh_bytes = 0;
        VtPartContext context{};
        uint32_t tail_slot = 0;
        bool tail_filled = false;
        bool live = false;
    };

    bool create_pool_image(uint32_t channel, VkFormat format, uint32_t layers,
                           std::string& error);
    void destroy_pool_image(PoolImage& image);
    bool create_indirection(uint32_t layers, std::string& error);
    void write_variant_record(const VariantRung& v);
    // `force` queues a page even when the indirection already maps it —
    // the pinned tail is mapped at registration but still needs its fill.
    void queue_page(VariantRung& v, VtPageKey page, bool force = false,
                    uint32_t preassigned_slot = 0xFFFFFFFFu);
    void drain_feedback(uint32_t frame_slot);

    matter::VulkanDevice* vulkan_ = nullptr;
    bool ready_ = false;

    PoolImage pool_[kVtChannelCount]{};
    PoolImage indirection_{};
    PoolImage feedback_{};
    VkSampler pool_sampler_ = VK_NULL_HANDLE;
    VkSampler point_sampler_ = VK_NULL_HANDLE;

    // Per-variant GPU record (std430; mirrored in vt_common.glsl).
    struct VariantRecordGpu {
        uint32_t atlas_w = 0;
        uint32_t atlas_h = 0;
        uint32_t mip_count = 0;
        uint32_t flags = 0;            // bit0 = valid
        uint32_t mip_row[kVtMaxMips]{};
    };
    static_assert(sizeof(VariantRecordGpu) == 16 + kVtMaxMips * 4,
                  "VariantRecordGpu must stay tightly packed for std430");

    std::vector<VariantRecordGpu> variant_records_;
    bool variant_records_dirty_ = true;

    // GPU buffers.
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
    };
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags properties, Buffer& out,
                       std::string& error);
    void destroy_buffer(Buffer& b);

    Buffer variant_buffer_{};        // host-visible storage buffer
    Buffer indirection_staging_{};   // host-visible, one layer per pending upload
    static constexpr uint32_t kFeedbackSlots = 3;
    Buffer feedback_readback_[kFeedbackSlots]{};
    bool feedback_slot_written_[kFeedbackSlots]{};

    uint32_t feedback_w_ = 0, feedback_h_ = 0;
    uint32_t pool_pages_ = 0;
    uint32_t max_fills_per_frame_ = 8;
    uint32_t max_variants_ = 0;
    size_t mesh_budget_bytes_ = 0;
    size_t mesh_bytes_used_ = 0;
    uint64_t frame_index_ = 0;
    uint32_t frame_slot_ = 0;

    VtSlotPool slots_;
    std::vector<VariantRung> variants_;      // indexed by layer
    std::vector<uint32_t> free_layers_;
    std::map<uint64_t, uint32_t> layer_of_;  // (hash, rung) key -> layer

    // Pending fills, priority = mip distance from the currently mapped mip.
    struct PendingFill {
        uint32_t layer = 0;
        VtPageKey page{};
        uint32_t priority = 0;
        uint64_t requested_frame = 0;
        // The pinned tail already owns its slot (it is what every unmapped
        // entry resolves to), so its fill must write THAT slot. Allocating a
        // fresh one instead leaves the pinned slot holding undefined bytes and
        // silently burns a page — UINT32_MAX means "allocate one".
        uint32_t preassigned_slot = 0xFFFFFFFFu;
    };
    std::vector<PendingFill> queue_;
    std::map<uint64_t, size_t> queued_keys_;   // dedup

    std::unique_ptr<VtPageFiller> filler_;
    std::vector<VtFillRequest> batch_;
    std::vector<VtFeedbackRequest> injected_;
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
