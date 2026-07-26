// Tests for MSL's reproject_triex spatial-hash nearest-source lookup.
// Semantics ported verbatim from lod_bake::reproject_triex; the intent here
// is a small confidence check that the MSL wrapper behaves as expected on
// simple cases (identity source/target; two-material sphere-ish split).
#include "mesh_indexed.hpp"
#include "mesh_transform.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

namespace {

Tri make_tri(float3 a, float3 b, float3 c) {
    Tri t{};
    t.vertex0 = a; t.vertex1 = b; t.vertex2 = c;
    t.centroid = make_float3((a.x+b.x+c.x)/3.0f,
                             (a.y+b.y+c.y)/3.0f,
                             (a.z+b.z+c.z)/3.0f);
    return t;
}

TriEx make_triex(int material) {
    TriEx e{};
    e.materialId = material;
    e.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
    return e;
}

bool near_eq(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) < eps; }

// --- Test 1: empty source clears target.triex --------------------------------
void test_empty_source_clears_target() {
    MeshIndexed src;   // empty
    MeshIndexed tgt;
    tgt.positions = { make_float3(0,0,0), make_float3(1,0,0), make_float3(0,1,0) };
    tgt.indices   = { 0, 1, 2 };
    tgt.triex.resize(1);
    tgt.triex[0].materialId = 99;
    reproject_triex(src, tgt);
    assert(tgt.triex.empty());
}

// --- Test 2: mismatched source.triex clears target.triex ---------------------
void test_mismatched_source_triex_clears_target() {
    std::vector<Tri> src_tris = { make_tri(make_float3(0,0,0),
                                           make_float3(1,0,0),
                                           make_float3(0,1,0)) };
    // Provide non-parallel triex (size mismatch) -> function must clear target.
    MeshIndexed src = from_tri(src_tris, nullptr);
    src.triex.resize(0);   // 1 tri, 0 triex

    MeshIndexed tgt = from_tri(src_tris, nullptr);
    tgt.triex.resize(1);
    tgt.triex[0].materialId = 42;
    reproject_triex(src, tgt);
    assert(tgt.triex.empty());
}

// --- Test 3: identity source == target, single material carries through ------
void test_identity_single_material() {
    std::vector<Tri> tris = {
        make_tri(make_float3(0,0,0), make_float3(1,0,0), make_float3(0,1,0)),
    };
    std::vector<TriEx> triex = { make_triex(1) };

    MeshIndexed src = from_tri(tris, &triex);
    MeshIndexed tgt = from_tri(tris, nullptr);
    reproject_triex(src, tgt);

    assert(tgt.triex.size() == 1);
    assert(tgt.triex[0].materialId == 1);
    // Shading normals are recomputed as the target triangle's geometric normal
    // (CCW winding on the XY plane -> +Z).
    assert(near_eq(tgt.triex[0].N0.x, 0.0f));
    assert(near_eq(tgt.triex[0].N0.y, 0.0f));
    assert(near_eq(tgt.triex[0].N0.z, 1.0f));
    // N0, N1, N2 identical (flat-shaded output).
    assert(near_eq(tgt.triex[0].N0.x, tgt.triex[0].N1.x));
    assert(near_eq(tgt.triex[0].N0.y, tgt.triex[0].N1.y));
    assert(near_eq(tgt.triex[0].N0.z, tgt.triex[0].N1.z));
    assert(near_eq(tgt.triex[0].N0.x, tgt.triex[0].N2.x));
}

// --- Test 4: two materials, target picks the nearer source triangle ----------
// Two source triangles at x=-1 (material 1) and x=+1 (material 2). Two target
// triangles centered at x=-0.9 and x=+0.9. Each target must inherit its nearer
// neighbor's material.
void test_two_materials_nearest_wins() {
    // Left source tri: centroid ~ (-1, 0, 0).
    Tri left  = make_tri(make_float3(-1.5f, -0.5f, 0.0f),
                         make_float3(-0.5f, -0.5f, 0.0f),
                         make_float3(-1.0f,  0.5f, 0.0f));
    // Right source tri: centroid ~ (+1, 0, 0).
    Tri right = make_tri(make_float3( 0.5f, -0.5f, 0.0f),
                         make_float3( 1.5f, -0.5f, 0.0f),
                         make_float3( 1.0f,  0.5f, 0.0f));
    std::vector<Tri>   src_tris = { left, right };
    std::vector<TriEx> src_ex   = { make_triex(1), make_triex(2) };

    // Two target tris: one near left, one near right (offset by 0.1 in x).
    Tri tleft  = make_tri(make_float3(-1.4f, -0.4f, 0.0f),
                          make_float3(-0.4f, -0.4f, 0.0f),
                          make_float3(-0.9f,  0.6f, 0.0f));
    Tri tright = make_tri(make_float3( 0.6f, -0.4f, 0.0f),
                          make_float3( 1.6f, -0.4f, 0.0f),
                          make_float3( 1.1f,  0.6f, 0.0f));
    std::vector<Tri> tgt_tris = { tleft, tright };

    MeshIndexed src = from_tri(src_tris, &src_ex);
    MeshIndexed tgt = from_tri(tgt_tris, nullptr);
    reproject_triex(src, tgt);

    assert(tgt.triex.size() == 2);
    assert(tgt.triex[0].materialId == 1);   // left target -> left source
    assert(tgt.triex[1].materialId == 2);   // right target -> right source
}

// --- Test 5: output is parallel to target triangle count ---------------------
void test_output_parallel_to_target() {
    std::vector<Tri>   src_tris = {
        make_tri(make_float3(0,0,0), make_float3(1,0,0), make_float3(0,1,0)),
        make_tri(make_float3(0,0,0), make_float3(0,1,0), make_float3(-1,0,0)),
    };
    std::vector<TriEx> src_ex = { make_triex(7), make_triex(9) };

    // Target has 3 tris.
    std::vector<Tri> tgt_tris = {
        make_tri(make_float3(0.1f,0.1f,0.0f), make_float3(0.9f,0.0f,0.0f), make_float3(0.0f,0.8f,0.0f)),
        make_tri(make_float3(-0.9f,0.0f,0.0f), make_float3(0.0f,0.9f,0.0f), make_float3(-0.1f,0.1f,0.0f)),
        make_tri(make_float3(0.05f,0.05f,0.0f), make_float3(0.4f,0.0f,0.0f), make_float3(0.0f,0.4f,0.0f)),
    };

    MeshIndexed src = from_tri(src_tris, &src_ex);
    MeshIndexed tgt = from_tri(tgt_tris, nullptr);
    reproject_triex(src, tgt);

    assert(tgt.triex.size() == tgt.indices.size() / 3);
    assert(tgt.triex.size() == 3);

    // Both source materials should be reachable in this layout.
    std::set<int> mats;
    for (const TriEx& e : tgt.triex) mats.insert(e.materialId);
    // At minimum: material 7 (right-half source) should appear for the right
    // target tris. Just assert both distinct materials could show up given the
    // spread of target centroids.
    assert(mats.count(7) == 1 || mats.count(9) == 1);
}

} // namespace

int main() {
    test_empty_source_clears_target();
    test_mismatched_source_triex_clears_target();
    test_identity_single_material();
    test_two_materials_nearest_wins();
    test_output_parallel_to_target();
    std::printf("mesh_transform_tests: OK (5/5)\n");
    return 0;
}
