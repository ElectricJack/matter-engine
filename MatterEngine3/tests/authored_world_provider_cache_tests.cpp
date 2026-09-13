#include "authored_world_cache.h"
#include "provider/local_provider.h"
#include "resolve_cache.h"
#include "script/world_definition_loader.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL %s\n", what);
            ++failures;
        }
    };
    const auto dir =
        fs::temp_directory_path() /
        ("matter-authored-provider-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir / "objects");
    fs::create_directories(dir / "worlds");
    const auto path = dir / "worlds" / "Cached.js";
    auto write = [&](const std::string& s) {
        std::ofstream f(path);
        f << s;
    };
    const std::string source =
        "const M=defineMaterial('cached-wall',{roughness:0.42}); class Cached extends "
        "World {static roots=[]; static props={speed:{default:2,min:0,max:7}};}";
    write(source);
    matter::WorldLoadDesc desc;
    desc.world_path = path.string();
    desc.objects_dir = (dir / "objects").string();
    matter::WorldDefinition world;
    matter::WorldLoadError loadError;
    check(matter::load_world_definition(desc, world, loadError), "cold authored load");
    std::string blob;
    check(authored_world_cache::capture(world, blob), "capture loaded world");
    const auto key =
        resolve_cache::compute_key(path.string(), "", desc.objects_dir, "", "");
    // Exercise the direct provider restore seam with a source that cannot execute.
    // The real engine's key gate rejects this changed source (checked below).
    write("throw new Error('world JS must not run in direct snapshot restore'); class "
          "Cached extends World {}");
    check(key !=
              resolve_cache::compute_key(path.string(), "", desc.objects_dir, "", ""),
          "source edit invalidates outer cache key");
    auto cfg = viewer::LocalProviderConfig::for_project(dir.string(), "Cached", "");
    cfg.cache_root = (dir / "cache").string();
    viewer::LocalProvider provider(cfg);
    std::string error;
    part_graph_snapshot::Snapshot graph;
    std::unordered_map<uint64_t, part_graph::BakeInputs> plan;
    std::vector<uint64_t> roots;
    std::puts("authored_world_provider_cache_tests: restore bypasses throwing source");
    check(provider.restore_from_cache(graph, plan, roots, error, blob),
          "restore avoids JS");
    check(provider.authored_world_cache() == blob,
          "validated blob retained without recapture");
    check(provider.world_prop_specs().size() == 1 &&
              provider.world_prop_specs()[0].max == 7,
          "runtime prop declarations retained");
    check(provider.world_materials().size() == 1 &&
              MaterialRegistryFindByName("cached-wall") ==
                  MaterialRegistryStaticCount(),
          "material handles replayed");
    auto corrupt = blob;
    corrupt[15] ^= 1;
    check(!provider.restore_from_cache(graph, plan, roots, error, corrupt),
          "corrupt snapshot falls back to throwing JS");
    check(MaterialRegistryDynamicCount() == 0, "fallback clears prior registry");
    write(source);
    check(provider.restore_from_cache(graph, plan, roots, error, corrupt),
          "corrupt snapshot falls back to valid JS");
    check(provider.world_prop_specs().size() == 1 &&
              provider.world_materials().size() == 1,
          "fallback restores declarations");
    MaterialRegistryResetDynamic();
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    std::printf("authored_world_provider_cache_tests: %s (%d failures)\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
