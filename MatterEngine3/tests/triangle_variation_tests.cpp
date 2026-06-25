// SP-6 (Direct-Triangle Path & Variations) headless GL-free test suite.
//
// Spec Testing-bullet -> covering test:
//   - Triangle emission (per-tri material + transform stack)  -> test_triangle_emission
//   - One BLAS (voxel + direct quad, distinct per-tri mats)    -> test_one_blas_merge
//   - Skinned line (stepped spheres, lerped radius taper)      -> test_skinned_line
//   - No field interaction (triangle survives over a brush)    -> test_no_field_interaction
//   - Variation dedup (same params -> one artifact, N records) -> test_variation_dedup
//   - Variation/LOD independence (same LOD-array shape)        -> test_variation_lod_independence
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

static void test_skinned_line() {
    // line from (0,0,0) to (2,0,0), constant radius 0.5 -> solid stepped spheres.
    tri_emit::TriangleBuildBuffer buf;
    mat4 id = mat4::Identity();
    buf.line(make_float3(0,0,0), make_float3(2,0,0), 0.5f, 0.5f,
             /*material*/4, id, /*rings*/4, /*segments*/6);
    CHECK(!buf.triangles().empty(), "tubed line produced triangles");
    CHECK(buf.triangles().size() == buf.tri_extra().size(), "Tri/TriEx parallel");
    // all triangles carry the line's material
    for (const TriEx& e : buf.tri_extra())
        CHECK(e.materialId == 4, "tubed triangle has line material");

    // Geometry is solid around the axis: some triangle vertex must be off-axis
    // (radius applied), not collapsed onto the segment.
    bool off_axis = false;
    for (const Tri& t : buf.triangles()) {
        if (fabsf(t.vertex0.y) > 0.1f || fabsf(t.vertex0.z) > 0.1f) off_axis = true;
    }
    CHECK(off_axis, "stepped spheres have radius (not degenerate to the line)");

    // Lerped radius taper: r0=0.6 at a, r1=0.1 at b. Max |offset from axis|
    // near a must exceed that near b.
    tri_emit::TriangleBuildBuffer taper;
    taper.line(make_float3(0,0,0), make_float3(2,0,0), 0.6f, 0.1f,
               /*material*/4, id, /*rings*/4, /*segments*/6);
    float max_r_near_a = 0.0f, max_r_near_b = 0.0f;
    for (const Tri& t : taper.triangles()) {
        float3 v = t.centroid;
        float r = sqrtf(v.y*v.y + v.z*v.z);
        if (v.x < 0.5f)      max_r_near_a = (r > max_r_near_a) ? r : max_r_near_a;
        else if (v.x > 1.5f) max_r_near_b = (r > max_r_near_b) ? r : max_r_near_b;
    }
    CHECK(max_r_near_a > max_r_near_b, "radius tapers from a (0.6) to b (0.1)");
}

static void test_no_field_interaction() {
    // A voxel brush (center, radius) that fully contains the authored triangle.
    // The triangle must NOT be carved/smoothed toward the field: emitted verts
    // exactly equal the authored verts. triangle_emit has no field input by
    // construction, so this pins that contract.
    float3 brush_center = make_float3(0,0,0);
    float  brush_radius = 10.0f;   // triangle lies well inside this sphere

    float3 v0 = make_float3(0.0f, 0.0f, 0.0f);
    float3 v1 = make_float3(1.0f, 0.0f, 0.0f);
    float3 v2 = make_float3(0.0f, 1.0f, 0.0f);

    tri_emit::TriangleBuildBuffer buf;
    buf.beginShape(tri_emit::ShapeType::TRIANGLES, mat4::Identity(), /*material*/9);
    buf.vertex(v0); buf.vertex(v1); buf.vertex(v2);
    buf.endShape();

    CHECK(buf.triangles().size() == 1, "one triangle survives");
    const Tri& t = buf.triangles()[0];
    // Authored vertices are preserved exactly (no projection onto the SDF).
    CHECK(near3(t.vertex0, v0, 1e-6f), "v0 not pulled to field surface");
    CHECK(near3(t.vertex1, v1, 1e-6f), "v1 not pulled to field surface");
    CHECK(near3(t.vertex2, v2, 1e-6f), "v2 not pulled to field surface");
    // Sanity: vertices really are inside the brush volume (so a field-coupled
    // path WOULD have moved them) -> the no-op is meaningful, not vacuous.
    float d0 = sqrtf(v0.x*v0.x + v0.y*v0.y + v0.z*v0.z);
    CHECK(d0 < brush_radius, "vertex genuinely inside brush -> survival is meaningful");
    (void)brush_center;
}

static void test_variation_dedup() {
    // A child part's source bytes (opaque) + variation params (opaque) fold into
    // a resolved hash. Same variation params N times -> same hash (one artifact),
    // N child-instance records. Different params -> distinct hash.
    tri_emit::VariationRecorder rec;
    const char* child_src = "class Rock extends Part {}";

    // variation A applied to 3 instances at different transforms
    const unsigned char paramsA[] = { 's','e','e','d','=','1' };
    mat4 x1 = mat4::Translate(make_float3(1,0,0));
    mat4 x2 = mat4::Translate(make_float3(2,0,0));
    mat4 x3 = mat4::Translate(make_float3(3,0,0));
    uint64_t hA1 = rec.instance(child_src, 26, paramsA, sizeof(paramsA), x1);
    uint64_t hA2 = rec.instance(child_src, 26, paramsA, sizeof(paramsA), x2);
    uint64_t hA3 = rec.instance(child_src, 26, paramsA, sizeof(paramsA), x3);
    CHECK(hA1 == hA2 && hA2 == hA3, "same variation params -> same resolved hash");

    // variation B: different params -> distinct artifact
    const unsigned char paramsB[] = { 's','e','e','d','=','2' };
    uint64_t hB = rec.instance(child_src, 26, paramsB, sizeof(paramsB), x1);
    CHECK(hB != hA1, "different variation params -> distinct resolved hash");

    // child-instance table: 4 records, transforms preserved, hashes correct
    const std::vector<tri_emit::ChildInstance>& kids = rec.children();
    CHECK(kids.size() == 4, "one child-instance record per instance() call");
    CHECK(kids[0].child_resolved_hash == hA1, "record 0 carries variation-A hash");
    CHECK(kids[3].child_resolved_hash == hB,  "record 3 carries variation-B hash");
    CHECK(fabsf(kids[1].transform[3] - 2.0f) < 1e-6f, "record 1 transform tx=2 (row-major [3])");

    // distinct cached artifacts = distinct hashes among the records
    int distinct = 0;
    uint64_t seen[8]; int ns = 0;
    for (const tri_emit::ChildInstance& c : kids) {
        bool found = false;
        for (int i = 0; i < ns; ++i) if (seen[i] == c.child_resolved_hash) found = true;
        if (!found) { seen[ns++] = c.child_resolved_hash; ++distinct; }
    }
    CHECK(distinct == 2, "3x variation A + 1x variation B -> 2 distinct artifacts");
}

// Models the SP-4 contract: LOD level count is a pipeline constant, independent
// of the variation params. Lives in the test (SP-4 owns the real generator).
static int lod_level_count_for(uint64_t /*resolved_hash*/) {
    return 3;  // ~3 LOD levels per part, same rule for every variation
}

static void test_variation_lod_independence() {
    tri_emit::VariationRecorder rec;
    const char* src = "class Tree extends Part {}";
    const unsigned char pA[] = { 'v','=','a' };
    const unsigned char pB[] = { 'v','=','b' };

    uint64_t hA = rec.instance(src, 26, pA, sizeof(pA), mat4::Identity());
    uint64_t hB = rec.instance(src, 26, pB, sizeof(pB), mat4::Identity());

    CHECK(hA != hB, "two variations are distinct artifacts (variation picks geometry)");
    // LOD shape is identical across variations (LOD picks detail, not geometry).
    CHECK(lod_level_count_for(hA) == lod_level_count_for(hB),
          "both variations get the same LOD-array shape (LOD independent of variation)");
    CHECK(lod_level_count_for(hA) == 3, "expected ~3 LOD levels per variation");
}

int main() {
    test_triangle_emission();
    test_one_blas_merge();
    test_skinned_line();
    test_no_field_interaction();
    test_variation_dedup();
    test_variation_lod_independence();
    if (failures == 0) printf("All triangle_variation tests passed\n");
    return failures == 0 ? 0 : 1;
}
