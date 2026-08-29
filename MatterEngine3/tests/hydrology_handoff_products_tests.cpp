#include "check.h"
#include "hydrology/hydrology_handoff_products.h"
#include "hydrology/river_presentation_field.h"
#include "hydrology/water_mesh_continuity.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

gpu_meshing::MeshResult quad(float x0, float x1, float y,
                             float z0 = -5.0f, float z1 = 5.0f) {
    gpu_meshing::MeshResult mesh{};
    mesh.positions = {x0, y, z0, x1, y, z0,
                      x1, y, z1, x0, y, z1};
    mesh.normals = {0,1,0, 0,1,0, 0,1,0, 0,1,0};
    mesh.indices = {0,1,2, 0,2,3};
    mesh.material = 4u;
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
    return mesh;
}

gpu_meshing::MeshResult combine(gpu_meshing::MeshResult first,
                                const gpu_meshing::MeshResult& second) {
    const auto offset = static_cast<std::uint32_t>(
        first.positions.size() / 3u);
    first.positions.insert(first.positions.end(), second.positions.begin(),
                           second.positions.end());
    first.normals.insert(first.normals.end(), second.normals.begin(),
                         second.normals.end());
    for (const auto index : second.indices)
        first.indices.push_back(offset + index);
    first.content_digest = gpu_meshing::mesh_content_digest(first);
    return first;
}

void append_dam_curtain(gpu_meshing::MeshResult& mesh) {
    const std::uint32_t base =
        static_cast<std::uint32_t>(mesh.positions.size() / 3u);
    mesh.positions.insert(mesh.positions.end(), {
        5.0f, 0.5f, -5.0f, 5.0f, 3.0f, -5.0f,
        5.0f, 3.0f, 5.0f, 5.0f, 0.5f, 5.0f});
    mesh.normals.insert(mesh.normals.end(), {
        -1,0,0, -1,0,0, -1,0,0, -1,0,0});
    mesh.indices.insert(mesh.indices.end(), {
        base, base + 1u, base + 2u, base, base + 2u, base + 3u});
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
}

hydrology::HydrologyArtifact section_artifact(bool upstream) {
    hydrology::HydrologyArtifact artifact{};
    artifact.section = upstream
        ? hydrology::HydrologySectionIdentity{"upper", "main", 0, 10, -1, 12.5f}
        : hydrology::HydrologySectionIdentity{"lower", "main", 10, 20, 7.5f, 21};
    artifact.semantic_key = upstream ? 101u : 202u;
    artifact.payload_digest = upstream ? 1001u : 2002u;
    artifact.particle_radius_m = 0.25f;
    artifact.accepted = true;
    artifact.visual_mesh = quad(upstream ? -10.0f : 2.5f,
                                upstream ? -2.5f : 10.0f, 1.0f);
    artifact.coarse_cpu_mesh = quad(upstream ? -10.0f : 0.0f,
                                    upstream ? 0.0f : 10.0f, 1.0f);
    if (upstream) append_dam_curtain(artifact.visual_mesh);
    artifact.particles = upstream
        ? std::vector<hydrology::FluidParticle>{
              {{-5,1,0}, {1,0,0}, 1}, {{-1,1,0}, {3,0,0}, 2},
              {{5,1,0}, {0,0,0}, 3}}
        : std::vector<hydrology::FluidParticle>{
              {{1,1,0}, {6,0,0}, 4}, {{5,1,0}, {6,0,0}, 5}};
    artifact.gameplay_layout = upstream
        ? hydrology::GameplayFieldLayout{{-10,0,-1}, 1, 15, 2}
        : hydrology::GameplayFieldLayout{{-5,0,-1}, 1, 15, 2};
    artifact.gameplay_field.assign(
        artifact.gameplay_layout.width * artifact.gameplay_layout.depth,
        hydrology::GameplaySample{upstream ? 2.0f : 5.0f,
                                  upstream ? 1.0f : 2.0f,
                                  upstream ? 1.0f : 6.0f,
                                  upstream ? -0.25f : 0.75f,
                                  upstream ? -0.5f : 1.5f,true});
    artifact.presentation_field.assign(
        artifact.gameplay_layout.width * artifact.gameplay_layout.depth,
        hydrology::PresentationSample{
            upstream ? 0.1f : 0.5f, upstream ? 0.2f : -0.2f,
            upstream ? 0.2f : 0.8f, upstream ? 0.3f : 0.7f,
            upstream ? 0.1f : 0.9f,
            upstream ? hydrology::RiverFeature::Current
                     : hydrology::RiverFeature::Spillway,
            true});
    if (upstream) {
        for (std::uint32_t z = 0; z != 2; ++z)
            for (std::uint32_t x = 9; x != 15; ++x)
                artifact.gameplay_field[z * 15u + x].velocity_x_mps = 3.0f;
    } else {
        for (std::uint32_t z = 0; z != 2; ++z) {
            artifact.gameplay_field[z * 15u + 13u] = {};
            artifact.presentation_field[z * 15u + 13u] = {};
        }
    }
    return artifact;
}

hydrology::SpillwayHandoffRecord spillway() {
    hydrology::SpillwayHandoffRecord handoff{};
    handoff.id = "pool-one";
    handoff.upstream_section_id = "upper";
    handoff.downstream_section_id = "lower";
    handoff.lip_origin_m = {};
    handoff.tangent = {1,0,0};
    handoff.lateral = {0,0,1};
    handoff.up = {0,1,0};
    handoff.discharge_m3s = 20;
    handoff.width_m = 10;
    handoff.effective_depth_m = 2;
    handoff.initial_speed_mps = 1;
    handoff.overlap_m = 5;
    handoff.upstream_visual_cut_m = -2.5f;
    handoff.downstream_visual_cut_m = 2.5f;
    handoff.temporary_dam_exclusion_bounds_m =
        {{4.75f, 0.5f, -5.0f}, {5.25f, 3.0f, 5.0f}};
    handoff.semantic_key = hydrology::spillway_handoff_semantic_key(handoff);
    return handoff;
}

hydrology::HandoffProductSettings settings() {
    hydrology::HandoffProductSettings result{};
    result.visual_job.bounds_m = {{-6,-1,-6}, {6,4,6}};
    result.visual_job.voxel_m = 0.5f;
    result.visual_job.blend_width_m = 0.2f;
    result.visual_job.iso_value = 0.0f;
    result.visual_job.material = 4;
    result.visual_job.limits = {1000, 100000, 100000, 300000};
    result.particle_radius_m = 0.25f;
    result.gameplay_layout = {{-10,0,-1}, 1, 20, 2};
    return result;
}

hydrology::WaterMeshAnimationArtifact animation_artifact(
    const char* id, bool upstream) {
    hydrology::WaterMeshAnimation animation{};
    animation.frames_per_second = 30u;
    animation.phase_offset_frames = 15u;
    animation.duration_seconds = 1.0f;
    for (std::uint32_t frame = 0u; frame != 30u; ++frame) {
        animation.frames.push_back(quad(
            upstream ? -10.0f : 2.5f,
            upstream ? -2.5f : 10.0f,
            1.0f + static_cast<float>(frame) * 0.001f,
            upstream ? -4.25f : -4.5f,
            upstream ? 4.25f : 4.5f));
    }
    hydrology::WaterMeshAnimationArtifact artifact{};
    gpu_meshing::Error error{};
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              {id, upstream ? 1011u : 2022u,
               upstream ? 1001u : 2002u, 0u, 0.5f,
               {{0.0f, 0.0f, 0.0f}, 0.5f, 1u}},
              animation, artifact, error), error.message.c_str());
    return artifact;
}

gpu_meshing::MeshResult branched_cut_mesh(float cut_x, float far_x) {
    gpu_meshing::MeshResult mesh{};
    const matter::Float3 centre{cut_x, 1.0f, 0.0f};
    const matter::Float3 far_centre{far_x, 1.0f, 0.0f};
    const matter::Float3 outer[] = {
        {cut_x, 1.0f, -4.0f}, {cut_x, 1.0f, 4.0f},
        {cut_x, 0.25f, 0.0f}, {cut_x, 2.0f, 0.0f},
    };
    for (const auto& point : outer) {
        const std::uint32_t base = static_cast<std::uint32_t>(
            mesh.positions.size() / 3u);
        const matter::Float3 far_point{far_x, point.y, point.z};
        for (const auto& vertex : {centre, point, far_point, far_centre}) {
            mesh.positions.insert(mesh.positions.end(),
                                  {vertex.x, vertex.y, vertex.z});
            mesh.normals.insert(mesh.normals.end(), {0.0f, 1.0f, 0.0f});
        }
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1u, base + 2u,
                             base, base + 2u, base + 3u});
    }
    mesh.material = 4u;
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
    return mesh;
}

hydrology::WaterMeshAnimationArtifact branched_animation_artifact() {
    hydrology::WaterMeshAnimation animation{};
    animation.frames_per_second = 30u;
    animation.phase_offset_frames = 15u;
    animation.duration_seconds = 1.0f;
    animation.frames.assign(30u, quad(2.5f, 10.0f, 1.0f, -4.5f, 4.5f));
    animation.frames[7u] = branched_cut_mesh(2.5f, 10.0f);
    hydrology::WaterMeshAnimationArtifact artifact{};
    gpu_meshing::Error error{};
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              {"lower", 2022u, 2002u, 0u, 0.5f,
               {{0.0f, 0.0f, 0.0f}, 0.5f, 1u}},
              animation, artifact, error), error.message.c_str());
    return artifact;
}

gpu_meshing::ParticleSamplingLattice shared_lattice() {
    gpu_meshing::ParticleSamplingLattice lattice{};
    gpu_meshing::Error error{};
    CHECK(gpu_meshing::make_particle_sampling_lattice(
              {0.0f, 0.0f, 0.0f}, 0.25f, lattice, error),
          error.message.c_str());
    return lattice;
}

hydrology::SpillwayHandoffRecord shared_spillway() {
    auto handoff = spillway();
    handoff.lip_origin_m = {0.0f, 1.0f, 0.0f};
    handoff.width_m = 1.0f;
    handoff.effective_depth_m = 0.5f;
    handoff.channel_depth_m = 0.25f;
    handoff.overlap_m = 2.0f;
    handoff.upstream_visual_cut_m = -0.5f;
    handoff.downstream_visual_cut_m = 0.5f;
    handoff.temporary_dam_exclusion_bounds_m =
        {{0.75f, 0.8f, -0.5f}, {1.0f, 1.6f, 0.5f}};
    handoff.semantic_key = hydrology::spillway_handoff_semantic_key(handoff);
    return handoff;
}

hydrology::FluidParticleAnimationCapture boundary_capture(
    bool upstream, const gpu_meshing::Aabb& crop) {
    hydrology::FluidParticleAnimationCapture capture{};
    capture.frames_per_second = 30u;
    capture.phase_offset_frames = 15u;
    capture.frames.resize(30u);
    const float near_min_x = crop.min_m.x + 0.01f;
    const float near_max_x = crop.max_m.x - 0.01f;
    const float near_min_z = crop.min_m.z + 0.01f;
    const float near_max_z = crop.max_m.z - 0.01f;
    for (std::uint32_t frame = 0u; frame != 30u; ++frame) {
        auto& output = capture.frames[frame];
        output.simulation_step = (frame + 1u) * 4u;
        const float phase = static_cast<float>(frame) * 0.001f;
        output.positions_m = {
            {upstream ? near_min_x : 0.0f, 0.75f + phase, near_min_z},
            {upstream ? near_min_x : 0.0f, 0.75f + phase, near_max_z},
            {upstream ? 0.0f : near_max_x, 1.25f + phase, near_min_z},
            {upstream ? 0.0f : near_max_x, 1.25f + phase, near_max_z},
            {0.0f, upstream ? 1.10f + phase : 1.20f + phase, 0.0f},
            {upstream ? 0.45f : -0.45f,
             upstream ? 1.30f + phase : 1.40f + phase, 0.0f},
        };
        if (upstream)
            output.positions_m.push_back({0.875f, 1.50f + phase, 0.0f});
    }
    return capture;
}

hydrology::WaterBoundaryAnimationSource boundary_source(bool upstream) {
    const auto handoff = shared_spillway();
    const auto lattice = shared_lattice();
    hydrology::FluidParticleAnimationCapture empty{};
    empty.frames_per_second = 30u;
    empty.phase_offset_frames = 15u;
    empty.frames.resize(30u);
    for (std::uint32_t frame = 0u; frame != 30u; ++frame)
        empty.frames[frame].simulation_step = (frame + 1u) * 4u;
    hydrology::WaterBoundaryAnimationSource metadata{};
    gpu_meshing::Error error{};
    CHECK(hydrology::build_water_boundary_animation_source(
              empty, upstream ? "upper" : "lower",
              upstream ? 1001u : 2002u, handoff, lattice,
              0.05f, 0.01f, upstream, metadata, error),
          error.message.c_str());
    hydrology::WaterBoundaryAnimationSource source{};
    CHECK(hydrology::build_water_boundary_animation_source(
              boundary_capture(upstream, metadata.crop_bounds_m),
              upstream ? "upper" : "lower",
              upstream ? 1001u : 2002u, handoff, lattice,
              0.05f, 0.01f, upstream, source, error),
          error.message.c_str());
    return source;
}

bool canonical_plane_mesher(
    const gpu_meshing::ParticleJob& job, gpu_meshing::MeshResult& mesh,
    gpu_meshing::Stats& stats, gpu_meshing::Error& error,
    const gpu_meshing::BuildControl&) {
    mesh = {};
    gpu_meshing::GridLayout layout{};
    if (!gpu_meshing::validate_particle_job(job, layout, error)) return false;
    const float y = 1.0f;
    const float top = layout.origin_m.y + layout.spacing_m.y *
        static_cast<float>(layout.cell_dims[1]);
    if (y < layout.origin_m.y || y >= top) {
        mesh.material = job.material;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
        return true;
    }
    for (std::uint32_t z = 0u; z != layout.cell_dims[2]; ++z) {
        for (std::uint32_t x = 0u; x != layout.cell_dims[0]; ++x) {
            const float x0 = layout.origin_m.x +
                static_cast<float>(x) * layout.spacing_m.x;
            const float x1 = x0 + layout.spacing_m.x;
            const float z0 = layout.origin_m.z +
                static_cast<float>(z) * layout.spacing_m.z;
            const float z1 = z0 + layout.spacing_m.z;
            const std::uint32_t base = static_cast<std::uint32_t>(
                mesh.positions.size() / 3u);
            mesh.positions.insert(mesh.positions.end(), {
                x0, y, z0, x1, y, z0, x1, y, z1, x0, y, z1});
            mesh.normals.insert(mesh.normals.end(), {
                0,1,0, 0,1,0, 0,1,0, 0,1,0});
            mesh.indices.insert(mesh.indices.end(), {
                base, base + 1u, base + 2u,
                base, base + 2u, base + 3u});
        }
    }
    mesh.material = job.material;
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
    stats.particles = job.particle_count;
    stats.triangles = static_cast<std::uint32_t>(mesh.indices.size() / 3u);
    error = {};
    return true;
}

std::vector<std::string> canonical_triangles(
    const gpu_meshing::MeshResult& mesh, float tolerance) {
    std::vector<std::string> triangles;
    for (std::size_t offset = 0u; offset != mesh.indices.size();
         offset += 3u) {
        std::array<std::string, 3> points{};
        for (std::size_t corner = 0u; corner != 3u; ++corner) {
            const auto vertex = mesh.indices[offset + corner];
            std::ostringstream value;
            value << std::llround(mesh.positions[vertex * 3u + 0u] /
                                  tolerance) << ':'
                  << std::llround(mesh.positions[vertex * 3u + 1u] /
                                  tolerance) << ':'
                  << std::llround(mesh.positions[vertex * 3u + 2u] /
                                  tolerance);
            points[corner] = value.str();
        }
        std::sort(points.begin(), points.end());
        triangles.push_back(points[0] + "|" + points[1] + "|" + points[2]);
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

struct SharedFieldFixture {
    hydrology::WaterBoundaryAnimationSource upstream_source;
    hydrology::WaterBoundaryAnimationSource downstream_source;
    hydrology::WaterMeshAnimationArtifact upstream_bulk;
    hydrology::WaterMeshAnimationArtifact downstream_bulk;
    hydrology::HandoffAnimationBuildInput input{};
};

SharedFieldFixture shared_field_fixture() {
    SharedFieldFixture fixture{};
    fixture.upstream_source = boundary_source(true);
    fixture.downstream_source = boundary_source(false);
    const auto lattice = shared_lattice();
    gpu_meshing::Aabb bounds{
        {std::min(fixture.upstream_source.crop_bounds_m.min_m.x,
                  fixture.downstream_source.crop_bounds_m.min_m.x),
         std::min(fixture.upstream_source.crop_bounds_m.min_m.y,
                  fixture.downstream_source.crop_bounds_m.min_m.y),
         std::min(fixture.upstream_source.crop_bounds_m.min_m.z,
                  fixture.downstream_source.crop_bounds_m.min_m.z)},
        {std::max(fixture.upstream_source.crop_bounds_m.max_m.x,
                  fixture.downstream_source.crop_bounds_m.max_m.x),
         std::max(fixture.upstream_source.crop_bounds_m.max_m.y,
                  fixture.downstream_source.crop_bounds_m.max_m.y),
         std::max(fixture.upstream_source.crop_bounds_m.max_m.z,
                  fixture.downstream_source.crop_bounds_m.max_m.z)}};
    gpu_meshing::ParticleJob visual{};
    visual.bounds_m = bounds;
    visual.voxel_m = lattice.voxel_m;
    visual.blend_width_m = 0.01f;
    visual.iso_value = 0.0f;
    visual.material = 4u;
    visual.limits = {512u, 1u << 20u, 1u << 22u, 1u << 23u};
    visual.sampling_lattice = lattice;

    gpu_meshing::MeshResult full{};
    gpu_meshing::Stats stats{};
    gpu_meshing::Error mesh_error{};
    CHECK(canonical_plane_mesher(visual, full, stats, mesh_error, {}),
          mesh_error.message.c_str());
    hydrology::WaterMeshAnimation raw{};
    raw.frames_per_second = 30u;
    raw.phase_offset_frames = 15u;
    raw.duration_seconds = 1.0f;
    raw.frames.assign(30u, full);
    hydrology::WaterMeshAnimation upstream_owned{};
    hydrology::WaterMeshAnimation downstream_owned{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::clip_section_water_mesh_animation(
              raw, "upper", {shared_spillway()}, lattice,
              upstream_owned, error), error.message.c_str());
    CHECK(hydrology::clip_section_water_mesh_animation(
              raw, "lower", {shared_spillway()}, lattice,
              downstream_owned, error), error.message.c_str());
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              {"upper", 1011u, 1001u, 0u, lattice.voxel_m, lattice},
              upstream_owned, fixture.upstream_bulk, mesh_error),
          mesh_error.message.c_str());
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              {"lower", 2022u, 2002u, 0u, lattice.voxel_m, lattice},
              downstream_owned, fixture.downstream_bulk, mesh_error),
          mesh_error.message.c_str());
    std::vector<std::uint8_t> upstream_bytes;
    std::vector<std::uint8_t> downstream_bytes;
    CHECK(hydrology::serialize_water_mesh_animation_artifact(
              fixture.upstream_bulk, upstream_bytes, mesh_error) &&
              hydrology::deserialize_water_mesh_animation_artifact(
                  upstream_bytes, fixture.upstream_bulk, mesh_error),
          mesh_error.message.c_str());
    CHECK(hydrology::serialize_water_mesh_animation_artifact(
              fixture.downstream_bulk, downstream_bytes, mesh_error) &&
              hydrology::deserialize_water_mesh_animation_artifact(
                  downstream_bytes, fixture.downstream_bulk, mesh_error),
          mesh_error.message.c_str());
    fixture.input.upstream = &fixture.upstream_source;
    fixture.input.downstream = &fixture.downstream_source;
    fixture.input.upstream_bulk = &fixture.upstream_bulk;
    fixture.input.downstream_bulk = &fixture.downstream_bulk;
    fixture.input.handoff = shared_spillway();
    fixture.input.lattice = lattice;
    fixture.input.visual_template = visual;
    return fixture;
}

void bind_shared_field_fixture(SharedFieldFixture& fixture) {
    fixture.input.upstream = &fixture.upstream_source;
    fixture.input.downstream = &fixture.downstream_source;
    fixture.input.upstream_bulk = &fixture.upstream_bulk;
    fixture.input.downstream_bulk = &fixture.downstream_bulk;
}

const hydrology::PhysxFluidBake::VisualMesher synthetic_visual_mesher =
    [](const gpu_meshing::ParticleJob& job, gpu_meshing::MeshResult& mesh,
       gpu_meshing::Stats& stats, gpu_meshing::Error& error,
       const gpu_meshing::BuildControl&) {
        if (job.particle_count == 0u) {
            error = {gpu_meshing::ErrorCode::InvalidInput,
                     "synthetic seam requires collar particles"};
            return false;
        }
        mesh = quad(-5.0f, 5.0f, 1.0f);
        stats.particles = job.particle_count;
        stats.triangles = 2u;
        error = {};
        return true;
    };

gpu_meshing::MeshResult continuity_ribbon(float height_m) {
    return quad(-5.0f, 0.0f, height_m, -4.0f, 4.0f);
}

void set_mesh_normal(gpu_meshing::MeshResult& mesh,
                     matter::Float3 normal) {
    for (std::size_t index = 0u; index != mesh.normals.size(); index += 3u) {
        mesh.normals[index + 0u] = normal.x;
        mesh.normals[index + 1u] = normal.y;
        mesh.normals[index + 2u] = normal.z;
    }
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
}

void append_duplicate_coplanar_triangle(gpu_meshing::MeshResult& mesh,
                                        float cut_x) {
    const auto append = [&]() {
        const auto base = static_cast<std::uint32_t>(
            mesh.positions.size() / 3u);
        mesh.positions.insert(mesh.positions.end(), {
            cut_x, 44.0f, -1.0f,
            cut_x, 45.0f, 0.0f,
            cut_x, 44.0f, 1.0f});
        mesh.normals.insert(mesh.normals.end(), {
            1.0f, 0.0f, 0.0f,
            1.0f, 0.0f, 0.0f,
            1.0f, 0.0f, 0.0f});
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1u, base + 2u});
    };
    append();
    append();
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
}

void test_measures_section_cut_continuity_without_welding() {
    const auto handoff = spillway();
    hydrology::WaterCutContourMetrics metrics{};
    hydrology::FluidBakeError error{};

    CHECK(hydrology::measure_water_cut_continuity(
              continuity_ribbon(44.9f), continuity_ribbon(48.8f), handoff,
              -2.5f, 1.0e-4f, metrics, error),
          error.message.c_str());
    CHECK(metrics.first_points != 0u && metrics.second_points != 0u,
          "intersected ribbon contours report deterministic point counts");
    CHECK(metrics.symmetric_hausdorff_m >= 3.8f &&
              std::fabs(metrics.first_height_quantiles_m[1] -
                        metrics.second_height_quantiles_m[1]) >= 3.8f,
          "the historical section-height mismatch remains measurable");
    CHECK(!hydrology::water_cut_is_assertion_weldable(
              metrics, 0.15f / 16.0f),
          "a multi-metre contour mismatch is never accepted as a weld");

    const auto exact = continuity_ribbon(44.9f);
    CHECK(hydrology::measure_water_cut_continuity(
              exact, exact, handoff, -2.5f, 1.0e-4f, metrics, error),
          error.message.c_str());
    CHECK(metrics.symmetric_hausdorff_m == 0.0f &&
              metrics.rms_distance_m == 0.0f &&
              metrics.unmatched_open_edges == 0u &&
              metrics.duplicate_coplanar_triangles == 0u &&
              hydrology::water_cut_is_assertion_weldable(
                  metrics, 0.15f / 16.0f),
          "an exact shared contour is assertion-weldable without geometry edits");

    auto discontinuous = exact;
    set_mesh_normal(discontinuous, {0.2f, 0.9797959f, 0.0f});
    CHECK(hydrology::measure_water_cut_continuity(
              exact, discontinuous, handoff, -2.5f, 1.0e-4f,
              metrics, error),
          error.message.c_str());
    CHECK(metrics.minimum_normal_dot < 0.995f &&
              metrics.p95_normal_angle_degrees > 0.0f &&
              !hydrology::water_cut_is_assertion_weldable(
                  metrics, 0.15f / 16.0f),
          "a normal discontinuity reports angle evidence and rejects welding");

    auto missing_segment = exact;
    missing_segment.indices.resize(3u);
    missing_segment.content_digest =
        gpu_meshing::mesh_content_digest(missing_segment);
    CHECK(hydrology::measure_water_cut_continuity(
              exact, missing_segment, handoff, -2.5f, 1.0e-4f,
              metrics, error),
          error.message.c_str());
    CHECK(metrics.unmatched_open_edges == 1u &&
              !hydrology::water_cut_is_assertion_weldable(
                  metrics, 0.15f / 16.0f),
          "one missing cut segment leaves unmatched combined boundary edges");

    auto duplicated = exact;
    append_duplicate_coplanar_triangle(duplicated, -2.5f);
    CHECK(hydrology::measure_water_cut_continuity(
              duplicated, exact, handoff, -2.5f, 1.0e-4f,
              metrics, error),
          error.message.c_str());
    CHECK(metrics.duplicate_coplanar_triangles != 0u &&
              !hydrology::water_cut_is_assertion_weldable(
                  metrics, 0.15f / 16.0f),
          "equal-position co-planar triangle triples reject duplicate ownership");
}

void test_cut_continuity_fails_closed_on_invalid_meshes() {
    const auto handoff = spillway();
    const auto exact = continuity_ribbon(44.9f);
    hydrology::WaterCutContourMetrics metrics{};
    hydrology::FluidBakeError error{};
    auto absent = exact;
    for (std::size_t index = 0u; index != absent.positions.size(); index += 3u)
        absent.positions[index] = 10.0f;
    absent.content_digest = gpu_meshing::mesh_content_digest(absent);
    CHECK(!hydrology::measure_water_cut_continuity(
              exact, absent, handoff, -2.5f, 1.0e-4f, metrics, error) &&
              error.code == hydrology::FluidBakeCode::ProductFailure,
          "a missing cut contour fails closed");

    auto non_finite = exact;
    non_finite.positions[1u] =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(!hydrology::measure_water_cut_continuity(
              exact, non_finite, handoff, -2.5f, 1.0e-4f,
              metrics, error) &&
              error.code == hydrology::FluidBakeCode::ProductFailure,
          "non-finite water geometry fails closed");
}

void test_cell_ownership_uses_one_exact_equality_convention() {
    auto handoff = shared_spillway();
    handoff.downstream_visual_cut_m = 0.5f;
    handoff.semantic_key = hydrology::spillway_handoff_semantic_key(handoff);
    CHECK(hydrology::classify_water_cell_ownership(
              handoff, {0.5f, 1.0f, 0.0f}) ==
              hydrology::WaterCellOwnership::Between &&
              hydrology::classify_water_cell_ownership(
                  handoff, {std::nextafter(0.5f, 1.0f), 1.0f, 0.0f}) ==
                  hydrology::WaterCellOwnership::After,
          "the downstream equality cell belongs to the strip and the next representable cell belongs downstream");

    gpu_meshing::ParticleSamplingLattice lattice{};
    gpu_meshing::Error lattice_error{};
    CHECK(gpu_meshing::make_particle_sampling_lattice(
              {0.125f, 0.0f, 0.0f}, 0.25f, lattice, lattice_error),
          lattice_error.message.c_str());
    const auto strip = quad(-0.625f, 0.625f, 1.0f, -0.5f, 0.5f);
    const auto downstream = quad(0.625f, 1.625f, 1.0f, -0.5f, 0.5f);
    hydrology::WaterCutContourMetrics metrics{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              strip, downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              lattice.voxel_m / 16.0f, metrics, error),
          error.message.c_str());
    CHECK(metrics.symmetric_hausdorff_m == 0.0f &&
              metrics.minimum_normal_dot == 1.0f &&
              metrics.unmatched_open_edges == 0u &&
              metrics.duplicate_coplanar_triangles == 0u,
          "cell-boundary measurement uses the same downstream equality predicate as ownership clipping");
    CHECK(!hydrology::measure_water_cell_boundary_continuity(
              strip, downstream,
              {lattice, handoff, 0.0f},
              lattice.voxel_m / 16.0f, metrics, error) &&
              error.code == hydrology::FluidBakeCode::ProductFailure,
          "cell-boundary measurement rejects a cut that is not an exact ownership boundary");
}

void test_stitches_independently_meshed_cut_contours() {
    auto upstream = section_artifact(true);
    auto downstream = section_artifact(false);
    upstream.visual_mesh = quad(-10.0f, 0.0f, 1.0f, -4.25f, 4.25f);
    downstream.visual_mesh = quad(0.0f, 10.0f, 1.0f, -4.50f, 4.50f);
    const auto handoff = spillway();
    const hydrology::PhysxFluidBake::VisualMesher mismatched_mesher =
        [](const gpu_meshing::ParticleJob& job,
           gpu_meshing::MeshResult& mesh,
           gpu_meshing::Stats& stats,
           gpu_meshing::Error& error,
           const gpu_meshing::BuildControl&) {
            if (job.particle_count == 0u) return false;
            mesh = quad(-5.0f, 5.0f, 1.15f, -5.0f, 5.0f);
            stats.particles = job.particle_count;
            stats.triangles = 2u;
            error = {};
            return true;
        };
    hydrology::HydrologyHandoffArtifact artifact{};
    hydrology::HydrologyNetworkProducts products{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::build_handoff_artifact(
              upstream, downstream, handoff, settings(),
              mismatched_mesher, artifact, products, error),
          error.message.c_str());
    CHECK(hydrology::validate_handoff_products(
              artifact, products, handoff, error),
          "independent water meshes receive watertight ownership stitches");
}

void test_keeps_water_in_the_removed_dam_footprint() {
    auto upstream = section_artifact(true);
    auto downstream = section_artifact(false);
    upstream.particles = {{{-1.0f, 1.0f, 0.0f}, {}, 1u}};
    downstream.particles = {{{5.0f, 1.0f, 0.0f}, {}, 2u}};
    const hydrology::PhysxFluidBake::VisualMesher counting_mesher =
        [](const gpu_meshing::ParticleJob& job,
           gpu_meshing::MeshResult& mesh,
           gpu_meshing::Stats& stats,
           gpu_meshing::Error& error,
           const gpu_meshing::BuildControl&) {
            if (job.particle_count != 2u) {
                error = {gpu_meshing::ErrorCode::InvalidInput,
                         "removed-dam water was omitted from the collar"};
                return false;
            }
            mesh = quad(-5.0f, 5.0f, 1.0f);
            stats.particles = job.particle_count;
            stats.triangles = 2u;
            error = {};
            return true;
        };
    hydrology::HydrologyHandoffArtifact artifact{};
    hydrology::HydrologyNetworkProducts products{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::build_handoff_artifact(
              upstream, downstream, spillway(), settings(),
              counting_mesher, artifact, products, error),
          "water occupying the removed dam volume contributes to the final collar");
}

void test_stitches_split_section_contours_to_one_smoothed_collar() {
    auto upstream = section_artifact(true);
    auto downstream = section_artifact(false);
    downstream.visual_mesh = combine(
        quad(0.0f, 10.0f, 1.0f, -4.5f, 2.5f),
        quad(0.0f, 10.0f, 1.15f, 1.0f, 4.5f));
    const hydrology::PhysxFluidBake::VisualMesher merged_mesher =
        [](const gpu_meshing::ParticleJob& job,
           gpu_meshing::MeshResult& mesh,
           gpu_meshing::Stats& stats,
           gpu_meshing::Error& error,
           const gpu_meshing::BuildControl&) {
            mesh = quad(-5.0f, 5.0f, 1.05f, -5.0f, 5.0f);
            stats.particles = job.particle_count;
            stats.triangles = 2u;
            error = {};
            return true;
        };
    hydrology::HydrologyHandoffArtifact artifact{};
    hydrology::HydrologyNetworkProducts products{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::build_handoff_artifact(
              upstream, downstream, spillway(), settings(), merged_mesher,
              artifact, products, error),
          "split section contours merge into the smoothed collar boundary");
}

void test_builds_deterministic_seam_without_dam_curtain() {
    const auto upstream = section_artifact(true);
    const auto downstream = section_artifact(false);
    const auto handoff = spillway();
    hydrology::HydrologyHandoffArtifact artifact{};
    hydrology::HydrologyNetworkProducts products{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::build_handoff_artifact(
              upstream, downstream, handoff, settings(),
              synthetic_visual_mesher, artifact, products, error),
          error.message.c_str());
    CHECK(hydrology::validate_handoff_products(
              artifact, products, handoff, error),
          error.message.c_str());
    bool contains_dam_curtain = false;
    for (std::size_t index = 0; index + 2u < products.visual_mesh.positions.size();
         index += 3u) {
        const float x = products.visual_mesh.positions[index];
        const float y = products.visual_mesh.positions[index + 1u];
        if (x >= 4.75f && x <= 5.25f && y >= 0.5f)
            contains_dam_curtain = true;
    }
    CHECK(!contains_dam_curtain,
          "aggregate visual product excludes the temporary-dam curtain");
    CHECK(!products.visual_mesh.indices.empty() &&
              !products.coarse_cpu_mesh.indices.empty() &&
              products.visual_mesh.content_digest != 0u &&
              products.coarse_cpu_mesh.content_digest != 0u,
          "the seam publishes nonempty visual and query meshes");

    hydrology::GameplaySample slow{}, lip{}, downstream_owned{}, dry{};
    CHECK(hydrology::sample_fluid_gameplay_field(
              products.gameplay_layout, products.gameplay_field,
              -4.5f, -0.5f, slow) &&
              hydrology::sample_fluid_gameplay_field(
                  products.gameplay_layout, products.gameplay_field,
                  0.5f, -0.5f, lip) &&
              hydrology::sample_fluid_gameplay_field(
                  products.gameplay_layout, products.gameplay_field,
                  4.5f, -0.5f, downstream_owned),
          "aggregate gameplay samples span both section owners");
    CHECK(slow.velocity_x_mps < lip.velocity_x_mps &&
              lip.velocity_x_mps < downstream_owned.velocity_x_mps,
          "gameplay flow accelerates smoothly through the spillway collar");
    CHECK(!hydrology::sample_fluid_gameplay_field(
              products.gameplay_layout, products.gameplay_field,
              8.5f, -0.5f, dry),
          "dry source samples remain invalid after aggregation");
    CHECK(!hydrology::sample_fluid_gameplay_field(
              products.gameplay_layout, products.gameplay_field,
              8.0f, -0.5f, dry),
          "a dry ownership edge rejects rather than partially blending water");

    hydrology::PresentationSample slow_surface{}, lip_surface{},
        downstream_surface{}, dry_surface{};
    CHECK(hydrology::sample_river_presentation_field(
              products.gameplay_layout, products.presentation_field,
              -4.5f, -0.5f, slow_surface) &&
              hydrology::sample_river_presentation_field(
                  products.gameplay_layout, products.presentation_field,
                  0.5f, -0.5f, lip_surface) &&
              hydrology::sample_river_presentation_field(
                  products.gameplay_layout, products.presentation_field,
                  4.5f, -0.5f, downstream_surface),
          "aggregate presentation samples span both section owners");
    const auto normal_y = [](const hydrology::PresentationSample& sample) {
        return std::sqrt(1.0f - sample.normal_x * sample.normal_x -
                         sample.normal_z * sample.normal_z);
    };
    CHECK(slow.height_m < lip.height_m &&
              lip.height_m < downstream_owned.height_m &&
              slow_surface.turbulence < lip_surface.turbulence &&
              lip_surface.turbulence < downstream_surface.turbulence &&
              slow_surface.foam_potential < lip_surface.foam_potential &&
              lip_surface.foam_potential < downstream_surface.foam_potential &&
              std::isfinite(normal_y(slow_surface)) &&
              std::isfinite(normal_y(lip_surface)) &&
              std::isfinite(normal_y(downstream_surface)) &&
              normal_y(lip_surface) > 0.0f,
          "ownership weights bound height, normal, turbulence, and foam across the handoff");
    CHECK(lip_surface.feature == hydrology::RiverFeature::Spillway,
          "feature classification follows explicit nearest ownership");
    CHECK(!hydrology::sample_river_presentation_field(
              products.gameplay_layout, products.presentation_field,
              8.0f, -0.5f, dry_surface),
          "presentation never wets a dry ownership edge");

    std::vector<std::uint8_t> bytes;
    gpu_meshing::Error artifact_error{};
    hydrology::HydrologyHandoffArtifact reopened{};
    CHECK(hydrology::serialize_handoff_artifact(
              artifact, bytes, artifact_error) &&
              hydrology::deserialize_handoff_artifact(
                  bytes, reopened, artifact_error) &&
              reopened.id == artifact.id &&
              reopened.semantic_key == artifact.semantic_key &&
              reopened.payload_digest == artifact.payload_digest &&
              reopened.visual_mesh.positions == artifact.visual_mesh.positions,
          "the independently cached handoff artifact round-trips exactly");

    hydrology::HydrologyHandoffArtifact repeat_artifact{};
    hydrology::HydrologyNetworkProducts repeat_products{};
    CHECK(hydrology::build_handoff_artifact(
              upstream, downstream, handoff, settings(),
              synthetic_visual_mesher, repeat_artifact, repeat_products, error) &&
              repeat_artifact.payload_digest == artifact.payload_digest &&
              repeat_products.visual_mesh.content_digest ==
                  products.visual_mesh.content_digest &&
              repeat_products.gameplay_field == products.gameplay_field &&
              repeat_products.presentation_field ==
                  products.presentation_field,
          "handoff aggregation is deterministic across repeated builds");
}

void test_builds_thirty_shared_field_frames_without_static_geometry() {
    auto fixture = shared_field_fixture();
    bind_shared_field_fixture(fixture);
    struct ObservedJob {
        std::vector<gpu_meshing::ParticleSample> particles;
        gpu_meshing::ParticlePhaseBlend phase{};
    };
    std::vector<ObservedJob> observed;
    const hydrology::PhysxFluidBake::VisualMesher observing_mesher =
        [&](const gpu_meshing::ParticleJob& job,
            gpu_meshing::MeshResult& mesh, gpu_meshing::Stats& stats,
            gpu_meshing::Error& error,
            const gpu_meshing::BuildControl& control) {
            observed.push_back({
                std::vector<gpu_meshing::ParticleSample>(
                    job.particles, job.particles + job.particle_count),
                job.phase_blend});
            return canonical_plane_mesher(job, mesh, stats, error, control);
        };
    hydrology::WaterMeshAnimationArtifact artifact{};
    hydrology::HandoffAnimationBuildDiagnostics diagnostics{};
    hydrology::FluidBakeError error{};
    const bool built = hydrology::build_handoff_water_animation_artifact(
        fixture.input, observing_mesher, artifact, diagnostics, error);
    CHECK(built, error.message.c_str());
    if (!built) return;
    CHECK(artifact.identity == "pool-one" && artifact.frames.size() == 30u &&
              artifact.frames_per_second == 30u &&
              artifact.phase_offset_frames == 15u &&
              artifact.source_primary_payload_digest == 1001u &&
              artifact.source_secondary_payload_digest == 2002u,
          "the shared-field strip preserves the one-second synchronized animation contract");
    CHECK(observed.size() == 30u &&
              observed[0].phase.primary_weight == 0.0f &&
              observed[0].phase.secondary_weight == 1.0f &&
              observed[0].phase.split_index > 0u &&
              observed[0].phase.split_index < observed[0].particles.size(),
          "frame zero jointly uses upstream/downstream captures zero and fifteen with the existing cosine phase weights");
    bool has_upstream_interior = false;
    bool has_downstream_interior = false;
    bool has_upstream_opposite_crossing = false;
    bool has_downstream_opposite_crossing = false;
    bool has_upstream_dam = false;
    for (const auto& particle : observed.front().particles) {
        has_upstream_interior = has_upstream_interior ||
            std::fabs(particle.position_m.y - 1.10f) < 0.02f ||
            std::fabs(particle.position_m.y - 1.115f) < 0.02f;
        has_downstream_interior = has_downstream_interior ||
            std::fabs(particle.position_m.y - 1.20f) < 0.02f ||
            std::fabs(particle.position_m.y - 1.215f) < 0.02f;
        has_upstream_opposite_crossing =
            has_upstream_opposite_crossing ||
            (particle.position_m.x > 0.40f && particle.position_m.x < 0.50f);
        has_downstream_opposite_crossing =
            has_downstream_opposite_crossing ||
            (particle.position_m.x < -0.40f && particle.position_m.x > -0.50f);
        has_upstream_dam = has_upstream_dam ||
            (particle.position_m.x > 0.75f &&
             particle.position_m.x < 1.00f);
    }
    CHECK(has_upstream_interior && has_downstream_interior &&
              !has_upstream_opposite_crossing &&
              !has_downstream_opposite_crossing && !has_upstream_dam,
          "both interior sources survive while opposite-cut support and upstream dam contributors are absent");
    CHECK(diagnostics.peak_decoded_boundary_frames == 4u,
          "one output frame retains only its four cropped phase-source decodes");

    gpu_meshing::MeshResult frame{};
    gpu_meshing::Error artifact_error{};
    CHECK(hydrology::decode_water_mesh_animation_frame(
              artifact, 0u, frame, artifact_error),
          artifact_error.message.c_str());
    const auto legacy_static_collar = quad(-5.0f, 5.0f, 1.0f);
    bool contains_legacy_static_vertex = false;
    for (std::size_t vertex = 0u; vertex != frame.positions.size();
         vertex += 3u) {
        contains_legacy_static_vertex = contains_legacy_static_vertex ||
            std::fabs(std::fabs(frame.positions[vertex]) - 5.0f) < 1.0e-4f ||
            std::fabs(std::fabs(frame.positions[vertex + 2u]) - 5.0f) <
                1.0e-4f;
    }
    CHECK(frame.content_digest != legacy_static_collar.content_digest &&
              !contains_legacy_static_vertex,
          "no static collar vertex, digest, or legacy zipper geometry enters an animated frame");

    const auto semantic = hydrology::derive_handoff_animation_semantic_key(
        fixture.input, fixture.upstream_bulk.payload_digest,
        fixture.downstream_bulk.payload_digest);
    CHECK(semantic == artifact.semantic_key &&
              semantic == hydrology::derive_handoff_animation_semantic_key(
                  fixture.input, fixture.upstream_bulk.payload_digest,
                  fixture.downstream_bulk.payload_digest),
          "identical handoff inputs derive one stable cache identity");
    auto changed_source = fixture.upstream_source;
    changed_source.payload_digest++;
    auto changed_input = fixture.input;
    changed_input.upstream = &changed_source;
    CHECK(hydrology::derive_handoff_animation_semantic_key(
              changed_input, fixture.upstream_bulk.payload_digest,
              fixture.downstream_bulk.payload_digest) != semantic,
          "the upstream boundary payload participates in handoff cache identity");
    auto changed_downstream_source = fixture.downstream_source;
    changed_downstream_source.payload_digest++;
    changed_input = fixture.input;
    changed_input.downstream = &changed_downstream_source;
    CHECK(hydrology::derive_handoff_animation_semantic_key(
              changed_input, fixture.upstream_bulk.payload_digest,
              fixture.downstream_bulk.payload_digest) != semantic,
          "the downstream boundary payload participates in handoff cache identity");
    CHECK(hydrology::derive_handoff_animation_semantic_key(
              fixture.input, fixture.upstream_bulk.payload_digest + 1u,
              fixture.downstream_bulk.payload_digest) != semantic &&
              hydrology::derive_handoff_animation_semantic_key(
              fixture.input, fixture.upstream_bulk.payload_digest,
              fixture.downstream_bulk.payload_digest + 1u) != semantic,
          "both owned-bulk payloads participate in handoff cache identity");
    auto changed_handoff = fixture.input;
    changed_handoff.handoff.overlap_m += 0.25f;
    changed_handoff.handoff.semantic_key =
        hydrology::spillway_handoff_semantic_key(changed_handoff.handoff);
    CHECK(hydrology::derive_handoff_animation_semantic_key(
              changed_handoff, fixture.upstream_bulk.payload_digest,
              fixture.downstream_bulk.payload_digest) != semantic,
          "the complete handoff record participates in handoff cache identity");
    auto changed_lattice = fixture.input;
    changed_lattice.lattice.origin_m.x +=
        changed_lattice.lattice.voxel_m;
    CHECK(hydrology::derive_handoff_animation_semantic_key(
              changed_lattice, fixture.upstream_bulk.payload_digest,
              fixture.downstream_bulk.payload_digest) != semantic,
          "the canonical lattice participates in handoff cache identity");
    auto changed_radius_source = fixture.upstream_source;
    changed_radius_source.particle_radius_m += 0.01f;
    changed_input = fixture.input;
    changed_input.upstream = &changed_radius_source;
    CHECK(hydrology::derive_handoff_animation_semantic_key(
              changed_input, fixture.upstream_bulk.payload_digest,
              fixture.downstream_bulk.payload_digest) != semantic,
          "the particle radius participates in handoff cache identity");
    auto changed_visual = fixture.input;
    changed_visual.visual_template.blend_width_m += 0.01f;
    CHECK(hydrology::derive_handoff_animation_semantic_key(
              changed_visual, fixture.upstream_bulk.payload_digest,
              fixture.downstream_bulk.payload_digest) != semantic,
          "the visual blend profile participates in handoff cache identity");
    changed_visual = fixture.input;
    changed_visual.visual_template.iso_value = 0.125f;
    CHECK(hydrology::derive_handoff_animation_semantic_key(
              changed_visual, fixture.upstream_bulk.payload_digest,
              fixture.downstream_bulk.payload_digest) != semantic,
          "the complete visual field contract participates in handoff cache identity");
    auto changed_phase_source = fixture.upstream_source;
    changed_phase_source.phase_offset_frames = 14u;
    changed_input = fixture.input;
    changed_input.upstream = &changed_phase_source;
    CHECK(hydrology::derive_handoff_animation_semantic_key(
              changed_input, fixture.upstream_bulk.payload_digest,
              fixture.downstream_bulk.payload_digest) != semantic,
          "the temporal phase profile participates in handoff cache identity");
}

void test_shared_cell_ownership_passes_every_frame_boundary() {
    auto fixture = shared_field_fixture();
    bind_shared_field_fixture(fixture);
    hydrology::WaterMeshAnimation animation{};
    hydrology::HandoffAnimationBuildDiagnostics diagnostics{};
    hydrology::FluidBakeError error{};
    const bool built = hydrology::build_handoff_animation_frames(
        fixture.input, canonical_plane_mesher, animation,
        diagnostics, error);
    CHECK(built, error.message.c_str());
    if (!built) return;
    CHECK(animation.frames.size() == 30u,
          "joint construction produces all thirty replacement frames");
    float worst_hausdorff = 0.0f;
    float worst_normal_dot = 1.0f;
    std::uint32_t worst_open_edges = 0u;
    std::uint32_t worst_duplicate_triangles = 0u;
    for (std::size_t frame = 0u; frame != animation.frames.size(); ++frame) {
        const auto& mesh = animation.frames[frame];
        const auto& upstream_range = diagnostics.upstream_band[frame];
        const auto& collar_range = diagnostics.collar[frame];
        const auto& downstream_range = diagnostics.downstream_band[frame];
        CHECK(upstream_range.first_index == 0u &&
                  collar_range.first_index == upstream_range.index_count &&
                  downstream_range.first_index ==
                      collar_range.first_index + collar_range.index_count &&
                  downstream_range.first_index +
                      downstream_range.index_count == mesh.indices.size(),
              "upstream/collar/downstream diagnostics cover every replacement index exactly once");
        for (const auto* metrics : {
                 &diagnostics.upstream_cut[frame],
                 &diagnostics.downstream_cut[frame]}) {
            worst_hausdorff = std::max(
                worst_hausdorff, metrics->symmetric_hausdorff_m);
            worst_normal_dot = std::min(
                worst_normal_dot, metrics->minimum_normal_dot);
            worst_open_edges = std::max(
                worst_open_edges, metrics->unmatched_open_edges);
            worst_duplicate_triangles = std::max(
                worst_duplicate_triangles,
                metrics->duplicate_coplanar_triangles);
            CHECK(metrics->symmetric_hausdorff_m <=
                      fixture.input.lattice.voxel_m / 16.0f &&
                      metrics->minimum_normal_dot >= 0.995f &&
                      metrics->unmatched_open_edges == 0u &&
                      metrics->duplicate_coplanar_triangles == 0u,
                  "every frame, including the loop boundary, has an exact shared-cell contour");
        }
    }
    std::ostringstream worst_metrics;
    worst_metrics << "worst continuity metrics: hausdorff="
                  << worst_hausdorff << ", normal_dot="
                  << worst_normal_dot << ", open_edges="
                  << worst_open_edges << ", duplicates="
                  << worst_duplicate_triangles;
    CHECK(worst_hausdorff <= fixture.input.lattice.voxel_m / 16.0f &&
              worst_normal_dot >= 0.995f && worst_open_edges == 0u &&
              worst_duplicate_triangles == 0u,
          worst_metrics.str().c_str());
}

void test_joint_builder_reapplies_exact_dam_support_exclusion() {
    auto fixture = shared_field_fixture();
    fixture.input.handoff.temporary_dam_exclusion_bounds_m =
        {{-0.02f, 1.08f, -0.02f}, {0.02f, 1.12f, 0.02f}};
    fixture.input.handoff.semantic_key =
        hydrology::spillway_handoff_semantic_key(fixture.input.handoff);
    fixture.upstream_source.handoff_semantic_key =
        fixture.input.handoff.semantic_key;
    fixture.downstream_source.handoff_semantic_key =
        fixture.input.handoff.semantic_key;
    bind_shared_field_fixture(fixture);
    bool saw_excluded_upstream_support = false;
    bool saw_downstream_interior = false;
    const hydrology::PhysxFluidBake::VisualMesher observing_mesher =
        [&](const gpu_meshing::ParticleJob& job,
            gpu_meshing::MeshResult& mesh, gpu_meshing::Stats& stats,
            gpu_meshing::Error& mesh_error,
            const gpu_meshing::BuildControl& control) {
            for (std::uint32_t index = 0u; index != job.particle_count;
                 ++index) {
                const auto& position = job.particles[index].position_m;
                saw_excluded_upstream_support =
                    saw_excluded_upstream_support ||
                    (std::fabs(position.x) < 0.03f &&
                     position.y >= 1.09f && position.y <= 1.13f);
                saw_downstream_interior = saw_downstream_interior ||
                    (std::fabs(position.x) < 0.03f &&
                     position.y >= 1.19f && position.y <= 1.23f);
            }
            return canonical_plane_mesher(
                job, mesh, stats, mesh_error, control);
        };
    hydrology::WaterMeshAnimation animation{};
    hydrology::HandoffAnimationBuildDiagnostics diagnostics{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::build_handoff_animation_frames(
              fixture.input, observing_mesher, animation,
              diagnostics, error), error.message.c_str());
    CHECK(!saw_excluded_upstream_support && saw_downstream_interior,
          "the joint builder independently removes upstream field support touching the dam while retaining downstream water");
}

void test_shared_strip_chunking_is_geometry_equivalent() {
    auto unchunked_fixture = shared_field_fixture();
    bind_shared_field_fixture(unchunked_fixture);
    hydrology::WaterMeshAnimation unchunked{};
    hydrology::HandoffAnimationBuildDiagnostics unchunked_diagnostics{};
    hydrology::FluidBakeError error{};
    const bool unchunked_built = hydrology::build_handoff_animation_frames(
        unchunked_fixture.input, canonical_plane_mesher, unchunked,
        unchunked_diagnostics, error);
    CHECK(unchunked_built, error.message.c_str());
    if (!unchunked_built) return;

    auto chunked_fixture = shared_field_fixture();
    bind_shared_field_fixture(chunked_fixture);
    chunked_fixture.input.visual_template.limits.max_grid_vertices = 384u;
    hydrology::WaterMeshAnimation chunked{};
    hydrology::HandoffAnimationBuildDiagnostics chunked_diagnostics{};
    std::uint32_t chunk_mesher_calls = 0u;
    const hydrology::PhysxFluidBake::VisualMesher chunking_mesher =
        [&](const gpu_meshing::ParticleJob& job,
            gpu_meshing::MeshResult& mesh, gpu_meshing::Stats& stats,
            gpu_meshing::Error& mesh_error,
            const gpu_meshing::BuildControl& control) {
            ++chunk_mesher_calls;
            return canonical_plane_mesher(
                job, mesh, stats, mesh_error, control);
        };
    const bool chunked_built = hydrology::build_handoff_animation_frames(
        chunked_fixture.input, chunking_mesher, chunked,
        chunked_diagnostics, error);
    CHECK(chunked_built, error.message.c_str());
    if (!chunked_built) return;
    CHECK(chunked.frames.size() == unchunked.frames.size() &&
              chunk_mesher_calls > chunked.frames.size(),
          "a sub-root grid cap really chunks while retaining every handoff frame");
    for (std::size_t frame = 0u; frame != chunked.frames.size(); ++frame) {
        CHECK(canonical_triangles(chunked.frames[frame], 1.0e-4f) ==
                  canonical_triangles(unchunked.frames[frame], 1.0e-4f),
              "chunked and unchunked shared-field strips have identical canonical topology");
    }
}

void test_formats_animation_acceptance_timing_trace() {
    hydrology::HydrologyNetworkBakeResult result{};
    hydrology::HydrologyArtifact section = section_artifact(true);
    section.stats.simulated_steps = 240u;
    section.stats.active_particles = 321u;
    section.stats.escaped_particles = 2u;
    result.sections.push_back(std::move(section));

    hydrology::HydrologySectionTimings timing{};
    timing.id = "upper";
    timing.simulate_ms = 12.5;
    timing.animation_capture_ms = 1.25;
    timing.animation_mesh_ms = 7.75;
    timing.animation_serialize_ms = 0.5;
    timing.animation_bytes = 123456u;
    timing.animation_device_capture_bytes = 654321u;
    timing.animation_semantic_key = 0x1234u;
    timing.animation_payload_digest = 0x5678u;
    timing.animation_capture_first_step = 124u;
    timing.animation_capture_last_step = 240u;
    timing.animation_capture_particle_counts = {300u, 310u};
    timing.animation_frame_vertex_counts = {100u, 110u};
    timing.animation_frame_triangle_counts = {50u, 55u};
    result.timings.sections.push_back(std::move(timing));
    result.timings.handoff_animation_mesh_ms = 2.5;
    hydrology::HydrologyHandoffTimings handoff_timing{};
    handoff_timing.id = "pool-one";
    handoff_timing.static_cache_hit = true;
    handoff_timing.animation_cache_hit = false;
    handoff_timing.animation_file_bytes = 987654321u;
    handoff_timing.boundary_source_bytes = 13579u;
    handoff_timing.peak_build_cpu_payload_bytes = 24680u;
    handoff_timing.source_blend_required = true;
    for (std::size_t frame = 0u;
         frame != handoff_timing.animation_frame_ms.size(); ++frame) {
        handoff_timing.animation_frame_ms[frame] =
            static_cast<double>(frame) * 0.125;
        handoff_timing.upstream_cut[frame].symmetric_hausdorff_m =
            3.8f + static_cast<float>(frame) * 0.01f;
        handoff_timing.downstream_cut[frame].symmetric_hausdorff_m =
            2.7f + static_cast<float>(frame) * 0.01f;
    }
    result.timings.handoffs.push_back(std::move(handoff_timing));
    result.timings.peak_build_cpu_payload_bytes = 1122334455u;

    const std::string json =
        hydrology::hydrology_network_timing_trace_json(result);
    CHECK(json.find("\"animationCaptureMs\":1.250") != std::string::npos &&
              json.find("\"animationMeshMs\":7.750") != std::string::npos &&
              json.find("\"animationSerializeMs\":0.500") != std::string::npos &&
              json.find("\"animationBytes\":123456") != std::string::npos &&
              json.find("\"animationDeviceCaptureBytes\":654321") != std::string::npos,
          "timing trace exposes animation time and memory lanes");
    CHECK(json.find("\"animationCaptureFirstStep\":124") != std::string::npos &&
              json.find("\"animationCaptureLastStep\":240") != std::string::npos &&
              json.find("\"animationCaptureParticleCounts\":[300,310]") != std::string::npos &&
              json.find("\"animationFrameVertexCounts\":[100,110]") != std::string::npos &&
              json.find("\"animationFrameTriangleCounts\":[50,55]") != std::string::npos,
          "timing trace exposes the retained capture and every mesh frame");
    CHECK(json.find("\"animationSemanticKey\":\"0000000000001234\"") !=
                  std::string::npos &&
              json.find("\"animationPayloadDigest\":\"0000000000005678\"") !=
                  std::string::npos &&
              json.find("\"handoffAnimationMeshMs\":2.500") !=
                  std::string::npos,
          "timing trace exposes immutable animation identity and handoff cost");
    CHECK(json.find("\"handoffs\": {\n    \"pool-one\":{") !=
                  std::string::npos &&
              json.find("\"staticCacheHit\":true") != std::string::npos &&
              json.find("\"animationCacheHit\":false") != std::string::npos &&
              json.find("\"animationFrameMs\":[0.000,0.125") !=
                  std::string::npos &&
              json.find("3.625]") != std::string::npos,
          "timing trace keys one handoff and emits exactly thirty finite frame timings");
    CHECK(json.find("\"animationFileBytes\":987654321") !=
                  std::string::npos &&
              json.find("\"boundarySourceBytes\":13579") !=
                  std::string::npos &&
              json.find("\"peakBuildCpuPayloadBytes\":24680") !=
                  std::string::npos &&
              json.find("\"networkPeakBuildCpuPayloadBytes\":1122334455") !=
                  std::string::npos,
          "timing trace keeps complete-file, transient boundary, handoff peak, and network peak bytes distinct");
    CHECK(json.find("\"sourceBlendRequired\":true") != std::string::npos &&
              json.find("\"upstreamCut\":[{") != std::string::npos &&
              json.find("\"downstreamCut\":[{") != std::string::npos,
          "timing trace retains failed Stage 0 cut and source-blend evidence");
}

}  // namespace

int main() {
    test_measures_section_cut_continuity_without_welding();
    test_cut_continuity_fails_closed_on_invalid_meshes();
    test_cell_ownership_uses_one_exact_equality_convention();
    test_builds_deterministic_seam_without_dam_curtain();
    test_stitches_independently_meshed_cut_contours();
    test_keeps_water_in_the_removed_dam_footprint();
    test_stitches_split_section_contours_to_one_smoothed_collar();
    test_builds_thirty_shared_field_frames_without_static_geometry();
    test_shared_cell_ownership_passes_every_frame_boundary();
    test_joint_builder_reapplies_exact_dam_support_exclusion();
    test_shared_strip_chunking_is_geometry_equivalent();
    test_formats_animation_acceptance_timing_trace();
    return check_summary();
}
