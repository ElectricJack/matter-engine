#include "check.h"

#include "hydrology/water_mesh_animation.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

hydrology::FluidParticleAnimationCapture capture_fixture() {
    hydrology::FluidParticleAnimationCapture capture{};
    capture.frames_per_second = 30u;
    capture.phase_offset_frames = 15u;
    capture.frames.resize(30u);
    for (std::uint32_t frame = 0u; frame != 30u; ++frame) {
        capture.frames[frame].simulation_step = (frame + 1u) * 4u;
        capture.frames[frame].positions_m = {
            {static_cast<float>(frame), 1.0f, 0.0f},
        };
    }
    return capture;
}

gpu_meshing::ParticleJob job_fixture() {
    gpu_meshing::ParticleJob job{};
    job.bounds_m = {{-2.0f, -2.0f, -2.0f}, {32.0f, 3.0f, 2.0f}};
    job.voxel_m = 0.2f;
    job.blend_width_m = 0.15f;
    job.iso_value = 0.0f;
    job.material = 4u;
    job.limits = {128u, 1u << 20u, 1u << 20u, 1u << 20u};
    job.generation = 71u;
    return job;
}

gpu_meshing::MeshResult triangle_for_call(std::uint32_t call) {
    gpu_meshing::MeshResult mesh{};
    const float x = static_cast<float>(call);
    mesh.positions = {x, 0.0f, 0.0f, x + 0.5f, 0.0f, 0.0f,
                      x, 0.5f, 0.0f};
    mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                    0.0f, 0.0f, 1.0f};
    mesh.indices = {0u, 1u, 2u};
    mesh.material = 4u;
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
    return mesh;
}

void test_build_pairs_half_cycle_and_requests_thirty_independent_meshes() {
    const auto capture = capture_fixture();
    const auto template_job = job_fixture();
    struct Observed {
        std::vector<gpu_meshing::ParticleSample> particles;
        gpu_meshing::ParticlePhaseBlend blend{};
    };
    std::vector<Observed> observed;
    hydrology::WaterMeshAnimationMesher mesher =
        [&](const gpu_meshing::ParticleJob& job,
            gpu_meshing::MeshResult& mesh,
            gpu_meshing::Stats& stats,
            gpu_meshing::Error& error) {
            Observed call{};
            call.particles.assign(job.particles,
                                  job.particles + job.particle_count);
            call.blend = job.phase_blend;
            observed.push_back(std::move(call));
            mesh = triangle_for_call(
                static_cast<std::uint32_t>(observed.size() - 1u));
            stats.triangles = 1u;
            error = {};
            return true;
        };

    hydrology::WaterMeshAnimation animation{};
    gpu_meshing::Error error{};
    CHECK(hydrology::build_water_mesh_animation(
              capture, 0.2f, template_job, mesher, animation, error),
          error.message.c_str());
    CHECK(observed.size() == 30u && animation.frames.size() == 30u,
          "one independent visual mesh is requested for every output frame");
    CHECK(observed[0].particles[0].position_m.x == 0.0f &&
              observed[0].particles[1].position_m.x == 15.0f &&
              observed[0].blend.split_index == 1u &&
              observed[0].blend.primary_weight == 0.0f &&
              observed[0].blend.secondary_weight == 1.0f,
          "frame zero pairs captures zero and fifteen at the wrap endpoint");
    CHECK(observed[15].particles[0].position_m.x == 15.0f &&
              observed[15].particles[1].position_m.x == 0.0f &&
              std::fabs(observed[15].blend.primary_weight - 1.0f) <= 1e-6f &&
              std::fabs(observed[15].blend.secondary_weight) <= 1e-6f,
          "frame fifteen reverses the capture pair at the opposite endpoint");
    constexpr float pi = 3.14159265358979323846f;
    const float expected = 0.5f - 0.5f * std::cos(2.0f * pi / 30.0f);
    CHECK(std::fabs(observed[1].blend.primary_weight - expected) <= 1e-6f &&
              std::fabs(observed[1].blend.primary_weight +
                            observed[1].blend.secondary_weight - 1.0f) <=
                  1e-6f,
          "analytic cosine weights are complementary on every frame");
    CHECK(animation.frames.front().content_digest !=
              animation.frames.back().content_digest,
          "mesh results are stored independently rather than aliased");
}

void test_frame_failure_publishes_nothing_and_names_the_frame() {
    const auto capture = capture_fixture();
    const auto template_job = job_fixture();
    std::uint32_t calls = 0u;
    hydrology::WaterMeshAnimationMesher mesher =
        [&](const gpu_meshing::ParticleJob&,
            gpu_meshing::MeshResult& mesh,
            gpu_meshing::Stats&,
            gpu_meshing::Error& error) {
            if (calls++ == 7u) {
                error = {gpu_meshing::ErrorCode::VulkanFailure,
                         "forced mesher failure"};
                return false;
            }
            mesh = triangle_for_call(calls);
            return true;
        };
    hydrology::WaterMeshAnimation animation{};
    animation.frames.push_back(triangle_for_call(99u));
    gpu_meshing::Error error{};
    CHECK(!hydrology::build_water_mesh_animation(
              capture, 0.2f, template_job, mesher, animation, error) &&
              animation.frames.empty() &&
              error.message.find("frame 7") != std::string::npos,
          "a failed frame cancels the whole animation and reports its index");

    auto short_capture = capture;
    short_capture.frames.pop_back();
    CHECK(!hydrology::build_water_mesh_animation(
              short_capture, 0.2f, template_job, mesher, animation, error) &&
              error.code == gpu_meshing::ErrorCode::InvalidInput,
          "partial capture histories cannot publish partial animations");
}

}  // namespace

int main() {
    test_build_pairs_half_cycle_and_requests_thirty_independent_meshes();
    test_frame_failure_publishes_nothing_and_names_the_frame();
    return check_summary();
}
