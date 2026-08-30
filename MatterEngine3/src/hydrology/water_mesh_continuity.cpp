#include "hydrology/water_mesh_continuity.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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

Vector3d cross(Vector3d a, Vector3d b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
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

bool offset_endpoint_key(EndpointKey source,
                         std::int64_t dx, std::int64_t dy, std::int64_t dz,
                         EndpointKey& result) {
    const auto coordinate = [](
        std::int64_t value, std::int64_t offset,
        std::int64_t& output) {
        if ((offset > 0 &&
             value > std::numeric_limits<std::int64_t>::max() - offset) ||
            (offset < 0 &&
             value < std::numeric_limits<std::int64_t>::min() - offset)) {
            return false;
        }
        output = value + offset;
        return true;
    };
    return coordinate(source.x, dx, result.x) &&
           coordinate(source.y, dy, result.y) &&
           coordinate(source.z, dz, result.z);
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

bool triangle_on_cell_cut(const std::array<Vector3d, 3>& vertices,
                          const WaterCellOwnershipCut& cut,
                          double tolerance_m) {
    const Vector3d lattice_origin{
        cut.lattice.origin_m.x, cut.lattice.origin_m.y,
        cut.lattice.origin_m.z};
    const double voxel = static_cast<double>(cut.lattice.voxel_m);
    const WaterCellOwnership boundary_owner =
        cut.signed_cut_m == cut.handoff.upstream_visual_cut_m
        ? WaterCellOwnership::Before
        : WaterCellOwnership::After;
    const Vector3d centroid = scale(
        add(add(vertices[0], vertices[1]), vertices[2]), 1.0 / 3.0);
    for (std::size_t axis = 0u; axis != 3u; ++axis) {
        const double first_relative =
            (coordinate(vertices[0], axis) -
             coordinate(lattice_origin, axis)) / voxel;
        const double face = std::round(first_relative);
        bool common_face = true;
        for (const Vector3d vertex : vertices) {
            const double relative =
                (coordinate(vertex, axis) -
                 coordinate(lattice_origin, axis)) / voxel;
            common_face = common_face &&
                std::fabs(relative - face) * voxel <= tolerance_m;
        }
        if (!common_face) continue;

        Vector3d before{};
        Vector3d after{};
        for (std::size_t cell_axis = 0u; cell_axis != 3u; ++cell_axis) {
            const double cell = cell_axis == axis
                ? face - 0.5
                : std::floor(
                      (coordinate(centroid, cell_axis) -
                       coordinate(lattice_origin, cell_axis)) / voxel) +
                      0.5;
            set_coordinate(
                before, cell_axis,
                coordinate(lattice_origin, cell_axis) + cell * voxel);
            set_coordinate(
                after, cell_axis,
                coordinate(lattice_origin, cell_axis) +
                    (cell_axis == axis ? cell + 1.0 : cell) * voxel);
        }
        const auto ownership = [&](Vector3d center) {
            return classify_water_cell_ownership(
                cut.handoff,
                {static_cast<float>(center.x),
                 static_cast<float>(center.y),
                 static_cast<float>(center.z)});
        };
        if ((ownership(before) == boundary_owner) !=
            (ownership(after) == boundary_owner)) {
            return true;
        }
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
    struct EdgeCandidate {
        CutSample first{};
        CutSample second{};
    };
    struct EdgeRecord {
        std::vector<EdgeCandidate> forward;
        std::vector<EdgeCandidate> reverse;
    };
    std::map<SegmentKey, EdgeRecord> edges;
    for (std::size_t triangle = 0u; triangle != mesh.indices.size();
         triangle += 3u) {
        std::array<EndpointKey, 3> triangle_points{};
        std::array<Vector3d, 3> triangle_vertices{};
        for (std::size_t corner = 0u; corner != 3u; ++corner) {
            const std::uint32_t index = mesh.indices[triangle + corner];
            triangle_vertices[corner] = mesh_position(mesh, index);
            if (!endpoint_key(triangle_vertices[corner],
                              endpoint_tolerance_m,
                              triangle_points[corner]))
                return false;
        }
        if (triangle_on_cell_cut(
                triangle_vertices, cut, endpoint_tolerance_m)) {
            std::sort(triangle_points.begin(), triangle_points.end());
            if (!(triangle_points[0] == triangle_points[1]) &&
                !(triangle_points[1] == triangle_points[2]))
                contour.coplanar_triangles.push_back({triangle_points});
        }
        const Vector3d triangle_cross = cross(
            subtract(triangle_vertices[1], triangle_vertices[0]),
            subtract(triangle_vertices[2], triangle_vertices[0]));
        if (length_squared(triangle_cross) <=
            std::numeric_limits<double>::epsilon())
            continue;
        for (std::size_t edge = 0u; edge != 3u; ++edge) {
            const std::uint32_t first_index = mesh.indices[triangle + edge];
            const std::uint32_t second_index =
                mesh.indices[triangle + (edge + 1u) % 3u];
            CutSample first_sample{mesh_position(mesh, first_index),
                                   mesh_normal(mesh, first_index)};
            CutSample second_sample{mesh_position(mesh, second_index),
                                    mesh_normal(mesh, second_index)};
            if (!edge_on_cell_cut(
                    first_sample.position, second_sample.position, cut,
                    endpoint_tolerance_m))
                continue;
            EndpointKey first_key{};
            EndpointKey second_key{};
            if (!endpoint_key(first_sample.position, endpoint_tolerance_m,
                              first_key) ||
                !endpoint_key(second_sample.position, endpoint_tolerance_m,
                              second_key))
                return false;
            bool forward = true;
            if (second_key < first_key) {
                forward = false;
                std::swap(first_key, second_key);
                std::swap(first_sample, second_sample);
            }
            if (first_key == second_key) continue;
            auto& record = edges[{first_key, second_key}];
            auto& candidates = forward ? record.forward : record.reverse;
            candidates.push_back({first_sample, second_sample});
        }
    }

    std::map<EndpointKey, PointAccumulator> point_accumulators;
    for (const auto& entry : edges) {
        const std::size_t paired = std::min(
            entry.second.forward.size(), entry.second.reverse.size());
        const auto append_unpaired = [&](const auto& candidates) {
            for (std::size_t index = paired;
                 index != candidates.size(); ++index) {
                const EdgeCandidate& candidate = candidates[index];
                contour.segments.push_back(entry.first);
                add_point(point_accumulators, entry.first.first,
                          candidate.first);
                add_point(point_accumulators, entry.first.second,
                          candidate.second);
            }
        };
        append_unpaired(entry.second.forward);
        append_unpaired(entry.second.reverse);
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

std::vector<SegmentKey> normalized_segments(const CutContour& contour,
                                            double tolerance_m) {
    // A packed contour can retain a near-zero subdivision edge that the raw
    // contour quantizes away. Collapse only endpoints connected by that
    // actual sub-tolerance edge. Unrelated nearby contour vertices must stay
    // distinct; their proximity is relevant only to cross-mesh matching.
    std::map<EndpointKey, EndpointKey> parent;
    for (const auto& point : contour.points)
        parent.emplace(point.first, point.first);
    std::function<EndpointKey(EndpointKey)> root = [&](EndpointKey key) {
        auto found = parent.find(key);
        if (found == parent.end() || found->second == key) return key;
        found->second = root(found->second);
        return found->second;
    };
    const double tolerance_squared = tolerance_m * tolerance_m;
    for (const SegmentKey& segment : contour.segments) {
        const auto first = contour.points.find(segment.first);
        const auto second = contour.points.find(segment.second);
        if (first == contour.points.end() ||
            second == contour.points.end())
            continue;
        if (length_squared(subtract(first->second.position,
                                    second->second.position)) >
            tolerance_squared)
            continue;
        EndpointKey first_root = root(segment.first);
        EndpointKey second_root = root(segment.second);
        if (first_root == second_root) continue;
        if (second_root < first_root) std::swap(first_root, second_root);
        parent[second_root] = first_root;
    }

    const auto collect_unique = [&]() {
        std::map<SegmentKey, bool> unique;
        for (const SegmentKey& segment : contour.segments) {
            SegmentKey canonical{root(segment.first), root(segment.second)};
            if (canonical.first == canonical.second) continue;
            if (canonical.second < canonical.first)
                std::swap(canonical.first, canonical.second);
            unique[canonical] = true;
        }
        std::vector<SegmentKey> segments;
        segments.reserve(unique.size());
        for (const auto& entry : unique) segments.push_back(entry.first);
        return segments;
    };

    std::vector<SegmentKey> normalized = collect_unique();
    std::map<EndpointKey, std::uint32_t> degree;
    std::map<EndpointKey, EndpointKey> only_neighbor;
    for (const SegmentKey& segment : normalized) {
        ++degree[segment.first];
        ++degree[segment.second];
        only_neighbor[segment.first] = segment.second;
        only_neighbor[segment.second] = segment.first;
    }

    // Quantization can split one logical degree-two vertex into two nearby
    // degree-one endpoints. Merge only a pair whose incident edges continue
    // through one another. Parallel nearby contours have matching directions
    // and therefore remain distinct.
    std::vector<std::pair<double, SegmentKey>> continuation_candidates;
    for (const auto& endpoint : degree) {
        if (endpoint.second != 1u) continue;
        const auto sample = contour.points.find(endpoint.first);
        const auto neighbor = only_neighbor.find(endpoint.first);
        if (sample == contour.points.end() ||
            neighbor == only_neighbor.end())
            continue;
        Vector3d direction{};
        const auto neighbor_sample = contour.points.find(neighbor->second);
        if (neighbor_sample == contour.points.end() ||
            !normalize(subtract(neighbor_sample->second.position,
                                sample->second.position), direction))
            continue;
        for (std::int64_t dz = -2; dz <= 2; ++dz) {
            for (std::int64_t dy = -2; dy <= 2; ++dy) {
                for (std::int64_t dx = -2; dx <= 2; ++dx) {
                    EndpointKey nearby{};
                    if (!offset_endpoint_key(
                            endpoint.first, dx, dy, dz, nearby) ||
                        !(endpoint.first < nearby))
                        continue;
                    const auto nearby_degree = degree.find(nearby);
                    const auto nearby_sample = contour.points.find(nearby);
                    const auto nearby_neighbor = only_neighbor.find(nearby);
                    if (nearby_degree == degree.end() ||
                        nearby_degree->second != 1u ||
                        nearby_sample == contour.points.end() ||
                        nearby_neighbor == only_neighbor.end())
                        continue;
                    const double distance_squared = length_squared(subtract(
                        sample->second.position,
                        nearby_sample->second.position));
                    if (distance_squared > tolerance_squared) continue;
                    const auto nearby_neighbor_sample = contour.points.find(
                        nearby_neighbor->second);
                    Vector3d nearby_direction{};
                    if (nearby_neighbor_sample == contour.points.end() ||
                        !normalize(subtract(
                            nearby_neighbor_sample->second.position,
                            nearby_sample->second.position),
                            nearby_direction) ||
                        dot(direction, nearby_direction) > -0.995)
                        continue;
                    continuation_candidates.push_back(
                        {distance_squared, {endpoint.first, nearby}});
                }
            }
        }
    }
    std::sort(continuation_candidates.begin(),
              continuation_candidates.end());
    std::map<EndpointKey, bool> claimed;
    bool merged_continuation = false;
    for (const auto& candidate : continuation_candidates) {
        const EndpointKey first = candidate.second.first;
        const EndpointKey second = candidate.second.second;
        if (claimed[first] || claimed[second]) continue;
        EndpointKey first_root = root(first);
        EndpointKey second_root = root(second);
        if (first_root == second_root) continue;
        if (second_root < first_root) std::swap(first_root, second_root);
        parent[second_root] = first_root;
        claimed[first] = true;
        claimed[second] = true;
        merged_continuation = true;
    }
    return merged_continuation ? collect_unique() : normalized;
}

bool reduce_short_contour_edges(const CutContour& source,
                                double tolerance_m,
                                CutContour& reduced) {
    reduced = {};
    reduced.coplanar_triangles = source.coplanar_triangles;
    reduced.segments = normalized_segments(source, tolerance_m);
    for (const SegmentKey& segment : reduced.segments) {
        for (const EndpointKey key : {segment.first, segment.second}) {
            if (reduced.points.find(key) != reduced.points.end()) continue;
            const auto found = source.points.find(key);
            if (found == source.points.end()) return false;
            reduced.points.emplace(key, found->second);
        }
    }
    return !reduced.segments.empty() && !reduced.points.empty();
}

std::uint32_t unmatched_open_edges(const CutContour& first,
                                   const CutContour& second,
                                   double tolerance_m) {
    struct ComponentSignature {
        std::uint64_t cycle_rank = 0u;
        std::uint32_t endpoint_count = 0u;
        std::vector<std::uint32_t> branch_degrees;

        bool operator==(const ComponentSignature& other) const noexcept {
            return endpoint_count == other.endpoint_count &&
                   cycle_rank == other.cycle_rank &&
                   branch_degrees == other.branch_degrees;
        }
    };

    struct ParameterInterval {
        double first = 0.0;
        double second = 0.0;
    };
    const auto append_quadratic_interval = [](
        Vector3d offset, Vector3d direction, double range_first,
        double range_second, double tolerance_squared,
        std::vector<ParameterInterval>& intervals) {
        const double a = length_squared(direction);
        const double b = 2.0 * dot(offset, direction);
        const double c = length_squared(offset) - tolerance_squared;
        const double numeric_epsilon =
            64.0 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::fabs(a), std::fabs(b), std::fabs(c)});
        if (a <= numeric_epsilon) {
            if (c <= numeric_epsilon)
                intervals.push_back({range_first, range_second});
            return;
        }
        double discriminant = b * b - 4.0 * a * c;
        if (discriminant < -numeric_epsilon) return;
        discriminant = std::max(0.0, discriminant);
        const double root_distance = std::sqrt(discriminant);
        const double denominator = 2.0 * a;
        const double first_root = (-b - root_distance) / denominator;
        const double second_root = (-b + root_distance) / denominator;
        const double first = std::max(range_first, first_root);
        const double second = std::min(range_second, second_root);
        if (first <= second + numeric_epsilon)
            intervals.push_back({first, second});
    };

    const auto segment_covered = [&](const CutContour& source,
                                     const SegmentKey& source_segment,
                                     const CutContour& target) {
        const auto source_first = source.points.find(source_segment.first);
        const auto source_second = source.points.find(source_segment.second);
        if (source_first == source.points.end() ||
            source_second == source.points.end())
            return false;
        const Vector3d source_origin = source_first->second.position;
        const Vector3d source_direction = subtract(
            source_second->second.position, source_origin);
        if (length_squared(source_direction) <=
            std::numeric_limits<double>::epsilon())
            return false;

        // The tolerance region of a target segment is a capsule. Intersect
        // the source segment with every target capsule, then require their
        // parameter intervals to cover [0, 1]. This compares curve geometry
        // without requiring independently packed contours to share vertices.
        std::vector<ParameterInterval> coverage;
        const double tolerance_squared = tolerance_m * tolerance_m;
        for (const SegmentKey& target_segment : target.segments) {
            const auto target_first = target.points.find(target_segment.first);
            const auto target_second = target.points.find(target_segment.second);
            if (target_first == target.points.end() ||
                target_second == target.points.end())
                return false;
            const Vector3d target_origin = target_first->second.position;
            const Vector3d target_direction = subtract(
                target_second->second.position, target_origin);
            const double target_length_squared =
                length_squared(target_direction);
            if (target_length_squared <=
                std::numeric_limits<double>::epsilon())
                return false;
            const Vector3d relative = subtract(source_origin, target_origin);
            const double projection_origin =
                dot(relative, target_direction) / target_length_squared;
            const double projection_direction =
                dot(source_direction, target_direction) /
                target_length_squared;
            std::vector<double> partitions{0.0, 1.0};
            if (std::fabs(projection_direction) >
                std::numeric_limits<double>::epsilon()) {
                for (const double projection : {0.0, 1.0}) {
                    const double parameter =
                        (projection - projection_origin) /
                        projection_direction;
                    if (parameter > 0.0 && parameter < 1.0)
                        partitions.push_back(parameter);
                }
            }
            std::sort(partitions.begin(), partitions.end());
            partitions.erase(std::unique(partitions.begin(),
                                         partitions.end()),
                             partitions.end());
            for (std::size_t partition = 1u;
                 partition != partitions.size(); ++partition) {
                const double range_first = partitions[partition - 1u];
                const double range_second = partitions[partition];
                const double midpoint =
                    0.5 * (range_first + range_second);
                const double projection = projection_origin +
                    midpoint * projection_direction;
                Vector3d offset{};
                Vector3d direction{};
                if (projection <= 0.0) {
                    offset = relative;
                    direction = source_direction;
                } else if (projection >= 1.0) {
                    offset = subtract(relative, target_direction);
                    direction = source_direction;
                } else {
                    offset = subtract(
                        relative,
                        scale(target_direction, projection_origin));
                    direction = subtract(
                        source_direction,
                        scale(target_direction, projection_direction));
                }
                append_quadratic_interval(
                    offset, direction, range_first, range_second,
                    tolerance_squared, coverage);
            }
        }
        if (coverage.empty()) return false;
        std::sort(coverage.begin(), coverage.end(),
                  [](const ParameterInterval& a,
                     const ParameterInterval& b) {
                      if (a.first != b.first) return a.first < b.first;
                      return a.second < b.second;
                  });
        constexpr double kParameterEpsilon = 1.0e-10;
        if (coverage.front().first > kParameterEpsilon) return false;
        double covered_through = coverage.front().second;
        for (std::size_t index = 1u; index != coverage.size(); ++index) {
            if (coverage[index].first >
                covered_through + kParameterEpsilon)
                return false;
            covered_through = std::max(covered_through,
                                       coverage[index].second);
            if (covered_through >= 1.0 - kParameterEpsilon) return true;
        }
        return covered_through >= 1.0 - kParameterEpsilon;
    };

    struct ContourComponent {
        CutContour contour{};
        ComponentSignature signature{};
    };
    const auto split_components = [](
        const CutContour& contour,
        std::vector<ContourComponent>& components) {
        components.clear();
        std::vector<EndpointKey> keys;
        std::map<EndpointKey, std::size_t> index_by_key;
        for (const SegmentKey& segment : contour.segments) {
            for (const EndpointKey key : {segment.first, segment.second}) {
                if (index_by_key.find(key) != index_by_key.end()) continue;
                index_by_key.emplace(key, keys.size());
                keys.push_back(key);
            }
        }
        if (keys.empty()) return false;

        std::vector<std::size_t> parent(keys.size());
        for (std::size_t index = 0u; index != parent.size(); ++index)
            parent[index] = index;
        std::function<std::size_t(std::size_t)> root =
            [&](std::size_t index) {
                if (parent[index] == index) return index;
                parent[index] = root(parent[index]);
                return parent[index];
            };
        for (const SegmentKey& segment : contour.segments) {
            const auto first = index_by_key.find(segment.first);
            const auto second = index_by_key.find(segment.second);
            if (first == index_by_key.end() ||
                second == index_by_key.end() ||
                first->second == second->second)
                return false;
            std::size_t first_root = root(first->second);
            std::size_t second_root = root(second->second);
            if (first_root != second_root) parent[second_root] = first_root;
        }

        std::map<std::size_t, std::size_t> component_by_root;
        for (std::size_t index = 0u; index != keys.size(); ++index) {
            const std::size_t component_root = root(index);
            auto inserted = component_by_root.emplace(
                component_root, components.size());
            if (inserted.second) components.push_back({});
            const auto sample = contour.points.find(keys[index]);
            if (sample == contour.points.end()) return false;
            components[inserted.first->second].contour.points.emplace(
                keys[index], sample->second);
        }
        for (const SegmentKey& segment : contour.segments) {
            const auto first = index_by_key.find(segment.first);
            if (first == index_by_key.end()) return false;
            const auto component = component_by_root.find(
                root(first->second));
            if (component == component_by_root.end()) return false;
            components[component->second].contour.segments.push_back(
                segment);
        }
        for (ContourComponent& component : components) {
            std::map<EndpointKey, std::uint32_t> degree;
            for (const SegmentKey& segment : component.contour.segments) {
                ++degree[segment.first];
                ++degree[segment.second];
            }
            for (const auto& vertex : degree) {
                if (vertex.second == 1u) {
                    ++component.signature.endpoint_count;
                } else if (vertex.second > 2u) {
                    component.signature.branch_degrees.push_back(
                        vertex.second);
                }
            }
            const std::uint64_t edge_count =
                component.contour.segments.size();
            const std::uint64_t vertex_count =
                component.contour.points.size();
            if (edge_count + 1u < vertex_count) return false;
            component.signature.cycle_rank =
                edge_count + 1u - vertex_count;
            std::sort(
                component.signature.branch_degrees.begin(),
                component.signature.branch_degrees.end());
        }
        return !components.empty();
    };

    const auto components_cover_each_other = [&](
        const CutContour& first_component,
        const CutContour& second_component) {
        for (const SegmentKey& segment : first_component.segments) {
            if (!segment_covered(
                    first_component, segment, second_component))
                return false;
        }
        for (const SegmentKey& segment : second_component.segments) {
            if (!segment_covered(
                    second_component, segment, first_component))
                return false;
        }
        return true;
    };

    CutContour first_normalized{};
    CutContour second_normalized{};
    if (!reduce_short_contour_edges(
            first, tolerance_m, first_normalized) ||
        !reduce_short_contour_edges(
            second, tolerance_m, second_normalized))
        return 1u;

    std::vector<ContourComponent> first_components;
    std::vector<ContourComponent> second_components;
    if (!split_components(first_normalized, first_components) ||
        !split_components(second_normalized, second_components) ||
        first_components.size() != second_components.size())
        return 1u;

    std::vector<std::vector<std::size_t>> candidates(
        first_components.size());
    for (std::size_t first_index = 0u;
         first_index != first_components.size(); ++first_index) {
        for (std::size_t second_index = 0u;
             second_index != second_components.size(); ++second_index) {
            if (components_cover_each_other(
                    first_components[first_index].contour,
                    second_components[second_index].contour)) {
                candidates[first_index].push_back(second_index);
            }
        }
        std::stable_sort(
            candidates[first_index].begin(),
            candidates[first_index].end(),
            [&](std::size_t a, std::size_t b) {
                const bool a_exact =
                    first_components[first_index].signature ==
                    second_components[a].signature;
                const bool b_exact =
                    first_components[first_index].signature ==
                    second_components[b].signature;
                return a_exact != b_exact ? a_exact : a < b;
            });
    }

    std::vector<std::int64_t> matched_second(
        second_components.size(), -1);
    const auto augment = [&](auto&& self, std::size_t first_index,
                             std::vector<bool>& visited) -> bool {
        for (const std::size_t second_index : candidates[first_index]) {
            if (visited[second_index]) continue;
            visited[second_index] = true;
            if (matched_second[second_index] < 0 ||
                self(self,
                     static_cast<std::size_t>(
                         matched_second[second_index]),
                     visited)) {
                matched_second[second_index] =
                    static_cast<std::int64_t>(first_index);
                return true;
            }
        }
        return false;
    };
    for (std::size_t first_index = 0u;
         first_index != first_components.size(); ++first_index) {
        std::vector<bool> visited(second_components.size(), false);
        if (!augment(augment, first_index, visited))
            return 1u;
    }

    std::uint64_t unmatched = 0u;
    const auto accumulate_uncovered = [&](const CutContour& source,
                                          const CutContour& target) {
        for (const SegmentKey& segment : source.segments) {
            if (segment_covered(source, segment, target)) continue;
            ++unmatched;
        }
    };
    accumulate_uncovered(first_normalized, second_normalized);
    accumulate_uncovered(second_normalized, first_normalized);
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
                     double endpoint_tolerance_m,
                     WaterCutContourMetrics& metrics) {
    CutContour first_reduced{};
    CutContour second_reduced{};
    if (!reduce_short_contour_edges(
            first_contour, endpoint_tolerance_m, first_reduced) ||
        !reduce_short_contour_edges(
            second_contour, endpoint_tolerance_m, second_reduced))
        return false;
    if (first_reduced.points.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        second_reduced.points.size() >
            std::numeric_limits<std::uint32_t>::max())
        return false;
    metrics.first_points = static_cast<std::uint32_t>(
        first_reduced.points.size());
    metrics.second_points = static_cast<std::uint32_t>(
        second_reduced.points.size());
    metrics.first_height_quantiles_m = height_quantiles(first_reduced);
    metrics.second_height_quantiles_m = height_quantiles(second_reduced);
    metrics.unmatched_open_edges = unmatched_open_edges(
        first_reduced, second_reduced, endpoint_tolerance_m);
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
            first_reduced, second_reduced, maximum_squared_distance,
            sum_squared_distance, normal_dots, normal_angles) ||
        !accumulate_directed_metrics(
            second_reduced, first_reduced, maximum_squared_distance,
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

        if (!reduce_contours(
                first_contour, second_contour,
                static_cast<double>(edge_match_tolerance_m), metrics))
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
        if (!reduce_contours(
                first_contour, second_contour,
                static_cast<double>(edge_match_tolerance_m), metrics))
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
