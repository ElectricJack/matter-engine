#include "../include/lattice.h"
#include "../include/occupancy.h"
#include "../include/particle_culling.h"
#include <cstdio>
#include <cmath>

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
    return p;
}

static void test_burial() {
    Occupancy occ = solid_block(5);
    CHECK(slot_is_buried(occ, SlotCoord{2,2,2}, 1), "center of 5^3 is buried at margin 1");
    CHECK(!slot_is_buried(occ, SlotCoord{0,2,2}, 1), "face slot not buried");
    CHECK(slot_is_buried(occ, SlotCoord{2,2,2}, 2), "center of 5^3 is buried at margin 2");
}

static void test_cull_counts() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(5);

    auto m1 = cull_interior(lat, occ, default_params(1));
    CHECK(m1.size() == 98, "margin 1 keeps 98 of 125 (drops inner 3^3)");

    auto m2 = cull_interior(lat, occ, default_params(2));
    CHECK(m2.size() == 124, "margin 2 keeps 124 of 125 (drops inner 1^3)");

    auto all = emit_all(lat, occ, default_params(1));
    CHECK(all.size() == 125, "emit_all keeps all 125");

    auto m0 = cull_interior(lat, occ, default_params(0));
    CHECK(m0.size() == 98, "margin 0 clamped to 1");
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

int main() {
    test_grid_lattice();
    test_occupancy();
    test_burial();
    test_cull_counts();
    test_thin_shape_keeps_all();
    test_determinism();
    if (failures == 0) printf("All particle_culling tests passed\n");
    return failures == 0 ? 0 : 1;
}
