#include "river_section_graph.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace hydrology {
namespace {

bool fail(std::string& error, const std::string& message) {
    error = "hydrology.section: " + message;
    return false;
}

bool finite(float value) { return std::isfinite(value); }

float sample_height(const RiverGeometry& geometry, float distance_m) {
    if (distance_m <= geometry.centreline.front().distance_m)
        return geometry.centreline.front().position_m.y;
    for (std::size_t index = 1u; index < geometry.centreline.size(); ++index) {
        const auto& a = geometry.centreline[index - 1u];
        const auto& b = geometry.centreline[index];
        if (distance_m <= b.distance_m || index + 1u == geometry.centreline.size()) {
            const float span = b.distance_m - a.distance_m;
            const float t = span > 0.0f
                ? std::clamp((distance_m - a.distance_m) / span, 0.0f, 1.0f)
                : 0.0f;
            return a.position_m.y + (b.position_m.y - a.position_m.y) * t;
        }
    }
    return geometry.centreline.back().position_m.y;
}

bool unique_nonempty(const std::vector<std::string>& ids) {
    std::set<std::string> seen;
    for (const auto& id : ids)
        if (id.empty() || !seen.insert(id).second) return false;
    return true;
}

std::vector<std::string> sorted(std::vector<std::string> ids) {
    std::sort(ids.begin(), ids.end());
    return ids;
}

}  // namespace

bool build_river_section_graph(
    const matter::RiverNetworkDefinition& network,
    const std::vector<RiverGeometry>& geometry,
    RiverSectionGraph& graph,
    std::string& error) {
    graph = {};
    error.clear();
    if (!network.bake_sequential)
        return fail(error, "bakeSequential is required");
    if (network.sections.empty())
        return fail(error, "at least one section is required");
    if (geometry.size() != network.rivers.size())
        return fail(error, "river geometry count does not match authored rivers");

    std::map<std::string, std::size_t> river_indices;
    for (std::size_t index = 0u; index < network.rivers.size(); ++index) {
        if (geometry[index].centreline.size() < 2u)
            return fail(error, "river geometry is incomplete");
        river_indices.emplace(network.rivers[index].name, index);
    }
    std::set<std::string> emitter_ids;
    for (const auto& emitter : network.fluid.emitters)
        emitter_ids.insert(emitter.id);

    std::map<std::string, std::size_t> section_indices;
    std::set<std::string> spillway_ids;
    const float tolerance = std::max(network.cell_size_m, 0.25f);
    for (std::size_t index = 0u; index < network.sections.size(); ++index) {
        const auto& section = network.sections[index];
        if (section.id.empty()) return fail(error, "section id must not be empty");
        if (!section_indices.emplace(section.id, index).second)
            return fail(error, "duplicate section id '" + section.id + "'");
        const auto river = river_indices.find(section.river);
        if (river == river_indices.end())
            return fail(error, "section '" + section.id + "' names an unknown river");
        const auto& river_geometry = geometry[river->second];
        const float river_length = river_geometry.centreline.back().distance_m;
        if (!finite(section.from_m) || !finite(section.to_m) ||
            section.from_m < 0.0f || section.to_m <= section.from_m ||
            section.to_m > river_length + tolerance)
            return fail(error, "section '" + section.id + "' range is outside river geometry");
        if (!finite(section.dry_margin_m) || section.dry_margin_m <= 0.0f)
            return fail(error, "section '" + section.id + "' dry margin is invalid");
        if (!unique_nonempty(section.emitter_ids))
            return fail(error, "section '" + section.id + "' emitter ids are invalid");
        for (const auto& emitter : section.emitter_ids)
            if (emitter_ids.count(emitter) == 0u)
                return fail(error, "section '" + section.id + "' names an unknown emitter");
        for (const auto& waterfall : section.waterfalls) {
            if (!finite(waterfall.lip_distance_m) ||
                !finite(waterfall.landing_distance_m) ||
                !finite(waterfall.expected_drop_m) ||
                waterfall.lip_distance_m < section.from_m - tolerance ||
                waterfall.landing_distance_m > section.to_m + tolerance ||
                waterfall.landing_distance_m <= waterfall.lip_distance_m ||
                waterfall.expected_drop_m <= 0.0f)
                return fail(error, "waterfall range is outside section '" + section.id + "'");
            const float actual_drop =
                sample_height(river_geometry, waterfall.lip_distance_m) -
                sample_height(river_geometry, waterfall.landing_distance_m);
            if (std::fabs(actual_drop - waterfall.expected_drop_m) > tolerance)
                return fail(error, "waterfall drop does not match authored curve in section '" +
                                   section.id + "'");
        }
        if (!section.terminal_pool)
            return fail(error, "section '" + section.id + "' requires a terminal pool");
        const auto& pool = *section.terminal_pool;
        if (!finite(pool.start_distance_m) || !finite(pool.end_distance_m) ||
            !finite(pool.fill_level_m) ||
            pool.start_distance_m < section.from_m - tolerance ||
            pool.end_distance_m > section.to_m + tolerance ||
            pool.end_distance_m <= pool.start_distance_m ||
            std::fabs(pool.end_distance_m - section.to_m) > tolerance)
            return fail(error, "terminal pool range is invalid in section '" + section.id + "'");
        if (!section.terminal_spillway)
            return fail(error, "section '" + section.id + "' requires a terminal spillway");
        const auto& spillway = *section.terminal_spillway;
        if (spillway.id.empty() || !spillway_ids.insert(spillway.id).second)
            return fail(error, "terminal spillway id must be non-empty and unique");
        if (!finite(spillway.distance_m) || !finite(spillway.width_m) ||
            !finite(spillway.effective_depth_m) || !finite(spillway.overlap_m) ||
            !finite(spillway.dam_offset_m) || spillway.width_m <= 0.0f ||
            spillway.effective_depth_m <= 0.0f || spillway.overlap_m <= 0.0f ||
            spillway.dam_offset_m < 0.0f ||
            std::fabs(spillway.distance_m - section.to_m) > tolerance)
            return fail(error, "terminal spillway range or dimensions are invalid in section '" +
                               section.id + "'");
        if (!unique_nonempty(section.after_section_ids) ||
            !unique_nonempty(section.upstream_spillway_section_ids))
            return fail(error, "section dependencies must be non-empty unique ids");
        if (sorted(section.after_section_ids) !=
            sorted(section.upstream_spillway_section_ids))
            return fail(error, "after and fromSpillway dependencies disagree in section '" +
                               section.id + "'");
    }

    graph.upstream.resize(network.sections.size());
    std::vector<std::vector<std::size_t>> downstream(network.sections.size());
    std::vector<std::size_t> indegree(network.sections.size(), 0u);
    for (std::size_t index = 0u; index < network.sections.size(); ++index) {
        for (const auto& upstream_id : network.sections[index].after_section_ids) {
            const auto upstream = section_indices.find(upstream_id);
            if (upstream == section_indices.end())
                return fail(error, "section '" + network.sections[index].id +
                                   "' names an unknown dependency");
            if (upstream->second == index)
                return fail(error, "section cannot depend on itself");
            graph.upstream[index].push_back(upstream->second);
            downstream[upstream->second].push_back(index);
            ++indegree[index];
        }
        std::sort(graph.upstream[index].begin(), graph.upstream[index].end(),
                  [&](std::size_t a, std::size_t b) {
                      return network.sections[a].id < network.sections[b].id;
                  });
    }

    auto compare = [&](std::size_t a, std::size_t b) {
        return network.sections[a].id < network.sections[b].id;
    };
    std::vector<std::size_t> ready;
    for (std::size_t index = 0u; index < indegree.size(); ++index)
        if (indegree[index] == 0u) ready.push_back(index);
    std::sort(ready.begin(), ready.end(), compare);
    while (!ready.empty()) {
        const std::size_t current = ready.front();
        ready.erase(ready.begin());
        graph.topological_order.push_back(current);
        for (const auto child : downstream[current]) {
            if (--indegree[child] == 0u) {
                ready.push_back(child);
                std::sort(ready.begin(), ready.end(), compare);
            }
        }
    }
    if (graph.topological_order.size() != network.sections.size()) {
        graph = {};
        return fail(error, "section dependency cycle detected");
    }
    return true;
}

}  // namespace hydrology
