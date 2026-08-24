#include "check.h"

#include "hydrology/river_section_coordinator.h"

#include <string>
#include <vector>

namespace {

matter::RiverNetworkDefinition network() {
    matter::RiverNetworkDefinition result{};
    result.canonical_hash = 101u;
    matter::RiverSectionDefinition upper{};
    upper.id = "upper";
    upper.river = "main";
    upper.from_m = 0.0f;
    upper.to_m = 100.0f;
    upper.terminal_spillway = matter::RiverSpillwayDefinition{
        "pool-one", 100.0f, 10.0f, 2.0f, 5.0f, 4.0f};
    matter::RiverSectionDefinition lower{};
    lower.id = "lower";
    lower.river = "main";
    lower.from_m = 100.0f;
    lower.to_m = 220.0f;
    lower.after_section_ids = {"upper"};
    lower.upstream_spillway_section_ids = {"upper"};
    lower.terminal_spillway = matter::RiverSpillwayDefinition{
        "pool-two", 220.0f, 10.0f, 2.0f, 5.0f, 4.0f};
    result.sections = {upper, lower};
    result.bake_sequential = true;
    return result;
}

hydrology::RiverSectionGraph graph() {
    return {{0u, 1u}, {{}, {0u}}};
}

void test_sections_run_serially_with_accepted_handoff() {
    const auto definition = network();
    const auto dependency_graph = graph();
    std::vector<std::string> calls;
    bool executor_active = false;
    std::vector<float> progress;
    hydrology::SectionBakeExecutor executor =
        [&](const matter::RiverSectionDefinition& section,
            const std::vector<hydrology::SpillwayHandoffRecord>& upstream,
            hydrology::SectionBakeResult& result,
            hydrology::FluidBakeError& error) {
            CHECK(!executor_active, "the serial coordinator never re-enters its executor");
            executor_active = true;
            calls.push_back(section.id);
            if (section.id == "lower") {
                CHECK(upstream.size() == 1u &&
                          upstream[0].upstream_section_id == "upper",
                      "lower receives the accepted upper handoff");
            } else {
                CHECK(upstream.empty(), "the root section has no inherited handoff");
            }
            result = {};
            result.artifact.accepted = true;
            result.artifact.section.section_id = section.id;
            result.artifact.section.river_id = section.river;
            result.artifact.semantic_key = section.id == "upper" ? 11u : 22u;
            result.artifact.payload_digest = section.id == "upper" ? 111u : 222u;
            result.cache_hit = section.id == "upper";
            if (section.id == "upper") {
                hydrology::SpillwayHandoffRecord record{};
                record.id = "pool-one";
                record.upstream_section_id = "upper";
                record.downstream_section_id = "lower";
                record.semantic_key = 17u;
                result.downstream_handoff = record;
            }
            executor_active = false;
            error = {};
            return true;
        };
    hydrology::RiverSectionSequenceCallbacks callbacks{};
    callbacks.progress = [&](float fraction, const std::string&) {
        progress.push_back(fraction);
    };
    hydrology::RiverSectionSequenceResult output{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::run_river_section_sequence(
              definition, dependency_graph, executor, callbacks, output, error),
          error.message.c_str());
    CHECK(calls == std::vector<std::string>({"upper", "lower"}),
          "sections run serially in stable dependency order");
    CHECK(output.sections.size() == 2u && output.sections[0].cache_hit &&
              !output.sections[1].cache_hit,
          "an upper cache hit and lower cache miss remain independently reusable");
    CHECK(progress == std::vector<float>({0.0f, 0.5f, 1.0f}),
          "aggregate section progress is monotonic and reaches one");
    CHECK(output.manifest.state ==
              hydrology::HydrologyNetworkState::Incomplete,
          "section completion alone does not claim Ready before handoff products exist");
}

void test_lower_failure_preserves_upper_and_stops() {
    std::vector<std::string> calls;
    hydrology::SectionBakeExecutor executor =
        [&](const matter::RiverSectionDefinition& section,
            const std::vector<hydrology::SpillwayHandoffRecord>&,
            hydrology::SectionBakeResult& result,
            hydrology::FluidBakeError& error) {
            calls.push_back(section.id);
            if (section.id == "lower") {
                error = {hydrology::FluidBakeCode::BackendFailure,
                         "lower failed"};
                return false;
            }
            result.artifact.accepted = true;
            result.artifact.section.section_id = section.id;
            hydrology::SpillwayHandoffRecord handoff{};
            handoff.id = "pool-one";
            handoff.upstream_section_id = "upper";
            handoff.downstream_section_id = "lower";
            handoff.semantic_key = 17u;
            result.downstream_handoff = handoff;
            return true;
        };
    hydrology::RiverSectionSequenceResult output{};
    hydrology::FluidBakeError error{};
    CHECK(!hydrology::run_river_section_sequence(
              network(), graph(), executor, {}, output, error) &&
              error.message == "lower failed",
          "the first section failure stops the sequence with its diagnostic");
    CHECK(calls == std::vector<std::string>({"upper", "lower"}) &&
              output.sections.size() == 1u &&
              output.sections[0].artifact.section.section_id == "upper" &&
              output.manifest.state == hydrology::HydrologyNetworkState::Failed,
          "lower failure preserves the accepted upper result and failed state");
}

void test_cancellation_is_observed_between_sections() {
    std::vector<std::string> calls;
    bool cancel = false;
    hydrology::SectionBakeExecutor executor =
        [&](const matter::RiverSectionDefinition& section,
            const std::vector<hydrology::SpillwayHandoffRecord>&,
            hydrology::SectionBakeResult& result,
            hydrology::FluidBakeError&) {
            calls.push_back(section.id);
            result.artifact.accepted = true;
            result.artifact.section.section_id = section.id;
            hydrology::SpillwayHandoffRecord handoff{};
            handoff.id = "pool-one";
            handoff.upstream_section_id = "upper";
            handoff.downstream_section_id = "lower";
            handoff.semantic_key = 17u;
            result.downstream_handoff = handoff;
            cancel = true;
            return true;
        };
    hydrology::RiverSectionSequenceCallbacks callbacks{};
    callbacks.cancelled = [&] { return cancel; };
    hydrology::RiverSectionSequenceResult output{};
    hydrology::FluidBakeError error{};
    CHECK(!hydrology::run_river_section_sequence(
              network(), graph(), executor, callbacks, output, error) &&
              error.code == hydrology::FluidBakeCode::Cancelled,
          "cancellation between sections stops before downstream creation");
    CHECK(calls == std::vector<std::string>({"upper"}) &&
              output.sections.size() == 1u &&
              output.manifest.state ==
                  hydrology::HydrologyNetworkState::Incomplete,
          "cancellation retains accepted upstream work without marking failure");
}

} // namespace

int main() {
    test_sections_run_serially_with_accepted_handoff();
    test_lower_failure_preserves_upper_and_stops();
    test_cancellation_is_observed_between_sections();
    return check_summary();
}
