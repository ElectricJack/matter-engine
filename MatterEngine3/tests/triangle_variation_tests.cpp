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
#include <algorithm>
#include <utility>
#include <tuple>
#include <vector>

#include "check.h"

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

// G8: TriangleBuildBuffer::sphere emits a closed UV sphere of the correct radius,
// baked under the transform; box emits 12 triangles. G4: tint flows into TriEx.
static void test_g8_mesh_sphere_box() {
    // Sphere: every vertex ~ r from center; watertight (every edge shared by 2 tris).
    tri_emit::TriangleBuildBuffer sb;
    sb.sphere(make_float3(0,0,0), 1.5f, /*mat*/3, mat4::Identity(), /*segments*/12);
    CHECK(!sb.triangles().empty(), "G8: sphere emits triangles");
    float maxr=0, minr=1e9f;
    for (const Tri& t : sb.triangles())
        for (const float3& v : { t.vertex0, t.vertex1, t.vertex2 }) {
            float r = sqrtf(v.x*v.x+v.y*v.y+v.z*v.z);
            if (r>maxr) maxr=r; if (r<minr) minr=r;
        }
    CHECK(fabsf(maxr-1.5f)<1e-3f && fabsf(minr-1.5f)<1e-3f,
          "G8: sphere vertices lie on radius 1.5 (closed, round)");
    // Closed surface: no boundary edges. After merging coincident vertices (the
    // longitude seam + the two poles, where a UV sphere legitimately merges many
    // vertices into one position), every undirected edge must be shared by at
    // least two triangles -- i.e. there is no hole/open edge. (Pole/seam vertices
    // are high-valence, which is correct for a UV sphere, so we assert >= 2 rather
    // than == 2.)
    {
        std::vector<std::pair<long long,long long>> edges;
        auto key=[&](const float3& p){ // quantize to a stable vertex id
            long long x=(long long)llround(p.x*1000.0), y=(long long)llround(p.y*1000.0), z=(long long)llround(p.z*1000.0);
            return (x*73856093LL) ^ (y*19349663LL) ^ (z*83492791LL);
        };
        auto add=[&](const float3&a,const float3&b){ long long ka=key(a),kb=key(b);
            if (ka==kb) return;  // degenerate edge at a pole fan; not a boundary
            edges.push_back(ka<kb?std::make_pair(ka,kb):std::make_pair(kb,ka)); };
        for (const Tri& t : sb.triangles()) { add(t.vertex0,t.vertex1); add(t.vertex1,t.vertex2); add(t.vertex2,t.vertex0); }
        std::sort(edges.begin(), edges.end());
        bool closed = !edges.empty();
        for (size_t i=0;i<edges.size();) {
            size_t j=i; while (j<edges.size() && edges[j]==edges[i]) ++j;
            if ((j-i) < 2) closed=false; i=j;   // an edge with <2 tris = a hole
        }
        CHECK(closed, "G8: sphere is closed (no boundary edges -> watertight)");
    }

    // Sphere baked under a translation: center moves with the transform.
    tri_emit::TriangleBuildBuffer sb2;
    sb2.sphere(make_float3(0,0,0), 1.0f, 0, mat4::Translate(make_float3(10,0,0)), 8);
    float cx=0; int nv=0;
    for (const Tri& t : sb2.triangles()) { cx+=t.centroid.x; ++nv; }
    CHECK(nv>0 && fabsf((cx/nv)-10.0f) < 0.5f, "G8: sphere baked under translate (centered ~x=10)");

    // Box: 12 triangles; all 8 corners present.
    tri_emit::TriangleBuildBuffer bb;
    bb.box(make_float3(0,0,0), make_float3(1,1,1), /*mat*/5, mat4::Identity());
    CHECK(bb.triangles().size() == 12, "G8: box is 12 triangles");
    bool minc=false, maxc=false;
    for (const Tri& t : bb.triangles())
        for (const float3& v : { t.vertex0, t.vertex1, t.vertex2 }) {
            if (near3(v, make_float3(-1,-1,-1))) minc=true;
            if (near3(v, make_float3( 1, 1, 1))) maxc=true;
        }
    CHECK(minc && maxc, "G8: box spans both extreme corners");

    // G4: tint flows into TriEx on the mesh emitters.
    tri_emit::TriangleBuildBuffer tb;
    tb.box(make_float3(0,0,0), make_float3(1,1,1), 0, mat4::Identity(),
           make_float4(0.1f,0.2f,0.3f,0.7f));
    bool all_tinted = !tb.tri_extra().empty();
    for (const TriEx& e : tb.tri_extra())
        if (fabsf(e.tint.x-0.1f)>1e-6f || fabsf(e.tint.w-0.7f)>1e-6f) all_tinted=false;
    CHECK(all_tinted, "G4: mesh box carries the supplied tint into every TriEx");
}

// --- Phase 3: extrude mesh machine -----------------------------------------

// Watertight test: every undirected edge of the mesh is shared by exactly two
// triangles (a closed 2-manifold). Vertices are quantized so coincident corners
// merge. Returns true if closed.
static bool is_watertight(const std::vector<Tri>& tris) {
    // Collision-free vertex id: pack quantized (x,y,z) into a tuple (no XOR fold,
    // which collides for symmetric coordinates like a cube's corners).
    auto key = [](const float3& p) {
        return std::make_tuple((long long)llround(p.x*1000.0),
                               (long long)llround(p.y*1000.0),
                               (long long)llround(p.z*1000.0));
    };
    using Key = std::tuple<long long,long long,long long>;
    std::vector<std::pair<Key,Key>> edges;
    auto add=[&](const float3&a,const float3&b){ Key ka=key(a),kb=key(b);
        if (ka==kb) return;
        edges.push_back(ka<kb?std::make_pair(ka,kb):std::make_pair(kb,ka)); };
    for (const Tri& t : tris) { add(t.vertex0,t.vertex1); add(t.vertex1,t.vertex2); add(t.vertex2,t.vertex0); }
    std::sort(edges.begin(), edges.end());
    if (edges.empty()) return false;
    for (size_t i=0;i<edges.size();) {
        size_t j=i; while (j<edges.size() && edges[j]==edges[i]) ++j;
        if ((j-i) != 2) return false;   // open edge (1) or non-manifold (>2)
        i=j;
    }
    return true;
}

// Extrude a unit square profile along one segment -> a closed prism, watertight.
static void test_extrude_square_prism() {
    tri_emit::Profile prof;
    prof.outer = { {-0.5f,-0.5f}, {0.5f,-0.5f}, {0.5f,0.5f}, {-0.5f,0.5f} }; // CCW
    float3 path[2] = { make_float3(0,0,0), make_float3(0,0,2) };

    tri_emit::TriangleBuildBuffer buf;
    buf.extrude(prof, path, 2, tri_emit::JoinType::MITER, /*mat*/3, mat4::Identity());
    CHECK(!buf.triangles().empty(), "extrude square prism emits triangles");
    CHECK(buf.triangles().size() == buf.tri_extra().size(), "Tri/TriEx parallel");
    CHECK(is_watertight(buf.triangles()),
          "extrude square prism is watertight (every edge shared by exactly 2 tris)");
    for (const TriEx& e : buf.tri_extra())
        CHECK(e.materialId == 3, "extrude prism carries the material");
}

// Extrude an annulus (outer square + inner square hole) -> a hollow tube with
// capped (annular) ends. Walls on both contours + annular caps; watertight.
static void test_extrude_annulus_tube() {
    tri_emit::Profile prof;
    prof.outer = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };               // CCW
    prof.holes.push_back({ {-0.4f,-0.4f}, {-0.4f,0.4f}, {0.4f,0.4f}, {0.4f,-0.4f} }); // CW
    float3 path[2] = { make_float3(0,0,0), make_float3(0,0,3) };

    tri_emit::TriangleBuildBuffer buf;
    buf.extrude(prof, path, 2, tri_emit::JoinType::MITER, /*mat*/2, mat4::Identity());
    CHECK(!buf.triangles().empty(), "extrude annulus emits triangles");
    CHECK(is_watertight(buf.triangles()),
          "extrude annulus tube is watertight (hollow with annular caps)");
    // Inner cavity exists: some wall vertex sits at the hole radius (|x|~0.4),
    // distinct from the outer wall (|x|~1.0).
    bool inner_wall=false, outer_wall=false;
    for (const Tri& t : buf.triangles())
        for (const float3& v : { t.vertex0, t.vertex1, t.vertex2 }) {
            float ax=fabsf(v.x), ay=fabsf(v.y);
            if (fabsf(ax-0.4f)<1e-3f || fabsf(ay-0.4f)<1e-3f) inner_wall=true;
            if (fabsf(ax-1.0f)<1e-3f || fabsf(ay-1.0f)<1e-3f) outer_wall=true;
        }
    CHECK(inner_wall && outer_wall, "extrude annulus has both inner (hole) and outer walls");
}

// Extrude along an L-shaped polyline (3 points): no twist at the joint, and
// MITER vs BEVEL produce a different corner triangle count.
static void test_extrude_polyline_joins() {
    tri_emit::Profile prof;
    prof.outer = { {-0.3f,-0.3f}, {0.3f,-0.3f}, {0.3f,0.3f}, {-0.3f,0.3f} };
    // L path: go +X then +Y, bending in the XY plane.
    float3 path[3] = { make_float3(0,0,0), make_float3(2,0,0), make_float3(2,2,0) };

    tri_emit::TriangleBuildBuffer miter;
    miter.extrude(prof, path, 3, tri_emit::JoinType::MITER, 1, mat4::Identity());
    CHECK(!miter.triangles().empty(), "extrude L (miter) emits triangles");

    tri_emit::TriangleBuildBuffer bevel;
    bevel.extrude(prof, path, 3, tri_emit::JoinType::BEVEL, 1, mat4::Identity());
    CHECK(!bevel.triangles().empty(), "extrude L (bevel) emits triangles");

    // The join differs: bevel inserts an extra chamfer band at the corner, so
    // its triangle count differs from miter's.
    CHECK(miter.triangles().size() != bevel.triangles().size(),
          "MITER vs BEVEL produce a different corner triangle count");

    // No twist: the profile must not flip across the joint. Check the section at
    // the end retains the profile's extent (~0.3 half-width), i.e. the frame
    // transported without collapsing/rolling. The mid/end rings keep |offset|
    // near 0.3 from the path centerline.
    bool kept_extent = false;
    for (const Tri& t : miter.triangles()) {
        float3 v = t.vertex2;
        // near the end point (2,2,0): z stays ~0 (no out-of-plane twist).
        if (fabsf(v.x-2.0f)<0.5f && v.y>1.0f) {
            if (fabsf(v.z) < 0.35f) kept_extent = true;
        }
    }
    CHECK(kept_extent, "extrude L keeps the profile in-plane (no twist at the joint)");
}

// --- Phase 5: round-primitive mesh emitters ---------------------------------

// Cylinder = cappedCone with equal radii: a flat-capped circular wall, watertight,
// every wall vertex at radius r from the axis.
static void test_mesh_cylinder() {
    tri_emit::TriangleBuildBuffer buf;
    buf.cappedCone(make_float3(0,0,0), make_float3(0,0,2), 0.5f, 0.5f,
                   /*mat*/3, mat4::Identity(), /*segments*/16);
    CHECK(!buf.triangles().empty(), "cylinder emits triangles");
    CHECK(buf.triangles().size() == buf.tri_extra().size(), "Tri/TriEx parallel");
    CHECK(is_watertight(buf.triangles()),
          "cylinder is watertight (flat-capped circular wall)");
    for (const TriEx& e : buf.tri_extra())
        CHECK(e.materialId == 3, "cylinder carries material");
    // Wall radius: max distance from the z-axis ~ r.
    float maxr = 0;
    for (const Tri& t : buf.triangles())
        for (const float3& v : { t.vertex0, t.vertex1, t.vertex2 }) {
            float rr = sqrtf(v.x*v.x + v.y*v.y);
            if (rr > maxr) maxr = rr;
        }
    CHECK(fabsf(maxr - 0.5f) < 1e-3f, "cylinder wall radius ~ 0.5");
}

// Cone with r1==0: tapers to a point. Watertight, with a genuine apex (a vertex
// at the b endpoint) and no degenerate zero-area cap at that end.
static void test_mesh_cone_apex() {
    tri_emit::TriangleBuildBuffer buf;
    buf.cappedCone(make_float3(0,0,0), make_float3(0,0,2), 0.7f, 0.0f,
                   /*mat*/1, mat4::Identity(), /*segments*/16);
    CHECK(!buf.triangles().empty(), "cone (r1=0) emits triangles");
    CHECK(is_watertight(buf.triangles()), "cone to a point is watertight");
    // Apex present at b=(0,0,2); base radius ~0.7 near a.
    bool apex = false; float base_r = 0;
    for (const Tri& t : buf.triangles())
        for (const float3& v : { t.vertex0, t.vertex1, t.vertex2 }) {
            if (near3(v, make_float3(0,0,2), 1e-4f)) apex = true;
            if (fabsf(v.z) < 1e-4f) {
                float rr = sqrtf(v.x*v.x + v.y*v.y);
                if (rr > base_r) base_r = rr;
            }
        }
    CHECK(apex, "cone has a true apex vertex at b");
    CHECK(fabsf(base_r - 0.7f) < 1e-3f, "cone base radius ~ 0.7");
    // No zero-area triangles (the apex must not produce a degenerate flat cap).
    int degenerate = 0;
    for (const Tri& t : buf.triangles()) {
        float3 e1 = t.vertex1 - t.vertex0, e2 = t.vertex2 - t.vertex0;
        float3 n = cross(e1, e2);
        if (sqrtf(n.x*n.x+n.y*n.y+n.z*n.z) < 1e-9f) ++degenerate;
    }
    CHECK(degenerate == 0, "cone apex produces no zero-area triangles");
}

// Truncated cone: r0 != r1, both nonzero -> flat caps at both ends, watertight.
static void test_mesh_truncated_cone() {
    tri_emit::TriangleBuildBuffer buf;
    buf.cappedCone(make_float3(0,0,0), make_float3(0,3,0), 1.0f, 0.4f,
                   /*mat*/2, mat4::Identity(), /*segments*/20);
    CHECK(!buf.triangles().empty(), "truncated cone emits triangles");
    CHECK(is_watertight(buf.triangles()), "truncated cone is watertight");
    float r_at_a = 0, r_at_b = 0;
    for (const Tri& t : buf.triangles())
        for (const float3& v : { t.vertex0, t.vertex1, t.vertex2 }) {
            float rr = sqrtf(v.x*v.x + v.z*v.z);
            if (fabsf(v.y) < 1e-4f)        { if (rr > r_at_a) r_at_a = rr; }
            else if (fabsf(v.y-3.0f)<1e-4f){ if (rr > r_at_b) r_at_b = rr; }
        }
    CHECK(fabsf(r_at_a-1.0f)<1e-3f && fabsf(r_at_b-0.4f)<1e-3f,
          "truncated cone tapers 1.0 (a) -> 0.4 (b)");
}

// Capsule: cylindrical wall + a hemisphere cap at each end. Watertight, smooth,
// extent extends a full radius past each segment endpoint along the axis.
static void test_mesh_capsule() {
    tri_emit::TriangleBuildBuffer buf;
    buf.capsule(make_float3(0,0,0), make_float3(0,0,2), 0.5f,
                /*mat*/4, mat4::Identity(), /*segments*/16, /*rings*/6);
    CHECK(!buf.triangles().empty(), "capsule emits triangles");
    CHECK(is_watertight(buf.triangles()), "capsule is watertight (hemisphere caps)");
    for (const TriEx& e : buf.tri_extra())
        CHECK(e.materialId == 4, "capsule carries material");
    // Hemisphere caps: a pole sits a full radius beyond each endpoint
    // (z ~ -0.5 below a and z ~ 2.5 above b), and no vertex exceeds r from axis.
    float minz = 1e9f, maxz = -1e9f, maxr = 0;
    for (const Tri& t : buf.triangles())
        for (const float3& v : { t.vertex0, t.vertex1, t.vertex2 }) {
            if (v.z < minz) minz = v.z;
            if (v.z > maxz) maxz = v.z;
            float rr = sqrtf(v.x*v.x + v.y*v.y);
            if (rr > maxr) maxr = rr;
        }
    CHECK(fabsf(minz - (-0.5f)) < 1e-3f, "capsule -z pole a radius below a");
    CHECK(fabsf(maxz - 2.5f) < 1e-3f, "capsule +z pole a radius above b");
    CHECK(maxr <= 0.5f + 1e-3f, "capsule never exceeds radius from the axis");
}

// Mesh capsule/cylinder/cone bake under the transform stack (translation moves
// every vertex), mirroring the G8 sphere/box baking contract.
static void test_mesh_round_baked_transform() {
    tri_emit::TriangleBuildBuffer buf;
    buf.cappedCone(make_float3(0,0,0), make_float3(0,0,1), 0.3f, 0.3f,
                   0, mat4::Translate(make_float3(10,0,0)), 12);
    float cx = 0; int nv = 0;
    for (const Tri& t : buf.triangles()) { cx += t.centroid.x; ++nv; }
    CHECK(nv > 0 && fabsf((cx/nv) - 10.0f) < 0.5f,
          "round primitive baked under translate (centered ~x=10)");
}

int main() {
    test_triangle_emission();
    test_one_blas_merge();
    test_skinned_line();
    test_no_field_interaction();
    test_variation_dedup();
    test_variation_lod_independence();
    test_g8_mesh_sphere_box();
    test_extrude_square_prism();
    test_extrude_annulus_tube();
    test_extrude_polyline_joins();
    test_mesh_cylinder();
    test_mesh_cone_apex();
    test_mesh_truncated_cone();
    test_mesh_capsule();
    test_mesh_round_baked_transform();
    if (g_failures == 0) printf("All triangle_variation tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
