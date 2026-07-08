// Phase 1 — typed iso-primitives + ordered CSG (voxel/SDF path).
//
// These tests assert that the REAL mesher field evaluator (surface.c, via the
// field_probe seam) agrees with the analytic field_is_solid oracle in
// csg_lowering.cpp for:
//   1. a box (a real sdBox, not the old stamp_box sphere-soup),
//   2. a uniformly-scaled sphere (G2 regression: radius must scale),
//   3. ordered CSG (add A -> subtract B -> add C keeps C where it overlaps B).
//
// The probe lowers the DslState's BuildBuffer the same way the bake does
// (lower_build_buffer) and evaluates the lowered field through the production
// CalculateScalarAndMaterial + fat-prim + ordered-stage code path, so a sample
// is "meshed-occupied" iff the probed scalar is < 0 (the marching-cubes inside
// test).

#include <cstdio>
#include <cmath>
#include "dsl_state.h"
#include "csg_lowering.h"

#include "check.h"

// Probe the production field at a world point through the lowered field. Returns
// true if the meshed surface would treat the point as inside (scalar < 0).
// Implemented in field_probe.cpp (links surface.c's field eval).
bool mesh_field_is_solid(const dsl::LoweredField& f, Vector3 p);

// Convenience: lower a buffer and probe.
static bool meshed(const dsl::BuildBuffer& buf, Vector3 p) {
    dsl::LoweredField f = dsl::lower_build_buffer(buf);
    return mesh_field_is_solid(f, p);
}

// Assert the meshed field agrees with the analytic oracle at a point.
static void agree(const dsl::BuildBuffer& buf, Vector3 p, const char* msg) {
    bool oracle = dsl::field_is_solid(buf, p);
    bool meshv  = meshed(buf, p);
    if (oracle != meshv) {
        printf("FAIL: %s (oracle=%d mesh=%d at %.2f,%.2f,%.2f)\n",
               msg, (int)oracle, (int)meshv, p.x, p.y, p.z);
        ++g_failures;
    }
}

// ---- Test 1: box is a real box, not sphere soup --------------------------
static void test_box_matches_oracle() {
    dsl::DslState s; s.beginVoxels(0.1f); s.fill(0);
    s.box({0,0,0}, {0.5f,0.5f,0.5f}, dsl::CsgOp::Union); s.endVoxels();

    // Interior, near faces, corners, just-outside: all must match the oracle.
    agree(s.buffer(), {0,0,0},        "box center solid");
    agree(s.buffer(), {0.45f,0,0},    "box near +x face solid");
    agree(s.buffer(), {0.45f,0.45f,0.45f}, "box near corner solid");
    agree(s.buffer(), {0.6f,0,0},     "box just outside +x empty");
    agree(s.buffer(), {0,0,0.7f},     "box just outside +z empty");
    // A box corner is the classic place sphere-soup fails (rounded / missing).
    agree(s.buffer(), {0.49f,0.49f,0.49f}, "box deep corner solid");
}

// ---- Test 2: G2 scale fix ------------------------------------------------
// scale(2,2,2); sphere(c=0, r=0.5) must mesh as radius 1.0 (matches oracle,
// which already respects scale). Pre-fix the sphere brush dropped scale, so the
// meshed radius stayed 0.5 and points in (0.5,1.0) read empty though the oracle
// says solid -> this test FAILS before the fix.
static dsl::BuildBuffer scaled_sphere() {
    dsl::DslState s; s.beginVoxels(0.1f); s.fill(0);
    s.scale(2,2,2);
    s.sphere({0,0,0}, 0.5f, dsl::CsgOp::Union);
    s.endVoxels();
    return s.buffer();
}
static void test_scaled_sphere_radius() {
    dsl::BuildBuffer b = scaled_sphere();
    // Oracle: scaled radius is 1.0.
    CHECK(dsl::field_is_solid(b, {0.9f,0,0}), "oracle: scaled sphere solid at r=0.9");
    CHECK(!dsl::field_is_solid(b, {1.1f,0,0}), "oracle: scaled sphere empty at r=1.1");
    // Meshed field must agree (this is the G2 regression).
    agree(b, {0,0,0},    "scaled sphere center solid");
    agree(b, {0.4f,0,0}, "scaled sphere solid inside raw radius");
    agree(b, {0.9f,0,0}, "scaled sphere solid at 0.9 (picks up 2x scale)");
    agree(b, {1.1f,0,0}, "scaled sphere empty beyond scaled radius");
}

// ---- Test 3: ordered CSG -------------------------------------------------
// add A (sphere @0 r1) -> subtract B (sphere @1 r1) -> add C (sphere @1 r0.6).
// Correct CSG = (A - B) u C : C must SURVIVE where it overlaps B. The old
// two-bucket pipeline computes (A u C) - B and removes C there.
static dsl::BuildBuffer ordered_abc() {
    dsl::DslState s; s.beginVoxels(0.1f); s.fill(0);
    s.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);       // A
    s.sphere({1,0,0}, 1.0f, dsl::CsgOp::Difference);  // - B
    s.sphere({1,0,0}, 0.6f, dsl::CsgOp::Union);       // + C (re-adds the center of B)
    s.endVoxels();
    return s.buffer();
}
static void test_ordered_csg() {
    dsl::BuildBuffer b = ordered_abc();
    // Oracle establishes the truth.
    CHECK(dsl::field_is_solid(b, {1.0f,0,0}),  "oracle: C re-adds at B center");
    CHECK(dsl::field_is_solid(b, {-0.9f,0,0}), "oracle: A survives away from B");
    // Inside A and inside B but OUTSIDE C: the carve must win -> empty.
    // {0.35,0,0}: |p-0|=0.35 (in A r1), |p-1|=0.65 (in B r1, outside C r0.6).
    CHECK(!dsl::field_is_solid(b, {0.35f,0,0}), "oracle: B carve removes A where C absent");

    agree(b, {-0.9f,0,0}, "ordered: A solid away from B");
    agree(b, {1.0f,0,0},  "ordered: C survives the earlier subtract (the key case)");
    agree(b, {0.35f,0,0}, "ordered: B carve still removes A where C absent");
}

// ---- Test 4: capsule (sdSegment - r) matches the oracle ------------------
// A capsule is a segment a->b skinned with radius r. The voxel `capsule()` brush
// lowers to a FatPrim evaluated as sdSegment(p,a,b) - r in brush-local space.
// Probed meshed occupancy must agree with the analytic capsule oracle, including
// the cap hemispheres at the segment ends and the cylindrical mid-section.
static void test_capsule_matches_oracle() {
    dsl::DslState s; s.beginVoxels(0.1f); s.fill(0);
    // segment from (-1,0,0) to (1,0,0), radius 0.5.
    s.capsule({-1,0,0}, {1,0,0}, 0.5f, dsl::CsgOp::Union); s.endVoxels();

    agree(s.buffer(), {0,0,0},      "capsule mid solid");
    agree(s.buffer(), {0,0.4f,0},   "capsule near wall solid");
    agree(s.buffer(), {0,0.6f,0},   "capsule just outside wall empty");
    agree(s.buffer(), {1.0f,0,0},   "capsule end-pole solid");
    agree(s.buffer(), {1.4f,0,0},   "capsule end-cap hemisphere solid");
    agree(s.buffer(), {1.6f,0,0},   "capsule beyond end-cap empty");
    agree(s.buffer(), {-1.4f,0,0},  "capsule start-cap hemisphere solid");
}

// ---- Test 5: capsule respects scale (brush-local invTransform) -----------
// scale(2,2,2); capsule(seg, r=0.25) must mesh with the segment AND radius scaled
// 2x (the oracle already respects scale). Probes the G2-style brush-local rule.
static void test_scaled_capsule() {
    dsl::DslState s; s.beginVoxels(0.1f); s.fill(0);
    s.scale(2,2,2);
    s.capsule({-0.5f,0,0}, {0.5f,0,0}, 0.25f, dsl::CsgOp::Union);
    s.endVoxels();
    // Scaled: segment ends at +-1, radius 0.5.
    CHECK(dsl::field_is_solid(s.buffer(), {0,0.4f,0}),  "oracle: scaled capsule wall solid at 0.4");
    CHECK(!dsl::field_is_solid(s.buffer(), {0,0.6f,0}), "oracle: scaled capsule empty at 0.6");
    agree(s.buffer(), {0,0,0},     "scaled capsule mid solid");
    agree(s.buffer(), {0,0.4f,0},  "scaled capsule wall solid (picks up 2x scale)");
    agree(s.buffer(), {0,0.6f,0},  "scaled capsule empty beyond scaled radius");
    agree(s.buffer(), {1.4f,0,0},  "scaled capsule end-cap solid");
}

// ---- Test 6: cylinder (sdCappedCone, equal radii) ------------------------
// A cylinder is a capped cone with r0==r1: flat discs at the ends, straight wall.
static void test_cylinder_matches_oracle() {
    dsl::DslState s; s.beginVoxels(0.1f); s.fill(0);
    s.cylinder({0,-1,0}, {0,1,0}, 0.5f, dsl::CsgOp::Union); s.endVoxels();

    agree(s.buffer(), {0,0,0},     "cylinder mid solid");
    agree(s.buffer(), {0.4f,0,0},  "cylinder near wall solid");
    agree(s.buffer(), {0.6f,0,0},  "cylinder just outside wall empty");
    agree(s.buffer(), {0,0.9f,0},  "cylinder near top-cap solid (flat)");
    agree(s.buffer(), {0,1.1f,0},  "cylinder beyond flat cap empty (NOT rounded)");
    agree(s.buffer(), {0.4f,0.9f,0},"cylinder near top edge solid");
    agree(s.buffer(), {0.6f,1.1f,0},"cylinder past top edge empty");
}

// ---- Test 7: cone (r1=0) tapers to a point -------------------------------
static void test_cone_tapers() {
    dsl::DslState s; s.beginVoxels(0.1f); s.fill(0);
    s.cone({0,-1,0}, {0,1,0}, 0.8f, 0.0f, dsl::CsgOp::Union); s.endVoxels();

    agree(s.buffer(), {0,-0.9f,0},  "cone near wide base solid");
    agree(s.buffer(), {0.7f,-0.9f,0},"cone wide base near wall solid");
    agree(s.buffer(), {0.7f,0.9f,0},"cone wide radius near tip empty (tapered)");
    agree(s.buffer(), {0,0.9f,0},   "cone axis near tip solid");
    agree(s.buffer(), {0,1.1f,0},   "cone beyond tip empty");
}

// ---- Test 8: ordered CSG with a capsule in the stage list ----------------
// add sphere @0 r1 -> subtract capsule through it keeps correct result: a sample
// inside the sphere but inside the carving capsule is removed; outside the capsule
// survives.
static void test_ordered_csg_capsule() {
    dsl::DslState s; s.beginVoxels(0.1f); s.fill(0);
    s.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);                 // A
    s.capsule({-2,0,0}, {2,0,0}, 0.3f, dsl::CsgOp::Difference); // - B (bores a hole through)
    s.endVoxels();
    // Oracle: on the bore axis the capsule removes the sphere; off-axis survives.
    CHECK(!dsl::field_is_solid(s.buffer(), {0,0,0}),    "oracle: capsule bore removes sphere center");
    CHECK(dsl::field_is_solid(s.buffer(), {0,0.7f,0}),  "oracle: sphere survives off the bore");

    agree(s.buffer(), {0,0,0},    "ordered: capsule carve removes center");
    agree(s.buffer(), {0,0.7f,0}, "ordered: sphere survives off the capsule bore");
    agree(s.buffer(), {0,0.2f,0}, "ordered: just inside bore removed");
}

int main() {
    test_box_matches_oracle();
    test_scaled_sphere_radius();
    test_ordered_csg();
    test_capsule_matches_oracle();
    test_scaled_capsule();
    test_cylinder_matches_oracle();
    test_cone_tapers();
    test_ordered_csg_capsule();
    if (g_failures == 0) printf("ALL PASS\n");
    return g_failures ? 1 : 0;
}
