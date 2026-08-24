#include "hydrology/river_section_coordinator.h"

#include <algorithm>
#include <exception>
#include <unordered_set>

namespace hydrology {
namespace {

bool fail(RiverSectionSequenceResult& output, FluidBakeError& error,
          FluidBakeCode code, const std::string& message) {
    output.manifest.state = code == FluidBakeCode::Cancelled
        ? HydrologyNetworkState::Incomplete
        : HydrologyNetworkState::Failed;
    error = {code, message};
    return false;
}

bool valid_graph(const matter::RiverNetworkDefinition& network,
                 const RiverSectionGraph& graph) {
    if (network.sections.empty() ||
        graph.topological_order.size() != network.sections.size() ||
        graph.upstream.size() != network.sections.size())
        return false;
    std::vector<bool> seen(network.sections.size(), false);
    for (const auto index : graph.topological_order) {
        if (index >= network.sections.size() || seen[index]) return false;
        seen[index] = true;
    }
    for (std::size_t index = 0; index < graph.upstream.size(); ++index) {
        std::unordered_set<std::size_t> dependencies;
        for (const auto upstream : graph.upstream[index]) {
            if (upstream >= network.sections.size() || upstream == index ||
                !dependencies.insert(upstream).second)
                return false;
        }
    }
    return true;
}

std::string section_path(const std::string& id) {
    return "sections/" + id + ".mhyd";
}

std::string handoff_path(const std::string& id) {
    return "handoffs/" + id + ".mhyd";
}

} // namespace

bool run_river_section_sequence(
    const matter::RiverNetworkDefinition& network,
    const RiverSectionGraph& graph,
    const SectionBakeExecutor& executor,
    const RiverSectionSequenceCallbacks& callbacks,
    RiverSectionSequenceResult& output,
    FluidBakeError& error) {
    output = {};
    error = {};
    output.manifest.state = HydrologyNetworkState::Incomplete;
    output.manifest.network_key = network.canonical_hash;
    if (!executor || !valid_graph(network, graph))
        return fail(output, error, FluidBakeCode::InvalidInput,
                    "river section sequence input is invalid");

    const auto report = [&](float fraction, const std::string& id) {
        if (callbacks.progress) callbacks.progress(fraction, id);
    };
    const auto cancelled = [&]() {
        return callbacks.cancelled && callbacks.cancelled();
    };
    std::vector<std::optional<SpillwayHandoffRecord>> downstream(
        network.sections.size());
    try {
        report(0.0f, {});
        for (std::size_t order = 0; order < graph.topological_order.size();
             ++order) {
            if (cancelled())
                return fail(output, error, FluidBakeCode::Cancelled,
                            "river section sequence was cancelled between sections");
            const std::size_t section_index = graph.topological_order[order];
            const auto& section = network.sections[section_index];
            std::vector<std::size_t> dependency_indices =
                graph.upstream[section_index];
            std::sort(dependency_indices.begin(), dependency_indices.end(),
                      [&](std::size_t lhs, std::size_t rhs) {
                          return network.sections[lhs].id <
                                 network.sections[rhs].id;
                      });
            std::vector<SpillwayHandoffRecord> inherited;
            std::vector<std::string> dependency_ids;
            for (const auto upstream_index : dependency_indices) {
                const auto& handoff = downstream[upstream_index];
                if (!handoff || handoff->semantic_key == 0u ||
                    handoff->upstream_section_id !=
                        network.sections[upstream_index].id ||
                    handoff->downstream_section_id != section.id) {
                    return fail(
                        output, error, FluidBakeCode::BackendFailure,
                        "accepted upstream section is missing its spillway handoff");
                }
                inherited.push_back(*handoff);
                dependency_ids.push_back(
                    network.sections[upstream_index].id);
            }

            SectionBakeResult result{};
            FluidBakeError section_error{};
            if (!executor(section, inherited, result, section_error)) {
                const auto code = section_error.code == FluidBakeCode::Ready
                    ? FluidBakeCode::BackendFailure : section_error.code;
                return fail(output, error, code,
                            section_error.message.empty()
                                ? "river section executor failed without a diagnostic"
                                : section_error.message);
            }
            if (!result.artifact.accepted ||
                result.artifact.section.section_id != section.id ||
                (!result.artifact.section.river_id.empty() &&
                 result.artifact.section.river_id != section.river)) {
                return fail(output, error, FluidBakeCode::BackendFailure,
                            "river section executor returned the wrong accepted identity");
            }
            if (result.downstream_handoff) {
                const auto& handoff = *result.downstream_handoff;
                if (handoff.id.empty() || handoff.semantic_key == 0u ||
                    handoff.upstream_section_id != section.id) {
                    return fail(output, error, FluidBakeCode::BackendFailure,
                                "river section executor returned an invalid spillway handoff");
                }
                downstream[section_index] = handoff;
                output.manifest.handoffs.push_back({
                    handoff.id, handoff_path(handoff.id),
                    {handoff.upstream_section_id,
                     handoff.downstream_section_id},
                    handoff.semantic_key, handoff.semantic_key});
            }
            output.manifest.sections.push_back({
                section.id, section_path(section.id), dependency_ids,
                result.artifact.semantic_key,
                result.artifact.payload_digest});
            output.manifest.topological_order.push_back(section.id);
            output.sections.push_back(std::move(result));
            report(static_cast<float>(order + 1u) /
                       static_cast<float>(graph.topological_order.size()),
                   section.id);
        }
        error = {};
        return true;
    } catch (const std::exception& exception) {
        return fail(output, error, FluidBakeCode::BackendFailure,
                    exception.what());
    } catch (...) {
        return fail(output, error, FluidBakeCode::BackendFailure,
                    "river section sequence raised an unknown exception");
    }
}

} // namespace hydrology
