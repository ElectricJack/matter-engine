#include "check.h"
#include "memory_fixture_io.h"
#include "../src/part_bundle.h"
#include "../src/source_snapshot_modules.h"
#include <atomic>
#include <thread>

static void source_tier_guards() {
    matter::SourceSnapshot::Files files{
        {"/project/shared-lib/leaf.js", "import { b } from 'shared-lib/branch.js'; export const a = 1;"},
        {"/engine/shared-lib/leaf.js", "export const a = 2;"},
        {"/engine/shared-lib/branch.js", "import { a } from 'shared-lib/leaf'; export const b = 3;"},
        {"/project/shared-lib/empty.js", ""},
        {"/project/shared-lib/nested/escape.js", "nested"},
        {"/project/shared-lib/.hidden.js", "hidden"}};
    const std::vector<std::string> roots{"/project/shared-lib", "/engine/shared-lib"};
    const matter::SourceSnapshot before(files);
    const auto modules = matter::source_snapshot_modules(before, roots);
    CHECK(modules.size() == 3, "shared module tiers exclude hidden and nested inputs");
    const auto fold = [](const std::string& source, const auto& selected,
                         module_resolver::FoldResult& output, std::string& error) {
        return module_resolver::fold_sources_with_loader(source,
            [&selected](const std::string& specifier, module_resolver::ResolvedModule& module,
                        std::string& reason) {
                for (const auto& candidate : selected)
                    if (candidate.specifier == specifier) { module = candidate; return true; }
                reason = "missing owned module";
                return false;
            }, output, error);
    };
    module_resolver::FoldResult original, alias, empty, changed;
    std::string error;
    const std::string source = "import { a } from 'shared-lib/leaf';";
    CHECK(fold(source, modules, original, error), "actual transitive fold closes a cycle");
    CHECK(original.modules.size() == 2 &&
              original.modules[1].source == files.at("/project/shared-lib/leaf.js") &&
              original.modules[1].source_path == "/project/shared-lib/leaf.js",
          "project tier bytes and diagnostic path win over engine duplicates");
    CHECK(fold("import { a } from 'shared-lib/leaf.js';", modules, alias, error) &&
              alias.resolved_specifiers == original.resolved_specifiers &&
              alias.modules.size() == 2,
          "canonical .js alias deduplicates transitive cyclic modules");
    CHECK(fold("import { a } from 'shared-lib/empty';", modules, empty, error) &&
              empty.modules.size() == 1 && empty.modules[0].source.empty(),
          "present empty module bytes remain distinct from absence");
    CHECK(!fold("import { a } from 'shared-lib/missing';", modules, empty, error) &&
              !fold("import { a } from 'shared-lib/../escape';", modules, empty, error),
          "missing and illegal imports fail closed without filesystem fallback");
    files["/project/shared-lib/leaf.js"] += "\n// next source generation\n";
    const auto next = matter::source_snapshot_modules(matter::SourceSnapshot(files), roots);
    CHECK(fold(source, next, changed, error) && changed.folded != original.folded &&
              modules[2].source == "import { b } from 'shared-lib/branch.js'; export const a = 1;",
          "next generation changes the actual hash fold while prior owned bytes survive");
    CHECK(matter::source_snapshot_modules(before, {}).empty(),
          "explicit empty roots do not import unrelated sources");
}

int main() {
    source_tier_guards();
    using namespace part_bundle;
    matter::SourceSnapshot sources({
        {"/project/scenes/group/Demo/Demo.js", "world"},
        {"/project/scenes/group/Demo/objects/Fake/Fake.js", "object"},
        {"/project/objects/group/Root.js", "root"},
        {"/project/objects/.hidden/Ignore.js", "hidden"}},
        {"/project/shared-lib", "/project/objects/empty"});
    CHECK(sources.scene_script("/project", "Demo") == "/project/scenes/group/Demo/Demo.js",
          "grouped scene is discovered from immutable metadata");
    CHECK(sources.scene_script("/project", "Fake").empty(), "scene contents are not discovered as scenes");
    CHECK(sources.is_directory("/project/shared-lib"), "empty directory metadata is retained");
    CHECK(sources.object_roots({"/project/objects"}) == std::vector<std::string>({
        "/project/objects", "/project/objects/empty", "/project/objects/group"}),
        "object roots retain sorted groups, empty directories and hidden exclusion");
    std::string text;
    CHECK(!sources.read("/project/missing.js", text), "missing owned source fails closed");

    auto cache = std::make_shared<matter::test::MemoryCacheByteStorage>();
    const std::string path = "/virtual/cache/parts/123.bundle";
    constexpr uint64_t hash = 123;
    const std::vector<uint8_t> first{1, 2, 3, 4};
    ScopedStorage scope(cache);
    CHECK(write_section(path, hash, kSectionRep0, first.data(), first.size()), "production bundle codec publishes in RAM");
    const auto old_generation = cache->open(path);
    cache->fail_publication(true);
    CHECK(!write_section(path, hash, kSectionRep0, "bad", 3), "publication failure propagates");
    CHECK(cache->open(path) == old_generation, "failed publication retains previous generation");
    cache->fail_publication(false);

    std::atomic<bool> worker_ok{true};
    std::atomic<bool> inherited{false};
    auto write = [cache, &path, &worker_ok, &inherited](uint32_t tag) {
        if (ScopedStorage::current()) inherited = true;
        ScopedStorage operation(cache);
        const uint8_t payload = static_cast<uint8_t>(tag);
        if (!write_section(path, hash, tag, &payload, 1)) worker_ok = false;
    };
    std::thread a(write, kSectionFlat), b(write, kSectionPlan);
    a.join(); b.join();
    CHECK(worker_ok && !inherited, "workers bind retained storage explicitly");
    CHECK(section_tags(path).size() == 3, "concurrent section merges preserve all sections");
    CHECK(cache->open(path) != old_generation, "successful publication replaces generation");
    std::vector<uint8_t> payload;
    CHECK(read_section(path, hash, kSectionRep0, payload) && payload == first, "original payload survives concurrent merges");
    uint64_t length = 0;
    CHECK(read_section_prefix(path, hash, kSectionRep0, 2, payload, length) &&
          payload == std::vector<uint8_t>({1, 2}) && length == 4, "bounded prefix uses real directory grammar");
    CHECK(!read_section(path, hash + 1, kSectionRep0, payload), "wrong resolved hash is rejected");
    auto isolated = std::make_shared<matter::test::MemoryCacheByteStorage>();
    {
        ScopedReadSnapshot snapshot(path, hash);
        CHECK(ScopedReadSnapshot::for_path(path) == &snapshot, "snapshot is selected within its store");
        ScopedStorage other(isolated);
        CHECK(!has_section(path, hash, kSectionRep0), "same logical path in another cache is isolated");
        CHECK(!ScopedReadSnapshot::for_path(path), "snapshots cannot cross stores");
    }
    auto corrupt = *cache->open(path);
    corrupt.back() ^= 1;
    CHECK(cache->publish(path, corrupt), "corrupt fixture is published through normal byte boundary");
    CHECK(!read_section(path, hash, kSectionRep0, payload), "full codec rejects corrupted section checksums");
    CHECK(cache->publish(path, *old_generation), "old immutable generation remains usable");
    CHECK(remove_section(path, hash, kSectionRep0) && !cache->exists(path), "removing final section erases cache entry");
    return check_summary();
}
