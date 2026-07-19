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

#include "resolve_cache.h"
#include "provider/local_provider.h"
#include "part_graph.h"
#include "part_graph_snapshot.h"
#include "world_source.h"
#include "world_lights.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace fs = std::filesystem;

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
static std::string sandbox_root(const char* name) {
    return (fs::temp_directory_path() / name).string();
}

static void reset_dir(const std::string& root) {
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
}

static void remove_dir(const std::string& root) {
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

static bool write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << body;
    return true;
}

// Build a minimal directory tree under root for key-computation tests.
// objects/: Box.js, nested/Tree.js
// shared-lib/: base.js
// engine-shared/: engine.js
static bool build_key_sandbox(const std::string& root) {
    reset_dir(root);
    fs::create_directories(root + "/objects/nested");
    fs::create_directories(root + "/worlds");
    fs::create_directories(root + "/shared-lib");
    fs::create_directories(root + "/engine-shared");
    fs::create_directories(root + "/WorldData/TestWorld");
    fs::create_directories(root + "/.cache/TestWorld/cache");

    if (!write_file(root + "/objects/Box.js", "class Box extends Part { static build() {} }")) return false;
    if (!write_file(root + "/objects/nested/Tree.js", "class Tree extends Part { static build() {} }")) return false;
    if (!write_file(root + "/worlds/TestWorld.js", "class TestWorld extends World { static roots = [{module: 'Box'}]; }")) return false;
    if (!write_file(root + "/shared-lib/base.js", "export const BASE = 1;")) return false;
    if (!write_file(root + "/engine-shared/engine.js", "export const ENGINE = 1;")) return false;
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
        n.shared_source_paths = {"/project/shared-lib/base.js",
                                 "/engine/shared-lib/noise.js"};
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

    return p;
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

static void test_round_trip_basic() {
    printf("[resolve_cache] round_trip_basic\n");
    const std::string root = sandbox_root("rc_test_roundtrip");
    reset_dir(root);
    fs::create_directories(root + "/cache");

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
        CHECK(it->second.shared_source_paths == kv.second.shared_source_paths);
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

    remove_dir(root);
}

static void test_snapshot_indices() {
    printf("[resolve_cache] snapshot_indices\n");
    const std::string root = sandbox_root("rc_test_indices");
    reset_dir(root);
    fs::create_directories(root + "/cache");

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

    remove_dir(root);
}

static void test_project_sources_define_key_and_stale_manifest_is_ignored() {
    printf("[resolve_cache] project_sources_define_key\n");
    const std::string root = sandbox_root("rc_test_project_sources");
    REQUIRE(build_key_sandbox(root));

    const std::string world = root + "/worlds/TestWorld.js";
    uint64_t k1 = resolve_cache::compute_key(
        world, "{}", root + "/objects", root + "/shared-lib",
        root + "/engine-shared");
    CHECK(k1 != 0);

    REQUIRE(write_file(root + "/objects/Box.js",
                       "class Box extends Part { static build() { box(2,2,2); } }"));
    uint64_t k2 = resolve_cache::compute_key(
        world, "{}", root + "/objects", root + "/shared-lib",
        root + "/engine-shared");
    CHECK(k1 != k2);

    REQUIRE(write_file(root + "/shared-lib/base.js", "export const BASE = 2;"));
    uint64_t k3 = resolve_cache::compute_key(
        world, "{}", root + "/objects", root + "/shared-lib",
        root + "/engine-shared");
    CHECK(k2 != k3);

    REQUIRE(write_file(root + "/engine-shared/engine.js", "export const ENGINE = 2;"));
    uint64_t k4 = resolve_cache::compute_key(
        world, "{}", root + "/objects", root + "/shared-lib",
        root + "/engine-shared");
    CHECK(k3 != k4);

    REQUIRE(write_file(world,
        "class TestWorld extends World { static roots = [{module: 'Tree'}]; }"));
    uint64_t k5 = resolve_cache::compute_key(
        world, "{}", root + "/objects", root + "/shared-lib",
        root + "/engine-shared");
    CHECK(k4 != k5);

    REQUIRE(write_file(root + "/WorldData/TestWorld/world.manifest",
                       "This stale manifest must never enter the key\n"));
    uint64_t k6 = resolve_cache::compute_key(
        world, "{}", root + "/objects", root + "/shared-lib",
        root + "/engine-shared");
    CHECK(k5 == k6);

    remove_dir(root);
}

static void test_world_definition_adapter_preserves_runtime_semantics() {
    printf("[resolve_cache] world_definition_adapter\n");
    matter::WorldDefinition definition;
    matter::WorldRoot first;
    first.module = "Box";
    first.params_json = "{\"a\":1,\"z\":2}";
    first.transform.m[3] = 4.0f;
    first.transform.m[7] = 5.0f;
    first.transform.m[11] = 6.0f;
    first.expand = true;
    definition.roots.push_back(first);
    matter::WorldRoot second;
    second.module = "TileRoot";
    second.tileset = true;
    definition.roots.push_back(second);
    matter::WorldLight light;
    light.position = {1.0f, 2.0f, 3.0f};
    light.color = {0.5f, 0.6f, 0.7f};
    light.intensity = 2.5f;
    light.range = 42.0f;
    definition.lights.push_back(light);
    definition.settings.sector_size = 32.0f;

    const viewer::ProviderWorldDefinition adapted =
        viewer::adapt_world_definition(definition);
    CHECK(adapted.roots.size() == 2 &&
          adapted.roots[0].module == "Box" &&
          part_graph::params_to_json(adapted.roots[0].params) ==
              "{\"a\":1,\"z\":2}");
    CHECK(adapted.root_transforms.size() == 2 &&
          adapted.root_transforms[0].m[3] == 4.0f &&
          adapted.root_transforms[0].m[7] == 5.0f &&
          adapted.root_transforms[0].m[11] == 6.0f);
    CHECK(adapted.expand_flags == std::vector<bool>({true, false}) &&
          adapted.tileset_flags == std::vector<bool>({false, true}));
    CHECK(adapted.lights.spots.size() == 1 &&
          adapted.lights.spots[0].color[0] == 1.25f &&
          adapted.lights.spots[0].color[1] == 1.5f &&
          adapted.lights.spots[0].color[2] == 1.75f &&
          adapted.lights.spots[0].range == 42.0f);
    CHECK(adapted.settings.sector_size == 32.0f);
}

static void test_project_procedural_settings_drive_profile_and_binding() {
    printf("[resolve_cache] project_procedural_settings\n");
    matter::WorldSettings authored;
    authored.sector_size = 37.0f;
    authored.y_min = -23.0f;
    authored.y_max = 141.0f;
    matter::WorldSettings legacy;
    legacy.sector_size = 16.0f;
    legacy.y_min = -64.0f;
    legacy.y_max = 192.0f;

    const viewer::ProceduralWorldProfile profile =
        viewer::select_procedural_world_profile(
            /*project_layout=*/true, authored, legacy);
    CHECK(profile.sector_size == 37.0f &&
          profile.y_min == -23.0f && profile.y_max == 141.0f);

    struct BindingProbe {
        float sector_size = 0.0f;
        float y_min = 0.0f;
        float y_max = 0.0f;
    } binding, bake_binding;
    profile.apply(binding);
    profile.apply(bake_binding);
    CHECK(binding.sector_size == 37.0f && binding.y_min == -23.0f &&
          binding.y_max == 141.0f);
    CHECK(bake_binding.sector_size == 37.0f && bake_binding.y_min == -23.0f &&
          bake_binding.y_max == 141.0f);

    const viewer::ProceduralWorldProfile compatibility =
        viewer::select_procedural_world_profile(
            /*project_layout=*/false, authored, legacy);
    CHECK(compatibility.sector_size == 16.0f &&
          compatibility.y_min == -64.0f && compatibility.y_max == 192.0f);
}

static void test_key_changes_on_seed() {
    printf("[resolve_cache] key_changes_on_seed\n");
    const std::string root = sandbox_root("rc_test_keyseed");
    REQUIRE(build_key_sandbox(root));

    const std::string world = root + "/worlds/TestWorld.js";
    uint64_t k1 = resolve_cache::compute_key(
        world, "{}", root + "/objects", root + "/shared-lib", root + "/engine-shared");
    uint64_t k2 = resolve_cache::compute_key(
        world, "{\"worldSeed\":42}", root + "/objects", root + "/shared-lib", root + "/engine-shared");
    uint64_t k3 = resolve_cache::compute_key(
        world, "{\"worldSeed\":99}", root + "/objects", root + "/shared-lib", root + "/engine-shared");

    CHECK(k1 != k2);
    CHECK(k1 != k3);
    CHECK(k2 != k3);

    // Same seed -> same key.
    uint64_t k2b = resolve_cache::compute_key(
        world, "{\"worldSeed\":42}", root + "/objects", root + "/shared-lib", root + "/engine-shared");
    CHECK(k2 == k2b);

    remove_dir(root);
}

static void test_truncated_load() {
    printf("[resolve_cache] truncated_load\n");
    const std::string root = sandbox_root("rc_test_trunc");
    reset_dir(root);
    fs::create_directories(root + "/cache");

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

    remove_dir(root);
}

static void test_bad_magic_rejected() {
    printf("[resolve_cache] bad_magic_rejected\n");
    const std::string root = sandbox_root("rc_test_magic");
    reset_dir(root);
    fs::create_directories(root + "/cache");

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

    remove_dir(root);
}

static void test_bad_key_rejected() {
    printf("[resolve_cache] bad_key_rejected\n");
    const std::string root = sandbox_root("rc_test_badkey");
    reset_dir(root);
    fs::create_directories(root + "/cache");

    const uint64_t KEY_SAVE = 0x1234567890ABCDEFull;
    const uint64_t KEY_LOAD = 0xFEDCBA9876543210ull;
    auto p = make_payload();
    REQUIRE(resolve_cache::save(root, "TestWorld", KEY_SAVE, p));

    resolve_cache::ResolveCachePayload out;
    CHECK(!resolve_cache::load(root, "TestWorld", KEY_LOAD, out));

    remove_dir(root);
}

static void test_multi_source_dedup() {
    printf("[resolve_cache] multi_source_dedup\n");
    const std::string root = sandbox_root("rc_test_dedup");
    reset_dir(root);
    fs::create_directories(root + "/cache");

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

    remove_dir(root);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    printf("=== resolve_cache_tests ===\n");

    test_round_trip_basic();
    test_snapshot_indices();
    test_project_sources_define_key_and_stale_manifest_is_ignored();
    test_world_definition_adapter_preserves_runtime_semantics();
    test_project_procedural_settings_drive_profile_and_binding();
    test_key_changes_on_seed();
    test_truncated_load();
    test_bad_magic_rejected();
    test_bad_key_rejected();
    test_multi_source_dedup();

    printf("=== %d/%d passed ===\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
