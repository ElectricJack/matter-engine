#include "hydrology/water_mesh_continuity.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace hydrology {
namespace {

constexpr double kRadiansToDegrees =
    57.295779513082320876798154814105;

struct Vector3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct EndpointKey {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    bool operator<(const EndpointKey& other) const noexcept {
        if (x != other.x) return x < other.x;
        if (y != other.y) return y < other.y;
        return z < other.z;
    }

    bool operator==(const EndpointKey& other) const noexcept {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct SegmentKey {
    EndpointKey first{};
    EndpointKey second{};

    bool operator<(const SegmentKey& other) const noexcept {
        if (first < other.first) return true;
        if (other.first < first) return false;
        return second < other.second;
    }
};

struct TriangleKey {
    std::array<EndpointKey, 3> points{};

    bool operator<(const TriangleKey& other) const noexcept {
        return std::lexicographical_compare(
            points.begin(), points.end(),
            other.points.begin(), other.points.end());
    }
};

struct CutSample {
    Vector3d position{};
    Vector3d normal{};
};

struct PointAccumulator {
    Vector3d position_sum{};
    Vector3d normal_sum{};
    std::uint32_t count = 0;
};

struct CutContour {
    std::map<EndpointKey, CutSample> points;
    std::vector<SegmentKey> segments;
    std::vector<TriangleKey> coplanar_triangles;
};

Vector3d add(Vector3d a, Vector3d b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vector3d subtract(Vector3d a, Vector3d b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vector3d scale(Vector3d value, double factor) {
    return {value.x * factor, value.y * factor, value.z * factor};
}

double dot(Vector3d a, Vector3d b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double length_squared(Vector3d value) { return dot(value, value); }

bool finite(Vector3d value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool normalize(Vector3d value, Vector3d& result) {
    const double magnitude_squared = length_squared(value);
    if (!std::isfinite(magnitude_squared) ||
        magnitude_squared <= std::numeric_limits<double>::epsilon())
        return false;
    result = scale(value, 1.0 / std::sqrt(magnitude_squared));
    return finite(result);
}

Vector3d mesh_position(const gpu_meshing::MeshResult& mesh,
                       std::uint32_t index) {
    return {static_cast<double>(mesh.positions[index * 3u + 0u]),
            static_cast<double>(mesh.positions[index * 3u + 1u]),
            static_cast<double>(mesh.positions[index * 3u + 2u])};
}

Vector3d mesh_normal(const gpu_meshing::MeshResult& mesh,
                     std::uint32_t index) {
    return {static_cast<double>(mesh.normals[index * 3u + 0u]),
            static_cast<double>(mesh.normals[index * 3u + 1u]),
            static_cast<double>(mesh.normals[index * 3u + 2u])};
}

bool valid_mesh(const gpu_meshing::MeshResult& mesh) {
    if (mesh.positions.empty() || mesh.indices.empty() ||
        mesh.positions.size() != mesh.normals.size() ||
        mesh.positions.size() % 3u != 0u ||
        mesh.indices.size() % 3u != 0u)
        return false;
    for (const float value : mesh.positions)
        if (!std::isfinite(value)) return false;
    for (const float value : mesh.normals)
        if (!std::isfinite(value)) return false;
    const std::size_t vertex_count = mesh.positions.size() / 3u;
    for (const std::uint32_t index : mesh.indices)
        if (index >= vertex_count) return false;
    return true;
}

bool endpoint_key(Vector3d point, double tolerance, EndpointKey& result) {
    const double limit = static_cast<double>(
        std::numeric_limits<std::int64_t>::max()) - 1.0;
    const Vector3d scaled = scale(point, 1.0 / tolerance);
    if (!finite(scaled) || std::fabs(scaled.x) > limit ||
        std::fabs(scaled.y) > limit || std::fabs(scaled.z) > limit)
        return false;
    result = {static_cast<std::int64_t>(std::llround(scaled.x)),
              static_cast<std::int64_t>(std::llround(scaled.y)),
              static_cast<std::int64_t>(std::llround(scaled.z))};
    return true;
}

double signed_axis_distance(const SpillwayHandoffRecord& handoff,
                            Vector3d point) {
    return (point.x - static_cast<double>(handoff.lip_origin_m.x)) *
               static_cast<double>(handoff.tangent.x) +
           (point.y - static_cast<double>(handoff.lip_origin_m.y)) *
               static_cast<double>(handoff.tangent.y) +
           (point.z - static_cast<double>(handoff.lip_origin_m.z)) *
               static_cast<double>(handoff.tangent.z);
}

CutSample interpolate(CutSample a, CutSample b, double amount) {
    return {add(a.position,
                scale(subtract(b.position, a.position), amount)),
            add(a.normal, scale(subtract(b.normal, a.normal), amount))};
}

void add_point(std::map<EndpointKey, PointAccumulator>& accumulators,
               const EndpointKey& key, const CutSample& sample) {
    auto& accumulator = accumulators[key];
    accumulator.position_sum = add(accumulator.position_sum,
                                   sample.position);
    accumulator.normal_sum = add(accumulator.normal_sum, sample.normal);
    ++accumulator.count;
}

bool extract_contour(const gpu_meshing::MeshResult& mesh,
                     const SpillwayHandoffRecord& handoff,
                     double signed_cut_m,
                     double endpoint_tolerance_m,
                     CutContour& contour) {
    contour = {};
    if (!valid_mesh(mesh)) return false;
    const double plane_epsilon =
        std::max(1.0e-7, endpoint_tolerance_m * 1.0e-3);
    std::map<EndpointKey, PointAccumulator> point_accumulators;

    for (std::size_t triangle = 0u; triangle != mesh.indices.size();
         triangle += 3u) {
        std::array<CutSample, 3> vertices{};
        std::array<double, 3> distances{};
        std::array<EndpointKey, 3> vertex_keys{};
        bool all_on_plane = true;
        for (std::size_t corner = 0u; corner != 3u; ++corner) {
            const std::uint32_t index = mesh.indices[triangle + corner];
            vertices[corner] = {mesh_position(mesh, index),
                                mesh_normal(mesh, index)};
            distances[corner] =
                signed_axis_distance(handoff, vertices[corner].position) -
                signed_cut_m;
            all_on_plane = all_on_plane &&
                std::fabs(distances[corner]) <= plane_epsilon;
            if (!endpoint_key(vertices[corner].position,
                              endpoint_tolerance_m,
                              vertex_keys[corner]))
                return false;
        }
        if (all_on_plane) {
            std::sort(vertex_keys.begin(), vertex_keys.end());
            contour.coplanar_triangles.push_back({vertex_keys});
            continue;
        }

        std::map<EndpointKey, CutSample> intersections;
        for (std::size_t edge = 0u; edge != 3u; ++edge) {
            const std::size_t next = (edge + 1u) % 3u;
            const bool first_on =
                std::fabs(distances[edge]) <= plane_epsilon;
            const bool second_on =
                std::fabs(distances[next]) <= plane_epsilon;
            if (first_on) {
                intersections[vertex_keys[edge]] = vertices[edge];
            }
            if (first_on || second_on ||
                !((distances[edge] < 0.0 && distances[next] > 0.0) ||
                  (distances[edge] > 0.0 && distances[next] < 0.0)))
                continue;
            const double amount = distances[edge] /
                (distances[edge] - distances[next]);
            const CutSample sample = interpolate(vertices[edge],
                                                  vertices[next], amount);
            EndpointKey key{};
            if (!finite(sample.position) || !finite(sample.normal) ||
                !endpoint_key(sample.position, endpoint_tolerance_m, key))
                return false;
            intersections[key] = sample;
        }
        if (intersections.size() < 2u) continue;

        auto first = intersections.begin();
        auto second = std::next(first);
        if (intersections.size() > 2u) {
            double largest_distance = -1.0;
            for (auto a = intersections.begin(); a != intersections.end();
                 ++a) {
                for (auto b = std::next(a); b != intersections.end(); ++b) {
                    const double distance = length_squared(subtract(
                        a->second.position, b->second.position));
                    if (distance > largest_distance) {
                        largest_distance = distance;
                        first = a;
                        second = b;
                    }
                }
            }
        }
        if (first->first == second->first) continue;
        const SegmentKey segment{first->first, second->first};
        contour.segments.push_back(segment);
        add_point(point_accumulators, first->first, first->second);
        add_point(point_accumulators, second->first, second->second);
    }

    if (contour.segments.empty()) return false;
    std::sort(contour.segments.begin(), contour.segments.end());
    std::sort(contour.coplanar_triangles.begin(),
              contour.coplanar_triangles.end());
    for (const auto& entry : point_accumulators) {
        const EndpointKey& key = entry.first;
        const PointAccumulator& accumulator = entry.second;
        if (accumulator.count == 0u) return false;
        Vector3d normal{};
        if (!normalize(accumulator.normal_sum, normal)) return false;
        const Vector3d position = scale(
            accumulator.position_sum,
            1.0 / static_cast<double>(accumulator.count));
        if (!finite(position)) return false;
        contour.points.emplace(key, CutSample{position, normal});
    }
    return !contour.points.empty();
}

double coordinate(Vector3d value, std::size_t axis) {
    if (axis == 0u) return value.x;
    if (axis == 1u) return value.y;
    return value.z;
}

void set_coordinate(Vector3d& value, std::size_t axis, double coordinate_m) {
    if (axis == 0u) value.x = coordinate_m;
    else if (axis == 1u) value.y = coordinate_m;
    else value.z = coordinate_m;
}

bool edge_on_cell_cut(Vector3d first, Vector3d second,
                      const WaterCellOwnershipCut& cut,
                      double tolerance_m) {
    const Vector3d midpoint = scale(add(first, second), 0.5);
    const Vector3d lattice_origin{
        cut.lattice.origin_m.x, cut.lattice.origin_m.y,
        cut.lattice.origin_m.z};
    const double voxel = static_cast<double>(cut.lattice.voxel_m);
    const WaterCellOwnership boundary_owner =
        cut.signed_cut_m == cut.handoff.upstream_visual_cut_m
        ? WaterCellOwnership::Before
        : WaterCellOwnership::After;
    for (std::size_t axis = 0u; axis != 3u; ++axis) {
        const double first_relative =
            (coordinate(first, axis) - coordinate(lattice_origin, axis)) /
            voxel;
        const double second_relative =
            (coordinate(second, axis) - coordinate(lattice_origin, axis)) /
            voxel;
        const double face = std::round(first_relative);
        if (std::fabs(first_relative - face) * voxel > tolerance_m ||
            std::fabs(second_relative - face) * voxel > tolerance_m)
            continue;
        Vector3d before{};
        Vector3d after{};
        for (std::size_t cell_axis = 0u; cell_axis != 3u; ++cell_axis) {
            double cell = 0.0;
            if (cell_axis == axis) {
                cell = face - 0.5;
            } else {
                const double relative =
                    (coordinate(midpoint, cell_axis) -
                     coordinate(lattice_origin, cell_axis)) / voxel;
                cell = std::floor(relative) + 0.5;
            }
            set_coordinate(before, cell_axis,
                           coordinate(lattice_origin, cell_axis) +
                               cell * voxel);
            set_coordinate(after, cell_axis,
                           coordinate(lattice_origin, cell_axis) +
                               (cell_axis == axis ? cell + 1.0 : cell) *
                                   voxel);
        }
        const auto ownership = [&](Vector3d center) {
            return classify_water_cell_ownership(
                cut.handoff,
                {static_cast<float>(center.x),
                 static_cast<float>(center.y),
                 static_cast<float>(center.z)});
        };
        const bool before_owned = ownership(before) == boundary_owner;
        const bool after_owned = ownership(after) == boundary_owner;
        if (before_owned != after_owned) return true;
    }
    return false;
}

bool extract_cell_boundary_contour(
    const gpu_meshing::MeshResult& mesh,
    const WaterCellOwnershipCut& cut,
    double endpoint_tolerance_m,
    CutContour& contour) {
    contour = {};
    if (!valid_mesh(mesh)) return false;
    struct EdgeRecord {
        std::uint32_t count = 0u;
        CutSample first{};
        CutSample second{};
    };
    std::map<SegmentKey, EdgeRecord> edges;
    std::map<EndpointKey, PointAccumulator> point_accumulators;
    for (std::size_t triangle = 0u; triangle != mesh.indices.size();
         triangle += 3u) {
        std::array<EndpointKey, 3> triangle_points{};
        for (std::size_t corner = 0u; corner != 3u; ++corner) {
            const std::uint32_t index = mesh.indices[triangle + corner];
            if (!endpoint_key(mesh_position(mesh, index),
                              endpoint_tolerance_m,
                              triangle_points[corner]))
                return false;
        }
        std::sort(triangle_points.begin(), triangle_points.end());
        contour.coplanar_triangles.push_back({triangle_points});
        for (std::size_t edge = 0u; edge != 3u; ++edge) {
            const std::uint32_t first_index = mesh.indices[triangle + edge];
            const std::uint32_t second_index =
                mesh.indices[triangle + (edge + 1u) % 3u];
            CutSample first_sample{mesh_position(mesh, first_index),
                                   mesh_normal(mesh, first_index)};
            CutSample second_sample{mesh_position(mesh, second_index),
                                    mesh_normal(mesh, second_index)};
            EndpointKey first_key{};
            EndpointKey second_key{};
            if (!endpoint_key(first_sample.position, endpoint_tolerance_m,
                              first_key) ||
                !endpoint_key(second_sample.position, endpoint_tolerance_m,
                              second_key))
                return false;
            if (second_key < first_key) {
                std::swap(first_key, second_key);
                std::swap(first_sample, second_sample);
            }
            auto& record = edges[{first_key, second_key}];
            ++record.count;
            record.first = first_sample;
            record.second = second_sample;
        }
    }
    for (const auto& entry : edges) {
        if ((entry.second.count & 1u) == 0u) continue;
        if (!edge_on_cell_cut(entry.second.first.position,
                              entry.second.second.position, cut,
                              endpoint_tolerance_m))
            continue;
        contour.segments.push_back(entry.first);
        add_point(point_accumulators, entry.first.first,
                  entry.second.first);
        add_point(point_accumulators, entry.first.second,
                  entry.second.second);
    }
    if (contour.segments.empty()) return false;
    std::sort(contour.segments.begin(), contour.segments.end());
    std::sort(contour.coplanar_triangles.begin(),
              contour.coplanar_triangles.end());
    for (const auto& entry : point_accumulators) {
        const PointAccumulator& accumulator = entry.second;
        Vector3d normal{};
        if (accumulator.count == 0u ||
            !normalize(accumulator.normal_sum, normal))
            return false;
        const Vector3d position = scale(
            accumulator.position_sum,
            1.0 / static_cast<double>(accumulator.count));
        if (!finite(position)) return false;
        contour.points.emplace(entry.first, CutSample{position, normal});
    }
    return !contour.points.empty();
}

float fixed_quantile(std::vector<double> values, double quantile) {
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(std::floor(
        quantile * static_cast<double>(values.size() - 1u)));
    return static_cast<float>(values[index]);
}

std::array<float, 3> height_quantiles(const CutContour& contour) {
    std::vector<double> heights;
    heights.reserve(contour.points.size());
    for (const auto& entry : contour.points)
        heights.push_back(entry.second.position.y);
    return {fixed_quantile(heights, 0.10),
            fixed_quantile(heights, 0.50),
            fixed_quantile(std::move(heights), 0.90)};
}

struct NearestSample {
    double squared_distance = std::numeric_limits<double>::infinity();
    Vector3d normal{};
};

bool nearest_segment_sample(Vector3d point, const CutContour& target,
                            NearestSample& nearest) {
    nearest = {};
    for (const SegmentKey& segment : target.segments) {
        const auto first = target.points.find(segment.first);
        const auto second = target.points.find(segment.second);
        if (first == target.points.end() || second == target.points.end())
            return false;
        const Vector3d delta = subtract(second->second.position,
                                        first->second.position);
        const double denominator = length_squared(delta);
        if (!std::isfinite(denominator) ||
            denominator <= std::numeric_limits<double>::epsilon())
            return false;
        const double amount = std::clamp(
            dot(subtract(point, first->second.position), delta) /
                denominator,
            0.0, 1.0);
        const Vector3d closest = add(first->second.position,
                                     scale(delta, amount));
        const double squared_distance =
            length_squared(subtract(point, closest));
        if (squared_distance < nearest.squared_distance) {
            Vector3d normal{};
            if (!normalize(add(scale(first->second.normal, 1.0 - amount),
                               scale(second->second.normal, amount)),
                           normal))
                return false;
            nearest = {squared_distance, normal};
        }
    }
    return std::isfinite(nearest.squared_distance) && finite(nearest.normal);
}

bool accumulate_directed_metrics(const CutContour& source,
                                 const CutContour& target,
                                 double& maximum_squared_distance,
                                 double& sum_squared_distance,
                                 std::vector<double>& normal_dots,
                                 std::vector<double>& normal_angles) {
    for (const auto& entry : source.points) {
        NearestSample nearest{};
        if (!nearest_segment_sample(entry.second.position, target, nearest))
            return false;
        maximum_squared_distance = std::max(maximum_squared_distance,
                                             nearest.squared_distance);
        sum_squared_distance += nearest.squared_distance;
        const double normal_dot = std::clamp(
            dot(entry.second.normal, nearest.normal), -1.0, 1.0);
        normal_dots.push_back(normal_dot);
        normal_angles.push_back(std::acos(normal_dot) * kRadiansToDegrees);
    }
    return true;
}

std::uint32_t unmatched_open_edges(const CutContour& first,
                                   const CutContour& second) {
    std::map<SegmentKey, std::uint64_t> combined_edges;
    const auto add = [&](const CutContour& contour) {
        for (const SegmentKey& segment : contour.segments)
            ++combined_edges[segment];
    };
    add(first);
    add(second);
    std::uint64_t unmatched = 0u;
    for (const auto& entry : combined_edges)
        if ((entry.second & 1u) != 0u) ++unmatched;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        unmatched, std::numeric_limits<std::uint32_t>::max()));
}

std::uint32_t duplicate_coplanar_triangles(const CutContour& first,
                                           const CutContour& second) {
    std::map<TriangleKey, std::uint64_t> occurrences;
    for (const TriangleKey& triangle : first.coplanar_triangles)
        ++occurrences[triangle];
    for (const TriangleKey& triangle : second.coplanar_triangles)
        ++occurrences[triangle];
    std::uint64_t duplicates = 0u;
    for (const auto& entry : occurrences)
        if (entry.second > 1u) duplicates += entry.second - 1u;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        duplicates, std::numeric_limits<std::uint32_t>::max()));
}

bool finite_handoff(const SpillwayHandoffRecord& handoff) {
    const Vector3d origin{handoff.lip_origin_m.x, handoff.lip_origin_m.y,
                          handoff.lip_origin_m.z};
    const Vector3d tangent{handoff.tangent.x, handoff.tangent.y,
                           handoff.tangent.z};
    return finite(origin) && finite(tangent) &&
           length_squared(tangent) >
               std::numeric_limits<double>::epsilon();
}

bool finite_metrics(const WaterCutContourMetrics& metrics) {
    if (!std::isfinite(metrics.symmetric_hausdorff_m) ||
        !std::isfinite(metrics.rms_distance_m) ||
        !std::isfinite(metrics.minimum_normal_dot) ||
        !std::isfinite(metrics.p95_normal_angle_degrees))
        return false;
    for (const float value : metrics.first_height_quantiles_m)
        if (!std::isfinite(value)) return false;
    for (const float value : metrics.second_height_quantiles_m)
        if (!std::isfinite(value)) return false;
    return true;
}

bool reduce_contours(const CutContour& first_contour,
                     const CutContour& second_contour,
                     WaterCutContourMetrics& metrics) {
    if (first_contour.points.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        second_contour.points.size() >
            std::numeric_limits<std::uint32_t>::max())
        return false;
    metrics.first_points = static_cast<std::uint32_t>(
        first_contour.points.size());
    metrics.second_points = static_cast<std::uint32_t>(
        second_contour.points.size());
    metrics.first_height_quantiles_m = height_quantiles(first_contour);
    metrics.second_height_quantiles_m = height_quantiles(second_contour);
    metrics.unmatched_open_edges = unmatched_open_edges(
        first_contour, second_contour);
    metrics.duplicate_coplanar_triangles =
        duplicate_coplanar_triangles(first_contour, second_contour);

    double maximum_squared_distance = 0.0;
    double sum_squared_distance = 0.0;
    std::vector<double> normal_dots;
    std::vector<double> normal_angles;
    normal_dots.reserve(first_contour.points.size() +
                        second_contour.points.size());
    normal_angles.reserve(normal_dots.capacity());
    if (!accumulate_directed_metrics(
            first_contour, second_contour, maximum_squared_distance,
            sum_squared_distance, normal_dots, normal_angles) ||
        !accumulate_directed_metrics(
            second_contour, first_contour, maximum_squared_distance,
            sum_squared_distance, normal_dots, normal_angles) ||
        normal_dots.empty())
        return false;
    metrics.symmetric_hausdorff_m = static_cast<float>(
        std::sqrt(maximum_squared_distance));
    metrics.rms_distance_m = static_cast<float>(std::sqrt(
        sum_squared_distance / static_cast<double>(normal_dots.size())));
    std::sort(normal_dots.begin(), normal_dots.end());
    metrics.minimum_normal_dot = static_cast<float>(normal_dots.front());
    metrics.p95_normal_angle_degrees =
        fixed_quantile(std::move(normal_angles), 0.95);
    return finite_metrics(metrics);
}

bool fail(const char* message, FluidBakeError& error) {
    error = {FluidBakeCode::ProductFailure, message};
    return false;
}

}  // namespace

WaterCellOwnership classify_water_cell_ownership(
    const SpillwayHandoffRecord& handoff,
    matter::Float3 root_cell_center) noexcept {
    const matter::Float3 relative{
        root_cell_center.x - handoff.lip_origin_m.x,
        root_cell_center.y - handoff.lip_origin_m.y,
        root_cell_center.z - handoff.lip_origin_m.z};
    const float distance = relative.x * handoff.tangent.x +
        relative.y * handoff.tangent.y +
        relative.z * handoff.tangent.z;
    if (distance < handoff.upstream_visual_cut_m)
        return WaterCellOwnership::Before;
    if (distance > handoff.downstream_visual_cut_m)
        return WaterCellOwnership::After;
    return WaterCellOwnership::Between;
}

bool measure_water_cut_continuity(
    const gpu_meshing::MeshResult& first,
    const gpu_meshing::MeshResult& second,
    const SpillwayHandoffRecord& handoff,
    float signed_cut_m,
    float edge_match_tolerance_m,
    WaterCutContourMetrics& metrics,
    FluidBakeError& error) {
    metrics = {};
    error = {};
    if (!std::isfinite(signed_cut_m) ||
        !std::isfinite(edge_match_tolerance_m) ||
        edge_match_tolerance_m <= 0.0f || !finite_handoff(handoff))
        return fail("water cut continuity input is invalid", error);
    try {
        CutContour first_contour{};
        CutContour second_contour{};
        if (!extract_contour(first, handoff,
                             static_cast<double>(signed_cut_m),
                             static_cast<double>(edge_match_tolerance_m),
                             first_contour))
            return fail("first water mesh has no valid cut contour", error);
        if (!extract_contour(second, handoff,
                             static_cast<double>(signed_cut_m),
                             static_cast<double>(edge_match_tolerance_m),
                             second_contour))
            return fail("second water mesh has no valid cut contour", error);

        if (!reduce_contours(first_contour, second_contour, metrics))
            return fail("water cut continuity reduction failed", error);
        return true;
    } catch (const std::exception& exception) {
        metrics = {};
        error = {FluidBakeCode::ProductFailure, exception.what()};
        return false;
    } catch (...) {
        metrics = {};
        return fail("water cut continuity measurement failed", error);
    }
}

bool measure_water_cell_boundary_continuity(
    const gpu_meshing::MeshResult& first,
    const gpu_meshing::MeshResult& second,
    const WaterCellOwnershipCut& cut,
    float edge_match_tolerance_m,
    WaterCutContourMetrics& metrics,
    FluidBakeError& error) {
    metrics = {};
    error = {};
    if (cut.lattice.version != 1u ||
        !std::isfinite(cut.lattice.voxel_m) ||
        cut.lattice.voxel_m <= 0.0f ||
        !std::isfinite(cut.signed_cut_m) ||
        !std::isfinite(edge_match_tolerance_m) ||
        edge_match_tolerance_m <= 0.0f ||
        !finite_handoff(cut.handoff) ||
        (cut.signed_cut_m != cut.handoff.upstream_visual_cut_m &&
         cut.signed_cut_m != cut.handoff.downstream_visual_cut_m))
        return fail("water cell boundary continuity input is invalid", error);
    try {
        CutContour first_contour{};
        CutContour second_contour{};
        if (!extract_cell_boundary_contour(
                first, cut, edge_match_tolerance_m, first_contour))
            return fail(
                "first water mesh has no valid cell boundary contour", error);
        if (!extract_cell_boundary_contour(
                second, cut, edge_match_tolerance_m, second_contour))
            return fail(
                "second water mesh has no valid cell boundary contour", error);
        if (!reduce_contours(first_contour, second_contour, metrics))
            return fail("water cell boundary continuity reduction failed",
                        error);
        return true;
    } catch (const std::exception& exception) {
        metrics = {};
        error = {FluidBakeCode::ProductFailure, exception.what()};
        return false;
    } catch (...) {
        metrics = {};
        return fail("water cell boundary continuity measurement failed",
                    error);
    }
}

bool water_cut_is_assertion_weldable(
    const WaterCutContourMetrics& metrics,
    float quantization_tolerance_m) noexcept {
    return std::isfinite(quantization_tolerance_m) &&
           quantization_tolerance_m >= 0.0f && finite_metrics(metrics) &&
           metrics.first_points != 0u && metrics.second_points != 0u &&
           metrics.symmetric_hausdorff_m <= quantization_tolerance_m &&
           metrics.rms_distance_m <= quantization_tolerance_m &&
           metrics.minimum_normal_dot >= 0.995f &&
           metrics.unmatched_open_edges == 0u &&
           metrics.duplicate_coplanar_triangles == 0u;
}

}  // namespace hydrology
