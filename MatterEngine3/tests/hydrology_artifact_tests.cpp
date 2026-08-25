#include "check.h"

#include "hydrology/hydrology_artifact.h"
#include "hydrology/fluid_gameplay_field.h"
#include "hydrology/river_presentation_field.h"
#include "hydrology/water_visual_products.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
    artifact.section = {"upper", "main", 0.0f, 145.0f, -5.0f, 150.0f};
    artifact.product_keys = {101u, 202u, 303u, 606u};
    artifact.semantic_key = 505u;
    artifact.particle_snapshot_digest = 404u;
    artifact.particle_radius_m = 0.65f;
    artifact.accepted = true;
    artifact.stats = {9u, 2u, 3u, 1u, 0u, 0.25};
    artifact.stats.emitted_particles = 3u;
    artifact.stats.retired_particles = 1u;
    artifact.stats.escape_policy = {32u, 0.0001f};
    artifact.stats.escape_budget = hydrology::fluid_escape_budget(
        3u, artifact.stats.escape_policy);
    artifact.sensor = {0.8f, 3u, 9u, true, 0.9f, 0.8f, 0.8f, 7u};
    artifact.particles = {
        {{0.0f, 1.0f, 0.0f}, {2.0f, 0.0f, -0.5f}, 4u},
        {{1.0f, 1.5f, 0.0f}, {1.0f, 0.2f, -0.25f}, 9u},
    };
    artifact.visual_mesh = visual_triangle();
    artifact.coarse_cpu_mesh = visual_triangle();
    artifact.coarse_cpu_mesh.positions[0] = -0.25f;
    artifact.coarse_cpu_mesh.content_digest =
        gpu_meshing::mesh_content_digest(artifact.coarse_cpu_mesh);
    artifact.gameplay_field = {
        {10.0f, 1.5f, 2.0f, 0.0f, -0.5f, true},
        {9.5f, 0.7f, 1.0f, 0.2f, -0.25f, false},
    };
    artifact.presentation_field = {
        {0.25f, -0.5f, 0.75f, 0.625f, 0.875f,
         hydrology::RiverFeature::Rapid, true},
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
         hydrology::RiverFeature::Calm, false},
    };
    artifact.gameplay_layout = {{0.0f, 0.0f, 0.0f}, 1.0f, 2u, 1u};
    artifact.provenance = {0x10deu, 0x2684u, 610074u, 0x050601u, 7u};
    return artifact;
}

void test_gameplay_field_marks_empty_cells_invalid_and_weights_velocity_by_volume() {
    const std::vector<hydrology::FluidParticle> fluid = {
        {{0.25f, 2.0f, 0.25f}, {1.0f, 0.0f, 0.0f}, 3u},
        {{0.75f, 3.0f, 0.25f}, {3.0f, 0.0f, 0.0f}, 4u},
    };
    hydrology::GameplayFieldLayout layout{{0.0f, 0.0f, 0.0f}, 1.0f, 2u, 1u};
    std::vector<hydrology::GameplaySample> field;
    std::string error;
    CHECK(hydrology::build_fluid_gameplay_field(
              fluid, 0.5f, layout,
              [](float, float, float& height) { height = 1.0f; return true; },
              field, error), error.c_str());
    CHECK(field.size() == 2u && field[0].wet_valid && !field[1].wet_valid,
          "an empty gameplay cell remains invalid instead of becoming zero-current water");
    CHECK(std::fabs(field[0].height_m - 3.5f) < 1e-6f &&
              std::fabs(field[0].depth_m - 2.5f) < 1e-6f &&
              std::fabs(field[0].velocity_x_mps - 2.0f) < 1e-6f,
          "equal-volume particles produce the literal average velocity and top surface");
    hydrology::GameplaySample sample{};
    CHECK(!hydrology::sample_fluid_gameplay_field(layout, field, 1.5f, 0.5f, sample),
          "queries fail outside the wet mask");
}

void test_semantic_key_covers_every_simulation_contract_input() {
    hydrology::HydrologySemanticInputs inputs{1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u};
    const uint64_t key = hydrology::derive_hydrology_semantic_key(inputs);
    for (uint64_t* value : {&inputs.physx_sdk_version, &inputs.adapter_version,
                            &inputs.pbd_settings_version, &inputs.collision_revision,
                            &inputs.network_hash, &inputs.terrain_revision,
                            &inputs.virtual_dam_revision, &inputs.sensor_revision,
                            &inputs.mesher_contract_version}) {
        ++*value;
        CHECK(hydrology::derive_hydrology_semantic_key(inputs) != key,
              "each PhysX, collision, terrain, network, dam, sensor, and mesher input invalidates the semantic key");
        --*value;
    }
}

void test_identity_separates_visual_from_authority_products() {
    const auto samples = particles();
    const auto job = particle_job(samples);
    hydrology::ProductIdentitySettings settings{};
    settings.shader_digests = {11u, 22u, 33u};
    const hydrology::GameplayFieldLayout gameplay_layout{
        {-2.0f, 0.0f, -2.0f}, 0.5f, 8u, 8u};
    const uint64_t snapshot = hydrology::particle_snapshot_digest(
        samples.data(), static_cast<uint32_t>(samples.size()));
    const auto first = hydrology::derive_product_keys(
        job, snapshot, settings, 0.35f, gameplay_layout);
    auto visual_change = job;
    visual_change.voxel_m = 0.1f;
    settings.shader_digests[1] = 99u;
    const auto second = hydrology::derive_product_keys(
        visual_change, snapshot, settings, 0.35f, gameplay_layout);
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

void test_cpu_fallback_repairs_only_nonfinite_normals() {
    auto repairable = visual_triangle();
    repairable.normals[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(hydrology::repair_nonfinite_cpu_mesh_normals(repairable),
          "valid CPU positions and topology permit deterministic normal repair");
    CHECK(std::all_of(repairable.normals.begin(), repairable.normals.end(),
                      [](float value) { return std::isfinite(value); }) &&
              repairable.normals[2] > 0.99f &&
              repairable.normals[5] > 0.99f &&
              repairable.normals[8] > 0.99f,
          "triangle-derived replacement normals are finite and consistently oriented");

    auto invalid_position = visual_triangle();
    invalid_position.positions[0] =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(!hydrology::repair_nonfinite_cpu_mesh_normals(invalid_position),
          "normal repair never masks non-finite CPU mesh positions");
    auto invalid_index = visual_triangle();
    invalid_index.indices[2] = 99u;
    CHECK(!hydrology::repair_nonfinite_cpu_mesh_normals(invalid_index),
          "normal repair never masks invalid CPU mesh topology");
}

void test_cpu_fallback_interpolates_finite_to_empty_cells_without_nan() {
    // At this resolution the 0.6 m grid edge is wider than MatterSurface's
    // 0.5 m particle query radius. One endpoint is inside the sphere and the
    // neighboring endpoint is the empty-field +infinity sentinel.
    const std::vector<gpu_meshing::ParticleSample> sparse = {
        {{-0.3f, -0.3f, -0.3f}, 0.2f}};
    gpu_meshing::ParticleJob job{};
    job.particles = sparse.data();
    job.particle_count = static_cast<std::uint32_t>(sparse.size());
    job.bounds_m = {{-2.1f, -2.1f, -2.1f}, {2.1f, 2.1f, 2.1f}};
    job.voxel_m = 0.55f;
    job.blend_width_m = 0.0f;
    job.material = 4u;
    job.limits = {8u, 4096u, 4096u, 4096u};

    gpu_meshing::MeshResult coarse{};
    gpu_meshing::Error error{};
    CHECK(hydrology::build_cpu_particle_visual(job, 0.55f, coarse, error),
          error.message.c_str());
    CHECK(!coarse.positions.empty() &&
              std::all_of(coarse.positions.begin(), coarse.positions.end(),
                          [](const float value) {
                              return std::isfinite(value);
                          }),
          "finite-to-empty coarse grid edges emit finite CPU mesh positions");
}

void test_cpu_fallback_preserves_fine_particle_water_on_a_coarse_query_lattice() {
    // A long, densely packed 0.20 m particle stream reproduces the accepted
    // ravine product coupling without requiring PhysX. MatterSurface's cubic
    // power-of-two lattice quantizes this 0.60 m request to roughly 0.69 m.
    // The CPU query wrapper must not let the library's coarse-LOD shrink erase
    // the physically authored 0.13 m particle field altogether.
    std::vector<gpu_meshing::ParticleSample> fine;
    fine.reserve(101u * 5u * 5u);
    for (int x = 0; x <= 100; ++x) {
        for (int y = -2; y <= 2; ++y) {
            for (int z = -2; z <= 2; ++z) {
                fine.push_back({{static_cast<float>(x) * 0.20f,
                                  static_cast<float>(y) * 0.20f,
                                  static_cast<float>(z) * 0.20f},
                                0.13f});
            }
        }
    }
    gpu_meshing::ParticleJob job{};
    job.particles = fine.data();
    job.particle_count = static_cast<std::uint32_t>(fine.size());
    job.bounds_m = {{-0.7f, -10.7f, -10.7f}, {20.7f, 10.7f, 10.7f}};
    job.voxel_m = 0.60f;
    job.blend_width_m = 0.05f;
    job.material = 4u;
    job.limits = {static_cast<std::uint32_t>(fine.size()), 1u << 20u,
                  1u << 20u, 1u << 20u};

    gpu_meshing::MeshResult coarse{};
    gpu_meshing::Error error{};
    CHECK(hydrology::build_cpu_particle_visual(job, 0.60f, coarse, error),
          error.message.c_str());
    CHECK(!coarse.positions.empty() && !coarse.indices.empty(),
          "coarse CPU query meshing retains a dense 0.20 m particle stream");
}

void test_artifact_round_trip_and_corruption_closure() {
    const matter::HydrologyEscapePolicy policy{32u, 0.0001f};
    CHECK(hydrology::fluid_escape_budget(1u, policy) == 32u,
          "one escaped particle is below the default budget");
    CHECK(hydrology::fluid_escape_budget(4000000u, policy) == 400u,
          "large runs use the proportional escape budget");
    const auto artifact = fixture_artifact();
    std::vector<uint8_t> bytes;
    gpu_meshing::Error error{};
    CHECK(hydrology::serialize_artifact(artifact, bytes, error),
          error.message.c_str());
    CHECK(bytes.size() > 32u &&
              std::equal(bytes.begin(), bytes.begin() + 8, "MHYDMSH3"),
          "accepted artifacts use the current hydrology schema");
    hydrology::HydrologyArtifact loaded{};
    CHECK(hydrology::deserialize_artifact(bytes, loaded, error),
          error.message.c_str());
    std::vector<uint8_t> round_trip;
    CHECK(hydrology::serialize_artifact(loaded, round_trip, error) &&
              round_trip == bytes,
          "artifact bytes round-trip exactly");
    CHECK(loaded.product_keys == artifact.product_keys &&
              loaded.accepted && loaded.semantic_key == artifact.semantic_key &&
              loaded.section.section_id == "upper" &&
              loaded.section.river_id == "main" &&
              loaded.section.visual_from_m == -5.0f &&
              loaded.section.visual_to_m == 150.0f &&
              loaded.stats.escaped_particles == 1u &&
              loaded.stats.retired_particles == 1u &&
              loaded.stats.emitted_particles == 3u &&
              loaded.stats.escape_budget == 32u &&
              loaded.particles.size() == artifact.particles.size() &&
              loaded.particles[0].id == artifact.particles[0].id &&
              loaded.visual_mesh.positions == artifact.visual_mesh.positions &&
              loaded.coarse_cpu_mesh.positions ==
                  artifact.coarse_cpu_mesh.positions &&
              loaded.gameplay_field == artifact.gameplay_field &&
              loaded.presentation_field == artifact.presentation_field,
          "all four independent products round-trip");
    CHECK(loaded.visual_mesh.positions.data() !=
              loaded.coarse_cpu_mesh.positions.data(),
          "visual and CPU mesh products never share vector storage");
    const auto reject_without_mutation =
        [&](const std::vector<std::uint8_t>& rejected,
            const char* rejection_message,
            const char* preservation_message) {
            CHECK(!hydrology::deserialize_artifact(rejected, loaded, error),
                  rejection_message);
            std::vector<std::uint8_t> preserved;
            CHECK(hydrology::serialize_artifact(loaded, preserved, error) &&
                      preserved == round_trip,
                  preservation_message);
        };

    constexpr std::size_t kGameplayRecordBytes = 21u;
    constexpr std::size_t kPresentationRecordBytes = 22u;
    const std::size_t presentation_section_bytes =
        8u + artifact.presentation_field.size() * kPresentationRecordBytes;
    const std::size_t gameplay_payload_offset =
        bytes.size() - presentation_section_bytes -
        artifact.gameplay_field.size() * kGameplayRecordBytes;
    auto corrupt = bytes;
    corrupt[gameplay_payload_offset] ^= 0x80u;
    reject_without_mutation(
        corrupt, "gameplay payload corruption is rejected by the stable digest",
        "gameplay corruption preserves the caller's prior valid artifact");
    corrupt = bytes;
    corrupt[bytes.size() - kPresentationRecordBytes] ^= 0x80u;
    reject_without_mutation(
        corrupt,
        "presentation payload corruption is rejected by the stable digest",
        "presentation corruption preserves the caller's prior valid artifact");
    corrupt = bytes;
    corrupt[0] = 'X';
    reject_without_mutation(
        corrupt, "wrong magic is rejected",
        "invalid headers preserve the caller's prior valid artifact");
    corrupt = bytes;
    corrupt[7] = '1';
    reject_without_mutation(
        corrupt,
        "legacy artifacts are explicitly rejected rather than silently migrated",
        "version rejection preserves the caller's prior valid artifact");
    corrupt = bytes;
    corrupt.resize(corrupt.size() - 1u);
    reject_without_mutation(
        corrupt, "truncated presentation section is rejected",
        "truncation preserves the caller's prior valid artifact");
    corrupt = bytes;
    corrupt.resize(bytes.size() - presentation_section_bytes - 1u);
    reject_without_mutation(
        corrupt, "truncated gameplay section is rejected",
        "deep truncation preserves the caller's prior valid artifact");
    corrupt = bytes;
    for (size_t i = 12u; i != 20u; ++i) corrupt[i] = 0xffu;
    reject_without_mutation(
        corrupt, "oversized declared payload is rejected before allocation",
        "oversized headers preserve the caller's prior valid artifact");

    auto invalid = artifact;
    invalid.visual_mesh.indices[2] = 99u;
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "out-of-range visual indices are rejected");
    invalid = artifact;
    invalid.coarse_cpu_mesh.positions[0] =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "non-finite CPU mesh payload is rejected");
    invalid = artifact;
    invalid.accepted = false;
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "failed or cancelled simulations cannot publish an artifact");
    invalid = artifact;
    invalid.particles[1].id = invalid.particles[0].id;
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "an artifact cannot replace the accepted stable-id snapshot with duplicate ids");
    invalid = artifact;
    invalid.provenance.adapter_version = 0u;
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "zero PhysX or adapter provenance cannot be persisted in an accepted artifact");
    invalid = artifact;
    invalid.section.visual_from_m = invalid.section.visual_to_m;
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "invalid section ownership intervals are rejected");
    invalid = artifact;
    invalid.stats.retired_particles = 0u;
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "emitted particles must equal active plus retired particles");
    invalid = artifact;
    invalid.product_keys.presentation = 0u;
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "an accepted v5 artifact requires presentation product identity");
    invalid = artifact;
    invalid.presentation_field.pop_back();
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "presentation dimensions must exactly match the gameplay layout");
    invalid = artifact;
    invalid.presentation_field[0].feature =
        static_cast<hydrology::RiverFeature>(255u);
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "presentation feature values outside the serialized enum are rejected");
    invalid = artifact;
    invalid.presentation_field[0].normal_x = 1.0f;
    invalid.presentation_field[0].normal_z = 0.0f;
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "presentation normals must reconstruct a positive normal Y");
    invalid = artifact;
    invalid.presentation_field[0].foam_potential = 1.01f;
    CHECK(!hydrology::serialize_artifact(invalid, corrupt, error),
          "normalized presentation channels stay in the closed unit interval");
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
    std::vector<std::uint8_t> loaded_before_stale;
    CHECK(hydrology::serialize_artifact(
              loaded, loaded_before_stale, error), error.message.c_str());
    CHECK(!hydrology::load_artifact_validated(
              path, artifact.product_keys.visual, loaded, error,
              artifact.semantic_key + 1u),
          "a changed hydrology semantic key invalidates a cached product");
    std::vector<std::uint8_t> preserved_after_stale_load;
    CHECK(hydrology::serialize_artifact(
              loaded, preserved_after_stale_load, error) &&
              preserved_after_stale_load == loaded_before_stale,
          "a stale cache load preserves the caller's prior valid artifact");

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

    std::vector<std::uint8_t> replacement_bytes;
    CHECK(hydrology::serialize_artifact(
              loaded, replacement_bytes, error), error.message.c_str());
    const auto write_bytes = [&](const std::vector<std::uint8_t>& value) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(value.data()),
                     static_cast<std::streamsize>(value.size()));
        return static_cast<bool>(stream);
    };
    const auto rejected_load_preserves =
        [&](std::vector<std::uint8_t> rejected, const char* message) {
            CHECK(write_bytes(rejected), "corrupt artifact fixture was written");
            CHECK(!hydrology::load_artifact_validated(
                      path, replacement.product_keys.visual, loaded, error),
                  message);
            std::vector<std::uint8_t> after;
            CHECK(hydrology::serialize_artifact(loaded, after, error) &&
                      after == replacement_bytes,
                  "failed artifact loads transactionally preserve prior output");
        };
    auto rejected = replacement_bytes;
    rejected[0] ^= 0x40u;
    rejected_load_preserves(rejected, "corrupt artifact files are rejected");
    rejected = replacement_bytes;
    rejected.pop_back();
    rejected_load_preserves(rejected, "truncated artifact files are rejected");
    rejected = replacement_bytes;
    rejected.back() ^= 0x80u;
    rejected_load_preserves(rejected,
                            "digest-invalid artifact files are rejected");
    CHECK(!hydrology::load_or_build_artifact(
              path, replacement.product_keys.visual,
              [](hydrology::HydrologyArtifact&, gpu_meshing::Error&) {
                  return false;
              },
              loaded, error),
          "a corrupt cache plus failed rebuild is rejected");
    std::vector<std::uint8_t> after_failed_rebuild;
    CHECK(hydrology::serialize_artifact(
              loaded, after_failed_rebuild, error) &&
              after_failed_rebuild == replacement_bytes,
          "load-or-build preserves prior output when both paths fail");
    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    test_identity_separates_visual_from_authority_products();
    test_gameplay_field_marks_empty_cells_invalid_and_weights_velocity_by_volume();
    test_semantic_key_covers_every_simulation_contract_input();
    test_cpu_fallback_builds_owned_coarse_mesh();
    test_cpu_fallback_repairs_only_nonfinite_normals();
    test_cpu_fallback_interpolates_finite_to_empty_cells_without_nan();
    test_cpu_fallback_preserves_fine_particle_water_on_a_coarse_query_lattice();
    test_artifact_round_trip_and_corruption_closure();
    test_atomic_save_validated_load_and_cache_hit();
    return check_summary();
}
