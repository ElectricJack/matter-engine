#pragma once

#include "hydrology/water_visual_products.h"
#include "matter/gpu_visual_meshing.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace gpu_meshing::fixtures {

// A deterministic final PBF-style snapshot: stable particle id is the vector
// index. The centerline bends twice and loses elevation in +Z, while the four
// particles at each station give the visual field enough volume to form a
// connected water ribbon instead of a string of isolated spheres.
inline std::vector<ParticleSample> synthetic_flowing_water_particles() {
    std::vector<ParticleSample> particles;
    constexpr std::uint32_t kStations = 29u;
    particles.reserve(kStations * 4u);
    for (std::uint32_t station = 0; station != kStations; ++station) {
        const float t = static_cast<float>(station) /
                        static_cast<float>(kStations - 1u);
        const float z = -7.0f + 14.0f * t;
        const float x = 1.15f * std::sin(t * 6.28318530718f) +
                        0.25f * std::sin(t * 12.5663706144f);
        const float y = 1.35f - 2.1f * t;
        for (const matter::Float3 offset : {
                 matter::Float3{-0.27f, 0.00f, 0.0f},
                 matter::Float3{ 0.27f, 0.00f, 0.0f},
                 matter::Float3{-0.18f, 0.24f, 0.0f},
                 matter::Float3{ 0.18f, 0.24f, 0.0f}}) {
            particles.push_back({{x + offset.x, y + offset.y,
                                  z + offset.z},
                                 0.38f});
        }
    }
    return particles;
}

inline ParticleJob synthetic_flowing_water_job(
    const std::vector<ParticleSample>& particles) {
    ParticleJob job{};
    job.particles = particles.data();
    job.particle_count = static_cast<std::uint32_t>(particles.size());
    job.bounds_m = {{-2.2f, -1.35f, -7.7f}, {2.2f, 2.2f, 7.7f}};
    job.voxel_m = 0.16f;
    job.blend_width_m = 0.16f;
    job.iso_value = 0.0f;
    job.material = 4u;
    job.limits = {512u, 1u << 22u, 1u << 22u, 1u << 22u};
    job.generation = 0x50424631u;
    return job;
}

inline std::vector<hydrology::GameplaySample>
synthetic_flowing_water_gameplay() {
    std::vector<hydrology::GameplaySample> result;
    constexpr std::uint32_t kSamples = 15u;
    result.reserve(kSamples);
    for (std::uint32_t sample = 0; sample != kSamples; ++sample) {
        const float t = static_cast<float>(sample) /
                        static_cast<float>(kSamples - 1u);
        const float phase = t * 6.28318530718f;
        const float dx_dt = 1.15f * 6.28318530718f * std::cos(phase) +
                            0.25f * 12.5663706144f * std::cos(phase * 2.0f);
        const float dz_dt = 14.0f;
        const float length = std::sqrt(dx_dt * dx_dt + dz_dt * dz_dt);
        result.push_back({1.59f - 2.1f * t, 0.55f,
                          5.5f * dx_dt / length, -0.825f,
                          5.5f * dz_dt / length, true});
    }
    return result;
}

}  // namespace gpu_meshing::fixtures
