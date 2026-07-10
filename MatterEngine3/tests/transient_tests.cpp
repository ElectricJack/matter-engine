// Phase C Task 2: transient artifact routing (tmpfs scratch dir)
//
// Transient modules (e.g., terrain sectors) bake to a per-process scratch dir
// (/tmp/matter_transient/<pid>/) and are released via release_transient.
// Persistent modules (e.g., rocks, trees) use the normal parts cache.
//
// Tests:
//   (a) bake a transient module and verify the artifact lands in scratch, not cache
//   (b) verify PartStore::get_or_load finds the artifact via scratch lookup
//   (c) release_transient unlinks the scratch file
//   (d) bake a persistent module and verify its artifact lands in cache (unchanged)

#include "check.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Include the provider/graph headers the demand_bake_tests suite uses
#include "../src/provider/local_provider.h"
#include "../src/part_graph.h"
#include "../src/part_asset_v2.h"
#include "../src/render/part_store.h"

static bool file_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

// Build a sandbox with transient (Terrain) and persistent (Rock) modules.
// Terrain: transient leaf part (no requires)
// Rock: persistent leaf part (no requires)
static bool build_transient_sandbox(const std::string& root) {
    // Clean up and create dirs
    std::system(("rm -rf " + root).c_str());
    std::system(("mkdir -p " + root + "/schemas").c_str());
    std::system(("mkdir -p " + root + "/world_data/Demo").c_str());
    std::system(("mkdir -p " + root + "/shared-lib").c_str());
    std::system(("mkdir -p " + root + "/cache/parts").c_str());

    // Terrain — transient leaf (no requires)
    {
        std::ofstream f(root + "/schemas/Terrain.js");
        if (!f) return false;
        f << "class Terrain extends Part {\n"
          << "  build(p) {\n"
          << "    this.fill(MAT.stone);\n"
          << "    const S = 0.5;\n"
          << "    this.beginShape(SHAPE.triangles);\n"
          << "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
          << "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
          << "    this.endShape();\n"
          << "  }\n"
          << "}\n";
    }

    // Rock — persistent leaf (no requires)
    {
        std::ofstream f(root + "/schemas/Rock.js");
        if (!f) return false;
        f << "class Rock extends Part {\n"
          << "  build(p) {\n"
          << "    this.fill(MAT.stone);\n"
          << "    const S = 0.3;\n"
          << "    this.beginShape(SHAPE.triangles);\n"
          << "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
          << "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
          << "    this.endShape();\n"
          << "  }\n"
          << "}\n";
    }

    // Manifest: two roots (Terrain and Rock)
    {
        std::ofstream f(root + "/world_data/Demo/world.manifest");
        if (!f) return false;
        f << "# transient artifact routing test\n"
          << "Terrain\n"
          << "Rock\n";
    }

    return true;
}

// Run install_graph() on the sandbox using LocalProvider directly (headless).
static std::unique_ptr<viewer::LocalProvider> make_provider(
    const std::string& sandbox,
    const std::string& world_name = "Demo") {
    viewer::LocalProviderConfig cfg;
    cfg.schemas_dir    = sandbox + "/schemas";
    cfg.world_data_dir = sandbox + "/world_data";
    cfg.world_name     = world_name;
    cfg.shared_lib_dir = sandbox + "/shared-lib";
    cfg.cache_root     = sandbox + "/cache";
    cfg.gl_available   = false;
    return std::make_unique<viewer::LocalProvider>(std::move(cfg));
}

// Main test: fixtures and assertions from the brief
int main() {
    std::string sandbox = "/tmp/transient_test_" + std::to_string(getpid());

    // Arrange a provider exactly the way demand_bake_tests.cpp does
    if (!build_transient_sandbox(sandbox)) {
        printf("FAIL: build_transient_sandbox\n");
        return 1;
    }

    auto prov = make_provider(sandbox);

    // Set transient modules: Terrain bakes to scratch, Rock to cache
    std::set<std::string> transient_mods;
    transient_mods.insert("Terrain");
    prov->set_transient_modules(transient_mods);

    // 1. Install graph and bake
    std::string err;
    bool ok = prov->install_graph(err);
    CHECK(ok, "install_graph() succeeded");
    if (!ok) {
        printf("  err: %s\n", err.c_str());
        return 1;
    }

    const std::string cache_root = sandbox + "/cache";
    const auto& snap = prov->graph_snapshot();

    // Get Terrain and Rock hashes
    uint64_t terrain_hash = 0, rock_hash = 0;
    {
        auto it = snap.nodes.find("Terrain");
        if (it != snap.nodes.end()) terrain_hash = it->second.resolved_hash;
        it = snap.nodes.find("Rock");
        if (it != snap.nodes.end()) rock_hash = it->second.resolved_hash;
    }

    CHECK(terrain_hash != 0, "Terrain hash resolved");
    CHECK(rock_hash != 0, "Rock hash resolved");

    printf("  terrain_hash=%016llx, rock_hash=%016llx\n",
           (unsigned long long)terrain_hash, (unsigned long long)rock_hash);

    // 2. Verify Terrain artifact is in scratch dir, not cache
    const std::string scratch_terrain = prov->transient_dir() + "/" +
                                        part_asset::cache_path_resolved(terrain_hash);
    const std::string cache_terrain = cache_root + "/" +
                                      part_asset::cache_path_resolved(terrain_hash);

    CHECK(file_exists(scratch_terrain),
          "Terrain .part exists under scratch dir");
    CHECK(!file_exists(cache_terrain),
          "Terrain .part does NOT exist under cache dir");
    printf("  Terrain transient file: %s\n", scratch_terrain.c_str());

    // 3. Verify PartStore::get_or_load finds Terrain via scratch lookup
    // Create a test PartStore with the scratch dir configured
    viewer::PartStore store(cache_root);
    store.set_scratch_dir(prov->transient_dir());

    // Verify scratch-first has() lookup works
    CHECK(store.has(terrain_hash),
          "PartStore::has(terrain_hash) returns true via scratch-first lookup");

    const viewer::LoadedPart* loaded_terrain = store.get_or_load(terrain_hash);
    CHECK(loaded_terrain != nullptr, "PartStore::get_or_load(terrain_hash) succeeded via scratch");
    if (loaded_terrain == nullptr) {
        printf("  Failed to load Terrain from scratch\n");
        return 1;
    }

    // 4. Release Terrain and verify scratch file is gone
    prov->release_transient(terrain_hash);
    CHECK(!file_exists(scratch_terrain),
          "Terrain scratch file deleted after release_transient");
    printf("  Terrain scratch file deleted\n");

    // 5. Verify Rock's artifact IS in cache (unchanged for persistent modules)
    const std::string cache_rock = cache_root + "/" +
                                   part_asset::cache_path_resolved(rock_hash);
    CHECK(file_exists(cache_rock),
          "Rock .part exists under cache dir (persistent module)");
    printf("  Rock cache file: %s\n", cache_rock.c_str());

    // Cleanup
    std::system(("rm -rf " + sandbox).c_str());

    printf("ALL TESTS PASSED\n");
    return check_summary();
}
