#include "terrain_river_overlay.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace terrain_field {
namespace {

constexpr std::uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);
constexpr std::uint32_t kRiverOverlayRevision = 3u;

void hash_bytes(std::uint64_t& hash, const void* value, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(value);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
}

float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float rounded_v(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    constexpr float roundness = 0.08f;
    const float floor = roundness;
    const float ceiling = std::sqrt(1.0f + roundness * roundness);
    return (std::sqrt(t * t + roundness * roundness) - floor) /
           (ceiling - floor);
}

bool finite(float value) { return std::isfinite(value); }

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

} // namespace

bool RiverHeightOverlay::build(
    const hydrology::RiverGeometry& geometry,
    std::shared_ptr<const RiverHeightOverlay>& out,
    std::string& error) {
    if (geometry.centreline.size() < 2u)
        return fail(error,
                    "river height overlay requires at least two centreline samples");

    auto result = std::shared_ptr<RiverHeightOverlay>(new RiverHeightOverlay());
    result->samples_.reserve(geometry.centreline.size());
    float previous_distance = -1.0f;
    for (const auto& source : geometry.centreline) {
        if (!finite(source.position_m.x) || !finite(source.position_m.y) ||
            !finite(source.position_m.z) || !finite(source.lateral.x) ||
            !finite(source.lateral.z) || !finite(source.distance_m) ||
            !finite(source.width_m) || !finite(source.depth_m) ||
            !finite(source.asymmetry) || source.width_m <= 0.0f ||
            source.depth_m <= 0.0f || std::fabs(source.asymmetry) > 1.0f ||
            source.distance_m <= previous_distance)
            return fail(error, "river height overlay centreline is invalid");
        previous_distance = source.distance_m;
        result->samples_.push_back({source.position_m.x,
                                    source.position_m.z,
                                    source.lateral.x,
                                    source.lateral.z,
                                    source.distance_m,
                                    source.position_m.y,
                                    source.width_m * 0.5f,
                                    source.depth_m,
                                    source.asymmetry});
    }

    std::uint64_t hash = kFnvOffset;
    hash_bytes(hash, &kRiverOverlayRevision, sizeof(kRiverOverlayRevision));
    hash_bytes(hash, &geometry.revision, sizeof(geometry.revision));
    for (const auto& sample : result->samples_)
        hash_bytes(hash, &sample, sizeof(sample));
    result->terrain_seed_ = hash;
    result->hash_ = hash;
    error.clear();
    out = std::move(result);
    return true;
}

std::size_t RiverHeightOverlay::nearest_sample(float x, float z) const {
    std::size_t nearest = 0u;
    float nearest_squared = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0u; i < samples_.size(); ++i) {
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
    const std::size_t sample_index = nearest_sample(x, z);
    const Sample& sample = samples_[sample_index];
    const float dx = x - sample.x;
    const float dz = z - sample.z;
    const float lateral = dx * sample.lateral_x + dz * sample.lateral_z;
    const float absolute_lateral = std::fabs(lateral);
    const float outer_width = sample.half_width_m * 3.0f;
    const float signed_asymmetry = lateral >= 0.0f
        ? sample.asymmetry : -sample.asymmetry;
    const float bank_rise = sample.depth_m *
                            (1.0f + 0.85f * signed_asymmetry);

    float carved_height = sample.thalweg_y;
    if (absolute_lateral <= sample.half_width_m) {
        carved_height += bank_rise *
                         rounded_v(absolute_lateral / sample.half_width_m);
    } else {
        const float shoulder = sample.thalweg_y + bank_rise;
        const float restore = smoothstep(
            (absolute_lateral - sample.half_width_m) /
            (outer_width - sample.half_width_m));
        carved_height = shoulder + (base_height - shoulder) * restore;
    }

    // Endpoint clamping supplies a short terrain-carved inlet apron. Restore
    // the authored terrain smoothly behind it so the dry collar never becomes
    // an implicit containment wall.
    if (sample_index == 0u) {
        const float tangent_x = sample.lateral_z;
        const float tangent_z = -sample.lateral_x;
        const float longitudinal = dx * tangent_x + dz * tangent_z;
        if (longitudinal < 0.0f) {
            const float width_m = sample.half_width_m * 2.0f;
            const float source_apron_m = std::max(2.0f, width_m * 0.15f);
            const float headwall_length_m = std::max(4.0f, width_m * 0.35f);
            const float restore = smoothstep(
                (-longitudinal - source_apron_m) / headwall_length_m);
            carved_height += (base_height - carved_height) * restore;
        }
    }
    return carved_height;
}

float RiverHeightOverlay::height_at(float x, float z,
                                    float base_height) const {
    return terrain_height(x, z, base_height);
}

} // namespace terrain_field
