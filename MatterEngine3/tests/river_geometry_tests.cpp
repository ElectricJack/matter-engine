#include "check.h"
#include "../src/hydrology/river_geometry.h"
#include "../src/terrain_river_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace {

matter::RiverNetworkDefinition approved_network(std::uint64_t seed = 0x52495645u) {
    matter::RiverNetworkDefinition network{};
    network.cell_size_m = 0.5f;
    network.seed = seed;
    network.first_section_river = "main";
    network.first_section = {100.0f, 4.0f, 0.80f, 32u, 256u, 65536u};

    matter::RiverDefinition river{};
    river.name = "main";
    river.inlet = {{0.0f, 18.0f, 0.0f}, 1.0f};
    river.spline = {{0.0f, 18.0f, 0.0f},
                    {34.0f, 14.0f, 11.0f},
                    {72.0f, 9.0f, -9.0f},
                    {128.0f, 3.0f, 5.0f}};
    river.reaches = {{64.0f, -0.035f, 0.15f},
                     {128.0f, -0.012f, 0.65f}};
    river.channel = {7.0f, 2.5f, 0.35f};
    river.boulders = {0.08f, {0.5f, 2.0f}};
    network.rivers.push_back(river);
    return network;
}

matter::RiverNetworkDefinition straight_response_network() {
    matter::RiverNetworkDefinition network = approved_network(0x9f41d13bu);
    network.rivers[0].spline = {{0.0f, 18.0f, 0.0f},
                               {32.0f, 16.0f, 0.0f},
                               {64.0f, 14.0f, 0.0f},
                               {96.0f, 12.0f, 0.0f},
                               {128.0f, 10.0f, 0.0f}};
    return network;
}

float planar_distance(matter::Float3 a, matter::Float3 b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

float spatial_distance(matter::Float3 a, matter::Float3 b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) +
                     (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}

float lateral_range(const hydrology::RiverGeometry& geometry,
                    float begin_m, float end_m) {
    float minimum = INFINITY;
    float maximum = -INFINITY;
    for (const auto& sample : geometry.centreline) {
        if (sample.distance_m < begin_m || sample.distance_m > end_m) continue;
        minimum = std::min(minimum, sample.position_m.z);
        maximum = std::max(maximum, sample.position_m.z);
    }
    return maximum - minimum;
}

float planar_orientation(matter::Float3 a, matter::Float3 b,
                         matter::Float3 c) {
    return (b.x - a.x) * (c.z - a.z) -
           (b.z - a.z) * (c.x - a.x);
}

bool proper_planar_crossing(matter::Float3 a, matter::Float3 b,
                            matter::Float3 c, matter::Float3 d) {
    const float ab_c = planar_orientation(a, b, c);
    const float ab_d = planar_orientation(a, b, d);
    const float cd_a = planar_orientation(c, d, a);
    const float cd_b = planar_orientation(c, d, b);
    return ((ab_c > 0.0f && ab_d < 0.0f) ||
            (ab_c < 0.0f && ab_d > 0.0f)) &&
           ((cd_a > 0.0f && cd_b < 0.0f) ||
            (cd_a < 0.0f && cd_b > 0.0f));
}

bool centreline_crosses(const hydrology::RiverGeometry& geometry) {
    const auto& samples = geometry.centreline;
    for (std::size_t first = 0; first + 1 < samples.size(); ++first) {
        for (std::size_t second = first + 2;
             second + 1 < samples.size(); ++second) {
            if (proper_planar_crossing(samples[first].position_m,
                                       samples[first + 1].position_m,
                                       samples[second].position_m,
                                       samples[second + 1].position_m))
                return true;
        }
    }
    return false;
}

std::shared_ptr<const terrain_field::RiverHeightOverlay> build_overlay(
    const matter::RiverNetworkDefinition& network,
    hydrology::RiverGeometry& geometry) {
    std::string error;
    CHECK(hydrology::build_river_geometry(network, geometry, error), error.c_str());
    std::shared_ptr<const terrain_field::RiverHeightOverlay> overlay;
    CHECK(terrain_field::RiverHeightOverlay::build(
              geometry, network.rivers[0].channel, overlay, error),
          error.c_str());
    return overlay;
}

void test_height_overlay_grade_ravine_boulders_and_hash() {
    matter::RiverNetworkDefinition network = straight_response_network();
    network.rivers[0].boulders.density = 0.0f;
    hydrology::RiverGeometry geometry{};
    const auto overlay = build_overlay(network, geometry);
    CHECK(overlay != nullptr, "approved river builds an immutable height overlay");
    if (!overlay || geometry.centreline.empty()) return;

    bool monotonic = true;
    float previous = overlay->height_at(
        geometry.centreline.front().position_m.x,
        geometry.centreline.front().position_m.z, 80.0f);
    const float first_height = previous;
    for (std::size_t i = 1; i < geometry.centreline.size(); ++i) {
        const auto& sample = geometry.centreline[i];
        const float height = overlay->height_at(
            sample.position_m.x, sample.position_m.z, 80.0f);
        monotonic = monotonic &&
                    height <= std::nextafter(previous,
                                             std::numeric_limits<float>::infinity());
        previous = height;
    }
    CHECK(monotonic,
          "smoothed thalweg never rises downstream by more than one float ULP");
    CHECK(first_height - previous > 2.0f,
          "compact smoothing preserves a measurable downstream grade");
    const auto& broad_upstream =
        geometry.centreline[geometry.centreline.size() / 4u];
    const auto& broad_downstream =
        geometry.centreline[geometry.centreline.size() * 3u / 4u];
    const float broad_offset = network.rivers[0].channel.width_m * 0.9f;
    const float upstream_terrain = overlay->height_at(
        broad_upstream.position_m.x + broad_upstream.lateral.x * broad_offset,
        broad_upstream.position_m.z + broad_upstream.lateral.z * broad_offset,
        80.0f);
    const float downstream_terrain = overlay->height_at(
        broad_downstream.position_m.x +
            broad_downstream.lateral.x * broad_offset,
        broad_downstream.position_m.z +
            broad_downstream.lateral.z * broad_offset,
        80.0f);
    CHECK(upstream_terrain - downstream_terrain > 0.5f,
          "broad terrain around the ravine follows the downstream grade");

    const auto& middle = geometry.centreline[geometry.centreline.size() / 2u];
    const float bank_offset = network.rivers[0].channel.width_m * 0.45f;
    const float left = overlay->height_at(
        middle.position_m.x + middle.lateral.x * bank_offset,
        middle.position_m.z + middle.lateral.z * bank_offset, 80.0f);
    const float right = overlay->height_at(
        middle.position_m.x - middle.lateral.x * bank_offset,
        middle.position_m.z - middle.lateral.z * bank_offset, 80.0f);
    CHECK(std::fabs(left - right) > 0.1f,
          "authored asymmetry produces different bank heights");
    const float bed = overlay->height_at(middle.position_m.x,
                                         middle.position_m.z, 80.0f);
    const float ray_height = bed + network.rivers[0].channel.depth_m * 0.75f;
    CHECK(left < ray_height || right < ray_height,
          "a lateral ray from the thalweg reaches sky over at least one bank");

    matter::RiverNetworkDefinition rounded_network = network;
    rounded_network.rivers[0].channel.asymmetry = 0.0f;
    hydrology::RiverGeometry rounded_geometry{};
    const auto rounded_overlay = build_overlay(rounded_network, rounded_geometry);
    const auto& rounded_middle =
        rounded_geometry.centreline[rounded_geometry.centreline.size() / 2u];
    const float rounded_half_width =
        rounded_network.rivers[0].channel.width_m * 0.5f;
    const auto rounded_height = [&](float fraction) {
        return rounded_overlay->height_at(
            rounded_middle.position_m.x + rounded_middle.lateral.x *
                rounded_half_width * fraction,
            rounded_middle.position_m.z + rounded_middle.lateral.z *
                rounded_half_width * fraction,
            80.0f);
    };
    const float rounded_bed = rounded_height(0.0f);
    const float rounded_bank_rise = rounded_height(1.0f) - rounded_bed;
    const float quarter_rise = rounded_height(0.25f) - rounded_bed;
    const float three_quarter_rise = rounded_height(0.75f) - rounded_bed;
    CHECK(quarter_rise > rounded_bank_rise * 0.17f,
          "rounded-V carve leaves the rounded thalweg on a visibly rising side");
    CHECK(three_quarter_rise < rounded_bank_rise * 0.80f,
          "rounded-V carve keeps an open near-linear wall below the shoulder");

    matter::RiverNetworkDefinition boulder_network = network;
    boulder_network.rivers[0].boulders = {1.0f, {0.7f, 1.1f}};
    hydrology::RiverGeometry boulder_geometry{};
    const auto boulder_overlay = build_overlay(boulder_network, boulder_geometry);
    bool all_solid = !boulder_geometry.boulders.empty();
    for (const auto& boulder : boulder_geometry.boulders) {
        const float projected_bed = overlay->height_at(
            boulder.center_m.x, boulder.center_m.z, 80.0f);
        const float projected_center_y = projected_bed + boulder.radius_m;
        all_solid = all_solid &&
            boulder_overlay->height_at(
                boulder.center_m.x, boulder.center_m.z, 80.0f) >
                projected_center_y;
    }
    CHECK(all_solid,
          "Task 2 boulder XZ/radius selections project onto the carved bed as solid");

    hydrology::RiverGeometry changed_spline{};
    matter::RiverNetworkDefinition spline_network = network;
    spline_network.rivers[0].spline[1].z += 2.0f;
    const auto spline_overlay = build_overlay(spline_network, changed_spline);
    hydrology::RiverGeometry changed_grade{};
    matter::RiverNetworkDefinition grade_network = network;
    grade_network.rivers[0].reaches[0].base_grade -= 0.005f;
    const auto grade_overlay = build_overlay(grade_network, changed_grade);
    hydrology::RiverGeometry changed_seed{};
    matter::RiverNetworkDefinition seed_network = network;
    ++seed_network.seed;
    const auto seed_overlay = build_overlay(seed_network, changed_seed);
    CHECK(overlay->hash() != spline_overlay->hash() &&
          overlay->hash() != grade_overlay->hash() &&
          overlay->hash() != seed_overlay->hash(),
          "overlay hash changes with spline, grade, and seed-derived geometry");
}

void test_arc_length_reaches_and_hard_controls() {
    const matter::RiverNetworkDefinition network = approved_network();
    hydrology::RiverGeometry geometry{};
    std::string error;
    CHECK(hydrology::build_river_geometry(network, geometry, error),
          error.c_str());
    if (geometry.centreline.empty()) return;

    CHECK(geometry.centreline.front().distance_m == 0.0f,
          "centreline arc length begins at zero");
    bool monotonic = true;
    bool half_cell_spacing = true;
    for (std::size_t i = 1; i < geometry.centreline.size(); ++i) {
        const float step = geometry.centreline[i].distance_m -
                           geometry.centreline[i - 1].distance_m;
        monotonic = monotonic && step > 0.0f;
        if (i + 1 < geometry.centreline.size())
            half_cell_spacing = half_cell_spacing &&
                                std::fabs(step - 0.25f) < 1.0e-4f;
    }
    CHECK(monotonic, "centreline sample distances are strictly increasing");
    CHECK(half_cell_spacing,
          "centreline is arc-length reparameterized at half-cell spacing");
    CHECK(geometry.centreline.back().distance_m >= 128.0f,
          "approved centreline retains at least 128 metres of length");

    const auto boundary = std::find_if(
        geometry.centreline.begin(), geometry.centreline.end(),
        [](const auto& sample) {
            return std::fabs(sample.distance_m - 64.0f) < 1.0e-5f;
        });
    CHECK(boundary != geometry.centreline.end(),
          "half-cell sampling includes the exact 64 metre reach boundary");
    if (boundary != geometry.centreline.end()) {
        CHECK(boundary->grade == -0.035f && boundary->meander == 0.15f,
              "the exact reach boundary belongs to the upstream reach");
        if (boundary + 1 != geometry.centreline.end())
            CHECK((boundary + 1)->grade == -0.012f &&
                      (boundary + 1)->meander == 0.65f,
                  "the first sample after a reach boundary uses the next reach");
    }

    CHECK(std::all_of(geometry.centreline.begin(), geometry.centreline.end(),
                      [](const auto& sample) { return sample.grade < 0.0f; }),
          "every centreline sample retains a negative downstream grade");

    for (const matter::Float3 control : network.rivers[0].spline) {
        float nearest = INFINITY;
        for (const auto& sample : geometry.centreline)
            nearest = std::min(nearest, spatial_distance(sample.position_m, control));
        CHECK(nearest <= 0.14f,
              "bounded meander preserves every authored hard spline control");
    }
}

void test_inverse_grade_response_is_deterministic_and_curvature_bounded() {
    const matter::RiverNetworkDefinition network = straight_response_network();
    hydrology::RiverGeometry first{};
    hydrology::RiverGeometry same{};
    hydrology::RiverGeometry changed_seed{};
    matter::RiverNetworkDefinition alternate_seed = network;
    alternate_seed.seed += 1u;
    std::string error;
    CHECK(hydrology::build_river_geometry(network, first, error), error.c_str());
    error.clear();
    CHECK(hydrology::build_river_geometry(network, same, error), error.c_str());
    error.clear();
    CHECK(hydrology::build_river_geometry(alternate_seed, changed_seed, error),
          error.c_str());
    if (first.centreline.empty() || same.centreline.empty() ||
        changed_seed.centreline.empty()) return;

    const float steep_lateral_range = lateral_range(first, 8.0f, 56.0f);
    const float gentle_lateral_range = lateral_range(first, 72.0f, 120.0f);
    CHECK(gentle_lateral_range > steep_lateral_range * 1.5f,
          "gentle reach produces substantially stronger lateral meander");

    bool identical = first.centreline.size() == same.centreline.size();
    for (std::size_t i = 0; identical && i < first.centreline.size(); ++i) {
        const auto& a = first.centreline[i];
        const auto& b = same.centreline[i];
        identical = a.position_m.x == b.position_m.x &&
                    a.position_m.y == b.position_m.y &&
                    a.position_m.z == b.position_m.z;
    }
    CHECK(identical && first.revision == same.revision,
          "same seed produces byte-identical sampled offsets and revision");

    bool seed_changes_offset = first.centreline.size() !=
                               changed_seed.centreline.size();
    const std::size_t common = std::min(first.centreline.size(),
                                        changed_seed.centreline.size());
    for (std::size_t i = 0; i < common; ++i)
        seed_changes_offset =
            seed_changes_offset ||
            first.centreline[i].position_m.x != changed_seed.centreline[i].position_m.x ||
            first.centreline[i].position_m.z != changed_seed.centreline[i].position_m.z;
    CHECK(seed_changes_offset,
          "changing the network seed changes deterministic lateral offsets");
    CHECK(first.revision != changed_seed.revision,
          "changing the network seed changes geometry revision identity");

    float maximum_curvature = 0.0f;
    for (std::size_t i = 1; i + 1 < first.centreline.size(); ++i) {
        const auto& before = first.centreline[i - 1].tangent;
        const auto& after = first.centreline[i + 1].tangent;
        const float before_length = std::hypot(before.x, before.z);
        const float after_length = std::hypot(after.x, after.z);
        if (before_length <= 0.0f || after_length <= 0.0f) continue;
        const float cosine = std::clamp(
            (before.x * after.x + before.z * after.z) /
                (before_length * after_length),
            -1.0f, 1.0f);
        const float span = first.centreline[i + 1].distance_m -
                           first.centreline[i - 1].distance_m;
        maximum_curvature = std::max(maximum_curvature,
                                     std::acos(cosine) / span);
    }
    CHECK(maximum_curvature <= 0.20f,
          "seeded meander remains below the approved curvature bound");
}

void test_self_intersection_is_rejected() {
    matter::RiverNetworkDefinition network = approved_network();
    network.rivers[0].spline = {{0.0f, 8.0f, 0.0f},
                               {20.0f, 7.0f, 20.0f},
                               {0.0f, 6.0f, 20.0f},
                               {20.0f, 5.0f, 0.0f}};
    network.rivers[0].reaches = {{100.0f, -0.02f, 0.2f}};
    hydrology::RiverGeometry geometry{};
    std::string error;
    CHECK(!hydrology::build_river_geometry(network, geometry, error) &&
              error.find("self-intersection") != std::string::npos,
          "non-neighbour spline segment intersections reject geometry");
}

void test_sub_epsilon_distinct_spline_fails_closed_or_keeps_two_points() {
    matter::RiverNetworkDefinition network = approved_network();
    network.rivers[0].spline = {{0.0f, 18.0f, 0.0f},
                               {0.00005f, 18.0f, 0.0f}};
    network.rivers[0].reaches = {{1.0f, -0.01f, 0.0f}};
    network.rivers[0].boulders.density = 0.0f;

    hydrology::RiverGeometry geometry{};
    geometry.centreline.push_back({{91.0f, 92.0f, 93.0f}});
    geometry.revision = 0xfeedu;
    std::string error;
    const bool built = hydrology::build_river_geometry(network, geometry, error);

    const bool valid_two_point_result =
        built && geometry.centreline.size() >= 2u &&
        geometry.centreline.back().distance_m > 0.0f;
    const bool named_unchanged_rejection =
        !built && error.find("spline is too short") != std::string::npos &&
        geometry.centreline.size() == 1u &&
        geometry.centreline[0].position_m.x == 91.0f &&
        geometry.revision == 0xfeedu;
    CHECK(valid_two_point_result || named_unchanged_rejection,
          "a distinct sub-epsilon spline returns two points or fails closed without changing output");
}

void test_revision_changes_when_geometry_bounds_change() {
    matter::RiverNetworkDefinition network = approved_network();
    hydrology::RiverGeometry shallow{};
    hydrology::RiverGeometry deep{};
    std::string error;
    CHECK(hydrology::build_river_geometry(network, shallow, error), error.c_str());
    network.rivers[0].channel.depth_m += 1.0f;
    error.clear();
    CHECK(hydrology::build_river_geometry(network, deep, error), error.c_str());
    CHECK(shallow.bounds_m.minimum.y != deep.bounds_m.minimum.y &&
              shallow.revision != deep.revision,
          "geometry revision changes when the channel bounds change");
}

void test_boulders_are_deterministic_bounded_and_reserve_cross_sections() {
    matter::RiverNetworkDefinition network = approved_network();
    hydrology::RiverGeometry first{};
    hydrology::RiverGeometry same{};
    std::string error;
    CHECK(hydrology::build_river_geometry(network, first, error), error.c_str());
    error.clear();
    CHECK(hydrology::build_river_geometry(network, same, error), error.c_str());
    CHECK(!first.boulders.empty(),
          "approved nonzero density selects deterministic boulders");
    CHECK(first.boulders.size() == same.boulders.size(),
          "boulder density selects a stable count");

    bool identical = first.boulders.size() == same.boulders.size();
    bool bounded = true;
    bool within_channel = true;
    bool reserves = true;
    for (std::size_t i = 0; i < first.boulders.size(); ++i) {
        const auto& boulder = first.boulders[i];
        if (identical) {
            const auto& other = same.boulders[i];
            identical = boulder.center_m.x == other.center_m.x &&
                        boulder.center_m.y == other.center_m.y &&
                        boulder.center_m.z == other.center_m.z &&
                        boulder.radius_m == other.radius_m;
        }
        bounded = bounded && boulder.radius_m >= 0.5f && boulder.radius_m <= 2.0f;

        float nearest_planar = INFINITY;
        float nearest_distance = 0.0f;
        for (const auto& sample : first.centreline) {
            const float candidate = planar_distance(boulder.center_m,
                                                    sample.position_m);
            if (candidate < nearest_planar) {
                nearest_planar = candidate;
                nearest_distance = sample.distance_m;
            }
        }
        within_channel = within_channel &&
                         nearest_planar + boulder.radius_m <=
                             network.rivers[0].channel.width_m * 0.5f + 0.26f;
        reserves = reserves && nearest_distance >= 7.0f &&
                   std::fabs(nearest_distance -
                             network.first_section.minimum_length_m) >= 3.5f &&
                   first.centreline.back().distance_m - nearest_distance >= 3.5f;
    }
    CHECK(identical, "boulder centres and radii are seed deterministic");
    CHECK(bounded, "all selected boulder radii remain in the authored interval");
    CHECK(within_channel,
          "complete boulder footprints remain inside the channel envelope");
    CHECK(reserves,
          "boulders avoid the inlet and planned cross-section reserve bands");

    matter::RiverNetworkDefinition empty = network;
    empty.rivers[0].boulders.density = 0.0f;
    hydrology::RiverGeometry no_boulders{};
    error.clear();
    CHECK(hydrology::build_river_geometry(empty, no_boulders, error) &&
              no_boulders.boulders.empty(),
          "zero authored density deterministically selects no boulders");

    matter::RiverNetworkDefinition dense = network;
    dense.rivers[0].boulders.density = 1.0f;
    hydrology::RiverGeometry all_candidates{};
    error.clear();
    CHECK(hydrology::build_river_geometry(dense, all_candidates, error) &&
              all_candidates.boulders.size() > first.boulders.size(),
          "maximum authored density selects more deterministic candidates");
}

void test_generated_intersection_is_deterministically_attenuated() {
    matter::RiverNetworkDefinition network = approved_network();
    network.cell_size_m = 32.0f;
    network.seed = 34u;
    network.rivers[0].spline = {
        {20.0f, 10.0f, 0.0f}, {14.0f, 9.0f, 14.0f},
        {0.0f, 8.0f, 20.0f}, {-14.0f, 7.0f, 14.0f},
        {-20.0f, 6.0f, 0.0f}, {-14.0f, 5.0f, -14.0f},
        {0.0f, 4.0f, -20.0f}, {14.0f, 3.0f, -14.0f},
        {19.9f, 2.0f, -2.0f}};
    network.rivers[0].reaches = {{200.0f, -0.005f, 1.0f}};
    network.rivers[0].channel.width_m = 20.0f;
    network.rivers[0].boulders.density = 0.0f;

    matter::RiverNetworkDefinition base_network = network;
    base_network.rivers[0].reaches[0].meander = 0.0f;
    matter::RiverNetworkDefinition half_scale_network = network;
    half_scale_network.rivers[0].reaches[0].meander = 0.5f;

    hydrology::RiverGeometry base{};
    hydrology::RiverGeometry generated{};
    hydrology::RiverGeometry half_scale{};
    std::string error;
    CHECK(hydrology::build_river_geometry(base_network, base, error),
          error.c_str());
    error.clear();
    CHECK(hydrology::build_river_geometry(network, generated, error),
          error.c_str());
    error.clear();
    CHECK(hydrology::build_river_geometry(half_scale_network, half_scale, error),
          error.c_str());
    if (base.centreline.empty() || generated.centreline.empty() ||
        half_scale.centreline.empty()) return;

    CHECK(!centreline_crosses(base),
          "the authored large-radius base fixture does not self-intersect");
    CHECK(!centreline_crosses(generated),
          "intersection attenuation publishes only a non-crossing centreline");

    bool matches_half_scale =
        generated.centreline.size() == half_scale.centreline.size();
    const std::size_t common = std::min(generated.centreline.size(),
                                        half_scale.centreline.size());
    for (std::size_t i = 0; i < common; ++i)
        matches_half_scale = matches_half_scale &&
            spatial_distance(generated.centreline[i].position_m,
                             half_scale.centreline[i].position_m) < 1.0e-5f;
    CHECK(matches_half_scale,
          "known full-scale crossing retries at the deterministic half scale");

    float maximum_curvature = 0.0f;
    for (std::size_t i = 1; i + 1 < generated.centreline.size(); ++i) {
        const auto& before = generated.centreline[i - 1].tangent;
        const auto& after = generated.centreline[i + 1].tangent;
        const float before_length = std::hypot(before.x, before.z);
        const float after_length = std::hypot(after.x, after.z);
        if (before_length <= 0.0f || after_length <= 0.0f) continue;
        const float cosine = std::clamp(
            (before.x * after.x + before.z * after.z) /
                (before_length * after_length),
            -1.0f, 1.0f);
        const float span = generated.centreline[i + 1].distance_m -
                           generated.centreline[i - 1].distance_m;
        maximum_curvature = std::max(maximum_curvature,
                                     std::acos(cosine) / span);
    }
    CHECK(maximum_curvature <= 0.20f,
          "intersection-attenuated centreline still satisfies curvature bounds");
}

void test_generated_intersection_exhaustion_rejects_unchanged() {
    matter::RiverNetworkDefinition network = approved_network();
    network.cell_size_m = 32.0f;
    network.seed = 34u;
    network.rivers[0].spline = {
        {20.0f, 10.0f, 0.0f}, {14.0f, 9.0f, 14.0f},
        {0.0f, 8.0f, 20.0f}, {-14.0f, 7.0f, 14.0f},
        {-20.0f, 6.0f, 0.0f}, {-14.0f, 5.0f, -14.0f},
        {0.0f, 4.0f, -20.0f}, {14.0f, 3.0f, -14.0f},
        {19.9f, 2.0f, -2.0f}};
    network.rivers[0].reaches = {{200.0f, -0.005f, 1.0f}};
    network.rivers[0].channel.width_m = 6400.0f;
    network.rivers[0].boulders = {0.0f, {0.5f, 2.0f}};

    matter::RiverNetworkDefinition base_network = network;
    base_network.rivers[0].reaches[0].meander = 0.0f;
    hydrology::RiverGeometry base{};
    hydrology::RiverGeometry rejected{};
    rejected.centreline.push_back({{71.0f, 72.0f, 73.0f}});
    rejected.revision = 0xbeefu;
    std::string error;
    CHECK(hydrology::build_river_geometry(base_network, base, error),
          error.c_str());
    error.clear();
    const bool built = hydrology::build_river_geometry(network, rejected, error);
    CHECK(!base.centreline.empty() && !centreline_crosses(base),
          "exhaustion fixture starts from a valid non-crossing base spline");
    CHECK(!built &&
              error.find("generated meander self-intersection after attenuation") !=
                  std::string::npos &&
              rejected.centreline.size() == 1u &&
              rejected.centreline[0].position_m.x == 71.0f &&
              rejected.revision == 0xbeefu,
          "intersection at every allowed scale rejects without changing output");
}

} // namespace

int main() {
    test_arc_length_reaches_and_hard_controls();
    test_inverse_grade_response_is_deterministic_and_curvature_bounded();
    test_self_intersection_is_rejected();
    test_sub_epsilon_distinct_spline_fails_closed_or_keeps_two_points();
    test_revision_changes_when_geometry_bounds_change();
    test_boulders_are_deterministic_bounded_and_reserve_cross_sections();
    test_height_overlay_grade_ravine_boulders_and_hash();
    test_generated_intersection_is_deterministically_attenuated();
    test_generated_intersection_exhaustion_rejects_unchanged();
    return check_summary();
}
