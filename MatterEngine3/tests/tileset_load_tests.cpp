// tileset_load_tests.cpp — LocalProvider tileset-root wiring test.
//
// Fixture: a minimal project-layout sandbox with worlds/<world>.js containing a
// World class with a tileset root + a matching TrivialGround.js object. Runs
// LocalProvider::connect() with a hidden GL context and asserts:
//   * connect() returns true (no err)
//   * baked_tileset_count() == 1
//   * <cache_root>/TrivialGround.gtex exists on disk
//   * viewer::tileset_provider::get_slot(0).valid == true after connect()
//
// The .gtex is baked via the GPU overload of run_tileset_phase, so the test
// requires a GL 4.6 context (WSLg: GALLIUM_DRIVER=d3d12).

#include "raylib.h"
#include "gl46.h"
#include "provider/local_provider.h"
#include "tileset_provider.h"
#include "world_source.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int g_tests = 0, g_failures = 0;
#define REQUIRE(cond) do { \
    ++g_tests; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

static bool write_file(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << content;
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

static void test_local_provider_processes_tileset_root() {
    const fs::path root = fs::temp_directory_path() / "me3_tileset_load";
    remove_tree(root);
    fs::create_directories(root / "objects");
    fs::create_directories(root / "worlds");
    fs::create_directories(root / "shared-lib");
    fs::create_directories(root / ".cache" / "TinyWorld");

    // World class: one tileset root.
    write_file(root / "worlds" / "TinyWorld.js",
        "class TinyWorld extends World {\n"
        "  static roots = [\n"
        "    { module: 'TrivialGround', transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1], tileset: true },\n"
        "  ];\n"
        "}\n");
    // Minimal schema: flat 2m tile, one dirt fill layer via base(), no children.
    // Global-script syntax: no `export default` (tileset eval uses plain class form).
    write_file(root / "objects" / "TrivialGround.js",
        "class TrivialGround extends Tileset {\n"
        "  static requires = [];\n"
        "  build() {\n"
        "    this.tile({ size: 2.0, texelsPerMeter: 32, seed: 1 });\n"
        "    this.base((x,z) => 0.0, 1);\n"
        "  }\n"
        "}\n");

    auto cfg = viewer::LocalProviderConfig::for_project(root.string(), "TinyWorld", "");
    cfg.gl_available = true;   // hidden raylib window is a real GL context

    viewer::LocalProvider provider(cfg);
    viewer::WorldManifest wm;
    std::string err;
    bool ok = provider.connect(wm, err);
    if (!ok) std::fprintf(stderr, "  connect err: %s\n", err.c_str());
    REQUIRE(ok);

    REQUIRE(provider.baked_tileset_count() == 1);
    // run_tileset_phase writes gtex into the cache root.
    REQUIRE(file_exists(root / ".cache" / "TinyWorld" / "TrivialGround.gtex"));
    REQUIRE(viewer::tileset_provider::get_slot(0).valid);

    viewer::tileset_provider::unload_all();
    remove_tree(root);
}

// Optional second-pass fixture: run the REAL ForestFloor tileset from the
// world_demo objects dir through LocalProvider::connect(). Gated on the env
// var MATTER_USE_REAL_FOREST_FLOOR=1 because it depends on the on-disk
// world_demo layout and produces artifacts inside temp. Combined with
// MATTER_TILESET_DUMP_PNG=1 (checked by LocalProvider itself) this is the way
// to eyeball the actual atlas without launching the viewer window.
static void test_local_provider_bakes_meadow_forestfloor() {
    // Relative to viewer/ (where the test binary is normally run from) OR
    // repo-root (build-all.sh) — try both.
    std::string src_objects = "../examples/world_demo/objects";
    std::error_code ec;
    if (!fs::is_directory(src_objects, ec)) {
        src_objects = "MatterEngine3/examples/world_demo/objects";
    }
    if (!fs::is_directory(src_objects, ec)) {
        // Try schemas as fallback for legacy layout.
        src_objects = "../examples/world_demo/schemas";
        if (!fs::is_directory(src_objects, ec))
            src_objects = "MatterEngine3/examples/world_demo/schemas";
    }

    const fs::path root = fs::temp_directory_path() / "me3_forestfloor_bake";
    remove_tree(root);
    fs::create_directories(root / "objects");
    fs::create_directories(root / "worlds");
    fs::create_directories(root / ".cache" / "Sandbox");

    // Copy the production schemas/objects verbatim.
    const char* needed[] = {
        "ForestFloor.js", "Pebble.js", "Rock.js", "Twig.js", "Leaf.js",
    };
    for (const char* fn : needed) {
        fs::path src = fs::path(src_objects) / fn;
        fs::path dst = root / "objects" / fn;
        std::error_code copy_ec;
        fs::copy_file(src, dst, copy_ec);
        if (copy_ec) {
            std::fprintf(stderr, "FAIL: could not stage %s (%s)\n", fn, copy_ec.message().c_str());
            ++g_failures;
            return;
        }
    }

    // Copy the shared-lib (Pebble.js imports shared-lib/rng).
    std::string src_shared_lib = "../shared-lib";
    if (!fs::is_directory(src_shared_lib, ec)) {
        src_shared_lib = "MatterEngine3/shared-lib";
    }
    {
        std::error_code copy_ec;
        fs::copy(src_shared_lib, (root / "shared-lib").string(),
                 fs::copy_options::recursive, copy_ec);
        if (copy_ec) {
            std::fprintf(stderr, "FAIL: could not stage shared-lib from %s (%s)\n",
                         src_shared_lib.c_str(), copy_ec.message().c_str());
            ++g_failures;
            return;
        }
    }

    // World class: single tileset root.
    write_file(root / "worlds" / "Sandbox.js",
        "class Sandbox extends World {\n"
        "  static roots = [\n"
        "    { module: 'ForestFloor', transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1], tileset: true },\n"
        "  ];\n"
        "}\n");

    auto cfg = viewer::LocalProviderConfig::for_project(root.string(), "Sandbox", "");
    cfg.gl_available = true;   // hidden raylib window is a real GL context

    viewer::LocalProvider provider(cfg);
    viewer::WorldManifest wm;
    std::string err;
    bool ok = provider.connect(wm, err);
    if (!ok) std::fprintf(stderr, "  ForestFloor connect err: %s\n", err.c_str());
    REQUIRE(ok);
    REQUIRE(provider.baked_tileset_count() == 1);
    REQUIRE(file_exists(root / ".cache" / "Sandbox" / "ForestFloor.gtex"));

    std::fprintf(stderr, "ForestFloor atlas + PNGs (if MATTER_TILESET_DUMP_PNG=1) at %s/.cache/Sandbox/\n",
                 root.string().c_str());

    viewer::tileset_provider::unload_all();
    // Do NOT remove here — the caller may want to copy the PNGs out.
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
