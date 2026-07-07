// tileset_load_tests.cpp — LocalProvider tileset-root wiring test.
//
// Fixture: a minimal WorldData/<world>/world.manifest with a `TrivialGround tileset`
// line + a matching TrivialGround.js schema. Runs LocalProvider::connect() with a
// hidden GL context and asserts:
//   * connect() returns true (no err)
//   * baked_tileset_count() == 1
//   * <world_data>/TrivialGround.gtex exists on disk
//     (run_tileset_phase writes gtex to world_data_dir/<root_module>.gtex,
//      not to the world subdir)
//   * viewer::tileset_provider::get_slot(0).valid == true after connect()
//
// The .gtex is baked via the GPU overload of run_tileset_phase, so the test
// requires a GL 4.6 context (WSLg: GALLIUM_DRIVER=d3d12).

#include "raylib.h"
#include "gl46.h"
#include "local_provider.h"
#include "tileset_provider.h"
#include "world_source.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>

static int g_tests = 0, g_failures = 0;
#define REQUIRE(cond) do { \
    ++g_tests; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

static bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

static void mkdirs(const std::string& p) {
    // Depth-2 tree only in this test.
    ::mkdir(p.c_str(), 0755);
}

static void test_local_provider_processes_tileset_root() {
    const std::string root = "/tmp/me3_tileset_load";
    ::system(("rm -rf " + root).c_str());
    mkdirs(root);
    mkdirs(root + "/schemas");
    mkdirs(root + "/WorldData");
    mkdirs(root + "/WorldData/TinyWorld");
    mkdirs(root + "/cache");
    mkdirs(root + "/shared-lib");

    // Minimal manifest: one tileset root.
    write_file(root + "/WorldData/TinyWorld/world.manifest",
               "TrivialGround tileset\n");
    // Minimal schema: flat 2m tile, one dirt fill layer via base(), no children.
    // Global-script syntax: no `export default` (tileset eval uses plain class form).
    write_file(root + "/schemas/TrivialGround.js",
        "class TrivialGround extends Tileset {\n"
        "  static requires = [];\n"
        "  build() {\n"
        "    this.tile({ size: 2.0, texelsPerMeter: 32, seed: 1 });\n"
        "    this.base((x,z) => 0.0, 1);\n"
        "  }\n"
        "}\n");

    viewer::LocalProviderConfig cfg;
    cfg.schemas_dir     = root + "/schemas";
    cfg.world_data_dir  = root + "/WorldData";
    cfg.world_name      = "TinyWorld";
    cfg.shared_lib_dir  = root + "/shared-lib";
    cfg.cache_root      = root + "/cache";

    viewer::LocalProvider provider(cfg);
    viewer::WorldManifest wm;
    std::string err;
    bool ok = provider.connect(wm, err);
    if (!ok) std::fprintf(stderr, "  connect err: %s\n", err.c_str());
    REQUIRE(ok);

    REQUIRE(provider.baked_tileset_count() == 1);
    // run_tileset_phase writes gtex to world_data_dir/<root_module>.gtex (not world subdir).
    REQUIRE(file_exists(root + "/WorldData/TrivialGround.gtex"));
    REQUIRE(viewer::tileset_provider::get_slot(0).valid);

    viewer::tileset_provider::unload_all();
    ::system(("rm -rf " + root).c_str());
}

// Optional second-pass fixture: run the REAL ForestFloor tileset from the
// world_demo schemas dir through LocalProvider::connect(). Gated on the env
// var MATTER_USE_REAL_FOREST_FLOOR=1 because it depends on the on-disk
// world_demo/schemas layout and produces artifacts inside /tmp. Combined with
// MATTER_TILESET_DUMP_PNG=1 (checked by LocalProvider itself) this is the way
// to eyeball the actual atlas without launching the viewer window.
static void test_local_provider_bakes_meadow_forestfloor() {
    // Relative to viewer/ (where the test binary is normally run from) OR
    // repo-root (build-all.sh) — try both.
    std::string src_schemas = "../examples/world_demo/schemas";
    struct stat st_probe;
    if (stat(src_schemas.c_str(), &st_probe) != 0) {
        src_schemas = "MatterEngine3/examples/world_demo/schemas";
    }
    const std::string root = "/tmp/me3_forestfloor_bake";
    ::system(("rm -rf " + root).c_str());
    mkdirs(root);
    mkdirs(root + "/schemas");
    mkdirs(root + "/WorldData");
    mkdirs(root + "/WorldData/Sandbox");
    mkdirs(root + "/cache");

    // Copy the production schemas verbatim — no scaled-down variant. With Rock
    // as the only physics layer (~256 bodies over the 4x4 torus), the settle
    // converges in seconds even at production density (Pebble/Twig/Leaf use
    // physics: false and surface-snap algorithmically).
    const char* needed[] = {
        "ForestFloor.js", "Pebble.js", "Rock.js", "Twig.js", "Leaf.js",
    };
    for (const char* fn : needed) {
        std::string cp = "cp " + src_schemas + "/" + fn + " " + root + "/schemas/" + fn;
        int rc = ::system(cp.c_str());
        if (rc != 0) {
            std::fprintf(stderr, "FAIL: could not stage %s (rc=%d)\n", fn, rc);
            ++g_failures;
            return;
        }
    }

    // Copy the shared-lib (Pebble.js imports shared-lib/rng).
    std::string src_shared_lib = "../shared-lib";
    struct stat st_sl;
    if (stat(src_shared_lib.c_str(), &st_sl) != 0) {
        src_shared_lib = "MatterEngine3/shared-lib";
    }
    std::string cp_sl = "cp -r " + src_shared_lib + " " + root + "/shared-lib";
    if (::system(cp_sl.c_str()) != 0) {
        std::fprintf(stderr, "FAIL: could not stage shared-lib from %s\n",
                     src_shared_lib.c_str());
        ++g_failures;
        return;
    }

    // Sandbox manifest: single tileset root.
    write_file(root + "/WorldData/Sandbox/world.manifest",
               "ForestFloor tileset\n");

    viewer::LocalProviderConfig cfg;
    cfg.schemas_dir     = root + "/schemas";
    cfg.world_data_dir  = root + "/WorldData";
    cfg.world_name      = "Sandbox";
    cfg.shared_lib_dir  = root + "/shared-lib";
    cfg.cache_root      = root + "/cache";

    viewer::LocalProvider provider(cfg);
    viewer::WorldManifest wm;
    std::string err;
    bool ok = provider.connect(wm, err);
    if (!ok) std::fprintf(stderr, "  ForestFloor connect err: %s\n", err.c_str());
    REQUIRE(ok);
    REQUIRE(provider.baked_tileset_count() == 1);
    REQUIRE(file_exists(root + "/WorldData/ForestFloor.gtex"));

    std::fprintf(stderr, "ForestFloor atlas + PNGs (if MATTER_TILESET_DUMP_PNG=1) at %s/WorldData/\n",
                 root.c_str());

    viewer::tileset_provider::unload_all();
    // Do NOT rm -rf here — the caller may want to copy the PNGs out.
}

int main() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(64, 64, "tileset_load_tests");
    std::string why;
    if (!viewer::gl46_available(why)) {
        std::fprintf(stderr, "SKIP: %s; set GALLIUM_DRIVER=d3d12 on WSLg\n", why.c_str());
        CloseWindow();
        return 0;
    }
    test_local_provider_processes_tileset_root();
    if (std::getenv("MATTER_USE_REAL_FOREST_FLOOR") != nullptr) {
        test_local_provider_bakes_meadow_forestfloor();
    }
    CloseWindow();
    std::fprintf(stderr, "tileset_load_tests: %d run, %d failed\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
