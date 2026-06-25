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

static void test_one_blas_merge() {
    // Simulate the voxel-lowered surface mesh as a small triangle set with
    // material 1 (real SP-2 lowering is GL-bound; here we exercise the merge
    // seam: voxel tris + direct tris -> ONE register_triangles call).
    std::vector<Tri>   host_tris;
    std::vector<TriEx> host_triex;
    // "voxel sphere" stand-in: two triangles, material 1.
    {
        Tri a; a.vertex0 = make_float3(0,0,0); a.vertex1 = make_float3(1,0,0);
               a.vertex2 = make_float3(0,1,0); a.centroid = make_float3(.33f,.33f,0);
        Tri b; b.vertex0 = make_float3(1,1,0); b.vertex1 = make_float3(0,1,0);
               b.vertex2 = make_float3(1,0,0); b.centroid = make_float3(.66f,.66f,0);
        TriEx ea{}; ea.materialId = 1; ea.tint = make_float4(1,1,1,0);
        ea.N0=ea.N1=ea.N2=make_float3(0,0,1); ea.ao0=ea.ao1=ea.ao2=1;
        TriEx eb = ea;
        host_tris.push_back(a); host_tris.push_back(b);
        host_triex.push_back(ea); host_triex.push_back(eb);
    }

    // Direct triangle quad, material 2, offset so it's distinct geometry.
    tri_emit::TriangleBuildBuffer buf;
    mat4 xf = mat4::Translate(make_float3(5, 0, 0));
    buf.beginShape(tri_emit::ShapeType::TRIANGLES, xf, /*material*/2);
    buf.vertex(make_float3(0,0,0)); buf.vertex(make_float3(1,0,0)); buf.vertex(make_float3(1,1,0));
    buf.vertex(make_float3(0,0,0)); buf.vertex(make_float3(1,1,0)); buf.vertex(make_float3(0,1,0));
    buf.endShape();
    CHECK(buf.triangles().size() == 2, "quad is 2 triangles");

    // MERGE into the single host buffer, then ONE BLAS.
    buf.appendTo(host_tris, host_triex);
    CHECK(host_tris.size() == 4, "voxel(2) + direct(2) merged before register");
    CHECK(host_triex.size() == 4, "triex parallel after merge");

    BLASManager mgr;
    BLASHandle h = mgr.register_triangles(host_tris.data(), (int)host_tris.size(),
                                          host_triex.data());
    CHECK(h != INVALID_BLAS_HANDLE, "merged BLAS registered");
    CHECK(mgr.get_unique_blas_count() == 1, "exactly ONE part BLAS, no second BLAS");

    const BLASManager::BLASEntry* e = mgr.get_entry(h);
    CHECK(e != nullptr, "entry retrievable");
    CHECK(e->triangles.size() == 4, "single BLAS holds all 4 triangles");
    // distinct per-triangle materials coexist in the one BLAS
    bool has1 = false, has2 = false;
    for (const TriEx& tx : e->tri_extra) { if (tx.materialId == 1) has1 = true;
                                           if (tx.materialId == 2) has2 = true; }
    CHECK(has1 && has2, "both per-triangle materials present in one BLAS");
}

int main() {
    test_triangle_emission();
    test_one_blas_merge();
    if (failures == 0) printf("All triangle_variation tests passed\n");
    return failures == 0 ? 0 : 1;
}
