#include "provider/sector_resolver.h"
#include "check.h"
#include <cmath>
#include <cstring>
#include <limits>

int main() {
    sector_grid::Sectors sectors;
    const sector_grid::SectorCoord occupied{0,0,0}, unknown{1,0,0};
    for (uint32_t i = 0; i < 20000; ++i) {
        world_flatten::FlatInstance f;
        f.resolved_hash = 1+i%2; f.stable_id = i+1;
        f.world = mat4::Translate(make_float3(3+float(i%5),0,0));
        sectors[occupied].push_back(f);
    }
    world_flatten::FlatInstance missing;
    missing.resolved_hash = 999;
    missing.world = mat4::Translate(make_float3(0.25f,0,0));
    sectors[occupied].push_back(missing);
    sectors[unknown].push_back(missing);
    lod_select::PartLodTable parts;
    parts[1] = {2, {0.5f, 0.1f, 0}};
    parts[2] = {1, {0.5f, 0.1f, 0}};
    lod_select::SectorParts indexed_parts{{occupied, {1, 2, 999}}, {unknown, {999}}};
    for (float camera : {0.0f, 20.0f, 100.0f}) {
        const auto choices = lod_select::select_sector_lods_ex(sectors, parts, make_float3(camera,0,0));
        const auto indexed = lod_select::select_sector_lods_ex(
            sectors, parts, make_float3(camera,0,0), 0, 1, &indexed_parts);
        CHECK(indexed.size() == choices.size(), "indexed selection preserves sector membership");
        CHECK(choices.size() == 1 && choices.count(unknown) == 0, "unknown-only sectors remain absent");
        const auto& selected = choices.at(occupied);
        CHECK(selected.size() == 2, "repeated foliage chooses each known part once");
        const float distance = camera == 0 ? 0.25f : camera-7;
        for (uint64_t hash : {1ull,2ull}) {
            CHECK(std::abs(selected.at(hash).distance-distance) < 1e-5f,
                  "all parts use the closest instance in the entire sector, including unknown parts");
            CHECK(indexed.at(occupied).at(hash).distance == selected.at(hash).distance &&
                  indexed.at(occupied).at(hash).level == selected.at(hash).level,
                  "precomputed hashes preserve exact distance and rung choices");
            CHECK(selected.at(hash).level == lod_select::select_level(parts.at(hash).bound_radius/distance,
                                                                      parts.at(hash).thresholds),
                  "deduplicated selection matches the projected-size reference");
        }
    }
    auto culled = lod_select::select_sector_lods_ex(sectors, parts, make_float3(100,0,0), 0.1f);
    CHECK(culled.at(occupied).at(1).level == -1 && culled.at(occupied).at(2).level == -1,
          "the subpixel floor still applies to repeated parts");
    parts[1].thresholds = {0.001f,0};
    auto changed = lod_select::select_sector_lods_ex(sectors, parts, make_float3(100,0,0));
    CHECK(changed.at(occupied).at(1).level == 0, "ladder edits take effect on the next selection");

    viewer::WorldManifest manifest;
    for (uint32_t i = 0; i < 10000; ++i) {
        viewer::WorldManifestEntry e;
        e.instance_id = i+1; e.part_hash = 1+i%2;
        e.transform[0] = e.transform[5] = e.transform[10] = e.transform[15] = 1;
        e.transform[3] = float(i%31)-15;
        manifest.instances.push_back(e);
    }
    viewer::WorldState state; state.reset(manifest);
    viewer::SectorLodResolver resolver(16, std::numeric_limits<float>::infinity());
    const auto near = resolver.resolve(state, parts, make_float3(0,0,0));
    const auto far = resolver.resolve(state, parts, make_float3(100,0,0));
    CHECK(near.size() == 10000 && far.size() == near.size(), "preallocation retains the whole active forest");
    for (size_t i = 0; i < near.size(); ++i)
        CHECK(near[i].stable_id == far[i].stable_id && near[i].transform[3] == far[i].transform[3],
              "camera movement retains instance ordering and placement");
    CHECK(resolver.rebin_count() == 1, "moving the camera does not rebin the forest");
    const uint64_t built = resolver.output_rebuild_count();
    resolver.resolve(state, parts, make_float3(101,0,0));
    CHECK(resolver.output_rebuild_count() == built,
          "camera movement reuses output when activation and rungs stay unchanged");
    resolver.set_active_radius(1);
    CHECK(resolver.resolve(state, parts, make_float3(100,0,0)).empty(), "inactive sectors emit nothing");

    // A fresh resolver is the uncached oracle. Compare every output field
    // after edits that must invalidate the retained vector, including inline
    // child transforms which depend on more than the parent rung.
    const auto verify = [&](const viewer::WorldState& world, float camera,
                            float floor = 0.0f, float budget = 1.0f) {
        viewer::SectorLodResolver fresh(16, std::numeric_limits<float>::infinity());
        fresh.set_min_projected_size(floor); fresh.set_pixel_budget(budget);
        resolver.set_active_radius(std::numeric_limits<float>::infinity());
        resolver.set_min_projected_size(floor); resolver.set_pixel_budget(budget);
        const auto& expected = fresh.resolve(world, parts, make_float3(camera,0,0));
        const auto& actual = resolver.resolve(world, parts, make_float3(camera,0,0));
        CHECK(actual.size() == expected.size(), "retained resolver matches fresh output count");
        for (size_t i = 0; i < actual.size() && i < expected.size(); ++i)
            CHECK(actual[i].part_hash == expected[i].part_hash &&
                  actual[i].stable_id == expected[i].stable_id &&
                  actual[i].lod_level == expected[i].lod_level &&
                  actual[i].segment == expected[i].segment &&
                  std::memcmp(actual[i].transform, expected[i].transform, sizeof(actual[i].transform)) == 0,
                  "retained resolver matches fresh identities, rungs, segments and transforms");
    };
    verify(state, 100);
    verify(state, 100, 0.1f);
    verify(state, 100, 0, 2);
    parts[2].thresholds = {0.001f, 0};
    verify(state, 100);
    parts.erase(2); // Newly unknown parts must still emit the default rung.
    verify(state, 100);
    parts[2] = {1, {0.5f, 0.1f, 0}};
    verify(state, 100);
    auto edited = manifest;
    edited.instances.back().transform[3] += 40;
    viewer::WorldState other;
    other.reset(edited); // Same version number, different world object.
    CHECK(other.version() == state.version(), "fixture has equal world version numbers");
    verify(other, 100);
    verify(state, 100);
    state.reset(edited);
    verify(state, 100);
    parts[1].inline_cutover = 0.1f;
    lod_select::PartLodRef ref{};
    ref.child_hash = 2; ref.child_scale = 1;
    ref.rel_transform[0] = ref.rel_transform[5] = ref.rel_transform[10] = ref.rel_transform[15] = 1;
    ref.rel_transform[3] = 0.5f;
    parts[1].refs = {ref};
    verify(state, 0);
    parts[1].refs[0].rel_transform[3] = 3;
    verify(state, 0);
    verify(state, 50);
    parts[1].inline_cutover = 0;
    verify(state, 0);
    return check_summary();
}
