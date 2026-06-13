#include <cstdio>
#include <cassert>
#include <cmath>
#include <vector>
#include <array>

#include "raylib.h"
#include "mesh_simplifier.hpp"

// Build an indexed Mesh from raw vertex + index arrays (CPU-only, no GL upload).
static Mesh makeMesh(const std::vector<float>& v, const std::vector<unsigned short>& idx) {
    Mesh m = {0};
    m.vertexCount = (int)(v.size() / 3);
    m.triangleCount = (int)(idx.size() / 3);
    m.vertices = (float*)MemAlloc(sizeof(float) * v.size());
    for (size_t i = 0; i < v.size(); ++i) m.vertices[i] = v[i];
    m.indices = (unsigned short*)MemAlloc(sizeof(unsigned short) * idx.size());
    for (size_t i = 0; i < idx.size(); ++i) m.indices[i] = idx[i];
    return m;
}

// A flat n x n grid of quads on the z=0 plane spanning [0,span]x[0,span].
static Mesh makeGrid(int n /*cells per side*/, float span) {
    std::vector<float> v;
    std::vector<unsigned short> idx;
    int side = n + 1;
    for (int j = 0; j < side; ++j)
        for (int i = 0; i < side; ++i) {
            v.push_back(span * (float)i / (float)n);
            v.push_back(span * (float)j / (float)n);
            v.push_back(0.0f);
        }
    auto vid = [&](int i, int j) { return (unsigned short)(j*side + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            idx.push_back(vid(i, j));   idx.push_back(vid(i+1, j));   idx.push_back(vid(i+1, j+1));
            idx.push_back(vid(i, j));   idx.push_back(vid(i+1, j+1)); idx.push_back(vid(i, j+1));
        }
    return makeMesh(v, idx);
}

static void test_empty_input() {
    printf("=== test_empty_input ===\n");
    Mesh empty = {0};
    Mesh out = simplify_mesh(empty, SimplifyOptions{});
    assert(out.vertexCount == 0 && out.triangleCount == 0);
    printf("PASSED\n");
}

static void test_single_triangle() {
    printf("=== test_single_triangle ===\n");
    std::vector<float> v = {0,0,0, 1,0,0, 0,1,0};
    std::vector<unsigned short> idx = {0,1,2};
    Mesh in = makeMesh(v, idx);
    SimplifyOptions o; o.target_ratio = 0.1f; // ask for fewer, but 1 tri is the floor
    Mesh out = simplify_mesh(in, o);
    assert(out.triangleCount == 1);
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

static void test_identity_ratio_one() {
    printf("=== test_identity_ratio_one ===\n");
    Mesh in = makeGrid(2, 2.0f); // 9 verts, 8 tris
    SimplifyOptions o; o.target_ratio = 1.0f;
    Mesh out = simplify_mesh(in, o);
    assert(out.triangleCount == in.triangleCount);
    assert(out.vertexCount == in.vertexCount);
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

static void test_weld_non_indexed() {
    printf("=== test_weld_non_indexed ===\n");
    // Two triangles sharing an edge, expressed WITHOUT indices (6 verts, 2 shared).
    std::vector<float> v = {
        0,0,0, 1,0,0, 1,1,0,   // tri 0
        0,0,0, 1,1,0, 0,1,0    // tri 1 (shares 0,0,0 and 1,1,0)
    };
    Mesh in = {0};
    in.vertexCount = 6;
    in.triangleCount = 2;
    in.vertices = (float*)MemAlloc(sizeof(float) * v.size());
    for (size_t i = 0; i < v.size(); ++i) in.vertices[i] = v[i];
    // indices == NULL -> simplifier must weld
    SimplifyOptions o; o.target_ratio = 1.0f;
    Mesh out = simplify_mesh(in, o);
    assert(out.vertexCount == 4);   // welded down to 4 unique corners
    assert(out.triangleCount == 2);
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

static void test_normals_unit_length() {
    printf("=== test_normals_unit_length ===\n");
    Mesh in = makeGrid(2, 2.0f);
    Mesh out = simplify_mesh(in, SimplifyOptions{}); // ratio 0.5; result must still have unit normals
    assert(out.normals != nullptr);
    for (int i = 0; i < out.vertexCount; ++i) {
        float nx = out.normals[i*3+0], ny = out.normals[i*3+1], nz = out.normals[i*3+2];
        float l = sqrtf(nx*nx + ny*ny + nz*nz);
        assert(fabsf(l - 1.0f) < 1e-3f);
    }
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

// Count distinct vertex positions in a mesh that lie on x == plane_x (within eps).
static std::vector<std::array<float,3>> boundaryVertsOnX(const Mesh& m, float plane_x, float eps) {
    std::vector<std::array<float,3>> out;
    for (int i = 0; i < m.vertexCount; ++i) {
        float x = m.vertices[i*3+0], y = m.vertices[i*3+1], z = m.vertices[i*3+2];
        if (fabsf(x - plane_x) < eps) out.push_back({x, y, z});
    }
    return out;
}

static bool containsPos(const std::vector<std::array<float,3>>& s, float x, float y, float z, float eps) {
    for (const auto& p : s)
        if (fabsf(p[0]-x) < eps && fabsf(p[1]-y) < eps && fabsf(p[2]-z) < eps) return true;
    return false;
}

static void test_triangle_reduction() {
    printf("=== test_triangle_reduction ===\n");
    Mesh in = makeGrid(8, 4.0f); // 81 verts, 128 tris
    SimplifyOptions o; o.target_ratio = 0.5f; o.lock_boundary = false;
    Mesh out = simplify_mesh(in, o);
    printf("  in tris=%d out tris=%d\n", in.triangleCount, out.triangleCount);
    assert(out.triangleCount <= (int)(0.5f * in.triangleCount));
    assert(out.triangleCount > 0);
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

static void test_determinism() {
    printf("=== test_determinism ===\n");
    Mesh in = makeGrid(8, 4.0f);
    SimplifyOptions o; o.target_ratio = 0.4f; o.lock_boundary = false;
    Mesh a = simplify_mesh(in, o);
    Mesh b = simplify_mesh(in, o);
    assert(a.vertexCount == b.vertexCount);
    assert(a.triangleCount == b.triangleCount);
    for (int i = 0; i < a.vertexCount*3; ++i) assert(a.vertices[i] == b.vertices[i]);
    for (int i = 0; i < a.triangleCount*3; ++i) assert(a.indices[i] == b.indices[i]);
    UnloadMesh(in); UnloadMesh(a); UnloadMesh(b);
    printf("PASSED\n");
}

static void test_no_degenerate_triangles() {
    printf("=== test_no_degenerate_triangles ===\n");
    Mesh in = makeGrid(8, 4.0f);
    SimplifyOptions o; o.target_ratio = 0.3f; o.lock_boundary = false;
    Mesh out = simplify_mesh(in, o);
    // Every output triangle must have non-trivial area, and (this is a flat
    // z=0 grid) its normal must stay aligned with +z -- no flips.
    for (int t = 0; t < out.triangleCount; ++t) {
        int a = out.indices[t*3+0], b = out.indices[t*3+1], c = out.indices[t*3+2];
        float ax=out.vertices[a*3+0], ay=out.vertices[a*3+1];
        float bx=out.vertices[b*3+0], by=out.vertices[b*3+1];
        float cx=out.vertices[c*3+0], cy=out.vertices[c*3+1];
        float area2 = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax); // 2*signed area, z component
        assert(fabsf(area2) > 1e-5f);   // non-degenerate
        assert(area2 > 0.0f);           // winding preserved (no flip)
    }
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

static void test_boundary_preserved() {
    printf("=== test_boundary_preserved ===\n");
    Mesh in = makeGrid(8, 4.0f); // grid spans [0,4]x[0,4] on z=0
    CellBounds cb; cb.min_bound = {0,0,-1}; cb.max_bound = {4,4,1};
    SimplifyOptions o; o.target_ratio = 0.3f; o.lock_boundary = true;
    Mesh out = simplify_mesh(in, o, &cb);
    // The x==0 edge of the grid has 9 boundary verts at y = 0,0.5,...,4.
    auto bv = boundaryVertsOnX(out, 0.0f, 1e-4f);
    for (int j = 0; j <= 8; ++j) {
        float y = 4.0f * (float)j / 8.0f;
        assert(containsPos(bv, 0.0f, y, 0.0f, 1e-3f));
    }
    UnloadMesh(in); UnloadMesh(out);
    printf("PASSED\n");
}

static void test_watertight_seam() {
    printf("=== test_watertight_seam ===\n");
    // Two grids sharing the plane x == 0. Cell A spans x in [-2,0], cell B x in [0,2].
    // Both use the SAME y samples on the shared face, so identical boundary verts.
    auto makeShiftedGrid = [](float x0, float x1) {
        std::vector<float> v; std::vector<unsigned short> idx;
        int n = 8, side = n + 1;
        for (int j = 0; j < side; ++j)
            for (int i = 0; i < side; ++i) {
                v.push_back(x0 + (x1 - x0) * (float)i / (float)n);
                v.push_back(4.0f * (float)j / (float)n);
                v.push_back(0.0f);
            }
        auto vid = [&](int i, int j) { return (unsigned short)(j*side + i); };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                idx.push_back(vid(i,j));   idx.push_back(vid(i+1,j));   idx.push_back(vid(i+1,j+1));
                idx.push_back(vid(i,j));   idx.push_back(vid(i+1,j+1)); idx.push_back(vid(i,j+1));
            }
        return makeMesh(v, idx);
    };
    Mesh A = makeShiftedGrid(-2.0f, 0.0f); // shared face is its x==0 (right) edge
    Mesh B = makeShiftedGrid(0.0f, 2.0f);  // shared face is its x==0 (left) edge
    CellBounds cbA; cbA.min_bound = {-2,0,-1}; cbA.max_bound = {0,4,1};
    CellBounds cbB; cbB.min_bound = {0,0,-1};  cbB.max_bound = {2,4,1};
    SimplifyOptions o; o.target_ratio = 0.3f; o.lock_boundary = true;
    Mesh sA = simplify_mesh(A, o, &cbA);
    Mesh sB = simplify_mesh(B, o, &cbB);
    auto bvA = boundaryVertsOnX(sA, 0.0f, 1e-4f);
    auto bvB = boundaryVertsOnX(sB, 0.0f, 1e-4f);
    // Every shared-face vertex must appear in BOTH simplified meshes at the
    // same position -> no crack along the seam.
    for (int j = 0; j <= 8; ++j) {
        float y = 4.0f * (float)j / 8.0f;
        assert(containsPos(bvA, 0.0f, y, 0.0f, 1e-3f));
        assert(containsPos(bvB, 0.0f, y, 0.0f, 1e-3f));
    }
    UnloadMesh(A); UnloadMesh(B); UnloadMesh(sA); UnloadMesh(sB);
    printf("PASSED\n");
}

int main() {
    printf("=== Mesh Simplifier Tests ===\n");
    test_empty_input();
    test_single_triangle();
    test_identity_ratio_one();
    test_weld_non_indexed();
    test_normals_unit_length();
    test_triangle_reduction();
    test_determinism();
    test_no_degenerate_triangles();
    test_boundary_preserved();
    test_watertight_seam();
    printf("\nAll mesh simplifier tests PASSED\n");
    return 0;
}
