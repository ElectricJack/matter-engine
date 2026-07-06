// tileset_bake_tests: baked-part collider bridge + fixture round-trip tests.
//
// Bakes two minimal parts (Pebble = voxel sphere, Twig = line capsule) through
// the real HostBaker into a temp cache dir, then exercises collider_for_part,
// scale_fit, and fit_half_height.
//
// Link recipe: mirrors run-graph-integration (full ScriptHost + QuickJS + MSL
// backend) plus box3d objects (like run-tilesetphysics) and
// ../src/tileset_collider.cpp + ../src/tileset_part_collider.cpp.

#include "part_graph.h"          // -DMATTER_HAVE_SCRIPT_HOST: FileModuleResolver + HostBaker
#include "part_asset_v2.h"       // cache_path_resolved
#include "tileset_part_collider.h"
#include "tileset_bake.h"
#include "tileset_spec.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

static bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static void write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary);
    f << body;
}

// NOTE: The host evaluates schemas as GLOBAL scripts (no shared-lib root set),
// so `export default class` would throw. Use plain `class X extends Part` syntax.

static const char* kPebbleJs = R"JS(
class Pebble extends Part {
  static params = { seed: 0 };
  build(p) {
    this.fill(1);
    this.beginVoxels(0.01);
    this.sphere([0, 0, 0], 0.05);
    this.endVoxels();
  }
}
)JS";

static const char* kTwigJs = R"JS(
class Twig extends Part {
  build(p) {
    this.fill(2);
    this.line([-0.1, 0, 0], [0.1, 0, 0], 0.015, 0.015);
  }
}
)JS";

// ---------------------------------------------------------------------------
// test_collider_bridge: given two already-baked hashes, exercise the bridge.
// ---------------------------------------------------------------------------
static void test_collider_bridge(const std::string& cache_dir,
                                 uint64_t pebble_hash, uint64_t twig_hash)
{
    std::string err;

    // --- Pebble: voxel sphere -> should fit sphere or hull (isotropic) ---
    tileset::ColliderFit pf;
    CHECK(tileset::collider_for_part(cache_dir, pebble_hash, nullptr, pf, err),
          "bridge: pebble collider fits");
    if (!tileset::collider_for_part(cache_dir, pebble_hash, nullptr, pf, err))
        printf("  err: %s\n", err.c_str());
    CHECK(pf.type == tileset::ColliderType::Sphere || pf.type == tileset::ColliderType::Hull,
          "bridge: pebble is sphere-ish");
    CHECK(pf.volume > 1e-7f, "bridge: pebble volume positive");

    // --- Twig: elongated line capsule -> auto-fits capsule ---
    tileset::ColliderFit tf;
    CHECK(tileset::collider_for_part(cache_dir, twig_hash, nullptr, tf, err),
          "bridge: twig collider fits");
    if (!tileset::collider_for_part(cache_dir, twig_hash, nullptr, tf, err))
        printf("  err: %s\n", err.c_str());
    CHECK(tf.type == tileset::ColliderType::Capsule, "bridge: twig auto-fits capsule");

    // --- Force box override ---
    tileset::ColliderFit forced;
    CHECK(tileset::collider_for_part(cache_dir, twig_hash, "box", forced, err),
          "bridge: override accepted");
    CHECK(forced.type == tileset::ColliderType::Box, "bridge: override forces box");

    // --- Missing part is a structured error ---
    tileset::ColliderFit dummy;
    bool missing_ok = !tileset::collider_for_part(cache_dir, 0xDEADull, nullptr, dummy, err)
                      && !err.empty();
    CHECK(missing_ok, "bridge: missing part is a structured error");
    if (!missing_ok) printf("  err was: '%s'\n", err.c_str());

    // --- scale_fit ---
    tileset::ColliderFit s = tileset::scale_fit(tf, 2.0f);
    CHECK(std::fabs(s.radius - tf.radius * 2.0f) < 1e-6f,
          "bridge: scale_fit scales radius");
    // Volume should scale cubically (2^3 = 8x).
    float vol_ratio = (tf.volume > 1e-30f) ? (s.volume / tf.volume) : 0.0f;
    CHECK(std::fabs(vol_ratio - 8.0f) < 1e-3f * 8.0f,
          "bridge: scale_fit scales volume cubically");

    // --- fit_half_height ---
    CHECK(tileset::fit_half_height(pf) > 0.0f, "bridge: pebble half height positive");
    CHECK(tileset::fit_half_height(tf) > 0.0f, "bridge: twig half height positive");
}

// ---------------------------------------------------------------------------
// Task 7: settle_tileset fixture
// ---------------------------------------------------------------------------
static tileset::TilesetSpec make_spec(uint64_t pebble_hash, uint64_t twig_hash) {
    tileset::TilesetSpec s;
    s.tile_called = true;
    s.cfg.size = 2.0f; s.cfg.seed = 99;
    // flat base
    s.base.n = 64; s.base.cell = 2.0f / 64.0f; s.base.material = 1;
    s.base.heights.assign(64 * 64, 0.0f); s.base.set = true;

    // one shared drop (Twig at tile-local (1.0, 0.2, 0.5), identity rotation)
    tileset::DropChildRec d{}; d.child_hash = twig_hash;
    float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(d.transform, I, sizeof I);
    d.transform[3] = 1.0f; d.transform[7] = 0.2f; d.transform[11] = 0.5f; // row-major TX/TY/TZ
    s.drops.push_back(d);

    // layer 0: pebbles, physics:false, embed 0.5
    tileset::LayerSpec l0; l0.module = "Pebble"; l0.density = 10.0f;
    l0.physics = false; l0.embed = 0.5f;
    for (int t = 0; t < 16; ++t)
        for (int i = 0; i < 3; ++i) {
            tileset::Placement p{}; p.child_hash = pebble_hash;
            p.pos[0] = 0.3f + 0.4f * i; p.pos[2] = 0.5f + 0.3f * i; p.scale = 1.0f;
            p.quat[3] = 1.0f;
            l0.interior[t].push_back(p);
        }
    s.layers.push_back(l0);

    // layer 1: twigs, physics:true -- strips + interiors
    tileset::LayerSpec l1; l1.module = "Twig"; l1.density = 4.0f; l1.physics = true;
    for (int o = 0; o < 2; ++o)
        for (int c = 0; c < 2; ++c) {
            tileset::Placement p{}; p.child_hash = twig_hash;
            p.pos[0] = (o == 0) ? 0.05f : 1.0f;   // strip-local
            p.pos[1] = 0.2f;
            p.pos[2] = (o == 0) ? 1.0f : 0.05f;
            p.scale = 1.0f; p.quat[3] = 1.0f;
            l1.strip[o][c].push_back(p);
        }
    for (int t = 0; t < 16; ++t) {
        tileset::Placement p{}; p.child_hash = twig_hash;
        p.pos[0] = 1.2f; p.pos[1] = 0.25f; p.pos[2] = 1.4f; p.scale = 1.0f; p.quat[3] = 1.0f;
        l1.interior[t].push_back(p);
    }
    s.layers.push_back(l1);
    return s;
}

static void test_settle_tileset(const std::string& cache_dir,
                                uint64_t pebble_hash, uint64_t twig_hash) {
    tileset::TilesetSpec spec = make_spec(pebble_hash, twig_hash);
    tileset::BakeInputs in{ cache_dir };
    tileset::SettledTorus torus;
    std::string err;
    CHECK(tileset::settle_tileset(spec, in, torus, err), "bake: settle_tileset succeeds");
    CHECK(err.empty(), "bake: no error text on success");

    // Counts: drops 1x16 sync instances; layer1 strips 4 placements x 8 occurrences,
    // interiors 16; layer0 pebbles 16 tiles x 3 (non-physics).
    size_t expect = 16 + (4 * 8 + 16) + 48;
    CHECK(torus.instances.size() == expect, "bake: instance count (16 drops + 48 strips+interiors + 48 pebbles)");

    // All inside torus, none below ground
    float E = 4 * 2.0f;
    bool in_range = true, grounded = true;
    for (const auto& si : torus.instances) {
        if (si.pose.px < 0 || si.pose.px >= E || si.pose.pz < 0 || si.pose.pz >= E) in_range = false;
        if (si.pose.py < -0.05f || si.pose.py > 1.0f) grounded = false;
    }
    CHECK(in_range, "bake: all instances inside torus");
    CHECK(grounded, "bake: all instances near ground");

    // Non-physics embed math: pebble radius 0.05, embed 0.5 => y = fh - embed*2*fh = 0
    // (flat base h=0, fh ~= 0.05): expect y in [-0.01, 0.06] band and equal across tiles.
    float y0 = -1.0f; bool pebble_y_consistent = true;
    for (const auto& si : torus.instances) {
        if (si.layer != 0) continue;
        if (y0 < 0.0f) y0 = si.pose.py;
        else if (std::fabs(si.pose.py - y0) > 1e-6f) pebble_y_consistent = false;
    }
    CHECK(pebble_y_consistent, "bake: snapped pebble height identical across tiles");

    // Sync invariants: the 16 drop instances differ only by tile translation
    const auto* first = &torus.instances[0];
    bool sync_ok = true;
    for (int k = 1; k < 16; ++k) {
        const auto& a = torus.instances[0], & b = torus.instances[k];
        float dx = b.pose.px - a.pose.px, dz = b.pose.pz - a.pose.pz;
        float rx = std::fmod(dx, 2.0f), rz = std::fmod(dz, 2.0f);
        if (std::fabs(rx) > 1e-3f && std::fabs(std::fabs(rx) - 2.0f) > 1e-3f) sync_ok = false;
        if (std::fabs(rz) > 1e-3f && std::fabs(std::fabs(rz) - 2.0f) > 1e-3f) sync_ok = false;
        if (std::fabs(b.pose.py - a.pose.py) > 1e-4f) sync_ok = false;
        if (b.pose.qx != a.pose.qx || b.pose.qw != a.pose.qw) sync_ok = false;
    }
    (void)first;
    CHECK(sync_ok, "bake: 16 drop instances identical modulo tile translation");

    CHECK(torus.report.layers.size() == 2, "bake: per-layer reports present");
    CHECK(torus.report.pose_hash != 0, "bake: pose hash produced");

    // Determinism
    tileset::SettledTorus torus2;
    CHECK(tileset::settle_tileset(spec, in, torus2, err), "bake: second run succeeds");
    CHECK(torus.report.pose_hash == torus2.report.pose_hash, "bake: settle deterministic");

    // Failure path: unknown hash
    spec.layers[0].interior[0][0].child_hash = 0xBAD;
    tileset::SettledTorus t3;
    CHECK(!tileset::settle_tileset(spec, in, t3, err) && err.find("Pebble") != std::string::npos,
          "bake: unknown child hash errors naming the layer module");
}

// ---------------------------------------------------------------------------
// main: set up temp cache, bake fixtures, call test.
// ---------------------------------------------------------------------------
int main()
{
    namespace pg = part_graph;

    // Absolute paths before any chdir.
    char prevcwd[4096] = {};
    if (!getcwd(prevcwd, sizeof prevcwd)) prevcwd[0] = '\0';

    // Fresh temp sandbox.
    const std::string root    = "/tmp/me3_tilesetbake_tests";
    const std::string schemas = root + "/schemas";
    const std::string parts   = root + "/parts";
    system(("rm -rf " + root).c_str());
    system(("mkdir -p " + schemas + " " + parts).c_str());

    write_file(schemas + "/Pebble.js", kPebbleJs);
    write_file(schemas + "/Twig.js",   kTwigJs);

    // chdir so HostBaker's relative "parts/<hash>.part" lands under <root>/parts.
    CHECK(chdir(root.c_str()) == 0, "setup: chdir into sandbox");

    script_host::ScriptHost host;
    pg::FileModuleResolver resolver(host, "schemas");
    pg::HostBaker baker(host, ".");
    pg::PartGraph graph(resolver, baker);

    pg::InstallResult pebble_ir = graph.install(
        { pg::ChildRequest{ "Pebble", pg::Params{} } });
    CHECK(pebble_ir.ok, "bake: Pebble installs ok");
    if (!pebble_ir.ok) printf("  install error: %s\n", pebble_ir.error.c_str());
    CHECK(pebble_ir.root_hashes.size() == 1, "bake: Pebble has one root hash");

    pg::InstallResult twig_ir = graph.install(
        { pg::ChildRequest{ "Twig", pg::Params{} } });
    CHECK(twig_ir.ok, "bake: Twig installs ok");
    if (!twig_ir.ok) printf("  install error: %s\n", twig_ir.error.c_str());
    CHECK(twig_ir.root_hashes.size() == 1, "bake: Twig has one root hash");

    if (pebble_ir.ok && !pebble_ir.root_hashes.empty() &&
        twig_ir.ok   && !twig_ir.root_hashes.empty())
    {
        uint64_t pebble_hash = pebble_ir.root_hashes[0];
        uint64_t twig_hash   = twig_ir.root_hashes[0];

        printf("[hash] Pebble -> %016llx\n", (unsigned long long)pebble_hash);
        printf("[hash] Twig   -> %016llx\n", (unsigned long long)twig_hash);

        // Confirm the .part files exist.
        std::string pebble_path = parts + "/" +
            std::string(part_asset::cache_path_resolved(pebble_hash)).substr(6); // strip "parts/"
        std::string twig_path = parts + "/" +
            std::string(part_asset::cache_path_resolved(twig_hash)).substr(6);
        // Actually cache_path_resolved returns "parts/<hash>.part"; the baker
        // writes to cwd-relative "parts/<hash>.part" and we chdir'd to root.
        pebble_path = root + "/" + part_asset::cache_path_resolved(pebble_hash);
        twig_path   = root + "/" + part_asset::cache_path_resolved(twig_hash);

        CHECK(file_exists(pebble_path), "bake: Pebble .part exists on disk");
        CHECK(file_exists(twig_path),   "bake: Twig .part exists on disk");

        // cache_dir for collider_for_part: the directory that CONTAINS parts/.
        test_collider_bridge(root, pebble_hash, twig_hash);
    }

    if (pebble_ir.ok && !pebble_ir.root_hashes.empty() &&
        twig_ir.ok   && !twig_ir.root_hashes.empty())
    {
        uint64_t pebble_hash = pebble_ir.root_hashes[0];
        uint64_t twig_hash   = twig_ir.root_hashes[0];
        test_settle_tileset(root, pebble_hash, twig_hash);
    }

    if (prevcwd[0]) (void)chdir(prevcwd);
    system(("rm -rf " + root).c_str());

    if (g_failures == 0)
        printf("\nAll tileset_bake_tests passed.\n");
    else
        printf("\n%d tileset_bake_test(s) FAILED.\n", g_failures);

    return g_failures;
}
