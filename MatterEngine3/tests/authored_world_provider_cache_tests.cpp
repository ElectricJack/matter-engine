#include "authored_world_cache.h"
#include "memory_fixture_io.h"
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
    const fs::path dir = "/matter-authored-provider";
    const auto path = dir / "worlds" / "Cached.js";
    matter::SourceSnapshot::Files files;
    auto storage = std::make_shared<matter::test::MemoryCacheByteStorage>();
    auto write = [&](const std::string& source) { files[path.generic_string()] = source; };
    auto snapshot = [&]() { return std::make_shared<const matter::SourceSnapshot>(
        files, std::vector<std::string>{(dir / "objects").generic_string(),
                                       (dir / "worlds").generic_string()}); };
    auto io = [&]() { auto owned = std::make_shared<matter::WorldFixtureIO>();
        owned->sources = snapshot(); owned->cache = storage; return owned; };
    const std::string source =
        "const M=defineMaterial('cached-wall',{roughness:0.42}); class Cached extends "
        "World {static roots=[]; static props={speed:{default:2,min:0,max:7}};}";
    write(source);
    matter::WorldLoadDesc desc;
    desc.world_path = path.string();
    desc.objects_dir = (dir / "objects").string();
    desc.source_snapshot = snapshot();
    matter::WorldDefinition world;
    matter::WorldLoadError loadError;
    check(matter::load_world_definition(desc, world, loadError), "cold authored load");
    std::string blob;
    check(authored_world_cache::capture(world, blob), "capture loaded world");
    const auto key =
        resolve_cache::compute_key(path.string(), "", desc.objects_dir, "", "", snapshot().get());
    // Exercise the direct provider restore seam with a source that cannot execute.
    // The real engine's key gate rejects this changed source (checked below).
    write("throw new Error('world JS must not run in direct snapshot restore'); class "
          "Cached extends World {}");
    check(key !=
              resolve_cache::compute_key(path.string(), "", desc.objects_dir, "", "", snapshot().get()),
          "source edit invalidates outer cache key");
    auto cfg = viewer::LocalProviderConfig::for_project(dir.string(), "Cached", "", io());
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
    viewer::LocalProvider valid_provider(viewer::LocalProviderConfig::for_project(
        dir.string(), "Cached", "", io()));
    check(valid_provider.restore_from_cache(graph, plan, roots, error, corrupt),
          "corrupt snapshot falls back to valid JS");
    check(valid_provider.world_prop_specs().size() == 1 &&
              valid_provider.world_materials().size() == 1,
          "fallback restores declarations");
    MaterialRegistryResetDynamic();

    std::printf("authored_world_provider_cache_tests: %s (%d failures)\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
