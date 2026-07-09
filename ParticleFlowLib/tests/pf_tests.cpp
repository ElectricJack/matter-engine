// ParticleFlowLib test binary. Plain assert() + printf, built with ASan+UBSan.
#include "particle_flow.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

using namespace pf;

static void test_rng_determinism() {
    Rng a(42), b(42), c(43);
    bool diverged = false;
    for (int i = 0; i < 1000; ++i) {
        uint64_t va = a.next_u64();
        assert(va == b.next_u64() && "same seed must produce identical stream");
        if (va != c.next_u64()) diverged = true;
    }
    assert(diverged && "different seeds must diverge");
    Rng d(7);
    for (int i = 0; i < 10000; ++i) {
        float u = d.next_unit();
        assert(u >= 0.0f && u < 1.0f);
        V3 s = d.unit_sphere();
        assert(std::fabs(length(s) - 1.0f) < 1e-4f);
    }
    printf("  rng determinism OK\n");
}

static void test_v3_math() {
    V3 v = normalize(V3{3, 0, 4});
    assert(std::fabs(v.x - 0.6f) < 1e-6f && std::fabs(v.z - 0.8f) < 1e-6f);
    V3 z = normalize(V3{0, 0, 0});           // zero-safe
    assert(z.x == 0 && z.y == 0 && z.z == 0);
    V3 c = cross(V3{1,0,0}, V3{0,1,0});
    assert(std::fabs(c.z - 1.0f) < 1e-6f);
    printf("  v3 math OK\n");
}

static void test_spatial_hash_vs_brute_force() {
    Rng rng(99);
    std::vector<V3> pts;
    for (int i = 0; i < 500; ++i)
        pts.push_back({rng.range(-5,5), rng.range(-5,5), rng.range(-5,5)});
    SpatialHash h(0.7f);
    for (uint32_t i = 0; i < pts.size(); ++i) h.insert(pts[i], i);
    assert(h.size() == pts.size());

    const V3 centers[] = {{0,0,0}, {2.5f,-1,3}, {-4.9f,4.9f,0}};
    const float radii[] = {0.3f, 1.1f, 4.0f};
    for (V3 q : centers) for (float r : radii) {
        std::vector<uint32_t> got;
        h.query(q, r, [&](uint32_t idx, V3, float d2) {
            assert(d2 <= r * r + 1e-5f);
            got.push_back(idx);
        });
        size_t expect = 0;
        for (uint32_t i = 0; i < pts.size(); ++i) {
            V3 d = pts[i] - q;
            if (dot(d, d) <= r * r) ++expect;
        }
        assert(got.size() == expect && "hash query must match brute force");
    }
    // Determinism: same query twice -> identical visit order.
    std::vector<uint32_t> o1, o2;
    h.query({0,0,0}, 3.0f, [&](uint32_t i, V3, float){ o1.push_back(i); });
    h.query({0,0,0}, 3.0f, [&](uint32_t i, V3, float){ o2.push_back(i); });
    assert(o1 == o2);
    printf("  spatial hash OK (%zu pts)\n", pts.size());
}

static void test_spatial_hash_no_duplicates() {
    // Regression test: injective packed-coordinate key must prevent
    // duplicate query results from cell collisions. Dense grid [-16,16]^3
    // at step 4 with cell_size 1.0 = 9^3 = 729 points.
    std::vector<V3> pts;
    for (int x = -16; x <= 16; x += 4) {
        for (int y = -16; y <= 16; y += 4) {
            for (int z = -16; z <= 16; z += 4) {
                pts.push_back({(float)x, (float)y, (float)z});
            }
        }
    }
    SpatialHash h(1.0f);
    for (uint32_t i = 0; i < pts.size(); ++i) h.insert(pts[i], i);
    assert(h.size() == pts.size());

    Rng rng(42);
    for (int q_iter = 0; q_iter < 50; ++q_iter) {
        V3 center{rng.range(-16, 16), rng.range(-16, 16), rng.range(-16, 16)};
        const float radius = 3.0f;

        // Collect query results.
        std::vector<uint32_t> got;
        h.query(center, radius, [&](uint32_t idx, V3, float d2) {
            got.push_back(idx);
        });

        // Verify count matches brute force.
        size_t expect_count = 0;
        for (uint32_t i = 0; i < pts.size(); ++i) {
            V3 d = pts[i] - center;
            if (dot(d, d) <= radius * radius) ++expect_count;
        }
        assert(got.size() == expect_count && "query count must match brute force");

        // Verify no duplicates: sort and check adjacent pairs.
        std::sort(got.begin(), got.end());
        for (size_t i = 1; i < got.size(); ++i) {
            assert(got[i] != got[i - 1] && "no point id can appear twice in query results");
        }
    }
    printf("  spatial hash no-duplicates OK (%zu pts, 50 queries)\n", pts.size());
}

static V3 slot_pos(Sim& s, uint32_t i) {
    float* p = s.pos_data();
    return {p[3*i], p[3*i+1], p[3*i+2]};
}
static V3 slot_vel(Sim& s, uint32_t i) {
    float* v = s.vel_data();
    return {v[3*i], v[3*i+1], v[3*i+2]};
}

static SimConfig base_cfg() {
    SimConfig c;
    c.seed = 1234;
    c.dt = 1.0f;
    c.max_turn_rate = 0.1f;
    c.deposit_every = 0.05f;
    c.attributes = {"radius"};
    EmitterConfig e;
    e.shape = 1; e.center = {0,0,0}; e.axis = {0,1,0}; e.radius = 0.3f;
    e.rate = 0.5f; e.vel0 = 0.05f; e.jitter = 0.01f;
    e.attr_init = {0.1f};
    c.emitters = {e};
    FieldConfig up; up.type = FieldType::Bias; up.mode = FieldMode::Steer;
    up.dir = {0,1,0}; up.weight = 1.0f;
    c.fields = {up};
    c.speed_target = 0.05f; c.speed_relax = 0.2f;
    return c;
}

static bool sims_equal(Sim& a, Sim& b) {
    if (a.slot_count() != b.slot_count()) return false;
    if (a.alive_count() != b.alive_count()) return false;
    if (a.deposited_count() != b.deposited_count()) return false;
    size_t n = a.slot_count();
    if (memcmp(a.pos_data(), b.pos_data(), n*3*sizeof(float))) return false;
    if (memcmp(a.vel_data(), b.vel_data(), n*3*sizeof(float))) return false;
    for (size_t i = 0; i < n; ++i)
        if (a.id_of((uint32_t)i) != b.id_of((uint32_t)i)) return false;
    return true;
}

static void test_sim_determinism_and_incremental() {
    Sim a(base_cfg()), b(base_cfg()), c(base_cfg());
    a.run(300);
    b.run(300);
    assert(sims_equal(a, b) && "same seed+config+ticks must be bit-identical");
    c.run(120); c.run(180);                      // N+M == N then M
    assert(sims_equal(a, c) && "incremental run must equal one-shot run");
    assert(a.alive_count() > 0 && a.deposited_count() > 100);
    printf("  sim determinism + incremental OK (%u alive, %zu deposited)\n",
           a.alive_count(), a.deposited_count());
}

static void test_gravity_parabola() {
    SimConfig c;
    c.seed = 5; c.dt = 0.05f; c.max_turn_rate = 10.0f; c.deposit_every = 1e9f;
    // no emitters/steer: single hand-emitted ballistic particle + force gravity
    FieldConfig g; g.type = FieldType::Bias; g.mode = FieldMode::Force;
    g.dir = {0,-1,0}; g.weight = 9.8f;
    c.fields = {g};
    Sim s(c);
    uint32_t slot = s.emit_particle({0,0,0}, {2,0,0}, nullptr);
    assert(slot != UINT32_MAX);
    const int K = 40;
    s.run(K);
    // Semi-implicit Euler: y = -g*dt^2 * K*(K+1)/2 ; x = vx*dt*K
    V3 p = slot_pos(s, slot);
    float ey = -9.8f * c.dt * c.dt * (K * (K + 1) / 2.0f);
    assert(std::fabs(p.y - ey) < 1e-3f && "gravity must integrate a parabola");
    assert(std::fabs(p.x - 2.0f * c.dt * K) < 1e-3f);
    printf("  gravity parabola OK (y=%.4f expect %.4f)\n", p.y, ey);
}

static void test_turn_clamp() {
    SimConfig c;
    c.seed = 6; c.dt = 1.0f; c.max_turn_rate = 0.1f; c.deposit_every = 1e9f;
    FieldConfig fx; fx.type = FieldType::Bias; fx.mode = FieldMode::Steer;
    fx.dir = {1,0,0}; fx.weight = 1.0f;
    c.fields = {fx};
    Sim s(c);
    uint32_t slot = s.emit_particle({0,0,0}, {0,1,0}, nullptr);  // moving +y, steered to +x
    V3 prev = slot_vel(s, slot);
    for (int k = 0; k < 30; ++k) {
        s.run(1);
        V3 cur = slot_vel(s, slot);
        float cang = dot(normalize(prev), normalize(cur));
        float ang = std::acos(std::fmin(1.0f, std::fmax(-1.0f, cang)));
        assert(ang <= c.max_turn_rate + 1e-4f && "per-tick turn must be clamped");
        assert(std::fabs(length(cur) - length(prev)) < 1e-5f && "steer preserves speed");
        prev = cur;
    }
    // pi/2 turn at 0.1 rad/tick -> aligned with +x after ~16 ticks (30 run)
    assert(normalize(prev).x > 0.999f && "must converge to steer target");
    printf("  turn clamp OK\n");
}

static void test_emission_cap_age_reuse() {
    SimConfig c = base_cfg();
    c.max_particles = 8; c.max_age = 5;
    c.emitters[0].rate = 2.5f;
    Sim s(c);
    s.run(3);
    assert(s.alive_count() == 7 && "rate 2.5/tick for 3 ticks = 7 particles");
    s.run(1);
    assert(s.alive_count() == 8 && "hard cap at max_particles");
    s.run(3);  // ticks 5..7: age-5 kills begin; slots recycle, ids stay unique
    assert(s.alive_count() <= 8);
    assert(s.slot_count() <= 8 && "slots must be reused after death");
    // Unique ids: collect and pairwise-compare
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < s.slot_count(); ++i) ids.push_back(s.id_of(i));
    for (size_t i = 0; i < ids.size(); ++i)
        for (size_t j = i + 1; j < ids.size(); ++j)
            assert(ids[i] != ids[j] && "live ids unique");
    printf("  emission/cap/age/reuse OK\n");
}

static void test_adhere_pulls_toward_deposited() {
    SimConfig c;
    c.seed = 11; c.dt = 1.0f; c.max_turn_rate = 0.5f; c.deposit_every = 1e9f;
    FieldConfig ad; ad.type = FieldType::Adhere; ad.mode = FieldMode::Steer;
    ad.weight = 1.0f; ad.radius = 2.0f; ad.surface_offset = 0.0f;
    c.fields = {ad};
    Sim s(c);
    // Wall of deposited points on the x=0 plane (via a throwaway particle's
    // spawn deposits): emit dead-still particles along the plane then kill.
    for (int i = -3; i <= 3; ++i) {
        uint32_t sl = s.emit_particle({0, (float)i * 0.3f, 0}, {0,0,0}, nullptr);
        s.kill(sl);
    }
    assert(s.deposited_count() == 7);
    // A mover at x=1 heading +y must curve toward the wall (x decreasing).
    uint32_t m = s.emit_particle({1, 0, 0}, {0, 0.1f, 0}, nullptr);
    s.run(20);
    assert(slot_pos(s, m).x < 0.9f && "adhere must pull toward deposited set");
    printf("  adhere OK (x=%.3f)\n", slot_pos(s, m).x);
}

static void test_separate_pushes_apart() {
    SimConfig c;
    c.seed = 12; c.dt = 1.0f; c.max_turn_rate = 0.5f; c.deposit_every = 1e9f;
    FieldConfig sep; sep.type = FieldType::Separate; sep.mode = FieldMode::Steer;
    sep.weight = 1.0f; sep.radius = 1.0f;
    c.fields = {sep};
    Sim s(c);
    uint32_t a = s.emit_particle({-0.1f, 0, 0}, {0, 0.1f, 0}, nullptr);
    uint32_t b = s.emit_particle({ 0.1f, 0, 0}, {0, 0.1f, 0}, nullptr);
    float d0 = length(slot_pos(s, a) - slot_pos(s, b));
    s.run(15);
    float d1 = length(slot_pos(s, a) - slot_pos(s, b));
    assert(d1 > d0 + 0.05f && "separate must push live particles apart");
    printf("  separate OK (%.3f -> %.3f)\n", d0, d1);
}

static void test_attract_consume_and_kill() {
    SimConfig c;
    c.seed = 13; c.dt = 1.0f; c.max_turn_rate = 0.6f; c.deposit_every = 1e9f;
    c.speed_target = 0.2f; c.speed_relax = 1.0f;
    FieldConfig at; at.type = FieldType::Attract; at.mode = FieldMode::Steer;
    at.weight = 1.0f; at.influence = 10.0f; at.kill_radius = 0.25f;
    at.kill_on_consume = true;
    c.fields = {at};
    Sim s(c);
    float cloud[6] = { 3, 0, 0,   0, 3, 0 };
    s.set_attractors(cloud, 2);
    assert(s.attractors_remaining() == 2);
    uint32_t a = s.emit_particle({0.5f, 0, 0}, {0.2f, 0, 0}, nullptr);
    (void)a;
    s.run(40);
    assert(s.attractors_remaining() == 1 && "nearest attractor must be consumed");
    assert(s.alive_count() == 0 && "kill_on_consume terminates the strand");
    printf("  attract consume/kill OK\n");
}

static void test_surface_normal() {
    SimConfig c; c.seed = 14; c.deposit_every = 1e9f;
    Sim s(c);
    // Deposit a small patch around the origin in the y=0 plane.
    for (int i = -2; i <= 2; ++i) for (int j = -2; j <= 2; ++j) {
        uint32_t sl = s.emit_particle({i * 0.2f, 0, j * 0.2f}, {0,0,0}, nullptr);
        s.kill(sl);
    }
    bool ok = false;
    V3 n = s.surface_normal({0, 0.5f, 0}, 1.5f, &ok);
    assert(ok && "patch within radius");
    assert(n.y > 0.95f && "normal points outward (away from the deposit)");
    ok = true;
    s.surface_normal({100, 100, 100}, 1.0f, &ok);
    assert(!ok && "no neighbors -> ok=false");
    printf("  surface normal OK\n");
}

static void test_curl_is_deterministic_and_bounded() {
    SimConfig c; c.seed = 15; c.deposit_every = 1e9f; c.max_turn_rate = 0.5f;
    FieldConfig cu; cu.type = FieldType::Curl; cu.mode = FieldMode::Steer;
    cu.weight = 1.0f; cu.scale = 2.0f; cu.seed = 777;
    c.fields = {cu};
    Sim s1(c), s2(c);
    uint32_t p1 = s1.emit_particle({0,0,0}, {0.1f, 0, 0}, nullptr);
    uint32_t p2 = s2.emit_particle({0,0,0}, {0.1f, 0, 0}, nullptr);
    s1.run(50); s2.run(50);
    V3 a = slot_pos(s1, p1), b = slot_pos(s2, p2);
    assert(std::memcmp(&a, &b, sizeof(V3)) == 0 && "curl must be deterministic");
    assert(length(a) > 1e-3f && "particle must move");
    printf("  curl OK\n");
}

int main() {
    printf("pf_tests:\n");
    test_rng_determinism();
    test_v3_math();
    test_spatial_hash_vs_brute_force();
    test_spatial_hash_no_duplicates();
    test_sim_determinism_and_incremental();
    test_gravity_parabola();
    test_turn_clamp();
    test_emission_cap_age_reuse();
    test_adhere_pulls_toward_deposited();
    test_separate_pushes_apart();
    test_attract_consume_and_kill();
    test_surface_normal();
    test_curl_is_deterministic_and_bounded();
    printf("pf_tests: ALL OK\n");
    return 0;
}
