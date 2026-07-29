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

    // 8192 is the worst case: mips 8192..64 is 8 levels and the stacked page
    // grids (64+32+16+8+4+2+1+1) fill the indirection layer exactly.
    CHECK(vt_build_layout(8192, 8192, layout), "8192 atlas builds");
    CHECK(layout.mip_count == 8, "8192 atlas has 8 mips down to the tail");
    CHECK(layout.page_w[0] == 64 && layout.page_h[0] == 64,
          "8192 atlas is 64x64 pages at mip 0");
    CHECK(layout.page_w[7] == 1 && layout.page_h[7] == 1,
          "the tail mip is a single page");
    CHECK(layout.mip_row[0] == 0, "mip 0 starts at row 0");
    CHECK(layout.mip_row[1] == 64, "mip 1 starts after mip 0's 64 rows");
    CHECK(layout.mip_row[7] + layout.page_h[7] == kVtIndirectionHeight,
          "the stacked mip rows fill the indirection layer exactly");

    // A small atlas stops at the first mip inside the tail budget.
    CHECK(vt_build_layout(256, 128, layout), "256x128 atlas builds");
    CHECK(layout.mip_count == 3,
          "256x128 needs mips 256x128, 128x64, 64x32 (tail)");
    CHECK(layout.page_w[0] == 2 && layout.page_h[0] == 1,
          "256x128 is 2x1 pages at mip 0");
    CHECK(layout.page_w[2] == 1 && layout.page_h[2] == 1,
          "the 64x32 tail is one page");

    // An atlas already inside the tail budget is a single (tail) mip.
    CHECK(vt_build_layout(64, 64, layout), "64x64 atlas builds");
    CHECK(layout.mip_count == 1, "a 64x64 atlas is nothing but its tail");
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

    // Texel packing must match R16G16_UINT (R = slot, G = mapped mip).
    map.map(0, 0, 0, 300);
    const std::vector<uint32_t>& texels = map.texels();
    const uint32_t packed =
        texels[VtIndirectionMap::texel_index_for(layout, 0, 0, 0)];
    CHECK((packed & 0xFFFFu) == 300u, "entry low half is the physical slot");
    CHECK((packed >> 16) == 0u, "entry high half is the mapped mip");
    CHECK(map.texels().size() == kVtIndirectionWidth * kVtIndirectionHeight,
          "the mirror is exactly one indirection layer");
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
    CHECK(pinned_pool.evictable() == 1,
          "only the unpinned slot is ever evictable");

    // A pool of nothing but pins cannot serve a fill; acquire must fail rather
    // than steal a tail (stealing a tail would break the never-fault promise).
    VtSlotPool all_pinned;
    all_pinned.reset(1);
    uint32_t only = 0;
    CHECK(all_pinned.acquire(1, VtPageKey{0, 0, 0}, true, 0, only, evicted),
          "the single slot pins");
    uint32_t denied = 0;
    CHECK(!all_pinned.acquire(2, VtPageKey{0, 0, 0}, false, 1, denied, evicted),
          "an all-pinned pool refuses to evict a pinned tail");

    // release() returns a slot to the free list and un-counts the pin.
    all_pinned.release(only);
    CHECK(all_pinned.used() == 0 && all_pinned.pinned() == 0,
          "releasing a pinned slot clears both counters");
    CHECK(all_pinned.acquire(3, VtPageKey{0, 0, 0}, false, 2, denied, evicted),
          "the released slot is reusable");
    CHECK(!evicted.live, "reusing a released slot is not an eviction");

    // A zero-capacity pool fails closed rather than indexing nothing.
    VtSlotPool empty;
    empty.reset(0);
    uint32_t none = 0;
    CHECK(!empty.acquire(1, VtPageKey{0, 0, 0}, false, 0, none, evicted),
          "a zero-capacity pool never hands out a slot");
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

}  // namespace

int main() {
    test_layout();
    test_indirection();
    test_indirection_dirty();
    test_border_math();
    test_slot_pool();
    test_entry_packing();
    std::printf("vt residency tests complete\n");
    return check_summary();
}
