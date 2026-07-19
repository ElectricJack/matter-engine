// tileset_bake_tests: baked-part collider bridge + fixture round-trip tests.
//
// Bakes two minimal parts (Pebble = voxel sphere, Twig = line capsule) through
// the real HostBaker into a temp cache dir, then exercises collider_for_part,
// scale_fit, and fit_half_height.
//
// Task 8 appends: test_e2e_manifest_to_settled_torus — full pipeline from a
// world class on disk through run_tileset_phase_from_objects to a SettledTorus.
//
// Link recipe: mirrors run-graph-integration (full ScriptHost + QuickJS + MSL
// backend) plus box3d objects (like run-tilesetphysics) and
// ../src/tileset_collider.cpp + ../src/tileset_part_collider.cpp +
// ../src/tileset_phase.cpp.

#include "part_graph.h"          // -DMATTER_HAVE_SCRIPT_HOST: FileModuleResolver + HostBaker
#include "part_asset_v2.h"       // cache_path_resolved
#include "tileset_part_collider.h"
#include "tileset_bake.h"
#include "tileset_phase.h"
#include "tileset_spec.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"

namespace fs = std::filesystem;

static bool write_file(const fs::path& path, const std::string& body) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << body;
    return f.good();
}

static void remove_tree(const fs::path& path) {
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

static bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// ---------------------------------------------------------------------------
// F2: params_to_json / params_from_json roundtrip regression test.
//
// run_tileset_phase converts params_json -> Params via params_from_json for
// graph.install, while the original JSON string is used as the composite
// child-hash key in eval_tileset.  If params_to_json(params_from_json(x)) != x
// for any canonical shape, resolved hashes can silently diverge.
// ---------------------------------------------------------------------------
static void test_params_roundtrip() {
    using namespace part_graph;

    // Helper: assert roundtrip idempotence for one canonical JSON string.
    auto check_rt = [&](const char* label, const std::string& json) {
        Params p = params_from_json(json);
        std::string back = params_to_json(p);
        if (back != json) {
            printf("FAIL: params roundtrip [%s]: in='%s' out='%s'\n",
                   label, json.c_str(), back.c_str());
            ++g_failures;
        } else {
            printf("ok:   params roundtrip [%s]\n", label);
        }
    };

    // Empty object.
    check_rt("{}", "{}");

    // Integer-valued number (%.17g prints without decimal point for exact integers).
    check_rt("integer", R"({"seed":0})");
    check_rt("integer nonzero", R"({"count":42})");

    // Float (verify %.17g -> strtod -> %.17g is identity).
    check_rt("float", R"({"scale":1.5})");
    check_rt("float 17g", R"({"x":0.10000000000000001})");

    // Bool.
    check_rt("bool true",  R"({"physics":true})");
    check_rt("bool false", R"({"physics":false})");

    // String.
    check_rt("string", R"({"variant":"oak"})");

    // Multiple keys (params_to_json outputs in std::map sorted order).
    // eval_requires emits keys in alphabetical order, matching std::map.
    check_rt("multi", R"({"physics":true,"scale":1.5,"seed":0})");
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
// test_scaled_collider_physics
//
// Verifies (a) that instances record their intended scale, and (b) that a
// physics body simulated with a half-scale collider rests at a lower y than
// a full-scale body — proving the physics engine actually used the scaled
// geometry and not the unscaled base.
//
// Also exercises the horizontal-strip axis convention with a non-physics layer:
// places a pebble at strip-local pos={along=0.8, y=0, across=0.1} inside a
// horizontal strip (color 0, so boundary row = 1), and asserts the resulting
// world coord is: wx = lane*T + 0.8, wz = boundary*T + 0.1 (i.e. the two
// swaps — DSL recording and frame construction — compose correctly).
// ---------------------------------------------------------------------------
static void test_scaled_collider_physics(const std::string& cache_dir,
                                         uint64_t pebble_hash, uint64_t twig_hash)
{
    const float T = 2.0f;

    // --- (a) Scale recorded in SettledInstance ---
    {
        tileset::TilesetSpec s;
        s.tile_called = true;
        s.cfg.size = T; s.cfg.seed = 7;
        s.base.n = 16; s.base.cell = T / 16.0f; s.base.material = 1;
        s.base.heights.assign(16 * 16, 0.0f); s.base.set = true;

        tileset::LayerSpec l1;
        l1.module = "Twig"; l1.density = 1.0f; l1.physics = true;
        // One interior placement at scale 0.5 in each of the 16 tiles.
        for (int t = 0; t < 16; ++t) {
            tileset::Placement p{}; p.child_hash = twig_hash;
            p.pos[0] = 1.0f; p.pos[1] = 0.3f; p.pos[2] = 1.0f;
            p.scale = 0.5f; p.quat[3] = 1.0f;
            l1.interior[t].push_back(p);
        }
        s.layers.push_back(l1);

        tileset::BakeInputs in{ cache_dir };
        tileset::SettledTorus torus;
        std::string serr;
        CHECK(tileset::settle_tileset(s, in, torus, serr),
              "scale: settle_tileset with scale=0.5 succeeds");

        bool all_half = true;
        for (const auto& si : torus.instances)
            if (std::fabs(si.scale - 0.5f) > 1e-6f) all_half = false;
        CHECK(all_half, "scale: all instances record scale=0.5");
    }

    // --- (b) Scaled body rests lower than unscaled body ---
    // A half-scale pebble (radius ~0.025) should rest at y ~0.025;
    // a full-scale pebble (radius ~0.05) rests at y ~0.05.
    // We run two separate single-tile specs and compare settled y.
    auto settle_single_pebble = [&](float scale) -> float {
        tileset::TilesetSpec s;
        s.tile_called = true;
        s.cfg.size = T; s.cfg.seed = 42;
        s.base.n = 16; s.base.cell = T / 16.0f; s.base.material = 1;
        s.base.heights.assign(16 * 16, 0.0f); s.base.set = true;

        tileset::LayerSpec l;
        l.module = "Pebble"; l.density = 1.0f; l.physics = true;
        tileset::Placement p{}; p.child_hash = pebble_hash;
        p.pos[0] = 1.0f; p.pos[1] = 0.4f; p.pos[2] = 1.0f;
        p.scale = scale; p.quat[3] = 1.0f;
        // Place in all 16 tiles so we can pick any instance.
        for (int t = 0; t < 16; ++t)
            l.interior[t].push_back(p);
        s.layers.push_back(l);

        tileset::BakeInputs in{ cache_dir };
        tileset::SettledTorus torus;
        std::string serr;
        if (!tileset::settle_tileset(s, in, torus, serr) || torus.instances.empty())
            return -1.0f;
        // All instances on a flat base, so any settled y is representative.
        return torus.instances[0].pose.py;
    };

    float y_full = settle_single_pebble(1.0f);
    float y_half = settle_single_pebble(0.5f);
    // Both must be above ground.
    CHECK(y_full > 0.005f, "scale: full-scale pebble rests above ground");
    CHECK(y_half > 0.001f, "scale: half-scale pebble rests above ground");
    // Half-scale rests meaningfully lower (at least 0.01m difference).
    CHECK(y_full - y_half > 0.01f,
          "scale: half-scale pebble rests lower than full-scale (collider was applied)");

    // --- (c) Horizontal strip axis convention (non-physics path) ---
    // Horizontal strip: DSL records p.pos={along, y, across}.
    // Frame: px = lane*T, pz = boundary*T.
    // Expected world: wx = lane*T + along, wz = boundary*T + across.
    //
    // We use a non-physics layer so y is analytically snapped and world x/z
    // come from the placement formula directly (no physics movement).
    {
        tileset::TilesetSpec s;
        s.tile_called = true;
        s.cfg.size = T; s.cfg.seed = 13;
        s.base.n = 16; s.base.cell = T / 16.0f; s.base.material = 1;
        s.base.heights.assign(16 * 16, 0.0f); s.base.set = true;

        tileset::LayerSpec l;
        l.module = "Pebble"; l.physics = false; l.embed = 0.0f; l.density = 1.0f;

        // Vertical strip (orient=0, color=0): across=0.1, along=0.8
        // DSL records vertical as p.pos={across, y, along}
        tileset::Placement pv{}; pv.child_hash = pebble_hash;
        pv.pos[0] = 0.1f; pv.pos[1] = 0.0f; pv.pos[2] = 0.8f; // {across, y, along}
        pv.scale = 1.0f; pv.quat[3] = 1.0f;
        l.strip[0][0].push_back(pv);  // orient=0 (vertical), color=0

        // Horizontal strip (orient=1, color=0): across=0.1, along=0.8
        // DSL records horizontal as p.pos={along, y, across}
        tileset::Placement ph{}; ph.child_hash = pebble_hash;
        ph.pos[0] = 0.8f; ph.pos[1] = 0.0f; ph.pos[2] = 0.1f; // {along, y, across}
        ph.scale = 1.0f; ph.quat[3] = 1.0f;
        l.strip[1][0].push_back(ph);  // orient=1 (horizontal), color=0

        s.layers.push_back(l);

        tileset::BakeInputs in{ cache_dir };
        tileset::SettledTorus torus;
        std::string serr;
        CHECK(tileset::settle_tileset(s, in, torus, serr),
              "hstrip: settle_tileset with h+v strips succeeds");

        // Both strip orientations produce 8 instances each (8 occurrences per color).
        CHECK(torus.instances.size() == 16,
              "hstrip: 8 vertical + 8 horizontal non-physics instances (8 occ each)");

        // Verify that horizontal instances are within the torus and have the
        // expected fractional along/across offsets embedded in their world coords.
        bool residues_ok = true;
        for (const auto& si : torus.instances) {
            float rx = std::fmod(si.pose.px, T);
            float rz = std::fmod(si.pose.pz, T);
            if (rx < 0) rx += T; if (rz < 0) rz += T;
            bool rx_ok = std::fabs(rx - 0.1f) < 1e-4f || std::fabs(rx - 0.8f) < 1e-4f;
            bool rz_ok = std::fabs(rz - 0.1f) < 1e-4f || std::fabs(rz - 0.8f) < 1e-4f;
            if (!rx_ok || !rz_ok) residues_ok = false;
        }
        CHECK(residues_ok,
              "hstrip: all strip instance world coords have fractional part 0.1 or 0.8 (axis convention correct)");
    }
}

// ---------------------------------------------------------------------------
// Task 8: test_e2e_to_settled_torus
//
// End-to-end test: writes a ForestFloor.js tileset root to the objects dir,
// then exercises run_tileset_phase_from_objects all the way to a SettledTorus.
// Uses the project-root layout: objects/ for module sources.
// ---------------------------------------------------------------------------
static void test_e2e_to_settled_torus(const fs::path& objects_dir,
                                      const fs::path& cache_dir) {
    // ForestFloor tileset fixture (global-script syntax: no export default).
    // Pebble uses seed:0 as a required param; Twig has no overrides.
    // Both layers use physics:false so the e2e test is fast and convergence is
    // guaranteed (physics settle is exercised by test_settle_tileset above).
    static const char* kFloorJs = R"JS(
class ForestFloor extends Tileset {
  static requires = [
    { module: 'Pebble', params: { seed: 0 } },
    { module: 'Twig' },
  ];
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 7 });
    this.base((x, z) => 0.0, 1);
    this.layer('Pebble', { density: 4, physics: false, embed: 0.4,
                           params: () => ({ seed: 0 }) });
    this.layer('Twig',   { density: 2, physics: false, embed: 0.2 });
  }
}
)JS";

    // Write the fixture file into objects/.
    write_file(objects_dir / "ForestFloor.js", kFloorJs);

    // Run the full tileset phase using the project-layout entry point.
    tileset::SettledTorus torus;
    std::string err;
    CHECK(tileset::run_tileset_phase_from_objects(
              objects_dir.string(), "ForestFloor",
              cache_dir.string(), torus, err),
          "e2e: tileset phase runs");
    if (!err.empty()) printf("  e2e phase err: %s\n", err.c_str());
    CHECK(torus.report.converged_all, "e2e: all layers converged");
    CHECK(!torus.instances.empty(), "e2e: settled instances produced");
    CHECK(torus.base.set, "e2e: base field present");

    // Determinism: run the phase a second time and compare pose hashes.
    tileset::SettledTorus torus2;
    CHECK(tileset::run_tileset_phase_from_objects(
              objects_dir.string(), "ForestFloor",
              cache_dir.string(), torus2, err),
          "e2e: second phase run");
    if (!err.empty()) printf("  e2e phase2 err: %s\n", err.c_str());
    CHECK(torus.report.pose_hash == torus2.report.pose_hash,
          "e2e: phase deterministic (pose_hash stable)");

    // Fail-closed: missing child module -> error, false return.
    static const char* kBadFloorJs =
        "class BadFloor extends Tileset {\n"
        "  static requires = [ { module: 'Nope' } ];\n"
        "  build() { this.tile({size:2.0, texelsPerMeter:64, seed:1});\n"
        "            this.layer('Nope', { density: 1 }); }\n}\n";
    write_file(objects_dir / "BadFloor.js", kBadFloorJs);
    tileset::SettledTorus t3;
    bool bad_ok = !tileset::run_tileset_phase_from_objects(
                      objects_dir.string(), "BadFloor",
                      cache_dir.string(), t3, err)
                  && !err.empty();
    CHECK(bad_ok, "e2e: missing child module fails closed");
    if (!bad_ok) printf("  bad floor err was: '%s'\n", err.c_str());
}

// ---------------------------------------------------------------------------
// main: set up temp cache, bake fixtures, call test.
// ---------------------------------------------------------------------------
int main()
{
    // F2: params roundtrip — no sandbox needed, runs first.
    test_params_roundtrip();

    namespace pg = part_graph;

    // Store original working directory.
    const fs::path prevcwd = fs::current_path();

    // Fresh temp sandbox.
    const fs::path root    = fs::temp_directory_path() / "me3_tilesetbake_tests";
    const fs::path objects = root / "objects";
    const fs::path parts   = root / "parts";
    remove_tree(root);
    fs::create_directories(objects);
    fs::create_directories(parts);

    write_file(objects / "Pebble.js", kPebbleJs);
    write_file(objects / "Twig.js",   kTwigJs);

    // chdir so HostBaker's relative "parts/<hash>.part" lands under <root>/parts.
    std::error_code ec;
    fs::current_path(root, ec);
    CHECK(ec.value() == 0, "setup: chdir into sandbox");

    script_host::ScriptHost host;
    pg::FileModuleResolver resolver(host, "objects");
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
        fs::path pebble_path = root / part_asset::cache_path_resolved(pebble_hash);
        fs::path twig_path   = root / part_asset::cache_path_resolved(twig_hash);

        CHECK(file_exists(pebble_path), "bake: Pebble .part exists on disk");
        CHECK(file_exists(twig_path),   "bake: Twig .part exists on disk");

        // cache_dir for collider_for_part: the directory that CONTAINS parts/.
        test_collider_bridge(root.string(), pebble_hash, twig_hash);
    }

    if (pebble_ir.ok && !pebble_ir.root_hashes.empty() &&
        twig_ir.ok   && !twig_ir.root_hashes.empty())
    {
        uint64_t pebble_hash = pebble_ir.root_hashes[0];
        uint64_t twig_hash   = twig_ir.root_hashes[0];
        test_settle_tileset(root.string(), pebble_hash, twig_hash);
    }

    if (pebble_ir.ok && !pebble_ir.root_hashes.empty() &&
        twig_ir.ok   && !twig_ir.root_hashes.empty())
    {
        uint64_t pebble_hash = pebble_ir.root_hashes[0];
        uint64_t twig_hash   = twig_ir.root_hashes[0];
        test_scaled_collider_physics(root.string(), pebble_hash, twig_hash);
    }

    // Task 8 e2e: use project-layout with objects/ dir (already written).
    // cache_dir = root (contains parts/); we are already chdir'd there.
    {
        test_e2e_to_settled_torus(objects, root);
    }

    fs::current_path(prevcwd, ec);
    remove_tree(root);

    if (g_failures == 0)
        printf("\nAll tileset_bake_tests passed.\n");
    else
        printf("\n%d tileset_bake_test(s) FAILED.\n", g_failures);

    return g_failures;
}
