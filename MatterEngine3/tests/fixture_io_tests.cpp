#include "check.h"
#include "memory_fixture_io.h"
#include "../src/part_bundle.h"
#include "../src/source_snapshot_modules.h"
#include "../src/terrain_collision/fixture_artifact_storage.h"
#include <atomic>
#include <functional>
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

static void collision_storage_guards() {
    auto cache = std::make_shared<matter::test::MemoryCacheByteStorage>();
    matter::terrain_collision::detail::FixtureArtifactStorage storage(cache);
    CHECK(cache->publish("/collision/left", {1, 2}) &&
          cache->publish("/collision/right", {3, 4}), "collision stages use owned bytes");
    const auto retained_left = cache->open("/collision/left");
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::uint32_t result[2] = {99, 99};
    auto publish = [&](int index, const char* source) {
        ++ready;
        while (!go.load()) std::this_thread::yield();
        result[index] = storage.move(source, "/collision/winner", false);
    };
    std::thread left(publish, 0, "/collision/left");
    std::thread right(publish, 1, "/collision/right");
    while (ready.load() != 2) std::this_thread::yield();
    go = true;
    left.join(); right.join();
    CHECK((result[0] == 0 && result[1] == 183) ||
          (result[1] == 0 && result[0] == 183), "one atomic no-replace publisher wins");
    const char* remaining = result[0] == 0 ? "/collision/right" : "/collision/left";
    const auto winner = cache->open("/collision/winner");
    const auto staged = cache->open(remaining);
    CHECK(winner && staged && *retained_left == matter::CacheByteStorage::Bytes({1, 2}),
          "loser stage and old immutable lease survive publication");
    cache->fail_publication(true);
    CHECK(storage.move(remaining, "/collision/winner", true) != 0 &&
          cache->open("/collision/winner") == winner && cache->open(remaining) == staged,
          "failed collision replacement preserves both generations");
    cache->fail_publication(false);
    CHECK(storage.move(remaining, "/collision/winner", true) == 0 &&
          cache->open("/collision/winner") == staged && !cache->open(remaining) &&
          winner->size() == 2, "replacement transfers owned bytes without invalidating old lease");
    std::uint8_t value = 77;
    CHECK(!storage.read("/collision/winner", 2, 1, &value) && value == 77 &&
          storage.read("/collision/winner", 1, 1, &value) && value == (*staged)[1],
          "collision reads validate bounds before touching caller bytes");
    std::error_code error;
    CHECK(storage.file_size("/collision/winner", error) == 2 && !error &&
          storage.file_size("/collision/missing", error) == std::uintmax_t(-1) &&
          error == std::errc::no_such_file_or_directory,
          "collision size and missing-key errors use memory storage");
    storage.remove("/collision/winner");
    CHECK(!cache->open("/collision/winner") && staged->size() == 2,
          "collision erasure preserves outstanding immutable reads");
}

static void collision_read_generation_guards() {
    using Bytes = matter::CacheByteStorage::Bytes;
    auto cache = std::make_shared<matter::test::MemoryCacheByteStorage>();
    matter::terrain_collision::detail::FixtureArtifactStorage storage(cache);
    const char* path = "/collision/read-generation";
    const Bytes first{11, 12, 13, 14, 15, 16};
    const Bytes second{21, 22, 23, 24, 25, 26, 27, 28, 29};
    CHECK(cache->publish(path, first), "initial collision read generation publishes");
    std::atomic<int> ready{0};
    std::atomic<bool> release{false}, readers_ok{true};
    auto read = [&](const Bytes& expected) {
        std::error_code error;
        std::uint8_t header[2]{};
        if (storage.file_size(path, error) != expected.size() || error ||
            !storage.read(path, 0, 2, header) || header[0] != expected[0] ||
            header[1] != expected[1]) readers_ok = false;
        ++ready;
        while (!release.load()) std::this_thread::yield();
        Bytes full(expected.size(), 0);
        if (!storage.read(path, 0, full.size(), full.data()) || full != expected)
            readers_ok = false;
    };
    std::thread older(read, std::cref(first));
    while (ready.load() != 1) std::this_thread::yield();
    CHECK(cache->publish(path, second), "replacement collision read generation publishes");
    std::thread newer(read, std::cref(second));
    while (ready.load() != 2) std::this_thread::yield();
    CHECK(cache->publish(path, {31, 32, 33, 34}) && cache->erase(path),
          "both readers retain their own size/header/payload generation after replace and erase");
    release = true;
    older.join(); newer.join();
    CHECK(readers_ok, "concurrent same-path readers never mix byte generations");
    std::error_code error;
    CHECK(storage.file_size(path, error) == std::uintmax_t(-1) && error,
          "a missing acquisition is an immutable read result");
    CHECK(cache->publish(path, {41, 42, 43}), "future generation can publish after missing acquisition");
    std::uint8_t value = 99;
    CHECK(!storage.read(path, 0, 1, &value) && value == 99,
          "a read cannot silently reopen a generation after its missing acquisition");
    CHECK(storage.file_size(path, error) == 3 && !error &&
          storage.read(path, 0, 1, &value) && value == 41,
          "a fresh size acquisition selects the newest immutable generation");
}

int main() {
    collision_read_generation_guards();
    collision_storage_guards();
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
