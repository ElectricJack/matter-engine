#include "check.h"
#include "../src/hydrology/river_geometry.h"
#include "../src/terrain_river_overlay.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>

namespace {

constexpr float kEpsilon = 1.0e-4f;

bool near(float a, float b, float epsilon = kEpsilon) {
    return std::fabs(a - b) <= epsilon;
}

matter::RiverNetworkDefinition approved_network() {
    matter::RiverNetworkDefinition network{};
    network.cell_size_m = 0.5f;
    network.seed = 0x52495645u;
    matter::RiverDefinition river{};
    river.name = "main";
    river.inlet = {{0.0f, 18.0f, 0.0f}, 1.0f};
    river.curve = {{0.0f, 18.0f, 0.0f},
                   {30.0f, 12.0f, 8.0f},
                   {65.0f, 4.0f, -3.0f},
                   {110.0f, -2.0f, 5.0f}};
    river.channel_profile = {{0.0f, 7.0f, 2.5f, 0.35f},
                             {50.0f, 10.0f, 3.0f, -0.25f},
                             {100.0f, 8.0f, 2.0f, 0.10f}};
    network.rivers.push_back(river);
    return network;
}

void test_authored_curve_is_the_only_centreline_source() {
    const auto network = approved_network();
    hydrology::RiverGeometry geometry{};
    std::string error;
    CHECK(hydrology::build_river_geometry(network, "main", geometry, error),
          error.c_str());
    CHECK(!geometry.centreline.empty(), "authored curve produces centreline samples");
    if (geometry.centreline.empty()) return;

    const auto& first = geometry.centreline.front();
    const auto& last = geometry.centreline.back();
    CHECK(near(first.position_m.x, 0.0f) && near(first.position_m.y, 18.0f) &&
              near(first.position_m.z, 0.0f),
          "native resampling preserves the authored start point exactly");
    CHECK(near(last.position_m.x, 110.0f) && near(last.position_m.y, -2.0f) &&
              near(last.position_m.z, 5.0f),
          "native resampling preserves the authored end elevation exactly");

    for (const auto& sample : geometry.centreline) {
        CHECK(std::isfinite(sample.position_m.x) &&
                  std::isfinite(sample.position_m.y) &&
                  std::isfinite(sample.position_m.z) &&
                  std::isfinite(sample.tangent.x) &&
                  std::isfinite(sample.lateral.z),
              "every resampled centreline frame is finite");
    }
}

void test_channel_profile_interpolates_by_physical_distance() {
    const auto network = approved_network();
    hydrology::RiverGeometry geometry{};
    std::string error;
    CHECK(hydrology::build_river_geometry(network, "main", geometry, error),
          error.c_str());

    const auto at_distance = [&](float target) -> const hydrology::RiverCentrelineSample& {
        const hydrology::RiverCentrelineSample* nearest = &geometry.centreline.front();
        for (const auto& sample : geometry.centreline)
            if (std::fabs(sample.distance_m - target) <
                std::fabs(nearest->distance_m - target)) nearest = &sample;
        return *nearest;
    };
    const auto& midpoint = at_distance(25.0f);
    CHECK(near(midpoint.width_m, 8.5f, 0.03f) &&
              near(midpoint.depth_m, 2.75f, 0.02f) &&
              near(midpoint.asymmetry, 0.05f, 0.02f),
          "width depth and asymmetry interpolate from authored profile distances");
    CHECK(near(geometry.centreline.front().width_m, 7.0f) &&
              near(geometry.centreline.back().width_m, 8.0f),
          "profile values clamp to authored endpoints beyond their marker range");
}

void test_geometry_is_deterministic_and_keys_authored_changes() {
    auto network = approved_network();
    hydrology::RiverGeometry first{}, same{}, changed{};
    std::string error;
    CHECK(hydrology::build_river_geometry(network, "main", first, error),
          error.c_str());
    CHECK(hydrology::build_river_geometry(network, "main", same, error),
          error.c_str());
    CHECK(first.revision == same.revision &&
              first.centreline.size() == same.centreline.size(),
          "identical authored curves produce identical geometry revisions");

    network.seed += 1u;
    CHECK(hydrology::build_river_geometry(network, "main", changed, error),
          error.c_str());
    CHECK(changed.revision == first.revision,
          "native seed cannot add un-authored meander or change geometry");

    network.rivers[0].channel_profile[1].width_m += 0.5f;
    CHECK(hydrology::build_river_geometry(network, "main", changed, error),
          error.c_str());
    CHECK(changed.revision != first.revision,
          "an authored channel-profile edit changes the geometry revision");
}

void test_invalid_curves_and_profiles_fail_closed() {
    hydrology::RiverGeometry geometry{};
    std::string error;

    auto network = approved_network();
    network.rivers[0].curve[1].y = std::numeric_limits<float>::infinity();
    CHECK(!hydrology::build_river_geometry(network, "main", geometry, error) &&
              error.find("nonfinite") != std::string::npos,
          "nonfinite authored curve points are rejected");

    network = approved_network();
    network.rivers[0].channel_profile[1].distance_m = 0.0f;
    CHECK(!hydrology::build_river_geometry(network, "main", geometry, error) &&
              error.find("strictly increase") != std::string::npos,
          "duplicate profile markers are rejected");

    network = approved_network();
    network.rivers[0].curve = {{0.0f, 10.0f, 0.0f},
                               {10.0f, 9.0f, 10.0f},
                               {0.0f, 8.0f, 10.0f},
                               {10.0f, 7.0f, 0.0f}};
    CHECK(!hydrology::build_river_geometry(network, "main", geometry, error) &&
              error.find("self-intersection") != std::string::npos,
          "non-neighbour curve self-intersections are rejected");

    network = approved_network();
    network.rivers[0].curve = {{0.0f, 10.0f, 0.0f},
                               {0.25f, 9.9f, 0.0f},
                               {0.25f, 9.8f, 0.25f}};
    CHECK(!hydrology::build_river_geometry(network, "main", geometry, error) &&
              error.find("curvature") != std::string::npos,
          "curves tighter than the native validation limit are rejected");
}

void test_waterfall_spans_exempt_only_their_authored_transition() {
    auto sharp_network = [] {
        auto network = approved_network();
        network.rivers[0].curve = {{0.0f, 10.0f, 0.0f},
                                   {5.0f, 9.0f, 0.0f},
                                   {5.25f, 8.0f, 0.25f}};
        network.rivers[0].channel_profile = {{0.0f, 7.0f, 2.5f, 0.0f},
                                             {6.0f, 7.0f, 2.5f, 0.0f}};
        return network;
    };

    hydrology::RiverGeometry geometry{};
    std::string error;
    auto network = sharp_network();
    CHECK(!hydrology::build_river_geometry(network, "main", geometry, error) &&
              error.find("curvature") != std::string::npos,
          "a sharp unannotated transition remains invalid");

    matter::RiverSectionDefinition section{};
    section.id = "falls";
    section.river = "main";
    section.from_m = 0.0f;
    section.to_m = 6.0f;
    section.waterfalls.push_back({4.9f, 5.4f, 1.0f});
    network.sections.push_back(section);
    CHECK(hydrology::build_river_geometry(network, "main", geometry, error),
          "a sharp transition inside its declared waterfall span is accepted");

    network = sharp_network();
    section.waterfalls.clear();
    section.terminal_spillway = matter::RiverSpillwayDefinition{
        "pool-exit", 5.0f, 10.0f, 2.0f, 5.0f, 4.0f};
    network.sections.push_back(section);
    CHECK(hydrology::build_river_geometry(network, "main", geometry, error),
          "a sharp transition at a declared spillway is accepted");

    network = sharp_network();
    section.terminal_spillway.reset();
    section.waterfalls.clear();
    section.waterfalls.push_back({0.5f, 1.5f, 1.0f});
    network.sections.push_back(section);
    CHECK(!hydrology::build_river_geometry(network, "main", geometry, error) &&
              error.find("curvature") != std::string::npos,
          "a waterfall annotation cannot exempt a sharp corner elsewhere");
}

void test_height_overlay_uses_authored_thalweg_and_profile() {
    auto network = approved_network();
    network.rivers[0].curve = {{0.0f, 20.0f, 0.0f},
                               {40.0f, 8.0f, 0.0f}};
    network.rivers[0].channel_profile = {{0.0f, 8.0f, 3.0f, 0.5f},
                                         {42.0f, 12.0f, 4.0f, -0.5f}};
    hydrology::RiverGeometry geometry{};
    std::string error;
    CHECK(hydrology::build_river_geometry(network, "main", geometry, error),
          error.c_str());
    std::shared_ptr<const terrain_field::RiverHeightOverlay> overlay;
    CHECK(terrain_field::RiverHeightOverlay::build(geometry, overlay, error),
          error.c_str());
    if (!overlay) return;

    const float start_bed = overlay->height_at(0.0f, 0.0f, 80.0f);
    const float end_bed = overlay->height_at(40.0f, 0.0f, 80.0f);
    CHECK(near(start_bed, 20.0f, 0.05f) && near(end_bed, 8.0f, 0.05f),
          "the thalweg follows the authored curve elevation exactly");
    CHECK(std::fabs(overlay->height_at(10.0f, 5.0f, 80.0f) -
                    overlay->height_at(10.0f, -5.0f, 80.0f)) > 0.1f,
          "authored asymmetry shifts the rounded-V cross section");
}

} // namespace

int main() {
    test_authored_curve_is_the_only_centreline_source();
    test_channel_profile_interpolates_by_physical_distance();
    test_geometry_is_deterministic_and_keys_authored_changes();
    test_invalid_curves_and_profiles_fail_closed();
    test_waterfall_spans_exempt_only_their_authored_transition();
    test_height_overlay_uses_authored_thalweg_and_profile();
    return check_summary();
}
