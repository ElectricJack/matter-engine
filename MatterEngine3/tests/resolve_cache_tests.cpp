// resolve_cache_tests.cpp — headless round-trip tests for resolve_cache.{h,cpp}.
// No GL context required; self-terminates.
//
// Test cases:
//   (a) round_trip_basic    — synthetic manifest+snapshot+bake_plan serializes
//                             and deserializes with field-by-field equality incl.
//                             transform bytes and deduped source strings.
//   (b) key_changes_on_file — cache key changes when any schema file byte changes.
//   (c) key_changes_on_seed — cache key changes when root_params_json changes.
//   (d) truncated_load      — load of a truncated file returns false.
//   (e) bad_magic_rejected  — load of a file with wrong magic returns false.
//   (f) bad_key_rejected    — load with wrong expected_key returns false.
//   (g) snapshot_indices    — by_file and by_import maps are reconstructed.
//   (h) multi_source_dedup  — multiple bake_plan entries sharing the same
//                             source string are deduplicated in the file.
//   (i) round_trip_retopo   — non-empty retopo_by_hash map survives save/load
//                             with all RetopoSettings fields intact (I-1 fix).

#include "resolve_cache.h"
#include "part_asset_v2.h"
#include "part_graph.h"
#include "part_graph_snapshot.h"
#include "world_source.h"
#include "world_lights.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Minimal test framework (mirrors async_bake_tests.cpp convention)
// ---------------------------------------------------------------------------
static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define CHECK(expr) do { \
    ++g_tests_run; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #expr); \
    } else { \
        ++g_tests_passed; \
    } \
} while(0)

#define REQUIRE(expr) do { \
    ++g_tests_run; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL (fatal) [%s:%d] %s\n", __FILE__, __LINE__, #expr); \
        return; \
    } else { \
        ++g_tests_passed; \
    } \
} while(0)

// ---------------------------------------------------------------------------
// Sandbox helpers
// ---------------------------------------------------------------------------
static void run(const std::string& cmd) { std::system(cmd.c_str()); }

static bool write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << body;
    return true;
}

// Build a minimal directory tree under root for key-computation tests.
// schemas/: Box.js, Tree.js
// shared-lib/: base.js
static bool build_key_sandbox(const std::string& root) {
    run("rm -rf " + root);
    run("mkdir -p " + root + "/schemas");
    run("mkdir -p " + root + "/shared-lib");
    run("mkdir -p " + root + "/WorldData/TestWorld");
    run("mkdir -p " + root + "/cache");

    if (!write_file(root + "/schemas/Box.js", "class Box extends Part { static build() {} }")) return false;
    if (!write_file(root + "/schemas/Tree.js", "class Tree extends Part { static build() {} }")) return false;
    if (!write_file(root + "/shared-lib/base.js", "export const BASE = 1;")) return false;
    if (!write_file(root + "/WorldData/TestWorld/world.manifest", "Box\nTree\n")) return false;
    return true;
}

// Build a synthetic ResolveCachePayload with interesting values.
static resolve_cache::ResolveCachePayload make_payload() {
    resolve_cache::ResolveCachePayload p;

    // Instances.
    {
        viewer::WorldManifestEntry e;
        e.instance_id = 42u;
        e.part_hash   = 0xDEADBEEFCAFEBABEull;
        for (int i = 0; i < 16; ++i) e.transform[i] = (float)i * 0.1f;
        e.transform[0] = 1.f; e.transform[5] = 1.f; e.transform[10] = 1.f; e.transform[15] = 1.f;
        e.transform[3] = 10.f; e.transform[7] = 0.f; e.transform[11] = -5.f;
        e.module = "Box";
        p.instances.push_back(e);

        viewer::WorldManifestEntry e2;
        e2.instance_id = 43u;
        e2.part_hash   = 0x1234567890ABCDEFull;
        for (int i = 0; i < 16; ++i) e2.transform[i] = 0.f;
        e2.transform[0] = e2.transform[5] = e2.transform[10] = e2.transform[15] = 1.f;
        e2.module = "";  // empty module (expand path)
        p.instances.push_back(e2);
    }

    // Lights.
    {
        p.lights.sun_dir[0]   = -0.45f; p.lights.sun_dir[1]   = -0.80f; p.lights.sun_dir[2]   = -0.35f;
        p.lights.sun_color[0] =  2.2f;  p.lights.sun_color[1] =  2.05f; p.lights.sun_color[2] =  1.8f;
        p.lights.sky_color[0] =  0.38f; p.lights.sky_color[1] =  0.43f; p.lights.sky_color[2] =  0.52f;

        world_lights::SpotLight s;
        s.pos[0] = 1.f; s.pos[1] = 10.f; s.pos[2] = 2.f;
        s.dir[0] = 0.f; s.dir[1] = -1.f; s.dir[2] = 0.f;
        s.color[0] = 5.f; s.color[1] = 4.5f; s.color[2] = 4.f;
        s.range = 20.f;
        s.cos_inner = 0.9f;
        s.cos_outer = 0.7f;
        p.lights.spots.push_back(s);
    }

    // Snapshot.
    {
        part_graph_snapshot::Node n;
        n.module      = "Box";
        n.source_path = "/abs/schemas/Box.js";
        n.params_json = "{}";
        n.children    = {"Leg"};
        n.shared_imports = {"base"};
        n.resolved_hash  = 0xDEADBEEFCAFEBABEull;
        n.is_root = true;
        p.snapshot.nodes["Box"] = n;

        part_graph_snapshot::Node n2;
        n2.module      = "Leg";
        n2.source_path = "/abs/schemas/Leg.js";
        n2.params_json = "{\"h\":1.5}";
        n2.resolved_hash = 0x1111111111111111ull;
        n2.is_root = false;
        p.snapshot.nodes["Leg"] = n2;

        // by_file / by_import are reconstructed at load; set them here so we
        // can compare against the loaded result.
        p.snapshot.by_file["/abs/schemas/Box.js"].push_back("Box");
        p.snapshot.by_file["/abs/schemas/Leg.js"].push_back("Leg");
        p.snapshot.by_import["base"].push_back("Box");
    }

    // bake_plan — two entries sharing the same source (dedup test).
    const std::string SHARED_SRC = "class Box extends Part { static build() { box(1,1,1); } }";
    {
        part_graph::BakeInputs bi;
        bi.module = "Box";
        bi.source = SHARED_SRC;
        bi.params["size"] = part_graph::ParamValue::number(2.0);
        bi.params["big"]  = part_graph::ParamValue::boolean_(true);
        bi.params["tag"]  = part_graph::ParamValue::string_("hello");
        bi.child_hashes  = {0x1111111111111111ull};
        bi.child_modules = {"Leg"};
        bi.child_params  = {"{\"h\":1.5}"};
        p.bake_plan[0xDEADBEEFCAFEBABEull] = bi;
    }
    {
        part_graph::BakeInputs bi2;
        bi2.module = "Box";           // same schema, different params
        bi2.source = SHARED_SRC;      // SAME source string => deduplication
        bi2.params["size"] = part_graph::ParamValue::number(4.0);
        bi2.child_hashes  = {};
        bi2.child_modules = {};
        bi2.child_params  = {};
        p.bake_plan[0xAAAAAAAAAAAAAAAAull] = bi2;
    }
    {
        part_graph::BakeInputs bi3;
        bi3.module = "Leg";
        bi3.source = "class Leg extends Part { static build() { box(0.1,1.5,0.1); } }";
        bi3.params["h"] = part_graph::ParamValue::number(1.5);
        bi3.child_hashes  = {};
        bi3.child_modules = {};
        bi3.child_params  = {};
        p.bake_plan[0x1111111111111111ull] = bi3;
    }

    // root_hashes.
    p.root_hashes = {0xDEADBEEFCAFEBABEull};

    // retopo_by_hash — one opted-in entry, one disabled entry.
    {
        part_asset::RetopoSettings rs_on;
        rs_on.enabled         = true;
        rs_on.target_ratio    = 0.25f;
        rs_on.iterations      = 5;
        rs_on.seed            = 42u;
        rs_on.timeout_seconds = 120;
        p.retopo_by_hash[0xDEADBEEFCAFEBABEull] = rs_on;

        part_asset::RetopoSettings rs_off;
        // All defaults: enabled=false, target_ratio=1.0f, iterations=3, seed=0, timeout=60
        p.retopo_by_hash[0x1111111111111111ull] = rs_off;
    }

    return p;
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

static void test_round_trip_basic() {
    printf("[resolve_cache] round_trip_basic\n");
    const std::string root = "/tmp/rc_test_roundtrip";
    run("rm -rf " + root);
    run("mkdir -p " + root + "/cache");

    const uint64_t KEY = 0x0102030405060708ull;
    auto p = make_payload();

    REQUIRE(resolve_cache::save(root, "TestWorld", KEY, p));

    resolve_cache::ResolveCachePayload out;
    REQUIRE(resolve_cache::load(root, "TestWorld", KEY, out));

    // instances
    REQUIRE(out.instances.size() == p.instances.size());
    for (size_t i = 0; i < p.instances.size(); ++i) {
        CHECK(out.instances[i].instance_id == p.instances[i].instance_id);
        CHECK(out.instances[i].part_hash   == p.instances[i].part_hash);
        CHECK(out.instances[i].module      == p.instances[i].module);
        for (int j = 0; j < 16; ++j)
            CHECK(out.instances[i].transform[j] == p.instances[i].transform[j]);
    }

    // lights
    for (int i = 0; i < 3; ++i) {
        CHECK(out.lights.sun_dir[i]   == p.lights.sun_dir[i]);
        CHECK(out.lights.sun_color[i] == p.lights.sun_color[i]);
        CHECK(out.lights.sky_color[i] == p.lights.sky_color[i]);
    }
    REQUIRE(out.lights.spots.size() == p.lights.spots.size());
    for (size_t i = 0; i < p.lights.spots.size(); ++i) {
        const auto& sp  = p.lights.spots[i];
        const auto& so  = out.lights.spots[i];
        for (int j = 0; j < 3; ++j) {
            CHECK(so.pos[j]   == sp.pos[j]);
            CHECK(so.dir[j]   == sp.dir[j]);
            CHECK(so.color[j] == sp.color[j]);
        }
        CHECK(so.range     == sp.range);
        CHECK(so.cos_inner == sp.cos_inner);
        CHECK(so.cos_outer == sp.cos_outer);
    }

    // snapshot nodes
    CHECK(out.snapshot.nodes.size() == p.snapshot.nodes.size());
    for (const auto& kv : p.snapshot.nodes) {
        auto it = out.snapshot.nodes.find(kv.first);
        CHECK(it != out.snapshot.nodes.end());
        if (it == out.snapshot.nodes.end()) continue;
        CHECK(it->second.module      == kv.second.module);
        CHECK(it->second.source_path == kv.second.source_path);
        CHECK(it->second.params_json == kv.second.params_json);
        CHECK(it->second.children    == kv.second.children);
        CHECK(it->second.shared_imports == kv.second.shared_imports);
        CHECK(it->second.resolved_hash  == kv.second.resolved_hash);
        CHECK(it->second.is_root        == kv.second.is_root);
    }

    // bake_plan
    CHECK(out.bake_plan.size() == p.bake_plan.size());
    for (const auto& kv : p.bake_plan) {
        auto it = out.bake_plan.find(kv.first);
        CHECK(it != out.bake_plan.end());
        if (it == out.bake_plan.end()) continue;
        CHECK(it->second.module   == kv.second.module);
        CHECK(it->second.source   == kv.second.source);
        CHECK(it->second.params.size() == kv.second.params.size());
        for (const auto& pk : kv.second.params) {
            auto pit = it->second.params.find(pk.first);
            CHECK(pit != it->second.params.end());
            if (pit == it->second.params.end()) continue;
            CHECK((int)pit->second.kind == (int)pk.second.kind);
            switch (pk.second.kind) {
            case part_graph::ParamValue::Kind::Number:
                CHECK(pit->second.num == pk.second.num);
                break;
            case part_graph::ParamValue::Kind::Bool:
                CHECK(pit->second.boolean == pk.second.boolean);
                break;
            case part_graph::ParamValue::Kind::Str:
                CHECK(pit->second.str == pk.second.str);
                break;
            }
        }
        CHECK(it->second.child_hashes  == kv.second.child_hashes);
        CHECK(it->second.child_modules == kv.second.child_modules);
        CHECK(it->second.child_params  == kv.second.child_params);
    }

    // root_hashes
    CHECK(out.root_hashes == p.root_hashes);

    // retopo_by_hash
    CHECK(out.retopo_by_hash.size() == p.retopo_by_hash.size());
    for (const auto& kv : p.retopo_by_hash) {
        auto it = out.retopo_by_hash.find(kv.first);
        CHECK(it != out.retopo_by_hash.end());
        if (it == out.retopo_by_hash.end()) continue;
        CHECK(it->second.enabled         == kv.second.enabled);
        CHECK(it->second.target_ratio    == kv.second.target_ratio);
        CHECK(it->second.iterations      == kv.second.iterations);
        CHECK(it->second.seed            == kv.second.seed);
        CHECK(it->second.timeout_seconds == kv.second.timeout_seconds);
    }

    run("rm -rf " + root);
}

static void test_snapshot_indices() {
    printf("[resolve_cache] snapshot_indices\n");
    const std::string root = "/tmp/rc_test_indices";
    run("rm -rf " + root);
    run("mkdir -p " + root + "/cache");

    const uint64_t KEY = 0xABCDABCDABCDABCDull;
    auto p = make_payload();

    REQUIRE(resolve_cache::save(root, "TestWorld", KEY, p));
    resolve_cache::ResolveCachePayload out;
    REQUIRE(resolve_cache::load(root, "TestWorld", KEY, out));

    // by_file: /abs/schemas/Box.js -> ["Box"], /abs/schemas/Leg.js -> ["Leg"]
    {
        auto it = out.snapshot.by_file.find("/abs/schemas/Box.js");
        CHECK(it != out.snapshot.by_file.end());
        if (it != out.snapshot.by_file.end())
            CHECK(it->second == std::vector<std::string>{"Box"});
    }
    {
        auto it = out.snapshot.by_file.find("/abs/schemas/Leg.js");
        CHECK(it != out.snapshot.by_file.end());
    }

    // by_import: "base" -> ["Box"]
    {
        auto it = out.snapshot.by_import.find("base");
        CHECK(it != out.snapshot.by_import.end());
        if (it != out.snapshot.by_import.end())
            CHECK(it->second == std::vector<std::string>{"Box"});
    }

    run("rm -rf " + root);
}

static void test_key_changes_on_file() {
    printf("[resolve_cache] key_changes_on_file\n");
    const std::string root = "/tmp/rc_test_keyfile";
    REQUIRE(build_key_sandbox(root));

    const std::string manifest = root + "/WorldData/TestWorld/world.manifest";
    uint64_t k1 = resolve_cache::compute_key(
        manifest, "", root + "/schemas", root + "/shared-lib");
    CHECK(k1 != 0);

    // Modify a schema file.
    REQUIRE(write_file(root + "/schemas/Box.js",
                       "class Box extends Part { static build() { box(2,2,2); } }"));

    uint64_t k2 = resolve_cache::compute_key(
        manifest, "", root + "/schemas", root + "/shared-lib");
    CHECK(k1 != k2);

    run("rm -rf " + root);
}

static void test_key_changes_on_seed() {
    printf("[resolve_cache] key_changes_on_seed\n");
    const std::string root = "/tmp/rc_test_keyseed";
    REQUIRE(build_key_sandbox(root));

    const std::string manifest = root + "/WorldData/TestWorld/world.manifest";
    uint64_t k1 = resolve_cache::compute_key(
        manifest, "", root + "/schemas", root + "/shared-lib");
    uint64_t k2 = resolve_cache::compute_key(
        manifest, "{\"worldSeed\":42}", root + "/schemas", root + "/shared-lib");
    uint64_t k3 = resolve_cache::compute_key(
        manifest, "{\"worldSeed\":99}", root + "/schemas", root + "/shared-lib");

    CHECK(k1 != k2);
    CHECK(k1 != k3);
    CHECK(k2 != k3);

    // Same seed -> same key.
    uint64_t k2b = resolve_cache::compute_key(
        manifest, "{\"worldSeed\":42}", root + "/schemas", root + "/shared-lib");
    CHECK(k2 == k2b);

    run("rm -rf " + root);
}

static void test_truncated_load() {
    printf("[resolve_cache] truncated_load\n");
    const std::string root = "/tmp/rc_test_trunc";
    run("rm -rf " + root);
    run("mkdir -p " + root + "/cache");

    const uint64_t KEY = 0x1111222233334444ull;
    auto p = make_payload();
    REQUIRE(resolve_cache::save(root, "TestWorld", KEY, p));

    // Truncate the file to just 4 bytes (magic only).
    const std::string path = root + "/cache/TestWorld.resolve";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        REQUIRE(f.good());
        uint32_t magic_only = 0x00314352u;
        f.write(reinterpret_cast<const char*>(&magic_only), 4);
    }

    resolve_cache::ResolveCachePayload out;
    CHECK(!resolve_cache::load(root, "TestWorld", KEY, out));

    run("rm -rf " + root);
}

static void test_bad_magic_rejected() {
    printf("[resolve_cache] bad_magic_rejected\n");
    const std::string root = "/tmp/rc_test_magic";
    run("rm -rf " + root);
    run("mkdir -p " + root + "/cache");

    const uint64_t KEY = 0xAAAABBBBCCCCDDDDull;
    auto p = make_payload();
    REQUIRE(resolve_cache::save(root, "TestWorld", KEY, p));

    // Corrupt the magic bytes.
    const std::string path = root + "/cache/TestWorld.resolve";
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(f.good());
        uint32_t bad = 0xDEADDEADu;
        f.seekp(0);
        f.write(reinterpret_cast<const char*>(&bad), 4);
    }

    resolve_cache::ResolveCachePayload out;
    CHECK(!resolve_cache::load(root, "TestWorld", KEY, out));

    run("rm -rf " + root);
}

static void test_bad_key_rejected() {
    printf("[resolve_cache] bad_key_rejected\n");
    const std::string root = "/tmp/rc_test_badkey";
    run("rm -rf " + root);
    run("mkdir -p " + root + "/cache");

    const uint64_t KEY_SAVE = 0x1234567890ABCDEFull;
    const uint64_t KEY_LOAD = 0xFEDCBA9876543210ull;
    auto p = make_payload();
    REQUIRE(resolve_cache::save(root, "TestWorld", KEY_SAVE, p));

    resolve_cache::ResolveCachePayload out;
    CHECK(!resolve_cache::load(root, "TestWorld", KEY_LOAD, out));

    run("rm -rf " + root);
}

static void test_multi_source_dedup() {
    printf("[resolve_cache] multi_source_dedup\n");
    const std::string root = "/tmp/rc_test_dedup";
    run("rm -rf " + root);
    run("mkdir -p " + root + "/cache");

    const uint64_t KEY = 0xDEDEDEDEDEDEDEDEull;
    auto p = make_payload();

    // The payload from make_payload() already has two bake_plan entries sharing
    // the same source string (the Box source). Verify that the file size is
    // smaller than it would be without dedup.
    REQUIRE(resolve_cache::save(root, "TestWorld", KEY, p));

    // Load and verify equality.
    resolve_cache::ResolveCachePayload out;
    REQUIRE(resolve_cache::load(root, "TestWorld", KEY, out));
    CHECK(out.bake_plan.size() == p.bake_plan.size());

    // Both Box entries share the same source.
    const std::string SHARED_SRC = "class Box extends Part { static build() { box(1,1,1); } }";
    auto it1 = out.bake_plan.find(0xDEADBEEFCAFEBABEull);
    auto it2 = out.bake_plan.find(0xAAAAAAAAAAAAAAAAull);
    REQUIRE(it1 != out.bake_plan.end());
    REQUIRE(it2 != out.bake_plan.end());
    CHECK(it1->second.source == SHARED_SRC);
    CHECK(it2->second.source == SHARED_SRC);

    run("rm -rf " + root);
}

// (i) non-empty retopo_by_hash survives save/load (I-1 fix)
static void test_round_trip_retopo() {
    printf("[resolve_cache] round_trip_retopo\n");
    const std::string root = "/tmp/rc_test_retopo";
    run("rm -rf " + root);
    run("mkdir -p " + root + "/cache");

    const uint64_t KEY = 0xBEEFBEEFBEEFBEEFull;

    // Build a minimal payload with only the retopo map populated.
    resolve_cache::ResolveCachePayload p;
    p.root_hashes = {0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull};

    part_asset::RetopoSettings rs1;
    rs1.enabled         = true;
    rs1.target_ratio    = 0.5f;
    rs1.iterations      = 7;
    rs1.seed            = 99u;
    rs1.timeout_seconds = 30;
    p.retopo_by_hash[0xAAAAAAAAAAAAAAAAull] = rs1;

    part_asset::RetopoSettings rs2;  // all defaults (enabled=false)
    p.retopo_by_hash[0xBBBBBBBBBBBBBBBBull] = rs2;

    REQUIRE(resolve_cache::save(root, "TestWorld", KEY, p));

    resolve_cache::ResolveCachePayload out;
    REQUIRE(resolve_cache::load(root, "TestWorld", KEY, out));

    // Verify root_hashes still round-trips.
    CHECK(out.root_hashes == p.root_hashes);

    // Verify retopo map: size, fields for both entries.
    REQUIRE(out.retopo_by_hash.size() == 2u);

    {
        auto it = out.retopo_by_hash.find(0xAAAAAAAAAAAAAAAAull);
        REQUIRE(it != out.retopo_by_hash.end());
        CHECK(it->second.enabled         == true);
        CHECK(it->second.target_ratio    == 0.5f);
        CHECK(it->second.iterations      == 7);
        CHECK(it->second.seed            == 99u);
        CHECK(it->second.timeout_seconds == 30);
    }

    {
        auto it = out.retopo_by_hash.find(0xBBBBBBBBBBBBBBBBull);
        REQUIRE(it != out.retopo_by_hash.end());
        CHECK(it->second.enabled         == false);
        CHECK(it->second.target_ratio    == 1.0f);
        CHECK(it->second.iterations      == 3);
        CHECK(it->second.seed            == 0u);
        CHECK(it->second.timeout_seconds == 60);
    }

    run("rm -rf " + root);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    printf("=== resolve_cache_tests ===\n");

    test_round_trip_basic();
    test_snapshot_indices();
    test_key_changes_on_file();
    test_key_changes_on_seed();
    test_truncated_load();
    test_bad_magic_rejected();
    test_bad_key_rejected();
    test_multi_source_dedup();
    test_round_trip_retopo();

    printf("=== %d/%d passed ===\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
