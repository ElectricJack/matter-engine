// Focused native coverage for source-owned singleton metadata and publication.
#include "blas_manager.hpp"
#include "part_asset_v2.h"
#include "part_bundle.h"
#include "memory_fixture_io.h"
#include "part_graph.h"
#include "render/part_store.h"
#include "tlas_manager.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
static void check(bool ok, const char *text) {
    if (!ok)
        throw std::runtime_error(text);
}
static void count_publish(const char *phase, double, size_t, uint32_t, void *user) {
    if (std::string(phase) == "bundle_atomic_replace")
        ++*static_cast<int *>(user);
}
static std::string schema(const std::string &name, const std::string &metadata) {
    return "class " + name + " extends Part {" + metadata + R"(
      build(){this.fill(8);this.beginShape(0);
        this.vertex(0,0,0);this.vertex(1,0,0);this.vertex(0,1,0);this.endShape();}
    })";
}
int main() {
    const fs::path root = "runtime/castle-singleton";
    auto storage = std::make_shared<matter::test::MemoryCacheByteStorage>();
    part_bundle::ScopedStorage scope(storage);
    const auto published_below = [&](const std::string& path) {
        const auto prefix = matter::SourceSnapshot::key(path) + "/";
        for (const auto& entry : storage->entries())
            if (entry.first.compare(0, prefix.size(), prefix) == 0) return true;
        return false;
    };
    try {
        script_host::ScriptHost host;
        host.set_module_sources(std::vector<module_resolver::ResolvedModule>{});
        script_host::BakeOptions options;
        options.parts_dir = root.string();
        options.retain_geometry = true;
        struct Case {
            const char *name;
            const char *metadata;
            bool singleton;
            bool noimp;
        };
        const Case cases[] = {
            {"Singleton", "static lodBudgets=[1];static noImpostor=true;", true, true},
            {"SingletonPicture", "static lodBudgets=[1];", true, false},
            {"Absent", "", false, false},
            {"NoImpostorOnly", "static noImpostor=true;", false, true},
            {"Multiple", "static lodBudgets=[1,.5];", false, false},
            {"Duplicate", "static lodBudgets=[1,1];", false, false},
            {"InvalidBudget", "static lodBudgets=[1,'x'];", false, false},
            {"InvalidLods", "static lodBudgets=[1];static lods=42;", true, false},
            {"AuthoredLods", "static lodBudgets=[1];static lods=[{at:0}];", false, false},
            {"FunctionalRequires", "static lodBudgets=[1];static requires(){return [];}", false,
             false},
            {"EmptyRequires", "static lodBudgets=[1];static requires=[];", true, false},
            {"MetadataGetter", "static get lodBudgets(){throw new Error('metadata only');}", false,
             false}};
        script_host::BakeResult singleton;
        for (const auto &item : cases) {
            const auto before = storage->publication_count();
            auto result = host.bake_source(schema(item.name, item.metadata), "{}", options);
            const auto publishes = storage->publication_count() - before;
            check(result.error.ok, result.error.message.c_str());
            check(bool(result.geometry), "retained geometry missing");
            check(result.leaf_metadata_published == item.singleton,
                  "metadata qualification differs");
            check(publishes == 1, "static direct bake must publish once");
            check((result.geometry->source_single_full_rep_hash == result.resolved_hash) ==
                      item.singleton,
                  "source provenance differs");
            part_asset::LodVariants variants;
            const bool has =
                part_asset::load_lod_sidecar(result.written_path, result.resolved_hash, variants);
            check(has == item.singleton, "direct bake persisted unexpected VARS");
            viewer::PartStore store(root.string(), storage);
            auto disk = store.stage_load(result.resolved_hash);
            auto memory = store.stage_from_bake(result.resolved_hash, *result.geometry);
            const size_t expected = item.singleton ? 1 : 3;
            check(disk.ok && memory.ok, "disk/memory stage failed");
            check(disk.lp.lod_blas.size() == expected && memory.lp.lod_blas.size() == expected,
                  "disk/memory ladder disagrees with source");
            if (std::string(item.name) == "Singleton")
                singleton = result;
        }
        // Runtime-only leaf preparation must succeed without creating even the
        // requested directory, and carry source validation rather than durable
        // metadata. Compare the same authored source against the persisted path.
        script_host::BakeOptions memory_options;
        memory_options.parts_dir = (root / "must-not-exist").string();
        memory_options.output_mode = script_host::BakeOptions::OutputMode::RuntimeLeafMemory;
        const auto before_memory = storage->publication_count();
        auto prepared = host.bake_source(
            schema("Singleton", "static lodBudgets=[1];static noImpostor=true;"),
            "{}", memory_options);
        const auto memory_publishes = storage->publication_count() - before_memory;
        check(prepared.error.ok && prepared.geometry && prepared.resolved_hash == singleton.resolved_hash,
              "runtime leaf preparation failed or changed content identity");
        check(memory_publishes == 0 && !published_below(memory_options.parts_dir) &&
                  prepared.written_path.empty() && !prepared.leaf_metadata_published &&
                  prepared.geometry->source_single_full_rep_hash == 0 &&
                  prepared.geometry->validated_single_full_rep_hash == prepared.resolved_hash,
              "runtime preparation claimed or created a durable artifact");
        viewer::PartStore store(root.string(), storage);
        auto prepared_stage = store.stage_from_bake(prepared.resolved_hash, *prepared.geometry);
        auto persisted_stage = store.stage_load(singleton.resolved_hash);
        std::string difference;
        check(prepared_stage.ok && prepared_stage.lp.lod_blas.size() == 1 &&
                  viewer::staged_parts_equal(prepared_stage, persisted_stage, &difference),
              ("prepared/persisted stage differs: " + difference).c_str());
        check(!store.stage_from_bake(prepared.resolved_hash ^ 1, *prepared.geometry).ok,
              "foreign memory-only policy hash accepted");
        for (const auto& item : cases) {
            if (item.singleton && std::string(item.name) != "InvalidLods") continue;
            auto excluded = host.bake_source(schema(item.name, item.metadata), "{}", memory_options);
            check(!excluded.error.ok && !excluded.geometry && excluded.written_path.empty(),
                  "excluded metadata entered memory-only preparation");
            check(!published_below(memory_options.parts_dir), "excluded preparation created directory");
        }
        auto rig = host.bake_source(R"(class RigLeaf extends Part {
            static lodBudgets=[1];
            build(){this.beginRig('r');this.root('root');this.bone('tip',[1,0,0]);this.endRig();}
        })", "{}", memory_options);
        check(!rig.error.ok && !rig.geometry && !published_below(memory_options.parts_dir),
              "canonical rig entered static memory-only preparation");
        auto emitter = host.bake_source(R"(class EmitterLeaf extends Part {
            static lodBudgets=[1];
            build(){this.emitVolume({pos:[0,0,0],radius:1,length:1,density:1});}
        })", "{}", memory_options);
        check(!emitter.error.ok && emitter.error.code == "runtime-leaf-memory-ineligible" &&
                  !emitter.geometry && !published_below(memory_options.parts_dir),
              "emitter entered static memory-only preparation");
        check(store.stage_load(singleton.resolved_hash, 0, true).lp.lod_blas.size() == 3,
              "terrain assertion changed ladder");
        check(store.stage_from_bake(singleton.resolved_hash, *singleton.geometry, 0, true)
                      .lp.lod_blas.size() == 3,
              "inmemory terrain assertion changed ladder");
        check(!store.stage_from_bake(singleton.resolved_hash ^ 1, *singleton.geometry).ok,
              "foreign in-memory provenance accepted");
        // An ANLK-bearing source is not a plain-static fallback, even if VARS
        // remains present. An uncommitted link must fail staging, never downgrade.
        BLASManager blas;
        TLASManager tlas;
        std::vector<part_asset::ChildInstance> kids;
        part_asset::LodLevels lods;
        check(part_asset::load_v2(singleton.written_path, singleton.resolved_hash, blas, tlas, kids,
                                  lods),
              "decode for ANLK probe failed");
        part_asset::PartAnimationLink link;
        link.resolved_hash = singleton.resolved_hash;
        link.nonce_high = 1;
        link.nonce_low = 1;
        check(part_asset::save_v2(singleton.written_path, blas, tlas, nullptr, 0, lods, {}, link,
                                  singleton.resolved_hash),
              "ANLK probe save failed");
        check(!store.stage_load(singleton.resolved_hash).ok,
              "ANLK was downgraded to singleton static");
        // Real graph cold install must perform ONE publish including all four
        // canonical sections. A later cache hit cannot reuse stale metadata flags.
        const auto graph_root = root / "graph";
        matter::SourceSnapshot::Files files;
        files[(root / "schemas/GraphLeaf.js").generic_string()] =
            schema("GraphLeaf", "static lodBudgets=[1];static noImpostor=true;");
        auto sources = std::make_shared<const matter::SourceSnapshot>(std::move(files));
        part_graph::FileModuleResolver resolver(host, (root / "schemas").string(), sources);
        part_graph::HostBaker baker(host, graph_root.string(), storage);
        part_graph::PartGraph graph(resolver, baker);
        const auto before_graph = storage->publication_count();
        auto installed = graph.install({{"GraphLeaf", {}}});
        const auto publishes = storage->publication_count() - before_graph;
        check(installed.ok && installed.failed.empty() && installed.baked.size() == 1,
              "graph install failed");
        check(publishes == 1, "graph singleton did not coalesce metadata");
        const auto hash = installed.root_hashes[0];
        const auto path = (graph_root / part_asset::cache_path_resolved(hash)).string();
        check(part_bundle::remove_section(path, hash, part_bundle::kSectionVariants),
              "remove VARS fixture failed");
        installed = graph.install({{"GraphLeaf", {}}});
        part_asset::LodVariants repaired;
        check(installed.ok && installed.failed.empty() && installed.hits == 1 &&
                  part_asset::load_lod_sidecar(path, hash, repaired),
              "cache hit did not repair removed metadata");
        // Valid bundle checksum is insufficient: a foreign variant or trailing
        // garbage must never prove source singleton authoring.
        part_asset::LodVariants foreign;
        foreign.budgets = {1};
        foreign.hashes = {hash ^ 1};
        check(part_asset::save_lod_sidecar(path, hash, foreign), "foreign VARS fixture failed");
        viewer::PartStore graph_store(graph_root.string(), storage);
        check(graph_store.stage_load(hash).lp.lod_blas.size() == 3,
              "foreign VARS granted singleton");
        char malformed[128];
        std::snprintf(malformed, sizeof malformed, "0\n1 %016llx\ninvalid\n",
                      (unsigned long long)hash);
        check(part_bundle::write_section(path, hash, part_bundle::kSectionVariants, malformed,
                                         std::char_traits<char>::length(malformed)),
              "malformed VARS fixture failed");
        check(graph_store.stage_load(hash).lp.lod_blas.size() == 3,
              "trailing malformed VARS granted singleton");
        std::puts("castle_singleton_tests: PASS");
        return 0;
    } catch (const std::exception &e) {
        part_bundle::set_write_observer(nullptr);
        std::fprintf(stderr, "castle_singleton_tests: FAIL: %s (retained %s)\n", e.what(),
                     root.string().c_str());
        return 1;
    }
}
