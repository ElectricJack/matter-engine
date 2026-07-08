// Phase 3: constrained ear-clipping triangulator unit tests (pure, GL-free).
//
// Spec testing bullets -> covering test:
//   - convex square           -> test_convex_square
//   - concave "L"             -> test_concave_L
//   - square-with-square-hole -> test_square_with_hole
// For each: correct triangle count, no degenerate/zero-area tris, triangles
// cover the polygon area (and not the hole).
#include "../src/polygon_triangulate.hpp"
#include <cstdio>
#include <cmath>
#include <vector>

using poly_tri::Contour;
using poly_tri::Profile;
using poly_tri::triangulate;

#include "check.h"

struct V2 { float x, y; };

// Signed area of triangle (a,b,c) in the profile plane.
static float tri_area(V2 a, V2 b, V2 c) {
    return 0.5f * ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
}

// Point-in-triangle (barycentric sign test), tolerant of edges.
static bool point_in_tri(V2 p, V2 a, V2 b, V2 c) {
    float d1 = (p.x-b.x)*(a.y-b.y) - (a.x-b.x)*(p.y-b.y);
    float d2 = (p.x-c.x)*(b.y-c.y) - (b.x-c.x)*(p.y-c.y);
    float d3 = (p.x-a.x)*(c.y-a.y) - (c.x-a.x)*(p.y-a.y);
    bool has_neg = (d1 < -1e-5f) || (d2 < -1e-5f) || (d3 < -1e-5f);
    bool has_pos = (d1 >  1e-5f) || (d2 >  1e-5f) || (d3 >  1e-5f);
    return !(has_neg && has_pos);
}

// Sum of |signed area| over the emitted triangle fan, given the flat profile
// vertex list (outer + holes concatenated) the triangulator indexed against.
static float total_area(const std::vector<V2>& verts, const std::vector<int>& idx) {
    float a = 0;
    for (size_t i = 0; i + 2 < idx.size() + 1 && i + 2 < idx.size(); i += 3) {
        a += fabsf(tri_area(verts[idx[i]], verts[idx[i+1]], verts[idx[i+2]]));
    }
    return a;
}

// Build the flat vertex list the triangulator sees: outer contour then each
// hole, in order. Indices returned by triangulate() reference this list.
static std::vector<V2> flat_verts(const Profile& p) {
    std::vector<V2> v;
    for (auto& q : p.outer) v.push_back({q.x, q.y});
    for (auto& h : p.holes) for (auto& q : h) v.push_back({q.x, q.y});
    return v;
}

static bool any_degenerate(const std::vector<V2>& verts, const std::vector<int>& idx) {
    for (size_t i = 0; i + 2 < idx.size() + 1 && i + 2 < idx.size(); i += 3) {
        if (fabsf(tri_area(verts[idx[i]], verts[idx[i+1]], verts[idx[i+2]])) < 1e-7f)
            return true;
    }
    return false;
}

static void test_convex_square() {
    Profile p;
    p.outer = { {0,0}, {2,0}, {2,2}, {0,2} };   // CCW
    std::vector<int> idx = triangulate(p);
    CHECK(idx.size() == 6, "convex square -> 2 triangles (6 indices)");
    std::vector<V2> v = flat_verts(p);
    CHECK(!any_degenerate(v, idx), "convex square -> no zero-area triangle");
    CHECK(fabsf(total_area(v, idx) - 4.0f) < 1e-3f,
          "convex square -> triangles cover area 4");
}

static void test_concave_L() {
    // An L-shape (concave): area = 3 (a 2x2 with a 1x1 corner removed).
    Profile p;
    p.outer = { {0,0}, {2,0}, {2,1}, {1,1}, {1,2}, {0,2} };  // CCW, concave at (1,1)
    std::vector<int> idx = triangulate(p);
    CHECK(idx.size() == 12, "concave L (6 verts) -> 4 triangles (12 indices)");
    std::vector<V2> v = flat_verts(p);
    CHECK(!any_degenerate(v, idx), "concave L -> no zero-area triangle");
    CHECK(fabsf(total_area(v, idx) - 3.0f) < 1e-3f,
          "concave L -> triangles cover area 3 (not the removed corner)");
    // The removed corner (1.5, 1.5) must NOT be covered by any triangle.
    V2 hole_pt{1.5f, 1.5f};
    bool covered = false;
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
        if (point_in_tri(hole_pt, v[idx[i]], v[idx[i+1]], v[idx[i+2]])) covered = true;
    CHECK(!covered, "concave L -> the cut-out corner is not triangulated");
}

static void test_square_with_hole() {
    // Outer 4x4 square (CCW), inner 2x2 hole (CW). Area = 16 - 4 = 12.
    Profile p;
    p.outer = { {0,0}, {4,0}, {4,4}, {0,4} };               // CCW
    p.holes.push_back({ {1,1}, {1,3}, {3,3}, {3,1} });      // CW (hole)
    std::vector<int> idx = triangulate(p);
    // 8 total vertices, with a hole -> 2*8 - 2 = ... earcut emits (n + 2*h - 2)
    // triangles for n boundary verts; for a square+square hole this is 8 tris.
    CHECK(idx.size() == 24, "square+hole (8 verts) -> 8 triangles (24 indices)");
    std::vector<V2> v = flat_verts(p);
    CHECK(!any_degenerate(v, idx), "square+hole -> no zero-area triangle");
    CHECK(fabsf(total_area(v, idx) - 12.0f) < 1e-3f,
          "square+hole -> triangles cover annular area 12 (hole excluded)");
    // Center (2,2) is inside the hole and must NOT be covered.
    V2 center{2.0f, 2.0f};
    bool covered = false;
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
        if (point_in_tri(center, v[idx[i]], v[idx[i+1]], v[idx[i+2]])) covered = true;
    CHECK(!covered, "square+hole -> hole interior is not triangulated");
    // A point in the annulus (0.5,0.5) MUST be covered.
    V2 ann{0.5f, 0.5f};
    bool ann_covered = false;
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
        if (point_in_tri(ann, v[idx[i]], v[idx[i+1]], v[idx[i+2]])) ann_covered = true;
    CHECK(ann_covered, "square+hole -> annulus region IS triangulated");
}

int main() {
    test_convex_square();
    test_concave_L();
    test_square_with_hole();
    if (g_failures == 0) printf("All polygon_triangulate tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
