#include "hydrology/hydrology_handoff_products.h"

#include "hydrology/river_presentation_field.h"
#include "hydrology/water_visual_products.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hydrology {
namespace {

constexpr float kGeometryEpsilon = 1.0e-5f;

bool finite(float value) { return std::isfinite(value); }

std::uint64_t mesh_payload_bytes(
    const gpu_meshing::MeshResult& mesh) noexcept {
    constexpr std::uint64_t kFloatBytes = sizeof(float);
    constexpr std::uint64_t kIndexBytes = sizeof(std::uint32_t);
    const std::uint64_t position_bytes =
        static_cast<std::uint64_t>(mesh.positions.size()) * kFloatBytes;
    const std::uint64_t normal_bytes =
        static_cast<std::uint64_t>(mesh.normals.size()) * kFloatBytes;
    const std::uint64_t index_bytes =
        static_cast<std::uint64_t>(mesh.indices.size()) * kIndexBytes;
    if (position_bytes > UINT64_MAX - normal_bytes ||
        position_bytes + normal_bytes > UINT64_MAX - index_bytes)
        return UINT64_MAX;
    return position_bytes + normal_bytes + index_bytes;
}

std::uint64_t animation_mesh_payload_bytes(
    const WaterMeshAnimation& animation) noexcept {
    std::uint64_t total = 0u;
    for (const auto& frame : animation.frames) {
        const std::uint64_t bytes = mesh_payload_bytes(frame);
        if (bytes == UINT64_MAX || total > UINT64_MAX - bytes)
            return UINT64_MAX;
        total += bytes;
    }
    return total;
}

std::uint64_t artifact_heap_payload_bytes(
    const WaterMeshAnimationArtifact& artifact) noexcept {
    const std::uint64_t identity = artifact.identity.size();
    const std::uint64_t records =
        static_cast<std::uint64_t>(artifact.frames.size()) *
        sizeof(WaterMeshAnimationFrameRecord);
    const std::uint64_t payload = artifact.frame_payload.size();
    if (identity > UINT64_MAX - records ||
        identity + records > UINT64_MAX - payload)
        return UINT64_MAX;
    return identity + records + payload;
}

std::uint64_t saturating_payload_sum(
    std::initializer_list<std::uint64_t> values) noexcept {
    std::uint64_t total = 0u;
    for (const std::uint64_t value : values) {
        if (value == UINT64_MAX || total > UINT64_MAX - value)
            return UINT64_MAX;
        total += value;
    }
    return total;
}

bool finite(matter::Float3 value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

matter::Float3 add(matter::Float3 a, matter::Float3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

matter::Float3 subtract(matter::Float3 a, matter::Float3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

matter::Float3 scale(matter::Float3 value, float factor) {
    return {value.x * factor, value.y * factor, value.z * factor};
}

float dot(matter::Float3 a, matter::Float3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

matter::Float3 cross(matter::Float3 a, matter::Float3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

float length_squared(matter::Float3 value) { return dot(value, value); }

float signed_distance(const SpillwayHandoffRecord& handoff,
                      matter::Float3 point) {
    return dot(subtract(point, handoff.lip_origin_m), handoff.tangent);
}

float lateral_distance(const SpillwayHandoffRecord& handoff,
                       matter::Float3 point) {
    return dot(subtract(point, handoff.lip_origin_m), handoff.lateral);
}

bool valid_bounds(const matter::Aabb& bounds) {
    return finite(bounds.minimum) && finite(bounds.maximum) &&
           bounds.minimum.x <= bounds.maximum.x &&
           bounds.minimum.y <= bounds.maximum.y &&
           bounds.minimum.z <= bounds.maximum.z;
}

bool valid_mesh(const gpu_meshing::MeshResult& mesh, bool allow_empty = false) {
    if (mesh.positions.size() != mesh.normals.size() ||
        mesh.positions.size() % 3u != 0u ||
        mesh.indices.size() % 3u != 0u || mesh.material != 4u)
        return false;
    if (!allow_empty && (mesh.positions.empty() || mesh.indices.empty()))
        return false;
    for (const float value : mesh.positions)
        if (!finite(value)) return false;
    for (const float value : mesh.normals)
        if (!finite(value)) return false;
    const std::size_t vertex_count = mesh.positions.size() / 3u;
    for (const auto index : mesh.indices)
        if (index >= vertex_count) return false;
    return mesh.content_digest == gpu_meshing::mesh_content_digest(mesh);
}

bool fail(const char* message, FluidBakeError& error) {
    error = {FluidBakeCode::ProductFailure, message};
    return false;
}

class Digest {
public:
    explicit Digest(std::uint64_t domain) { u64(domain); }
    void byte(std::uint8_t value) {
        value_ ^= value;
        value_ *= UINT64_C(1099511628211);
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8)
            byte(static_cast<std::uint8_t>(value >> shift));
    }
    void floating(float value) {
        std::uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(bits));
        for (unsigned shift = 0; shift != 32; shift += 8)
            byte(static_cast<std::uint8_t>(bits >> shift));
    }
    std::uint64_t finish() const { return value_ == 0u ? 1u : value_; }

private:
    std::uint64_t value_ = UINT64_C(1469598103934665603);
};

struct WeldKey {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
    bool operator==(const WeldKey& other) const noexcept {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct WeldKeyHash {
    std::size_t operator()(const WeldKey& value) const noexcept {
        std::size_t hash = static_cast<std::size_t>(value.x);
        hash ^= static_cast<std::size_t>(value.y) +
                UINT64_C(0x9e3779b97f4a7c15) + (hash << 6u) + (hash >> 2u);
        hash ^= static_cast<std::size_t>(value.z) +
                UINT64_C(0x9e3779b97f4a7c15) + (hash << 6u) + (hash >> 2u);
        return hash;
    }
};

class MeshAssembler {
public:
    explicit MeshAssembler(float tolerance)
        : tolerance_(std::max(tolerance, 1.0e-6f)) {
        mesh_.material = 4u;
    }

    void triangle(matter::Float3 a, matter::Float3 b, matter::Float3 c) {
        if (length_squared(cross(subtract(b, a), subtract(c, a))) <=
            kGeometryEpsilon * kGeometryEpsilon)
            return;
        const std::uint32_t ia = vertex(a);
        const std::uint32_t ib = vertex(b);
        const std::uint32_t ic = vertex(c);
        if (ia == ib || ib == ic || ia == ic) return;
        mesh_.indices.insert(mesh_.indices.end(), {ia, ib, ic});
    }

    gpu_meshing::MeshResult finish() {
        mesh_.normals.clear();
        if (!repair_nonfinite_cpu_mesh_normals(mesh_)) return {};
        mesh_.content_digest = gpu_meshing::mesh_content_digest(mesh_);
        return std::move(mesh_);
    }

private:
    WeldKey key(matter::Float3 point) const {
        return {static_cast<std::int64_t>(std::llround(point.x / tolerance_)),
                static_cast<std::int64_t>(std::llround(point.y / tolerance_)),
                static_cast<std::int64_t>(std::llround(point.z / tolerance_))};
    }

    std::uint32_t vertex(matter::Float3 point) {
        const WeldKey weld = key(point);
        const auto found = vertices_.find(weld);
        if (found != vertices_.end()) return found->second;
        const auto index = static_cast<std::uint32_t>(
            mesh_.positions.size() / 3u);
        mesh_.positions.insert(mesh_.positions.end(),
                               {point.x, point.y, point.z});
        vertices_.emplace(weld, index);
        return index;
    }

    float tolerance_ = 0.0f;
    gpu_meshing::MeshResult mesh_{};
    std::unordered_map<WeldKey, std::uint32_t, WeldKeyHash> vertices_;
};

matter::Float3 mesh_point(const gpu_meshing::MeshResult& mesh,
                         std::uint32_t index) {
    return {mesh.positions[index * 3u + 0u],
            mesh.positions[index * 3u + 1u],
            mesh.positions[index * 3u + 2u]};
}

std::vector<matter::Float3> clip_plane(
    const std::vector<matter::Float3>& input,
    const SpillwayHandoffRecord& handoff,
    float boundary, bool keep_less) {
    std::vector<matter::Float3> output;
    if (input.empty()) return output;
    const auto accepted = [&](float value) {
        return keep_less ? value <= boundary + kGeometryEpsilon
                         : value >= boundary - kGeometryEpsilon;
    };
    matter::Float3 previous = input.back();
    float previous_distance = signed_distance(handoff, previous);
    bool previous_inside = accepted(previous_distance);
    for (const matter::Float3 current : input) {
        const float current_distance = signed_distance(handoff, current);
        const bool current_inside = accepted(current_distance);
        if (current_inside != previous_inside) {
            const float denominator = current_distance - previous_distance;
            const float t = std::fabs(denominator) > kGeometryEpsilon
                ? std::clamp((boundary - previous_distance) / denominator,
                             0.0f, 1.0f)
                : 0.0f;
            output.push_back(add(previous,
                                 scale(subtract(current, previous), t)));
        }
        if (current_inside) output.push_back(current);
        previous = current;
        previous_distance = current_distance;
        previous_inside = current_inside;
    }
    return output;
}

bool append_clipped_mesh(const gpu_meshing::MeshResult& source,
                         const SpillwayHandoffRecord& handoff,
                         bool has_lower, float lower,
                         bool has_upper, float upper,
                         MeshAssembler& output) {
    if (!valid_mesh(source)) return false;
    for (std::size_t triangle = 0; triangle < source.indices.size();
         triangle += 3u) {
        std::vector<matter::Float3> polygon = {
            mesh_point(source, source.indices[triangle + 0u]),
            mesh_point(source, source.indices[triangle + 1u]),
            mesh_point(source, source.indices[triangle + 2u])};
        if (has_lower)
            polygon = clip_plane(polygon, handoff, lower, false);
        if (has_upper)
            polygon = clip_plane(polygon, handoff, upper, true);
        for (std::size_t index = 1u; index + 1u < polygon.size(); ++index)
            output.triangle(polygon[0], polygon[index], polygon[index + 1u]);
    }
    return true;
}

bool append_mesh(const gpu_meshing::MeshResult& source,
                 MeshAssembler& output) {
    if (!valid_mesh(source)) return false;
    for (std::size_t triangle = 0u; triangle < source.indices.size();
         triangle += 3u) {
        output.triangle(mesh_point(source, source.indices[triangle + 0u]),
                        mesh_point(source, source.indices[triangle + 1u]),
                        mesh_point(source, source.indices[triangle + 2u]));
    }
    return true;
}

struct CutPath {
    std::vector<matter::Float3> points;
    bool closed = false;
};

std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<std::uint64_t>(a) << 32u) | b;
}

std::uint32_t edge_first(std::uint64_t edge) {
    return static_cast<std::uint32_t>(edge >> 32u);
}

std::uint32_t edge_second(std::uint64_t edge) {
    return static_cast<std::uint32_t>(edge);
}

bool cut_boundary_paths(const gpu_meshing::MeshResult& mesh,
                        const SpillwayHandoffRecord& handoff,
                        float cut,
                        std::vector<CutPath>& paths,
                        std::string& diagnostic) {
    paths.clear();
    diagnostic.clear();
    constexpr float kCutTolerance = 1.0e-3f;
    std::unordered_map<std::uint64_t, std::uint32_t> edge_counts;
    for (std::size_t index = 0u; index < mesh.indices.size(); index += 3u) {
        const std::uint32_t triangle[] = {
            mesh.indices[index + 0u], mesh.indices[index + 1u],
            mesh.indices[index + 2u]};
        for (std::size_t edge = 0u; edge != 3u; ++edge) {
            const auto a = triangle[edge];
            const auto b = triangle[(edge + 1u) % 3u];
            if (std::fabs(signed_distance(handoff, mesh_point(mesh, a)) - cut) <=
                    kCutTolerance &&
                std::fabs(signed_distance(handoff, mesh_point(mesh, b)) - cut) <=
                    kCutTolerance)
                ++edge_counts[edge_key(a, b)];
        }
    }

    std::unordered_set<std::uint64_t> remaining;
    std::vector<std::vector<std::uint32_t>> adjacency(
        mesh.positions.size() / 3u);
    for (const auto& [edge, count] : edge_counts) {
        if (count % 2u == 0u) continue;
        const auto a = edge_first(edge);
        const auto b = edge_second(edge);
        remaining.insert(edge);
        adjacency[a].push_back(b);
        adjacency[b].push_back(a);
    }
    if (remaining.empty()) {
        float minimum_distance = std::numeric_limits<float>::infinity();
        float maximum_distance = -std::numeric_limits<float>::infinity();
        for (std::uint32_t index = 0u;
             index != mesh.positions.size() / 3u; ++index) {
            const float distance = signed_distance(
                handoff, mesh_point(mesh, index));
            minimum_distance = std::min(minimum_distance, distance);
            maximum_distance = std::max(maximum_distance, distance);
        }
        diagnostic = "has no boundary edges on the ownership plane "
            "(signed range " + std::to_string(minimum_distance) + " to " +
            std::to_string(maximum_distance) + ", cut " +
            std::to_string(cut) + ")";
        return false;
    }
    while (!remaining.empty()) {
        const auto seed = *std::min_element(
            remaining.begin(), remaining.end());
        std::vector<std::uint32_t> component;
        std::vector<std::uint32_t> stack{edge_first(seed)};
        std::unordered_set<std::uint32_t> seen;
        while (!stack.empty()) {
            const auto vertex = stack.back();
            stack.pop_back();
            if (!seen.insert(vertex).second) continue;
            component.push_back(vertex);
            for (const auto neighbour : adjacency[vertex]) {
                if (remaining.count(edge_key(vertex, neighbour)) != 0u)
                    stack.push_back(neighbour);
            }
        }
        std::uint32_t start = *std::min_element(
            component.begin(), component.end());
        bool have_endpoint = false;
        for (const auto vertex : component) {
            std::size_t unused_degree = 0u;
            for (const auto neighbour : adjacency[vertex]) {
                if (remaining.count(edge_key(vertex, neighbour)) != 0u)
                    ++unused_degree;
            }
            if (unused_degree == 1u &&
                (!have_endpoint || vertex < start)) {
                start = vertex;
                have_endpoint = true;
            }
        }

        CutPath path{};
        std::uint32_t current = start;
        while (true) {
            path.points.push_back(mesh_point(mesh, current));
            std::uint32_t next = std::numeric_limits<std::uint32_t>::max();
            for (const auto neighbour : adjacency[current]) {
                const auto edge = edge_key(current, neighbour);
                if (remaining.count(edge) != 0u && neighbour < next)
                    next = neighbour;
            }
            if (next == std::numeric_limits<std::uint32_t>::max()) break;
            remaining.erase(edge_key(current, next));
            current = next;
            if (current == start) {
                path.closed = true;
                break;
            }
        }
        if (path.points.size() >= 2u) paths.push_back(std::move(path));
    }
    if (paths.empty()) {
        diagnostic = "has no traceable cut contours";
        return false;
    }
    return true;
}

matter::Float3 path_centroid(const CutPath& path) {
    matter::Float3 result{};
    for (const auto point : path.points) result = add(result, point);
    return scale(result, 1.0f / static_cast<float>(path.points.size()));
}

matter::Float3 sample_path(const CutPath& path, float fraction) {
    const std::size_t segment_count = path.closed
        ? path.points.size() : path.points.size() - 1u;
    std::vector<float> lengths(segment_count, 0.0f);
    float total = 0.0f;
    for (std::size_t index = 0u; index != segment_count; ++index) {
        lengths[index] = std::sqrt(length_squared(subtract(
            path.points[(index + 1u) % path.points.size()],
            path.points[index])));
        total += lengths[index];
    }
    if (total <= kGeometryEpsilon) return path.points.front();
    float target = std::clamp(fraction, 0.0f, 1.0f) * total;
    for (std::size_t index = 0u; index != segment_count; ++index) {
        if (target <= lengths[index] || index + 1u == segment_count) {
            const float t = lengths[index] > kGeometryEpsilon
                ? std::clamp(target / lengths[index], 0.0f, 1.0f) : 0.0f;
            const auto& a = path.points[index];
            const auto& b = path.points[(index + 1u) % path.points.size()];
            return add(a, scale(subtract(b, a), t));
        }
        target -= lengths[index];
    }
    return path.points.back();
}

void reverse_closed_path(CutPath& path) {
    if (path.points.size() < 2u) return;
    std::reverse(path.points.begin() + 1u, path.points.end());
}

void align_paths(CutPath& a, CutPath& b) {
    if (!a.closed) {
        const float straight = length_squared(subtract(a.points.front(),
                                                       b.points.front())) +
            length_squared(subtract(a.points.back(), b.points.back()));
        const float crossed = length_squared(subtract(a.points.front(),
                                                      b.points.back())) +
            length_squared(subtract(a.points.back(), b.points.front()));
        if (crossed < straight) std::reverse(b.points.begin(), b.points.end());
        return;
    }

    std::size_t best_a = 0u;
    std::size_t best_b = 0u;
    float best = std::numeric_limits<float>::infinity();
    for (std::size_t ia = 0u; ia != a.points.size(); ++ia) {
        for (std::size_t ib = 0u; ib != b.points.size(); ++ib) {
            const float distance = length_squared(
                subtract(a.points[ia], b.points[ib]));
            if (distance < best) {
                best = distance;
                best_a = ia;
                best_b = ib;
            }
        }
    }
    std::rotate(a.points.begin(), a.points.begin() + best_a, a.points.end());
    std::rotate(b.points.begin(), b.points.begin() + best_b, b.points.end());
    const float forward = length_squared(subtract(a.points[1u], b.points[1u]));
    const float reverse = length_squared(subtract(
        a.points[1u], b.points.back()));
    if (reverse < forward) reverse_closed_path(b);
}

void append_path_stitch(CutPath first_path, CutPath second_path,
                        MeshAssembler& output) {
    align_paths(first_path, second_path);
    const std::size_t samples = std::max(first_path.points.size(),
                                         second_path.points.size());
    const std::size_t segments = first_path.closed ? samples : samples - 1u;
    std::vector<matter::Float3> a(samples);
    std::vector<matter::Float3> b(samples);
    for (std::size_t index = 0u; index != samples; ++index) {
        const float denominator = static_cast<float>(
            first_path.closed ? samples : samples - 1u);
        const float fraction = static_cast<float>(index) / denominator;
        a[index] = sample_path(first_path, fraction);
        b[index] = sample_path(second_path, fraction);
    }
    for (std::size_t index = 0u; index != segments; ++index) {
        const std::size_t next = (index + 1u) % samples;
        output.triangle(a[index], a[next], b[next]);
        output.triangle(a[index], b[next], b[index]);
    }
}

CutPath merge_open_paths(std::vector<CutPath> paths,
                         const SpillwayHandoffRecord& handoff) {
    std::size_t first = 0u;
    float minimum_endpoint = std::numeric_limits<float>::infinity();
    bool reverse_first = false;
    for (std::size_t index = 0u; index != paths.size(); ++index) {
        const float front = lateral_distance(handoff, paths[index].points.front());
        const float back = lateral_distance(handoff, paths[index].points.back());
        if (front < minimum_endpoint) {
            minimum_endpoint = front;
            first = index;
            reverse_first = false;
        }
        if (back < minimum_endpoint) {
            minimum_endpoint = back;
            first = index;
            reverse_first = true;
        }
    }
    CutPath result = std::move(paths[first]);
    paths.erase(paths.begin() + static_cast<std::ptrdiff_t>(first));
    if (reverse_first)
        std::reverse(result.points.begin(), result.points.end());
    result.closed = false;

    while (!paths.empty()) {
        std::size_t nearest_path = 0u;
        bool reverse_path = false;
        float nearest = std::numeric_limits<float>::infinity();
        for (std::size_t index = 0u; index != paths.size(); ++index) {
            const float front = length_squared(subtract(
                result.points.back(), paths[index].points.front()));
            const float back = length_squared(subtract(
                result.points.back(), paths[index].points.back()));
            if (front < nearest) {
                nearest = front;
                nearest_path = index;
                reverse_path = false;
            }
            if (back < nearest) {
                nearest = back;
                nearest_path = index;
                reverse_path = true;
            }
        }
        CutPath next = std::move(paths[nearest_path]);
        paths.erase(paths.begin() +
                    static_cast<std::ptrdiff_t>(nearest_path));
        if (reverse_path) std::reverse(next.points.begin(), next.points.end());
        result.points.insert(result.points.end(), next.points.begin(),
                             next.points.end());
    }
    return result;
}

void append_closed_cap(const CutPath& path, MeshAssembler& output) {
    const auto centre = path_centroid(path);
    for (std::size_t index = 0u; index != path.points.size(); ++index)
        output.triangle(centre, path.points[index],
                        path.points[(index + 1u) % path.points.size()]);
}

bool append_cut_bridge(const gpu_meshing::MeshResult& first,
                       float first_cut,
                       const gpu_meshing::MeshResult& second,
                       float second_cut,
                       const SpillwayHandoffRecord& handoff,
                       MeshAssembler& output,
                       std::string& diagnostic) {
    diagnostic.clear();
    std::vector<CutPath> first_paths;
    std::vector<CutPath> second_paths;
    std::string first_diagnostic;
    std::string second_diagnostic;
    const bool first_valid = cut_boundary_paths(
        first, handoff, first_cut, first_paths, first_diagnostic);
    const bool second_valid = cut_boundary_paths(
        second, handoff, second_cut, second_paths, second_diagnostic);
    if (!first_valid || !second_valid) {
        diagnostic = !first_valid
            ? "first mesh " + first_diagnostic
            : "second mesh " + second_diagnostic;
        return false;
    }
    std::vector<CutPath> first_open;
    std::vector<CutPath> second_open;
    std::vector<CutPath> first_closed;
    std::vector<CutPath> second_closed;
    for (auto& path : first_paths)
        (path.closed ? first_closed : first_open).push_back(std::move(path));
    for (auto& path : second_paths)
        (path.closed ? second_closed : second_open).push_back(std::move(path));

    if (first_open.empty() != second_open.empty()) {
        diagnostic = "main cut contours do not exist on both ownership meshes";
        return false;
    }
    if (!first_open.empty()) {
        append_path_stitch(merge_open_paths(std::move(first_open), handoff),
                           merge_open_paths(std::move(second_open), handoff),
                           output);
    }

    std::vector<bool> consumed(second_closed.size(), false);
    for (auto first_path : first_closed) {
        std::size_t match = second_closed.size();
        float nearest = std::numeric_limits<float>::infinity();
        const auto first_centre = path_centroid(first_path);
        for (std::size_t index = 0u; index != second_closed.size(); ++index) {
            if (consumed[index]) continue;
            const float distance = length_squared(
                subtract(first_centre, path_centroid(second_closed[index])));
            if (distance < nearest) {
                nearest = distance;
                match = index;
            }
        }
        if (match == second_closed.size()) {
            append_closed_cap(first_path, output);
            continue;
        }
        consumed[match] = true;
        append_path_stitch(std::move(first_path), second_closed[match], output);
    }
    for (std::size_t index = 0u; index != second_closed.size(); ++index)
        if (!consumed[index]) append_closed_cap(second_closed[index], output);
    return true;
}

bool append_cut_stitches(const gpu_meshing::MeshResult& first,
                         const gpu_meshing::MeshResult& second,
                         const SpillwayHandoffRecord& handoff,
                         float cut,
                         MeshAssembler& output,
                         std::string& diagnostic) {
    return append_cut_bridge(first, cut, second, cut, handoff, output,
                             diagnostic);
}

bool valid_layout(const GameplayFieldLayout& layout) {
    return finite(layout.origin_m) && finite(layout.cell_size_m) &&
           layout.cell_size_m > 0.0f && layout.width != 0u &&
           layout.depth != 0u &&
           static_cast<std::uint64_t>(layout.width) * layout.depth <=
               16ull * 1024ull * 1024ull;
}

GameplaySample blend(GameplaySample upstream, GameplaySample downstream,
                     float t) {
    const auto linear = [t](float a, float b) { return a + (b - a) * t; };
    return {linear(upstream.height_m, downstream.height_m),
            linear(upstream.depth_m, downstream.depth_m),
            linear(upstream.velocity_x_mps, downstream.velocity_x_mps),
            linear(upstream.velocity_y_mps, downstream.velocity_y_mps),
            linear(upstream.velocity_z_mps, downstream.velocity_z_mps),
            true};
}

PresentationSample blend(PresentationSample upstream,
                         PresentationSample downstream, float t) {
    const auto linear = [t](float a, float b) { return a + (b - a) * t; };
    const float upstream_y = std::sqrt(std::max(
        0.0f, 1.0f - upstream.normal_x * upstream.normal_x -
            upstream.normal_z * upstream.normal_z));
    const float downstream_y = std::sqrt(std::max(
        0.0f, 1.0f - downstream.normal_x * downstream.normal_x -
            downstream.normal_z * downstream.normal_z));
    matter::Float3 normal{
        linear(upstream.normal_x, downstream.normal_x),
        linear(upstream_y, downstream_y),
        linear(upstream.normal_z, downstream.normal_z)};
    const float inverse_length = 1.0f / std::sqrt(length_squared(normal));
    normal = scale(normal, inverse_length);
    return {normal.x,
            normal.z,
            linear(upstream.turbulence, downstream.turbulence),
            linear(upstream.aeration, downstream.aeration),
            linear(upstream.foam_potential, downstream.foam_potential),
            t < 0.5f ? upstream.feature : downstream.feature,
            true};
}

bool build_gameplay(const HydrologyArtifact& upstream,
                    const HydrologyArtifact& downstream,
                    const SpillwayHandoffRecord& handoff,
                    const GameplayFieldLayout& layout,
                    std::vector<GameplaySample>& field) {
    if (!valid_layout(layout)) return false;
    field.assign(static_cast<std::size_t>(layout.width) * layout.depth, {});
    for (std::uint32_t z = 0u; z != layout.depth; ++z) {
        for (std::uint32_t x = 0u; x != layout.width; ++x) {
            const matter::Float3 point{
                layout.origin_m.x + (static_cast<float>(x) + 0.5f) *
                    layout.cell_size_m,
                handoff.lip_origin_m.y,
                layout.origin_m.z + (static_cast<float>(z) + 0.5f) *
                    layout.cell_size_m};
            const float distance = signed_distance(handoff, point);
            GameplaySample before{};
            GameplaySample after{};
            const bool has_before = sample_fluid_gameplay_field(
                upstream.gameplay_layout, upstream.gameplay_field,
                point.x, point.z, before);
            const bool has_after = sample_fluid_gameplay_field(
                downstream.gameplay_layout, downstream.gameplay_field,
                point.x, point.z, after);
            GameplaySample sample{};
            if (distance < -handoff.overlap_m) {
                if (has_before) sample = before;
            } else if (distance > handoff.overlap_m) {
                if (has_after) sample = after;
            } else if (has_before && has_after) {
                const float t = std::clamp(
                    0.5f + distance / (2.0f * handoff.overlap_m),
                    0.0f, 1.0f);
                sample = blend(before, after, t);
            } else if (has_before) {
                sample = before;
            } else if (has_after) {
                sample = after;
            }
            field[static_cast<std::size_t>(z) * layout.width + x] = sample;
        }
    }
    return true;
}

bool build_presentation(const HydrologyArtifact& upstream,
                        const HydrologyArtifact& downstream,
                        const SpillwayHandoffRecord& handoff,
                        const GameplayFieldLayout& layout,
                        const std::vector<GameplaySample>& gameplay,
                        std::vector<PresentationSample>& field) {
    if (!valid_layout(layout) || gameplay.size() !=
            static_cast<std::size_t>(layout.width) * layout.depth)
        return false;
    field.assign(gameplay.size(), {});
    for (std::uint32_t z = 0u; z != layout.depth; ++z) {
        for (std::uint32_t x = 0u; x != layout.width; ++x) {
            const matter::Float3 point{
                layout.origin_m.x + (static_cast<float>(x) + 0.5f) *
                    layout.cell_size_m,
                handoff.lip_origin_m.y,
                layout.origin_m.z + (static_cast<float>(z) + 0.5f) *
                    layout.cell_size_m};
            const float distance = signed_distance(handoff, point);
            PresentationSample before{};
            PresentationSample after{};
            const bool has_before = sample_river_presentation_field(
                upstream.gameplay_layout, upstream.presentation_field,
                point.x, point.z, before);
            const bool has_after = sample_river_presentation_field(
                downstream.gameplay_layout, downstream.presentation_field,
                point.x, point.z, after);
            PresentationSample sample{};
            if (distance < -handoff.overlap_m) {
                if (has_before) sample = before;
            } else if (distance > handoff.overlap_m) {
                if (has_after) sample = after;
            } else if (has_before && has_after) {
                const float t = std::clamp(
                    0.5f + distance / (2.0f * handoff.overlap_m),
                    0.0f, 1.0f);
                sample = blend(before, after, t);
            } else if (has_before) {
                sample = before;
            } else if (has_after) {
                sample = after;
            }
            const auto index = static_cast<std::size_t>(z) * layout.width + x;
            if (sample.wet_valid != gameplay[index].wet_valid) return false;
            field[index] = sample;
        }
    }
    return true;
}

std::uint64_t handoff_semantic_key(
    const HydrologyArtifact& upstream,
    const HydrologyArtifact& downstream,
    const SpillwayHandoffRecord& handoff) {
    Digest digest(UINT64_C(0x48414e444f464631));
    digest.u64(handoff.semantic_key);
    digest.u64(upstream.semantic_key);
    digest.u64(downstream.semantic_key);
    digest.u64(upstream.payload_digest);
    digest.u64(downstream.payload_digest);
    return digest.finish();
}

std::uint64_t handoff_artifact_digest(
    const HydrologyHandoffArtifact& artifact) {
    Digest digest(UINT64_C(0x48414e4450524f44));
    digest.u64(artifact.semantic_key);
    digest.u64(artifact.upstream_payload_digest);
    digest.u64(artifact.downstream_payload_digest);
    digest.u64(artifact.visual_mesh.content_digest);
    return digest.finish();
}

bool valid_sample(const GameplaySample& sample) {
    if (!sample.wet_valid) return sample == GameplaySample{};
    return finite(sample.height_m) && finite(sample.depth_m) &&
           finite(sample.velocity_x_mps) &&
           finite(sample.velocity_y_mps) &&
           finite(sample.velocity_z_mps) && sample.depth_m >= 0.0f;
}

bool valid_sample(const PresentationSample& sample) {
    if (!sample.wet_valid) return sample == PresentationSample{};
    const float normal_y_squared = 1.0f - sample.normal_x * sample.normal_x -
        sample.normal_z * sample.normal_z;
    return finite(sample.normal_x) && finite(sample.normal_z) &&
           finite(sample.turbulence) && finite(sample.aeration) &&
           finite(sample.foam_potential) && normal_y_squared > 0.0f &&
           sample.turbulence >= 0.0f && sample.turbulence <= 1.0f &&
           sample.aeration >= 0.0f && sample.aeration <= 1.0f &&
           sample.foam_potential >= 0.0f && sample.foam_potential <= 1.0f &&
           static_cast<std::uint8_t>(sample.feature) <=
               static_cast<std::uint8_t>(RiverFeature::Pool);
}

bool has_open_cut_edge(const gpu_meshing::MeshResult& mesh,
                       const SpillwayHandoffRecord& handoff) {
    const float tolerance = 1.0e-3f;
    const auto cut_is_open = [&](float cut) {
        std::vector<std::pair<float, float>> intervals;
        std::vector<std::pair<float, float>> stitched_intervals;
        std::vector<float> breaks;
        for (std::size_t i = 0u; i < mesh.indices.size(); i += 3u) {
            const std::uint32_t vertices[] = {
                mesh.indices[i], mesh.indices[i + 1u], mesh.indices[i + 2u]};
            const auto p0 = mesh_point(mesh, vertices[0u]);
            const auto p1 = mesh_point(mesh, vertices[1u]);
            const auto p2 = mesh_point(mesh, vertices[2u]);
            if (std::fabs(signed_distance(handoff, p0) - cut) <= tolerance &&
                std::fabs(signed_distance(handoff, p1) - cut) <= tolerance &&
                std::fabs(signed_distance(handoff, p2) - cut) <= tolerance) {
                const float lateral[] = {
                    lateral_distance(handoff, p0),
                    lateral_distance(handoff, p1),
                    lateral_distance(handoff, p2)};
                const float vertical[] = {
                    dot(subtract(p0, handoff.lip_origin_m), handoff.up),
                    dot(subtract(p1, handoff.lip_origin_m), handoff.up),
                    dot(subtract(p2, handoff.lip_origin_m), handoff.up)};
                const float twice_area = std::fabs(
                    (lateral[1] - lateral[0]) *
                        (vertical[2] - vertical[0]) -
                    (vertical[1] - vertical[0]) *
                        (lateral[2] - lateral[0]));
                if (twice_area > tolerance * tolerance) {
                    stitched_intervals.emplace_back(
                        std::min({lateral[0], lateral[1], lateral[2]}),
                        std::max({lateral[0], lateral[1], lateral[2]}));
                }
            }
            for (std::size_t edge = 0u; edge != 3u; ++edge) {
                const auto a = mesh_point(mesh, vertices[edge]);
                const auto b = mesh_point(mesh, vertices[(edge + 1u) % 3u]);
                if (std::fabs(signed_distance(handoff, a) - cut) > tolerance ||
                    std::fabs(signed_distance(handoff, b) - cut) > tolerance)
                    continue;
                float first = lateral_distance(handoff, a);
                float second = lateral_distance(handoff, b);
                if (first > second) std::swap(first, second);
                first = std::max(first, -handoff.width_m * 0.5f);
                second = std::min(second, handoff.width_m * 0.5f);
                if (second - first <= tolerance) continue;
                intervals.emplace_back(first, second);
                breaks.push_back(first);
                breaks.push_back(second);
            }
        }
        if (intervals.empty()) return true;
        std::sort(breaks.begin(), breaks.end());
        breaks.erase(std::unique(breaks.begin(), breaks.end(),
                                 [=](float a, float b) {
                                     return std::fabs(a - b) <= tolerance;
                                 }),
                     breaks.end());
        for (std::size_t index = 0u; index + 1u < breaks.size(); ++index) {
            if (breaks[index + 1u] - breaks[index] <= tolerance) continue;
            const float midpoint = (breaks[index] + breaks[index + 1u]) * 0.5f;
            std::uint32_t coverage = 0u;
            for (const auto& interval : intervals)
                if (midpoint >= interval.first - tolerance &&
                    midpoint <= interval.second + tolerance)
                    ++coverage;
            if (coverage >= 2u && coverage % 2u == 0u) continue;
            bool stitched = false;
            for (const auto& interval : stitched_intervals)
                if (midpoint >= interval.first - tolerance &&
                    midpoint <= interval.second + tolerance) {
                    stitched = true;
                    break;
                }
            if (!stitched) return true;
        }
        return false;
    };
    if (cut_is_open(handoff.upstream_visual_cut_m) ||
        cut_is_open(handoff.downstream_visual_cut_m))
        return true;
    return false;
}

bool duplicate_cpu_triangle(const gpu_meshing::MeshResult& mesh) {
    std::unordered_set<std::string> triangles;
    for (std::size_t i = 0u; i < mesh.indices.size(); i += 3u) {
        std::array<std::string, 3> vertices;
        for (std::size_t corner = 0u; corner != 3u; ++corner) {
            const auto point = mesh_point(mesh, mesh.indices[i + corner]);
            vertices[corner] =
                std::to_string(std::llround(point.x * 100000.0f)) + "," +
                std::to_string(std::llround(point.y * 100000.0f)) + "," +
                std::to_string(std::llround(point.z * 100000.0f));
        }
        std::sort(vertices.begin(), vertices.end());
        const std::string key =
            vertices[0] + ";" + vertices[1] + ";" + vertices[2];
        if (!triangles.insert(key).second) return true;
    }
    return false;
}

constexpr std::uint8_t kHandoffMagic[8] = {
    'M', 'H', 'Y', 'D', 'H', 'O', 'F', '1'};
constexpr std::uint32_t kHandoffVersion = 2u;
constexpr std::size_t kHandoffHeaderBytes = 28u;
constexpr std::uint64_t kMaximumHandoffPayload = 256ull * 1024ull * 1024ull;
constexpr std::uint32_t kMaximumHandoffString = 1024u;
constexpr std::uint64_t kMaximumMeshElements = 64ull * 1024ull * 1024ull;

class Writer {
public:
    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift != 32; shift += 8)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    void floating(float value) {
        std::uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
    void string(const std::string& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void point(matter::Float3 value) {
        floating(value.x); floating(value.y); floating(value.z);
    }
    void raw(const std::uint8_t* data, std::size_t size) {
        bytes.insert(bytes.end(), data, data + size);
    }
    std::vector<std::uint8_t> bytes;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size)
        : current(data), remaining(size) {}
    bool u8(std::uint8_t& value) {
        if (remaining == 0u) return false;
        value = *current++;
        --remaining;
        return true;
    }
    bool u32(std::uint32_t& value) {
        value = 0u;
        for (unsigned shift = 0; shift != 32; shift += 8) {
            std::uint8_t byte = 0u;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool u64(std::uint64_t& value) {
        value = 0u;
        for (unsigned shift = 0; shift != 64; shift += 8) {
            std::uint8_t byte = 0u;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }
    bool floating(float& value) {
        std::uint32_t bits = 0u;
        if (!u32(bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }
    bool string(std::string& value) {
        std::uint32_t size = 0u;
        if (!u32(size) || size > kMaximumHandoffString || size > remaining)
            return false;
        value.assign(reinterpret_cast<const char*>(current), size);
        current += size;
        remaining -= size;
        return true;
    }
    bool point(matter::Float3& value) {
        return floating(value.x) && floating(value.y) && floating(value.z);
    }
    const std::uint8_t* current = nullptr;
    std::size_t remaining = 0u;
};

std::uint64_t byte_digest(const std::uint8_t* bytes, std::size_t size) {
    std::uint64_t digest = UINT64_C(1469598103934665603);
    for (std::size_t index = 0u; index != size; ++index) {
        digest ^= bytes[index];
        digest *= UINT64_C(1099511628211);
    }
    return digest == 0u ? 1u : digest;
}

void write_handoff(Writer& writer, const SpillwayHandoffRecord& value) {
    writer.string(value.id);
    writer.string(value.upstream_section_id);
    writer.string(value.downstream_section_id);
    writer.point(value.lip_origin_m);
    writer.point(value.tangent);
    writer.point(value.lateral);
    writer.point(value.up);
    writer.floating(value.discharge_m3s);
    writer.floating(value.width_m);
    writer.floating(value.effective_depth_m);
    writer.floating(value.channel_depth_m);
    writer.floating(value.channel_asymmetry);
    writer.floating(value.initial_speed_mps);
    writer.floating(value.overlap_m);
    writer.floating(value.upstream_visual_cut_m);
    writer.floating(value.downstream_visual_cut_m);
    writer.point(value.temporary_dam_exclusion_bounds_m.minimum);
    writer.point(value.temporary_dam_exclusion_bounds_m.maximum);
    writer.u64(value.semantic_key);
}

bool read_handoff(Reader& reader, SpillwayHandoffRecord& value) {
    return reader.string(value.id) &&
           reader.string(value.upstream_section_id) &&
           reader.string(value.downstream_section_id) &&
           reader.point(value.lip_origin_m) && reader.point(value.tangent) &&
           reader.point(value.lateral) && reader.point(value.up) &&
           reader.floating(value.discharge_m3s) &&
           reader.floating(value.width_m) &&
           reader.floating(value.effective_depth_m) &&
           reader.floating(value.channel_depth_m) &&
           reader.floating(value.channel_asymmetry) &&
           reader.floating(value.initial_speed_mps) &&
           reader.floating(value.overlap_m) &&
           reader.floating(value.upstream_visual_cut_m) &&
           reader.floating(value.downstream_visual_cut_m) &&
           reader.point(value.temporary_dam_exclusion_bounds_m.minimum) &&
           reader.point(value.temporary_dam_exclusion_bounds_m.maximum) &&
           reader.u64(value.semantic_key);
}

void write_mesh(Writer& writer, const gpu_meshing::MeshResult& mesh) {
    writer.u64(mesh.positions.size());
    for (const float value : mesh.positions) writer.floating(value);
    writer.u64(mesh.normals.size());
    for (const float value : mesh.normals) writer.floating(value);
    writer.u64(mesh.indices.size());
    for (const auto value : mesh.indices) writer.u32(value);
    writer.u32(mesh.material);
    writer.u64(mesh.content_digest);
}

bool read_mesh(Reader& reader, gpu_meshing::MeshResult& mesh) {
    std::uint64_t count = 0u;
    if (!reader.u64(count) || count > kMaximumMeshElements ||
        count > reader.remaining / sizeof(float))
        return false;
    mesh.positions.resize(static_cast<std::size_t>(count));
    for (float& value : mesh.positions)
        if (!reader.floating(value)) return false;
    if (!reader.u64(count) || count > kMaximumMeshElements ||
        count > reader.remaining / sizeof(float))
        return false;
    mesh.normals.resize(static_cast<std::size_t>(count));
    for (float& value : mesh.normals)
        if (!reader.floating(value)) return false;
    if (!reader.u64(count) || count > kMaximumMeshElements ||
        count > reader.remaining / sizeof(std::uint32_t))
        return false;
    mesh.indices.resize(static_cast<std::size_t>(count));
    for (auto& value : mesh.indices)
        if (!reader.u32(value)) return false;
    return reader.u32(mesh.material) && reader.u64(mesh.content_digest);
}

bool valid_handoff_artifact(const HydrologyHandoffArtifact& artifact) {
    return !artifact.id.empty() && artifact.id == artifact.handoff.id &&
           artifact.id.size() <= kMaximumHandoffString &&
           artifact.handoff.upstream_section_id.size() <=
               kMaximumHandoffString &&
           artifact.handoff.downstream_section_id.size() <=
               kMaximumHandoffString &&
           artifact.handoff.semantic_key != 0u &&
           artifact.handoff.semantic_key ==
               spillway_handoff_semantic_key(artifact.handoff) &&
           artifact.semantic_key != 0u &&
           artifact.upstream_payload_digest != 0u &&
           artifact.downstream_payload_digest != 0u &&
           valid_mesh(artifact.visual_mesh) &&
           artifact.payload_digest == handoff_artifact_digest(artifact);
}

bool replace_file(const std::filesystem::path& source,
                  const std::filesystem::path& target) {
#ifdef _WIN32
    return MoveFileExW(source.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(source.c_str(), target.c_str()) == 0;
#endif
}

}  // namespace

std::uint64_t hydrology_runtime_field_digest(
    const GameplayFieldLayout& layout,
    const std::vector<GameplaySample>& field) noexcept {
    try {
        if (!valid_layout(layout) || field.size() !=
                static_cast<std::size_t>(layout.width) * layout.depth)
            return 0u;
        Digest digest(UINT64_C(0x52554e54494d4531));
        digest.floating(layout.origin_m.x);
        digest.floating(layout.origin_m.y);
        digest.floating(layout.origin_m.z);
        digest.floating(layout.cell_size_m);
        digest.u64(layout.width);
        digest.u64(layout.depth);
        for (const auto& sample : field) {
            if (!valid_sample(sample)) return 0u;
            digest.floating(sample.height_m);
            digest.floating(sample.depth_m);
            digest.floating(sample.velocity_x_mps);
            digest.floating(sample.velocity_y_mps);
            digest.floating(sample.velocity_z_mps);
            digest.byte(sample.wet_valid ? 1u : 0u);
        }
        return digest.finish();
    } catch (...) {
        return 0u;
    }
}

std::uint64_t hydrology_presentation_field_digest(
    const GameplayFieldLayout& layout,
    const std::vector<PresentationSample>& field) noexcept {
    try {
        if (!valid_layout(layout) || field.size() !=
                static_cast<std::size_t>(layout.width) * layout.depth)
            return 0u;
        Digest digest(UINT64_C(0x50524553454e5431));
        digest.floating(layout.origin_m.x);
        digest.floating(layout.origin_m.y);
        digest.floating(layout.origin_m.z);
        digest.floating(layout.cell_size_m);
        digest.u64(layout.width);
        digest.u64(layout.depth);
        for (const auto& sample : field) {
            if (!valid_sample(sample)) return 0u;
            digest.floating(sample.normal_x);
            digest.floating(sample.normal_z);
            digest.floating(sample.turbulence);
            digest.floating(sample.aeration);
            digest.floating(sample.foam_potential);
            digest.byte(static_cast<std::uint8_t>(sample.feature));
            digest.byte(sample.wet_valid ? 1u : 0u);
        }
        return digest.finish();
    } catch (...) {
        return 0u;
    }
}

bool build_handoff_artifact(
    const HydrologyArtifact& upstream,
    const HydrologyArtifact& downstream,
    const SpillwayHandoffRecord& handoff,
    const HandoffProductSettings& settings,
    const PhysxFluidBake::VisualMesher& visual_mesher,
    HydrologyHandoffArtifact& artifact,
    HydrologyNetworkProducts& products,
    FluidBakeError& error) {
    artifact = {};
    products = {};
    error = {};
    try {
        if (!upstream.accepted || !downstream.accepted ||
            upstream.section.section_id != handoff.upstream_section_id ||
            downstream.section.section_id != handoff.downstream_section_id ||
            upstream.payload_digest == 0u || downstream.payload_digest == 0u ||
            handoff.id.empty() || handoff.semantic_key == 0u ||
            handoff.semantic_key != spillway_handoff_semantic_key(handoff) ||
            !finite(handoff.lip_origin_m) || !finite(handoff.tangent) ||
            !finite(handoff.lateral) || !finite(handoff.overlap_m) ||
            handoff.overlap_m <= 0.0f || !finite(handoff.width_m) ||
            handoff.width_m <= 0.0f ||
            !finite(handoff.channel_depth_m) ||
            handoff.channel_depth_m < 0.0f ||
            !finite(handoff.channel_asymmetry) ||
            std::fabs(handoff.channel_asymmetry) > 1.0f ||
            handoff.upstream_visual_cut_m >=
                handoff.downstream_visual_cut_m ||
            !valid_bounds(handoff.temporary_dam_exclusion_bounds_m) ||
            !finite(settings.particle_radius_m) ||
            settings.particle_radius_m <= 0.0f || !visual_mesher ||
            !valid_layout(settings.gameplay_layout))
            return fail("handoff product input is invalid", error);

        std::vector<FluidParticle> collar_particles;
        const auto gather = [&](const std::vector<FluidParticle>& particles) {
            for (const auto& particle : particles) {
                if (!finite(particle.position_m) ||
                    !finite(particle.velocity_mps))
                    return false;
                if (std::fabs(signed_distance(handoff, particle.position_m)) <=
                        handoff.overlap_m)
                    collar_particles.push_back(particle);
            }
            return true;
        };
        if (!gather(upstream.particles) || !gather(downstream.particles) ||
            collar_particles.empty())
            return fail("handoff collar has no finite water particles", error);
        std::size_t positive_collar_particles = 0u;
        float minimum_collar_distance = std::numeric_limits<float>::infinity();
        float maximum_collar_distance = -std::numeric_limits<float>::infinity();
        for (const auto& particle : collar_particles) {
            const float distance = signed_distance(handoff, particle.position_m);
            minimum_collar_distance = std::min(minimum_collar_distance,
                                               distance);
            maximum_collar_distance = std::max(maximum_collar_distance,
                                               distance);
            if (distance > 0.0f) ++positive_collar_particles;
        }
        std::sort(collar_particles.begin(), collar_particles.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });

        gpu_meshing::ParticleJob template_job =
            PhysxFluidBake::resolved_visual_job(
                collar_particles, settings.particle_radius_m,
                settings.visual_job);
        std::vector<gpu_meshing::ParticleSample> mesh_particles;
        gpu_meshing::ParticleJob patch_job{};
        gpu_meshing::Error mesh_error{};
        if (!make_fluid_particle_job(
                collar_particles, settings.particle_radius_m, template_job,
                mesh_particles, patch_job, mesh_error)) {
            error = {FluidBakeCode::ProductFailure, mesh_error.message};
            return false;
        }
        gpu_meshing::MeshResult raw_patch{};
        gpu_meshing::Stats stats{};
        if (!visual_mesher(patch_job, raw_patch, stats, mesh_error, {}) ||
            !valid_mesh(raw_patch)) {
            error = {FluidBakeCode::ProductFailure,
                     mesh_error.message.empty()
                         ? "handoff visual mesher returned invalid geometry"
                         : mesh_error.message};
            return false;
        }

        const float weld = std::max(1.0e-5f,
                                    settings.visual_job.voxel_m * 1.0e-4f);
        MeshAssembler patch_assembler(weld);
        if (!append_clipped_mesh(raw_patch, handoff, true,
                                 handoff.upstream_visual_cut_m, true,
                                 handoff.downstream_visual_cut_m,
                                 patch_assembler))
            return fail("handoff patch clipping failed", error);
        gpu_meshing::MeshResult patch = patch_assembler.finish();
        if (!valid_mesh(patch))
            return fail("handoff patch is empty or invalid", error);

        MeshAssembler upstream_assembler(weld);
        MeshAssembler downstream_assembler(weld);
        if (!append_clipped_mesh(upstream.visual_mesh, handoff, false, 0.0f,
                                 true, handoff.upstream_visual_cut_m,
                                 upstream_assembler) ||
            !append_clipped_mesh(downstream.visual_mesh, handoff, true,
                                 handoff.downstream_visual_cut_m, false, 0.0f,
                                 downstream_assembler))
            return fail("handoff visual ownership clipping failed", error);
        gpu_meshing::MeshResult upstream_piece = upstream_assembler.finish();
        gpu_meshing::MeshResult downstream_piece =
            downstream_assembler.finish();
        if (!valid_mesh(upstream_piece) || !valid_mesh(downstream_piece))
            return fail("handoff visual ownership piece is invalid", error);

        MeshAssembler visual(weld);
        if (!append_mesh(upstream_piece, visual) ||
            !append_mesh(patch, visual) ||
            !append_mesh(downstream_piece, visual))
            return fail("handoff visual contour stitching failed", error);
        std::string stitch_diagnostic;
        if (!append_cut_stitches(upstream_piece, patch, handoff,
                                 handoff.upstream_visual_cut_m, visual,
                                 stitch_diagnostic)) {
            error = {FluidBakeCode::ProductFailure,
                     "upstream handoff contour stitching failed: " +
                         stitch_diagnostic};
            return false;
        }
        if (!append_cut_stitches(patch, downstream_piece, handoff,
                                 handoff.downstream_visual_cut_m, visual,
                                 stitch_diagnostic)) {
            error = {FluidBakeCode::ProductFailure,
                     "downstream handoff contour stitching failed: " +
                         stitch_diagnostic + "; collar signed range " +
                         std::to_string(minimum_collar_distance) + " to " +
                         std::to_string(maximum_collar_distance) + " with " +
                         std::to_string(positive_collar_particles) + " of " +
                         std::to_string(collar_particles.size()) +
                         " particles downstream"};
            return false;
        }
        products.visual_mesh = visual.finish();

        MeshAssembler coarse(weld);
        if (!append_clipped_mesh(upstream.coarse_cpu_mesh, handoff, false,
                                 0.0f, true, 0.0f, coarse) ||
            !append_clipped_mesh(downstream.coarse_cpu_mesh, handoff, true,
                                 0.0f, false, 0.0f, coarse))
            return fail("handoff query ownership clipping failed", error);
        products.coarse_cpu_mesh = coarse.finish();
        products.gameplay_layout = settings.gameplay_layout;
        if (!build_gameplay(upstream, downstream, handoff,
                            products.gameplay_layout,
                            products.gameplay_field))
            return fail("handoff gameplay aggregation failed", error);
        if (!build_presentation(upstream, downstream, handoff,
                                products.gameplay_layout,
                                products.gameplay_field,
                                products.presentation_field))
            return fail("handoff presentation aggregation failed", error);

        artifact.id = handoff.id;
        artifact.handoff = handoff;
        artifact.semantic_key =
            handoff_semantic_key(upstream, downstream, handoff);
        artifact.upstream_payload_digest = upstream.payload_digest;
        artifact.downstream_payload_digest = downstream.payload_digest;
        artifact.visual_mesh = std::move(patch);
        artifact.payload_digest = handoff_artifact_digest(artifact);
        if (!validate_handoff_products(artifact, products, handoff, error)) {
            artifact = {};
            products = {};
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        artifact = {};
        products = {};
        error = {FluidBakeCode::ProductFailure, exception.what()};
        return false;
    } catch (...) {
        artifact = {};
        products = {};
        return fail("handoff product construction raised an unknown exception",
                    error);
    }
}

bool build_handoff_water_animation_artifact(
    const WaterMeshAnimationArtifact& upstream,
    const WaterMeshAnimationArtifact& downstream,
    const SpillwayHandoffRecord& handoff,
    float visual_voxel_m,
    WaterMeshAnimationArtifact& artifact,
    FluidBakeError& error,
    HandoffAnimationBuildDiagnostics* diagnostics) {
    artifact = {};
    error = {};
    if (diagnostics) *diagnostics = {};
    try {
        if (upstream.identity != handoff.upstream_section_id ||
            downstream.identity != handoff.downstream_section_id ||
            handoff.id.empty() || handoff.semantic_key == 0u ||
            handoff.semantic_key != spillway_handoff_semantic_key(handoff) ||
            upstream.semantic_key == 0u || downstream.semantic_key == 0u ||
            upstream.source_primary_payload_digest == 0u ||
            downstream.source_primary_payload_digest == 0u ||
            upstream.source_secondary_payload_digest != 0u ||
            downstream.source_secondary_payload_digest != 0u ||
            upstream.frames.size() != 30u ||
            upstream.frames.size() != downstream.frames.size() ||
            upstream.frames_per_second != downstream.frames_per_second ||
            upstream.phase_offset_frames != downstream.phase_offset_frames ||
            upstream.duration_seconds != downstream.duration_seconds ||
            upstream.material != downstream.material ||
            !finite(visual_voxel_m) || visual_voxel_m <= 0.0f ||
            handoff.upstream_visual_cut_m >=
                handoff.downstream_visual_cut_m)
            return fail("handoff animation input is invalid", error);

        WaterMeshAnimation animation{};
        animation.frames_per_second = upstream.frames_per_second;
        animation.phase_offset_frames = upstream.phase_offset_frames;
        animation.duration_seconds = upstream.duration_seconds;
        animation.frames.reserve(upstream.frames.size());

        const float weld = std::max(1.0e-5f, visual_voxel_m * 1.0e-4f);
        const float cut_tolerance_m = visual_voxel_m / 16.0f;
        gpu_meshing::Error decode_error{};
        for (std::uint32_t frame_index = 0u;
             frame_index != upstream.frames.size(); ++frame_index) {
            const auto frame_start = std::chrono::steady_clock::now();
            gpu_meshing::MeshResult upstream_frame{};
            gpu_meshing::MeshResult downstream_frame{};
            if (!decode_water_mesh_animation_frame(
                    upstream, frame_index, upstream_frame, decode_error) ||
                !decode_water_mesh_animation_frame(
                    downstream, frame_index, downstream_frame,
                    decode_error)) {
                error = {FluidBakeCode::ProductFailure,
                         decode_error.message.empty()
                             ? "handoff animation frame decode failed"
                             : decode_error.message};
                return false;
            }
            MeshAssembler upstream_owned(weld);
            MeshAssembler downstream_owned(weld);
            if (!append_clipped_mesh(
                    upstream_frame, handoff, false, 0.0f, true,
                    handoff.upstream_visual_cut_m, upstream_owned) ||
                !append_clipped_mesh(
                    downstream_frame, handoff, true,
                    handoff.downstream_visual_cut_m, false, 0.0f,
                    downstream_owned))
                return fail("handoff animation ownership clipping failed",
                            error);
            gpu_meshing::MeshResult upstream_piece = upstream_owned.finish();
            gpu_meshing::MeshResult downstream_piece =
                downstream_owned.finish();
            if (!valid_mesh(upstream_piece) || !valid_mesh(downstream_piece))
                return fail("handoff animation ownership piece is invalid",
                            error);

            MeshAssembler bridge(weld);
            std::string diagnostic;
            if (!append_cut_bridge(
                    upstream_piece, handoff.upstream_visual_cut_m,
                    downstream_piece, handoff.downstream_visual_cut_m,
                    handoff, bridge, diagnostic)) {
                error = {FluidBakeCode::ProductFailure,
                         "handoff animation contour bridge failed at frame " +
                             std::to_string(frame_index) + ": " + diagnostic};
                return false;
            }
            gpu_meshing::MeshResult frame = bridge.finish();
            if (!valid_mesh(frame))
                return fail("handoff animation bridge frame is invalid",
                            error);
            animation.frames.push_back(std::move(frame));
            if (diagnostics) {
                diagnostics->frame_mesh_ms[frame_index] =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - frame_start)
                        .count();
                FluidBakeError cut_error{};
                const gpu_meshing::MeshResult& built_frame =
                    animation.frames.back();
                const bool upstream_measured = measure_water_cut_continuity(
                    upstream_frame, built_frame, handoff,
                    handoff.upstream_visual_cut_m, cut_tolerance_m,
                    diagnostics->upstream_cut[frame_index], cut_error);
                cut_error = {};
                const bool downstream_measured = measure_water_cut_continuity(
                    built_frame, downstream_frame, handoff,
                    handoff.downstream_visual_cut_m, cut_tolerance_m,
                    diagnostics->downstream_cut[frame_index], cut_error);
                diagnostics->source_blend_required =
                    diagnostics->source_blend_required ||
                    !upstream_measured || !downstream_measured ||
                    !water_cut_is_assertion_weldable(
                        diagnostics->upstream_cut[frame_index],
                        cut_tolerance_m) ||
                    !water_cut_is_assertion_weldable(
                        diagnostics->downstream_cut[frame_index],
                        cut_tolerance_m);
                diagnostics->peak_build_cpu_payload_bytes = std::max(
                    diagnostics->peak_build_cpu_payload_bytes,
                    saturating_payload_sum({
                        animation_mesh_payload_bytes(animation),
                        mesh_payload_bytes(upstream_frame),
                        mesh_payload_bytes(downstream_frame)}));
            }
        }

        gpu_meshing::Error artifact_error{};
        Digest semantic(UINT64_C(0x48414e44414e494d));
        semantic.u64(handoff.semantic_key);
        semantic.u64(upstream.semantic_key);
        semantic.u64(downstream.semantic_key);
        semantic.u64(upstream.source_primary_payload_digest);
        semantic.u64(downstream.source_primary_payload_digest);
        semantic.floating(visual_voxel_m);
        semantic.u64(animation.frames_per_second);
        semantic.u64(animation.phase_offset_frames);
        semantic.u64(animation.frames.size());
        const WaterMeshAnimationArtifactMetadata metadata{
            handoff.id, semantic.finish(),
            upstream.source_primary_payload_digest,
            downstream.source_primary_payload_digest, visual_voxel_m};
        if (!pack_water_mesh_animation_artifact(
                metadata, animation, artifact, artifact_error)) {
            error = {FluidBakeCode::ProductFailure,
                     artifact_error.message.empty()
                         ? "handoff animation packing failed"
                         : artifact_error.message};
            return false;
        }
        if (diagnostics) {
            std::vector<std::uint8_t> serialized;
            if (!serialize_water_mesh_animation_artifact(
                    artifact, serialized, artifact_error)) {
                error = {FluidBakeCode::ProductFailure,
                         artifact_error.message.empty()
                             ? "handoff animation diagnostics serialization failed"
                             : artifact_error.message};
                artifact = {};
                *diagnostics = {};
                return false;
            }
            diagnostics->artifact_file_bytes = serialized.size();
            diagnostics->peak_build_cpu_payload_bytes = std::max(
                diagnostics->peak_build_cpu_payload_bytes,
                saturating_payload_sum({
                    animation_mesh_payload_bytes(animation),
                    artifact_heap_payload_bytes(artifact),
                    diagnostics->artifact_file_bytes}));
        }
        return true;
    } catch (const std::exception& exception) {
        artifact = {};
        if (diagnostics) *diagnostics = {};
        error = {FluidBakeCode::ProductFailure, exception.what()};
        return false;
    } catch (...) {
        artifact = {};
        if (diagnostics) *diagnostics = {};
        return fail("handoff animation construction raised an unknown exception",
                    error);
    }
}

bool clip_section_water_mesh_animation(
    const WaterMeshAnimation& source,
    const std::string& section_id,
    const std::vector<SpillwayHandoffRecord>& handoffs,
    float visual_voxel_m,
    WaterMeshAnimation& owned,
    FluidBakeError& error) {
    owned = {};
    error = {};
    try {
        if (section_id.empty() || source.frames.empty() ||
            source.frames_per_second == 0u ||
            !finite(source.duration_seconds) ||
            source.duration_seconds <= 0.0f || !finite(visual_voxel_m) ||
            visual_voxel_m <= 0.0f)
            return fail("section animation ownership input is invalid", error);
        owned.frames_per_second = source.frames_per_second;
        owned.phase_offset_frames = source.phase_offset_frames;
        owned.duration_seconds = source.duration_seconds;
        owned.frames.reserve(source.frames.size());
        const float weld = std::max(1.0e-5f, visual_voxel_m * 1.0e-4f);
        for (const auto& source_frame : source.frames) {
            if (!valid_mesh(source_frame))
                return fail("section animation contains an invalid frame",
                            error);
            gpu_meshing::MeshResult frame = source_frame;
            for (const auto& handoff : handoffs) {
                const bool upstream_owner =
                    handoff.upstream_section_id == section_id;
                const bool downstream_owner =
                    handoff.downstream_section_id == section_id;
                if (upstream_owner == downstream_owner)
                    return fail(
                        "section animation ownership handoff identity is invalid",
                        error);
                MeshAssembler clipped(weld);
                const bool appended = upstream_owner
                    ? append_clipped_mesh(
                          frame, handoff, false, 0.0f, true,
                          handoff.upstream_visual_cut_m, clipped)
                    : append_clipped_mesh(
                          frame, handoff, true,
                          handoff.downstream_visual_cut_m, false, 0.0f,
                          clipped);
                if (!appended)
                    return fail("section animation ownership clipping failed",
                                error);
                frame = clipped.finish();
                if (!valid_mesh(frame))
                    return fail(
                        "section animation ownership produced an empty frame",
                        error);
            }
            owned.frames.push_back(std::move(frame));
        }
        return true;
    } catch (const std::exception& exception) {
        owned = {};
        error = {FluidBakeCode::ProductFailure, exception.what()};
        return false;
    } catch (...) {
        owned = {};
        return fail(
            "section animation ownership raised an unknown exception", error);
    }
}

bool validate_handoff_products(
    const HydrologyHandoffArtifact& artifact,
    const HydrologyNetworkProducts& products,
    const SpillwayHandoffRecord& handoff,
    FluidBakeError& error) {
    error = {};
    if (artifact.id != handoff.id || artifact.handoff.semantic_key !=
            handoff.semantic_key || artifact.semantic_key == 0u ||
        artifact.upstream_payload_digest == 0u ||
        artifact.downstream_payload_digest == 0u ||
        artifact.payload_digest == 0u || !valid_mesh(artifact.visual_mesh) ||
        !valid_mesh(products.visual_mesh) ||
        !valid_mesh(products.coarse_cpu_mesh) ||
        !valid_layout(products.gameplay_layout) ||
        products.gameplay_field.size() !=
            static_cast<std::size_t>(products.gameplay_layout.width) *
                products.gameplay_layout.depth ||
        products.presentation_field.size() != products.gameplay_field.size())
        return fail("handoff products are structurally invalid", error);
    for (std::size_t index = 0u; index != products.gameplay_field.size();
         ++index) {
        if (!valid_sample(products.gameplay_field[index]))
            return fail("handoff gameplay field contains an invalid sample",
                        error);
        if (!valid_sample(products.presentation_field[index]) ||
            products.presentation_field[index].wet_valid !=
                products.gameplay_field[index].wet_valid)
            return fail(
                "handoff presentation field contains an invalid sample",
                error);
    }
    // Visual products are built exclusively from fluid particle isosurfaces;
    // collision meshes are never an input to this assembler. Do not reject
    // water merely because it occupies the former temporary-dam footprint:
    // the dam is removed before the downstream section begins, so that space
    // is intentionally wet in the completed network.
    if (has_open_cut_edge(products.visual_mesh, handoff))
        return fail("handoff visual has an open ownership boundary", error);
    if (duplicate_cpu_triangle(products.coarse_cpu_mesh))
        return fail("handoff query mesh has duplicate triangle ownership",
                    error);
    if (artifact.payload_digest != handoff_artifact_digest(artifact))
        return fail("handoff product digest is stale", error);
    return true;
}

bool serialize_handoff_artifact(
    const HydrologyHandoffArtifact& artifact,
    std::vector<std::uint8_t>& bytes,
    gpu_meshing::Error& error) {
    bytes.clear();
    error = {};
    if (!valid_handoff_artifact(artifact)) {
        error = {gpu_meshing::ErrorCode::ArtifactFailure,
                 "hydrology handoff artifact is invalid"};
        return false;
    }
    Writer payload;
    payload.string(artifact.id);
    write_handoff(payload, artifact.handoff);
    payload.u64(artifact.semantic_key);
    payload.u64(artifact.upstream_payload_digest);
    payload.u64(artifact.downstream_payload_digest);
    write_mesh(payload, artifact.visual_mesh);
    payload.u64(artifact.payload_digest);
    if (payload.bytes.size() > kMaximumHandoffPayload) {
        error = {gpu_meshing::ErrorCode::LimitExceeded,
                 "hydrology handoff artifact exceeds its payload limit"};
        return false;
    }
    Writer file;
    file.raw(kHandoffMagic, sizeof(kHandoffMagic));
    file.u32(kHandoffVersion);
    file.u64(payload.bytes.size());
    file.u64(byte_digest(payload.bytes.data(), payload.bytes.size()));
    file.raw(payload.bytes.data(), payload.bytes.size());
    bytes = std::move(file.bytes);
    return true;
}

bool deserialize_handoff_artifact(
    const std::vector<std::uint8_t>& bytes,
    HydrologyHandoffArtifact& artifact,
    gpu_meshing::Error& error) {
    artifact = {};
    error = {};
    const auto reject = [&](const char* message) {
        artifact = {};
        error = {gpu_meshing::ErrorCode::ArtifactFailure, message};
        return false;
    };
    if (bytes.size() < kHandoffHeaderBytes ||
        std::memcmp(bytes.data(), kHandoffMagic, sizeof(kHandoffMagic)) != 0)
        return reject("hydrology handoff artifact header is invalid");
    Reader header(bytes.data() + sizeof(kHandoffMagic),
                  bytes.size() - sizeof(kHandoffMagic));
    std::uint32_t version = 0u;
    std::uint64_t payload_size = 0u;
    std::uint64_t expected_digest = 0u;
    if (!header.u32(version) || !header.u64(payload_size) ||
        !header.u64(expected_digest) || version != kHandoffVersion ||
        payload_size > kMaximumHandoffPayload ||
        payload_size != bytes.size() - kHandoffHeaderBytes)
        return reject("hydrology handoff artifact header is invalid");
    const auto* payload = bytes.data() + kHandoffHeaderBytes;
    if (byte_digest(payload, static_cast<std::size_t>(payload_size)) !=
        expected_digest)
        return reject("hydrology handoff artifact digest is invalid");
    Reader reader(payload, static_cast<std::size_t>(payload_size));
    HydrologyHandoffArtifact candidate{};
    if (!reader.string(candidate.id) ||
        !read_handoff(reader, candidate.handoff) ||
        !reader.u64(candidate.semantic_key) ||
        !reader.u64(candidate.upstream_payload_digest) ||
        !reader.u64(candidate.downstream_payload_digest) ||
        !read_mesh(reader, candidate.visual_mesh) ||
        !reader.u64(candidate.payload_digest) || reader.remaining != 0u ||
        !valid_handoff_artifact(candidate))
        return reject("hydrology handoff artifact payload is invalid");
    artifact = std::move(candidate);
    return true;
}

bool save_handoff_artifact_atomic(
    const std::filesystem::path& path,
    const HydrologyHandoffArtifact& artifact,
    gpu_meshing::Error& error) {
    std::vector<std::uint8_t> bytes;
    if (!serialize_handoff_artifact(artifact, bytes, error)) return false;
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            error = {gpu_meshing::ErrorCode::ArtifactFailure,
                     "could not create hydrology handoff directory"};
            return false;
        }
    }
    static std::atomic<std::uint64_t> serial{0u};
    const std::filesystem::path temporary =
        path.string() + ".tmp-" + std::to_string(++serial);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, filesystem_error);
            error = {gpu_meshing::ErrorCode::ArtifactFailure,
                     "could not write hydrology handoff temporary"};
            return false;
        }
    }
    std::vector<std::uint8_t> reopened(bytes.size());
    {
        std::ifstream stream(temporary, std::ios::binary);
        stream.read(reinterpret_cast<char*>(reopened.data()),
                    static_cast<std::streamsize>(reopened.size()));
        if (!stream) {
            std::filesystem::remove(temporary, filesystem_error);
            error = {gpu_meshing::ErrorCode::ArtifactFailure,
                     "could not reopen hydrology handoff temporary"};
            return false;
        }
    }
    HydrologyHandoffArtifact validated{};
    if (!deserialize_handoff_artifact(reopened, validated, error)) {
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    if (!replace_file(temporary, path)) {
        std::filesystem::remove(temporary, filesystem_error);
        error = {gpu_meshing::ErrorCode::ArtifactFailure,
                 "could not publish hydrology handoff artifact"};
        return false;
    }
    return true;
}

bool load_handoff_artifact_validated(
    const std::filesystem::path& path,
    std::uint64_t expected_semantic_key,
    std::uint64_t expected_upstream_payload_digest,
    std::uint64_t expected_downstream_payload_digest,
    HydrologyHandoffArtifact& artifact,
    gpu_meshing::Error& error) {
    artifact = {};
    error = {};
    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size < kHandoffHeaderBytes ||
        size > kHandoffHeaderBytes + kMaximumHandoffPayload) {
        error = {gpu_meshing::ErrorCode::ArtifactFailure,
                 "hydrology handoff artifact file size is invalid"};
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!stream || !deserialize_handoff_artifact(bytes, artifact, error))
        return false;
    if (artifact.semantic_key != expected_semantic_key ||
        artifact.upstream_payload_digest != expected_upstream_payload_digest ||
        artifact.downstream_payload_digest !=
            expected_downstream_payload_digest) {
        artifact = {};
        error = {gpu_meshing::ErrorCode::ArtifactFailure,
                 "hydrology handoff artifact is stale"};
        return false;
    }
    return true;
}

std::string hydrology_network_timing_trace_json(
    const HydrologyNetworkBakeResult& result) {
    const auto hex64 = [](std::uint64_t value) {
        std::ostringstream stream;
        stream << std::hex << std::setfill('0') << std::setw(16) << value;
        return stream.str();
    };
    const auto write_u32_array = [](std::ostringstream& stream,
                                    const std::vector<std::uint32_t>& values) {
        stream << '[';
        for (std::size_t index = 0u; index != values.size(); ++index) {
            if (index != 0u) stream << ',';
            stream << values[index];
        }
        stream << ']';
    };
    const auto write_frame_ms = [](std::ostringstream& stream,
                                   const std::array<double, 30>& values) {
        stream << '[';
        for (std::size_t index = 0u; index != values.size(); ++index) {
            if (index != 0u) stream << ',';
            const double value = values[index];
            stream << (std::isfinite(value) && value >= 0.0 ? value : 0.0);
        }
        stream << ']';
    };
    const auto write_cut_array = [](std::ostringstream& stream,
                                    const std::array<WaterCutContourMetrics,
                                                     30>& values) {
        const auto finite_or_zero = [](float value) {
            return std::isfinite(value) ? value : 0.0f;
        };
        stream << '[';
        for (std::size_t index = 0u; index != values.size(); ++index) {
            if (index != 0u) stream << ',';
            const auto& value = values[index];
            stream << "{\"firstPoints\":" << value.first_points
                   << ",\"secondPoints\":" << value.second_points
                   << ",\"unmatchedOpenEdges\":"
                   << value.unmatched_open_edges
                   << ",\"duplicateCoplanarTriangles\":"
                   << value.duplicate_coplanar_triangles
                   << ",\"symmetricHausdorffM\":"
                   << finite_or_zero(value.symmetric_hausdorff_m)
                   << ",\"rmsDistanceM\":"
                   << finite_or_zero(value.rms_distance_m)
                   << ",\"firstHeightQuantilesM\":["
                   << finite_or_zero(value.first_height_quantiles_m[0]) << ','
                   << finite_or_zero(value.first_height_quantiles_m[1]) << ','
                   << finite_or_zero(value.first_height_quantiles_m[2])
                   << "],\"secondHeightQuantilesM\":["
                   << finite_or_zero(value.second_height_quantiles_m[0]) << ','
                   << finite_or_zero(value.second_height_quantiles_m[1]) << ','
                   << finite_or_zero(value.second_height_quantiles_m[2])
                   << "],\"minimumNormalDot\":"
                   << finite_or_zero(value.minimum_normal_dot)
                   << ",\"p95NormalAngleDegrees\":"
                   << finite_or_zero(value.p95_normal_angle_degrees) << '}';
        }
        stream << ']';
    };

    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3)
           << "{\n  \"networkState\": \"Ready\",\n  \"sections\": [\n";
    for (std::size_t index = 0u;
         index != result.timings.sections.size(); ++index) {
        const auto& timing = result.timings.sections[index];
        const auto section = std::find_if(
            result.sections.begin(), result.sections.end(),
            [&](const auto& value) {
                return value.section.section_id == timing.id;
            });
        const std::uint32_t particles = section == result.sections.end()
            ? 0u : section->stats.active_particles;
        const std::uint32_t escaped = section == result.sections.end()
            ? 0u : section->stats.escaped_particles;
        const std::uint32_t accepted_step = section == result.sections.end()
            ? 0u : section->stats.simulated_steps;
        stream << "    {\"id\":" << std::quoted(timing.id)
               << ",\"cacheHit\":" << std::boolalpha << timing.cache_hit
               << ",\"setupMs\":" << timing.setup_ms
               << ",\"physxInitMs\":" << timing.physx_init_ms
               << ",\"simulateMs\":" << timing.simulate_ms
               << ",\"gpuMeshMs\":" << timing.gpu_mesh_ms
               << ",\"animationCaptureMs\":" << timing.animation_capture_ms
               << ",\"animationMeshMs\":" << timing.animation_mesh_ms
               << ",\"animationSerializeMs\":"
               << timing.animation_serialize_ms
               << ",\"cpuMeshMs\":" << timing.cpu_mesh_ms
               << ",\"particles\":" << particles
               << ",\"escaped\":" << escaped
               << ",\"acceptedStep\":" << accepted_step
               << ",\"animationCacheHit\":"
               << timing.animation_cache_hit
               << ",\"animationBytes\":" << timing.animation_bytes
               << ",\"animationDeviceCaptureBytes\":"
               << timing.animation_device_capture_bytes
               << ",\"animationSemanticKey\":"
               << std::quoted(hex64(timing.animation_semantic_key))
               << ",\"animationPayloadDigest\":"
               << std::quoted(hex64(timing.animation_payload_digest))
               << ",\"animationCaptureFirstStep\":"
               << timing.animation_capture_first_step
               << ",\"animationCaptureLastStep\":"
               << timing.animation_capture_last_step
               << ",\"animationCaptureParticleCounts\":";
        write_u32_array(stream, timing.animation_capture_particle_counts);
        stream << ",\"animationFrameVertexCounts\":";
        write_u32_array(stream, timing.animation_frame_vertex_counts);
        stream << ",\"animationFrameTriangleCounts\":";
        write_u32_array(stream, timing.animation_frame_triangle_counts);
        stream << '}'
               << (index + 1u == result.timings.sections.size()
                       ? "\n" : ",\n");
    }
    stream << "  ],\n  \"handoffs\": {";
    if (!result.timings.handoffs.empty()) stream << '\n';
    for (std::size_t index = 0u;
         index != result.timings.handoffs.size(); ++index) {
        const auto& timing = result.timings.handoffs[index];
        stream << "    " << std::quoted(timing.id) << ":{"
               << "\"staticCacheHit\":" << std::boolalpha
               << timing.static_cache_hit
               << ",\"animationCacheHit\":"
               << timing.animation_cache_hit
               << ",\"animationFrameMs\":";
        write_frame_ms(stream, timing.animation_frame_ms);
        stream << ",\"animationFileBytes\":"
               << timing.animation_file_bytes
               << ",\"boundarySourceBytes\":"
               << timing.boundary_source_bytes
               << ",\"peakBuildCpuPayloadBytes\":"
               << timing.peak_build_cpu_payload_bytes
               << ",\"sourceBlendRequired\":"
               << timing.source_blend_required
               << ",\"upstreamCut\":";
        write_cut_array(stream, timing.upstream_cut);
        stream << ",\"downstreamCut\":";
        write_cut_array(stream, timing.downstream_cut);
        stream << '}'
               << (index + 1u == result.timings.handoffs.size()
                       ? "\n" : ",\n");
    }
    stream << "  },\n  \"networkPeakBuildCpuPayloadBytes\":"
           << result.timings.peak_build_cpu_payload_bytes
           << ",\n  \"handoffMeshMs\":"
           << result.timings.handoff_mesh_ms
           << ",\n  \"handoffAnimationMeshMs\":"
           << result.timings.handoff_animation_mesh_ms
           << ",\n  \"serializeMs\":" << result.timings.serialize_ms
           << ",\n  \"totalWallMs\":" << result.timings.total_wall_ms
           << ",\n  \"visualVertices\":"
           << result.products.visual_mesh.positions.size() / 3u
           << ",\n  \"visualTriangles\":"
           << result.products.visual_mesh.indices.size() / 3u
           << "\n}\n";
    return stream.str();
}

}  // namespace hydrology
