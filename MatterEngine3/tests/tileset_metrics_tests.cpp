// Unit tests for the pose-delta metric (settle-tick-optimizer.md §II.3,
// bake-lab-plan task 5.3): identity, known offset, torus wrap, fail-closed
// count mismatch, sphere-orientation skip, teleports, gate boundary.
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "tileset_metrics.h"

#include "check.h"

using tileset::ColliderFit;
using tileset::ColliderType;
using tileset::Pose;
using tileset::PoseDeltaGate;
using tileset::PoseDeltaReport;
using tileset::PoseMetricInfo;
using tileset::PoseMetricMap;
using tileset::SettledInstance;
using tileset::SettledTorus;

static bool near_f(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

// One-instance torus at (x, y, z) with quaternion (qx,qy,qz,qw), cfg.size=2
// (=> torus extent 4*2 = 8 m).
static SettledTorus make_torus(std::initializer_list<SettledInstance> insts) {
    SettledTorus t;
    t.cfg.size = 2.0f;
    for (const SettledInstance& i : insts) t.instances.push_back(i);
    return t;
}

static SettledInstance inst(uint64_t hash, float x, float y, float z,
                            float qx = 0, float qy = 0, float qz = 0,
                            float qw = 1) {
    SettledInstance i;
    i.child_hash = hash;
    i.pose = Pose{ x, y, z, qx, qy, qz, qw };
    return i;
}

static void test_identity() {
    SettledTorus a = make_torus({
        inst(1, 0.5f, 0.1f, 0.5f),
        inst(2, 3.0f, 0.0f, 7.5f, 0, 0.7071068f, 0, 0.7071068f),
        inst(3, 6.25f, 0.2f, 1.0f),
    });
    PoseDeltaReport r = tileset::compare_settled(a, a);
    CHECK(r.pass, "identity: pass");
    CHECK(r.median == 0 && r.p95 == 0 && r.max == 0, "identity: position deltas all zero");
    CHECK(r.p95_orient_deg == 0, "identity: orientation delta zero");
    CHECK(r.teleports == 0, "identity: no teleports");
    CHECK(r.compared == 3 && r.skipped == 0, "identity: compared=3, skipped=0");
}

static void test_known_offset() {
    // 0.2 m displacement, characteristic size 2.0 m -> normalized 0.1.
    SettledTorus a = make_torus({ inst(42, 1.0f, 0.0f, 1.0f) });
    SettledTorus b = make_torus({ inst(42, 1.2f, 0.0f, 1.0f) });
    PoseMetricMap map;
    map[42] = PoseMetricInfo{ 2.0f, 1.0f };
    PoseDeltaReport r = tileset::compare_settled(a, b, map);
    CHECK(near_f(r.p95, 0.1f), "offset: p95 = 0.2/2.0 = 0.1");
    CHECK(near_f(r.median, 0.1f) && near_f(r.max, 0.1f), "offset: median/max = 0.1");
    CHECK(r.teleports == 0 && r.pass, "offset: within gate, pass");

    // Same displacement, unknown hash -> 0.5 m fallback -> normalized 0.4.
    PoseDeltaReport rf = tileset::compare_settled(a, b);
    CHECK(near_f(rf.p95, 0.4f), "offset: 0.5 m fallback char size -> 0.4");
}

static void test_torus_wrap() {
    // cfg.size=2 -> extent 8. x: 0.05 vs 7.95 is 0.1 m across the seam,
    // not 7.9 m. Same on z.
    SettledTorus a = make_torus({ inst(7, 0.05f, 0.0f, 7.95f) });
    SettledTorus b = make_torus({ inst(7, 7.95f, 0.0f, 0.05f) });
    PoseMetricMap map;
    map[7] = PoseMetricInfo{ 1.0f, 1.0f };
    PoseDeltaReport r = tileset::compare_settled(a, b, map);
    float expect = std::sqrt(0.1f * 0.1f + 0.1f * 0.1f);  // ~0.1414
    CHECK(near_f(r.max, expect, 1e-3f), "wrap: seam displacement is ~0.14, not ~8");
    CHECK(r.teleports == 0, "wrap: no teleport across the seam");
    CHECK(r.pass, "wrap: pass");
}

static void test_count_mismatch() {
    SettledTorus a = make_torus({ inst(1, 1, 0, 1), inst(2, 2, 0, 2), inst(3, 3, 0, 3) });
    SettledTorus b = make_torus({ inst(1, 1, 0, 1), inst(2, 2, 0, 2) });
    PoseDeltaReport r = tileset::compare_settled(a, b);
    CHECK(!r.pass, "mismatch: fails closed even with zero deltas");
    CHECK(r.skipped == 1, "mismatch: 1 unpaired instance in skipped");
    CHECK(r.compared == 2, "mismatch: common prefix still compared");
    CHECK(r.median == 0 && r.p95 == 0, "mismatch: paired deltas still zero");

    // Symmetric: candidate longer than baseline.
    PoseDeltaReport r2 = tileset::compare_settled(b, a);
    CHECK(!r2.pass && r2.skipped == 1 && r2.compared == 2,
          "mismatch: candidate-longer also fails closed");
}

static void test_sphere_orientation_skip() {
    // A rotated sphere (90 deg about y) with orient_weight 0 contributes no
    // orientation sample: p95_orient stays 0 and the gate passes.
    SettledTorus a = make_torus({ inst(9, 1, 0, 1, 0, 0, 0, 1) });
    SettledTorus b = make_torus({ inst(9, 1, 0, 1, 0, 0.7071068f, 0, 0.7071068f) });

    ColliderFit sphere;
    sphere.type = ColliderType::Sphere;
    sphere.radius = 0.3f;
    PoseMetricMap map;
    map[9] = tileset::metric_info_of(sphere);
    CHECK(map[9].orient_weight == 0.0f, "sphere: orient_weight_of = 0");
    CHECK(near_f(map[9].char_size, 0.3f), "sphere: char_size_of = radius");

    PoseDeltaReport r = tileset::compare_settled(a, b, map);
    CHECK(r.p95_orient_deg == 0, "sphere: 90-deg rotation ignored");
    CHECK(r.pass, "sphere: pass despite rotation");

    // Control: same rotation with full weight would be ~90 deg and fail.
    map[9].orient_weight = 1.0f;
    PoseDeltaReport rc = tileset::compare_settled(a, b, map);
    CHECK(near_f(rc.p95_orient_deg, 90.0f, 0.1f), "control: full weight sees ~90 deg");
    CHECK(!rc.pass, "control: 90 deg > 15-deg gate fails");
}

static void test_half_weighting() {
    // Near-isotropic box (ratio 1.1 < 1.2): weight 0.5 halves the reported angle.
    ColliderFit box;
    box.type = ColliderType::Box;
    box.half_extent[0] = 0.55f; box.half_extent[1] = 0.5f; box.half_extent[2] = 0.5f;
    CHECK(tileset::orient_weight_of(box) == 0.5f, "half-weight: ratio 1.1 -> 0.5");
    CHECK(near_f(tileset::char_size_of(box), 0.55f), "half-weight: box char size = max extent");

    ColliderFit slab = box;
    slab.half_extent[0] = 1.0f;  // ratio 2.0
    CHECK(tileset::orient_weight_of(slab) == 1.0f, "half-weight: ratio 2.0 -> 1.0");

    SettledTorus a = make_torus({ inst(5, 1, 0, 1, 0, 0, 0, 1) });
    // 20 deg about y: q = (0, sin10, 0, cos10).
    float h = 10.0f * 3.14159265f / 180.0f;
    SettledTorus b = make_torus({ inst(5, 1, 0, 1, 0, std::sin(h), 0, std::cos(h)) });
    PoseMetricMap map;
    map[5] = tileset::metric_info_of(box);
    PoseDeltaReport r = tileset::compare_settled(a, b, map);
    CHECK(near_f(r.p95_orient_deg, 10.0f, 0.05f), "half-weight: 20 deg reported as 10");
    CHECK(r.pass, "half-weight: 10 < 15-deg gate passes");
}

static void test_teleport() {
    // 1.5x characteristic size: teleports=1, pass=false even under p95 gate?
    // (single instance -> p95 = 1.5 too, but the teleport count alone must fail).
    SettledTorus a = make_torus({ inst(3, 1.0f, 0, 1.0f) });
    SettledTorus b = make_torus({ inst(3, 2.5f, 0, 1.0f) });
    PoseMetricMap map;
    map[3] = PoseMetricInfo{ 1.0f, 1.0f };
    PoseDeltaReport r = tileset::compare_settled(a, b, map);
    CHECK(r.teleports == 1, "teleport: normalized 1.5 counted");
    CHECK(near_f(r.max, 1.5f), "teleport: max = 1.5");
    CHECK(!r.pass, "teleport: fails the gate");

    // A permissive gate that allows one teleport (and a wide p95) passes.
    PoseDeltaGate loose;
    loose.p95 = 2.0f; loose.teleports = 1; loose.p95_orient_deg = 180.0f;
    PoseDeltaReport rl = tileset::compare_settled(a, b, map, loose);
    CHECK(rl.pass, "teleport: gate.teleports=1 admits it");
}

static void test_gate_boundary() {
    // p95 exactly at the gate value passes (<=); just above fails.
    SettledTorus a = make_torus({ inst(8, 1.0f, 0, 1.0f) });
    SettledTorus b = make_torus({ inst(8, 1.15f, 0, 1.0f) });  // 0.15 m
    PoseMetricMap map;
    map[8] = PoseMetricInfo{ 1.0f, 1.0f };
    PoseDeltaGate gate;  // p95 = 0.15
    PoseDeltaReport r = tileset::compare_settled(a, b, map, gate);
    CHECK(near_f(r.p95, 0.15f), "boundary: p95 = 0.15");
    CHECK(r.pass, "boundary: p95 == gate passes");

    SettledTorus b2 = make_torus({ inst(8, 1.16f, 0, 1.0f) });
    PoseDeltaReport r2 = tileset::compare_settled(a, b2, map, gate);
    CHECK(!r2.pass, "boundary: p95 just above gate fails");
}

static void test_percentiles() {
    // 20 instances, one displaced by 1.9 (teleport+outlier), rest zero.
    // Nearest-rank p95 over N=20 = 19th value (index ceil(0.95*20)-1 = 18)
    // = 0 with a single outlier at index 19; median = index 9 = 0.
    SettledTorus a, b;
    a.cfg.size = 2.0f; b.cfg.size = 2.0f;
    for (int i = 0; i < 20; ++i) {
        a.instances.push_back(inst((uint64_t)i, (float)i * 0.3f, 0, 0));
        float dx = (i == 13) ? 1.9f : 0.0f;
        b.instances.push_back(inst((uint64_t)i, (float)i * 0.3f + dx, 0, 0));
    }
    PoseMetricMap map;
    for (int i = 0; i < 20; ++i) map[(uint64_t)i] = PoseMetricInfo{ 1.0f, 1.0f };
    PoseDeltaReport r = tileset::compare_settled(a, b, map);
    CHECK(r.median == 0 && r.p95 == 0, "percentiles: single outlier below p95 rank");
    CHECK(near_f(r.max, 1.9f), "percentiles: max sees the outlier");
    CHECK(r.teleports == 1 && !r.pass, "percentiles: teleport count still fails it");
}

static void test_hull_char_size() {
    ColliderFit hull;
    hull.type = ColliderType::Hull;
    hull.center[0] = 1.0f; hull.center[1] = 0.0f; hull.center[2] = 0.0f;
    // Points at distance 0.5 and 1.25 from center.
    hull.hull_points = { 1.5f, 0, 0,   2.25f, 0, 0,   1.0f, 0.5f, 0 };
    CHECK(near_f(tileset::char_size_of(hull), 1.25f), "hull: bounding radius from center");

    ColliderFit empty_hull;
    empty_hull.type = ColliderType::Hull;
    CHECK(tileset::char_size_of(empty_hull) == 0.5f, "hull: degenerate -> 0.5 m fallback");

    ColliderFit cap;
    cap.type = ColliderType::Capsule;
    cap.radius = 0.2f; cap.seg_half = 0.4f;
    CHECK(near_f(tileset::char_size_of(cap), 0.2f), "capsule: char size = radius");
    CHECK(tileset::orient_weight_of(cap) == 1.0f, "capsule: 3.0 extent ratio -> weight 1");
}

static void test_scale_normalization() {
    // Two instances SHARING one child_hash (map keyed by plain hash, one
    // entry) at scales 0.5 and 2.0, same 0.2 m absolute displacement. The
    // effective characteristic size is char_size * instance scale, so the
    // normalized deltas differ 4x: 0.2/(1*0.5)=0.4 vs 0.2/(1*2.0)=0.1.
    SettledInstance a0 = inst(11, 1.0f, 0, 1.0f);  a0.scale = 0.5f;
    SettledInstance a1 = inst(11, 4.0f, 0, 1.0f);  a1.scale = 2.0f;
    SettledInstance b0 = a0;  b0.pose.px += 0.2f;
    SettledInstance b1 = a1;  b1.pose.px += 0.2f;
    SettledTorus a = make_torus({ a0, a1 });
    SettledTorus b = make_torus({ b0, b1 });

    PoseMetricMap map;
    map[11] = PoseMetricInfo{ 1.0f, 1.0f };  // one UNSCALED entry serves both
    CHECK(map.size() == 1, "scale: map keyed by plain hash (one shared entry)");

    PoseDeltaReport r = tileset::compare_settled(a, b, map);
    // N=2 nearest-rank: median = index 0 (smaller), p95/max = index 1.
    CHECK(near_f(r.median, 0.1f), "scale: scale-2 instance normalized to 0.1");
    CHECK(near_f(r.max, 0.4f), "scale: scale-0.5 instance normalized to 0.4");
    CHECK(near_f(r.max / r.median, 4.0f, 1e-3f),
          "scale: same displacement, 4x normalized-delta ratio");

    // Non-positive scale falls back to 1.0 (plain char_size normalization).
    SettledInstance z0 = inst(11, 1.0f, 0, 1.0f);  z0.scale = 0.0f;
    SettledInstance z1 = z0;  z1.pose.px += 0.2f;
    PoseDeltaReport rz = tileset::compare_settled(make_torus({ z0 }),
                                                  make_torus({ z1 }), map);
    CHECK(near_f(rz.max, 0.2f), "scale: scale<=0 falls back to 1.0");
}

int main() {
    test_identity();
    test_known_offset();
    test_torus_wrap();
    test_count_mismatch();
    test_sphere_orientation_skip();
    test_half_weighting();
    test_teleport();
    test_gate_boundary();
    test_percentiles();
    test_hull_char_size();
    test_scale_normalization();
    printf("tileset_metrics_tests: ");
    return check_summary();
}
