// vt_residency_tests.cpp — WP-E headless unit gate for the VT residency
// addressing and bookkeeping layer (vt_residency.h's pure CPU half).
//
// Everything here runs without a Vulkan device: layout construction, the
// indirection resolve the shader mirrors, page/border addressing, and LRU
// eviction order. The GPU half (pool images, fills, feedback) is covered by
// MATTER_VK_SMOKE_MODE=vt in vulkan_smoke_tests.cpp.

#include "check.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "render/vt_residency.h"
#include "render/vt_caster_select.h"   // M6.5

using namespace vt;

namespace {

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
void test_layout() {
    VtVariantLayout layout{};
    CHECK(!vt_build_layout(0, 512, layout), "zero atlas width is rejected");
    CHECK(!vt_build_layout(512, 0, layout), "zero atlas height is rejected");
    CHECK(!vt_build_layout(chart_atlas::kVtMaxAtlasDim * 2, 512, layout),
          "atlas wider than kVtMaxAtlasDim is rejected");

    // 8192 is the worst case: mips 8192..64 is 8 levels and the concatenated
    // page grids sum to 4096+1024+256+64+16+4+1+1 = 5462 exact entries — the
    // number kVtMaxTableWords pins.
    CHECK(vt_build_layout(8192, 8192, layout), "8192 atlas builds");
    CHECK(layout.mip_count == 8, "8192 atlas has 8 mips down to the tail");
    CHECK(layout.page_w[0] == 64 && layout.page_h[0] == 64,
          "8192 atlas is 64x64 pages at mip 0");
    CHECK(layout.page_w[7] == 1 && layout.page_h[7] == 1,
          "the tail mip is a single page");
    CHECK(layout.mip_offset[0] == 0, "mip 0's grid starts the table");
    CHECK(layout.mip_offset[1] == 4096,
          "mip 1's grid starts after mip 0's 64x64 entries");
    CHECK(layout.mip_offset[7] == 5461,
          "the tail entry is the last table word");
    CHECK(layout.entry_count == kVtMaxTableWords,
          "the 8192 atlas needs exactly kVtMaxTableWords entries");

    // EXACT sizing is the point of the buffer indirection: a 512^2 atlas
    // needs 16+4+1+1 = 22 entries (88 bytes), not the old 64x128 = 32 KiB
    // fixed layer.
    CHECK(vt_build_layout(512, 512, layout), "512 atlas builds");
    CHECK(layout.mip_count == 4, "512 atlas has 4 mips down to the tail");
    CHECK(layout.entry_count == 16 + 4 + 1 + 1,
          "a 512^2 atlas costs exactly 22 table entries");
    CHECK(layout.mip_offset[1] == 16 && layout.mip_offset[2] == 20 &&
              layout.mip_offset[3] == 21,
          "mip offsets are the running prefix sum of the grid sizes");

    // A small atlas stops at the first mip inside the tail budget.
    CHECK(vt_build_layout(256, 128, layout), "256x128 atlas builds");
    CHECK(layout.mip_count == 3,
          "256x128 needs mips 256x128, 128x64, 64x32 (tail)");
    CHECK(layout.page_w[0] == 2 && layout.page_h[0] == 1,
          "256x128 is 2x1 pages at mip 0");
    CHECK(layout.page_w[2] == 1 && layout.page_h[2] == 1,
          "the 64x32 tail is one page");
    CHECK(layout.entry_count == 2 + 1 + 1,
          "a 256x128 atlas costs exactly 4 table entries");

    // An atlas already inside the tail budget is a single (tail) mip.
    CHECK(vt_build_layout(64, 64, layout), "64x64 atlas builds");
    CHECK(layout.mip_count == 1, "a 64x64 atlas is nothing but its tail");
    CHECK(layout.entry_count == 1, "a tail-only atlas is one entry");

    // The shader recomputes pw(m) closed-form as (max(w>>m,1)+127)>>7; that
    // must agree with the layout's page_w for every mip of an odd-sized atlas
    // (page-aligned only at the finest mip, like real chart packs).
    CHECK(vt_build_layout(1920, 1080, layout), "1920x1080 atlas builds");
    for (uint32_t m = 0; m < layout.mip_count; ++m) {
        const uint32_t w = layout.atlas_w >> m ? layout.atlas_w >> m : 1u;
        const uint32_t h = layout.atlas_h >> m ? layout.atlas_h >> m : 1u;
        CHECK(layout.page_w[m] == ((w + 127u) >> 7u),
              "closed-form pw(m) matches the layout");
        CHECK(layout.page_h[m] == ((h + 127u) >> 7u),
              "closed-form ph(m) matches the layout");
    }
}

// ---------------------------------------------------------------------------
// Indirection resolve
// ---------------------------------------------------------------------------
void test_indirection() {
    VtVariantLayout layout{};
    vt_build_layout(1024, 1024, layout);   // mips 1024,512,256,128,64 -> 5
    CHECK(layout.mip_count == 5, "1024 atlas has 5 mips");
    CHECK(layout.page_w[0] == 8 && layout.page_h[0] == 8,
          "1024 atlas is 8x8 pages at mip 0");

    VtIndirectionMap map;
    map.reset(layout, /*tail_slot=*/7);

    // Unmapped everywhere -> every entry resolves to the pinned tail. This is
    // the "every loaded variant always has valid texels" guarantee.
    VtEntry entry = map.resolve(0, 3, 5);
    CHECK(entry.slot == 7 && entry.mapped_mip == 4,
          "an unmapped fine page resolves to the pinned tail page");
    entry = map.resolve(4, 0, 0);
    CHECK(entry.slot == 7 && entry.mapped_mip == 4,
          "the tail entry resolves to itself");

    // Map a mip-2 page (2x2 pages at mip 2). It must claim its own entry AND
    // every finer entry it covers, so a mip-0 sample finds it in one fetch.
    map.map(2, 1, 0, 42);
    entry = map.resolve(2, 1, 0);
    CHECK(entry.slot == 42 && entry.mapped_mip == 2,
          "a mapped page resolves to itself");
    entry = map.resolve(1, 2, 0);
    CHECK(entry.slot == 42 && entry.mapped_mip == 2,
          "a finer page inside a mapped coarse page resolves to it");
    entry = map.resolve(0, 5, 3);
    CHECK(entry.slot == 42 && entry.mapped_mip == 2,
          "mip-0 page (5,3) is covered by mip-2 page (1,0)");
    entry = map.resolve(0, 1, 1);
    CHECK(entry.slot == 7 && entry.mapped_mip == 4,
          "a mip-0 page outside the mapped region still sees the tail");

    // A finer resident page must win inside its own region and nowhere else.
    map.map(0, 5, 3, 99);
    entry = map.resolve(0, 5, 3);
    CHECK(entry.slot == 99 && entry.mapped_mip == 0,
          "the finer page wins its own entry");
    entry = map.resolve(0, 4, 3);
    CHECK(entry.slot == 42 && entry.mapped_mip == 2,
          "its neighbour still resolves to the coarse page");
    entry = map.resolve(2, 1, 0);
    CHECK(entry.slot == 42 && entry.mapped_mip == 2,
          "the coarse entry itself is untouched by a finer mapping");

    // Unmapping the finer page must restore the coarse coverage exactly —
    // this is the case an incremental updater gets wrong.
    map.unmap(0, 5, 3);
    entry = map.resolve(0, 5, 3);
    CHECK(entry.slot == 42 && entry.mapped_mip == 2,
          "unmapping a fine page falls back to the coarse page, not the tail");
    map.unmap(2, 1, 0);
    entry = map.resolve(0, 5, 3);
    CHECK(entry.slot == 7 && entry.mapped_mip == 4,
          "unmapping the coarse page falls all the way back to the tail");

    // Out-of-range coordinates never index outside the layer.
    entry = map.resolve(0, 999, 999);
    CHECK(entry.slot == 7, "an out-of-range page resolves to the tail");
    entry = map.resolve(99, 0, 0);
    CHECK(entry.slot == 7, "an out-of-range mip resolves to the tail");

    // Entry packing must match the shader's unpack (low 16 = slot, high 16 =
    // mapped mip).
    map.map(0, 0, 0, 300);
    const std::vector<uint32_t>& texels = map.texels();
    const uint32_t packed =
        texels[VtIndirectionMap::texel_index_for(layout, 0, 0, 0)];
    CHECK((packed & 0xFFFFu) == 300u, "entry low half is the physical slot");
    CHECK((packed >> 16) == 0u, "entry high half is the mapped mip");
    CHECK(map.texels().size() == layout.entry_count,
          "the mirror is exactly the variant's exact-sized table");
    // texel_index_for must agree with the shader's addressing:
    // mip_offset[m] + py * pw(m) + px.
    CHECK(VtIndirectionMap::texel_index_for(layout, 1, 2, 3) ==
              layout.mip_offset[1] + 3u * layout.page_w[1] + 2u,
          "table indexing is mip_offset + row-major within the mip grid");
}

void test_indirection_dirty() {
    VtVariantLayout layout{};
    vt_build_layout(512, 512, layout);
    VtIndirectionMap map;
    map.reset(layout, 0);
    CHECK(map.dirty(), "a freshly reset map needs an upload");
    (void)map.texels();
    map.clear_dirty();
    CHECK(!map.dirty(), "clear_dirty settles the map");
    map.unmap(0, 0, 0);   // nothing mapped there
    CHECK(!map.dirty(), "unmapping a page that was not resident is a no-op");
    map.map(1, 0, 0, 3);
    CHECK(map.dirty(), "mapping a page dirties the map");
    CHECK(map.resident_count() == 1, "one resident page after one map");
}

// ---------------------------------------------------------------------------
// Page / border addressing (the same arithmetic vt_common.glsl performs)
// ---------------------------------------------------------------------------
void test_border_math() {
    // Slot geometry: payload starts kVtPageBorder texels into the slot rect,
    // and a whole page (payload + both borders) is kVtPageStride texels.
    CHECK(kVtPageStride == chart_atlas::kVtPagePayload +
                               2u * chart_atlas::kVtPageBorder,
          "a page slot is payload plus a border on each side");
    CHECK(kVtPageStride % 4u == 0u,
          "the page stride is BC-block aligned (4x4 blocks)");
    CHECK(kVtPoolLayerEdgeTexels == kVtPagesPerLayerEdge * kVtPageStride,
          "a pool layer is exactly the page grid");

    uint32_t layer = 0, x = 0, y = 0;
    vt_slot_origin(0, layer, x, y);
    CHECK(layer == 0 && x == 0 && y == 0, "slot 0 is the first page of layer 0");
    vt_slot_origin(1, layer, x, y);
    CHECK(layer == 0 && x == kVtPageStride && y == 0,
          "slot 1 is one page to the right");
    vt_slot_origin(kVtPagesPerLayerEdge, layer, x, y);
    CHECK(layer == 0 && x == 0 && y == kVtPageStride,
          "slot 16 starts the second page row");
    vt_slot_origin(kVtPagesPerLayer, layer, x, y);
    CHECK(layer == 1 && x == 0 && y == 0,
          "slot 256 is the first page of the next array layer");
    vt_slot_origin(kVtPagesPerLayer * 3u + 17u, layer, x, y);
    CHECK(layer == 3 && x == kVtPageStride && y == kVtPageStride,
          "slot decomposition is layer-major then row-major");

    // Every page's payload rect must stay inside its own slot, so a bilinear
    // fetch at the payload edge only ever touches this page's border texels.
    for (uint32_t slot = 0; slot < kVtPagesPerLayer; ++slot) {
        vt_slot_origin(slot, layer, x, y);
        const uint32_t payload_end_x =
            x + chart_atlas::kVtPageBorder + chart_atlas::kVtPagePayload;
        const uint32_t payload_end_y =
            y + chart_atlas::kVtPageBorder + chart_atlas::kVtPagePayload;
        CHECK(payload_end_x + chart_atlas::kVtPageBorder <= kVtPoolLayerEdgeTexels,
              "payload + trailing border fits the pool layer horizontally");
        CHECK(payload_end_y + chart_atlas::kVtPageBorder <= kVtPoolLayerEdgeTexels,
              "payload + trailing border fits the pool layer vertically");
        CHECK(x % 4u == 0u && y % 4u == 0u,
              "page origins are BC-block aligned");
    }
}

// ---------------------------------------------------------------------------
// LRU slot pool
// ---------------------------------------------------------------------------
void test_slot_pool() {
    VtSlotPool pool;
    pool.reset(4);
    CHECK(pool.capacity() == 4 && pool.used() == 0,
          "a reset pool is empty at its capacity");

    uint32_t slots[4]{};
    VtSlotPool::Owner evicted;
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(pool.acquire(1000 + i, VtPageKey{0, i, 0}, false, /*frame=*/i,
                           slots[i], evicted),
              "acquiring from a pool with free slots succeeds");
        CHECK(!evicted.live, "a free-list acquire evicts nothing");
    }
    CHECK(pool.used() == 4, "the pool is full");

    // Full pool: the least-recently-used unpinned slot is recycled, and its
    // previous owner comes back so the caller can unmap it.
    CHECK(pool.acquire(2000, VtPageKey{0, 9, 9}, false, 10, slots[0], evicted),
          "a full pool still acquires by eviction");
    CHECK(evicted.live, "the recycled slot reports its previous owner");
    CHECK(evicted.variant_key == 1000 && evicted.page.px == 0,
          "eviction picks the oldest last_used (frame 0)");
    CHECK(pool.evictions() == 1, "the eviction is counted");
    CHECK(pool.used() == 4, "eviction keeps the pool full, it does not grow it");

    // Touching a slot moves it to the back of the eviction order.
    pool.touch(slots[0], 20);   // the just-acquired one, now newest anyway
    uint32_t next = 0;
    CHECK(pool.acquire(2001, VtPageKey{0, 8, 8}, false, 21, next, evicted),
          "second eviction succeeds");
    CHECK(evicted.variant_key == 1001,
          "the next-oldest owner is evicted, in order");

    // Pinned slots are never chosen.
    VtSlotPool pinned_pool;
    pinned_pool.reset(2);
    uint32_t pinned_slot = 0, spare = 0;
    CHECK(pinned_pool.acquire(1, VtPageKey{4, 0, 0}, true, 0, pinned_slot,
                              evicted),
          "a pinned tail acquires");
    CHECK(pinned_pool.pinned() == 1, "the pin is counted");
    CHECK(pinned_pool.acquire(2, VtPageKey{0, 0, 0}, false, 1, spare, evicted),
          "the remaining slot acquires");
    uint32_t recycled = 0;
    CHECK(pinned_pool.acquire(3, VtPageKey{0, 1, 0}, false, 2, recycled,
                              evicted),
          "a full pool with one pin still recycles the unpinned slot");
    CHECK(recycled == spare, "the unpinned slot is the one recycled");
    CHECK(evicted.variant_key == 2, "the unpinned owner is what came back");
    CHECK(pinned_pool.evictable(3) == 1,
          "only the unpinned slot is ever evictable");
    CHECK(pinned_pool.evictable(2) == 0,
          "a slot last used the current frame is hysteresis-protected");

    // A pool of nothing but pins cannot serve a fill; acquire must fail rather
    // than steal a tail (stealing a tail would break the never-fault promise).
    VtSlotPool all_pinned;
    all_pinned.reset(1);
    uint32_t only = 0;
    CHECK(all_pinned.acquire(1, VtPageKey{0, 0, 0}, true, 0, only, evicted),
          "the single slot pins");
    const uint64_t pinned_generation = all_pinned.owner(only).generation;
    CHECK(pinned_generation != 0, "an acquire stamps a nonzero generation");
    uint32_t denied = 0;
    CHECK(!all_pinned.acquire(2, VtPageKey{0, 0, 0}, false, 1, denied, evicted),
          "an all-pinned pool refuses to evict a pinned tail");

    // Variant-death release parks the slot in the GRAVEYARD: it is not
    // allocatable until its retire serial has passed collect() — an in-flight
    // frame's draw records may still resolve into it until then.
    all_pinned.release(only, /*retire_serial=*/10);
    CHECK(all_pinned.used() == 0 && all_pinned.pinned() == 0,
          "releasing a pinned slot clears both counters");
    CHECK(all_pinned.graveyard_slots() == 1,
          "the released slot ages in the graveyard");
    CHECK(!all_pinned.acquire(3, VtPageKey{0, 0, 0}, false, 5, denied, evicted),
          "a graveyarded slot is NOT allocatable before its retire serial");
    all_pinned.collect(9);
    CHECK(all_pinned.graveyard_slots() == 1,
          "collect before the retire serial keeps the grave");
    CHECK(!all_pinned.acquire(3, VtPageKey{0, 0, 0}, false, 9, denied, evicted),
          "still not allocatable one frame early");
    all_pinned.collect(10);
    CHECK(all_pinned.graveyard_slots() == 0,
          "collect at the retire serial matures the slot");
    CHECK(all_pinned.acquire(3, VtPageKey{0, 0, 0}, false, 11, denied, evicted),
          "the matured slot is reusable");
    CHECK(!evicted.live, "reusing a matured slot is not an eviction");
    CHECK(all_pinned.owner(denied).generation > pinned_generation,
          "reuse stamps a strictly newer generation");

    // release_now() (the never-mapped rollback / device-idle path) skips the
    // graveyard by design.
    all_pinned.release_now(denied);
    CHECK(all_pinned.graveyard_slots() == 0 && all_pinned.used() == 0,
          "release_now frees immediately");
    CHECK(all_pinned.acquire(4, VtPageKey{0, 0, 0}, false, 12, denied, evicted),
          "a release_now slot is immediately reusable");

    // A zero-capacity pool fails closed rather than indexing nothing.
    VtSlotPool empty;
    empty.reset(0);
    uint32_t none = 0;
    CHECK(!empty.acquire(1, VtPageKey{0, 0, 0}, false, 0, none, evicted),
          "a zero-capacity pool never hands out a slot");
}

// ---------------------------------------------------------------------------
// Eviction hysteresis (exhaustion behaviour: degrade, never thrash)
// ---------------------------------------------------------------------------
void test_slot_hysteresis() {
    VtSlotPool pool;
    pool.reset(2);
    uint32_t a = 0, b = 0, c = 0;
    VtSlotPool::Owner evicted;
    CHECK(pool.acquire(1, VtPageKey{0, 0, 0}, false, /*frame=*/5, a, evicted),
          "first slot acquires");
    CHECK(pool.acquire(2, VtPageKey{0, 1, 0}, false, 5, b, evicted),
          "second slot acquires");
    // Both pages were requested (touched) this frame: NOTHING is evictable,
    // so admission fails cleanly instead of evicting a page the same frame's
    // feedback asked for — the caller retries next frame while the
    // indirection serves coarser coverage.
    CHECK(pool.evictable(5) == 0,
          "pages requested by the current frame are ineligible for eviction");
    CHECK(!pool.acquire(3, VtPageKey{0, 2, 0}, false, 5, c, evicted),
          "a fully-protected pool refuses to evict (no thrash)");
    // Next frame the protection window moves and LRU eviction resumes.
    CHECK(pool.evictable(6) == 2, "the window is one frame wide");
    CHECK(pool.acquire(3, VtPageKey{0, 2, 0}, false, 6, c, evicted),
          "eviction resumes the next frame");
    CHECK(evicted.live && evicted.variant_key == 1,
          "the LRU (first-acquired) slot is the victim");
    // touch() re-arms protection: the untouched slot is the only candidate.
    pool.touch(c, 7);
    uint32_t d = 0;
    CHECK(pool.acquire(4, VtPageKey{0, 3, 0}, false, 7, d, evicted),
          "an unprotected slot still evicts");
    CHECK(evicted.variant_key == 2,
          "the touched slot is skipped; the stale one is the victim");

    // A widened window (MATTER_VT_EVICT_PROTECT_FRAMES) keeps pages requested
    // every FEW frames protected — the jittered-feedback case, where a hot
    // page is only sampled into the feedback buffer every couple of frames
    // and a one-frame window would let it ping-pong with its competitor.
    VtSlotPool wide;
    wide.reset(1);
    wide.set_protect_frames(4);
    uint32_t w = 0, w2 = 0;
    CHECK(wide.acquire(1, VtPageKey{0, 0, 0}, false, 10, w, evicted),
          "the single slot acquires");
    CHECK(!wide.acquire(2, VtPageKey{0, 1, 0}, false, 13, w2, evicted),
          "a page used 3 frames ago is still protected by a 4-frame window");
    CHECK(wide.evictable(13) == 0, "the window reports it unevictable");
    CHECK(wide.acquire(2, VtPageKey{0, 1, 0}, false, 14, w2, evicted),
          "the window expires exactly protect_frames after last use");
}

// ---------------------------------------------------------------------------
// Indirection table sub-allocator (exact sizes, size-class reuse, graveyard)
// ---------------------------------------------------------------------------
void test_table_allocator() {
    // Size classes are pow-2 from 16 words: internal fragmentation <= 2x.
    CHECK(VtTableAllocator::class_words(VtTableAllocator::size_class(1)) == 16,
          "tiny tables round to the 16-word floor class");
    CHECK(VtTableAllocator::class_words(VtTableAllocator::size_class(22)) == 32,
          "a 512^2 atlas's 22-entry table rounds to a 32-word block");
    CHECK(VtTableAllocator::class_words(
              VtTableAllocator::size_class(kVtMaxTableWords)) == 8192,
          "the worst-case table rounds to an 8192-word block");

    VtTableAllocator alloc;
    alloc.reset(/*capacity_words=*/1024);
    uint32_t off_a = 0, block_a = 0;
    uint64_t gen_a = 0;
    CHECK(alloc.acquire(22, /*frame=*/1, off_a, block_a, gen_a),
          "a fresh arena serves a table");
    CHECK(off_a == 0 && block_a == 32, "first block bumps from offset 0");
    uint32_t off_b = 0, block_b = 0;
    uint64_t gen_b = 0;
    CHECK(alloc.acquire(17, 1, off_b, block_b, gen_b),
          "a second table allocates");
    CHECK(off_b == 32 && block_b == 32,
          "same class packs contiguously off the bump pointer");
    CHECK(gen_b > gen_a, "generations are strictly monotonic");
    CHECK(alloc.used_words() == 64 && alloc.live_blocks() == 2,
          "accounting tracks live blocks exactly");

    // Graveyard: a released block is invisible to acquire until collect().
    alloc.release(off_a, block_a, /*retire_serial=*/9);
    CHECK(alloc.graveyard_blocks() == 1 && alloc.used_words() == 32,
          "release moves the block to the graveyard");
    uint32_t off_c = 0, block_c = 0;
    uint64_t gen_c = 0;
    CHECK(alloc.acquire(30, 2, off_c, block_c, gen_c),
          "acquire keeps working while the grave ages");
    CHECK(off_c == 64,
          "the graveyarded block is NOT reused early — fresh words instead");
    alloc.collect(8);
    CHECK(alloc.graveyard_blocks() == 1,
          "collect before the retire serial keeps the grave");
    alloc.collect(9);
    CHECK(alloc.graveyard_blocks() == 0, "collect at the serial matures it");
    uint32_t off_d = 0, block_d = 0;
    uint64_t gen_d = 0;
    CHECK(alloc.acquire(25, 10, off_d, block_d, gen_d),
          "a matured block serves the next same-class table");
    CHECK(off_d == off_a && block_d == 32,
          "size-class reuse hands back the freed block");
    CHECK(gen_d > gen_c, "reuse stamps a strictly newer generation");

    // Exhaustion fails closed: the arena refuses, nothing is disturbed.
    uint32_t off_e = 0, block_e = 0;
    uint64_t gen_e = 0;
    CHECK(!alloc.acquire(2000, 11, off_e, block_e, gen_e),
          "a table larger than the remaining arena is refused cleanly");
    CHECK(alloc.live_blocks() == 3 && alloc.used_words() == 96,
          "a refused acquire spends nothing");
    CHECK(!alloc.acquire(0, 11, off_e, block_e, gen_e),
          "a zero-word table is refused");
    CHECK(!alloc.acquire(kVtMaxTableWords * 2u, 11, off_e, block_e, gen_e),
          "a table beyond the largest class is refused");

    // release_now (registration rollback) returns capacity immediately.
    alloc.release_now(off_d, block_d);
    CHECK(alloc.used_words() == 64 && alloc.graveyard_blocks() == 0,
          "release_now skips the graveyard");
    CHECK(alloc.acquire(20, 11, off_e, block_e, gen_e) && off_e == off_d,
          "a rolled-back block is immediately reusable");
}

// ---------------------------------------------------------------------------
// Tail-gated activation (the streaming black-flash fix)
// ---------------------------------------------------------------------------
// Registration maps every entry to the pinned tail immediately, but the tail
// FILL drains through the bounded queue — the draw side must not route
// through the VT path until the fill's frame is submitted. This rule is what
// VtResidency::slot_active applies; a registered-but-unfilled-tail variant
// must report "not VT-active".
void test_tail_activation_rule() {
    CHECK(!vt_slot_activation_rule(/*live=*/true, kVtTailNotReady, 1000),
          "a registered variant whose tail was never written is NOT VT-active");
    CHECK(!vt_slot_activation_rule(/*live=*/false, 5, 1000),
          "a released variant is never VT-active");
    CHECK(!vt_slot_activation_rule(true, 101, 100),
          "not active before the tail's ready serial (fill frame + 1)");
    CHECK(vt_slot_activation_rule(true, 100, 100),
          "active exactly at the ready serial");
    CHECK(vt_slot_activation_rule(true, 100, 5000),
          "active ever after");
}

// ---------------------------------------------------------------------------
// Entry packing round-trip
// ---------------------------------------------------------------------------
void test_entry_packing() {
    for (uint32_t slot : {0u, 1u, 255u, 4095u, 65535u}) {
        for (uint32_t mip = 0; mip < kVtMaxMips; ++mip) {
            const VtEntry entry = vt_unpack_entry(vt_pack_entry(slot, mip));
            CHECK(entry.slot == slot && entry.mapped_mip == mip,
                  "entry pack/unpack round-trips");
        }
    }
}

// ---------------------------------------------------------------------------
// Registration cost model + the fail-closed capacity gates
// ---------------------------------------------------------------------------
// register_variant refuses a registration when either capacity gate is spent
// and the part then renders through the LEGACY per-material path — correct
// topology, authored surfaces() classification ignored. That silent fallback is
// what made StreamMountain's far field uniform tan, so the arithmetic behind it
// is pinned here: the per-variant cost, the gate order, and the invariant that a
// refused registration spends nothing.

// A StreamMountain-shaped rung-0 terrain sector: 64 m sector, one 4-material
// surfaces() tape, the packed material registry travelling with the part. The
// vertex/triangle counts are calibrated so vt_variant_mesh_bytes lands on the
// figure a real StreamMountain run reports -- 1024 registered variants held
// 97.7 MiB of copied mesh, i.e. ~97 KiB each. This is the shape of the world
// the shipped defaults have to fit, not a fixture for a particular bake.
struct SectorFixture {
    std::vector<float> positions, normals, uvs, material_table;
    std::vector<uint32_t> material_ids, indices;
    std::vector<uint8_t> weights;
    std::vector<uint32_t> materials;
    chart_atlas::ChartAtlasRung atlas;
    VtPartContext context;

    SectorFixture(uint32_t vertices, uint32_t triangles, uint32_t charts,
                  uint32_t tape_materials, uint32_t registry_materials,
                  uint32_t registry_stride) {
        positions.assign(size_t(vertices) * 3, 0.0f);
        normals.assign(size_t(vertices) * 3, 0.0f);
        uvs.assign(size_t(vertices) * 2, 0.0f);
        material_ids.assign(vertices, 0u);
        indices.assign(size_t(triangles) * 3, 0u);
        material_table.assign(size_t(registry_materials) * registry_stride, 0.0f);
        weights.assign(size_t(vertices) * tape_materials, 0u);
        materials.assign(tape_materials, 0u);
        atlas.atlas_w = 1024;
        atlas.atlas_h = 1024;
        atlas.charts.assign(charts, chart_atlas::ChartEntry{});
        atlas.tri_order.assign(triangles, 0u);
        context.vertex_count = vertices;
        context.triangle_count = triangles;
        context.positions = positions.data();
        context.normals = normals.data();
        context.surface_uvs = uvs.data();
        context.material_ids = material_ids.data();
        context.indices = indices.data();
        context.material_table = material_table.data();
        context.material_count = registry_materials;
        context.material_stride = registry_stride;
        context.surface_weights = weights.data();
        context.surface_materials = materials.data();
        context.surface_material_count = tape_materials;
    }
};

void test_registration_cost() {
    // Per-vertex cost with every stream present and a 4-column tape:
    // 12 + 12 + 8 + 4 (no tint) + 4 = 40 bytes. Per triangle: 12 (indices) + 4
    // (tri_order). Plus 64 bytes per chart and the registry snapshot.
    const uint32_t kVerts = 1300, kTris = 2470, kCharts = 24;
    SectorFixture s(kVerts, kTris, kCharts, /*tape_materials=*/4,
                    /*registry_materials=*/32, /*registry_stride=*/32);
    CHECK(vt_context_has_surface_tape(s.context),
          "a complete 4-material tape classification is usable");
    const size_t expected =
        size_t(kVerts) * (12 + 12 + 8 + 4 + 4) +         // vertex streams
        size_t(kTris) * 12 +                             // indices
        size_t(32) * 32 * 4 +                            // material registry
        4 * sizeof(uint32_t) +                           // tape material ids
        size_t(kCharts) * sizeof(chart_atlas::ChartEntry) +  // chart table
        size_t(kTris) * sizeof(uint32_t);                // tri_order
    CHECK(vt_variant_mesh_bytes(s.atlas, s.context) == expected,
          "the per-variant mesh cost is the sum of every copied stream");

    // The tape columns are only charged when they are actually adopted.
    VtPartContext untaped = s.context;
    untaped.surface_weights = nullptr;
    CHECK(!vt_context_has_surface_tape(untaped),
          "a tape with no weight matrix fails closed");
    CHECK(vt_variant_mesh_bytes(s.atlas, untaped) ==
              expected - size_t(kVerts) * 4 - 4 * sizeof(uint32_t),
          "a fail-closed tape costs neither weight columns nor material ids");

    // A null optional stream is not copied and not charged.
    VtPartContext bare = s.context;
    bare.normals = nullptr;
    bare.surface_uvs = nullptr;
    CHECK(vt_variant_mesh_bytes(s.atlas, bare) ==
              expected - size_t(kVerts) * (12 + 8),
          "null optional streams cost nothing");

    const size_t per_variant = vt_variant_mesh_bytes(s.atlas, s.context);
    CHECK(per_variant > 90u * 1024u && per_variant < 105u * 1024u,
          "a StreamMountain-shaped rung-0 sector variant costs ~95 KiB");

    // THE SIZING ARGUMENT for the shipped defaults, as an assertion.
    //
    // Two real measurements on StreamMountain (RTX 4090, 2026-07-29): the first
    // 1024 registered variants held 97.7 MiB (95 KiB each, all rung 0); at
    // 2048 registered they held 244.3 MiB (122 KiB each, the average pulled up
    // by the denser rung-1/rung-2 sectors near the camera). So 122 KiB is the
    // number to size against, not 95.
    //
    // Since the buffer indirection, MATTER_VT_MAX_VARIANTS (default 32768) is
    // a SOFT bookkeeping bound — the 2048 R16G16_UINT array-layer wall is
    // gone. The REAL working-set limits are the CPU mesh budget and the page
    // pool: at 122 KiB/variant, MATTER_VT_MESH_BUDGET_MB=1024 admits ~8.6k
    // variants — comfortably past the old 2048 wall (the redesign's point)
    // and below the variant-slot bound (so the byte budget, not the slot
    // table, is what a saturated world hits first, exactly as intended). If
    // the cost model grows a stream, these assertions say the budgets need
    // revisiting.
    SectorFixture dense(/*vertices=*/1700, /*triangles=*/3230, kCharts,
                        /*tape_materials=*/4, /*registry_materials=*/32,
                        /*registry_stride=*/32);
    const size_t measured_per_variant =
        vt_variant_mesh_bytes(dense.atlas, dense.context);
    CHECK(measured_per_variant > 118u * 1024u &&
              measured_per_variant < 128u * 1024u,
          "the measured StreamMountain per-variant average is ~122 KiB");
    const size_t default_budget_bytes = size_t(1024) * 1024 * 1024;
    const size_t default_max_variants = 32768;
    const size_t budget_admits = default_budget_bytes / measured_per_variant;
    CHECK(budget_admits > 4096,
          "the mesh budget admits a working set well past the old 2048 wall");
    CHECK(budget_admits < default_max_variants,
          "the byte budget, not the soft variant-slot bound, is the binding "
          "constraint");
    // And the indirection arena must NEVER bind before the mesh budget:
    // budget_admits variants at the worst-case table (8192-word block = 32
    // KiB) fit the 64 MiB default arena's typical mix — assert with the
    // TYPICAL table instead (1024^2 atlas -> 86 entries -> 128-word block).
    VtVariantLayout typical{};
    CHECK(vt_build_layout(1024, 1024, typical), "typical atlas builds");
    const size_t typical_block_bytes =
        VtTableAllocator::class_words(
            VtTableAllocator::size_class(typical.entry_count)) *
        4u;
    CHECK(budget_admits * typical_block_bytes < size_t(64) * 1024 * 1024,
          "MATTER_VT_INDIRECTION_MB=64 outlasts the mesh budget at typical "
          "table sizes");
}

void test_registration_gates() {
    SectorFixture s(/*vertices=*/1300, /*triangles=*/2470, /*charts=*/24,
                    /*tape_materials=*/4, /*registry_materials=*/32,
                    /*registry_stride=*/32);
    const size_t per_variant = vt_variant_mesh_bytes(s.atlas, s.context);

    // --- tiny layer cap: register N + 1, the last one is refused -------------
    const uint32_t caps = 4;   // the env floor for MATTER_VT_MAX_VARIANTS
    uint32_t live = 0;
    size_t used = 0;
    uint32_t rejected = 0;
    for (uint32_t i = 0; i < caps + 1u; ++i) {
        const VtRejectReason verdict = vt_registration_verdict(
            live, caps, used, /*budget=*/per_variant * 64, per_variant);
        if (verdict == VtRejectReason::Accept) {
            ++live;
            used += per_variant;
            continue;
        }
        CHECK(verdict == VtRejectReason::NoLayer,
              "the (N+1)th registration is refused for want of a layer");
        ++rejected;
    }
    CHECK(live == caps, "exactly the capped number of variants registered");
    CHECK(rejected == 1, "the census counts the one rejection");
    CHECK(used == size_t(caps) * per_variant,
          "a refused registration spends no mesh budget");

    // The refusal is not sticky: releasing a variant frees its layer AND its
    // bytes, and the next registration is accepted again.
    --live;
    used -= per_variant;
    CHECK(vt_registration_verdict(live, caps, used, per_variant * 64,
                                  per_variant) == VtRejectReason::Accept,
          "a freed layer is reusable");

    // --- tiny mesh budget: the layer is free but the bytes are not -----------
    CHECK(vt_registration_verdict(/*live=*/0, /*max=*/1024, /*used=*/0,
                                  /*budget=*/per_variant - 1, per_variant) ==
              VtRejectReason::MeshBudget,
          "a registration larger than the whole budget is refused");
    CHECK(vt_registration_verdict(0, 1024, 0, per_variant, per_variant) ==
              VtRejectReason::Accept,
          "a registration that exactly fills the budget is accepted");
    CHECK(vt_registration_verdict(0, 1024, 1, per_variant, per_variant) ==
              VtRejectReason::MeshBudget,
          "the budget is on the TOTAL, not the single registration");

    // Gate order matters: with both spent, layer exhaustion is what is
    // reported, because register_variant checks it first (and so must never be
    // seen to take a layer it then hands back).
    CHECK(vt_registration_verdict(4, 4, per_variant, per_variant,
                                  per_variant) == VtRejectReason::NoLayer,
          "layer exhaustion outranks the mesh budget in the verdict");

    // --- sampling falls back cleanly ---------------------------------------
    // A refused registration returns kVtNoSlot, which is 0, which is exactly
    // what an unclassified draw record already carries — so the shader's VT
    // branch is simply not taken. Nothing about a rejection can point a sample
    // at a layer that was never reset.
    CHECK(kVtNoSlot == 0u,
          "the no-VT sentinel is the zero a rejected part keeps");
    VtVariantLayout layout{};
    CHECK(vt_build_layout(s.atlas.atlas_w, s.atlas.atlas_h, layout),
          "the refused variant's layout was still well formed");
    VtIndirectionMap unregistered;   // never reset: no layer was taken
    CHECK(unregistered.resident_count() == 0,
          "a refused registration leaves no resident page behind");
    CHECK(!unregistered.in_range(0, 0, 0),
          "an unreset indirection layer resolves nothing in range");
    const VtEntry entry = unregistered.resolve(0, 0, 0);
    CHECK(entry.slot == 0 && entry.mapped_mip == 0,
          "resolving an unreset layer is defined and points at slot 0");
}


// ---------------------------------------------------------------------------
// M6.5 — caster selection and set identity
// ---------------------------------------------------------------------------
// The harvest itself lives in VkSceneRenderer and needs a device. These two
// functions are the parts that can be WRONG, and both fail quietly: a shadow
// that stops at a sector seam and one that does not look equally plausible in
// a screenshot, and a hash that churns re-bakes forever looks like "the bake is
// slow" rather than like a bug.
void test_caster_selection() {
    std::printf("== caster selection ==\n");
    const float rmin[3] = {0.0f, 0.0f, 0.0f};
    const float rmax[3] = {64.0f, 20.0f, 64.0f};

    auto at = [](float x, float z, float half) {
        vt::CasterCandidate c;
        c.aabb_min[0] = x - half; c.aabb_min[1] = 0.0f;  c.aabb_min[2] = z - half;
        c.aabb_max[0] = x + half; c.aabb_max[1] = 12.0f; c.aabb_max[2] = z + half;
        c.part_hash = 0xABCDEF01u; c.rung = 3;
        return c;
    };

    CHECK(vt::caster_overlaps_receiver(rmin, rmax, 0.0f, at(32.0f, 32.0f, 1.0f)),
          "a caster inside the sector is selected");

    // THE SEAM CASE, and the reason `reach` exists at all. A prop 10 m outside
    // the sector still casts into it when the sun is low; without the
    // expansion the shadow stops dead on the sector grid.
    CHECK(!vt::caster_overlaps_receiver(rmin, rmax, 0.0f, at(-10.0f, 32.0f, 1.0f)),
          "10 m outside is NOT selected with zero reach");
    CHECK(vt::caster_overlaps_receiver(rmin, rmax, 32.0f, at(-10.0f, 32.0f, 1.0f)),
          "...and IS selected once reach covers it (shadows cross seams)");
    CHECK(!vt::caster_overlaps_receiver(rmin, rmax, 32.0f, at(-200.0f, 32.0f, 1.0f)),
          "far outside stays excluded, so reach is a window and not a no-op");

    // Animated casters move every frame: a baked shadow would be wrong the
    // moment it landed AND the set hash would never settle.
    {
        vt::CasterCandidate a = at(32.0f, 32.0f, 1.0f);
        a.animated = true;
        CHECK(!vt::caster_overlaps_receiver(rmin, rmax, 32.0f, a),
              "animated casters are excluded");
        vt::CasterCandidate self = at(32.0f, 32.0f, 1.0f);
        self.is_receiver = true;
        CHECK(!vt::caster_overlaps_receiver(rmin, rmax, 32.0f, self),
              "the receiver does not cast onto itself (that is the contact tier)");
    }

    // ---- set identity ----
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    vt::CasterCandidate a = at(10.0f, 10.0f, 1.0f);
    vt::CasterCandidate b = at(40.0f, 12.0f, 1.0f);
    b.part_hash = 0x1234u;

    const vt::CasterCandidate ab[2] = {a, b};
    const vt::CasterCandidate ba[2] = {b, a};
    const uint64_t h_ab = vt::caster_set_hash(ab, 2, origin);
    CHECK(h_ab == vt::caster_set_hash(ba, 2, origin),
          "set hash ignores harvest ORDER (or it re-bakes on instance churn "
          "with nothing having moved)");
    CHECK(h_ab != vt::caster_set_hash(ab, 1, origin),
          "dropping a caster changes the hash");

    {
        vt::CasterCandidate moved[2] = {a, b};
        moved[1].aabb_min[0] += 5.0f;
        moved[1].aabb_max[0] += 5.0f;
        CHECK(h_ab != vt::caster_set_hash(moved, 2, origin),
              "MOVING a caster changes the hash (or a stale shadow persists)");
    }
    {
        // Below the quantum: nothing visible moved, so nothing should re-bake.
        vt::CasterCandidate jittered[2] = {a, b};
        jittered[1].aabb_min[0] += 0.0001f;
        jittered[1].aabb_max[0] += 0.0001f;
        CHECK(h_ab == vt::caster_set_hash(jittered, 2, origin),
              "sub-quantum jitter does NOT change the hash (no re-bake churn)");
    }
    {
        vt::CasterCandidate rerung[2] = {a, b};
        rerung[1].rung += 1;
        CHECK(h_ab != vt::caster_set_hash(rerung, 2, origin),
              "a different caster rung is a different set");
    }
    {
        // Duplicates must not annihilate: an XOR combine would make two
        // identical casters hash as zero of them.
        const vt::CasterCandidate dup[2] = {a, a};
        CHECK(vt::caster_set_hash(dup, 2, origin) !=
                  vt::caster_set_hash(&a, 0, origin),
              "duplicate casters do not cancel out");
    }
    CHECK(vt::caster_set_hash(nullptr, 0, origin) !=
              vt::caster_set_hash(&a, 1, origin),
          "an empty set differs from a one-caster set");
}

}  // namespace

int main() {
    test_layout();
    test_indirection();
    test_indirection_dirty();
    test_border_math();
    test_slot_pool();
    test_slot_hysteresis();
    test_table_allocator();
    test_tail_activation_rule();
    test_entry_packing();
    test_registration_cost();
    test_registration_gates();
    test_caster_selection();
    std::printf("vt residency tests complete\n");
    return check_summary();
}
