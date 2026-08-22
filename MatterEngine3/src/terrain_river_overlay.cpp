#include "terrain_river_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace terrain_field {
namespace {

constexpr std::uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);

void hash_bytes(std::uint64_t& hash, const void* value, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(value);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
}

std::uint64_t mix64(std::uint64_t value) {
    value ^= value >> 30u;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27u;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31u);
}

float unit_float(std::uint64_t value) {
    return static_cast<float>((mix64(value) >> 40u) * (1.0 / 16777216.0));
}

float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float rounded_v(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    constexpr float roundness = 0.08f;
    const float floor = std::sqrt(roundness * roundness);
    const float ceiling = std::sqrt(1.0f + roundness * roundness);
    return (std::sqrt(t * t + roundness * roundness) - floor) /
           (ceiling - floor);
}

float broad_noise(float distance_m, std::uint64_t seed) {
    constexpr float wavelength_m = 12.0f;
    const float coordinate = distance_m / wavelength_m;
    const auto cell = static_cast<std::int64_t>(std::floor(coordinate));
    const float t = smoothstep(coordinate - static_cast<float>(cell));
    const float a = unit_float(seed ^ static_cast<std::uint64_t>(cell)) * 2.0f - 1.0f;
    const float b = unit_float(seed ^ static_cast<std::uint64_t>(cell + 1)) * 2.0f - 1.0f;
    return a + (b - a) * t;
}

bool finite(float value) {
    return std::isfinite(value);
}

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

} // namespace

bool RiverHeightOverlay::build(
    const hydrology::RiverGeometry& geometry,
    const matter::RiverChannel& channel,
    std::shared_ptr<const RiverHeightOverlay>& out,
    std::string& error) {
    if (geometry.centreline.size() < 2u)
        return fail(error, "river height overlay requires at least two centreline samples");
    if (!finite(channel.width_m) || !finite(channel.depth_m) ||
        !finite(channel.asymmetry) || channel.width_m <= 0.0f ||
        channel.depth_m <= 0.0f || std::fabs(channel.asymmetry) > 1.0f)
        return fail(error, "river height overlay channel parameters are invalid");

    auto result = std::shared_ptr<RiverHeightOverlay>(new RiverHeightOverlay());
    result->channel_ = channel;
    result->samples_.reserve(geometry.centreline.size());

    std::uint64_t terrain_seed = kFnvOffset;
    float previous_distance = -1.0f;
    for (const auto& source : geometry.centreline) {
        if (!finite(source.position_m.x) || !finite(source.position_m.y) ||
            !finite(source.position_m.z) || !finite(source.lateral.x) ||
            !finite(source.lateral.z) || !finite(source.distance_m) ||
            !finite(source.grade) || source.distance_m <= previous_distance)
            return fail(error, "river height overlay centreline is invalid");
        previous_distance = source.distance_m;
        hash_bytes(terrain_seed, &source.position_m, sizeof(source.position_m));
        hash_bytes(terrain_seed, &source.grade, sizeof(source.grade));
    }
    result->terrain_seed_ = terrain_seed;

    std::vector<float> raw(geometry.centreline.size());
    raw.front() = geometry.centreline.front().position_m.y;
    const float noise_amplitude = std::min(channel.depth_m * 0.22f, 0.65f);
    const float inlet_noise = broad_noise(0.0f, terrain_seed);
    for (std::size_t i = 1; i < geometry.centreline.size(); ++i) {
        const float step = geometry.centreline[i].distance_m -
                           geometry.centreline[i - 1].distance_m;
        raw[i] = raw[i - 1] + geometry.centreline[i - 1].grade * step;
    }
    for (std::size_t i = 0; i < raw.size(); ++i)
        raw[i] += noise_amplitude *
                  (broad_noise(geometry.centreline[i].distance_m, terrain_seed) -
                   inlet_noise);

    // A compact symmetric kernel removes sub-cell chatter. The following pin
    // is load-bearing: broad seeded relief may locally slope uphill, while the
    // river thalweg may not. One upward ULP is retained as the explicit float
    // tolerance used by the downstream validation contract.
    std::vector<float> smoothed(raw.size());
    constexpr float weights[5] = {1.0f, 2.0f, 3.0f, 2.0f, 1.0f};
    for (std::size_t i = 0; i < raw.size(); ++i) {
        float weighted = 0.0f;
        float total = 0.0f;
        for (int offset = -2; offset <= 2; ++offset) {
            const std::size_t index = static_cast<std::size_t>(std::clamp<long long>(
                static_cast<long long>(i) + offset, 0,
                static_cast<long long>(raw.size() - 1u)));
            const float weight = weights[offset + 2];
            weighted += raw[index] * weight;
            total += weight;
        }
        smoothed[i] = weighted / total;
    }
    for (std::size_t i = 1; i < smoothed.size(); ++i) {
        const float maximum = std::nextafter(
            smoothed[i - 1], std::numeric_limits<float>::infinity());
        smoothed[i] = std::min(smoothed[i], maximum);
    }

    for (std::size_t i = 0; i < geometry.centreline.size(); ++i) {
        const auto& source = geometry.centreline[i];
        result->samples_.push_back({source.position_m.x,
                                    source.position_m.z,
                                    source.lateral.x,
                                    source.lateral.z,
                                    source.distance_m,
                                    smoothed[i]});
    }

    for (const auto& source : geometry.boulders) {
        if (!finite(source.center_m.x) || !finite(source.center_m.z) ||
            !finite(source.radius_m) || source.radius_m <= 0.0f)
            return fail(error, "river height overlay boulder is invalid");
        Boulder boulder{};
        boulder.x = source.center_m.x;
        boulder.z = source.center_m.z;
        boulder.radius_m = source.radius_m;
        // Task 2 owns XZ and radius. Only Y is replaced, by the final carved
        // terrain bed at that immutable plan-view selection.
        boulder.bed_y = result->terrain_height(boulder.x, boulder.z, 0.0f);
        result->boulders_.push_back(boulder);
    }

    std::uint64_t hash = kFnvOffset;
    hash_bytes(hash, &geometry.revision, sizeof(geometry.revision));
    hash_bytes(hash, &channel.width_m, sizeof(channel.width_m));
    hash_bytes(hash, &channel.depth_m, sizeof(channel.depth_m));
    hash_bytes(hash, &channel.asymmetry, sizeof(channel.asymmetry));
    for (float value : smoothed) hash_bytes(hash, &value, sizeof(value));
    result->hash_ = hash;
    error.clear();
    out = std::move(result);
    return true;
}

std::size_t RiverHeightOverlay::nearest_sample(float x, float z) const {
    std::size_t nearest = 0u;
    float nearest_squared = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < samples_.size(); ++i) {
        const float dx = x - samples_[i].x;
        const float dz = z - samples_[i].z;
        const float squared = dx * dx + dz * dz;
        if (squared < nearest_squared) {
            nearest_squared = squared;
            nearest = i;
        }
    }
    return nearest;
}

float RiverHeightOverlay::terrain_height(float x, float z,
                                         float base_height) const {
    const Sample& sample = samples_[nearest_sample(x, z)];
    const float dx = x - sample.x;
    const float dz = z - sample.z;
    const float lateral = dx * sample.lateral_x + dz * sample.lateral_z;
    const float absolute_lateral = std::fabs(lateral);
    const float half_width = channel_.width_m * 0.5f;
    const float outer_width = half_width * 3.0f;
    const float asymmetry = std::clamp(channel_.asymmetry, -1.0f, 1.0f);
    const float signed_asymmetry = lateral >= 0.0f ? asymmetry : -asymmetry;
    const float bank_rise = channel_.depth_m *
                            (1.0f + 0.85f * signed_asymmetry);

    if (absolute_lateral <= half_width) {
        const float across = rounded_v(absolute_lateral / half_width);
        return sample.thalweg_y + bank_rise * across;
    }

    const float shoulder = sample.thalweg_y + bank_rise;
    const float broad_relief = channel_.depth_m +
        std::min(channel_.depth_m * 0.18f, 0.45f) *
            broad_noise(sample.distance_m + absolute_lateral * 0.35f,
                        terrain_seed_ ^ UINT64_C(0x726176696e65));
    const float graded_terrain = sample.thalweg_y + broad_relief;
    const float corridor = 1.0f - smoothstep(
        (absolute_lateral - half_width) / (outer_width - half_width));
    const float broad = base_height + (graded_terrain - base_height) * corridor;
    const float shoulder_blend = smoothstep(
        (absolute_lateral - half_width) / (outer_width - half_width));
    return shoulder + (broad - shoulder) * shoulder_blend;
}

float RiverHeightOverlay::height_at(float x, float z, float base_height) const {
    float result = terrain_height(x, z, base_height);
    for (const Boulder& boulder : boulders_) {
        const float dx = x - boulder.x;
        const float dz = z - boulder.z;
        const float squared = dx * dx + dz * dz;
        const float radius_squared = boulder.radius_m * boulder.radius_m;
        if (squared >= radius_squared) continue;
        const float upper_sphere = boulder.bed_y + boulder.radius_m +
                                   std::sqrt(radius_squared - squared);
        result = std::max(result, upper_sphere);
    }
    return result;
}

} // namespace terrain_field
