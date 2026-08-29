#include "check.h"

#include "matter/gpu_visual_meshing.h"
#include "hydrology/fluid_gameplay_field.h"
#include "hydrology/water_visual_products.h"
#include "surface.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

gpu_meshing::ParticleJob one_sphere_job(
    gpu_meshing::ParticleSample (&particles)[1]) {
    particles[0] = {{0.0f, 0.0f, 0.0f}, 0.5f};
    gpu_meshing::ParticleJob job{};
    job.particles = particles;
    job.particle_count = 1;
    job.bounds_m = {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
    job.voxel_m = 0.25f;
    job.blend_width_m = 0.0f;
    job.iso_value = 0.0f;
    job.material = 4;
    job.limits = {16, 4096, 65536, 65536};
    job.generation = 7;
    return job;
}

bool rejects(gpu_meshing::ParticleJob job,
             gpu_meshing::ErrorCode expected_code) {
    gpu_meshing::GridLayout layout{};
    gpu_meshing::Error error{};
    return !gpu_meshing::validate_particle_job(job, layout, error) &&
           error.code == expected_code && !error.message.empty();
}

bool same_float_bits(float left, float right) {
    return std::memcmp(&left, &right, sizeof(float)) == 0;
}

void test_world_anchored_lattice_expands_outward_without_drifting() {
    constexpr matter::Float3 anchor{-31.2f, 7.4f, 11.8f};
    constexpr float voxel = 0.15f;
    gpu_meshing::ParticleSamplingLattice lattice{};
    gpu_meshing::Error error{};
    CHECK(gpu_meshing::make_particle_sampling_lattice(
              anchor, voxel, lattice, error),
          error.message.c_str());
    CHECK(lattice.origin_m.x == anchor.x &&
              lattice.origin_m.y == anchor.y &&
              lattice.origin_m.z == anchor.z &&
              same_float_bits(lattice.voxel_m, voxel) &&
              lattice.version == 1u,
          "a canonical lattice retains the authored world anchor and exact voxel bits");

    gpu_meshing::ParticleSample particles[1];
    auto job = one_sphere_job(particles);
    job.bounds_m = {{-31.03f, 7.46f, 11.67f},
                    {-29.71f, 8.11f, 12.46f}};
    job.voxel_m = voxel;
    job.sampling_lattice = lattice;
    gpu_meshing::GridLayout layout{};
    CHECK(gpu_meshing::validate_particle_job(job, layout, error),
          error.message.c_str());
    const std::array<std::int64_t, 3> expected_min{1, 0, -1};
    const std::array<std::uint32_t, 3> expected_dims{9u, 5u, 6u};
    CHECK(layout.cell_min == expected_min &&
              layout.cell_dims == expected_dims,
          "non-aligned bounds expand outward to hand-derived canonical integer cells");
    CHECK(std::fabs(layout.origin_m.x - -31.05f) <= 2.0e-6f &&
              std::fabs(layout.origin_m.y - 7.4f) <= 2.0e-6f &&
              std::fabs(layout.origin_m.z - 11.65f) <= 2.0e-6f &&
              same_float_bits(layout.spacing_m.x, voxel) &&
              same_float_bits(layout.spacing_m.y, voxel) &&
              same_float_bits(layout.spacing_m.z, voxel),
          "canonical layout origin is a lattice face and all axes keep exact requested spacing");

    auto shifted_tight_bounds = job;
    shifted_tight_bounds.bounds_m = {{-31.04f, 7.41f, 11.66f},
                                     {-29.72f, 8.14f, 12.49f}};
    gpu_meshing::GridLayout shifted_layout{};
    CHECK(gpu_meshing::validate_particle_job(
              shifted_tight_bounds, shifted_layout, error),
          error.message.c_str());
    CHECK(shifted_layout.cell_min == layout.cell_min &&
              shifted_layout.cell_dims == layout.cell_dims &&
              shifted_layout.origin_m.x == layout.origin_m.x &&
              shifted_layout.origin_m.y == layout.origin_m.y &&
              shifted_layout.origin_m.z == layout.origin_m.z,
          "small tight-bound changes inside the same faces retain stable canonical cell indices");

    auto conflict = job;
    conflict.voxel_m = 0.15000002f;
    CHECK(rejects(conflict, gpu_meshing::ErrorCode::InvalidInput),
          "a canonical job rejects a voxel that is not bit-equal to its lattice voxel");
}

void test_canonical_lattice_faces_round_trip_to_the_same_integer_cell() {
    constexpr float voxel = 0.15f;
    const float cell_five_face = voxel * 5.0f;
    gpu_meshing::ParticleSample particles[1];
    auto job = one_sphere_job(particles);
    job.bounds_m = {
        {cell_five_face, 0.01f, 0.01f},
        {cell_five_face + 0.01f, 0.10f, 0.10f}};
    job.voxel_m = voxel;
    job.sampling_lattice = {{0.0f, 0.0f, 0.0f}, voxel, 1u};

    gpu_meshing::GridLayout layout{};
    gpu_meshing::Error error{};
    CHECK(gpu_meshing::validate_particle_job(job, layout, error),
          error.message.c_str());
    CHECK(cell_five_face == 0.75f && layout.cell_min[0] == 5 &&
              layout.cell_dims[0] == 1u &&
              layout.origin_m.x == cell_five_face,
          "a representable canonical face at 0.15 * cell 5 round-trips to cell 5 without acquiring cell 4");

    auto below_face = job;
    below_face.bounds_m.min_m.x = std::nextafter(
        cell_five_face, -std::numeric_limits<float>::infinity());
    gpu_meshing::GridLayout below_layout{};
    CHECK(gpu_meshing::validate_particle_job(
              below_face, below_layout, error) &&
              below_layout.cell_min[0] == 4,
          "the representable float immediately below a canonical face still expands outward into the preceding cell");

    auto above_face = job;
    above_face.bounds_m.max_m.x = std::nextafter(
        cell_five_face, std::numeric_limits<float>::infinity());
    gpu_meshing::GridLayout above_layout{};
    CHECK(gpu_meshing::validate_particle_job(
              above_face, above_layout, error) &&
              above_layout.cell_min[0] == 5 &&
              above_layout.cell_dims[0] == 1u,
          "the representable float immediately above a canonical face still expands outward into the following cell");
}

void test_canonical_lattice_rejects_support_and_grid_overflow_before_allocation() {
    float support = 0.0f;
    gpu_meshing::Error error{};
    CHECK(gpu_meshing::particle_field_support_radius_m(
              0.2f, 0.05f, support, error) &&
              std::fabs(support - 0.7f) <= 1.0e-6f,
          "the public support query returns the mesher's complete influence radius");
    CHECK(!gpu_meshing::particle_field_support_radius_m(
              0.0f, 0.05f, support, error) &&
              error.code == gpu_meshing::ErrorCode::InvalidInput,
          "the support query rejects a non-positive particle radius");

    gpu_meshing::ParticleSample particles[1];
    auto job = one_sphere_job(particles);
    job.bounds_m = {{0.0f, 0.0f, 0.0f},
                    {4294967296.0f, 1.0f, 1.0f}};
    job.voxel_m = 1.0f;
    job.sampling_lattice = {{0.0f, 0.0f, 0.0f}, 1.0f, 1u};
    CHECK(rejects(job, gpu_meshing::ErrorCode::Overflow),
          "canonical dimensions reject uint32 sample-count overflow before allocation");

    job = one_sphere_job(particles);
    job.bounds_m = {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
    job.voxel_m = std::numeric_limits<float>::denorm_min();
    job.sampling_lattice = {
        {0.0f, 0.0f, 0.0f}, job.voxel_m, 1u};
    CHECK(rejects(job, gpu_meshing::ErrorCode::Overflow),
          "canonical cell-index overflow is rejected before any grid allocation");
}

void test_validates_and_derives_particle_grid() {
    gpu_meshing::ParticleSample particles[1];
    const auto job = one_sphere_job(particles);
    gpu_meshing::GridLayout layout{};
    gpu_meshing::Error error{};
    CHECK(gpu_meshing::validate_particle_job(job, layout, error),
          error.message.c_str());
    const std::array<std::uint32_t, 3> expected_samples{9, 9, 9};
    const std::array<std::uint32_t, 3> expected_cells{8, 8, 8};
    CHECK(layout.sample_dims == expected_samples,
          "2 m bounds at 0.25 m create 9^3 samples");
    CHECK(layout.cell_dims == expected_cells,
          "sample lattice creates 8^3 cells");
    CHECK(layout.grid_vertices == 9u * 9u * 9u,
          "grid vertex total is exact");
    CHECK(layout.grid_cells == 8u * 8u * 8u,
          "grid cell total is exact");
    CHECK(layout.spacing_m.x == 0.25f &&
              layout.spacing_m.y == 0.25f &&
              layout.spacing_m.z == 0.25f,
          "divisible bounds retain the requested voxel spacing");
    CHECK(layout.query_radius_m == 1.25f &&
              layout.bin_size_m == layout.query_radius_m,
          "bin scale matches MatterSurfaceLib's field query radius");
    CHECK(layout.bin_origin_m.x == -2.25f &&
              layout.bin_origin_m.y == -2.25f &&
              layout.bin_origin_m.z == -2.25f,
          "bin domain includes one query-radius collar");
}

void test_validation_fails_closed_without_rejecting_supported_edges() {
    gpu_meshing::ParticleSample particles[1];
    auto job = one_sphere_job(particles);

    job.voxel_m = 0.0f;
    CHECK(rejects(job, gpu_meshing::ErrorCode::InvalidInput),
          "zero voxel size is rejected");

    job = one_sphere_job(particles);
    job.blend_width_m = -0.01f;
    CHECK(rejects(job, gpu_meshing::ErrorCode::InvalidInput),
          "negative blend is rejected");

    job = one_sphere_job(particles);
    particles[0].radius_m = 0.0f;
    CHECK(rejects(job, gpu_meshing::ErrorCode::InvalidInput),
          "zero particle radius is rejected");

    job = one_sphere_job(particles);
    particles[0].position_m.x = std::numeric_limits<float>::infinity();
    CHECK(rejects(job, gpu_meshing::ErrorCode::InvalidInput),
          "non-finite particle position is rejected");

    job = one_sphere_job(particles);
    job.bounds_m.max_m.x = job.bounds_m.min_m.x;
    CHECK(rejects(job, gpu_meshing::ErrorCode::InvalidInput),
          "inverted or empty bounds are rejected");

    job = one_sphere_job(particles);
    job.limits.max_particles = 0;
    CHECK(rejects(job, gpu_meshing::ErrorCode::LimitExceeded),
          "particle limit is enforced before dispatch");

    job = one_sphere_job(particles);
    job.limits.max_grid_vertices = 728;
    CHECK(rejects(job, gpu_meshing::ErrorCode::LimitExceeded),
          "grid vertex limit is enforced before dispatch");

    job = one_sphere_job(particles);
    job.voxel_m = std::numeric_limits<float>::denorm_min();
    CHECK(rejects(job, gpu_meshing::ErrorCode::Overflow),
          "grid dimension overflow is rejected");

    job = one_sphere_job(particles);
    particles[0].position_m = {3.0f, 0.0f, 0.0f};
    gpu_meshing::GridLayout layout{};
    gpu_meshing::Error error{};
    CHECK(gpu_meshing::validate_particle_job(job, layout, error),
          "a particle outside scalar bounds remains a valid snapshot");

    gpu_meshing::ParticleJob empty = job;
    empty.particles = nullptr;
    empty.particle_count = 0;
    CHECK(gpu_meshing::validate_particle_job(empty, layout, error),
          "an empty particle snapshot is a valid dry result");
}

void test_reference_field_matches_matter_surface_oracle() {
    gpu_meshing::ParticleSample particles[2] = {
        {{-0.25f, 0.0f, 0.0f}, 0.5f},
        {{0.25f, 0.0f, 0.0f}, 0.5f},
    };
    const matter::Float3 probes[] = {
        {0.0f, 0.0f, 0.0f},
        {-0.8f, 0.1f, 0.0f},
        {0.3f, -0.2f, 0.4f},
    };
    for (const float blend : {0.0f, 0.12f}) {
        Particle cpu_particles[2]{};
        for (int i = 0; i < 2; ++i) {
            cpu_particles[i].position = {particles[i].position_m.x,
                                         particles[i].position_m.y,
                                         particles[i].position_m.z};
            cpu_particles[i].radius = particles[i].radius_m;
            cpu_particles[i].materialId = 4;
        }
        SurfaceScratch* scratch = CreateSurfaceScratch();
        CHECK(scratch != nullptr, "create MatterSurfaceLib oracle scratch");
        if (!scratch) return;
        for (const auto probe : probes) {
            const float reference =
                gpu_meshing::evaluate_particle_field_reference(
                    particles, 2, blend, probe);
            const float oracle = ProbeFieldScalar(
                scratch, cpu_particles, 0.5f, 2, blend, nullptr, nullptr, 0,
                nullptr, 0, 0.0f, {probe.x, probe.y, probe.z});
            CHECK(std::fabs(reference - oracle) <= 1e-6f,
                  "reference field matches ProbeFieldScalar");
        }
        DestroySurfaceScratch(scratch);
    }

    const gpu_meshing::ParticleSample centered[] = {
        {{0.0f, 0.0f, 0.0f}, 0.5f}};
    const float center = gpu_meshing::evaluate_particle_field_reference(
        centered, 1, 0.0f, {0.0f, 0.0f, 0.0f});
    CHECK(std::fabs(center + 0.5f) < 1e-6f,
          "sphere center equals negative radius");
}

void test_phase_weighted_reference_field_blends_two_captures() {
    gpu_meshing::ParticleSample particles[2] = {
        {{-0.35f, 0.0f, 0.0f}, 0.5f},
        {{0.45f, 0.0f, 0.0f}, 0.5f},
    };
    constexpr float blend = 0.18f;
    const matter::Float3 probe{0.1f, 0.05f, 0.0f};
    const float primary_only =
        gpu_meshing::evaluate_particle_field_reference(
            particles, 2u, blend, {1u, 1.0f, 0.0f}, probe);
    const float primary_reference =
        gpu_meshing::evaluate_particle_field_reference(
            particles, 1u, blend, probe);
    CHECK(std::memcmp(&primary_only, &primary_reference, sizeof(float)) == 0,
          "primary weight one ignores the secondary capture exactly");

    const float secondary_only =
        gpu_meshing::evaluate_particle_field_reference(
            particles, 2u, blend, {1u, 0.0f, 1.0f}, probe);
    const float secondary_reference =
        gpu_meshing::evaluate_particle_field_reference(
            particles + 1u, 1u, blend, probe);
    CHECK(std::memcmp(&secondary_only, &secondary_reference, sizeof(float)) == 0,
          "secondary weight one ignores the primary capture exactly");

    particles[1] = particles[0];
    const float half_and_half =
        gpu_meshing::evaluate_particle_field_reference(
            particles, 2u, blend, {1u, 0.5f, 0.5f}, probe);
    CHECK(std::fabs(half_and_half - primary_reference) <= 1e-6f,
          "two coincident half-weight particles equal one full-weight particle");

    particles[1].position_m.x =
        std::numeric_limits<float>::quiet_NaN();
    const float skipped_nan =
        gpu_meshing::evaluate_particle_field_reference(
            particles, 2u, blend, {1u, 1.0f, 0.0f}, probe);
    CHECK(std::isfinite(skipped_nan) &&
              std::memcmp(&skipped_nan, &primary_reference,
                          sizeof(float)) == 0,
          "a zero-weight phase is skipped before it can introduce NaN");
}

void test_phase_weighted_job_validation_fails_closed() {
    gpu_meshing::ParticleSample particles[2] = {
        {{-0.25f, 0.0f, 0.0f}, 0.5f},
        {{0.25f, 0.0f, 0.0f}, 0.5f},
    };
    gpu_meshing::ParticleSample one_particle[1];
    auto job = one_sphere_job(one_particle);
    job.particles = particles;
    job.particle_count = 2u;
    job.blend_width_m = 0.15f;
    CHECK(gpu_meshing::resolved_particle_phase_split(job) == 2u,
          "a default static descriptor resolves every particle as primary");
    job.phase_blend = {1u, 1.0f, 0.0f};
    CHECK(gpu_meshing::resolved_particle_phase_split(job) == 1u,
          "an explicit animation endpoint retains its authored split");

    job.phase_blend = {3u, 0.5f, 0.5f};
    CHECK(rejects(job, gpu_meshing::ErrorCode::InvalidInput),
          "phase split beyond particle count is rejected");
    job.phase_blend = {1u, -0.1f, 1.1f};
    CHECK(rejects(job, gpu_meshing::ErrorCode::InvalidInput),
          "negative phase weight is rejected");
    job.phase_blend = {
        1u, 0.5f, std::numeric_limits<float>::quiet_NaN()};
    CHECK(rejects(job, gpu_meshing::ErrorCode::InvalidInput),
          "non-finite phase weight is rejected");
    job.phase_blend = {1u, 0.4f, 0.5f};
    CHECK(rejects(job, gpu_meshing::ErrorCode::InvalidInput),
          "phase weights must sum to one");
    job.phase_blend = {1u, 0.5f, 0.5f};
    job.blend_width_m = 0.0f;
    CHECK(rejects(job, gpu_meshing::ErrorCode::InvalidInput),
          "dual-phase jobs require a nonzero smooth-min width");
}

void test_reference_scan_covers_empty_zero_max_and_overflow() {
    std::vector<std::uint32_t> output;
    std::uint32_t total = 99;
    CHECK(gpu_meshing::exclusive_scan_reference({}, output, total) &&
              output.empty() && total == 0,
          "empty exclusive scan succeeds with zero total");

    const std::vector<std::uint32_t> input{3, 0, 2, 5};
    const std::vector<std::uint32_t> expected{0, 3, 3, 5};
    CHECK(gpu_meshing::exclusive_scan_reference(input, output, total),
          "exclusive scan succeeds");
    CHECK(output == expected && total == 10,
          "exclusive scan and total are exact");

    const std::vector<std::uint32_t> edge{
        std::numeric_limits<std::uint32_t>::max(), 0};
    const std::vector<std::uint32_t> expected_edge{
        0, std::numeric_limits<std::uint32_t>::max()};
    CHECK(gpu_meshing::exclusive_scan_reference(edge, output, total) &&
              output == expected_edge &&
              total == std::numeric_limits<std::uint32_t>::max(),
          "maximum representable scan total succeeds");

    const std::vector<std::uint32_t> overflow{
        std::numeric_limits<std::uint32_t>::max(), 1};
    CHECK(!gpu_meshing::exclusive_scan_reference(overflow, output, total) &&
              output.empty() && total == 0,
          "exclusive scan rejects uint32 overflow without partial output");
}

void test_mesh_digest_is_stable_and_sensitive() {
    gpu_meshing::MeshResult mesh{};
    mesh.positions = {0.0f, 1.0f, 2.0f};
    mesh.normals = {0.0f, 1.0f, 0.0f};
    mesh.indices = {0};
    mesh.material = 4;
    const std::uint64_t digest = gpu_meshing::mesh_content_digest(mesh);
    CHECK(digest != 0 && digest == gpu_meshing::mesh_content_digest(mesh),
          "mesh digest is stable and nonzero");

    auto changed = mesh;
    changed.positions[2] = 3.0f;
    CHECK(gpu_meshing::mesh_content_digest(changed) != digest,
          "position bytes affect mesh digest");
    changed = mesh;
    changed.normals[1] = -1.0f;
    CHECK(gpu_meshing::mesh_content_digest(changed) != digest,
          "normal bytes affect mesh digest");
    changed = mesh;
    changed.indices[0] = 1;
    CHECK(gpu_meshing::mesh_content_digest(changed) != digest,
          "index bytes affect mesh digest");
    changed = mesh;
    changed.material = 7;
    CHECK(gpu_meshing::mesh_content_digest(changed) != digest,
          "material affects mesh digest");
}

void test_gameplay_identity_ignores_visual_job_bounds() {
    gpu_meshing::ParticleSample particles[1];
    const auto job = one_sphere_job(particles);
    hydrology::ProductIdentitySettings settings{};
    settings.semantic_key = 91u;
    const hydrology::GameplayFieldLayout gameplay_layout{
        {-1.0f, 0.0f, -1.0f}, 0.5f, 4u, 4u};
    const std::uint64_t snapshot = hydrology::particle_snapshot_digest(particles, 1u);
    const auto first = hydrology::derive_product_keys(
        job, snapshot, settings, 0.25f, gameplay_layout);
    auto visual_bounds_changed = job;
    visual_bounds_changed.bounds_m.max_m.x = 4.0f;
    const auto second = hydrology::derive_product_keys(
        visual_bounds_changed, snapshot, settings, 0.25f, gameplay_layout);
    CHECK(first.visual != second.visual && first.coarse_cpu != second.coarse_cpu &&
              first.gameplay == second.gameplay,
          "visual and CPU bounds do not spuriously invalidate the gameplay field key");
}

void test_canonical_lattice_identity_is_visual_only() {
    gpu_meshing::ParticleSample particles[1];
    auto job = one_sphere_job(particles);
    job.sampling_lattice = {{-31.2f, 7.4f, 11.8f}, 0.25f, 1u};
    hydrology::ProductIdentitySettings settings{};
    settings.semantic_key = 94u;
    const hydrology::GameplayFieldLayout gameplay_layout{
        {-1.0f, 0.0f, -1.0f}, 0.5f, 4u, 4u};
    const auto keys = hydrology::derive_product_keys(
        job, hydrology::particle_snapshot_digest(particles, 1u), settings,
        0.5f, gameplay_layout);

    auto origin_changed = job;
    origin_changed.sampling_lattice.origin_m.x += 3.0f;
    const auto origin_keys = hydrology::derive_product_keys(
        origin_changed,
        hydrology::particle_snapshot_digest(particles, 1u), settings,
        0.5f, gameplay_layout);
    CHECK(keys.visual != origin_keys.visual &&
              keys.coarse_cpu == origin_keys.coarse_cpu &&
              keys.gameplay == origin_keys.gameplay &&
              keys.presentation == origin_keys.presentation,
          "changing the canonical origin invalidates only the visual product key");

    auto voxel_changed = job;
    voxel_changed.voxel_m = 0.2f;
    voxel_changed.sampling_lattice.voxel_m = 0.2f;
    const auto voxel_keys = hydrology::derive_product_keys(
        voxel_changed,
        hydrology::particle_snapshot_digest(particles, 1u), settings,
        0.5f, gameplay_layout);
    CHECK(keys.visual != voxel_keys.visual &&
              keys.coarse_cpu == voxel_keys.coarse_cpu &&
              keys.gameplay == voxel_keys.gameplay &&
              keys.presentation == voxel_keys.presentation,
          "changing the visual lattice voxel leaves collision and gameplay identities reusable");

    auto version_changed = job;
    version_changed.sampling_lattice.version = 0u;
    const auto version_keys = hydrology::derive_product_keys(
        version_changed,
        hydrology::particle_snapshot_digest(particles, 1u), settings,
        0.5f, gameplay_layout);
    CHECK(keys.visual != version_keys.visual &&
              keys.coarse_cpu == version_keys.coarse_cpu &&
              keys.gameplay == version_keys.gameplay &&
              keys.presentation == version_keys.presentation,
          "changing lattice contract version invalidates only visual identity");
}

void test_coarse_identity_includes_cpu_mesher_blend_width() {
    gpu_meshing::ParticleSample particles[1];
    const auto job = one_sphere_job(particles);
    hydrology::ProductIdentitySettings settings{};
    settings.semantic_key = 92u;
    const hydrology::GameplayFieldLayout gameplay_layout{
        {-1.0f, 0.0f, -1.0f}, 0.5f, 4u, 4u};
    const std::uint64_t snapshot = hydrology::particle_snapshot_digest(particles, 1u);
    const auto first = hydrology::derive_product_keys(
        job, snapshot, settings, 0.25f, gameplay_layout);
    auto blend_changed = job;
    blend_changed.blend_width_m = 0.1f;
    const auto second = hydrology::derive_product_keys(
        blend_changed, snapshot, settings, 0.25f, gameplay_layout);
    CHECK(first.visual != second.visual &&
              first.coarse_cpu != second.coarse_cpu &&
              first.gameplay == second.gameplay,
          "the CPU mesher blend width invalidates its coarse key without churning gameplay");
}

void test_gameplay_sampling_requires_all_bilinear_contributors() {
    const hydrology::GameplayFieldLayout layout{{0.0f, 0.0f, 0.0f}, 1.0f,
                                                2u, 2u};
    std::vector<hydrology::GameplaySample> wet = {
        {2.0f, 1.0f, 1.0f, 2.0f, 3.0f, true},
        {4.0f, 3.0f, 3.0f, 4.0f, 5.0f, true},
        {6.0f, 5.0f, 5.0f, 6.0f, 7.0f, true},
        {8.0f, 7.0f, 7.0f, 8.0f, 9.0f, true},
    };
    hydrology::GameplaySample sample{};
    CHECK(hydrology::sample_fluid_gameplay_field(layout, wet, 1.0f, 1.0f,
                                                  sample),
          "cell-centre bilinear sampling accepts wet contributors");
    CHECK(sample.height_m == 5.0f && sample.depth_m == 4.0f &&
              sample.velocity_x_mps == 4.0f && sample.velocity_y_mps == 5.0f &&
              sample.velocity_z_mps == 6.0f && sample.wet_valid,
          "bilinear sampling preserves all continuous and 3D velocity channels");

    wet[1].height_m = std::numeric_limits<float>::quiet_NaN();
    CHECK(hydrology::sample_fluid_gameplay_field(layout, wet, 0.5f, 0.5f,
                                                  sample),
          "zero-weight invalid gameplay contributors are skipped");
    wet[1].height_m = 4.0f;

    wet[3].wet_valid = false;
    CHECK(!hydrology::sample_fluid_gameplay_field(layout, wet, 1.0f, 1.0f,
                                                   sample) &&
              sample == hydrology::GameplaySample{},
          "wet dry edge returns no partially blended gameplay surface");
    CHECK(!hydrology::sample_fluid_gameplay_field(layout, wet, -0.01f, 0.5f,
                                                   sample),
          "gameplay sampling does not clamp out of bounds coordinates");
    wet[0].wet_valid = false;
    CHECK(!hydrology::sample_fluid_gameplay_field(layout, wet, 0.5f, 0.5f,
                                                   sample),
          "gameplay sampling rejects a dry border cell");
}

void test_presentation_identity_is_independent_of_visual_identity() {
    gpu_meshing::ParticleSample particles[1];
    const auto job = one_sphere_job(particles);
    hydrology::ProductIdentitySettings settings{};
    settings.semantic_key = 93u;
    const hydrology::GameplayFieldLayout gameplay_layout{
        {-1.0f, 0.0f, -1.0f}, 0.5f, 4u, 4u};
    const auto first = hydrology::derive_product_keys(
        job, hydrology::particle_snapshot_digest(particles, 1u), settings,
        0.25f, gameplay_layout);
    settings.presentation.wake_distance_weight += 0.25f;
    const auto second = hydrology::derive_product_keys(
        job, hydrology::particle_snapshot_digest(particles, 1u), settings,
        0.25f, gameplay_layout);
    CHECK(first.visual == second.visual && first.coarse_cpu == second.coarse_cpu &&
              first.gameplay == second.gameplay &&
              first.presentation != second.presentation,
          "presentation-only settings change only the presentation key");

    settings.presentation_local_overrides.push_back({1.0f, 1.0f, 1.5f});
    const auto overridden = hydrology::derive_product_keys(
        job, hydrology::particle_snapshot_digest(particles, 1u), settings,
        0.25f, gameplay_layout);
    CHECK(second.visual == overridden.visual &&
              second.coarse_cpu == overridden.coarse_cpu &&
              second.gameplay == overridden.gameplay &&
              second.presentation != overridden.presentation,
          "canonical local overrides change only the presentation key");
    settings.presentation.pool_weight = 2.0f;
    const auto pool_changed = hydrology::derive_product_keys(
        job, hydrology::particle_snapshot_digest(particles, 1u), settings,
        0.25f, gameplay_layout);
    CHECK(overridden.visual == pool_changed.visual &&
              overridden.coarse_cpu == pool_changed.coarse_cpu &&
              overridden.gameplay == pool_changed.gameplay &&
              overridden.presentation != pool_changed.presentation,
          "pool calm evidence changes only the presentation key");
}

void test_v5_cache_keys_require_persisted_presentation_identity() {
    const hydrology::ProductKeys persisted{11u, 22u, 33u, 0u};
    const hydrology::ProductKeys expected{11u, 22u, 33u, 44u};
    CHECK(!(persisted == expected),
          "v5 cache matching rejects a missing persisted presentation key");
}

}  // namespace

int main() {
    test_world_anchored_lattice_expands_outward_without_drifting();
    test_canonical_lattice_faces_round_trip_to_the_same_integer_cell();
    test_canonical_lattice_rejects_support_and_grid_overflow_before_allocation();
    test_validates_and_derives_particle_grid();
    test_validation_fails_closed_without_rejecting_supported_edges();
    test_reference_field_matches_matter_surface_oracle();
    test_phase_weighted_reference_field_blends_two_captures();
    test_phase_weighted_job_validation_fails_closed();
    test_reference_scan_covers_empty_zero_max_and_overflow();
    test_mesh_digest_is_stable_and_sensitive();
    test_gameplay_identity_ignores_visual_job_bounds();
    test_canonical_lattice_identity_is_visual_only();
    test_coarse_identity_includes_cpu_mesher_blend_width();
    test_gameplay_sampling_requires_all_bilinear_contributors();
    test_presentation_identity_is_independent_of_visual_identity();
    test_v5_cache_keys_require_persisted_presentation_identity();
    return check_summary();
}
