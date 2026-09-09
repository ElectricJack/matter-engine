#include "hydrology/water_mesh_animation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

bool same_lattice(const gpu_meshing::ParticleSamplingLattice& first,
                  const gpu_meshing::ParticleSamplingLattice& second) {
    return std::memcmp(&first, &second, sizeof(first)) == 0;
}

bool crop_interiors_overlap(const gpu_meshing::Aabb& first,
                            const gpu_meshing::Aabb& second) noexcept {
    return std::max(first.min_m.x, second.min_m.x) <
               std::min(first.max_m.x, second.max_m.x) &&
           std::max(first.min_m.y, second.min_m.y) <
               std::min(first.max_m.y, second.max_m.y) &&
           std::max(first.min_m.z, second.min_m.z) <
               std::min(first.max_m.z, second.max_m.z);
}

bool validate_endpoint_sources(
    WaterBoundaryAnimationSourceSpan sources,
    float particle_radius_m,
    const gpu_meshing::ParticleJob& template_job,
    gpu_meshing::Error& error) {
    if (sources.size != 0u && sources.data == nullptr)
        return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                    "water mesh animation endpoint source span is invalid");
    for (std::size_t index = 0u; index != sources.size; ++index) {
        const WaterBoundaryAnimationSource& source = sources.data[index];
        if (source.frames.size() != 30u ||
            source.frames_per_second != 30u ||
            source.phase_offset_frames != 15u ||
            source.particle_radius_m != particle_radius_m ||
            source.blend_width_m != template_job.blend_width_m ||
            !same_lattice(source.lattice, template_job.sampling_lattice)) {
            return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                        "water mesh animation endpoint source metadata does not match the section job");
        }
        for (std::size_t other = 0u; other != index; ++other) {
            if (crop_interiors_overlap(
                    source.crop_bounds_m,
                    sources.data[other].crop_bounds_m)) {
                return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                            "water mesh animation endpoint source crops overlap");
            }
        }
    }
    return true;
}

bool append_normalized_capture_frame(
    const FluidParticleAnimationCapture& capture,
    std::uint32_t capture_index,
    float particle_radius_m,
    WaterBoundaryAnimationSourceSpan sources,
    std::vector<gpu_meshing::ParticleSample>& particles,
    gpu_meshing::Error& error) {
    const auto& raw = capture.frames[capture_index].positions_m;
    for (matter::Float3 position : raw) {
        if (!finite(position))
            return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                        "water mesh animation capture has a non-finite particle");
        bool substituted = false;
        for (std::size_t source_index = 0u;
             source_index != sources.size; ++source_index) {
            if (water_boundary_source_contains(
                    sources.data[source_index], position)) {
                substituted = true;
                break;
            }
        }
        if (!substituted)
            particles.push_back({position, particle_radius_m});
    }
    std::vector<matter::Float3> decoded;
    for (std::size_t source_index = 0u;
         source_index != sources.size; ++source_index) {
        gpu_meshing::Error decode_error{};
        if (!decode_water_boundary_frame(
                sources.data[source_index], capture_index, decoded,
                decode_error)) {
            return fail(
                error,
                decode_error.code == gpu_meshing::ErrorCode::None
                    ? gpu_meshing::ErrorCode::ArtifactFailure
                    : decode_error.code,
                "water mesh animation endpoint frame " +
                    std::to_string(capture_index) + " failed: " +
                    decode_error.message);
        }
        for (matter::Float3 position : decoded) {
            if (!water_boundary_source_contains(
                    sources.data[source_index], position)) {
                return fail(
                    error, gpu_meshing::ErrorCode::ArtifactFailure,
                    "water mesh animation endpoint frame escaped its half-open crop");
            }
            particles.push_back({position, particle_radius_m});
        }
    }
    return true;
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
    gpu_meshing::Error& error,
    WaterBoundaryAnimationSourceSpan endpoint_sources) {
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
    if (!validate_endpoint_sources(endpoint_sources, particle_radius_m,
                                   template_job, error))
        return false;

    for (std::uint32_t capture_index = 0u;
         capture_index != kFrames; ++capture_index) {
        if (capture.frames[capture_index].positions_m.size() >
            template_job.limits.max_particles) {
            return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                        "water mesh animation capture frame " +
                            std::to_string(capture_index) +
                            " exceeds the visual particle limit");
        }
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
    if (maximum_combined > std::numeric_limits<std::uint32_t>::max()) {
        return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                    "water mesh animation overlap workset exceeds the supported particle count");
    }
    const std::uint32_t overlap_workset_limit = std::max(
        template_job.limits.max_particles,
        static_cast<std::uint32_t>(maximum_combined));

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
        particles.clear();
        if (!append_normalized_capture_frame(
                capture, phase.primary_capture, particle_radius_m,
                endpoint_sources, particles, error))
            return false;
        const std::uint32_t split =
            static_cast<std::uint32_t>(particles.size());
        if (!append_normalized_capture_frame(
                capture, phase.secondary_capture, particle_radius_m,
                endpoint_sources, particles, error))
            return false;

        gpu_meshing::ParticleJob job = template_job;
        job.limits.max_particles = overlap_workset_limit;
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
