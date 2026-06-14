#include "../include/lattice.h"
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

int main() {
    test_grid_lattice();
    if (failures == 0) printf("All particle_culling tests passed\n");
    return failures == 0 ? 0 : 1;
}
