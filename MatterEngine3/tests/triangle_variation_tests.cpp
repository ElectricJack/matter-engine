#include "../include/triangle_emit.hpp"
#include "../../MatterSurfaceLib/include/blas_manager.hpp"
#include <cstdio>
#include <cmath>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static bool near3(float3 a, float3 b, float eps = 1e-5f) {
    return fabsf(a.x-b.x) < eps && fabsf(a.y-b.y) < eps && fabsf(a.z-b.z) < eps;
}

static void test_triangle_emission() {
    // A single TRIANGLES shape of one tri, identity transform, material 7.
    tri_emit::TriangleBuildBuffer buf;
    mat4 xf = mat4::Identity();
    buf.beginShape(tri_emit::ShapeType::TRIANGLES, xf, /*material*/7);
    buf.vertex(make_float3(0, 0, 0));
    buf.vertex(make_float3(1, 0, 0));
    buf.vertex(make_float3(0, 1, 0));
    buf.endShape();

    CHECK(buf.triangles().size() == 1, "one triangle emitted");
    CHECK(buf.tri_extra().size() == 1, "one TriEx emitted (parallel)");
    const Tri& t = buf.triangles()[0];
    CHECK(near3(t.vertex0, make_float3(0,0,0)), "v0 untransformed under identity");
    CHECK(near3(t.vertex1, make_float3(1,0,0)), "v1 untransformed under identity");
    CHECK(near3(t.vertex2, make_float3(0,1,0)), "v2 untransformed under identity");
    const TriEx& e = buf.tri_extra()[0];
    CHECK(e.materialId == 7, "per-triangle material is the current cursor");
    CHECK(fabsf(e.tint.w - 0.0f) < 1e-6f, "neutral tint alpha 0 (no tint)");
    CHECK(near3(e.N0, make_float3(0,0,1)) && near3(e.N1, make_float3(0,0,1))
          && near3(e.N2, make_float3(0,0,1)), "face normal = +Z for CCW XY tri");

    // Transform stack applied: translate (10,0,0) then the same tri.
    tri_emit::TriangleBuildBuffer buf2;
    mat4 tr = mat4::Translate(make_float3(10, 0, 0));
    buf2.beginShape(tri_emit::ShapeType::TRIANGLES, tr, /*material*/3);
    buf2.vertex(make_float3(0, 0, 0));
    buf2.vertex(make_float3(1, 0, 0));
    buf2.vertex(make_float3(0, 1, 0));
    buf2.endShape();
    CHECK(near3(buf2.triangles()[0].vertex0, make_float3(10,0,0)), "transform applied to v0");
    CHECK(near3(buf2.triangles()[0].centroid,
                make_float3(10 + 1.0f/3.0f, 1.0f/3.0f, 0.0f)), "centroid transformed");
}

int main() {
    test_triangle_emission();
    if (failures == 0) printf("All triangle_variation tests passed\n");
    return failures == 0 ? 0 : 1;
}
