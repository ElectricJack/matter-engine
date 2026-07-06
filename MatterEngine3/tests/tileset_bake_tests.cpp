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

    if (prevcwd[0]) (void)chdir(prevcwd);
    system(("rm -rf " + root).c_str());

    if (g_failures == 0)
        printf("\nAll tileset_bake_tests passed.\n");
    else
        printf("\n%d tileset_bake_test(s) FAILED.\n", g_failures);

    return g_failures;
}
