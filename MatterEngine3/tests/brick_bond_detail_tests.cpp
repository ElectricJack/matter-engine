#include "brick_bond_detail.h"
#include "provider/detail_cache_validation.h"
#include "check.h"
#include <fstream>
#include <iterator>
#include <string>
namespace {
std::string read(const char *path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void cache_validation_test() {
    const auto check_case = [](bool cached, int failed_loads, bool can_recover,
                               bool generation_ok, bool expected_ok,
                               int expected_generations, int expected_loads, bool expected_rebuilt) {
        int generations = 0, loads = 0;
        bool rebuilt = false;
        const bool okay = viewer::load_detail_with_cache_recovery(cached,
            [&] { ++generations; return generation_ok; },
            [&] { return ++loads > failed_loads; },
            [&] { return can_recover; }, rebuilt);
        CHECK(okay == expected_ok && generations == expected_generations &&
                  loads == expected_loads && rebuilt == expected_rebuilt,
              "cache recovery respects one regeneration and load attempt bound");
    };
    check_case(true, 0, true, true, true, 0, 1, false); // Healthy warm cache.
    check_case(true, 1, true, true, true, 1, 2, true); // Failed cached decode healed.
    check_case(true, 2, true, true, false, 1, 2, true); // Reload failure is final.
    check_case(false, 1, true, true, false, 1, 1, false); // Cold failure has no retry.
    check_case(true, 1, false, true, false, 0, 1, false); // Cancelled/non-backend failure.
    check_case(true, 1, true, false, false, 1, 1, true); // Regeneration failure is final.
}
void tests() {
    cache_validation_test();
    script_host::ScriptHost host;
    host.set_shared_lib_roots({"projects/world_demo/shared-lib", "MatterEngine3/shared-lib"});
    const auto descriptor_source = read("projects/world_demo/objects/CastleBrickBondDetail.js");
    const auto source = read("projects/world_demo/objects/CastleStoneSource.js");
    CHECK(!descriptor_source.empty() && !source.empty(),
          "actual authored detail/source files available");
    if (descriptor_source.empty() || source.empty())
        return;
    const auto merged = host.merged_params_json(descriptor_source, "{}");
    detail_bake::BrickBondDescriptor descriptor;
    bool recognized = false;
    std::string error;
    CHECK(detail_bake::parse_brick_bond_descriptor(merged, descriptor, recognized, error),
          error.c_str());
    CHECK(recognized && descriptor.source_module == "CastleStoneSource" && descriptor.variants == 8,
          "shared-library declarative metadata recognized without build");
    CHECK(descriptor.bond.tile_pixels == 512 && descriptor.bond.columns == 4 &&
              descriptor.bond.rows == 8,
          "finite periodic atlas dimensions");
    auto preserved = descriptor.source_module;
    for (const std::string invalid :
         {R"({"detailBake":"brickBondV1","sourceModule":"../outside"})",
          R"({"detailBake":"brickBondV1","sourceModule":"Source","variants":7})",
          R"({"detailBake":"brickBondV1","sourceModule":"Source","sourceParams":[]})",
          R"({"detailBake":"brickBondV1","sourceModule":"Source","bond":{"tilePixels":256}})",
          R"({"detailBake":"brickBondV1","sourceModule":"Source","bond":{"mortarRgb":[256,1,2]}})",
          R"({"detailBake":"brickBondV1","sourceModule":"Source","unknown":1})"}) {
        CHECK(!detail_bake::parse_brick_bond_descriptor(invalid, descriptor, recognized, error),
              "malformed declarative detail fails closed");
        CHECK(descriptor.source_module == preserved, "invalid descriptor preserves prior output");
    }
    CHECK(detail_bake::parse_brick_bond_descriptor("{}", descriptor, recognized, error) &&
              !recognized,
          "legacy detail remains on existing settle path");
    uint32_t gpu_calls = 0;
    host.set_solid_source_baker([&](const gpu_meshing::SolidJob &, gpu_meshing::MeshResult &,
                                    gpu_meshing::SolidStats &, gpu_meshing::Error &,
                                    const gpu_meshing::BuildControl &) {
        ++gpu_calls;
        return false;
    });
    const auto descriptor_hash = host.resolve_hash(descriptor_source, "{}");
    detail_bake::PreparedBrickBond prepared;
    script_host::SolidSourceEvaluationOptions options;
    CHECK(detail_bake::prepare_brick_bond_sources(descriptor, descriptor_hash, source, host,
                                                  options, prepared, error),
          error.c_str());
    CHECK(prepared.cache_key != 0 && gpu_calls == 0,
          "cache identity established before GPU or mesh callback");
    const auto old_key = prepared.cache_key;
    auto palette = descriptor;
    palette.bond.brick_rgb[0][0]++;
    CHECK(detail_bake::prepare_brick_bond_sources(palette, descriptor_hash, source, host, options,
                                                  prepared, error),
          error.c_str());
    CHECK(prepared.cache_key != old_key, "palette enters preprojection identity");
    CHECK(detail_bake::prepare_brick_bond_sources(descriptor, descriptor_hash,
                                                  source + "\n// selected source edit\n", host,
                                                  options, prepared, error),
          error.c_str());
    CHECK(prepared.cache_key != old_key, "actual resolved source identity enters atlas cache key");
    const auto front = detail_bake::brick_bond_face_job(descriptor, prepared.sources[0].job(),
                                                        prepared.sources[0].resolved_hash, false);
    const auto back = detail_bake::brick_bond_face_job(descriptor, prepared.sources[0].job(),
                                                       prepared.sources[0].resolved_hash, true);
    CHECK(front.frame.u.x == 1 && front.frame.n.z == 1 && back.frame.u.x == -1 &&
              back.frame.n.z == -1,
          "front/back projections retain proper finite frames");
    CHECK(gpu_meshing::face_recipe_digest(front) != gpu_meshing::face_recipe_digest(back),
          "both source face frames enter identity");
    CHECK(gpu_calls == 0, "all cache probes remain source-only");
}
} // namespace
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        tests();
    } catch (const std::exception &e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    return check_summary();
}
