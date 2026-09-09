#include "check.h"
#include "../src/hydrology/river_section_graph.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

matter::RiverSectionDefinition upper_section() {
    matter::RiverSectionDefinition section{};
    section.id = "upper";
    section.river = "main";
    section.from_m = 0.0f;
    section.to_m = 145.0f;
    section.dry_margin_m = 15.0f;
    section.emitter_ids = {"headwater"};
    section.waterfalls.push_back({105.0f, 117.0f, 12.0f});
    section.terminal_pool = matter::RiverPoolDefinition{117.0f, 145.0f, 24.0f};
    section.terminal_spillway = matter::RiverSpillwayDefinition{
        "pool-one", 145.0f, 10.0f, 2.0f, 5.0f, 4.0f};
    return section;
}

matter::RiverSectionDefinition lower_section() {
    matter::RiverSectionDefinition section{};
    section.id = "lower";
    section.river = "main";
    section.from_m = 145.0f;
    section.to_m = 275.0f;
    section.dry_margin_m = 15.0f;
    section.after_section_ids = {"upper"};
    section.upstream_spillway_section_ids = {"upper"};
    section.terminal_pool = matter::RiverPoolDefinition{255.0f, 275.0f, 3.0f};
    section.terminal_spillway = matter::RiverSpillwayDefinition{
        "pool-two", 275.0f, 10.0f, 2.0f, 5.0f, 4.0f};
    return section;
}

matter::RiverNetworkDefinition network() {
    matter::RiverNetworkDefinition result{};
    result.cell_size_m = 0.5f;
    result.rivers.push_back({"main", {{0.0f, 36.0f, 0.0f}, 1.0f},
                             {{0.0f, 36.0f, 0.0f},
                              {300.0f, 0.0f, 0.0f}},
                             {{0.0f, 10.0f, 5.0f, 0.0f}}});
    matter::HydrologyEmitter emitter{};
    emitter.id = "headwater";
    result.fluid.emitters.push_back(emitter);
    result.sections = {upper_section(), lower_section()};
    result.bake_sequential = true;
    return result;
}

hydrology::RiverGeometry geometry() {
    hydrology::RiverGeometry result{};
    const auto add = [&](float distance, float y) {
        result.centreline.push_back(
            {{distance, y, 0.0f}, {1.0f, 0.0f, 0.0f},
             {0.0f, 0.0f, 1.0f}, distance, 10.0f, 5.0f, 0.0f});
    };
    add(0.0f, 36.0f);
    add(105.0f, 30.0f);
    add(117.0f, 18.0f);
    add(145.0f, 18.0f);
    add(255.0f, 3.0f);
    add(275.0f, 3.0f);
    add(300.0f, 0.0f);
    return result;
}

bool build(const matter::RiverNetworkDefinition& definition,
           hydrology::RiverSectionGraph& graph, std::string& error) {
    return hydrology::build_river_section_graph(
        definition, {geometry()}, graph, error);
}

void test_stable_dependency_order() {
    auto definition = network();
    hydrology::RiverSectionGraph graph{};
    std::string error;
    CHECK(build(definition, graph, error), error.c_str());
    CHECK(graph.topological_order.size() == 2u &&
              definition.sections[graph.topological_order[0]].id == "upper" &&
              definition.sections[graph.topological_order[1]].id == "lower" &&
              graph.upstream[1].size() == 1u &&
              graph.upstream[1][0] == 0u,
          "section graph orders upper before its spillway-fed lower section");

    std::reverse(definition.sections.begin(), definition.sections.end());
    CHECK(build(definition, graph, error), error.c_str());
    CHECK(definition.sections[graph.topological_order[0]].id == "upper" &&
              definition.sections[graph.topological_order[1]].id == "lower",
          "topological order is stable when DSL declaration order is reversed");
}

void test_identity_and_dependency_failures() {
    hydrology::RiverSectionGraph graph{};
    std::string error;
    auto definition = network();
    definition.sections[1].id = "upper";
    CHECK(!build(definition, graph, error) && error.find("duplicate") != std::string::npos,
          "duplicate section ids are rejected");

    definition = network();
    definition.sections[1].after_section_ids = {"missing"};
    definition.sections[1].upstream_spillway_section_ids = {"missing"};
    CHECK(!build(definition, graph, error) && error.find("unknown") != std::string::npos,
          "unknown section dependencies are rejected");

    definition = network();
    definition.sections[1].upstream_spillway_section_ids = {"other"};
    CHECK(!build(definition, graph, error) &&
              error.find("disagree") != std::string::npos,
          "after and fromSpillway dependencies cannot disagree");

    definition = network();
    definition.sections[0].after_section_ids = {"lower"};
    definition.sections[0].upstream_spillway_section_ids = {"lower"};
    CHECK(!build(definition, graph, error) && error.find("cycle") != std::string::npos,
          "section dependency cycles are rejected");
}

void test_physical_feature_failures() {
    hydrology::RiverSectionGraph graph{};
    std::string error;
    auto definition = network();
    definition.sections[0].to_m = 305.0f;
    CHECK(!build(definition, graph, error) && error.find("range") != std::string::npos,
          "section intervals outside river geometry are rejected");

    definition = network();
    definition.sections[0].waterfalls[0].expected_drop_m = 8.0f;
    CHECK(!build(definition, graph, error) && error.find("drop") != std::string::npos,
          "authored waterfall drop must match completed curve elevation");

    definition = network();
    definition.sections[0].waterfalls[0].lip_distance_m = 150.0f;
    CHECK(!build(definition, graph, error) && error.find("range") != std::string::npos,
          "waterfall markers must lie within their section");

    definition = network();
    definition.sections[1].terminal_pool.reset();
    CHECK(!build(definition, graph, error) && error.find("pool") != std::string::npos,
          "every section requires a terminal pool");

    definition = network();
    definition.sections[1].terminal_spillway.reset();
    CHECK(!build(definition, graph, error) && error.find("spillway") != std::string::npos,
          "every section requires a terminal spillway");
}

}  // namespace

int main() {
    test_stable_dependency_order();
    test_identity_and_dependency_failures();
    test_physical_feature_failures();
    return check_summary();
}
