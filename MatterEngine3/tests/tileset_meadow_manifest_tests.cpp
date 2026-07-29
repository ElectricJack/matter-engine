// tileset_meadow_manifest_tests.cpp — verify Meadow's world definition declares
// ForestFloor as a tileset root. Non-GL test (parses the World class JS file only;
// no bake).
//
// Migrated from legacy read_manifest to load_world_definition (project-root layout).

#include "script/world_definition_loader.h"
#include "detail_bake_plan.h"
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
    fs::path worlds_dir = "../../projects/world_demo/worlds";
    std::error_code ec;
    if (!fs::is_directory(worlds_dir, ec)) {
        worlds_dir = "projects/world_demo/worlds";
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

    // chart-VT Phase 3: `tileset: true` is now an ALIAS into the generalized
    // detail-bake plan rather than its own hardwired path. Shipped worlds must
    // therefore produce exactly what the old hardcoded loop produced — one bake
    // per tileset root, bound to material 16 and to nothing else — and must not
    // pick up any dynamic materials they never declared.
    for (const char* world : {"Meadow.js", "StreamMountain.js", "FloorDemo.js"}) {
        matter::WorldDefinition legacy;
        matter::WorldLoadError legacy_err;
        matter::WorldLoadDesc legacy_desc;
        legacy_desc.world_path = (worlds_dir / world).string();
        if (!matter::load_world_definition(legacy_desc, legacy, legacy_err)) {
            std::fprintf(stderr, "  %s: %s\n", world, legacy_err.message.c_str());
            REQUIRE(false);
            continue;
        }
        REQUIRE(legacy.materials.empty());   // no defineMaterial in shipped worlds

        std::vector<tileset::DetailBakeRoot> roots;
        for (const matter::WorldRoot& root : legacy.roots)
            if (root.tileset) roots.push_back({root.module, root.params_json});
        const std::vector<tileset::DetailBakeRequest> plan =
            tileset::plan_detail_bakes(roots, legacy.materials);
        REQUIRE(plan.size() == roots.size());
        for (size_t i = 0; i < plan.size() && i < roots.size(); ++i) {
            REQUIRE(plan[i].module == roots[i].module);
            REQUIRE(plan[i].from_tileset_root);
            REQUIRE(plan[i].texels_per_meter == 0);
            REQUIRE(plan[i].materials.size() == 1);
            REQUIRE(plan[i].materials[0] == tileset::kDeprecatedTilesetRootMaterial);
        }
    }

    std::fprintf(stderr, "tileset_meadow_manifest_tests: %d run, %d failed\n",
                 g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
