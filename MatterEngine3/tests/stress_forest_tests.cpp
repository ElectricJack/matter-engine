// stress_forest_tests.cpp
// Determinism of the Stage-4 stress fixture scatter (StressForest50k.js).
//
// Bakes the schema twice into SEPARATE cache dirs with a fixed synthetic child
// hash standing in for 'Pebble' (child geometry is irrelevant here — the
// flattened placement stream is the contract), then verifies:
//   1. both bakes produce the same resolved hash
//   2. the .part files are byte-identical
//   3. the flattened placement stream (child_resolved_hash + transform[16], in
//      order) hashes identically (FNV-1a) and has exactly COUNT entries
//
// Uses the 50k variant (not 500k) for test runtime sanity; the 100k/200k/500k
// schemas are byte-for-byte copies except COUNT/class name, so the 50k stream
// determinism covers the shared scatter logic.
//
// Follows the grass_lod_tests.cpp determinism pattern (two sandbox caches,
// bake_source, compare hash + bytes). Must be run from MatterEngine3/tests/ so
// relative schema paths resolve.

#include "script_host.h"
#include "part_asset_v2.h"
#include "part_asset.h"          // fnv1a64
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

static const size_t   kExpectedCount = 50000;
static const uint64_t kFakePebbleHash = 0x5157e55f04e57000ull;  // synthetic 'Pebble'

static std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
}

static std::string read_text(const std::string& path) {
    std::string s;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return s;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    s.resize((size_t)n);
    size_t got = fread(&s[0], 1, (size_t)n, f);
    s.resize(got);
    fclose(f);
    return s;
}

static std::vector<uint8_t> file_bytes(const std::string& path) {
    std::vector<uint8_t> b;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    b.resize((size_t)n);
    if (fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
    fclose(f);
    return b;
}

static std::string g_source;
static std::string g_shared_lib;

struct BakeRec {
    uint64_t    resolved_hash = 0;
    std::string written_path;   // absolute
};

// Bake StressForest50k.js in `cache_dir` (must contain parts/). The synthetic
// Pebble hash feeds placeChild's placement table exactly as PartGraph would.
static BakeRec bake_forest_in(const std::string& cache_dir) {
    char prev[PATH_MAX];
    getcwd(prev, sizeof(prev));
    if (chdir(cache_dir.c_str()) != 0) {
        printf("  ERROR: chdir(%s)\n", cache_dir.c_str());
        return {};
    }

    script_host::ScriptHost host;
    host.set_shared_lib_root(g_shared_lib);

    uint64_t    kids[1]  = { kFakePebbleHash };
    std::string names[1] = { "Pebble" };
    script_host::BakeResult r =
        host.bake_source(g_source, "{}", {}, kids, 1, names);

    std::string abs_written;
    if (!r.written_path.empty()) {
        char abs_buf[PATH_MAX];
        if (realpath(r.written_path.c_str(), abs_buf))
            abs_written = abs_buf;
        else
            abs_written = cache_dir + "/" + r.written_path;
    }
    chdir(prev);

    if (!r.error.ok)
        printf("  bake ERROR: %s\n", r.error.message.c_str());
    return BakeRec{ r.resolved_hash, abs_written };
}

// FNV-1a over the placement stream: for each ChildInstance in table order,
// fold child_resolved_hash then the raw transform[16] floats.
static uint64_t placement_stream_hash(const std::string& part_path,
                                      uint64_t resolved_hash,
                                      size_t* out_count) {
    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    *out_count = 0;
    if (!part_asset::load_v2(part_path, resolved_hash, blas, tlas, children, lods))
        return 0;
    *out_count = children.size();
    std::vector<uint8_t> buf;
    buf.reserve(children.size() * (sizeof(uint64_t) + 16 * sizeof(float)));
    for (const auto& c : children) {
        const size_t off = buf.size();
        buf.resize(off + sizeof(uint64_t) + 16 * sizeof(float));
        std::memcpy(buf.data() + off, &c.child_resolved_hash, sizeof(uint64_t));
        std::memcpy(buf.data() + off + sizeof(uint64_t), c.transform,
                    16 * sizeof(float));
    }
    return part_asset::fnv1a64(buf.data(), buf.size());
}

int main() {
    const std::string schemas = abspath("../examples/world_demo/schemas");
    g_shared_lib = abspath("../shared-lib");
    const std::string src_path = schemas + "/StressForest50k.js";
    g_source = read_text(src_path);
    if (g_source.empty()) {
        printf("FAIL: could not read %s\n", src_path.c_str());
        return 1;
    }
    printf("StressForest50k.js source: %zu bytes from %s\n",
           g_source.size(), src_path.c_str());

    // Fresh sandbox with two independent caches.
    const std::string sandbox = "/tmp/me3_stress_forest";
    system(("rm -rf " + sandbox).c_str());
    const std::string cacheA = sandbox + "/cacheA";
    const std::string cacheB = sandbox + "/cacheB";
    system(("mkdir -p " + cacheA + "/parts").c_str());
    system(("mkdir -p " + cacheB + "/parts").c_str());

    printf("\n[test_scatter_determinism]\n");
    BakeRec r1 = bake_forest_in(cacheA);
    BakeRec r2 = bake_forest_in(cacheB);
    CHECK(!r1.written_path.empty() && !r2.written_path.empty(),
          "both bakes wrote a .part file");
    CHECK(r1.resolved_hash != 0 && r1.resolved_hash == r2.resolved_hash,
          "separate caches => same resolved hash");

    auto b1 = file_bytes(r1.written_path);
    auto b2 = file_bytes(r2.written_path);
    CHECK(!b1.empty() && b1 == b2,
          "separate caches => byte-identical .part files");

    size_t n1 = 0, n2 = 0;
    uint64_t h1 = placement_stream_hash(r1.written_path, r1.resolved_hash, &n1);
    uint64_t h2 = placement_stream_hash(r2.written_path, r2.resolved_hash, &n2);
    printf("  placement stream: n1=%zu n2=%zu hash1=%016llx hash2=%016llx\n",
           n1, n2, (unsigned long long)h1, (unsigned long long)h2);
    CHECK(n1 == kExpectedCount, "placement count == 50000");
    CHECK(n1 == n2, "both bakes place the same number of children");
    CHECK(h1 != 0 && h1 == h2,
          "flattened placement stream hashes identically across bakes");

    printf("\n");
    if (g_failures == 0) printf("ALL PASS\n");
    else                 printf("%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
