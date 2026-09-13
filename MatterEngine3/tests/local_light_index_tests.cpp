// GL/Vulkan-independent contract tests for resolved local-light packing,
// world-space indexing, publication revisions and reference attenuation.

#include "check.h"
#include "../src/world_lights.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
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

void test_scaled_publication() {
    using namespace world_lights;
    LocalLightPublication authored;
    authored.records = {point(0, 0, 0, 8.0f, 3.0f)};
    LocalLightIndexConfig config;
    config.cell_size = 1.0f;
    config.max_cells_per_light = 10000;
    std::string error;
    CHECK(rebuild_local_light_publication(authored, config, error), error.c_str());
    const LocalLight original = authored.records[0];
    const uint64_t original_revision = authored.revision;
    LocalLightPublication scaled;
    CHECK(make_scaled_local_light_publication(authored, 0.75f, scaled, error), error.c_str());
    CHECK(scaled.records[0].range == 6.0f, "effective range is 75 percent of authored range");
    LocalLight expected = original;
    expected.range = 6.0f;
    CHECK(std::memcmp(&expected, &scaled.records[0], sizeof(expected)) == 0,
          "only range changes; source radius, colors and flags remain exact");
    CHECK(std::memcmp(&original, &authored.records[0], sizeof(original)) == 0 &&
          authored.revision == original_revision, "authored source remains unchanged");
    CHECK(scaled.revision != authored.revision, "effective range changes publication revision");
    CHECK(scaled.index.cell_size == 1.0f && scaled.index.max_cells_per_light == 10000,
          "publication preserves explicit spatial index config");
    CHECK(contains(query(authored, 7.5f, 0, 0), 0) &&
          !contains(query(scaled, 7.5f, 0, 0), 0), "index uses the reduced GPU-record range");
    const uint64_t scaled_revision = scaled.revision;
    CHECK(rebuild_local_light_publication(scaled, config, error), error.c_str());
    CHECK(scaled.records[0].range == 6 && scaled.revision == scaled_revision,
          "renderer validation never compounds the range scale");
    LocalLightPublication repeat;
    CHECK(make_scaled_local_light_publication(authored, 0.75f, repeat, error), error.c_str());
    CHECK(repeat.revision == scaled.revision, "repeated source publication is deterministic");
    CHECK(make_scaled_local_light_publication(authored, repeat, error), error.c_str());
    CHECK(repeat.revision == authored.revision, "default scale one preserves original publication");
    for (float x : {0.0f, 1.0f, 5.0f, 5.99f, 6.0f, 7.0f}) {
        const float receiver[3] = {x, 0, 0};
        const float actual = local_light_attenuation(scaled.records[0], receiver);
        const double normalized = static_cast<double>(x) / 6.0;
        const double cutoff = std::max(0.0, 1.0 - normalized * normalized);
        const double reference = cutoff * cutoff /
            (static_cast<double>(x) * x + original.source_radius * original.source_radius);
        CHECK(std::fabs(actual - reference) <= 2e-5 * std::max(1.0, reference),
              "scaled range preserves squared smooth cutoff and unscaled source softening");
        CHECK(std::fabs(evaluated_red(scaled, receiver, true) -
                        evaluated_red(scaled, receiver, false)) < 1e-5f,
              "scaled indexed evaluation matches brute force");
    }
    const float near_edge[3] = {5.999f, 0, 0};
    CHECK(local_light_attenuation(scaled.records[0], near_edge) < 1e-7f,
          "attenuation smoothly approaches zero at shortened range");
    for (float bad : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::max()}) {
        CHECK(!make_scaled_local_light_publication(authored, bad, scaled, error),
              "invalid or overflowing range scale rejected");
        CHECK(scaled.revision == scaled_revision && scaled.records[0].range == 6,
              "failed scale preserves prior output");
    }
    LocalLightPublication spot_source = authored;
    spot_source.records[0].kind = static_cast<uint32_t>(LocalLightKind::Spot);
    spot_source.records[0].direction[0] = 1.0f;
    spot_source.records[0].cos_inner = 0.95f;
    spot_source.records[0].cos_outer = 0.8f;
    LocalLightPublication spot_scaled;
    CHECK(make_scaled_local_light_publication(spot_source, 0.75f, spot_scaled, error), error.c_str());
    const float inside_cone[3] = {2, 0, 0};
    const float outside_cone[3] = {0, 2, 0};
    const float beyond_range[3] = {7, 0, 0};
    CHECK(local_light_attenuation(spot_scaled.records[0], inside_cone) > 0 &&
          local_light_attenuation(spot_scaled.records[0], outside_cone) == 0 &&
          local_light_attenuation(spot_scaled.records[0], beyond_range) == 0,
          "spot cone remains unchanged while its finite range shortens");
    CHECK(!make_scaled_local_light_publication(authored, 0.75f, authored, error),
          "in-place scaling rejected to protect authored source");
    CHECK(authored.revision == original_revision && authored.records[0].range == 8,
          "alias rejection leaves source intact");
}

void test_importance_selection() {
    using namespace world_lights;
    std::string error;
    std::vector<LocalLightContribution> contributions;
    const float receiver[3] = {0, 0, 0};
    // Nearby low-intensity light beats distant modest lights; a sufficiently
    // bright distant light also survives. Range and cone rejection happen
    // before ranking, exactly as in the RT shader's BRDF evaluator.
    auto near_light = point(0.25f, 0, 0, 10, 1);
    auto bright_light = point(3, 0, 0, 10, 200);
    auto distant_light = point(4, 0, 0, 10, 1);
    auto out_of_range = point(11, 0, 0, 10, 100000);
    for (const LocalLight& light : {near_light, bright_light, distant_light, out_of_range}) {
        LocalLightContribution entry;
        entry.light_index = static_cast<uint32_t>(contributions.size());
        evaluate_local_light_irradiance(light, receiver, entry.contribution);
        contributions.push_back(entry);
    }
    LocalLightContribution exact;
    exact.light_index = 4;
    exact.contribution[0] = 0.01f;
    exact.casts_shadow = false;
    contributions.push_back(exact);
    std::vector<uint32_t> result;
    CHECK(select_local_light_contributions(contributions, 2, result, error), error.c_str());
    CHECK(result.size() == 3 && contains(result, 0) && contains(result, 1) && contains(result, 4),
          "nearest and highest contribution retained; unshadowed light outside budget");
    CHECK(!contains(result, 2) && !contains(result, 3), "weak and out-of-range lights not selected");
    CHECK(select_local_light_contributions(contributions, 0, result, error), error.c_str());
    CHECK(result == std::vector<uint32_t>({0, 1, 2, 4}), "zero budget preserves all contributing input order");
    contributions.clear();
    for (uint32_t id : {9u, 2u, 7u, 1u}) {
        LocalLightContribution entry;
        entry.light_index = id;
        entry.contribution[0] = entry.contribution[1] = entry.contribution[2] = 1;
        contributions.push_back(entry);
    }
    CHECK(select_local_light_contributions(contributions, 2, result, error), error.c_str());
    CHECK(result == std::vector<uint32_t>({1, 2}), "equal contributions tie by stable light ID");
    std::reverse(contributions.begin(), contributions.end());
    CHECK(select_local_light_contributions(contributions, 2, result, error), error.c_str());
    CHECK(result == std::vector<uint32_t>({1, 2}), "budget ranking independent of candidate traversal order");
    const auto before = result;
    CHECK(!select_local_light_contributions(contributions, 9, result, error) && result == before,
          "unsupported budget fails transactionally");
    contributions[0].contribution[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!select_local_light_contributions(contributions, 2, result, error) && result == before,
          "invalid contribution fails transactionally");
}

void test_effective_publication_cache() {
    using namespace world_lights;
    LocalLightPublication authored;
    authored.records = {point(1, 2, 3, 12), point(-8, 2, 0, 6)};
    std::string error;
    CHECK(rebuild_local_light_publication(authored, error), error.c_str());
    EffectiveLocalLightCache cache;
    const LocalLightPublication* output = nullptr;
    LocalLightIndexConfig config{authored.index.cell_size,
                                authored.index.max_cells_per_light};
    CHECK(cache.resolve(authored, 1.0f, config, output, error), error.c_str());
    CHECK(output == &authored && cache.rebuild_count() == 0,
          "identity request borrows the publication without copies or rebuilds");

    config.cell_size = 4.0f;
    CHECK(cache.resolve(authored, .75f, config, output, error), error.c_str());
    CHECK(cache.rebuild_count() == 1 && output->records[0].range == 9,
          "castle policy builds only the requested index once");
    LocalLightPublication legacy;
    CHECK(make_scaled_local_light_publication(authored, .75f, legacy, error), error.c_str());
    CHECK(rebuild_local_light_publication(legacy, config, error), error.c_str());
    CHECK(output->revision == legacy.revision &&
          output->index.light_indices == legacy.index.light_indices,
          "one-pass result retains legacy two-pass content and history revision");
    const auto* records = output->records.data();
    for (int frame = 0; frame < 100; ++frame)
        CHECK(cache.resolve(authored, .75f, config, output, error), error.c_str());
    CHECK(cache.rebuild_count() == 1 && output->records.data() == records,
          "unchanged frames reuse storage and do not reconstruct indices");

    authored.records[0].position[0] += 1;
    CHECK(rebuild_local_light_publication(authored, error), error.c_str());
    CHECK(cache.resolve(authored, .75f, config, output, error), error.c_str());
    CHECK(cache.rebuild_count() == 2, "source revision invalidates cached result");
    CHECK(cache.resolve(authored, .5f, config, output, error), error.c_str());
    CHECK(cache.rebuild_count() == 3 && output->records[0].range == 6,
          "scale changes derive from authored ranges, never compounded ranges");
    config.cell_size = 2;
    CHECK(cache.resolve(authored, .5f, config, output, error), error.c_str());
    config.max_cells_per_light = 1;
    CHECK(cache.resolve(authored, .5f, config, output, error), error.c_str());
    CHECK(cache.rebuild_count() == 5 && !output->index.oversized_light_indices.empty(),
          "both index configuration fields invalidate the cache");

    const auto revision = output->revision;
    const auto* previous = output;
    CHECK(!cache.resolve(authored, -1, config, output, error), "invalid scale rejected");
    auto invalid = authored;
    invalid.revision = 0; // Unpublished records must be validated rather than trusted.
    invalid.records[0].range = std::numeric_limits<float>::quiet_NaN();
    CHECK(!cache.resolve(invalid, .5f, config, output, error), "invalid records rejected");
    CHECK(output == previous && output->revision == revision && cache.rebuild_count() == 5,
          "validation failure preserves the last accepted publication and cache key");
    CHECK(cache.resolve(authored, .5f, config, output, error), error.c_str());
    CHECK(cache.rebuild_count() == 5, "failed request did not evict accepted cache");

    LocalLightPublication startup;
    CHECK(cache.resolve(startup, 1.0f, LocalLightIndexConfig{}, output, error), error.c_str());
    CHECK(output->records.empty() && output->revision != 0,
          "unpublished empty startup still produces valid renderer publication");
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
    test_scaled_publication();
    test_effective_publication_cache();
    test_importance_selection();
    return check_summary();
}
