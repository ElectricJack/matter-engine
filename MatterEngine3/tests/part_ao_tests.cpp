// part_ao_tests.cpp — headless unit tests for the deterministic AO baker.
// Convention: CHECK macro from check.h, g_failures counter, returns non-zero on
// any failure. No GL/raylib dependencies; BVH/precomp types only.
#include "part_ao_bake.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>

#include "check.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Tri make_tri(float3 a, float3 b, float3 c) {
    Tri t;
    t.vertex0 = a; t.vertex1 = b; t.vertex2 = c;
    t.centroid = make_float3((a.x+b.x+c.x)/3.0f,
                             (a.y+b.y+c.y)/3.0f,
                             (a.z+b.z+c.z)/3.0f);
    return t;
}

static TriEx make_triex_up() {
    TriEx ex{};   // value-init: zero all members including padding
    ex.ao0 = ex.ao1 = ex.ao2 = 1.0f;
    ex.N0 = ex.N1 = ex.N2 = make_float3(0, 1, 0);
    return ex;
}

// Returns two Tris forming a horizontal square centred at (cx,cz,y), half-width
// `half`, normals pointing up (+Y). Initial ao0/1/2 = 1.0.
static std::pair<std::vector<Tri>, std::vector<TriEx>>
quad(float cx, float cz, float y, float half) {
    float3 a = make_float3(cx - half, y, cz - half);
    float3 b = make_float3(cx + half, y, cz - half);
    float3 c = make_float3(cx + half, y, cz + half);
    float3 d = make_float3(cx - half, y, cz + half);
    std::vector<Tri> tris = { make_tri(a, b, c), make_tri(a, c, d) };
    std::vector<TriEx> triex = { make_triex_up(), make_triex_up() };
    return { tris, triex };
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

static void test_open_plate_unoccluded() {
    auto [tris, triex] = quad(0, 0, /*y=*/0, /*half=*/1.0f);
    part_ao::bake_part_ao({&tris}, {&triex}, {});
    for (const TriEx& e : triex) {
        CHECK(e.ao0 > 0.95f && e.ao1 > 0.95f && e.ao2 > 0.95f,
              "open plate stays ~unoccluded");
    }
}

static void test_overhang_darkens() {
    // Lid (half=3) is larger than the floor (half=1) so every floor corner vertex
    // lies well inside the lid's footprint; ~100% of upward hemisphere rays hit
    // the lid at y=0.2, yielding heavy occlusion (ao << 0.5).
    auto [floor_t, floor_e] = quad(0, 0, 0, 1.0f);
    auto [lid_t, lid_e]     = quad(0, 0, 0.2f, 3.0f);   // lid 0.2 above floor
    part_ao::bake_part_ao({&floor_t, &lid_t}, {&floor_e, &lid_e}, {});
    CHECK(floor_e[0].ao0 < 0.5f, "floor under a close lid darkens");
}

static void test_determinism() {
    auto [t1, e1] = quad(0, 0, 0, 1.0f); auto [l1, le1] = quad(0, 0, 0.2f, 1.0f);
    auto [t2, e2] = quad(0, 0, 0, 1.0f); auto [l2, le2] = quad(0, 0, 0.2f, 1.0f);
    part_ao::bake_part_ao({&t1, &l1}, {&e1, &le1}, {});
    part_ao::bake_part_ao({&t2, &l2}, {&e2, &le2}, {});
    CHECK(std::memcmp(e1.data(), e2.data(), e1.size() * sizeof(TriEx)) == 0 &&
          std::memcmp(le1.data(), le2.data(), le1.size() * sizeof(TriEx)) == 0,
          "AO bake is byte-deterministic");
}

static void test_quality_zero_disables() {
    auto [t, e] = quad(0, 0, 0, 1.0f); auto [l, le] = quad(0, 0, 0.2f, 1.0f);
    part_ao::AoBakeParams p; p.quality = 0.0f;
    part_ao::bake_part_ao({&t, &l}, {&e, &le}, p);
    CHECK(e[0].ao0 == 1.0f && le[0].ao0 == 1.0f, "quality 0 leaves ao untouched");
}

static void test_adaptive_budget_scales_down() {
    auto [t, e] = quad(0, 0, 0, 1.0f);
    part_ao::AoBakeParams p; p.max_total_rays = 8;   // 6 verts -> ~1 ray each
    part_ao::AoBakeStats s{};
    part_ao::bake_part_ao({&t}, {&e}, p, &s);
    CHECK(s.rays_per_vertex >= 4, "rays/vertex never below floor of 4");
    part_ao::AoBakeParams q; q.quality = 4.0f;        // clamps at 128
    part_ao::bake_part_ao({&t}, {&e}, q, &s);
    CHECK(s.rays_per_vertex == 128, "quality clamps at 128 rays/vertex");
}

static void test_budget_counts_unique_positions_not_corners() {
    // 8x8 grid of quads on a flat plane, all normals +Y, adjacent quads sharing
    // bitwise-identical corner coordinates: 128 tris = 384 raw corners but only
    // 81 unique (position,normal) keys. Budget must be charged against the 81
    // welded vertices actually raycast, not the 384 raw corners — otherwise a
    // large welded part over-halves its rays/vertex into blotchy noise.
    std::vector<Tri> tris;
    std::vector<TriEx> triex;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) {
            float3 a = make_float3(i * 0.5f,       0.0f, j * 0.5f);
            float3 b = make_float3((i + 1) * 0.5f, 0.0f, j * 0.5f);
            float3 c = make_float3((i + 1) * 0.5f, 0.0f, (j + 1) * 0.5f);
            float3 d = make_float3(i * 0.5f,       0.0f, (j + 1) * 0.5f);
            tris.push_back(make_tri(a, b, c)); triex.push_back(make_triex_up());
            tris.push_back(make_tri(a, c, d)); triex.push_back(make_triex_up());
        }

    part_ao::AoBakeParams p;
    p.max_total_rays = 3000;   // 384*32 > 3000 (raw would halve to 4);
                               //  81*32 <= 3000 (unique keeps all 32)
    part_ao::AoBakeStats s{};
    part_ao::bake_part_ao({&tris}, {&triex}, p, &s);
    CHECK(s.unique_positions == 81, "welded plane has 81 unique keys");
    CHECK(s.rays_per_vertex == 32, "budget charged on unique positions");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    test_open_plate_unoccluded();
    test_overhang_darkens();
    test_determinism();
    test_quality_zero_disables();
    test_adaptive_budget_scales_down();
    test_budget_counts_unique_positions_not_corners();

    if (g_failures == 0) {
        printf("ALL PASS (%d tests)\n", 6);
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
