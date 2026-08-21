#include "check.h"
#include "../src/hydrology/river_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

    bool seed_changes_offset = first.revision != changed_seed.revision;
    const std::size_t common = std::min(first.centreline.size(),
                                        changed_seed.centreline.size());
    for (std::size_t i = 0; !seed_changes_offset && i < common; ++i)
        seed_changes_offset =
            first.centreline[i].position_m.x != changed_seed.centreline[i].position_m.x ||
            first.centreline[i].position_m.z != changed_seed.centreline[i].position_m.z;
    CHECK(seed_changes_offset,
          "changing the network seed changes deterministic lateral offsets");

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

} // namespace

int main() {
    test_arc_length_reaches_and_hard_controls();
    test_inverse_grade_response_is_deterministic_and_curvature_bounded();
    test_self_intersection_is_rejected();
    test_revision_changes_when_geometry_bounds_change();
    test_boulders_are_deterministic_bounded_and_reserve_cross_sections();
    return check_summary();
}
