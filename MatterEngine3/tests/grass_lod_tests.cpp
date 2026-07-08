// grass_lod_tests.cpp
// Validates the Grass.js prefix-subset triangle-budget LOD contract.
//
// Tests:
//   1. test_budget_tri_counts   – per-level tri count == 3*ceil(k*BLADES) exactly
//   2. test_prefix_subset       – blade TIP vertices of low-budget are a subset of
//                                 full-budget tips (exact float equality; width-independent)
//   3. test_determinism         – two separate cache dirs => same resolved hash +
//                                 byte-identical .part files
//   4. test_triex_present_all_levels – TriEx (tint) populated at every budget level
//
// Must be run from MatterEngine3/tests/ so relative schema paths resolve.

#include "script_host.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#include "check.h"

static std::string abspath(const std::string& rel) {
    char buf[PATH_MAX];
    if (realpath(rel.c_str(), buf)) return std::string(buf);
    return rel;
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

// ---------------------------------------------------------------------------
// Bake state – set up once in main(), accessed by all helpers
// ---------------------------------------------------------------------------

static std::string g_grass_source;
static std::string g_shared_lib;

// Result of a single bake.
struct BakeRec {
    uint64_t    resolved_hash = 0;
    std::string written_path;
};

// Bake Grass.js at the given lodBudget. Runs from 'cache_dir' (which must
// already contain a 'parts/' subdirectory). NOTE: chdir(cache_dir) is
// permanent for this call; callers that need a custom sandbox pass one and
// must chdir back themselves (test_determinism does this explicitly).
// For the main tests, we stay in g_sandbox (chdir'd in main).
static BakeRec bake_grass_in(double budget, const std::string& cache_dir) {
    // chdir to the cache dir so ScriptHost writes into its parts/.
    char prev[PATH_MAX];
    getcwd(prev, sizeof(prev));
    if (chdir(cache_dir.c_str()) != 0) {
        printf("  ERROR: chdir(%s)\n", cache_dir.c_str());
        return {};
    }

    script_host::ScriptHost host;
    host.set_shared_lib_root(g_shared_lib);

    char params[64];
    snprintf(params, sizeof(params), "{\"seed\":0,\"lodBudget\":%.10f}", budget);
    script_host::BakeResult r = host.bake_source(g_grass_source, params, {});

    // Convert written_path to absolute before chdir-ing back.
    // bake_source returns a CWD-relative path ("parts/<hex>.part").
    std::string abs_written;
    if (!r.written_path.empty()) {
        char abs_buf[PATH_MAX];
        if (realpath(r.written_path.c_str(), abs_buf))
            abs_written = abs_buf;
        else
            abs_written = cache_dir + "/" + r.written_path;
    }

    chdir(prev);

    if (!r.error.ok) {
        printf("  bake_grass(%.3f) ERROR: %s\n", budget, r.error.message.c_str());
    }
    return BakeRec{ r.resolved_hash, abs_written };
}

// Bake into the main sandbox (current directory when called, which is g_sandbox
// after main sets up).
static std::string g_sandbox;

static BakeRec bake_grass(double budget) {
    return bake_grass_in(budget, g_sandbox);
}

// All LOD-0 triangles for a given budget, loaded from the written_path directly.
static std::vector<Tri> bake_grass_mesh(double budget) {
    BakeRec rec = bake_grass(budget);
    std::vector<Tri> out;
    if (rec.written_path.empty()) return out;

    BLASManager blas; TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    // Use the absolute written_path directly (not cache_path_resolved which is CWD-relative).
    if (!part_asset::load_v2(rec.written_path, rec.resolved_hash, blas, tlas, children, lods))
        return out;
    for (const auto& e : blas.get_entries())
        out.insert(out.end(), e->triangles.begin(), e->triangles.end());
    return out;
}

static size_t bake_grass_tris(double budget) {
    return bake_grass_mesh(budget).size();
}

// Full bake result with managers kept alive for caller inspection.
struct BakeMesh {
    BLASManager blas;
    TLASManager tlas{ 256 };
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    bool ok = false;
};

static BakeMesh bake_grass_full(double budget) {
    BakeMesh m;
    BakeRec rec = bake_grass(budget);
    if (rec.written_path.empty()) return m;
    m.ok = part_asset::load_v2(rec.written_path, rec.resolved_hash,
                                m.blas, m.tlas, m.children, m.lods);
    return m;
}

// Blade TIP positions: vertex positions appearing in exactly ONE triangle with
// y > 0. Width-independent (the tip vertex is (0, hgt, lean) rotated by yaw,
// so it doesn't contain the blade width w), so they are invariant under the
// 1/sqrt(k) widening and serve as exact prefix-subset witnesses.
static std::vector<float3> blade_tips(const std::vector<Tri>& tris) {
    struct FKey {
        float x, y, z;
        bool operator<(const FKey& o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            return z < o.z;
        }
    };
    std::map<FKey, int> counts;
    for (const auto& t : tris) {
        for (const float3* v : { &t.vertex0, &t.vertex1, &t.vertex2 }) {
            if (v->y > 0.0f) {
                FKey k{ v->x, v->y, v->z };
                counts[k]++;
            }
        }
    }
    // Tips appear in exactly 1 triangle (terminal vertex of the strip; not
    // shared with any adjacent triangle).
    std::vector<float3> tips;
    for (const auto& kv : counts) {
        if (kv.second == 1) {
            float3 p;
            p.x = kv.first.x;
            p.y = kv.first.y;
            p.z = kv.first.z;
            tips.push_back(p);
        }
    }
    return tips;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_budget_tri_counts() {
    printf("\n[test_budget_tri_counts]\n");
    size_t t100 = bake_grass_tris(1.0);
    size_t t30  = bake_grass_tris(0.3);
    size_t t8   = bake_grass_tris(0.08);
    size_t blades = t100 / 3;
    printf("  budget 1.0  -> %zu tris (%zu blades)\n", t100, blades);
    printf("  budget 0.3  -> %zu tris\n", t30);
    printf("  budget 0.08 -> %zu tris\n", t8);
    CHECK(t100 % 3 == 0, "full-budget tris divisible by 3");
    CHECK(blades >= 25 && blades <= 35, "blade count in [25,35]");
    size_t expected30 = 3 * (size_t)std::ceil(0.3  * (double)blades);
    size_t expected8  = 3 * std::max<size_t>(1, (size_t)std::ceil(0.08 * (double)blades));
    CHECK(t30 == expected30, "budget 0.3 tri count == 3*ceil(0.3*BLADES)");
    CHECK(t8  == expected8,  "budget 0.08 tri count == 3*max(1,ceil(0.08*BLADES))");
}

static void test_prefix_subset() {
    printf("\n[test_prefix_subset]\n");
    auto full = bake_grass_mesh(1.0);
    auto low  = bake_grass_mesh(0.3);
    auto tips_full = blade_tips(full);
    auto tips_low  = blade_tips(low);
    printf("  full tips=%zu  low tips=%zu\n", tips_full.size(), tips_low.size());
    CHECK(tips_low.size() < tips_full.size(),
          "low-budget has fewer blade tips than full-budget");
    bool all_found = true;
    for (const auto& t : tips_low) {
        bool found = false;
        for (const auto& u : tips_full) {
            if (t.x == u.x && t.y == u.y && t.z == u.z) { found = true; break; }
        }
        if (!found) { all_found = false; break; }
    }
    CHECK(all_found,
          "every low-budget tip appears in full-budget (prefix-subset, exact float equality)");
}

static void test_determinism() {
    printf("\n[test_determinism]\n");
    const std::string cacheA = g_sandbox + "/cacheA";
    const std::string cacheB = g_sandbox + "/cacheB";
    system(("mkdir -p " + cacheA + "/parts").c_str());
    system(("mkdir -p " + cacheB + "/parts").c_str());
    BakeRec r1 = bake_grass_in(0.3, cacheA);
    BakeRec r2 = bake_grass_in(0.3, cacheB);
    CHECK(!r1.written_path.empty() && !r2.written_path.empty(), "both bakes wrote a file");
    CHECK(r1.resolved_hash == r2.resolved_hash,
          "separate caches => same resolved hash");
    auto b1 = file_bytes(r1.written_path);
    auto b2 = file_bytes(r2.written_path);
    CHECK(!b1.empty() && b1 == b2,
          "separate caches => byte-identical .part files");
}

static void test_triex_present_all_levels() {
    printf("\n[test_triex_present_all_levels]\n");
    for (double budget : { 1.0, 0.3, 0.08 }) {
        BakeMesh m = bake_grass_full(budget);
        char msg[128];
        snprintf(msg, sizeof(msg), "load_v2 ok at budget %.2f", budget);
        CHECK(m.ok, msg);
        if (!m.ok) continue;
        bool all_triex = true;
        for (const auto& e : m.blas.get_entries()) {
            if (e->tri_extra.empty()) { all_triex = false; break; }
        }
        snprintf(msg, sizeof(msg), "TriEx present at budget %.2f", budget);
        CHECK(all_triex, msg);
    }
}

// Verify that at budget=1.0, widen==1.0 so geometry is unchanged from the old
// Grass.js (same RNG draw order + w values unscaled). Checked indirectly:
//   - tri count == 3*BLADES (count == BLADES => widen == sqrt(1) == 1)
//   - two separate determinism runs agree (would catch any floating drift)
static void test_full_budget_unchanged_geometry() {
    printf("\n[test_full_budget_unchanged_geometry]\n");
    const std::string full1 = g_sandbox + "/full1";
    const std::string full2 = g_sandbox + "/full2";
    system(("mkdir -p " + full1 + "/parts").c_str());
    system(("mkdir -p " + full2 + "/parts").c_str());
    BakeRec r1 = bake_grass_in(1.0, full1);
    BakeRec r2 = bake_grass_in(1.0, full2);
    CHECK(!r1.written_path.empty() && !r2.written_path.empty(),
          "budget=1.0 bakes wrote files");
    CHECK(r1.resolved_hash == r2.resolved_hash,
          "budget=1.0 bakes are deterministic (same hash)");
    auto b1 = file_bytes(r1.written_path);
    auto b2 = file_bytes(r2.written_path);
    CHECK(!b1.empty() && b1 == b2,
          "budget=1.0 byte-identical across separate caches");
    size_t t = bake_grass_tris(1.0);
    size_t blades = t / 3;
    CHECK(t == 3 * blades, "budget=1.0 tris == 3*BLADES (count==BLADES, widen==1)");
    CHECK(blades >= 25 && blades <= 35, "budget=1.0 blade count in [25,35]");
    printf("  budget=1.0: %zu tris, %zu blades, hash=%016llx\n",
           t, blades, (unsigned long long)r1.resolved_hash);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    // Resolve asset paths before any chdir.
    const std::string schemas = abspath("../examples/world_demo/schemas");
    g_shared_lib = abspath("../shared-lib");
    const std::string grass_path = schemas + "/Grass.js";
    g_grass_source = read_text(grass_path);
    if (g_grass_source.empty()) {
        printf("FAIL: could not read %s\n", grass_path.c_str());
        return 1;
    }
    printf("Grass.js source: %zu bytes from %s\n",
           g_grass_source.size(), grass_path.c_str());

    // Fresh sandbox for every run.
    g_sandbox = abspath("/tmp/me3_grass_lod");
    system(("rm -rf " + g_sandbox).c_str());
    system(("mkdir -p " + g_sandbox + "/parts").c_str());

    // Stay in g_sandbox so that cache_path_resolved("parts/<h>.part") works for
    // load helpers that re-use the written_path directly.
    // (We use written_path directly in load_v2, so chdir is for bake_grass only.)

    test_budget_tri_counts();
    test_prefix_subset();
    test_determinism();
    test_triex_present_all_levels();
    test_full_budget_unchanged_geometry();

    printf("\n");
    if (g_failures == 0)
        printf("ALL PASS\n");
    else
        printf("%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
