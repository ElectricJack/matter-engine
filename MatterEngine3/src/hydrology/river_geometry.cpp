#include "river_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace hydrology {
namespace {

constexpr float kMaximumCurvaturePerM = 0.20f;
constexpr float kMinimumSegmentM = 1.0e-4f;

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

bool finite(float value) { return std::isfinite(value); }

bool finite(matter::Float3 value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

matter::Float3 add(matter::Float3 a, matter::Float3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

matter::Float3 subtract(matter::Float3 a, matter::Float3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

matter::Float3 multiply(matter::Float3 value, float scalar) {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

float length(matter::Float3 value) {
    return std::sqrt(value.x * value.x + value.y * value.y +
                     value.z * value.z);
}

float distance(matter::Float3 a, matter::Float3 b) {
    return length(subtract(b, a));
}

matter::Float3 normalize(matter::Float3 value) {
    const float magnitude = length(value);
    return magnitude > kMinimumSegmentM
               ? multiply(value, 1.0f / magnitude)
               : matter::Float3{1.0f, 0.0f, 0.0f};
}

matter::Float3 horizontal_lateral(matter::Float3 direction) {
    const float magnitude = std::hypot(direction.x, direction.z);
    return magnitude > kMinimumSegmentM
               ? matter::Float3{-direction.z / magnitude, 0.0f,
                                direction.x / magnitude}
               : matter::Float3{0.0f, 0.0f, 1.0f};
}

matter::Float3 lerp(matter::Float3 a, matter::Float3 b, float t) {
    return add(a, multiply(subtract(b, a), t));
}

float orient(matter::Float3 a, matter::Float3 b, matter::Float3 c) {
    return (b.x - a.x) * (c.z - a.z) -
           (b.z - a.z) * (c.x - a.x);
}

bool on_segment(matter::Float3 a, matter::Float3 b, matter::Float3 point) {
    constexpr float epsilon = 1.0e-5f;
    return point.x >= std::min(a.x, b.x) - epsilon &&
           point.x <= std::max(a.x, b.x) + epsilon &&
           point.z >= std::min(a.z, b.z) - epsilon &&
           point.z <= std::max(a.z, b.z) + epsilon;
}

bool segments_intersect(matter::Float3 a, matter::Float3 b,
                        matter::Float3 c, matter::Float3 d) {
    constexpr float epsilon = 1.0e-5f;
    const float o1 = orient(a, b, c);
    const float o2 = orient(a, b, d);
    const float o3 = orient(c, d, a);
    const float o4 = orient(c, d, b);
    if (((o1 > epsilon && o2 < -epsilon) ||
         (o1 < -epsilon && o2 > epsilon)) &&
        ((o3 > epsilon && o4 < -epsilon) ||
         (o3 < -epsilon && o4 > epsilon)))
        return true;
    if (std::fabs(o1) <= epsilon && on_segment(a, b, c)) return true;
    if (std::fabs(o2) <= epsilon && on_segment(a, b, d)) return true;
    if (std::fabs(o3) <= epsilon && on_segment(c, d, a)) return true;
    if (std::fabs(o4) <= epsilon && on_segment(c, d, b)) return true;
    return false;
}

bool has_self_intersection(const std::vector<matter::Float3>& curve) {
    if (curve.size() < 4u) return false;
    for (std::size_t first = 0; first + 1u < curve.size(); ++first) {
        for (std::size_t second = first + 2u;
             second + 1u < curve.size(); ++second) {
            if (segments_intersect(curve[first], curve[first + 1u],
                                   curve[second], curve[second + 1u]))
                return true;
        }
    }
    return false;
}

bool is_declared_hydraulic_transition(
    const matter::RiverNetworkDefinition& network,
    std::string_view river_id, float distance_m) {
    const float tolerance_m = std::max(kMinimumSegmentM,
                                       network.cell_size_m * 0.5f);
    for (const auto& section : network.sections) {
        if (section.river != river_id) continue;
        for (const auto& waterfall : section.waterfalls) {
            if (!finite(waterfall.lip_distance_m) ||
                !finite(waterfall.landing_distance_m) ||
                !finite(waterfall.expected_drop_m) ||
                waterfall.lip_distance_m < 0.0f ||
                waterfall.landing_distance_m <= waterfall.lip_distance_m ||
                waterfall.expected_drop_m <= 0.0f)
                continue;
            if (distance_m >= waterfall.lip_distance_m - tolerance_m &&
                distance_m <= waterfall.landing_distance_m + tolerance_m)
                return true;
        }
        if (section.terminal_spillway) {
            const auto& spillway = *section.terminal_spillway;
            if (!spillway.id.empty() && finite(spillway.distance_m) &&
                finite(spillway.width_m) &&
                finite(spillway.effective_depth_m) &&
                finite(spillway.overlap_m) && finite(spillway.dam_offset_m) &&
                spillway.distance_m >= 0.0f && spillway.width_m > 0.0f &&
                spillway.effective_depth_m > 0.0f &&
                spillway.overlap_m > 0.0f && spillway.dam_offset_m >= 0.0f &&
                std::fabs(distance_m - spillway.distance_m) <= tolerance_m)
                return true;
        }
    }
    return false;
}

float maximum_authored_curvature(
    const matter::RiverNetworkDefinition& network,
    std::string_view river_id,
    const std::vector<matter::Float3>& curve,
    const std::vector<float>& cumulative,
    float& maximum_distance_m) {
    float maximum = 0.0f;
    maximum_distance_m = 0.0f;
    for (std::size_t i = 1u; i + 1u < curve.size(); ++i) {
        // Waterfall lips and landings are intentionally discontinuous channel
        // transitions. Their sharpness is authored explicitly and validated
        // against the declared drop by river_section_graph; ordinary river
        // corners remain subject to the native curvature bound.
        if (is_declared_hydraulic_transition(network, river_id, cumulative[i]))
            continue;
        const matter::Float3 before = subtract(curve[i], curve[i - 1u]);
        const matter::Float3 after = subtract(curve[i + 1u], curve[i]);
        const float before_horizontal = std::hypot(before.x, before.z);
        const float after_horizontal = std::hypot(after.x, after.z);
        if (before_horizontal <= kMinimumSegmentM ||
            after_horizontal <= kMinimumSegmentM)
            continue;
        const float cosine = std::clamp(
            (before.x * after.x + before.z * after.z) /
                (before_horizontal * after_horizontal),
            -1.0f, 1.0f);
        const float support = std::min(distance(curve[i - 1u], curve[i]),
                                       distance(curve[i], curve[i + 1u]));
        if (support > kMinimumSegmentM) {
            const float curvature = std::acos(cosine) / support;
            if (curvature > maximum) {
                maximum = curvature;
                maximum_distance_m = cumulative[i];
            }
        }
    }
    return maximum;
}

matter::RiverChannelProfilePoint profile_at(
    const std::vector<matter::RiverChannelProfilePoint>& profile,
    float distance_m) {
    if (distance_m <= profile.front().distance_m) return profile.front();
    const auto after = std::lower_bound(
        profile.begin(), profile.end(), distance_m,
        [](const matter::RiverChannelProfilePoint& point, float value) {
            return point.distance_m < value;
        });
    if (after == profile.end()) return profile.back();
    const auto& before = *(after - 1);
    const float span = after->distance_m - before.distance_m;
    const float t = (distance_m - before.distance_m) / span;
    matter::RiverChannelProfilePoint result{};
    result.distance_m = distance_m;
    result.width_m = before.width_m + (after->width_m - before.width_m) * t;
    result.depth_m = before.depth_m + (after->depth_m - before.depth_m) * t;
    result.asymmetry = before.asymmetry +
                       (after->asymmetry - before.asymmetry) * t;
    return result;
}

bool validate(const matter::RiverNetworkDefinition& network,
              const std::string& river_id,
              const matter::RiverDefinition*& river,
              std::vector<float>& cumulative,
              std::string& error) {
    if (!finite(network.cell_size_m) || network.cell_size_m <= 0.0f)
        return fail(error, "river geometry cell size must be finite and positive");
    river = nullptr;
    for (const auto& candidate : network.rivers) {
        if (candidate.name != river_id) continue;
        if (river) return fail(error, "river id is ambiguous");
        river = &candidate;
    }
    if (!river) return fail(error, "river id was not found");
    if (river->curve.size() < 2u)
        return fail(error, "curve requires at least two authored points");

    cumulative.assign(river->curve.size(), 0.0f);
    for (std::size_t i = 0u; i < river->curve.size(); ++i) {
        if (!finite(river->curve[i]))
            return fail(error, "curve contains a nonfinite point");
        if (i == 0u) continue;
        const float segment = distance(river->curve[i - 1u], river->curve[i]);
        if (!finite(segment) || segment <= kMinimumSegmentM)
            return fail(error, "curve contains a zero-length segment");
        if (std::hypot(river->curve[i].x - river->curve[i - 1u].x,
                       river->curve[i].z - river->curve[i - 1u].z) <=
            kMinimumSegmentM)
            return fail(error, "curve contains a horizontally degenerate segment");
        cumulative[i] = cumulative[i - 1u] + segment;
    }
    if (has_self_intersection(river->curve))
        return fail(error, "curve has a non-neighbour self-intersection");
    float maximum_curvature_distance_m = 0.0f;
    const float maximum_curvature = maximum_authored_curvature(
        network, river_id, river->curve, cumulative,
        maximum_curvature_distance_m);
    if (maximum_curvature > kMaximumCurvaturePerM) {
        error = "curve exceeds the bounded curvature limit at distance " +
                std::to_string(maximum_curvature_distance_m) +
                " m (curvature " + std::to_string(maximum_curvature) +
                " per m)";
        return false;
    }

    if (river->channel_profile.empty())
        return fail(error, "channel profile requires at least one point");
    float previous = -1.0f;
    for (const auto& point : river->channel_profile) {
        if (!finite(point.distance_m) || point.distance_m < 0.0f)
            return fail(error, "channel profile distance must be finite and nonnegative");
        if (point.distance_m <= previous)
            return fail(error, "channel profile distances must strictly increase");
        if (!finite(point.width_m) || point.width_m <= 0.0f ||
            !finite(point.depth_m) || point.depth_m <= 0.0f)
            return fail(error, "channel profile dimensions must be finite and positive");
        if (!finite(point.asymmetry) || std::fabs(point.asymmetry) > 1.0f)
            return fail(error, "channel profile asymmetry must lie in [-1, 1]");
        previous = point.distance_m;
    }
    return true;
}

std::vector<RiverCentrelineSample> resample(
    const matter::RiverDefinition& river,
    const std::vector<float>& cumulative, float spacing_m) {
    const float total = cumulative.back();
    const std::size_t full_steps = static_cast<std::size_t>(
        std::floor(total / spacing_m));
    std::vector<RiverCentrelineSample> result;
    result.reserve(full_steps + 2u);
    std::size_t curve_index = 1u;

    const auto append = [&](float target, std::size_t& index,
                            std::vector<RiverCentrelineSample>& samples) {
        while (index + 1u < cumulative.size() && cumulative[index] < target)
            ++index;
        const float before_distance = cumulative[index - 1u];
        const float span = cumulative[index] - before_distance;
        const float t = std::clamp((target - before_distance) / span,
                                   0.0f, 1.0f);
        RiverCentrelineSample sample{};
        sample.position_m = lerp(river.curve[index - 1u], river.curve[index], t);
        sample.distance_m = target;
        const auto profile = profile_at(river.channel_profile, target);
        sample.width_m = profile.width_m;
        sample.depth_m = profile.depth_m;
        sample.asymmetry = profile.asymmetry;
        samples.push_back(sample);
    };

    append(0.0f, curve_index, result);
    for (std::size_t step = 1u; step <= full_steps; ++step)
        append(static_cast<float>(step) * spacing_m, curve_index, result);
    if (total - result.back().distance_m > 1.0e-4f)
        append(total, curve_index, result);
    else
        result.back().position_m = river.curve.back();

    for (std::size_t i = 0u; i < result.size(); ++i) {
        const matter::Float3 direction =
            i == 0u ? subtract(result[1u].position_m, result[0u].position_m)
                    : (i + 1u == result.size()
                           ? subtract(result[i].position_m,
                                      result[i - 1u].position_m)
                           : subtract(result[i + 1u].position_m,
                                      result[i - 1u].position_m));
        result[i].tangent = normalize(direction);
        result[i].lateral = horizontal_lateral(direction);
    }
    return result;
}

void include(matter::Aabb& bounds, matter::Float3 point) {
    bounds.minimum.x = std::min(bounds.minimum.x, point.x);
    bounds.minimum.y = std::min(bounds.minimum.y, point.y);
    bounds.minimum.z = std::min(bounds.minimum.z, point.z);
    bounds.maximum.x = std::max(bounds.maximum.x, point.x);
    bounds.maximum.y = std::max(bounds.maximum.y, point.y);
    bounds.maximum.z = std::max(bounds.maximum.z, point.z);
}

matter::Aabb geometry_bounds(
    const std::vector<RiverCentrelineSample>& centreline) {
    const float infinity = std::numeric_limits<float>::infinity();
    matter::Aabb bounds{{infinity, infinity, infinity},
                        {-infinity, -infinity, -infinity}};
    for (const auto& sample : centreline) {
        const float half_width = sample.width_m * 0.5f;
        include(bounds, {sample.position_m.x - half_width,
                         sample.position_m.y - sample.depth_m,
                         sample.position_m.z - half_width});
        include(bounds, {sample.position_m.x + half_width,
                         sample.position_m.y + sample.depth_m,
                         sample.position_m.z + half_width});
    }
    return bounds;
}

void hash_bytes(std::uint64_t& hash, const void* bytes, std::size_t size) {
    const auto* data = static_cast<const unsigned char*>(bytes);
    for (std::size_t i = 0u; i < size; ++i) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
}

void hash_text(std::uint64_t& hash, std::string_view text) {
    hash_bytes(hash, text.data(), text.size());
}

std::uint64_t geometry_revision(
    const std::string& river_id, const RiverGeometry& geometry) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    hash_text(hash, river_id);
    for (const auto& sample : geometry.centreline) {
        hash_bytes(hash, &sample.position_m, sizeof(sample.position_m));
        hash_bytes(hash, &sample.tangent, sizeof(sample.tangent));
        hash_bytes(hash, &sample.lateral, sizeof(sample.lateral));
        hash_bytes(hash, &sample.distance_m, sizeof(sample.distance_m));
        hash_bytes(hash, &sample.width_m, sizeof(sample.width_m));
        hash_bytes(hash, &sample.depth_m, sizeof(sample.depth_m));
        hash_bytes(hash, &sample.asymmetry, sizeof(sample.asymmetry));
    }
    hash_bytes(hash, &geometry.bounds_m.minimum, sizeof(geometry.bounds_m.minimum));
    hash_bytes(hash, &geometry.bounds_m.maximum, sizeof(geometry.bounds_m.maximum));
    return hash;
}

} // namespace

bool build_river_geometry(const matter::RiverNetworkDefinition& network,
                          const std::string& river_id,
                          RiverGeometry& out, std::string& error) {
    const matter::RiverDefinition* river = nullptr;
    std::vector<float> cumulative;
    if (!validate(network, river_id, river, cumulative, error)) return false;

    RiverGeometry result{};
    result.centreline = resample(*river, cumulative, network.cell_size_m * 0.5f);
    result.bounds_m = geometry_bounds(result.centreline);
    result.revision = geometry_revision(river_id, result);
    error.clear();
    out = std::move(result);
    return true;
}

} // namespace hydrology
