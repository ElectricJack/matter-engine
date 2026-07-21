// Phase C Task 2: transient artifact routing (tmpfs scratch dir)
//
// Transient modules (e.g., terrain sectors) bake to a per-process scratch dir
// (/tmp/matter_transient/<pid>/) and are released via release_transient.
// Persistent modules (e.g., rocks, trees) use the normal parts cache.
//
// Tests:
//   (a) bake a transient module and verify the artifact lands in scratch, not cache
//   (b) verify PartStore::get_or_load finds the artifact via scratch lookup
//   (c) release_transient unlinks the scratch file
//   (d) bake a persistent module and verify its artifact lands in cache (unchanged)

#include "check.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>

// Include the provider/graph headers the demand_bake_tests suite uses
#include "../src/provider/local_provider.h"
#include "../src/part_graph.h"
#include "../src/part_asset_v2.h"
#include "../src/render/part_store.h"

namespace fs = std::filesystem;

static std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

static void write_bytes(const std::string& path,
                        const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

static void make_prior_schema(std::vector<uint8_t>& bytes) {
    const uint32_t prior = MaterialRegistrySchemaVersion() - 1u;
    std::memcpy(bytes.data() + 40, &prior, sizeof(prior));
    const uint64_t body_hash =
        part_asset::fnv1a64(bytes.data() + 40, bytes.size() - 40);
    std::memcpy(bytes.data() + 32, &body_hash, sizeof(body_hash));
}

// Build a sandbox with transient (Terrain) and persistent (Rock) modules.
// Terrain: transient leaf part (no requires)
// Rock: persistent leaf part (no requires)
static bool build_transient_sandbox(const std::string& root) {
    std::error_code ec;
    fs::remove_all(root, ec);
    ec.clear();
    fs::create_directories(root + "/objects", ec);
    fs::create_directories(root + "/worlds", ec);
    fs::create_directories(root + "/shared-lib", ec);
    fs::create_directories(root + "/.cache/Demo/parts", ec);
    if (ec) return false;

    // Terrain — transient leaf (no requires)
    {
        std::ofstream f(root + "/objects/Terrain.js");
        if (!f) return false;
        f << "class Terrain extends Part {\n"
          << "  build(p) {\n"
          << "    this.fill(MAT.stone);\n"
          << "    const S = 0.5;\n"
          << "    this.beginShape(SHAPE.triangles);\n"
          << "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
          << "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
          << "    this.endShape();\n"
          << "  }\n"
          << "}\n";
    }

    // Rock — persistent leaf (no requires)
    {
        std::ofstream f(root + "/objects/Rock.js");
        if (!f) return false;
        f << "class Rock extends Part {\n"
          << "  build(p) {\n"
          << "    this.fill(MAT.stone);\n"
          << "    const S = 0.3;\n"
          << "    this.beginShape(SHAPE.triangles);\n"
          << "    this.vertex(-S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, -S);\n"
          << "    this.vertex(S, 0, -S); this.vertex(-S, 0, S); this.vertex(S, 0, S);\n"
          << "    this.endShape();\n"
          << "  }\n"
          << "}\n";
    }

    // World class: two roots (Terrain and Rock)
    {
        std::ofstream f(root + "/worlds/Demo.js");
        if (!f) return false;
        f << "class Demo extends World {\n"
          << "  static roots = [\n"
          << "    { module: 'Terrain', transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] },\n"
          << "    { module: 'Rock', transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] },\n"
          << "  ];\n"
          << "}\n";
    }

    return true;
}

// Run install_graph() on the sandbox using LocalProvider directly (headless).
static std::unique_ptr<viewer::LocalProvider> make_provider(
    const std::string& sandbox,
    const std::string& world_name = "Demo") {
    auto cfg = viewer::LocalProviderConfig::for_project(sandbox, world_name, "");
    cfg.gl_available = false;
    return std::make_unique<viewer::LocalProvider>(std::move(cfg));
}

// Main test: fixtures and assertions from the brief
int main() {
    std::string sandbox = (fs::temp_directory_path() / "me3_transient_tests").string();

    // Arrange a provider exactly the way demand_bake_tests.cpp does
    if (!build_transient_sandbox(sandbox)) {
        printf("FAIL: build_transient_sandbox\n");
        return 1;
    }

    auto prov = make_provider(sandbox);

    // Set transient modules: Terrain bakes to scratch, Rock to cache
    std::set<std::string> transient_mods;
    transient_mods.insert("Terrain");
    prov->set_transient_modules(transient_mods);

    // 1. Install graph and bake
    std::string err;
    bool ok = prov->install_graph(err);
    CHECK(ok, "install_graph() succeeded");
    if (!ok) {
        printf("  err: %s\n", err.c_str());
        return 1;
    }

    const std::string cache_root = sandbox + "/.cache/Demo";
    const auto& snap = prov->graph_snapshot();

    // Get Terrain and Rock hashes
    uint64_t terrain_hash = 0, rock_hash = 0;
    {
        auto it = snap.nodes.find("Terrain");
        if (it != snap.nodes.end()) terrain_hash = it->second.resolved_hash;
        it = snap.nodes.find("Rock");
        if (it != snap.nodes.end()) rock_hash = it->second.resolved_hash;
    }

    CHECK(terrain_hash != 0, "Terrain hash resolved");
    CHECK(rock_hash != 0, "Rock hash resolved");

    printf("  terrain_hash=%016llx, rock_hash=%016llx\n",
           (unsigned long long)terrain_hash, (unsigned long long)rock_hash);

    // 2. Verify Terrain artifact is in scratch dir, not cache
    const std::string scratch_terrain = prov->transient_dir() + "/" +
                                        part_asset::cache_path_resolved(terrain_hash);
    const std::string cache_terrain = cache_root + "/" +
                                      part_asset::cache_path_resolved(terrain_hash);

    CHECK(fs::is_regular_file(scratch_terrain),
          "Terrain .part exists under scratch dir");
    CHECK(!fs::exists(cache_terrain),
          "Terrain .part does NOT exist under cache dir");
    printf("  Terrain transient file: %s\n", scratch_terrain.c_str());

    const std::string scratch_flat = prov->transient_dir() + "/" +
                                     part_asset::cache_path_flat(terrain_hash);
    const std::string cache_flat = cache_root + "/" +
                                   part_asset::cache_path_flat(terrain_hash);

    // 3. Create the first transient flat, then turn both scratch and cache copies
    // into valid-hash prior-schema artifacts. Scratch must remain the selected
    // root; the stale persistent copy must never win.
    {
        bool ok = prov->ensure_part_flattened(terrain_hash);
        CHECK(ok, "initial transient flatten succeeds");
        std::vector<uint8_t> stale = read_bytes(scratch_flat);
        CHECK(stale.size() >= 44, "transient flat fixture has material prefix");
        if (stale.size() >= 44) {
            make_prior_schema(stale);
            write_bytes(scratch_flat, stale);
            write_bytes(cache_flat, stale);
        }
        CHECK(!part_asset::is_cache_artifact_header_compatible(
                  scratch_flat, terrain_hash, part_asset::kFormatVersionFlat),
              "scratch flat is deliberately incompatible");
        CHECK(!part_asset::is_cache_artifact_header_compatible(
                  cache_flat, terrain_hash, part_asset::kFormatVersionFlat),
              "cache flat is deliberately incompatible");

        ok = prov->ensure_part_flattened(terrain_hash);
        CHECK(ok, "incompatible transient flat automatically regenerates");
        CHECK(part_asset::is_cache_artifact_header_compatible(
                  scratch_flat, terrain_hash, part_asset::kFormatVersionFlat),
              "regenerated transient flat is current");
        CHECK(!part_asset::is_cache_artifact_header_compatible(
                  cache_flat, terrain_hash, part_asset::kFormatVersionFlat),
              "stale persistent flat remains untouched when scratch is selected");

        BLASManager loaded_blas; TLASManager loaded_tlas(16);
        std::vector<part_asset::FlatCluster> loaded_clusters;
        std::vector<part_asset::FlatInstanceRef> loaded_refs;
        CHECK(part_asset::load_flat_v3(scratch_flat, terrain_hash,
                                       loaded_blas, loaded_tlas,
                                       loaded_clusters, loaded_refs),
              "regenerated transient flat loads successfully");

        const std::vector<uint8_t> warm_before = read_bytes(scratch_flat);
        CHECK(prov->ensure_part_flattened(terrain_hash),
              "second transient ensure succeeds");
        CHECK(read_bytes(scratch_flat) == warm_before,
              "second transient ensure is warm and does not rewrite flat");
    }

    // 4. Verify Terrain's .flat.part exists in scratch, NOT in cache
    {
        CHECK(part_asset::is_cache_artifact_header_compatible(
                  scratch_flat, terrain_hash, part_asset::kFormatVersionFlat),
              "Terrain .flat.part exists in scratch (valid format)");
        CHECK(!part_asset::is_cache_artifact_header_compatible(
                  cache_flat, terrain_hash, part_asset::kFormatVersionFlat),
              "stale Terrain cache flat was not selected");
        printf("  Terrain flat in scratch: %s\n", scratch_flat.c_str());
    }

    // 5. Put one boundary ref in the current scratch flat. connect() must load
    // refs from that same selected root (not the stale persistent flat) and
    // expand the referenced Rock into an additional manifest instance.
    {
        BLASManager flat_blas; TLASManager flat_tlas(16);
        std::vector<part_asset::FlatCluster> clusters;
        std::vector<part_asset::FlatInstanceRef> refs;
        CHECK(part_asset::load_flat_v3(scratch_flat, terrain_hash, flat_blas,
                                       flat_tlas, clusters, refs),
              "scratch flat loads before adding boundary ref");
        part_asset::FlatInstanceRef ref{};
        ref.child_resolved_hash = rock_hash;
        for (int i = 0; i < 16; ++i) ref.transform[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        refs.push_back(ref);
        CHECK(part_asset::save_flat_v3(scratch_flat, flat_blas, flat_tlas,
                                       clusters, refs, terrain_hash),
              "scratch flat with boundary ref saved");
        BLASManager verify_blas; TLASManager verify_tlas(16);
        std::vector<part_asset::FlatCluster> verify_clusters;
        std::vector<part_asset::FlatInstanceRef> verify_refs;
        CHECK(part_asset::load_flat_v3(scratch_flat, terrain_hash, verify_blas,
                                       verify_tlas, verify_clusters, verify_refs) &&
                  verify_refs.size() == 1,
              "scratch flat persists one boundary ref before connect");

        viewer::WorldManifest manifest;
        std::string connect_err;
        CHECK(prov->connect(manifest, connect_err),
              "connect succeeds with scratch flat boundary ref");
        size_t rock_instances = 0;
        for (const auto& instance : manifest.instances)
            if (instance.part_hash == rock_hash) ++rock_instances;
        CHECK(rock_instances >= 2,
              "scratch flat boundary ref expands into an additional Rock instance");
        BLASManager post_blas; TLASManager post_tlas(16);
        std::vector<part_asset::FlatCluster> post_clusters;
        std::vector<part_asset::FlatInstanceRef> post_refs;
        CHECK(part_asset::load_flat_v3(scratch_flat, terrain_hash, post_blas,
                                       post_tlas, post_clusters, post_refs) &&
                  post_refs.size() == 1,
              "connect keeps selected scratch flat and its boundary ref warm");
    }

    // 6. Test ensure_part_flattened for persistent Rock (flatten to cache root)
    {
        bool ok = prov->ensure_part_flattened(rock_hash);
        CHECK(ok, "ensure_part_flattened(rock_hash) returns true");
    }

    // 7. Verify Rock's .flat.part exists in cache, NOT in scratch
    {
        const std::string cache_flat = cache_root + "/" +
                                       part_asset::cache_path_flat(rock_hash);
        const std::string scratch_flat = prov->transient_dir() + "/" +
                                         part_asset::cache_path_flat(rock_hash);

        CHECK(part_asset::peek_format_version(cache_flat) == part_asset::kFormatVersionFlat,
              "Rock .flat.part exists in cache (valid format)");
        CHECK(part_asset::peek_format_version(scratch_flat) == 0,
              "Rock .flat.part does NOT exist in scratch (peek_format_version returns 0)");
        printf("  Rock flat in cache: %s\n", cache_flat.c_str());
    }

    // 8. Verify PartStore::get_or_load finds Terrain via scratch lookup
    // Create a test PartStore with the scratch dir configured
    viewer::PartStore store(cache_root);
    store.set_scratch_dir(prov->transient_dir());

    // Verify scratch-first has() lookup works
    CHECK(store.has(terrain_hash),
          "PartStore::has(terrain_hash) returns true via scratch-first lookup");

    const viewer::LoadedPart* loaded_terrain = store.get_or_load(terrain_hash);
    CHECK(loaded_terrain != nullptr, "PartStore::get_or_load(terrain_hash) succeeded via scratch");
    if (loaded_terrain == nullptr) {
        printf("  Failed to load Terrain from scratch\n");
        return 1;
    }

    // 9. Release Terrain and verify scratch files are gone
    prov->release_transient(terrain_hash);
    CHECK(!fs::exists(scratch_terrain),
          "Terrain scratch .part file deleted after release_transient");
    printf("  Terrain scratch file deleted\n");

    // 10. Verify Rock's artifact IS in cache (unchanged for persistent modules)
    const std::string cache_rock = cache_root + "/" +
                                   part_asset::cache_path_resolved(rock_hash);
    CHECK(fs::is_regular_file(cache_rock),
          "Rock .part exists under cache dir (persistent module)");
    printf("  Rock cache file: %s\n", cache_rock.c_str());

    // Cleanup
    std::error_code cleanup_ec;
    fs::remove_all(sandbox, cleanup_ec);

    printf("ALL TESTS PASSED\n");
    return check_summary();
}
