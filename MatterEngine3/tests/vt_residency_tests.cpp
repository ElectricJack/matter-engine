// vt_residency_tests.cpp — WP-E headless unit gate for the VT residency
// addressing and bookkeeping layer (vt_residency.h's pure CPU half).
//
// Everything here runs without a Vulkan device: layout construction, the
// indirection resolve the shader mirrors, page/border addressing, and LRU
// eviction order. The GPU half (pool images, fills, feedback) is covered by
// MATTER_VK_SMOKE_MODE=vt in vulkan_smoke_tests.cpp.

#include "check.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

#include "render/vt_residency.h"
#include "render/vt_density.h"
#include "render/vt_canonical_page.h"
#include "render/vt_world_receivers.h"
#include "render/vt_resolve_bvh.h"

using namespace vt;

namespace {

void test_world_receiver_frames() {
    // Parent turns +X into -Z, raises the placement 16m, then the child
    // translates two local metres along +X. This matches expanded draw frames.
    const float parent[]={0,0,1,8, 0,1,0,16, -1,0,0,5, 0,0,0,1};
    const float child[]={1,0,0,2, 0,1,0,.5f, 0,0,1,0, 0,0,0,1};
    VtWorldReceiverFrame frame;frame.add(parent,child);
    CHECK(frame.supported() && frame.local_to_world[3]==8 && frame.local_to_world[7]==16.5f &&
          frame.local_to_world[11]==3,"world receiver: parent rotation and all child translation axes compose");
    frame.add(parent,child);
    CHECK(!frame.supported(),"world receiver: two physical placements cannot masquerade as one frame");
    frame={};frame.add(parent);frame.local_to_world[0]=2;
    CHECK(!frame.supported(),"world receiver: scaled/sheared frames need explicit physical-height support");
    frame={};frame.add(parent);frame.local_to_world[3]=NAN;
    CHECK(!frame.supported(),"world receiver: nonfinite transforms fail closed");
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
void test_layout() {
    VtVariantLayout layout{};
    CHECK(!vt_build_layout(0, 512, layout), "zero atlas width is rejected");
    CHECK(!vt_build_layout(512, 0, layout), "zero atlas height is rejected");
    CHECK(!vt_build_layout(chart_atlas::kVtMaxAtlasDim * 2, 512, layout),
          "atlas wider than kVtMaxAtlasDim is rejected");

    // The existing 8K layout stays unchanged when 16K support is enabled.
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
    CHECK(layout.entry_count == 5462, "the 8192 atlas still needs 5462 entries");
    CHECK(vt_build_layout(16384, 16384, layout), "16384 atlas builds");
    CHECK(layout.mip_count == 9 && layout.page_w[0] == 128 && layout.page_h[0] == 128 &&
          layout.page_w[8] == 1 && layout.page_h[8] == 1 &&
          layout.mip_offset[8] == 21845 && layout.entry_count == 21846 &&
          layout.entry_count == kVtMaxTableWords,
          "16K layout covers high page coordinates and reaches the unchanged 64-texel tail");

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

void test_feedback_residency_equivalence() {
    for (const auto extent : {std::array<uint32_t, 2>{16384, 16384}, {16383, 8193}, {8192, 8192}, {1920, 1080}, {64, 32}}) {
        VtVariantLayout layout{};
        CHECK(vt_build_layout(extent[0], extent[1], layout), "feedback residency: valid atlas");
        VtIndirectionMap map;
        map.reset(layout, 7);
        const uint32_t tail = layout.mip_count - 1;
        map.map(tail, 0, 0, 7); // same lifetime invariant as runtime registration
        uint32_t random = 0x712347ABu;
        for (uint32_t change = 0; change < 32; ++change) {
            random = random * 1664525u + 1013904223u;
            if (tail) {
                const uint32_t mip = random % tail;
                const uint32_t x = (random >> 8) % layout.page_w[mip];
                const uint32_t y = (random >> 16) % layout.page_h[mip];
                if (change & 1u) map.unmap(mip, x, y);
                else map.map(mip, x, y, 100 + change);
            }
            bool equivalent = true;
            for (uint32_t mip = 0; mip < layout.mip_count; ++mip)
                for (uint32_t y = 0; y < layout.page_h[mip]; ++y)
                    for (uint32_t x = 0; x < layout.page_w[mip]; ++x)
                        equivalent = equivalent &&
                            (map.is_mapped(mip, x, y) == (map.resolve(mip, x, y).mapped_mip == mip));
            CHECK(equivalent, "feedback residency: resolved mip equals exact resident membership after edits");
        }
    }
}

void test_feedback_collection() {
    // Independent tuple-set oracle: exercises all four fields, repeated rows,
    // more distinct pages than filter buckets, empty texels and u16 boundaries.
    using Request = std::array<uint32_t, 4>; // owner index, mip, y, x
    std::set<Request> expected;
    std::vector<uint16_t> texels;
    for (uint32_t i = 0; i < 48000; ++i) {
        const uint32_t key = i % 6000u;
        const uint16_t owner = i % 19u ? uint16_t(1 + key % 6u) : 0;
        const uint16_t mip = uint16_t((key / 6u) % 8u);
        const uint16_t x = uint16_t((key / 48u) % 64u);
        const uint16_t y = uint16_t(key / 3072u);
        texels.insert(texels.end(), {owner, x, y, mip});
        if (owner) expected.insert({uint32_t(owner - 1), mip, y, x});
    }
    texels.insert(texels.end(), {65535, 65535, 65535, 65535});
    expected.insert({65534, 65535, 65535, 65535});
    expected.insert({65534, 7, 63, 63});
    VtFeedbackKeys collector;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        collector.begin();
        collector.add_request(65534, 7, 63, 63);
        collector.add_request(0, 0, 0, 0); // already present in the image
        collector.add_request(65535, 0, 0, 0); // unencodable, must not alias owner zero
        collector.add_request(0, 0, 65536, 0);
        collector.append_texels(texels.data(), texels.size() / 4u);
        std::vector<Request> actual;
        for (uint64_t key : collector.finish())
            actual.push_back({uint32_t(key >> 48) - 1u, uint32_t((key >> 32) & 65535u),
                              uint32_t((key >> 16) & 65535u), uint32_t(key & 65535u)});
        CHECK(actual == std::vector<Request>(expected.begin(), expected.end()),
              "feedback collection: exact canonical requests survive collisions and frame reuse");
    }
    collector.begin();
    CHECK(collector.finish().empty(), "feedback collection: empty frame retains no previous requests");

    // Only uint16 alignment is part of append_texels' contract. Empty texels
    // may contain arbitrary other channels; they must not interrupt duplicate
    // runs or turn into page demand. Distinct fields must retain tuple order.
    alignas(uint64_t) const uint16_t offset_texels[] = {
        99, 7, 101, 203, 5, 0, 9, 8, 7, 7, 101, 203, 5,
        7, 102, 203, 5, 7, 102, 204, 5, 7, 102, 204, 6,
    };
    collector.append_texels(offset_texels + 1, 6);
    const std::vector<uint64_t> offset_expected = {
        (uint64_t{7} << 48) | (uint64_t{5} << 32) | (uint64_t{203} << 16) | 101,
        (uint64_t{7} << 48) | (uint64_t{5} << 32) | (uint64_t{203} << 16) | 102,
        (uint64_t{7} << 48) | (uint64_t{5} << 32) | (uint64_t{204} << 16) | 102,
        (uint64_t{7} << 48) | (uint64_t{6} << 32) | (uint64_t{204} << 16) | 102,
    };
    CHECK(collector.finish() == offset_expected && collector.raw_hits() == 5 &&
              collector.run_hits() == 4,
          "feedback collection: unaligned texels preserve fields, empty gaps and hit counters");
}

// Opt-in native CPU benchmark. Timings are evidence, never pass/fail limits;
// the tuple-set regression above supplies the independent correctness oracle.
void benchmark_feedback_collection() {
    if (!std::getenv("MATTER_VT_FEEDBACK_BENCH")) return;
    using Clock = std::chrono::steady_clock;
    constexpr size_t count = 32400;
    for (uint32_t pattern = 0; pattern < 3; ++pattern) {
        std::vector<uint16_t> texels(1 + count * 4);
        for (size_t i = 0; i < count; ++i) {
            const uint32_t key = static_cast<uint32_t>(
                pattern == 1 ? (i / 6) % 735 : i % 6000);
            auto* t = texels.data() + 1 + i * 4;
            t[0] = pattern == 0 || (i % 11 == 0) ? 0 : uint16_t(1 + key % 7);
            t[1] = uint16_t(key / 7 % 64);
            t[2] = uint16_t(key / 448);
            t[3] = uint16_t(key % 5);
        }
        VtFeedbackKeys collector;
        std::vector<double> scan_us, total_us;
        uint64_t checksum = 0;
        for (uint32_t frame = 0; frame < 1050; ++frame) {
            const auto begin = Clock::now();
            collector.begin(count / 8);
            const auto scan_begin = Clock::now();
            collector.append_texels(texels.data() + 1, count);
            const auto scan_end = Clock::now();
            const auto& keys = collector.finish();
            checksum += keys.size();
            if (!keys.empty()) checksum ^= keys.front() ^ keys.back();
            const auto end = Clock::now();
            if (frame < 50) continue;
            scan_us.push_back(std::chrono::duration<double, std::micro>(scan_end - scan_begin).count());
            total_us.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
        }
        std::sort(scan_us.begin(), scan_us.end());
        std::sort(total_us.begin(), total_us.end());
        std::printf("VT_FEEDBACK_BENCH,%u,%zu,%.3f,%.3f,%.3f,%.3f,%llu\n",
                    pattern, scan_us.size(), scan_us[499], scan_us[949],
                    total_us[499], total_us[949],
                    static_cast<unsigned long long>(checksum));
    }
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
              VtTableAllocator::size_class(kVtMaxTableWords)) == 32768,
          "the worst-case table rounds to a 32768-word block");

    VtTableAllocator large;
    large.reset(32768);
    uint32_t large_offset=0,large_words=0;uint64_t large_generation=0;
    CHECK(large.acquire(21846,1,large_offset,large_words,large_generation) &&
          large_offset==0 && large_words==32768 && large.used_words()==32768,
          "16K table fits its exact bounded arena class");
    large.release(large_offset,large_words,9);
    CHECK(!large.acquire(21846,8,large_offset,large_words,large_generation),
          "large table cannot reuse storage still visible to readers");
    large.collect(9);
    CHECK(large.acquire(21846,9,large_offset,large_words,large_generation) && large_offset==0,
          "large table reuses storage after the reader horizon");

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
    // The independent 64 MiB indirection budget fits the typical mix at the
    // mesh budget. This does not guarantee the worst-case 16K mix fits:
    // a maximum table rounds to 32768 words (128 KiB). Keep that capacity
    // gate rather than growing the arena or sampling unregistered records.
    // Typical 1024^2 atlas -> 86 entries -> 128-word block.
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


void test_page_density() {
    chart_atlas::ChartAtlasRung atlas;
    atlas.atlas_w = atlas.atlas_h = 128;
    chart_atlas::ChartEntry chart{};
    chart.tangent[0] = chart.bitangent[1] = 1;
    chart.texels_per_meter = 1;
    chart.rect_w = chart.rect_h = 128;
    chart.tri_count = 2;
    atlas.charts.push_back(chart);
    atlas.tri_order = {0, 1};
    float positions[] = {0,0,0, 120,0,0, 120,120,0, 0,120,0};
    const uint32_t indices[] = {0,1,2, 0,2,3};
    VtPartContext ctx;
    ctx.positions = positions; ctx.vertex_count = 4;
    ctx.indices = indices; ctx.triangle_count = 2;
    auto density = vt_measure_page_density(atlas, ctx, 0, 0, 0);
    CHECK(density.geometry_available && density.triangle_texels == 14400,
          "density: a 120-square surface covers 14400 centers, excluding gutters");
    CHECK(density.atlas_texels == 16384 && density.chart_block_texels == 16384 &&
          density.gutter_bounds_texels == 16384 && density.content_bounds_texels == 14400,
          "density: surface bounds, padded bounds, chart block, and atlas are separate");
    atlas.tri_order.push_back(0); atlas.charts[0].tri_count = 3;
    CHECK(vt_measure_page_density(atlas, ctx, 0, 0, 0).triangle_texels == 14400,
          "density: duplicate triangle coverage counts once");
    atlas.charts[0].tri_count = 1;
    density = vt_measure_page_density(atlas, ctx, 0, 0, 0);
    CHECK(density.triangle_texels == 7260 && density.content_bounds_texels == 14400,
          "density: a triangle's bounding rectangle does not count as useful coverage");
    atlas.charts[0].tri_count = 2;
    positions[3] = positions[6] = 16; positions[7] = positions[10] = 16;
    density = vt_measure_page_density(atlas, ctx, 0, 0, 0);
    CHECK(density.triangle_texels == 256 && density.gutter_bounds_texels == 576 &&
          density.chart_block_texels == 16384,
          "density: page rounding is separate from a small chart's gutter");
    density = vt_measure_page_density(atlas, ctx, 1, 0, 0);
    CHECK(density.atlas_texels == 4096 && density.triangle_texels == 64,
          "density: the 64-square tail uses a full 128-square payload slot");
    atlas.atlas_w = atlas.charts[0].rect_w = 256;
    positions[3] = positions[6] = 240; positions[7] = positions[10] = 120;
    const auto left = vt_measure_page_density(atlas, ctx, 0, 0, 0);
    const auto right = vt_measure_page_density(atlas, ctx, 0, 1, 0);
    CHECK(left.triangle_texels == 14880 && right.triangle_texels == 13920,
          "density: page-boundary clipping conserves coverage over a two-page surface");
    density = vt_measure_page_density(atlas, ctx, 2, 0, 0);
    CHECK(density.atlas_texels == 2048 && density.triangle_texels == 1800,
          "density: rectangular tail bounds and mip-scaled centers are preserved");
    // Rotate the chart and mesh together; stored vertex UVs are not consulted.
    atlas.charts[0].tangent[0] = 0; atlas.charts[0].tangent[2] = 1;
    for (int i = 0; i < 4; ++i) { positions[3*i+2] = positions[3*i]; positions[3*i] = 0; }
    CHECK(vt_measure_page_density(atlas, ctx, 2, 0, 0).triangle_texels == 1800,
          "density: projected coverage follows rotated chart frames");
    ctx.positions = nullptr;
    density = vt_measure_page_density(atlas, ctx, 2, 0, 0);
    CHECK(!density.geometry_available && density.atlas_texels == 2048,
          "density: unavailable geometry is explicit, not a measured empty surface");
}

void test_material_page_ownership() {
    VtMaterialPages pages;
    pages.reset(4);
    const auto a = VtMaterialPages::key({17, 29}, 1, {-.01f, .02f, 1});
    const auto b = VtMaterialPages::key({18, 29}, 1, {-.01f, .02f, 1});
    auto first = pages.publish(0, a);
    CHECK(first.write && first.slot != 0, "pixels: allocation is independent of receiver slot");
    const auto second = pages.publish(1, a);
    CHECK(!second.write && second.slot == first.slot && pages.used() == 1 && pages.references() == 2,
          "pixels: two receivers share one immutable material allocation");
    auto edit = pages.publish(0, b);
    CHECK(edit.write && edit.slot != first.slot && pages.slot(1) == first.slot,
          "pixels: a local edit copies on write without changing the other receiver");
    CHECK(!pages.publish(0, a).write && pages.used() == 1,
          "pixels: removing an override rejoins the shared base");
    pages.release(0, 20);
    edit = pages.publish(1, b);
    CHECK(edit.write && edit.slot != first.slot,
          "pixels: a surviving owner cannot overwrite deleted owner's in-flight pixels");
    CHECK(pages.publish(2, a).slot != first.slot,
          "pixels: retired payload is unavailable before its reader horizon");
    pages.collect(19);
    pages.publish(3, {});
    const uint32_t before = pages.slot(1);
    pages.release(2, 30);
    CHECK(pages.publish(2, b).slot == before,
          "pixels: sharing remains possible while retired storage is full");
    // Split the final shared allocation with no free slots: publication must
    // fail without corrupting either prior binding.
    CHECK(pages.publish(1, a).slot == UINT32_MAX && pages.slot(1) == before && pages.slot(2) == before,
          "pixels: exhausted copy-on-write keeps both previous bindings");
    pages.collect(20);
    CHECK(pages.publish(1, a).write && pages.slot(1) == first.slot,
          "pixels: retirement permits bounded storage reuse");
    pages.reset(4);
    first = pages.publish(0, a);
    auto different = a; different.snapshot = 2;
    CHECK(pages.publish(1, different).slot != first.slot,
          "pixels: different input lifetimes cannot alias through recycled bank indices");
    different = a; different.height[1] ^= 1;
    CHECK(pages.publish(2, different).slot != first.slot,
          "pixels: height decode is part of shared payload compatibility");
    const auto private_page = pages.publish(3, {});
    CHECK(private_page.write && pages.used() == 4 && pages.publish(3, {}).write,
          "pixels: unspecified identity always follows the private producer path");
}

void test_canonical_material_grid() {
    float positions[]={0,0,0, 4,0,0, 4,0,4, 0,0,4};
    const float normals[]={0,1,0, 0,1,0, 0,1,0, 0,1,0};
    uint32_t indices[]={0,1,2, 0,2,3},material=9;
    chart_atlas::ChartAtlasRung atlas;
    atlas.atlas_w=atlas.atlas_h=512;atlas.tri_order={0,1};atlas.charts.resize(1);
    auto& c=atlas.charts[0];c={};c.tangent[0]=1;c.bitangent[2]=1;
    c.texels_per_meter=64;c.tri_count=2;c.rect_w=c.rect_h=384;
    VtPartContext context;
    context.positions=positions;context.normals=normals;context.indices=indices;
    context.vertex_count=4;context.triangle_count=2;context.surface_material_count=1;
    context.surface_materials=&material;context.surface_tape_text="constant material fixture";
    VtFillRequest request;request.atlas=&atlas;request.part_context=&context;request.page_x=request.page_y=1;
    VtCanonicalPage a,b;
    CHECK(vt_canonical_page(request,0,{-.01f,.02f,1},a),"canonical grid: covered rectangle includes all stored gutters");
    positions[3]=positions[6]=8;c.rect_x=128;c.rect_w=640;atlas.atlas_w=1024;request.page_x=2;
    CHECK(vt_canonical_page(request,0,{-.01f,.02f,1},b) && a.key.low==b.key.low && a.key.high==b.key.high &&
          a.origin==b.origin && a.du==b.du && a.dv==b.dv,
          "canonical grid: wall resize and chart repacking preserve exact material coordinates and identity");
    request.page_x=1;
    CHECK(!vt_canonical_page(request,0,{-.01f,.02f,1},b),"canonical grid: boundary dilation stays private");
    request.page_x=3;
    CHECK(vt_canonical_page(request,0,{-.01f,.02f,1},b) && a.key.low!=b.key.low,
          "canonical grid: shifted physical sampling phase cannot alias");
    request.page_x=2;indices[3]=1;indices[4]=2;indices[5]=3;
    CHECK(!vt_canonical_page(request,0,{-.01f,.02f,1},b),"canonical grid: overlapping triangles and uncovered rectangle rejected");
    indices[3]=0;indices[4]=2;indices[5]=3;positions[7]=.01f;
    CHECK(!vt_canonical_page(request,0,{-.01f,.02f,1},b),"canonical grid: a nonplanar receiver stays private");
    positions[7]=0;context.surface_lane_count=1;
    CHECK(!vt_canonical_page(request,0,{-.01f,.02f,1},b),"canonical grid: geometry-dependent field interpolation stays private");
    context.surface_lane_count=0;context.surface_tape_text="edited material fixture";
    CHECK(vt_canonical_page(request,0,{-.01f,.02f,1},b) && a.key.low!=b.key.low,
          "canonical grid: material appearance edits invalidate pixel identity");
    context.surface_tape_text="constant material fixture";
    context.dominant_material=18;
    CHECK(vt_canonical_page(request,0,{-.01f,.02f,1},b) && a.key.low!=b.key.low,
          "canonical grid: original receiver material is part of shared pixel identity");
    uint32_t receiver_ids[]={18,18,19,18};context.material_ids=receiver_ids;
    indices[3]=2;indices[4]=3;indices[5]=0; // same face, different first-corner category
    CHECK(!vt_canonical_page(request,0,{-.01f,.02f,1},b),
          "canonical grid: mixed receiver identities cannot alias a uniform material rectangle");
}

void test_material_reads() {
    VtMaterialPages pages; pages.reset(2);
    const auto a = VtMaterialPages::key({71, 11}, 1, {-.02f, .02f, 1});
    const auto b = VtMaterialPages::key({72, 11}, 1, {-.02f, .02f, 1});
    const auto old = pages.publish(0, a);
    auto read = pages.retain_read(0), alias = read;
    CHECK(read && read->slot() == old.slot && pages.read_pages() == 1,
          "read lease: encoded material address retained independently of receiver");
    read->retain_until(20);
    const auto replacement = pages.publish(0, b);
    CHECK(replacement.write && replacement.slot != old.slot && pages.used() == 2 &&
          pages.references() == 1 && pages.shared_references() == 0,
          "read lease: sole receiver replacement copies on write without counting readers as sharing savings");
    CHECK(pages.publish(1, {}).slot == UINT32_MAX,
          "read lease: memory pressure cannot steal retained source pixels");
    const auto rejoin = pages.publish(1, a);
    CHECK(!rejoin.write && rejoin.slot == old.slot,
          "read lease: identical content may rejoin a read-only allocation");
    pages.release(1); read.reset(); alias->retain_until(40); alias.reset();
    CHECK(!pages.read_pages() && pages.used() == 1,
          "read lease: final release leaves only the replacement logically live");
    pages.collect(39);
    CHECK(pages.publish(1, {}).slot == UINT32_MAX,
          "read lease: released source bytes remain protected through the last recorded use");
    pages.collect(40);
    CHECK(pages.publish(1, {}).slot == old.slot,
          "read lease: retirement returns source capacity at the exact horizon");
    read = pages.retain_read(0); pages.reset(2); pages.publish(0, a);
    read->retain_until(100); read.reset();
    CHECK(pages.used() == 1 && !pages.read_pages() && pages.references() == 1,
          "read lease: old allocator epoch cannot release or pin new allocations");
    { VtMaterialPages temporary; temporary.reset(1); temporary.publish(0, a); read = temporary.retain_read(0); }
    read->retain_until(200); read.reset();

    const gpu_meshing::FaceFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    VtPeriodicDomain domain; std::string error;
    std::vector<std::array<uint32_t, 2>> tiles;
    CHECK(vt_make_periodic_domain(frame, {4,4}, 128, domain, error), error.c_str());
    const std::vector<std::array<uint32_t, 2>> wrapped{{0,1},{3,1}};
    CHECK(vt_material_read_tiles(domain, 0, {-.02,.3,.02,.4}, tiles, error) && tiles == wrapped,
          "read footprint: negative wrapped bounds include both period edges exactly once");
    CHECK(vt_material_read_tiles(domain, 0, {.98,.3,1.02,.4}, tiles, error) && tiles == wrapped,
          "read footprint: positive and negative periods have identical dependencies");
    CHECK(vt_material_read_tiles(domain, 0, {-2,-3,4,5}, tiles, error) && tiles.size() == 16,
          "read footprint: repeated periods do not duplicate page dependencies");
    const std::vector<std::array<uint32_t,2>> boundary{{0,1},{1,1}};
    CHECK(vt_material_read_tiles(domain, 0, {.25,.35,.25,.35}, tiles, error) && tiles == boundary,
          "read footprint: a boundary point includes bilinear support on both pages");
    CHECK(vt_make_periodic_domain(frame, {3,2}, 128, domain, error) &&
          vt_material_read_tiles(domain, 1, {.98,.4,1.02,.5}, tiles, error) && tiles.size() == 2,
          "read footprint: non-power-of-two dimensions use the actual mip grid");
    CHECK(!vt_material_read_tiles(domain, 0, {NAN,0,1,1}, tiles, error) && tiles.empty() &&
          !vt_material_read_tiles(domain, 0, {1,0,0,1}, tiles, error),
          "read footprint: nonfinite or reversed bounds cannot produce partial dependencies");
    CHECK(vt_make_periodic_domain(frame, {64,64}, 128, domain, error) &&
          !vt_material_read_tiles(domain, 0, {0,0,1,1}, tiles, error) && tiles.empty(),
          "read footprint: oversized jobs fail explicitly without reducing mip quality");
}

void test_receiver_coverage_pages() {
    float positions[]={0,0,0, 4,0,0, 4,4,0, 0,4,0};
    float normals[]={0,0,1, 0,0,1, 0,0,1, 0,0,1};
    uint32_t indices[]={0,1,2,0,2,3},material=9,ids[]={9,9,9,9};
    uint8_t weights[]={255,255,255,255};
    chart_atlas::ChartAtlasRung atlas;
    atlas.atlas_w=atlas.atlas_h=640;atlas.tri_order={0,1};atlas.charts.resize(1);
    auto& chart=atlas.charts[0];chart={};chart.tangent[0]=chart.bitangent[1]=1;
    chart.texels_per_meter=128;chart.tri_count=2;chart.rect_w=chart.rect_h=640;
    VtPartContext context;context.positions=positions;context.normals=normals;context.indices=indices;
    context.vertex_count=4;context.triangle_count=2;context.surface_material_count=1;
    context.surface_materials=&material;context.surface_weights=weights;context.dominant_material=material;
    context.surface_tape_text="coverage geometry fixture";
    std::vector<VtReceiverMaterialGpu> mappings(1);
    auto& map=mappings[0];map.binding[0]=1;map.binding[1]=1;
    // Physical metres, expressed in normalized atlas coordinates.
    map.uv_u[0]=map.uv_v[1]=5;map.uv_u[2]=map.uv_v[2]=-4.f/128;
    const auto bounds=[&](float low,float high) {
        std::memcpy(map.binding+2,&low,4);std::memcpy(map.binding+3,&high,4);
    };
    bounds(0,4);
    const auto eligible=[&](uint32_t mip,uint32_t x,uint32_t y) {
        return vt_receiver_material_page(*VtPartSnapshot::capture(atlas,context),mappings,mip,x,y);
    };
    CHECK(eligible(0,1,1) && eligible(0,2,2),"coverage-only: wholly mapped rectangle interiors qualify");
    CHECK(!eligible(0,0,1) && !eligible(0,4,1) && !eligible(0,1,0) && !eligible(0,1,4),
          "coverage-only: stored gutters and finite boundaries retain complete material");
    CHECK(!eligible(1,1,1) && !eligible(2,0,0),"coverage-only: coarser footprints touching finite edges keep their pixels");
    // Page (1,1) starts near .94m, while its centre is 1.47m. A centre-only
    // test would accept this interval and expose missing material at its edge.
    bounds(1,2.1f);
    CHECK(!eligible(0,1,1),"coverage-only: interval must cover gutters, not only page centre");
    bounds(.9f,2.1f);
    CHECK(eligible(0,1,1),"coverage-only: bounded material interior still qualifies");
    bounds(0,4);map.binding[0]=0;
    CHECK(!eligible(0,1,1),"coverage-only: removed mapping requires finite pixels");map.binding[0]=1;
    bounds(NAN,4);
    CHECK(!eligible(0,1,1),"coverage-only: malformed nonfinite interval cannot discard pixels");bounds(0,4);
    context.material_ids=ids;ids[2]=3;
    CHECK(!eligible(0,1,1),"coverage-only: mixed carrier IDs retain ordinary AUX evaluation");ids[2]=9;
    CHECK(eligible(0,1,1),"coverage-only: explicit matching carrier IDs qualify");
    indices[3]=1;indices[4]=2;indices[5]=3;
    CHECK(!eligible(0,1,1),"coverage-only: bounding rectangle cannot hide holes/overlapping triangles");
    indices[3]=0;indices[4]=2;indices[5]=3;positions[8]=.01f;
    CHECK(!eligible(0,1,1),"coverage-only: curved/nonplanar receivers retain complete material");positions[8]=0;
    auto other=chart;other.first_tri=0;atlas.charts.push_back(other);mappings.push_back(map);
    CHECK(!eligible(0,1,1),"coverage-only: multiple candidate charts require complete material");
    atlas.charts.pop_back();mappings.pop_back();
    // Rotate the surface and its chart together. The proof uses the physical
    // chart plane rather than assuming all receivers are XY aligned.
    for(int i=0;i<4;++i) {
        positions[i*3+2]=positions[i*3+1];positions[i*3+1]=0;
        normals[i*3+1]=-1;normals[i*3+2]=0;
    }
    atlas.charts[0].bitangent[1]=0;atlas.charts[0].bitangent[2]=1;
    CHECK(eligible(0,1,1),"coverage-only: rotated planar wall interior qualifies");
}

}  // namespace

int main() {
    {
        VtGpuWorkBudget budget;
        CHECK(budget.rows() == 2, "GPU work budget bounds unpriced page transitions to two rows");
        VtGpuWorkBudget other;
        CHECK(!budget.pair_exceeds_budget(other), "cheap paired slices fit their combined targets");
        budget.observe(40, 4);
        CHECK(budget.rows() == 1, "costly slice immediately reduces admission to one row");
        CHECK(budget.pair_exceeds_budget(other), "expensive minimum-progress work requires separate frames");
        budget.observe(NAN, 8); budget.observe(0, 8); budget.observe(10, 0);
        CHECK(budget.rows() == 1, "invalid/empty GPU samples cannot inflate the budget");
        for (int i=0;i<200;++i) budget.observe(.1f, 1);
        CHECK(budget.rows() == 2 && !budget.pair_exceeds_budget(other), "cheap retired samples recover within the row ceiling");
        budget.set_ms(0);
        CHECK(!budget.enabled() && budget.rows() == 136, "zero time target restores full-page work");
        CHECK(!budget.pair_exceeds_budget(other), "disabled time targets bypass pair pacing");
        budget.set_ms(NAN);
        CHECK(budget.enabled() && budget.rows() <= 2, "invalid settings restore a bounded default");
    }
    {
        std::vector<GpuChart> charts(1); std::vector<GpuTriGeometry> tris(2200);
        charts[0].tri_range[1] = uint32_t(tris.size());
        for (uint32_t i=0;i<tris.size();++i) {
            tris[i].p0[3]=float(i);tris[i].p1[3]=float(i)+.5f;tris[i].p2[3]=float(i)+.2f;
            tris[i].n0[3]=tris[i].n1[3]=1;tris[i].n2[3]=2;
        }
        std::vector<VtResolveChart> table;std::vector<VtResolveNode> nodes;
        CHECK(vt_build_resolve_bvh(charts,tris,table,nodes) && table[0].range[0]==16 &&
              table[0].range[1]==nodes.size(), "resolve BVH: full hierarchy built past the POM seed limit");
        uint32_t next=0;bool valid=true;
        for (uint32_t i=0;i<nodes.size();++i) {
            const auto& n=nodes[i];valid &= n.range[2]>i && n.range[2]<=nodes.size();
            if (!n.range[1]) continue;
            valid &= n.range[0]==next && n.range[1]<=8;
            for (uint32_t t=next;t<next+n.range[1];++t) {
                const auto& tri=tris[t];
                valid &= n.bounds[0]<=tri.p0[3] && n.bounds[2]>=tri.p1[3] && n.bounds[1]<=1 && n.bounds[3]>=2;
            }
            next+=n.range[1];
        }
        CHECK(valid && next==tris.size(), "resolve BVH: conservative leaves cover every triangle in original tie order");
    }
    test_world_receiver_frames();
    test_layout();
    test_indirection();
    test_indirection_dirty();
    test_feedback_residency_equivalence();
    test_feedback_collection();
    test_border_math();
    test_slot_pool();
    test_slot_hysteresis();
    test_table_allocator();
    test_tail_activation_rule();
    test_entry_packing();
    test_registration_cost();
    test_registration_gates();
    test_page_density();
    test_material_page_ownership();
    test_material_reads();
    test_canonical_material_grid();
    test_receiver_coverage_pages();
    benchmark_feedback_collection();
    std::printf("vt residency tests complete\n");
    return check_summary();
}
