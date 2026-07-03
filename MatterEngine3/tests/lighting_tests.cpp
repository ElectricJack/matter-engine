// Task 1+2: World light list + ProbeVolume tests.
// Task 3: WorldTracer GL-free CPU tracer tests.
// Tests parse_lights, lights_fingerprint, read_manifest light-line skip,
// probe_volume save/load round-trip + rejection cases, and WorldTracer
// ray/instance intersection with transform correctness.
#include "world_lights.h"
#include "part_graph.h"
#include "probe_volume.h"
#include "world_tracer.h"

#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "part_asset_v2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++failures; } \
    else         { printf("ok:   %s\n", (msg)); } } while (0)

static bool feq(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) < eps;
}

// Write a file to a path.
static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

// ---- Test 1: defaults (no light lines) ----
static void test_defaults() {
    // Write a manifest with no light lines (only a module line and blank lines).
    std::string manifest = "# comment\n\nMeadow\n";
    write_file("sandbox/world.manifest", manifest);

    world_lights::WorldLights wl;
    std::string err;
    bool ok = world_lights::parse_lights("sandbox/world.manifest", wl, err);
    CHECK(ok, "defaults: parse_lights returns true");
    CHECK(err.empty(), "defaults: no error");

    // Verify exact default values from the header.
    CHECK(feq(wl.sun_dir[0], -0.45f), "defaults: sun_dir[0]");
    CHECK(feq(wl.sun_dir[1], -0.80f), "defaults: sun_dir[1]");
    CHECK(feq(wl.sun_dir[2], -0.35f), "defaults: sun_dir[2]");
    CHECK(feq(wl.sun_color[0], 2.2f),  "defaults: sun_color[0]");
    CHECK(feq(wl.sun_color[1], 2.05f), "defaults: sun_color[1]");
    CHECK(feq(wl.sun_color[2], 1.8f),  "defaults: sun_color[2]");
    CHECK(feq(wl.sky_color[0], 0.38f), "defaults: sky_color[0]");
    CHECK(feq(wl.sky_color[1], 0.43f), "defaults: sky_color[1]");
    CHECK(feq(wl.sky_color[2], 0.52f), "defaults: sky_color[2]");
    CHECK(wl.spots.empty(), "defaults: no spots");
}

// ---- Test 2: missing file yields defaults and true ----
static void test_missing_file() {
    world_lights::WorldLights wl;
    std::string err;
    bool ok = world_lights::parse_lights("sandbox/no_such_world.manifest", wl, err);
    CHECK(ok, "missing file: returns true");
    CHECK(err.empty(), "missing file: no error");
    CHECK(feq(wl.sun_dir[1], -0.80f), "missing file: default sun_dir[1]");
    CHECK(wl.spots.empty(), "missing file: no spots");
}

// ---- Test 3: full parse ----
static void test_parse() {
    // light sun 0 -1 0 3 3 3
    // light sky 0.2 0.3 0.5
    // light spot 0 5 0  0 -1 0  10 8 6  20 15 30
    std::string manifest =
        "# This is a comment\n"
        "light sun 0 -1 0 3 3 3\n"
        "light sky 0.2 0.3 0.5\n"
        "light spot 0 5 0  0 -1 0  10 8 6  20 15 30\n"
        "SomeModule\n";
    write_file("sandbox/parse_world.manifest", manifest);

    world_lights::WorldLights wl;
    std::string err;
    bool ok = world_lights::parse_lights("sandbox/parse_world.manifest", wl, err);
    CHECK(ok, "parse: returns true");
    CHECK(err.empty(), "parse: no error");

    // sun direction (0 -1 0) is already unit length; normalized stays the same.
    CHECK(feq(wl.sun_dir[0], 0.0f),  "parse: sun_dir[0]");
    CHECK(feq(wl.sun_dir[1], -1.0f), "parse: sun_dir[1]");
    CHECK(feq(wl.sun_dir[2], 0.0f),  "parse: sun_dir[2]");
    CHECK(feq(wl.sun_color[0], 3.0f), "parse: sun_color[0]");
    CHECK(feq(wl.sun_color[1], 3.0f), "parse: sun_color[1]");
    CHECK(feq(wl.sun_color[2], 3.0f), "parse: sun_color[2]");

    CHECK(feq(wl.sky_color[0], 0.2f), "parse: sky_color[0]");
    CHECK(feq(wl.sky_color[1], 0.3f), "parse: sky_color[1]");
    CHECK(feq(wl.sky_color[2], 0.5f), "parse: sky_color[2]");

    CHECK(wl.spots.size() == 1, "parse: 1 spot");
    if (wl.spots.size() == 1) {
        const auto& s = wl.spots[0];
        CHECK(feq(s.pos[0], 0.0f), "parse: spot pos[0]");
        CHECK(feq(s.pos[1], 5.0f), "parse: spot pos[1]");
        CHECK(feq(s.pos[2], 0.0f), "parse: spot pos[2]");

        // Direction (0 -1 0) is unit length; normalized.
        CHECK(feq(s.dir[0], 0.0f),  "parse: spot dir[0]");
        CHECK(feq(s.dir[1], -1.0f), "parse: spot dir[1]");
        CHECK(feq(s.dir[2], 0.0f),  "parse: spot dir[2]");

        CHECK(feq(s.color[0], 10.0f), "parse: spot color[0]");
        CHECK(feq(s.color[1], 8.0f),  "parse: spot color[1]");
        CHECK(feq(s.color[2], 6.0f),  "parse: spot color[2]");
        CHECK(feq(s.range, 20.0f), "parse: spot range");

        // cos(15 * PI/180) ~= 0.96593
        float expected_inner = std::cos(15.0f * (float)M_PI / 180.0f);
        CHECK(feq(s.cos_inner, expected_inner, 1e-5f), "parse: spot cos_inner");
        // cos(30 * PI/180) ~= 0.86603
        float expected_outer = std::cos(30.0f * (float)M_PI / 180.0f);
        CHECK(feq(s.cos_outer, expected_outer, 1e-5f), "parse: spot cos_outer");
    }
}

// ---- Test 4: repeated sun/sky lines — last one wins ----
static void test_last_wins() {
    std::string manifest =
        "light sun 1 0 0 1 1 1\n"
        "light sun 0 -1 0 3 3 3\n"
        "light sky 1 0 0\n"
        "light sky 0.2 0.3 0.5\n";
    write_file("sandbox/last_wins.manifest", manifest);

    world_lights::WorldLights wl;
    std::string err;
    bool ok = world_lights::parse_lights("sandbox/last_wins.manifest", wl, err);
    CHECK(ok, "last_wins: returns true");
    CHECK(feq(wl.sun_dir[1], -1.0f), "last_wins: sun_dir from last sun line");
    CHECK(feq(wl.sky_color[0], 0.2f), "last_wins: sky_color from last sky line");
}

// ---- Test 5: malformed line ----
static void test_malformed() {
    std::string manifest = "light spot 1 2\n";
    write_file("sandbox/bad_world.manifest", manifest);

    world_lights::WorldLights wl;
    std::string err;
    bool ok = world_lights::parse_lights("sandbox/bad_world.manifest", wl, err);
    CHECK(!ok, "malformed: returns false");
    CHECK(!err.empty(), "malformed: err non-empty");
}

// ---- Test 6: fingerprint differs when float changes, equal for identical ----
static void test_fingerprint() {
    world_lights::WorldLights a;
    world_lights::WorldLights b;

    uint64_t fa = world_lights::lights_fingerprint(a);
    uint64_t fb = world_lights::lights_fingerprint(b);
    CHECK(fa == fb, "fingerprint: identical lights give same hash");

    // Mutate one float in b.
    b.sun_dir[0] = -0.46f;
    uint64_t fc = world_lights::lights_fingerprint(b);
    CHECK(fa != fc, "fingerprint: differs after sun_dir change");

    // Reset sun_dir, change sky_color.
    b.sun_dir[0] = a.sun_dir[0];
    b.sky_color[2] = 0.99f;
    uint64_t fd = world_lights::lights_fingerprint(b);
    CHECK(fa != fd, "fingerprint: differs after sky_color change");

    // Add a spot to a fresh copy of a.
    world_lights::WorldLights c;
    world_lights::SpotLight sp;
    sp.pos[0] = 0.0f; sp.pos[1] = 5.0f; sp.pos[2] = 0.0f;
    sp.dir[0] = 0.0f; sp.dir[1] = -1.0f; sp.dir[2] = 0.0f;
    sp.color[0] = 1.0f; sp.color[1] = 1.0f; sp.color[2] = 1.0f;
    sp.range = 10.0f; sp.cos_inner = 0.96f; sp.cos_outer = 0.86f;
    c.spots.push_back(sp);
    uint64_t fe = world_lights::lights_fingerprint(c);
    CHECK(fa != fe, "fingerprint: differs when spot added");
}

// ---- Test 7: read_manifest skips light lines ----
// Writes a manifest with a light line and a module line; calls read_manifest and
// checks that only the module ends up in roots_out (not the light line).
static void test_read_manifest_skip() {
    std::string manifest =
        "# comment\n"
        "light sun 1 0 0 2 2 2\n"
        "MyModule\n"
        "light sky 0.1 0.2 0.3\n";
    // Write under a fake world directory structure.
    // read_manifest expects: world_data_dir + "/" + world + "/world.manifest"
    std::string world_dir = "sandbox/WorldData/TestWorld";
    // mkdir -p equivalent using system()
    system(("mkdir -p " + world_dir).c_str());
    write_file(world_dir + "/world.manifest", manifest);

    std::vector<part_graph::ChildRequest> roots;
    std::string err;
    bool ok = part_graph::PartGraph::read_manifest("sandbox/WorldData", "TestWorld", roots, err);
    CHECK(ok, "read_manifest skip: returns true");
    CHECK(err.empty(), "read_manifest skip: no error");
    CHECK(roots.size() == 1, "read_manifest skip: exactly 1 root (light lines ignored)");
    if (!roots.empty()) {
        CHECK(roots[0].module == "MyModule", "read_manifest skip: root is MyModule");
    }
}

// ---- Task 2 Tests: ProbeVolume save/load round-trip ----

// Build a deterministic 2x2x2 ProbeVolume with distinct values per cell.
static probe_volume::ProbeVolume make_test_volume() {
    probe_volume::ProbeVolume v;
    v.grid.origin[0] = 1.0f; v.grid.origin[1] = 2.0f; v.grid.origin[2] = 3.0f;
    v.grid.cell = 0.5f;
    v.grid.nx = 2; v.grid.ny = 2; v.grid.nz = 2;
    size_t n = v.cells(); // 8 cells
    v.ambient.resize(n * 4);
    v.dominant.resize(n * 4);
    for (size_t i = 0; i < n * 4; ++i) {
        v.ambient[i]  = (float)(i + 1) * 0.01f;
        v.dominant[i] = (float)(i + 1) * 0.02f;
    }
    return v;
}

// Test 8: save + load with matching fingerprint yields equal data.
static void test_probe_roundtrip() {
    probe_volume::ProbeVolume orig = make_test_volume();
    const uint64_t fp = 0xDEADBEEF12345678ULL;
    const std::string path = "sandbox/test.probes";

    bool saved = probe_volume::save_probes(path, orig, fp);
    CHECK(saved, "probe_roundtrip: save returns true");

    probe_volume::ProbeVolume loaded;
    bool loaded_ok = probe_volume::load_probes(path, loaded, fp);
    CHECK(loaded_ok, "probe_roundtrip: load returns true");

    // Grid equality.
    CHECK(loaded.grid.nx == orig.grid.nx, "probe_roundtrip: grid.nx");
    CHECK(loaded.grid.ny == orig.grid.ny, "probe_roundtrip: grid.ny");
    CHECK(loaded.grid.nz == orig.grid.nz, "probe_roundtrip: grid.nz");
    CHECK(feq(loaded.grid.cell, orig.grid.cell), "probe_roundtrip: grid.cell");
    CHECK(feq(loaded.grid.origin[0], orig.grid.origin[0]), "probe_roundtrip: origin[0]");
    CHECK(feq(loaded.grid.origin[1], orig.grid.origin[1]), "probe_roundtrip: origin[1]");
    CHECK(feq(loaded.grid.origin[2], orig.grid.origin[2]), "probe_roundtrip: origin[2]");

    // Blob equality.
    CHECK(loaded.ambient.size() == orig.ambient.size(), "probe_roundtrip: ambient.size");
    CHECK(loaded.dominant.size() == orig.dominant.size(), "probe_roundtrip: dominant.size");
    bool amb_eq = (loaded.ambient.size() == orig.ambient.size()) &&
                  (memcmp(loaded.ambient.data(), orig.ambient.data(),
                          orig.ambient.size() * sizeof(float)) == 0);
    CHECK(amb_eq, "probe_roundtrip: ambient memcmp");
    bool dom_eq = (loaded.dominant.size() == orig.dominant.size()) &&
                  (memcmp(loaded.dominant.data(), orig.dominant.data(),
                          orig.dominant.size() * sizeof(float)) == 0);
    CHECK(dom_eq, "probe_roundtrip: dominant memcmp");
    CHECK(loaded.valid(), "probe_roundtrip: loaded.valid()");
}

// Test 9: load with wrong fingerprint returns false.
static void test_probe_fingerprint_mismatch() {
    probe_volume::ProbeVolume orig = make_test_volume();
    const uint64_t fp = 0xAAAABBBBCCCCDDDDULL;
    const std::string path = "sandbox/test_fp.probes";

    probe_volume::save_probes(path, orig, fp);

    probe_volume::ProbeVolume loaded;
    bool ok = probe_volume::load_probes(path, loaded, fp ^ 1ULL);
    CHECK(!ok, "probe_fp_mismatch: load returns false for wrong fingerprint");
}

// Test 10: truncated file returns false.
static void test_probe_truncated() {
    probe_volume::ProbeVolume orig = make_test_volume();
    const uint64_t fp = 0x1234567890ABCDEFULL;
    const std::string path = "sandbox/test_trunc.probes";

    probe_volume::save_probes(path, orig, fp);

    // Truncate by 10 bytes.
    FILE* ff = std::fopen(path.c_str(), "r+b");
    if (ff) {
        std::fseek(ff, 0, SEEK_END);
        long sz = std::ftell(ff);
        std::fclose(ff);
        if (sz > 10) ::truncate(path.c_str(), sz - 10);
    }

    probe_volume::ProbeVolume loaded;
    bool ok = probe_volume::load_probes(path, loaded, fp);
    CHECK(!ok, "probe_truncated: load returns false for truncated file");
}

// Test 11: nonexistent path returns false.
static void test_probe_nonexistent() {
    probe_volume::ProbeVolume loaded;
    bool ok = probe_volume::load_probes("sandbox/no_such_file.probes", loaded, 0ULL);
    CHECK(!ok, "probe_nonexistent: load returns false");
}

// ================================================================
// Task 3: WorldTracer tests
// ================================================================

static const char* kTracerCache = "/tmp/world_tracer_tests_cache";
static const uint64_t kCubeHash = 0xAAAA000000000000ULL;

// ---- Cube triangle builder (unit cube spanning -0.5 to +0.5) ----
static Tri wt_make_tri(float ax, float ay, float az,
                        float bx, float by, float bz,
                        float cx, float cy, float cz) {
    Tri t;
    t.vertex0 = make_float3(ax, ay, az);
    t.vertex1 = make_float3(bx, by, bz);
    t.vertex2 = make_float3(cx, cy, cz);
    t.centroid = make_float3((ax+bx+cx)/3.f, (ay+by+cy)/3.f, (az+bz+cz)/3.f);
    return t;
}

static std::vector<Tri> unit_cube_tris() {
    // 12 triangles for a unit cube spanning [-0.5, 0.5]^3
    std::vector<Tri> v;
    const float n = -0.5f, p = 0.5f;
    // -Z face
    v.push_back(wt_make_tri(n,n,n, p,n,n, p,p,n));
    v.push_back(wt_make_tri(n,n,n, p,p,n, n,p,n));
    // +Z face
    v.push_back(wt_make_tri(p,n,p, n,n,p, n,p,p));
    v.push_back(wt_make_tri(p,n,p, n,p,p, p,p,p));
    // -X face
    v.push_back(wt_make_tri(n,n,p, n,n,n, n,p,n));
    v.push_back(wt_make_tri(n,n,p, n,p,n, n,p,p));
    // +X face
    v.push_back(wt_make_tri(p,n,n, p,n,p, p,p,p));
    v.push_back(wt_make_tri(p,n,n, p,p,p, p,p,n));
    // -Y face
    v.push_back(wt_make_tri(n,n,p, p,n,p, p,n,n));
    v.push_back(wt_make_tri(n,n,p, p,n,n, n,n,n));
    // +Y face
    v.push_back(wt_make_tri(n,p,n, p,p,n, p,p,p));
    v.push_back(wt_make_tri(n,p,n, p,p,p, n,p,p));
    return v;
}

static TriEx wt_make_triex(int mat_id) {
    TriEx ex;
    std::memset(&ex, 0, sizeof(TriEx));
    ex.materialId = mat_id;
    ex.tint = make_float4(1, 1, 1, 0);
    ex.ao0 = ex.ao1 = ex.ao2 = 1.0f;
    ex.N0 = ex.N1 = ex.N2 = make_float3(0, 1, 0);
    return ex;
}

static bool wt_write_cube_fixture() {
    mkdir(kTracerCache, 0755);
    mkdir((std::string(kTracerCache) + "/parts").c_str(), 0755);

    std::vector<Tri> tris = unit_cube_tris();
    std::vector<TriEx> triex(tris.size(), wt_make_triex(1));

    BLASManager blas;
    TLASManager tlas(16);
    BLASHandle h = blas.register_triangles(tris.data(), (int)tris.size(), triex.data());
    uint32_t idx = UINT32_MAX;
    const auto& entries = blas.get_entries();
    for (size_t k = 0; k < entries.size(); ++k)
        if (entries[k]->handle == h) { idx = (uint32_t)k; break; }
    if (idx == UINT32_MAX) return false;

    part_asset::LodLevel lvl;
    lvl.screen_size_threshold = 1e9f;
    lvl.blas_indices.push_back(idx);
    part_asset::LodLevels lods;
    lods.push_back(std::move(lvl));

    std::string path = std::string(kTracerCache) + "/" +
                       part_asset::cache_path_resolved(kCubeHash);
    return part_asset::save_v2(path, blas, tlas, nullptr, 0, lods, kCubeHash);
}

static float wt_identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

static void wt_translate(float m[16], float tx, float ty, float tz) {
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.f;
    m[3] = tx; m[7] = ty; m[11] = tz;
}

static void wt_scale_uniform(float m[16], float s) {
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = s;
    m[15] = 1.f;
}

// ---- Test 12: identity placement — ray hits front face of cube ----
static void test_tracer_identity_hit() {
    world_tracer::WorldTracer wt;
    std::string err;
    world_tracer::TraceInstance inst;
    inst.part_hash = kCubeHash;
    std::memcpy(inst.transform, wt_identity, 64);
    std::vector<world_tracer::TraceInstance> insts = {inst};
    bool built = wt.build(kTracerCache, insts, err);
    if (!built) printf("  build error: %s\n", err.c_str());
    CHECK(built, "tracer identity: build ok");
    if (!built) return;

    // Ray from (0,0,-5) toward +Z; cube front face is at z=-0.5
    const float O[3] = {0.f, 0.f, -5.f};
    const float D[3] = {0.f, 0.f,  1.f};
    world_tracer::Hit hit;
    bool ok = wt.trace(O, D, 100.f, hit);
    CHECK(ok, "tracer identity: trace returns true");
    CHECK(hit.t > 0.f, "tracer identity: hit.t > 0");
    // cube -Z face is at z=-0.5; ray origin z=-5; t should be ~4.5
    char msg[128];
    std::snprintf(msg, sizeof msg, "tracer identity: t ≈ 4.5 (got %.4f)", hit.t);
    CHECK(feq(hit.t, 4.5f, 0.05f), msg);
    // normal should face -Z (toward ray origin)
    std::snprintf(msg, sizeof msg, "tracer identity: normal[2] ≈ -1 (got %.4f)", hit.normal[2]);
    CHECK(feq(hit.normal[2], -1.0f, 0.1f), msg);
    CHECK(hit.material_id == 1, "tracer identity: material_id == 1");
}

// ---- Test 13: instance translated +10x — original ray misses, shifted ray hits ----
static void test_tracer_translated() {
    world_tracer::WorldTracer wt;
    std::string err;
    world_tracer::TraceInstance inst;
    inst.part_hash = kCubeHash;
    wt_translate(inst.transform, 10.f, 0.f, 0.f);
    std::vector<world_tracer::TraceInstance> insts = {inst};
    bool built = wt.build(kTracerCache, insts, err);
    CHECK(built, "tracer translated: build ok");
    if (!built) return;

    // Original ray misses (cube is at x=10)
    const float O0[3] = {0.f, 0.f, -5.f};
    const float D0[3] = {0.f, 0.f,  1.f};
    world_tracer::Hit miss_hit;
    bool missed = wt.trace(O0, D0, 100.f, miss_hit);
    CHECK(!missed, "tracer translated: original ray misses");

    // Ray at x=10 hits
    const float O1[3] = {10.f, 0.f, -5.f};
    const float D1[3] = { 0.f, 0.f,  1.f};
    world_tracer::Hit hit;
    bool ok = wt.trace(O1, D1, 100.f, hit);
    CHECK(ok, "tracer translated: shifted ray hits");
    char msg[128];
    std::snprintf(msg, sizeof msg, "tracer translated: t ≈ 4.5 (got %.4f)", hit.t);
    CHECK(feq(hit.t, 4.5f, 0.05f), msg);
}

// ---- Test 14: uniform scale 2× — t reflects scaled surface (t ≈ 4.0) ----
static void test_tracer_scaled_instance() {
    world_tracer::WorldTracer wt;
    std::string err;
    world_tracer::TraceInstance inst;
    inst.part_hash = kCubeHash;
    // Uniform scale 2: cube now spans [-1, 1]^3. Front face at z=-1.
    // Ray from (0,0,-5): t = 5 - 1 = 4.0 (world-space t preserved)
    wt_scale_uniform(inst.transform, 2.f);
    std::vector<world_tracer::TraceInstance> insts = {inst};
    bool built = wt.build(kTracerCache, insts, err);
    CHECK(built, "tracer scaled: build ok");
    if (!built) return;

    const float O[3] = {0.f, 0.f, -5.f};
    const float D[3] = {0.f, 0.f,  1.f};
    world_tracer::Hit hit;
    bool ok = wt.trace(O, D, 100.f, hit);
    CHECK(ok, "tracer scaled: trace returns true");
    char msg[128];
    std::snprintf(msg, sizeof msg, "tracer scaled: t ≈ 4.0 (got %.4f)", hit.t);
    CHECK(feq(hit.t, 4.0f, 0.1f), msg);
}

// ---- Test 15: occluded ----
static void test_tracer_occluded() {
    world_tracer::WorldTracer wt;
    std::string err;
    world_tracer::TraceInstance inst;
    inst.part_hash = kCubeHash;
    std::memcpy(inst.transform, wt_identity, 64);
    std::vector<world_tracer::TraceInstance> insts = {inst};
    bool built = wt.build(kTracerCache, insts, err);
    CHECK(built, "tracer occluded: build ok");
    if (!built) return;

    const float O[3] = {0.f, 0.f, -5.f};
    const float D[3] = {0.f, 0.f,  1.f};
    // max_t = 10: ray can reach (hits at t≈4.5) — occluded
    CHECK(wt.occluded(O, D, 10.f), "tracer occluded: max_t=10 is occluded");
    // max_t = 3: ray cannot reach cube — not occluded
    CHECK(!wt.occluded(O, D, 3.f), "tracer occluded: max_t=3 is not occluded");
}

// ---- Test 16: two instances — nearer one wins ----
static void test_tracer_two_instances_nearer_wins() {
    world_tracer::WorldTracer wt;
    std::string err;

    world_tracer::TraceInstance near_inst, far_inst;
    near_inst.part_hash = kCubeHash;
    far_inst.part_hash  = kCubeHash;
    // Near cube: identity (front face at z=-0.5, hit at t=4.5)
    std::memcpy(near_inst.transform, wt_identity, 64);
    // Far cube: translated +3z (front face at z=2.5, hit at t=7.5)
    wt_translate(far_inst.transform, 0.f, 0.f, 3.f);

    std::vector<world_tracer::TraceInstance> insts = {near_inst, far_inst};
    bool built = wt.build(kTracerCache, insts, err);
    CHECK(built, "tracer two-inst: build ok");
    if (!built) return;

    const float O[3] = {0.f, 0.f, -5.f};
    const float D[3] = {0.f, 0.f,  1.f};
    world_tracer::Hit hit;
    bool ok = wt.trace(O, D, 100.f, hit);
    CHECK(ok, "tracer two-inst: trace hits");
    char msg[128];
    std::snprintf(msg, sizeof msg, "tracer two-inst: nearer wins (t ≈ 4.5, got %.4f)", hit.t);
    CHECK(feq(hit.t, 4.5f, 0.1f), msg);
}

// ================================================================
// Task 3 gap: compositional child expansion
// CHILD part (0xC1): unit cube, no flat artifact.
// PARENT part (0xB1): own cube geometry + one ChildInstance { 0xC1, translate+3x }.
//   No flat artifact — tracer must expand children from the compositional .part.
// WorldTracer: one identity instance of parent.
// Asserts: ray from (3,0,-5) dir (0,0,1) hits (child expanded at x=3).
// ================================================================

static const uint64_t kChildHash  = 0xC100000000000000ULL;
static const uint64_t kParentHash = 0xB100000000000000ULL;

static bool wt_write_compositional_fixture() {
    // --- Write CHILD part (0xC1): unit cube geometry, no children, no lods ---
    {
        std::vector<Tri>  tris  = unit_cube_tris();
        std::vector<TriEx> tex(tris.size(), wt_make_triex(1));
        BLASManager blas;
        TLASManager tlas(16);
        BLASHandle h = blas.register_triangles(tris.data(), (int)tris.size(), tex.data());
        (void)h;
        std::string path = std::string(kTracerCache) + "/" +
                           part_asset::cache_path_resolved(kChildHash);
        if (!part_asset::save_v2(path, blas, tlas, nullptr, 0,
                                 part_asset::LodLevels{}, kChildHash))
            return false;
    }
    // --- Write PARENT part (0xB1): one cube geometry + child table { 0xC1, +3x } ---
    {
        std::vector<Tri>  tris  = unit_cube_tris();
        std::vector<TriEx> tex(tris.size(), wt_make_triex(1));
        BLASManager blas;
        TLASManager tlas(16);
        BLASHandle h = blas.register_triangles(tris.data(), (int)tris.size(), tex.data());
        (void)h;
        // Child instance: kChildHash translated +3 along x
        part_asset::ChildInstance ci;
        ci.child_resolved_hash = kChildHash;
        // row-major identity with tx=3
        float tf[16] = {1,0,0,3, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::memcpy(ci.transform, tf, sizeof(tf));
        std::string path = std::string(kTracerCache) + "/" +
                           part_asset::cache_path_resolved(kParentHash);
        if (!part_asset::save_v2(path, blas, tlas, &ci, 1,
                                 part_asset::LodLevels{}, kParentHash))
            return false;
    }
    return true;
}

// ---- Test 18: compositional child expansion ----
static void test_tracer_compositional_child() {
    world_tracer::WorldTracer wt;
    std::string err;

    // Single identity instance of the parent (which has no flat artifact).
    world_tracer::TraceInstance inst;
    inst.part_hash = kParentHash;
    std::memcpy(inst.transform, wt_identity, 64);

    bool built = wt.build(kTracerCache, {inst}, err);
    if (!built) printf("  build error: %s\n", err.c_str());
    CHECK(built, "tracer compositional: build ok");
    if (!built) return;

    // Ray aimed at the child's translated position (x=3): from (3,0,-5) dir (0,0,1)
    // Child cube spans [2.5,3.5]x[-0.5,0.5]x[-0.5,0.5]; front face at z=-0.5, t=4.5
    const float O[3] = {3.f, 0.f, -5.f};
    const float D[3] = {0.f, 0.f,  1.f};
    world_tracer::Hit hit;
    bool ok = wt.trace(O, D, 100.f, hit);
    CHECK(ok, "tracer compositional: ray at child pos hits");
    char msg[128];
    std::snprintf(msg, sizeof msg,
                  "tracer compositional: t ≈ 4.5 (got %.4f)", hit.t);
    CHECK(feq(hit.t, 4.5f, 0.1f), msg);

    // Ray aimed at the parent's own geometry (x=0): should also hit (t≈4.5)
    const float O2[3] = {0.f, 0.f, -5.f};
    world_tracer::Hit hit2;
    bool ok2 = wt.trace(O2, D, 100.f, hit2);
    CHECK(ok2, "tracer compositional: ray at parent pos hits own geometry");
}

// ---- Test 17: world_bounds contains both instances ----
static void test_tracer_world_bounds() {
    world_tracer::WorldTracer wt;
    std::string err;

    world_tracer::TraceInstance inst0, inst1;
    inst0.part_hash = kCubeHash;
    inst1.part_hash = kCubeHash;
    std::memcpy(inst0.transform, wt_identity, 64);        // cube at origin
    wt_translate(inst1.transform, 10.f, 0.f, 0.f);        // cube at x=10

    std::vector<world_tracer::TraceInstance> insts = {inst0, inst1};
    bool built = wt.build(kTracerCache, insts, err);
    CHECK(built, "tracer bounds: build ok");
    if (!built) return;

    float mn[3], mx[3];
    wt.world_bounds(mn, mx);
    // inst0 spans [-0.5, 0.5]^3; inst1 spans [9.5, 10.5] in x
    CHECK(mn[0] <= -0.5f + 0.01f, "tracer bounds: mn[0] <= -0.5");
    CHECK(mx[0] >= 10.5f - 0.01f, "tracer bounds: mx[0] >= 10.5");
    CHECK(mn[1] <= -0.5f + 0.01f, "tracer bounds: mn[1] <= -0.5");
    CHECK(mx[1] >=  0.5f - 0.01f, "tracer bounds: mx[1] >= 0.5");
}

int main() {
    // Create fresh sandbox.
    system("rm -rf sandbox && mkdir -p sandbox");

    test_defaults();
    test_missing_file();
    test_parse();
    test_last_wins();
    test_malformed();
    test_fingerprint();
    test_read_manifest_skip();

    // Task 2: ProbeVolume tests.
    test_probe_roundtrip();
    test_probe_fingerprint_mismatch();
    test_probe_truncated();
    test_probe_nonexistent();

    // Task 3: WorldTracer tests.
    printf("\n--- Task 3: WorldTracer ---\n");
    if (!wt_write_cube_fixture()) {
        printf("FAIL: could not write cube fixture under %s\n", kTracerCache);
        ++failures;
    } else {
        test_tracer_identity_hit();
        test_tracer_translated();
        test_tracer_scaled_instance();
        test_tracer_occluded();
        test_tracer_two_instances_nearer_wins();
        test_tracer_world_bounds();
    }

    // Task 3 gap: compositional child expansion.
    printf("\n--- Task 3 gap: compositional child expansion ---\n");
    if (!wt_write_compositional_fixture()) {
        printf("FAIL: could not write compositional fixture under %s\n", kTracerCache);
        ++failures;
    } else {
        test_tracer_compositional_child();
    }

    if (failures == 0) {
        printf("\nALL PASS (%d checks failed)\n", 0);
    } else {
        printf("\n%d FAIL(s)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
