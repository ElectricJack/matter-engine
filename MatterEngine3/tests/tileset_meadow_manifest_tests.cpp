// tileset_meadow_manifest_tests.cpp — verify Meadow's world definition declares
// ForestFloor as a tileset root. Non-GL test (parses the World class JS file only;
// no bake).
//
// Migrated from legacy read_manifest to load_world_definition (project-root layout).

#include "script/world_definition_loader.h"
#include "detail_bake_plan.h"
#include "tileset_gtex.h"   // tileset::kMaxTilesetSlots (slot-count source of truth)
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
    fs::path scenes_dir = "../../projects/world_demo/scenes";
    fs::path engine_shared_lib = "../shared-lib";
    std::error_code ec;
    if (!fs::is_directory(scenes_dir, ec)) {
        scenes_dir = "projects/world_demo/scenes";
        engine_shared_lib = "MatterEngine3/shared-lib";
    }
    // A world's own `objects/` is never read here (no bake, no roots resolved),
    // but its SHARED-LIB imports are: load_world_definition folds every
    // `import ... from 'shared-lib/x'` before it evaluates the class, so a
    // world that imports anything fails to load outright without these roots.
    // StreamMountain has imported `shared-lib/alpine_ecology` since 2026-07-30
    // (6e51d3c6), which is exactly how long this suite had been red.
    const fs::path project_shared_lib = scenes_dir.parent_path() / "shared-lib";
    auto fill_lib_roots = [&](matter::WorldLoadDesc& d) {
        d.project_shared_lib_dir = project_shared_lib.string();
        d.engine_shared_lib_dir = engine_shared_lib.string();
    };

    const fs::path world_path = scenes_dir / "Meadow" / "Meadow.js";
    REQUIRE(fs::exists(world_path, ec));

    matter::WorldLoadDesc desc;
    desc.world_path = world_path.string();
    fill_lib_roots(desc);

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
    for (const char* world : {"Meadow.js", "FloorDemo.js"}) {
        matter::WorldDefinition legacy;
        matter::WorldLoadError legacy_err;
        matter::WorldLoadDesc legacy_desc;
        legacy_desc.world_path =
            (scenes_dir / fs::path(world).stem() / world).string();
        fill_lib_roots(legacy_desc);
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

    // StreamMountain migrated to defineMaterial (chart-VT Phase 4): it declares
    // five alpine materials AND keeps the deprecated `tileset: true` ForestFloor
    // root. That combination is only sound because plan_detail_bakes() merges
    // the root with AlpineGround's identical `detail: "ForestFloor"` request —
    // one settle, one .gtex, one slot, two bound materials (16 + AlpineGround).
    // Assert exactly that, so a future edit that splits them (a detailDensity
    // on AlpineGround, params on the root) is caught here rather than as a
    // doubled 350 s settle and a wasted slot at runtime.
    {
        matter::WorldDefinition mountain;
        matter::WorldLoadError mountain_err;
        matter::WorldLoadDesc mountain_desc;
        mountain_desc.world_path =
            (scenes_dir / "StreamMountain" / "StreamMountain.js").string();
        fill_lib_roots(mountain_desc);
        const bool mountain_ok = matter::load_world_definition(
            mountain_desc, mountain, mountain_err);
        if (!mountain_ok)
            std::fprintf(stderr, "  StreamMountain.js: %s\n",
                         mountain_err.message.c_str());
        REQUIRE(mountain_ok);

        // AlpineMeadow is the FIFTH and it is declared LAST on purpose:
        // declaration order is the slot order, so a new material goes on the
        // end and the existing four keep their indices (see the comment above
        // its defineMaterial in StreamMountain.js). It arrived with its
        // AlpineMeadowDetail tileset in 96b7859d (2026-07-30); this expectation
        // list, written for the four that existed before it, is what went
        // stale.
        constexpr size_t kMountainMaterials = 5;
        REQUIRE(mountain.materials.size() == kMountainMaterials);
        const char* const expect_names[kMountainMaterials] = {
            "AlpineGround", "AlpineRock", "Scree", "AlpineSnow", "AlpineMeadow"};
        const char* const expect_detail[kMountainMaterials] = {
            "ForestFloor", "AlpineRockDetail", "ScreeDetail", "AlpineSnowDetail",
            "AlpineMeadowDetail"};
        for (size_t i = 0;
             i < mountain.materials.size() && i < kMountainMaterials; ++i) {
            REQUIRE(mountain.materials[i].name == expect_names[i]);
            REQUIRE(mountain.materials[i].detail_module == expect_detail[i]);
            REQUIRE(mountain.materials[i].detail_density == 0);
            REQUIRE(mountain.materials[i].index >= 0);
        }

        std::vector<tileset::DetailBakeRoot> mountain_roots;
        for (const matter::WorldRoot& root : mountain.roots)
            if (root.tileset)
                mountain_roots.push_back({root.module, root.params_json});
        REQUIRE(mountain_roots.size() == 1);

        const std::vector<tileset::DetailBakeRequest> plan =
            tileset::plan_detail_bakes(mountain_roots, mountain.materials);
        // One bake per DECLARED detail scene, not one more: the deprecated
        // ForestFloor root folded into AlpineGround's identical request.
        REQUIRE(plan.size() == kMountainMaterials);
        if (plan.size() == kMountainMaterials) {
            REQUIRE(plan[0].module == "ForestFloor");
            REQUIRE(plan[0].from_tileset_root);
            REQUIRE(plan[0].materials.size() == 2);
            REQUIRE(plan[0].materials[0] ==
                    tileset::kDeprecatedTilesetRootMaterial);
            REQUIRE(plan[0].materials[1] == mountain.materials[0].index);
            // The merged ForestFloor request stays FIRST: run_tileset_deferred
            // stops at the first settle failure, and this is the one entry that
            // carries the deprecated material-16 binding the non-VT path still
            // samples, so it must not be stranded behind another scene.
            for (size_t i = 1; i < plan.size(); ++i) {
                REQUIRE(plan[i].module == expect_detail[i]);
                REQUIRE(!plan[i].from_tileset_root);
                REQUIRE(plan[i].materials.size() == 1);
                REQUIRE(plan[i].materials[0] == mountain.materials[i].index);
            }
        }
        // Every declared detail scene must fit the slot pool with room to
        // spare; sharing ForestFloor is what keeps that true.
        REQUIRE((int)plan.size() <= tileset::kMaxTilesetSlots);
    }

    std::fprintf(stderr, "tileset_meadow_manifest_tests: %d run, %d failed\n",
                 g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
