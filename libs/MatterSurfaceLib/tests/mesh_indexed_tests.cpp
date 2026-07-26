// Tests for MeshIndexed's from_tri/to_tri round-trip.
#include "mesh_indexed.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

Tri make_tri(float3 a, float3 b, float3 c) {
    Tri t{};
    t.vertex0 = a; t.vertex1 = b; t.vertex2 = c;
    t.centroid = make_float3((a.x+b.x+c.x)/3.0f, (a.y+b.y+c.y)/3.0f, (a.z+b.z+c.z)/3.0f);
    return t;
}

bool near_eq(float a, float b, float eps = 1e-6f) { return std::fabs(a - b) < eps; }
bool near_eq(float3 a, float3 b, float eps = 1e-6f) {
    return near_eq(a.x, b.x, eps) && near_eq(a.y, b.y, eps) && near_eq(a.z, b.z, eps);
}

void test_empty_input_empty_output() {
    std::vector<Tri>   tris;
    MeshIndexed m = from_tri(tris, nullptr);
    assert(m.positions.empty());
    assert(m.indices.empty());
    assert(m.triex.empty());

    std::vector<Tri> back_tris;
    std::vector<TriEx> back_triex;
    to_tri(m, back_tris, back_triex);
    assert(back_tris.empty());
    assert(back_triex.empty());
}

void test_two_triangles_share_edge_weld_reduces_verts() {
    // Two tris sharing an edge; 6 corner slots, 4 unique positions.
    float3 A = make_float3(0,0,0), B = make_float3(1,0,0),
           C = make_float3(1,1,0), D = make_float3(0,1,0);
    std::vector<Tri> tris = { make_tri(A,B,C), make_tri(A,C,D) };
    MeshIndexed m = from_tri(tris, nullptr);
    assert(m.positions.size() == 4);
    assert(m.indices.size() == 6);
    assert(m.triex.empty());
}

void test_round_trip_preserves_triangles() {
    float3 A = make_float3(0,0,0), B = make_float3(1,0,0),
           C = make_float3(1,1,0), D = make_float3(0,1,0);
    std::vector<Tri> tris = { make_tri(A,B,C), make_tri(A,C,D) };
    MeshIndexed m = from_tri(tris, nullptr);

    std::vector<Tri>   out_tris;
    std::vector<TriEx> out_triex;
    to_tri(m, out_tris, out_triex);

    assert(out_tris.size() == 2);
    assert(out_triex.empty());
    // Order preserved: triangle 0 corners are A,B,C.
    assert(near_eq(out_tris[0].vertex0, A));
    assert(near_eq(out_tris[0].vertex1, B));
    assert(near_eq(out_tris[0].vertex2, C));
    assert(near_eq(out_tris[1].vertex0, A));
    assert(near_eq(out_tris[1].vertex1, C));
    assert(near_eq(out_tris[1].vertex2, D));
}

void test_triex_parallel_preserved() {
    float3 A = make_float3(0,0,0), B = make_float3(1,0,0),
           C = make_float3(1,1,0), D = make_float3(0,1,0);
    std::vector<Tri>   tris  = { make_tri(A,B,C), make_tri(A,C,D) };
    std::vector<TriEx> triex(2);
    triex[0].materialId = 7;
    triex[1].materialId = 11;

    MeshIndexed m = from_tri(tris, &triex);
    assert(m.triex.size() == 2);
    assert(m.triex[0].materialId == 7);
    assert(m.triex[1].materialId == 11);

    std::vector<Tri>   out_tris;
    std::vector<TriEx> out_triex;
    to_tri(m, out_tris, out_triex);
    assert(out_triex.size() == 2);
    assert(out_triex[0].materialId == 7);
    assert(out_triex[1].materialId == 11);
}

void test_weld_tolerance() {
    // Two verts within epsilon collapse; two verts outside stay separate.
    float3 A  = make_float3(0.0f, 0.0f, 0.0f);
    float3 A2 = make_float3(0.3e-4f, 0.0f, 0.0f);   // within default 1e-4 (0.3*eps < half-cell)
    float3 B  = make_float3(1.0f, 0.0f, 0.0f);
    float3 B2 = make_float3(1.0f + 2e-4f, 0.0f, 0.0f); // outside default 1e-4

    std::vector<Tri> tris = {
        make_tri(A,  B,  make_float3(0,1,0)),
        make_tri(A2, B2, make_float3(0,2,0)),
    };
    MeshIndexed m = from_tri(tris, nullptr);
    // A/A2 collapse to one; B and B2 stay separate; two apexes are distinct.
    // Unique positions: {A∪A2, B, B2, apex1, apex2} = 5.
    // (Brief said 4, but two distinct apexes (0,1,0) and (0,2,0) count separately.)
    assert(m.positions.size() == 5);
}

} // namespace

int run_mesh_indexed_tests() {
    test_empty_input_empty_output();
    test_two_triangles_share_edge_weld_reduces_verts();
    test_round_trip_preserves_triangles();
    test_triex_parallel_preserved();
    test_weld_tolerance();
    std::printf("mesh_indexed_tests: OK (5/5)\n");
    return 0;
}

int main() {
    return run_mesh_indexed_tests();
}
