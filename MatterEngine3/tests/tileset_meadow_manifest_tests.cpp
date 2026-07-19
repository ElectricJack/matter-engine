// tileset_meadow_manifest_tests.cpp — verify Meadow's world definition declares
// ForestFloor as a tileset root. Non-GL test (parses the World class JS file only;
// no bake).
//
// Migrated from legacy read_manifest to load_world_definition (project-root layout).

#include "script/world_definition_loader.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "check.h"
static int g_tests = 0;
#define REQUIRE(cond) do { \
    ++g_tests; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

namespace fs = std::filesystem;

int main() {
    // Try both paths: relative to viewer/ (normal test run) and relative to
    // repo-root (build-all.sh).
    fs::path worlds_dir = "../examples/world_demo/worlds";
    std::error_code ec;
    if (!fs::is_directory(worlds_dir, ec)) {
        worlds_dir = "MatterEngine3/examples/world_demo/worlds";
    }

    const fs::path world_path = worlds_dir / "Meadow.js";
    REQUIRE(fs::exists(world_path, ec));

    matter::WorldLoadDesc desc;
    desc.world_path = world_path.string();
    // objects_dir and shared-lib dirs not needed for just parsing roots.

    matter::WorldDefinition def;
    matter::WorldLoadError load_err;
    bool ok = matter::load_world_definition(desc, def, load_err);
    if (!ok) std::fprintf(stderr, "  load_world_definition err: %s\n", load_err.message.c_str());
    REQUIRE(ok);

    bool saw_forest_floor_tileset = false;
    for (size_t i = 0; i < def.roots.size(); ++i) {
        if (def.roots[i].module == "ForestFloor") {
            REQUIRE(def.roots[i].tileset);
            REQUIRE(!def.roots[i].expand);
            saw_forest_floor_tileset = true;
        }
    }
    REQUIRE(saw_forest_floor_tileset);

    std::fprintf(stderr, "tileset_meadow_manifest_tests: %d run, %d failed\n",
                 g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
