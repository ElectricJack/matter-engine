// Task 1: World light list tests.
// Tests parse_lights, lights_fingerprint, and read_manifest light-line skip.
#include "world_lights.h"
#include "part_graph.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>

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

    if (failures == 0) {
        printf("\nALL PASS (%d checks failed)\n", 0);
    } else {
        printf("\n%d FAIL(s)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
