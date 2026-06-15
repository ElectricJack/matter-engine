#include "../include/lattice.h"
#include "../include/occupancy.h"
#include "../include/particle_culling.h"
#include <cstdio>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static void test_grid_lattice() {
    GridLattice lat(2.0f);
    Vector3 p = lat.slot_position(SlotCoord{3, 0, -1});
    CHECK(fabsf(p.x - 6.0f) < 1e-6f, "grid slot_position x scales by spacing");
    CHECK(fabsf(p.y - 0.0f) < 1e-6f, "grid slot_position y scales by spacing");
    CHECK(fabsf(p.z + 2.0f) < 1e-6f, "grid slot_position z scales by spacing");

    const auto& n = lat.neighbor_offsets();
    CHECK(n.size() == 6, "grid has 6 face neighbors");
    bool has_plus_x = false;
    for (const auto& o : n) if (o.x == 1 && o.y == 0 && o.z == 0) has_plus_x = true;
    CHECK(has_plus_x, "grid neighbors include +x");
    CHECK(fabsf(lat.spacing() - 2.0f) < 1e-6f, "grid reports its spacing");
}

static void test_occupancy() {
    Occupancy occ;
    CHECK(occ.count() == 0, "empty occupancy has count 0");
    CHECK(!occ.occupied(SlotCoord{0,0,0}), "unset slot not occupied");

    occ.set(SlotCoord{2, -3, 5}, SlotData{7});
    occ.set(SlotCoord{2, -3, 5}, SlotData{42});  // overwrite with different materialId
    CHECK(occ.count() == 1, "re-setting same slot does not grow count");
    CHECK(occ.occupied(SlotCoord{2, -3, 5}), "set slot is occupied");
    CHECK(!occ.occupied(SlotCoord{2, -3, 6}), "neighbor slot not occupied");

    // Verify overwrite actually stored the new materialId (42, not the old 7).
    int overwritten_id = -1;
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        if (c.x == 2 && c.y == -3 && c.z == 5) overwritten_id = d.materialId;
    });
    CHECK(overwritten_id == 42, "overwrite stores new materialId, not old one");

    // pack/unpack round-trips negatives via for_each.
    bool seen = false;
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        if (c.x == 2 && c.y == -3 && c.z == 5 && d.materialId == 42) seen = true;
    });
    CHECK(seen, "for_each round-trips coords (incl. negatives) and data");
}

// Fill an N x N x N solid block of one material at the origin.
static Occupancy solid_block(int N) {
    Occupancy occ;
    for (int x = 0; x < N; ++x)
    for (int y = 0; y < N; ++y)
    for (int z = 0; z < N; ++z)
        occ.set(SlotCoord{x, y, z}, SlotData{8});
    return occ;
}

static CullParams default_params(int margin) {
    CullParams p;
    p.margin = margin; p.base_radius = 0.4f;
    p.jitter_amount = 0.1f; p.tint_alpha = 0.2f; p.seed = 1337;
    p.cell_size = 1.6f; p.cell_origin_offset = Vector3{0,0,0};
    p.spacing = 0.8f;   // matches the GridLattice(0.8f) used throughout
    p.max_tier = 0;     // refinement off unless a test opts in
    return p;
}

static void test_burial() {
    Occupancy occ = solid_block(5);
    CHECK(slot_is_buried(occ, SlotCoord{2,2,2}, 1), "center of 5^3 is buried at margin 1");
    CHECK(!slot_is_buried(occ, SlotCoord{0,2,2}, 1), "face slot not buried");
    CHECK(slot_is_buried(occ, SlotCoord{2,2,2}, 2), "center of 5^3 is buried at margin 2");
}

static void test_slot_depth() {
    Occupancy occ = solid_block(5);   // coords 0..4 each axis
    // Center (2,2,2): radius-1 box fully occupied, radius-2 box hits the
    // boundary at coord 0/4 which is still occupied, radius-3 would leave block.
    CHECK(slot_depth(occ, SlotCoord{2,2,2}, 3) == 2, "center of 5^3 has depth 2 (capped scan 3)");
    CHECK(slot_depth(occ, SlotCoord{0,2,2}, 3) == 0, "face slot has depth 0");
    CHECK(slot_depth(occ, SlotCoord{1,2,2}, 3) == 1, "one-layer-in slot has depth 1");
    // Cap: never report more than max_depth even if deeper matter exists.
    CHECK(slot_depth(occ, SlotCoord{2,2,2}, 1) == 1, "depth is capped at max_depth");
    Occupancy single; single.set(SlotCoord{0,0,0}, SlotData{8});
    CHECK(slot_depth(single, SlotCoord{0,0,0}, 3) == 0, "isolated slot has depth 0");
}

static void test_slot_tier() {
    // tier = max_tier - min(depth, max_tier): outermost shell is finest.
    CHECK(slot_tier(0, 2) == 2, "depth 0 -> finest tier (max_tier)");
    CHECK(slot_tier(1, 2) == 1, "depth 1 -> one coarser");
    CHECK(slot_tier(2, 2) == 0, "depth == max_tier -> tier 0");
    CHECK(slot_tier(5, 2) == 0, "deep interior clamps to tier 0");
    CHECK(slot_tier(0, 0) == 0, "max_tier 0 disables refinement");
}

static void test_cull_counts() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(10);

    // spacing 0.8, cell_size 1.6 -> 2 slots/cell; cell k covers slots {2k,2k+1}.
    // margin 1 buries slots with every coord in [1,8]. A cell is interior iff all
    // its slots are buried -> interior cells = k in {1,2,3}^3 = 27. Only (2,2,2)
    // has all 26 neighbors interior -> 1 core cell, dropping its 8 slots.
    CullStats stats;
    std::vector<SlotCoord> no_mesh;
    auto m1 = cull_interior(lat, occ, default_params(1), &stats, &no_mesh);
    CHECK(m1.size() == 992, "margin 1 drops only the single core cell's 8 slots");
    CHECK(no_mesh.size() == 27, "27 interior cells reported as no-mesh");
    CHECK(stats.cells_total == 125, "125 occupied cells total");
    CHECK(stats.cells_skipped == 27, "27 cells skip-meshed (interior)");
    CHECK(stats.cells_core == 1, "1 core cell");
    CHECK(stats.cells_meshed == 98, "98 cells meshed (125 - 27 interior)");

    auto all = emit_all(lat, occ, default_params(1));
    CHECK(all.size() == 1000, "emit_all keeps all 1000 slots");

    // margin 0 clamps to 1 -> same result.
    CullStats s0;
    auto m0 = cull_interior(lat, occ, default_params(0), &s0, nullptr);
    CHECK(m0.size() == 992, "margin 0 clamped to 1");
}

// Same key formula as test_cell_key, but from an integer cell coord directly
// (the cull reports no_mesh cells as integer coords with floor already applied).
static long long cell_coord_key(int cx, int cy, int cz) {
    return (cx + 1000) * 4000000LL + (cy + 1000) * 2000LL + (cz + 1000);
}

// A cell-key helper mirroring cull_interior's bucketing (offset 0 in tests).
static long long test_cell_key(const Vector3& pos, float cs) {
    long long cx = (long long)floorf(pos.x / cs);
    long long cy = (long long)floorf(pos.y / cs);
    long long cz = (long long)floorf(pos.z / cs);
    return (cx + 1000) * 4000000LL + (cy + 1000) * 2000LL + (cz + 1000);
}

static void test_no_meshed_cell_borders_dropped() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(10);
    CullParams p = default_params(1);
    p.jitter_amount = 0.0f;   // emitted position == slot_position, exact bucketing

    std::vector<SlotCoord> no_mesh;
    auto kept = cull_interior(lat, occ, p, nullptr, &no_mesh);

    auto cell_of = [&](const Vector3& pos) {
        return std::make_tuple((int)floorf(pos.x / p.cell_size),
                               (int)floorf(pos.y / p.cell_size),
                               (int)floorf(pos.z / p.cell_size));
    };

    std::set<std::tuple<int,int,int>> occupied;
    occ.for_each([&](SlotCoord c, const SlotData&) {
        occupied.insert(cell_of(lat.slot_position(c)));
    });
    std::set<std::tuple<int,int,int>> kept_cells;   // still bear particles
    for (const auto& ep : kept) kept_cells.insert(cell_of(ep.position));
    std::set<std::tuple<int,int,int>> interior;     // skip-meshed
    for (const SlotCoord& c : no_mesh) interior.insert(std::make_tuple(c.x, c.y, c.z));

    // dropped = occupied with no particles; meshed = occupied and not interior.
    // Invariant: no dropped cell is a 26-neighbor of any meshed cell.
    bool invariant = true;
    for (const auto& cell : occupied) {
        if (kept_cells.find(cell) != kept_cells.end()) continue;  // not dropped
        int cx, cy, cz; std::tie(cx, cy, cz) = cell;
        for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy && !dz) continue;
            auto nb = std::make_tuple(cx + dx, cy + dy, cz + dz);
            bool meshed = occupied.count(nb) && !interior.count(nb);
            if (meshed) invariant = false;
        }
    }
    CHECK(invariant, "no dropped cell borders a meshed cell (no inner surface)");
}

static void test_skip_set_is_interior() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(10);
    CullParams p = default_params(1);

    std::vector<SlotCoord> no_mesh;
    cull_interior(lat, occ, p, nullptr, &no_mesh);

    // Reference: a cell is interior iff every slot in it is buried.
    std::map<long long, bool> all_buried;
    occ.for_each([&](SlotCoord c, const SlotData&) {
        long long k = test_cell_key(lat.slot_position(c), p.cell_size);
        bool b = slot_is_buried(occ, c, 1);
        auto it = all_buried.find(k);
        if (it == all_buried.end()) all_buried.emplace(k, b);
        else it->second = it->second && b;
    });

    std::set<long long> nm;
    for (const SlotCoord& c : no_mesh) nm.insert(cell_coord_key(c.x, c.y, c.z));

    bool ok = true;
    for (long long k : nm) if (!all_buried[k]) ok = false;
    CHECK(ok, "every no-mesh cell is interior (all slots buried)");

    size_t interior_count = 0;
    for (const auto& kv : all_buried) if (kv.second) ++interior_count;
    CHECK(nm.size() == interior_count, "no-mesh set equals exactly the interior cells");
}

static void test_thin_shape_keeps_all() {
    Occupancy occ;
    for (int x = 0; x < 5; ++x)
    for (int y = 0; y < 5; ++y)
        occ.set(SlotCoord{x, y, 0}, SlotData{8});
    GridLattice lat(0.8f);
    auto kept = cull_interior(lat, occ, default_params(2));
    CHECK(kept.size() == 25, "one-slot-thick wall keeps all slots at any margin");
}

static void test_determinism() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(5);

    auto a = cull_interior(lat, occ, default_params(1));
    auto b = cull_interior(lat, occ, default_params(1));
    CHECK(a.size() == b.size(), "two identical culls produce equal counts");
    bool identical = (a.size() == b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (a[i].position.x != b[i].position.x ||
            a[i].position.y != b[i].position.y ||
            a[i].position.z != b[i].position.z ||
            a[i].tint.x != b[i].tint.x) identical = false;
    }
    CHECK(identical, "two identical culls produce bit-identical particles");

    CullParams p2 = default_params(1); p2.seed = 9999;
    auto c = cull_interior(lat, occ, p2);
    CHECK(c.size() == a.size(), "different seed keeps the same slots");
    bool any_moved = false;
    for (size_t i = 0; i < a.size() && i < c.size(); ++i)
        if (a[i].position.x != c[i].position.x) any_moved = true;
    CHECK(any_moved, "different seed changes jittered positions");
}

static void test_tier0_regression_and_detail() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(5);

    // max_tier 0: every emitted particle carries detail_size == spacing (tier 0).
    auto base = cull_interior(lat, occ, default_params(1));
    bool all_tier0_detail = true;
    for (const auto& ep : base)
        if (fabsf(ep.detail_size - 0.8f) > 1e-6f) all_tier0_detail = false;
    CHECK(all_tier0_detail, "tier-0 emit sets detail_size == spacing");
    CHECK(!base.empty(), "tier-0 emit is non-empty");
}

static void test_tiered_emission() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(6);   // coords 0..5

    CullParams p = default_params(1);
    p.max_tier = 1;

    // With max_tier 1, every outermost-shell slot (depth 0) emits 2^3 = 8
    // sub-particles; deeper slots stay at tier 0 (1 particle). So the total
    // exceeds emit_all's one-per-slot count, and is strictly larger than tier 0.
    auto tier0 = cull_interior(lat, occ, default_params(1));
    auto tier1 = cull_interior(lat, occ, p);
    CHECK(tier1.size() > tier0.size(), "max_tier 1 emits more particles than tier 0");

    // detail_size appears at two scales only: S (tier 0) and S/2 (tier 1).
    int n_full = 0, n_half = 0, n_other = 0;
    for (const auto& ep : tier1) {
        if (fabsf(ep.detail_size - 0.8f) < 1e-5f)      ++n_full;
        else if (fabsf(ep.detail_size - 0.4f) < 1e-5f) ++n_half;
        else ++n_other;
    }
    CHECK(n_half > 0, "tier-1 sub-particles carry detail_size == S/2");
    CHECK(n_full > 0, "interior keeps tier-0 detail_size == S");
    CHECK(n_other == 0, "no unexpected detail_size values");

    // Sub-particle count for a single refined slot is exactly 8.
    Occupancy one; one.set(SlotCoord{0,0,0}, SlotData{8});
    CullParams q = default_params(1); q.max_tier = 2;   // isolated slot -> depth 0 -> tier 2
    auto refined = emit_all(lat, one, q);
    CHECK(refined.size() == 64, "isolated slot at tier 2 emits 4^3 = 64 sub-particles");
}

static void test_core_dropped_with_tiers() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(10);
    CullParams p = default_params(1);
    p.max_tier = 1;
    p.jitter_amount = 0.0f;   // emitted position == slot position, exact bucketing

    CullStats stats;
    std::vector<SlotCoord> no_mesh;
    auto kept = cull_interior(lat, occ, p, &stats, &no_mesh);

    // The single core cell's 8 slots are still dropped (they are fully buried,
    // depth >= max_tier so tier 0, but core => not emitted at all).
    CHECK(stats.cells_core == 1, "core cell count unchanged with tiers on");
    // No emitted particle may sit inside the core cell (cell index (2,2,2)).
    bool any_in_core = false;
    for (const auto& ep : kept) {
        int cx = (int)floorf(ep.position.x / p.cell_size);
        int cy = (int)floorf(ep.position.y / p.cell_size);
        int cz = (int)floorf(ep.position.z / p.cell_size);
        if (cx == 2 && cy == 2 && cz == 2) any_in_core = true;
    }
    CHECK(!any_in_core, "core cell emits zero particles even with tiers on");
}

int main() {
    test_grid_lattice();
    test_occupancy();
    test_burial();
    test_slot_depth();
    test_slot_tier();
    test_cull_counts();
    test_no_meshed_cell_borders_dropped();
    test_skip_set_is_interior();
    test_thin_shape_keeps_all();
    test_determinism();
    test_tier0_regression_and_detail();
    test_tiered_emission();
    test_core_dropped_with_tiers();
    if (failures == 0) printf("All particle_culling tests passed\n");
    return failures == 0 ? 0 : 1;
}
