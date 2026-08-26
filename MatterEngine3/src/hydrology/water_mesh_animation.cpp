#include "hydrology/water_mesh_animation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace hydrology {
namespace {

bool fail(gpu_meshing::Error& error,
          gpu_meshing::ErrorCode code,
          const std::string& message) {
    error = {code, message};
    return false;
}

bool finite(matter::Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool valid_mesh(const gpu_meshing::MeshResult& mesh,
                std::uint32_t material) {
    if (mesh.material != material || mesh.positions.empty() ||
        mesh.positions.size() != mesh.normals.size() ||
        mesh.positions.size() % 3u != 0u ||
        mesh.indices.empty() || mesh.indices.size() % 3u != 0u)
        return false;
    for (float value : mesh.positions)
        if (!std::isfinite(value)) return false;
    for (float value : mesh.normals)
        if (!std::isfinite(value)) return false;
    const std::size_t vertex_count = mesh.positions.size() / 3u;
    for (std::uint32_t index : mesh.indices)
        if (index >= vertex_count) return false;
    return mesh.content_digest == gpu_meshing::mesh_content_digest(mesh);
}

}  // namespace

WaterMeshAnimationPhase water_mesh_animation_phase(
    std::uint32_t frame_index,
    std::uint32_t frame_count,
    std::uint32_t phase_offset_frames) noexcept {
    if (frame_count == 0u || frame_index >= frame_count ||
        phase_offset_frames >= frame_count)
        return {};
    constexpr double pi = 3.1415926535897932384626433832795;
    const double t = static_cast<double>(frame_index) /
                     static_cast<double>(frame_count);
    const float primary = static_cast<float>(
        0.5 - 0.5 * std::cos(2.0 * pi * t));
    return {
        frame_index,
        (frame_index + phase_offset_frames) % frame_count,
        primary,
        1.0f - primary,
    };
}

bool build_water_mesh_animation(
    const FluidParticleAnimationCapture& capture,
    float particle_radius_m,
    const gpu_meshing::ParticleJob& template_job,
    const WaterMeshAnimationMesher& mesher,
    WaterMeshAnimation& animation,
    gpu_meshing::Error& error) {
    animation = {};
    error = {};
    constexpr std::uint32_t kFrames = 30u;
    constexpr std::uint32_t kFramesPerSecond = 30u;
    constexpr std::uint32_t kPhaseOffset = 15u;
    if (capture.frames_per_second != kFramesPerSecond ||
        capture.phase_offset_frames != kPhaseOffset ||
        capture.frames.size() != kFrames || !mesher ||
        !std::isfinite(particle_radius_m) || particle_radius_m <= 0.0f) {
        return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                    "water mesh animation requires a complete fixed 30-frame capture");
    }

    std::size_t maximum_combined = 0u;
    for (std::uint32_t frame_index = 0u;
         frame_index != kFrames; ++frame_index) {
        const WaterMeshAnimationPhase phase = water_mesh_animation_phase(
            frame_index, kFrames, kPhaseOffset);
        const std::size_t primary =
            capture.frames[phase.primary_capture].positions_m.size();
        const std::size_t secondary =
            capture.frames[phase.secondary_capture].positions_m.size();
        if (primary > std::numeric_limits<std::size_t>::max() - secondary)
            return fail(error, gpu_meshing::ErrorCode::Overflow,
                        "water mesh animation particle count overflowed");
        maximum_combined = std::max(maximum_combined, primary + secondary);
    }
    if (maximum_combined > template_job.limits.max_particles ||
        maximum_combined > std::numeric_limits<std::uint32_t>::max()) {
        return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                    "water mesh animation exceeds the visual particle limit");
    }

    std::vector<gpu_meshing::ParticleSample> particles;
    particles.reserve(maximum_combined);
    WaterMeshAnimation candidate{};
    candidate.frames_per_second = kFramesPerSecond;
    candidate.phase_offset_frames = kPhaseOffset;
    candidate.duration_seconds = 1.0f;
    candidate.frames.reserve(kFrames);
    for (std::uint32_t frame_index = 0u;
         frame_index != kFrames; ++frame_index) {
        const WaterMeshAnimationPhase phase = water_mesh_animation_phase(
            frame_index, kFrames, kPhaseOffset);
        const auto& primary =
            capture.frames[phase.primary_capture].positions_m;
        const auto& secondary =
            capture.frames[phase.secondary_capture].positions_m;
        particles.clear();
        for (matter::Float3 position : primary) {
            if (!finite(position))
                return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                            "water mesh animation frame " +
                                std::to_string(frame_index) +
                                " has a non-finite primary particle");
            particles.push_back({position, particle_radius_m});
        }
        const std::uint32_t split =
            static_cast<std::uint32_t>(particles.size());
        for (matter::Float3 position : secondary) {
            if (!finite(position))
                return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                            "water mesh animation frame " +
                                std::to_string(frame_index) +
                                " has a non-finite secondary particle");
            particles.push_back({position, particle_radius_m});
        }

        gpu_meshing::ParticleJob job = template_job;
        job.particles = particles.empty() ? nullptr : particles.data();
        job.particle_count = static_cast<std::uint32_t>(particles.size());
        job.phase_blend = {
            split, phase.primary_weight, phase.secondary_weight,
        };
        gpu_meshing::MeshResult mesh{};
        gpu_meshing::Stats stats{};
        gpu_meshing::Error frame_error{};
        if (!mesher(job, mesh, stats, frame_error)) {
            const gpu_meshing::ErrorCode code =
                frame_error.code == gpu_meshing::ErrorCode::None
                    ? gpu_meshing::ErrorCode::ArtifactFailure
                    : frame_error.code;
            return fail(error, code,
                        "water mesh animation frame " +
                            std::to_string(frame_index) + " failed: " +
                            frame_error.message);
        }
        if (!valid_mesh(mesh, template_job.material)) {
            return fail(error, gpu_meshing::ErrorCode::ArtifactFailure,
                        "water mesh animation frame " +
                            std::to_string(frame_index) +
                            " returned invalid geometry");
        }
        candidate.frames.push_back(std::move(mesh));
    }
    animation = std::move(candidate);
    return true;
}

}  // namespace hydrology
