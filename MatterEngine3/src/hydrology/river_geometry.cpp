#include "river_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace hydrology {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMaximumCurvaturePerM = 0.20f;
constexpr float kNoisePeriodM = 16.0f;

struct DensePoint {
    matter::Float3 base{};
    matter::Float3 lateral{};
    float base_distance_m = 0.0f;
    float hard_envelope = 0.0f;
};

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

matter::Float3 normalize(matter::Float3 value) {
    const float magnitude = length(value);
    return magnitude > 1.0e-8f ? multiply(value, 1.0f / magnitude)
                               : matter::Float3{1.0f, 0.0f, 0.0f};
}

matter::Float3 horizontal_lateral(matter::Float3 direction) {
    const float magnitude = std::hypot(direction.x, direction.z);
    return magnitude > 1.0e-8f
               ? matter::Float3{-direction.z / magnitude, 0.0f,
                                direction.x / magnitude}
               : matter::Float3{0.0f, 0.0f, 1.0f};
}

matter::Float3 lerp(matter::Float3 a, matter::Float3 b, float t) {
    return add(a, multiply(subtract(b, a), t));
}

float distance(matter::Float3 a, matter::Float3 b) {
    return length(subtract(b, a));
}

matter::Float3 hermite(matter::Float3 p0, matter::Float3 p1,
                       matter::Float3 m0, matter::Float3 m1, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return add(add(multiply(p0, 2.0f * t3 - 3.0f * t2 + 1.0f),
                   multiply(m0, t3 - 2.0f * t2 + t)),
               add(multiply(p1, -2.0f * t3 + 3.0f * t2),
                   multiply(m1, t3 - t2)));
}

matter::Float3 hermite_derivative(matter::Float3 p0, matter::Float3 p1,
                                  matter::Float3 m0, matter::Float3 m1,
                                  float t) {
    const float t2 = t * t;
    return add(add(multiply(p0, 6.0f * t2 - 6.0f * t),
                   multiply(m0, 3.0f * t2 - 4.0f * t + 1.0f)),
               add(multiply(p1, -6.0f * t2 + 6.0f * t),
                   multiply(m1, 3.0f * t2 - 2.0f * t)));
}

std::uint64_t mix64(std::uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

std::uint64_t hash_string(std::uint64_t hash, const std::string& text) {
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::uint64_t keyed_hash(std::uint64_t seed, const std::string& name,
                         std::uint64_t index, std::uint64_t salt) {
    std::uint64_t value = hash_string(seed ^ UINT64_C(14695981039346656037),
                                     name);
    value ^= mix64(index + salt);
    return mix64(value);
}

float unit_float(std::uint64_t hash) {
    return static_cast<float>((hash >> 40) & UINT64_C(0xffffff)) /
           16777216.0f;
}

float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

float value_noise(std::uint64_t seed, const std::string& name,
                  float distance_m) {
    const float coordinate = distance_m / kNoisePeriodM;
    const std::uint64_t index = static_cast<std::uint64_t>(
        std::max(0.0f, std::floor(coordinate)));
    const float fraction = coordinate - std::floor(coordinate);
    const float first = unit_float(keyed_hash(seed, name, index,
                                               UINT64_C(0x6d65616e64657231))) *
                            2.0f -
                        1.0f;
    const float second = unit_float(keyed_hash(seed, name, index + 1u,
                                                UINT64_C(0x6d65616e64657231))) *
                             2.0f -
                         1.0f;
    return first + (second - first) * smoothstep(fraction);
}

const matter::RiverReach& reach_at(const matter::RiverDefinition& river,
                                   float distance_m) {
    for (const matter::RiverReach& reach : river.reaches) {
        if (distance_m <= reach.until_m) return reach;
    }
    return river.reaches.back();
}

float meander_response(const matter::RiverDefinition& river,
                        float distance_m) {
    const auto response = [](const matter::RiverReach& reach) {
        return reach.meander / std::max(std::fabs(reach.base_grade), 0.005f);
    };

    const matter::RiverReach& current = reach_at(river, distance_m);
    float result = response(current);
    for (std::size_t i = 0; i + 1 < river.reaches.size(); ++i) {
        const float boundary = river.reaches[i].until_m;
        const float blend_half_width = 4.0f;
        if (distance_m >= boundary - blend_half_width &&
            distance_m <= boundary + blend_half_width) {
            const float t = smoothstep((distance_m - boundary + blend_half_width) /
                                       (2.0f * blend_half_width));
            result = response(river.reaches[i]) +
                     (response(river.reaches[i + 1]) -
                      response(river.reaches[i])) *
                         t;
            break;
        }
    }
    return result;
}

float width_scale_at(const matter::RiverDefinition& river,
                     float distance_m) {
    const matter::RiverReach& current = reach_at(river, distance_m);
    float result = current.width_scale;
    constexpr float blend_half_width = 8.0f;
    for (std::size_t i = 0; i + 1 < river.reaches.size(); ++i) {
        const float boundary = river.reaches[i].until_m;
        if (distance_m < boundary - blend_half_width ||
            distance_m > boundary + blend_half_width)
            continue;
        const float t = smoothstep(
            (distance_m - boundary + blend_half_width) /
            (2.0f * blend_half_width));
        result = river.reaches[i].width_scale +
                 (river.reaches[i + 1].width_scale -
                  river.reaches[i].width_scale) * t;
        break;
    }
    return result;
}

bool fail(std::string& error, const std::string& message) {
    error = "river geometry: " + message;
    return false;
}

bool validate(const matter::RiverNetworkDefinition& network,
              const matter::RiverDefinition*& river, std::string& error) {
    if (!finite(network.cell_size_m) || network.cell_size_m <= 0.0f)
        return fail(error, "cell size must be finite and positive");
    if (network.first_section_river.empty())
        return fail(error, "first-section river is required");
    river = nullptr;
    for (const matter::RiverDefinition& candidate : network.rivers) {
        if (candidate.name == network.first_section_river) {
            if (river) return fail(error, "first-section river name is ambiguous");
            river = &candidate;
        }
    }
    if (!river) return fail(error, "first-section river was not found");
    if (river->spline.size() < 2u)
        return fail(error, "spline requires at least two hard controls");
    for (const matter::Float3 point : river->spline)
        if (!finite(point)) return fail(error, "spline contains a nonfinite control");
    if (river->reaches.empty()) return fail(error, "at least one reach is required");
    float previous_until = 0.0f;
    for (const matter::RiverReach& reach : river->reaches) {
        if (!finite(reach.until_m) || reach.until_m <= previous_until)
            return fail(error, "reach order is ambiguous");
        if (!finite(reach.base_grade) || reach.base_grade >= 0.0f)
            return fail(error, "every reach grade must be finite and negative");
        if (!finite(reach.meander) || reach.meander < 0.0f)
            return fail(error, "reach meander must be finite and nonnegative");
        if (!finite(reach.width_scale) || reach.width_scale <= 0.0f ||
            reach.width_scale > 3.0f)
            return fail(error, "reach width scale must lie in (0, 3]");
        previous_until = reach.until_m;
    }
    if (!finite(river->channel.width_m) || river->channel.width_m <= 0.0f ||
        !finite(river->channel.depth_m) || river->channel.depth_m <= 0.0f)
        return fail(error, "channel dimensions must be finite and positive");
    if (!finite(river->boulders.density) || river->boulders.density < 0.0f ||
        river->boulders.density > 1.0f)
        return fail(error, "boulder density must lie in [0, 1]");
    if (!finite(river->boulders.radius_m.x) ||
        !finite(river->boulders.radius_m.y) ||
        river->boulders.radius_m.x <= 0.0f ||
        river->boulders.radius_m.y < river->boulders.radius_m.x ||
        river->boulders.radius_m.y > river->channel.width_m * 0.5f)
        return fail(error, "boulder radii do not fit the channel envelope");
    return true;
}

std::vector<matter::Float3> control_tangents(
    const std::vector<matter::Float3>& controls) {
    std::vector<matter::Float3> tangents(controls.size());
    tangents.front() = subtract(controls[1], controls[0]);
    tangents.back() = subtract(controls.back(), controls[controls.size() - 2]);
    for (std::size_t i = 1; i + 1 < controls.size(); ++i)
        tangents[i] = multiply(subtract(controls[i + 1], controls[i - 1]),
                               0.5f);
    return tangents;
}

std::vector<DensePoint> build_dense_base(const matter::RiverDefinition& river,
                                         float cell_size_m) {
    const auto tangents = control_tangents(river.spline);
    std::vector<DensePoint> dense;
    float accumulated = 0.0f;
    for (std::size_t segment = 0; segment + 1 < river.spline.size(); ++segment) {
        const matter::Float3 p0 = river.spline[segment];
        const matter::Float3 p1 = river.spline[segment + 1];
        const matter::Float3 m0 = tangents[segment];
        const matter::Float3 m1 = tangents[segment + 1];
        const float chord = distance(p0, p1);
        const std::size_t subdivisions = static_cast<std::size_t>(
            std::max(64.0f, std::ceil(chord / (cell_size_m * 0.125f))));
        for (std::size_t step = segment == 0 ? 0u : 1u;
             step <= subdivisions; ++step) {
            const float t = static_cast<float>(step) /
                            static_cast<float>(subdivisions);
            DensePoint point{};
            point.base = hermite(p0, p1, m0, m1, t);
            point.lateral = horizontal_lateral(
                hermite_derivative(p0, p1, m0, m1, t));
            point.hard_envelope = std::sin(kPi * t);
            if (!dense.empty()) accumulated += distance(dense.back().base, point.base);
            point.base_distance_m = accumulated;
            dense.push_back(point);
        }
    }
    return dense;
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

template <typename Position>
bool has_self_intersection(std::size_t point_count, Position position) {
    if (point_count < 4u) return false;
    for (std::size_t first = 0; first + 1 < point_count; ++first) {
        const matter::Float3 a = position(first);
        const matter::Float3 b = position(first + 1);
        for (std::size_t second = first + 2; second + 1 < point_count; ++second) {
            if (segments_intersect(a, b, position(second), position(second + 1)))
                return true;
        }
    }
    return false;
}

std::vector<matter::Float3> displace_dense(
    const matter::RiverNetworkDefinition& network,
    const matter::RiverDefinition& river, const std::vector<DensePoint>& dense,
    float scale) {
    std::vector<matter::Float3> displaced;
    displaced.reserve(dense.size());
    const float amplitude_scale = river.channel.width_m * 0.008f * scale;
    const float envelope_limit = river.channel.width_m * 0.35f;
    for (const DensePoint& point : dense) {
        float offset = amplitude_scale *
                       meander_response(river, point.base_distance_m) *
                       value_noise(network.seed, river.name,
                                   point.base_distance_m) *
                       point.hard_envelope;
        offset = std::clamp(offset, -envelope_limit, envelope_limit);
        displaced.push_back(add(point.base, multiply(point.lateral, offset)));
    }
    return displaced;
}

std::vector<RiverCentrelineSample> resample(
    const matter::RiverDefinition& river,
    const std::vector<matter::Float3>& dense, float spacing_m) {
    std::vector<float> cumulative(dense.size(), 0.0f);
    for (std::size_t i = 1; i < dense.size(); ++i)
        cumulative[i] = cumulative[i - 1] + distance(dense[i - 1], dense[i]);

    const float total = cumulative.back();
    const std::size_t full_steps = static_cast<std::size_t>(
        std::floor(total / spacing_m));
    std::vector<RiverCentrelineSample> result;
    result.reserve(full_steps + 2u);
    std::size_t dense_index = 1u;
    const auto append = [&](float target, std::size_t& index,
                            std::vector<RiverCentrelineSample>& samples) {
        while (index + 1 < cumulative.size() && cumulative[index] < target)
            ++index;
        const float previous_distance = cumulative[index - 1];
        const float span = cumulative[index] - previous_distance;
        const float t = span > 0.0f ? (target - previous_distance) / span : 0.0f;
        RiverCentrelineSample sample{};
        sample.position_m = lerp(dense[index - 1], dense[index],
                                 std::clamp(t, 0.0f, 1.0f));
        sample.distance_m = target;
        const matter::RiverReach& reach = reach_at(river, target);
        sample.grade = reach.base_grade;
        sample.meander = reach.meander;
        sample.width_scale = width_scale_at(river, target);
        samples.push_back(sample);
    };

    RiverCentrelineSample first{};
    first.position_m = dense.front();
    first.distance_m = 0.0f;
    first.grade = river.reaches.front().base_grade;
    first.meander = river.reaches.front().meander;
    first.width_scale = river.reaches.front().width_scale;
    result.push_back(first);
    for (std::size_t step = 1; step <= full_steps; ++step)
        append(static_cast<float>(step) * spacing_m, dense_index, result);
    if (result.size() == 1u ||
        total - result.back().distance_m > 1.0e-4f)
        append(total, dense_index, result);

    for (std::size_t i = 0; i < result.size(); ++i) {
        const matter::Float3 direction =
            i == 0u ? subtract(result[1].position_m, result[0].position_m)
                    : (i + 1u == result.size()
                           ? subtract(result[i].position_m,
                                      result[i - 1].position_m)
                           : subtract(result[i + 1].position_m,
                                      result[i - 1].position_m));
        result[i].tangent = normalize(direction);
        result[i].lateral = horizontal_lateral(direction);
    }
    return result;
}

float maximum_curvature(const std::vector<RiverCentrelineSample>& samples) {
    float maximum = 0.0f;
    for (std::size_t i = 1; i + 1 < samples.size(); ++i) {
        const matter::Float3 a = samples[i - 1].tangent;
        const matter::Float3 b = samples[i + 1].tangent;
        const float a_length = std::hypot(a.x, a.z);
        const float b_length = std::hypot(b.x, b.z);
        if (a_length <= 1.0e-6f || b_length <= 1.0e-6f) continue;
        const float cosine = std::clamp((a.x * b.x + a.z * b.z) /
                                            (a_length * b_length),
                                        -1.0f, 1.0f);
        const float span = samples[i + 1].distance_m -
                           samples[i - 1].distance_m;
        if (span > 0.0f) maximum = std::max(maximum, std::acos(cosine) / span);
    }
    return maximum;
}

RiverCentrelineSample sample_at(
    const std::vector<RiverCentrelineSample>& samples, float distance_m) {
    const auto after = std::lower_bound(
        samples.begin(), samples.end(), distance_m,
        [](const RiverCentrelineSample& sample, float value) {
            return sample.distance_m < value;
        });
    if (after == samples.begin()) return samples.front();
    if (after == samples.end()) return samples.back();
    const RiverCentrelineSample& before = *(after - 1);
    const float span = after->distance_m - before.distance_m;
    const float t = span > 0.0f ? (distance_m - before.distance_m) / span : 0.0f;
    RiverCentrelineSample result = before;
    result.position_m = lerp(before.position_m, after->position_m, t);
    result.tangent = normalize(lerp(before.tangent, after->tangent, t));
    result.lateral = horizontal_lateral(result.tangent);
    result.distance_m = distance_m;
    result.width_scale = before.width_scale +
                         (after->width_scale - before.width_scale) * t;
    return result;
}

std::vector<RiverBoulder> select_boulders(
    const matter::RiverNetworkDefinition& network,
    const matter::RiverDefinition& river,
    const std::vector<RiverCentrelineSample>& centreline) {
    std::vector<RiverBoulder> result;
    if (river.boulders.density <= 0.0f) return result;

    const float maximum_width_scale = std::max_element(
        river.reaches.begin(), river.reaches.end(),
        [](const auto& a, const auto& b) {
            return a.width_scale < b.width_scale;
        })->width_scale;
    const float maximum_half_width =
        river.channel.width_m * maximum_width_scale * 0.5f;
    const float inlet_reserve = std::max(river.channel.width_m * maximum_width_scale,
                                         river.boulders.radius_m.y * 2.0f);
    const float cross_section_reserve =
        std::max(maximum_half_width, river.boulders.radius_m.y);
    const float end = centreline.back().distance_m - cross_section_reserve;
    const std::uint64_t first_index = static_cast<std::uint64_t>(
        std::ceil(inlet_reserve));
    const std::uint64_t last_index = end > 0.0f
        ? static_cast<std::uint64_t>(std::floor(end))
        : 0u;

    for (std::uint64_t index = first_index; index <= last_index; ++index) {
        const float distance_m = static_cast<float>(index);
        if (std::fabs(distance_m - network.first_section.minimum_length_m) <
            cross_section_reserve)
            continue;
        const std::uint64_t choose = keyed_hash(
            network.seed, river.name, index, UINT64_C(0x626f756c64657231));
        if (unit_float(choose) >= river.boulders.density) continue;

        const float radius_t = unit_float(keyed_hash(
            network.seed, river.name, index, UINT64_C(0x626f756c64657232)));
        const float radius = river.boulders.radius_m.x +
                             (river.boulders.radius_m.y -
                              river.boulders.radius_m.x) *
                                 radius_t;
        const RiverCentrelineSample sample = sample_at(centreline, distance_m);
        const float local_half_width = river.channel.width_m *
                                       sample.width_scale * 0.5f;
        const float available_lateral =
            std::max(0.0f, local_half_width - radius);
        const float lateral_t = unit_float(keyed_hash(
            network.seed, river.name, index, UINT64_C(0x626f756c64657233)));
        const float lateral_offset = (lateral_t * 2.0f - 1.0f) *
                                     available_lateral;
        RiverBoulder boulder{};
        boulder.center_m = add(sample.position_m,
                               multiply(sample.lateral, lateral_offset));
        boulder.radius_m = radius;
        result.push_back(boulder);
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

matter::Aabb geometry_bounds(const matter::RiverDefinition& river,
                             const RiverGeometry& geometry) {
    const float infinity = std::numeric_limits<float>::infinity();
    matter::Aabb bounds{{infinity, infinity, infinity},
                        {-infinity, -infinity, -infinity}};
    for (const RiverCentrelineSample& sample : geometry.centreline) {
        const float half_width = river.channel.width_m *
                                 sample.width_scale * 0.5f;
        include(bounds, {sample.position_m.x - half_width,
                         sample.position_m.y - river.channel.depth_m,
                         sample.position_m.z - half_width});
        include(bounds, {sample.position_m.x + half_width,
                         sample.position_m.y + river.channel.depth_m,
                         sample.position_m.z + half_width});
    }
    for (const RiverBoulder& boulder : geometry.boulders) {
        include(bounds, {boulder.center_m.x - boulder.radius_m,
                         boulder.center_m.y - boulder.radius_m,
                         boulder.center_m.z - boulder.radius_m});
        include(bounds, {boulder.center_m.x + boulder.radius_m,
                         boulder.center_m.y + boulder.radius_m,
                         boulder.center_m.z + boulder.radius_m});
    }
    return bounds;
}

void hash_bytes(std::uint64_t& hash, const void* bytes, std::size_t size) {
    const auto* data = static_cast<const unsigned char*>(bytes);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
}

std::uint64_t geometry_revision(const matter::RiverNetworkDefinition& network,
                                const matter::RiverDefinition& river,
                                const RiverGeometry& geometry) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    hash_bytes(hash, &network.seed, sizeof(network.seed));
    hash = hash_string(hash, river.name);
    for (const RiverCentrelineSample& sample : geometry.centreline) {
        hash_bytes(hash, &sample.position_m, sizeof(sample.position_m));
        hash_bytes(hash, &sample.grade, sizeof(sample.grade));
        hash_bytes(hash, &sample.meander, sizeof(sample.meander));
        hash_bytes(hash, &sample.width_scale, sizeof(sample.width_scale));
    }
    for (const RiverBoulder& boulder : geometry.boulders)
        hash_bytes(hash, &boulder, sizeof(boulder));
    hash_bytes(hash, &geometry.bounds_m.minimum,
               sizeof(geometry.bounds_m.minimum));
    hash_bytes(hash, &geometry.bounds_m.maximum,
               sizeof(geometry.bounds_m.maximum));
    return hash;
}

} // namespace

bool build_river_geometry(const matter::RiverNetworkDefinition& network,
                          RiverGeometry& out, std::string& error) {
    const matter::RiverDefinition* river = nullptr;
    if (!validate(network, river, error)) return false;

    const std::vector<DensePoint> base =
        build_dense_base(*river, network.cell_size_m);
    if (base.size() < 2u || base.back().base_distance_m <= 1.0e-4f)
        return fail(error, "spline is too short to sample safely");
    if (has_self_intersection(base.size(),
                              [&](std::size_t i) { return base[i].base; }))
        return fail(error, "spline has a non-neighbour self-intersection");

    RiverGeometry result{};
    float meander_scale = 1.0f;
    bool accepted = false;
    bool every_candidate_intersected = true;
    for (int attempt = 0; attempt < 9; ++attempt) {
        const std::vector<matter::Float3> displaced =
            displace_dense(network, *river, base, meander_scale);
        if (has_self_intersection(displaced.size(),
                                  [&](std::size_t i) { return displaced[i]; })) {
            meander_scale *= 0.5f;
            continue;
        }
        every_candidate_intersected = false;
        result.centreline = resample(*river, displaced,
                                     network.cell_size_m * 0.5f);
        if (maximum_curvature(result.centreline) <= kMaximumCurvaturePerM) {
            accepted = true;
            break;
        }
        meander_scale *= 0.5f;
    }
    if (!accepted && every_candidate_intersected)
        return fail(error,
                    "generated meander self-intersection after attenuation");
    if (!accepted)
        return fail(error, "spline exceeds the bounded curvature limit");

    result.boulders = select_boulders(network, *river, result.centreline);
    result.bounds_m = geometry_bounds(*river, result);
    result.revision = geometry_revision(network, *river, result);
    error.clear();
    out = std::move(result);
    return true;
}

} // namespace hydrology
