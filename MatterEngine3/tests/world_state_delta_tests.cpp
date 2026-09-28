#include "provider/world_source.h"
#include "check.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

using viewer::WorldDelta;
using viewer::WorldManifestEntry;

static WorldManifestEntry entry(uint32_t id, uint64_t value) {
    WorldManifestEntry e;
    e.instance_id = id; e.part_hash = value;
    e.transform[3] = float(value); e.module = std::to_string(value);
    return e;
}

// The sequential contract is the oracle: remove the first occurrence for
// each request, then replace the first match or append for each addition.
static void reference_apply(std::vector<WorldManifestEntry>& entries, const WorldDelta& d) {
    for (auto id : d.removed) {
        auto at = std::find_if(entries.begin(), entries.end(),
                              [id](const auto& e) { return e.instance_id == id; });
        if (at != entries.end()) entries.erase(at);
    }
    for (const auto& add : d.added) {
        auto at = std::find_if(entries.begin(), entries.end(),
                              [&](const auto& e) { return e.instance_id == add.instance_id; });
        if (at == entries.end()) entries.push_back(add);
        else *at = add;
    }
}

static bool equal(const std::vector<WorldManifestEntry>& a,
                  const std::vector<WorldManifestEntry>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].instance_id != b[i].instance_id || a[i].part_hash != b[i].part_hash ||
            a[i].module != b[i].module ||
            std::memcmp(a[i].transform, b[i].transform, sizeof(a[i].transform))) return false;
    return true;
}

template<class F> static double timed(F&& f) {
    const auto start = std::chrono::steady_clock::now(); f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
}

int main(int argc, char** argv) {
    std::mt19937 random(827);
    viewer::WorldState state;
    viewer::WorldManifest manifest;
    // Duplicate ids in a reset manifest are unusual but were accepted by the
    // old implementation. Exercise them rather than silently changing policy.
    for (uint32_t i = 0; i < 80; ++i) manifest.instances.push_back(entry(i % 23, i));
    state.reset(manifest);
    auto expected = manifest.instances;
    for (int round = 0; round < 300; ++round) {
        WorldDelta delta;
        for (uint32_t i = 0, n = random()%24; i < n; ++i) delta.removed.push_back(random()%50);
        for (uint32_t i = 0, n = random()%32; i < n; ++i) delta.added.push_back(entry(random()%50, random()));
        const auto version = state.version();
        reference_apply(expected, delta); state.apply(delta);
        CHECK(equal(state.entries(), expected), "bulk delta preserves first-match, re-add, move and survivor order");
        CHECK(state.version() == version+1, "each delta bumps the version exactly once");
    }
    const auto version = state.version(); state.apply({});
    CHECK(state.version() == version+1 && equal(state.entries(), expected), "empty delta retains entries and bumps version");

    WorldDelta bulk;
    constexpr uint32_t count = 200000;
    bulk.added.reserve(count);
    for (uint32_t i = 0; i < count; ++i) bulk.added.push_back(entry(i, i));
    state.reset({}); state.apply(bulk);
    CHECK(state.entries().size() == count, "large forest publication retains all instances");
    WorldDelta replace;
    for (uint32_t i = 0; i < count; ++i) {
        if (i%3 == 0) replace.removed.push_back(i);
        replace.added.push_back(entry(i, i+count));
    }
    state.apply(replace);
    CHECK(state.entries().size() == count, "large remove and re-add has no duplicate instances");
    for (const auto& e : state.entries())
        CHECK(e.part_hash == e.instance_id+count && e.transform[3] == float(e.part_hash),
              "large delta preserves the latest payload");

    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
        for (uint32_t n : {20000u, 40000u, 2085163u}) {
            WorldDelta delta;
            for (uint32_t i = 0; i < n; ++i) delta.added.push_back(entry(i, i));
            viewer::WorldState measured;
            const double indexed = timed([&] { measured.apply(delta); });
            if (n <= 40000) {
                std::vector<WorldManifestEntry> legacy;
                const double linear = timed([&] { reference_apply(legacy, delta); });
                CHECK(equal(measured.entries(), legacy), "measured outputs are identical");
                std::printf("DELTA_BENCH,%u,legacy_ms=%.3f,indexed_ms=%.3f\n", n, linear, indexed);
            } else std::printf("DELTA_BENCH,%u,indexed_ms=%.3f\n", n, indexed);
        }
    }
    return check_summary();
}
