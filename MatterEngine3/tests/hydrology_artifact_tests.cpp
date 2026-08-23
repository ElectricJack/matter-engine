#include "check.h"

#include "hydrology/hydrology_artifact.h"
#include "hydrology/water_visual_products.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

namespace {

std::vector<gpu_meshing::ParticleSample> particles() {
    return {{{-0.35f, 0.0f, 0.0f}, 0.65f},
            {{0.35f, 0.0f, 0.0f}, 0.65f}};
}

gpu_meshing::ParticleJob particle_job(
    const std::vector<gpu_meshing::ParticleSample>& samples) {
    gpu_meshing::ParticleJob job{};
    job.particles = samples.data();
    job.particle_count = static_cast<uint32_t>(samples.size());
    job.bounds_m = {{-1.5f, -1.5f, -1.5f}, {1.5f, 1.5f, 1.5f}};
    job.voxel_m = 0.15f;
    job.blend_width_m = 0.18f;
    job.iso_value = 0.0f;
    job.material = 4;
    job.limits = {64u, 1u << 20u, 1u << 20u, 1u << 20u};
    return job;
}

gpu_meshing::MeshResult visual_triangle() {
    gpu_meshing::MeshResult mesh{};
    mesh.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                      0.0f, 1.0f, 0.0f};
    mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                    0.0f, 0.0f, 1.0f};
    mesh.indices = {0u, 1u, 2u};
    mesh.material = 4u;
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
    return mesh;
}

hydrology::HydrologyArtifact fixture_artifact() {
    hydrology::HydrologyArtifact artifact{};
    artifact.product_keys = {101u, 202u, 303u};
    artifact.particle_snapshot_digest = 404u;
    artifact.visual_mesh = visual_triangle();
    artifact.coarse_cpu_mesh = visual_triangle();
    artifact.coarse_cpu_mesh.positions[0] = -0.25f;
    artifact.coarse_cpu_mesh.content_digest =
        gpu_meshing::mesh_content_digest(artifact.coarse_cpu_mesh);
    artifact.gameplay_field = {
        {10.0f, 1.5f, 2.0f, 0.0f, -0.5f, true},
        {9.5f, 0.7f, 1.0f, 0.2f, -0.25f, false},
    };
    artifact.provenance = {0x10deu, 0x2684u, 610074u};
    return artifact;
}

void test_identity_separates_visual_from_authority_products() {
    const auto samples = particles();
    const auto job = particle_job(samples);
    hydrology::ProductIdentitySettings settings{};
    settings.coarse_voxel_m = 0.35f;
    settings.shader_digests = {11u, 22u, 33u};
    const uint64_t snapshot = hydrology::particle_snapshot_digest(
        samples.data(), static_cast<uint32_t>(samples.size()));
    const auto first = hydrology::derive_product_keys(job, snapshot, settings);
    auto visual_change = job;
    visual_change.voxel_m = 0.1f;
    settings.shader_digests[1] = 99u;
    const auto second = hydrology::derive_product_keys(
        visual_change, snapshot, settings);
    CHECK(first.visual != second.visual,
          "visual resolution or shader changes invalidate the visual key");
    CHECK(first.coarse_cpu == second.coarse_cpu &&
              first.gameplay == second.gameplay,
          "visual-only changes retain CPU and gameplay product keys");
}

void test_cpu_fallback_builds_owned_coarse_mesh() {
    const auto samples = particles();
    const auto job = particle_job(samples);
    gpu_meshing::MeshResult coarse{};
    gpu_meshing::Error error{};
    CHECK(hydrology::build_cpu_particle_visual(job, 0.35f, coarse, error),
          error.message.c_str());
    CHECK(!coarse.positions.empty() && !coarse.indices.empty(),
          "coarse CPU fallback emits geometry");
    CHECK(coarse.positions.size() == coarse.normals.size() &&
              coarse.positions.size() % 3u == 0u &&
              coarse.indices.size() % 3u == 0u,
          "coarse CPU fallback owns valid mesh streams");
    CHECK(coarse.material == job.material && coarse.content_digest != 0u,
          "coarse CPU fallback retains material and digest");
}

void test_artifact_round_trip_and_corruption_closure() {
    const auto artifact = fixture_artifact();
    std::vector<uint8_t> bytes;
    gpu_meshing::Error error{};
    CHECK(hydrology::serialize_artifact(artifact, bytes, error),
          error.message.c_str());
    CHECK(bytes.size() > 32u &&
              std::equal(bytes.begin(), bytes.begin() + 8, "MHYDMSH1"),
          "artifact starts with fixed hydrology magic");
    hydrology::HydrologyArtifact loaded{};
    CHECK(hydrology::deserialize_artifact(bytes, loaded, error),
          error.message.c_str());
    std::vector<uint8_t> round_trip;
    CHECK(hydrology::serialize_artifact(loaded, round_trip, error) &&
              round_trip == bytes,
          "artifact bytes round-trip exactly");
    CHECK(loaded.product_keys == artifact.product_keys &&
              loaded.visual_mesh.positions == artifact.visual_mesh.positions &&
              loaded.coarse_cpu_mesh.positions ==
                  artifact.coarse_cpu_mesh.positions &&
              loaded.gameplay_field == artifact.gameplay_field,
          "all three independent products round-trip");
    CHECK(loaded.visual_mesh.positions.data() !=
              loaded.coarse_cpu_mesh.positions.data(),
          "visual and CPU mesh products never share vector storage");

    auto corrupt = bytes;
    corrupt.back() ^= 0x80u;
    CHECK(!hydrology::deserialize_artifact(corrupt, loaded, error),
          "payload corruption is rejected");
    corrupt = bytes;
    corrupt[0] = 'X';
    CHECK(!hydrology::deserialize_artifact(corrupt, loaded, error),
          "wrong magic is rejected");
    corrupt = bytes;
    corrupt.resize(corrupt.size() - 1u);
    CHECK(!hydrology::deserialize_artifact(corrupt, loaded, error),
          "truncated payload is rejected");
    corrupt = bytes;
    for (size_t i = 12u; i != 20u; ++i) corrupt[i] = 0xffu;
    CHECK(!hydrology::deserialize_artifact(corrupt, loaded, error),
          "oversized declared payload is rejected before allocation");

    auto invalid = artifact;
    invalid.visual_mesh.indices[2] = 99u;
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "out-of-range visual indices are rejected");
    invalid = artifact;
    invalid.coarse_cpu_mesh.positions[0] =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "non-finite CPU mesh payload is rejected");
}

void test_atomic_save_validated_load_and_cache_hit() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("matter-hydrology-artifact-" + std::to_string(stamp));
    const std::filesystem::path path = root / "fixture.mhyd";
    const auto artifact = fixture_artifact();
    gpu_meshing::Error error{};
    CHECK(hydrology::save_artifact_atomic(path, artifact, error),
          error.message.c_str());
    hydrology::HydrologyArtifact loaded{};
    CHECK(hydrology::load_artifact_validated(
              path, artifact.product_keys.visual, loaded, error),
          error.message.c_str());
    CHECK(loaded.visual_mesh.content_digest ==
              artifact.visual_mesh.content_digest,
          "validated file load retains visual mesh bytes");

    int builder_calls = 0;
    hydrology::HydrologyArtifact cached{};
    CHECK(hydrology::load_or_build_artifact(
              path, artifact.product_keys.visual,
              [&](hydrology::HydrologyArtifact&, gpu_meshing::Error&) {
                  ++builder_calls;
                  return false;
              },
              cached, error),
          error.message.c_str());
    CHECK(builder_calls == 0,
          "cache hit reload never invokes the GPU mesher callback");

    auto replacement = artifact;
    replacement.visual_mesh.positions[0] = 0.125f;
    replacement.visual_mesh.content_digest =
        gpu_meshing::mesh_content_digest(replacement.visual_mesh);
    CHECK(hydrology::save_artifact_atomic(path, replacement, error),
          error.message.c_str());
    CHECK(hydrology::load_artifact_validated(
              path, replacement.product_keys.visual, loaded, error) &&
              loaded.visual_mesh.positions[0] == 0.125f,
          "atomic replacement publishes the complete new artifact");
    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    test_identity_separates_visual_from_authority_products();
    test_cpu_fallback_builds_owned_coarse_mesh();
    test_artifact_round_trip_and_corruption_closure();
    test_atomic_save_validated_load_and_cache_hit();
    return check_summary();
}
