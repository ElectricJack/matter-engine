#include "hydrology/fluid_emission.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace hydrology {
namespace {

constexpr float kSnippetVolumeScale = 1.333f * 3.14159f;
constexpr double kIntegerSnapTolerance = 1.0e-6;

bool fail(FluidBakeCode code, const char* message,
          std::vector<FluidParticleActivation>& activations,
          FluidBakeError& error) {
    activations.clear();
    error = {code, message};
    return false;
}

}  // namespace

float physx_particle_volume_m3(float particle_spacing_m) noexcept {
    return kSnippetVolumeScale * particle_spacing_m * particle_spacing_m *
           particle_spacing_m;
}

bool schedule_fluid_emission_step(
    const std::vector<FluidEmitter>& emitters,
    const FluidPbdSettings& settings,
    std::uint32_t step,
    std::uint32_t active_particle_count,
    FluidEmissionState& state,
    std::vector<FluidParticleActivation>& activations,
    FluidBakeError& error) {
    activations.clear();
    error = {};

    const float particle_volume =
        physx_particle_volume_m3(settings.particle_spacing_m);
    if (emitters.empty() || !std::isfinite(particle_volume) ||
        !(particle_volume > 0.0f) ||
        !std::isfinite(settings.fixed_step_seconds) ||
        !(settings.fixed_step_seconds > 0.0f) ||
        settings.max_particles == 0u ||
        active_particle_count > settings.max_particles ||
        step == std::numeric_limits<std::uint32_t>::max()) {
        return fail(FluidBakeCode::InvalidInput,
                    "fluid emission settings are invalid", activations,
                    error);
    }
    std::unordered_set<std::uint32_t> emitter_ids;
    for (const FluidEmitter& emitter : emitters) {
        if (!valid_fluid_emitter(emitter) ||
            !emitter_ids.insert(emitter.id).second) {
            return fail(FluidBakeCode::InvalidInput,
                        "fluid emitter schedule input is invalid",
                        activations, error);
        }
    }

    FluidEmissionState candidate = state;
    if (!candidate.initialized) {
        candidate.emitter_ids.reserve(emitters.size());
        for (const FluidEmitter& emitter : emitters) {
            candidate.emitter_ids.push_back(emitter.id);
        }
        candidate.fractional_carry.assign(emitters.size(), 0.0);
        candidate.next_step = step;
        candidate.initialized = true;
    }
    if (candidate.next_step != step ||
        candidate.emitter_ids.size() != emitters.size() ||
        candidate.fractional_carry.size() != emitters.size()) {
        return fail(FluidBakeCode::InvalidInput,
                    "fluid emission state does not match this solver step",
                    activations, error);
    }
    for (std::size_t index = 0; index < emitters.size(); ++index) {
        if (candidate.emitter_ids[index] != emitters[index].id ||
            !std::isfinite(candidate.fractional_carry[index]) ||
            candidate.fractional_carry[index] < 0.0 ||
            candidate.fractional_carry[index] >= 1.0) {
            return fail(FluidBakeCode::InvalidInput,
                        "fluid emission state or emitter ordering changed",
                        activations, error);
        }
    }

    std::vector<std::uint32_t> emitter_counts(emitters.size(), 0u);
    std::uint64_t total_count = 0u;
    for (std::size_t index = 0; index < emitters.size(); ++index) {
        const FluidEmitter& emitter = emitters[index];
        if (step < emitter.start_step || step >= emitter.stop_step) continue;

        const double rate = static_cast<double>(emitter.flow_m3s) *
                            static_cast<double>(settings.fixed_step_seconds) /
                            static_cast<double>(particle_volume);
        double owed = rate + candidate.fractional_carry[index];
        const double nearest_integer = std::round(owed);
        if (std::fabs(owed - nearest_integer) <=
            kIntegerSnapTolerance * std::max(1.0, std::fabs(owed))) {
            owed = nearest_integer;
        }
        if (!std::isfinite(owed) || owed < 0.0 ||
            owed > static_cast<double>(
                       std::numeric_limits<std::uint32_t>::max())) {
            return fail(FluidBakeCode::CapacityExceeded,
                        "fluid emitter activation count exceeds capacity",
                        activations, error);
        }
        const auto count = static_cast<std::uint32_t>(std::floor(owed));
        candidate.fractional_carry[index] = owed - count;
        emitter_counts[index] = count;
        total_count += count;
    }

    if (total_count > settings.max_particles - active_particle_count ||
        total_count > std::numeric_limits<std::uint64_t>::max() -
                          candidate.next_particle_id) {
        activations.clear();
        const std::uint64_t required =
            static_cast<std::uint64_t>(active_particle_count) + total_count;
        error = {
            FluidBakeCode::CapacityExceeded,
            "fluid emission requires " + std::to_string(required) +
                " particles but capacity is " +
                std::to_string(settings.max_particles)};
        return false;
    }

    activations.reserve(static_cast<std::size_t>(total_count));
    for (std::size_t emitter_index = 0;
         emitter_index < emitters.size(); ++emitter_index) {
        const FluidEmitter& emitter = emitters[emitter_index];
        for (std::uint32_t count = 0; count < emitter_counts[emitter_index];
             ++count) {
            activations.push_back({candidate.next_particle_id++, emitter.id,
                                   emitter.position_m,
                                   emitter.initial_velocity_mps});
        }
    }
    candidate.next_step = step + 1u;
    state = std::move(candidate);
    return true;
}

}  // namespace hydrology
