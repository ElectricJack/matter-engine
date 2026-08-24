#include "hydrology/spillway_handoff.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hydrology {
namespace {

constexpr std::uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);

void hash_bytes(std::uint64_t& hash, const void* data,
                std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0u; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
}

void hash_string(std::uint64_t& hash, const std::string& value) noexcept {
    const std::uint64_t size = value.size();
    hash_bytes(hash, &size, sizeof(size));
    hash_bytes(hash, value.data(), value.size());
}

void hash_float3(std::uint64_t& hash, matter::Float3 value) noexcept {
    hash_bytes(hash, &value.x, sizeof(value.x));
    hash_bytes(hash, &value.y, sizeof(value.y));
    hash_bytes(hash, &value.z, sizeof(value.z));
}

bool finite(float value) { return std::isfinite(value); }

bool finite(matter::Float3 value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

matter::Float3 add(matter::Float3 a, matter::Float3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
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

bool normalize(matter::Float3 value, matter::Float3& result) {
    const float length_squared = dot(value, value);
    if (!finite(length_squared) || length_squared <= 1.0e-12f) return false;
    result = scale(value, 1.0f / std::sqrt(length_squared));
    return finite(result);
}

RiverCentrelineSample sample_at_distance(const RiverGeometry& geometry,
                                         float distance_m) {
    if (distance_m <= geometry.centreline.front().distance_m)
        return geometry.centreline.front();
    for (std::size_t index = 1u; index < geometry.centreline.size(); ++index) {
        const auto& next = geometry.centreline[index];
        if (distance_m > next.distance_m) continue;
        const auto& previous = geometry.centreline[index - 1u];
        const float span = next.distance_m - previous.distance_m;
        const float t = span > 0.0f
            ? std::clamp((distance_m - previous.distance_m) / span,
                         0.0f, 1.0f)
            : 0.0f;
        const auto blend = [t](float a, float b) {
            return a + (b - a) * t;
        };
        RiverCentrelineSample result = previous;
        result.position_m = {
            blend(previous.position_m.x, next.position_m.x),
            blend(previous.position_m.y, next.position_m.y),
            blend(previous.position_m.z, next.position_m.z)};
        result.tangent = {
            blend(previous.tangent.x, next.tangent.x),
            blend(previous.tangent.y, next.tangent.y),
            blend(previous.tangent.z, next.tangent.z)};
        result.lateral = {
            blend(previous.lateral.x, next.lateral.x),
            blend(previous.lateral.y, next.lateral.y),
            blend(previous.lateral.z, next.lateral.z)};
        result.distance_m = distance_m;
        return result;
    }
    return geometry.centreline.back();
}

bool fail(const char* message, SpillwayHandoffRecord& handoff,
          FluidBakeError& error) {
    handoff = {};
    error = {FluidBakeCode::InvalidInput, message};
    return false;
}

bool contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

std::uint64_t spillway_handoff_semantic_key(
    const SpillwayHandoffRecord& handoff) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, handoff.id);
    hash_string(hash, handoff.upstream_section_id);
    hash_string(hash, handoff.downstream_section_id);
    hash_float3(hash, handoff.lip_origin_m);
    hash_float3(hash, handoff.tangent);
    hash_float3(hash, handoff.lateral);
    hash_float3(hash, handoff.up);
    hash_bytes(hash, &handoff.discharge_m3s, sizeof(float));
    hash_bytes(hash, &handoff.width_m, sizeof(float));
    hash_bytes(hash, &handoff.effective_depth_m, sizeof(float));
    hash_bytes(hash, &handoff.channel_depth_m, sizeof(float));
    hash_bytes(hash, &handoff.channel_asymmetry, sizeof(float));
    hash_bytes(hash, &handoff.initial_speed_mps, sizeof(float));
    hash_bytes(hash, &handoff.overlap_m, sizeof(float));
    hash_bytes(hash, &handoff.upstream_visual_cut_m, sizeof(float));
    hash_bytes(hash, &handoff.downstream_visual_cut_m, sizeof(float));
    hash_float3(hash, handoff.temporary_dam_exclusion_bounds_m.minimum);
    hash_float3(hash, handoff.temporary_dam_exclusion_bounds_m.maximum);
    return hash == 0u ? 1u : hash;
}

bool resolve_spillway_handoff(
    const matter::RiverSectionDefinition& upstream,
    const matter::RiverSectionDefinition& downstream,
    const RiverGeometry& geometry,
    float accepted_discharge_m3s,
    SpillwayHandoffRecord& handoff,
    FluidBakeError& error) {
    handoff = {};
    error = {};
    if (!upstream.terminal_spillway || geometry.centreline.size() < 2u ||
        upstream.id.empty() || downstream.id.empty() ||
        upstream.river != downstream.river ||
        !contains(downstream.after_section_ids, upstream.id) ||
        !contains(downstream.upstream_spillway_section_ids, upstream.id) ||
        !finite(accepted_discharge_m3s) || accepted_discharge_m3s <= 0.0f)
        return fail("spillway handoff section relationship is invalid",
                    handoff, error);
    const auto& spillway = *upstream.terminal_spillway;
    if (spillway.id.empty() || !finite(spillway.distance_m) ||
        !finite(spillway.width_m) || spillway.width_m <= 0.0f ||
        !finite(spillway.effective_depth_m) ||
        spillway.effective_depth_m <= 0.0f ||
        !finite(spillway.overlap_m) || spillway.overlap_m <= 0.0f ||
        std::fabs(spillway.distance_m - upstream.to_m) > 1.0e-3f ||
        std::fabs(downstream.from_m - upstream.to_m) > 1.0e-3f)
        return fail("spillway handoff dimensions or section ranges are invalid",
                    handoff, error);
    const auto sample = sample_at_distance(geometry, spillway.distance_m);
    matter::Float3 tangent{};
    if (!normalize(sample.tangent, tangent))
        return fail("spillway tangent is invalid", handoff, error);
    matter::Float3 lateral_candidate = add(
        sample.lateral, scale(tangent, -dot(sample.lateral, tangent)));
    matter::Float3 lateral{};
    matter::Float3 up{};
    if (!normalize(lateral_candidate, lateral) ||
        !normalize(cross(lateral, tangent), up))
        return fail("spillway frame is invalid", handoff, error);

    handoff.id = spillway.id;
    handoff.upstream_section_id = upstream.id;
    handoff.downstream_section_id = downstream.id;
    handoff.lip_origin_m = sample.position_m;
    handoff.tangent = tangent;
    handoff.lateral = lateral;
    handoff.up = up;
    handoff.discharge_m3s = accepted_discharge_m3s;
    handoff.width_m = spillway.width_m;
    handoff.effective_depth_m = spillway.effective_depth_m;
    handoff.channel_depth_m = sample.depth_m;
    handoff.channel_asymmetry = sample.asymmetry;
    handoff.initial_speed_mps = accepted_discharge_m3s /
                                (spillway.width_m * spillway.effective_depth_m);
    handoff.overlap_m = spillway.overlap_m;
    handoff.upstream_visual_cut_m = -spillway.overlap_m * 0.5f;
    handoff.downstream_visual_cut_m = spillway.overlap_m * 0.5f;
    if (!finite(handoff.initial_speed_mps) ||
        handoff.initial_speed_mps <= 0.0f)
        return fail("spillway discharge produces an invalid speed", handoff,
                    error);
    handoff.semantic_key = spillway_handoff_semantic_key(handoff);
    return true;
}

bool make_spillway_emitter(
    const SpillwayHandoffRecord& handoff,
    const FluidPbdSettings& settings,
    FluidEmitter& emitter,
    FluidBakeError& error) {
    emitter = {};
    error = {};
    if (handoff.semantic_key == 0u || handoff.id.empty() ||
        !finite(handoff.discharge_m3s) || handoff.discharge_m3s <= 0.0f ||
        !finite(handoff.width_m) || handoff.width_m <= 0.0f ||
        !finite(handoff.effective_depth_m) ||
        handoff.effective_depth_m <= 0.0f ||
        !finite(handoff.channel_depth_m) || handoff.channel_depth_m < 0.0f ||
        !finite(handoff.channel_asymmetry) ||
        std::fabs(handoff.channel_asymmetry) > 1.0f ||
        !finite(handoff.initial_speed_mps) || handoff.initial_speed_mps <= 0.0f ||
        !finite(settings.particle_spacing_m) ||
        settings.particle_spacing_m <= 0.0f || settings.max_steps == 0u) {
        error = {FluidBakeCode::InvalidInput,
                 "spillway emitter settings are invalid"};
        return false;
    }
    emitter.id = static_cast<std::uint32_t>(handoff.semantic_key);
    if (emitter.id == 0u) emitter.id = 1u;
    emitter.shape = FluidEmitterShape::Ribbon;
    emitter.position_m = add(
        handoff.lip_origin_m,
        scale(handoff.up, handoff.effective_depth_m * 0.5f +
                              settings.particle_spacing_m));
    emitter.direction = handoff.tangent;
    emitter.lateral_axis = handoff.lateral;
    emitter.up_axis = handoff.up;
    emitter.initial_velocity_mps = scale(
        handoff.tangent, handoff.initial_speed_mps);
    emitter.flow_m3s = handoff.discharge_m3s;
    emitter.radius_m = 0.0f;
    emitter.half_extent_m = {
        handoff.width_m * 0.5f, handoff.effective_depth_m * 0.5f};
    emitter.channel_depth_m = handoff.channel_depth_m;
    emitter.channel_asymmetry = handoff.channel_asymmetry;
    emitter.start_step = 0u;
    emitter.stop_step = settings.max_steps;
    if (!valid_fluid_emitter(emitter)) {
        emitter = {};
        error = {FluidBakeCode::InvalidInput,
                 "resolved spillway emitter frame is invalid"};
        return false;
    }
    return true;
}

}  // namespace hydrology
