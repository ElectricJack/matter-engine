// Headless unit tests for bake_vertex_ao (pure, no GL context needed).
#include <cstdio>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "vertex_ao.h"
#include "occupancy.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; } \
                              else { printf("ok: %s\n", msg); } } while (0)

// A single triangle whose 3 vertices all sit at position `p` with normal `n`
// (degenerate geometrically, but bake_vertex_ao treats vertices independently, so
// each gets the same AO -- convenient for asserting a single vertex's value).
static void make_point_tri(std::vector<Tri>& tris, std::vector<TriEx>& ex,
                           float3 p, float3 n) {
    Tri t; std::memset(&t, 0, sizeof(Tri));
    t.vertex0 = p; t.vertex1 = p; t.vertex2 = p;
    tris.push_back(t);
    TriEx e{}; e.N0 = n; e.N1 = n; e.N2 = n;
    ex.push_back(e);
}

static Occupancy solid_block(int N) {
    Occupancy occ;
    for (int z = 0; z < N; ++z)
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
                occ.set(SlotCoord{x, y, z}, SlotData{8});
    return occ;
}

static void test_exposed_vertex_is_bright() {
    Occupancy occ; // empty
    std::vector<Tri> tris; std::vector<TriEx> ex;
    make_point_tri(tris, ex, make_float3(0.5f, 0.5f, 0.5f), make_float3(0,1,0));
    bake_vertex_ao(tris, ex, occ, AoGrid{1.0f, make_float3(0,0,0)}, AoParams{2.0f, 1.0f});
    CHECK(ex[0].ao0 > 0.99f, "exposed vertex AO ~ 1.0");
}

static void test_buried_vertex_is_dark() {
    Occupancy occ = solid_block(7);
    std::vector<Tri> tris; std::vector<TriEx> ex;
    make_point_tri(tris, ex, make_float3(3.0f, 3.0f, 3.0f), make_float3(0,1,0));
    bake_vertex_ao(tris, ex, occ, AoGrid{1.0f, make_float3(0,0,0)}, AoParams{2.0f, 1.0f});
    CHECK(ex[0].ao0 < 0.5f, "buried vertex AO < 0.5");
}

static void test_monotonic_with_added_occluder() {
    Occupancy a; // empty
    Occupancy b; b.set(SlotCoord{0, 1, 0}, SlotData{8}); // one occluder above origin
    std::vector<Tri> ta; std::vector<TriEx> ea;
    std::vector<Tri> tb; std::vector<TriEx> eb;
    make_point_tri(ta, ea, make_float3(0,0,0), make_float3(0,1,0));
    make_point_tri(tb, eb, make_float3(0,0,0), make_float3(0,1,0));
    AoGrid g{1.0f, make_float3(0,0,0)}; AoParams p{2.0f, 1.0f};
    bake_vertex_ao(ta, ea, a, g, p);
    bake_vertex_ao(tb, eb, b, g, p);
    CHECK(eb[0].ao0 < ea[0].ao0, "adding an occluder lowers AO");
}

static void test_behind_surface_does_not_occlude() {
    Occupancy occ; occ.set(SlotCoord{0, -1, 0}, SlotData{8}); // directly below
    std::vector<Tri> tris; std::vector<TriEx> ex;
    make_point_tri(tris, ex, make_float3(0,0,0), make_float3(0,1,0));
    bake_vertex_ao(tris, ex, occ, AoGrid{1.0f, make_float3(0,0,0)}, AoParams{2.0f, 1.0f});
    CHECK(ex[0].ao0 > 0.99f, "occluder behind surface does not darken");
}

static void test_pack_roundtrip() {
    float vals[3] = {0.0f, 0.5f, 1.0f};
    float packed = pack_ao_w(vals[0], vals[1], vals[2]);
    uint32_t bits; std::memcpy(&bits, &packed, sizeof(bits));
    float u0 = float(bits & 0xFFu) / 255.0f;
    float u1 = float((bits >> 8) & 0xFFu) / 255.0f;
    float u2 = float((bits >> 16) & 0xFFu) / 255.0f;
    CHECK(std::fabs(u0 - vals[0]) <= 1.0f/255.0f, "pack roundtrip ch0");
    CHECK(std::fabs(u1 - vals[1]) <= 1.0f/255.0f, "pack roundtrip ch1");
    CHECK(std::fabs(u2 - vals[2]) <= 1.0f/255.0f, "pack roundtrip ch2");
}

int main() {
    test_exposed_vertex_is_bright();
    test_buried_vertex_is_dark();
    test_monotonic_with_added_occluder();
    test_behind_surface_does_not_occlude();
    test_pack_roundtrip();
    if (g_failures) { printf("\n%d FAILURE(S)\n", g_failures); return 1; }
    printf("\n=== All vertex_ao tests PASSED ===\n");
    return 0;
}
