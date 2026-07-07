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
    CloseWindow();
    std::fprintf(stderr, "tileset_load_tests: %d run, %d failed\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
