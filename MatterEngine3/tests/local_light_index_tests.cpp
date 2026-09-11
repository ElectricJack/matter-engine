// GL/Vulkan-independent contract tests for resolved local-light packing,
// world-space indexing, publication revisions and reference attenuation.

#include "check.h"
#include "../src/world_lights.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

world_lights::LocalLight point(float x, float y, float z, float range = 3.0f,
                               float intensity = 1.0f) {
    world_lights::LocalLight light{};
    light.position[0] = x;
    light.position[1] = y;
    light.position[2] = z;
    light.range = range;
    light.cos_inner = -1.0f;
    light.cos_outer = -1.0f;
    light.color[0] = intensity;
    light.color[1] = intensity * 0.5f;
    light.color[2] = intensity * 0.25f;
    light.source_radius = 0.1f;
    light.kind = static_cast<std::uint32_t>(
        world_lights::LocalLightKind::Point);
    return light;
}

bool contains(const std::vector<std::uint32_t>& values,
              std::uint32_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<std::uint32_t> query(
    const world_lights::LocalLightPublication& publication,
    float x, float y, float z) {
    const float position[3] = {x, y, z};
    std::vector<std::uint32_t> candidates;
    std::string error;
    CHECK(world_lights::query_local_light_candidates(
              publication.index, position, candidates, error),
          error.c_str());
    return candidates;
}

float evaluated_red(const world_lights::LocalLightPublication& publication,
                    const float position[3], bool indexed) {
    std::vector<std::uint32_t> candidates;
    if (indexed) {
        std::string error;
        if (!world_lights::query_local_light_candidates(
                publication.index, position, candidates, error))
            return -1.0f;
    } else {
        candidates.resize(publication.records.size());
        for (std::uint32_t i = 0; i < candidates.size(); ++i)
            candidates[i] = i;
    }

    float total = 0.0f;
    for (std::uint32_t index : candidates) {
        float rgb[3]{};
        world_lights::evaluate_local_light_irradiance(
            publication.records[index], position, rgb);
        total += rgb[0];
    }
    return total;
}

void test_empty_and_transactional_failure() {
    world_lights::LocalLightPublication publication;
    std::string error;
    CHECK(world_lights::rebuild_local_light_publication(publication, error),
          error.c_str());
    CHECK(publication.revision != 0u,
          "empty publication still has a stable clear-state revision");
    CHECK(publication.index.cells.empty() &&
              publication.index.light_indices.empty() &&
              publication.index.oversized_light_indices.empty(),
          "empty input builds an empty index");
    CHECK(query(publication, -100.0f, 20.0f, 7.0f).empty(),
          "empty index returns no candidates");

    const std::uint64_t previous_revision = publication.revision;
    const auto previous_cells = publication.index.cells;
    world_lights::LocalLightIndexConfig invalid;
    invalid.cell_size = 0.0f;
    CHECK(!world_lights::rebuild_local_light_publication(
              publication, invalid, error),
          "invalid index configuration returns a real failure");
    CHECK(publication.revision == previous_revision &&
              publication.index.cells.size() == previous_cells.size(),
          "failed rebuild leaves the previous publication intact");
}

void test_negative_coordinates_and_boundaries() {
    world_lights::LocalLightPublication publication;
    publication.records.push_back(point(-8.0f, 1.0f, 1.0f, 0.1f));
    publication.records.push_back(point(0.0f, 1.0f, 1.0f, 0.1f));
    publication.records.push_back(point(8.0f, 1.0f, 1.0f, 0.1f));
    std::string error;
    CHECK(world_lights::rebuild_local_light_publication(publication, error),
          error.c_str());

    CHECK(contains(query(publication, -8.001f, 1.0f, 1.0f), 0u),
          "negative side of -8m maps to cell -2");
    CHECK(contains(query(publication, -8.0f, 1.0f, 1.0f), 0u),
          "exact -8m maps to cell -1");
    CHECK(contains(query(publication, -0.001f, 1.0f, 1.0f), 1u),
          "negative epsilon maps to cell -1");
    CHECK(contains(query(publication, 0.0f, 1.0f, 1.0f), 1u),
          "zero maps to cell 0");
    CHECK(contains(query(publication, 7.999f, 1.0f, 1.0f), 2u),
          "positive side below 8m maps to cell 0");
    CHECK(contains(query(publication, 8.0f, 1.0f, 1.0f), 2u),
          "exact 8m maps to cell 1");
}

void test_distributed_many_lights_are_sparse() {
    world_lights::LocalLightPublication publication;
    for (int z = 0; z < 17; ++z)
        for (int x = 0; x < 17; ++x)
            publication.records.push_back(
                point(static_cast<float>(x * 24), 2.0f,
                      static_cast<float>(z * 24)));

    std::string error;
    CHECK(world_lights::rebuild_local_light_publication(publication, error),
          error.c_str());
    CHECK(publication.records.size() == 289u,
          "stress fixture contains more than 256 lights");
    CHECK(publication.index.stats.oversized_light_count == 0u,
          "distributed finite lights stay in spatial cells");
    CHECK(publication.index.stats.max_candidates_per_cell < 8u,
          "distributed lights do not degenerate into an all-light scan");

    for (std::uint32_t i = 0; i < publication.records.size(); ++i) {
        const auto& light = publication.records[i];
        const auto candidates = query(
            publication, light.position[0], light.position[1],
            light.position[2]);
        CHECK(contains(candidates, i),
              "every distributed light appears in its world-space cell");
    }
}

void test_dense_overlap_has_no_candidate_cap() {
    world_lights::LocalLightPublication publication;
    for (int i = 0; i < 129; ++i)
        publication.records.push_back(point(1.0f, 1.0f, 1.0f, 4.0f,
                                            1.0f + i * 0.01f));
    std::string error;
    CHECK(world_lights::rebuild_local_light_publication(publication, error),
          error.c_str());

    const auto candidates = query(publication, 2.0f, 1.0f, 1.0f);
    CHECK(candidates.size() == 129u,
          "more than 64 coincident lights are never truncated");
    bool ascending = true;
    for (std::uint32_t i = 0; i < candidates.size(); ++i)
        ascending = ascending && candidates[i] == i;
    CHECK(ascending, "coincident candidate order follows authored order");

    const float position[3] = {2.0f, 1.0f, 1.0f};
    CHECK(std::fabs(evaluated_red(publication, position, true) -
                    evaluated_red(publication, position, false)) < 1.0e-4f,
          "dense indexed evaluation preserves every contribution");
}

void test_oversized_fallback_and_reference_match() {
    world_lights::LocalLightPublication publication;
    publication.records.push_back(point(0.0f, 0.0f, 0.0f, 220.0f, 3.0f));
    publication.records.push_back(point(16.0f, 0.0f, 0.0f, 2.0f, 2.0f));
    std::string error;
    CHECK(world_lights::rebuild_local_light_publication(publication, error),
          error.c_str());
    CHECK(publication.index.oversized_light_indices ==
              std::vector<std::uint32_t>{0u},
          "large-range light uses the explicit oversized list");

    const float probes[][3] = {
        {-128.0f, 0.0f, 0.0f}, {0.0f, 5.0f, 0.0f},
        {16.0f, 0.5f, 0.0f}, {127.0f, 0.0f, 0.0f},
        {400.0f, 0.0f, 0.0f},
    };
    for (const auto& probe : probes) {
        const auto candidates = query(
            publication, probe[0], probe[1], probe[2]);
        CHECK(contains(candidates, 0u),
              "oversized light is available at every world-space query");
        CHECK(std::fabs(evaluated_red(publication, probe, true) -
                        evaluated_red(publication, probe, false)) < 1.0e-5f,
              "oversized plus cell candidates match brute-force evaluation");
    }
}

void test_unrepresentable_query_still_returns_oversized() {
    world_lights::LocalLightPublication publication;
    publication.records.push_back(point(3.0e10f, 0.0f, 0.0f, 2.0f));
    std::string error;
    CHECK(world_lights::rebuild_local_light_publication(publication, error),
          error.c_str());
    CHECK(publication.index.oversized_light_indices ==
              std::vector<std::uint32_t>{0u},
          "unrepresentable light coordinates use the oversized list");
    CHECK(query(publication, 3.0e10f, 0.0f, 0.0f) ==
              std::vector<std::uint32_t>{0u},
          "unrepresentable query coordinates retain oversized candidates");
}

void test_determinism_and_light_only_revision() {
    world_lights::LocalLightPublication first;
    for (int i = 0; i < 32; ++i)
        first.records.push_back(point(i * 2.25f - 18.0f,
                                      (i % 5) * 1.5f,
                                      (i % 7) * -2.0f, 9.0f));
    world_lights::LocalLightPublication second = first;
    std::string error;
    CHECK(world_lights::rebuild_local_light_publication(first, error),
          error.c_str());
    CHECK(world_lights::rebuild_local_light_publication(second, error),
          error.c_str());
    CHECK(first.revision == second.revision,
          "identical light-only reload has a stable revision");
    CHECK(first.index.light_indices == second.index.light_indices &&
              first.index.oversized_light_indices ==
                  second.index.oversized_light_indices &&
              first.index.cells.size() == second.index.cells.size() &&
              (first.index.cells.empty() ||
               std::memcmp(first.index.cells.data(), second.index.cells.data(),
                           first.index.cells.size() *
                               sizeof(world_lights::LocalLightCell)) == 0),
          "index packing is byte-deterministic");

    second.records[3].color[0] += 0.25f;
    CHECK(world_lights::rebuild_local_light_publication(second, error),
          error.c_str());
    CHECK(first.revision != second.revision,
          "light-only data edit changes the publication revision");
}

void test_spot_cone_and_finite_range_attenuation() {
    world_lights::LocalLight light = point(0.0f, 0.0f, 0.0f, 10.0f, 4.0f);
    light.kind = static_cast<std::uint32_t>(
        world_lights::LocalLightKind::Spot);
    light.direction[0] = 1.0f;
    constexpr float kPiOver180 = 3.14159265358979323846f / 180.0f;
    light.cos_inner = std::cos(10.0f * kPiOver180);
    light.cos_outer = std::cos(20.0f * kPiOver180);

    const float inside[3] = {2.0f, 0.0f, 0.0f};
    const float outside[3] = {-2.0f, 0.0f, 0.0f};
    const float cutoff[3] = {10.0f, 0.0f, 0.0f};
    const float at_source[3] = {0.0f, 0.0f, 0.0f};
    CHECK(world_lights::local_light_attenuation(light, inside) > 0.0f,
          "spot contributes inside its cone");
    CHECK(world_lights::local_light_attenuation(light, outside) == 0.0f,
          "spot contributes nothing outside its cone");
    CHECK(world_lights::local_light_attenuation(light, cutoff) == 0.0f,
          "finite range has an exact zero cutoff");
    CHECK(std::isfinite(
              world_lights::local_light_attenuation(light, at_source)),
          "source-radius softening prevents a singularity");

    light.kind = static_cast<std::uint32_t>(
        world_lights::LocalLightKind::Point);
    light.range = 1.0e20f;
    const float one_metre_away[3] = {1.0f, 0.0f, 0.0f};
    CHECK(world_lights::local_light_attenuation(light, one_metre_away) > 0.0f,
          "large finite ranges do not overflow the cutoff calculation");
}

} // namespace

int main() {
    test_empty_and_transactional_failure();
    test_negative_coordinates_and_boundaries();
    test_distributed_many_lights_are_sparse();
    test_dense_overlap_has_no_candidate_cap();
    test_oversized_fallback_and_reference_match();
    test_unrepresentable_query_still_returns_oversized();
    test_determinism_and_light_only_revision();
    test_spot_cone_and_finite_range_attenuation();
    return check_summary();
}
