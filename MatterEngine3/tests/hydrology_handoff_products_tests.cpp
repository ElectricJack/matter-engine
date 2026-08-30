#include "check.h"
#include "hydrology/hydrology_handoff_products.h"
#include "hydrology/river_presentation_field.h"
#include "hydrology/water_mesh_continuity.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <set>
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

bool same_quantized_mesh(const gpu_meshing::MeshResult& actual,
                         const gpu_meshing::MeshResult& expected,
                         float position_tolerance) {
    if (actual.positions.size() != expected.positions.size() ||
        actual.normals.size() != expected.normals.size() ||
        actual.indices != expected.indices)
        return false;
    for (std::size_t index = 0u; index != actual.positions.size(); ++index)
        if (std::fabs(actual.positions[index] - expected.positions[index]) >
            position_tolerance)
            return false;
    for (std::size_t index = 0u; index != actual.normals.size(); index += 3u) {
        const float dot = actual.normals[index + 0u] *
                              expected.normals[index + 0u] +
                          actual.normals[index + 1u] *
                              expected.normals[index + 1u] +
                          actual.normals[index + 2u] *
                              expected.normals[index + 2u];
        if (dot < 0.999f) return false;
    }
    return true;
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

bool sampled_field_coverage_mesher(
    const gpu_meshing::ParticleJob& job, gpu_meshing::MeshResult& mesh,
    gpu_meshing::Stats& stats, gpu_meshing::Error& error) {
    mesh = {};
    gpu_meshing::GridLayout layout{};
    if (!gpu_meshing::validate_particle_job(job, layout, error)) return false;
    constexpr float surface_y_m = 1.1f;
    const auto field_at = [&](matter::Float3 point) {
        if (job.longitudinal_field_blend.enabled) {
            return gpu_meshing::evaluate_particle_field_reference(
                job.particles, job.particle_count, job.blend_width_m,
                job.phase_blend, job.longitudinal_field_blend, point);
        }
        return gpu_meshing::evaluate_particle_field_reference(
            job.particles, job.particle_count, job.blend_width_m,
            job.phase_blend, point);
    };
    for (std::uint32_t z = 0u; z != layout.cell_dims[2]; ++z) {
        for (std::uint32_t x = 0u; x != layout.cell_dims[0]; ++x) {
            const float x0 = layout.origin_m.x +
                static_cast<float>(x) * layout.spacing_m.x;
            const float x1 = x0 + layout.spacing_m.x;
            const float z0 = layout.origin_m.z +
                static_cast<float>(z) * layout.spacing_m.z;
            const float z1 = z0 + layout.spacing_m.z;
            const float field = field_at(
                {(x0 + x1) * 0.5f, surface_y_m, (z0 + z1) * 0.5f});
            if (!std::isfinite(field) || field > job.iso_value) continue;
            const std::uint32_t base = static_cast<std::uint32_t>(
                mesh.positions.size() / 3u);
            mesh.positions.insert(mesh.positions.end(), {
                x0, surface_y_m, z0, x1, surface_y_m, z0,
                x1, surface_y_m, z1, x0, surface_y_m, z1});
            mesh.normals.insert(mesh.normals.end(), {
                0,1,0, 0,1,0, 0,1,0, 0,1,0});
            mesh.indices.insert(mesh.indices.end(), {
                base, base + 1u, base + 2u,
                base, base + 2u, base + 3u});
        }
    }
    mesh.material = job.material;
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
    stats = {};
    stats.particles = job.particle_count;
    stats.triangles = static_cast<std::uint32_t>(mesh.indices.size() / 3u);
    error = {};
    return true;
}

std::set<std::array<std::int64_t, 3>> covered_cells_near_cut(
    const std::vector<const gpu_meshing::MeshResult*>& meshes,
    const gpu_meshing::ParticleSamplingLattice& lattice,
    float cut_m) {
    std::set<std::array<std::int64_t, 3>> cells;
    for (const gpu_meshing::MeshResult* mesh : meshes) {
        for (std::size_t offset = 0u; offset != mesh->indices.size();
             offset += 3u) {
            matter::Float3 centroid{};
            for (std::size_t corner = 0u; corner != 3u; ++corner) {
                const std::uint32_t vertex = mesh->indices[offset + corner];
                centroid.x += mesh->positions[vertex * 3u + 0u] / 3.0f;
                centroid.y += mesh->positions[vertex * 3u + 1u] / 3.0f;
                centroid.z += mesh->positions[vertex * 3u + 2u] / 3.0f;
            }
            if (std::fabs(centroid.x - cut_m) > lattice.voxel_m) continue;
            cells.insert({
                static_cast<std::int64_t>(std::floor(
                    (centroid.x - lattice.origin_m.x) / lattice.voxel_m)),
                static_cast<std::int64_t>(std::floor(
                    (centroid.y - lattice.origin_m.y) / lattice.voxel_m)),
                static_cast<std::int64_t>(std::floor(
                    (centroid.z - lattice.origin_m.z) / lattice.voxel_m))});
        }
    }
    return cells;
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

void append_quantized_cell_face_sliver(gpu_meshing::MeshResult& mesh,
                                       float face_x, float tolerance_m,
                                       float base_y = 2.0f) {
    const auto base = static_cast<std::uint32_t>(mesh.positions.size() / 3u);
    mesh.positions.insert(mesh.positions.end(), {
        face_x, base_y, 0.0f,
        face_x, base_y + tolerance_m * 0.1f, 0.0f,
        face_x, base_y + 1.0f, 0.0f});
    mesh.normals.insert(mesh.normals.end(), {
        1.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1u, base + 2u});
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
}

void append_sub_quantization_cell_face_triangle(
    gpu_meshing::MeshResult& mesh, float face_x, float tolerance_m,
    float base_y = 8.0f) {
    const auto base = static_cast<std::uint32_t>(mesh.positions.size() / 3u);
    mesh.positions.insert(mesh.positions.end(), {
        face_x, base_y, 0.0f,
        face_x, base_y + tolerance_m * 0.1f, 0.0f,
        face_x, base_y, tolerance_m * 0.1f});
    mesh.normals.insert(mesh.normals.end(), {
        1.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1u, base + 2u});
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
}

gpu_meshing::MeshResult cell_face_segment(float face_x, bool before_face,
                                           float y0, float z0,
                                           float y1, float z1) {
    gpu_meshing::MeshResult mesh{};
    const float interior_x = face_x + (before_face ? -0.05f : 0.05f);
    mesh.positions = {
        face_x, y0, z0,
        face_x, y1, z1,
        interior_x, (y0 + y1) * 0.5f + 0.05f,
        (z0 + z1) * 0.5f + 0.05f};
    mesh.normals = {
        0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f};
    mesh.indices = {0u, 1u, 2u};
    mesh.material = 4u;
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
    return mesh;
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
    auto tolerance_straddled = downstream;
    const float endpoint_tolerance_m = lattice.voxel_m / 16.0f;
    for (std::size_t index = 1u; index < tolerance_straddled.positions.size();
         index += 3u) {
        tolerance_straddled.positions[index] += endpoint_tolerance_m * 0.51f;
    }
    tolerance_straddled.content_digest =
        gpu_meshing::mesh_content_digest(tolerance_straddled);
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              strip, tolerance_straddled,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error),
          error.message.c_str());
    CHECK(metrics.symmetric_hausdorff_m < endpoint_tolerance_m &&
              metrics.unmatched_open_edges == 0u &&
              hydrology::water_cut_is_assertion_weldable(
                  metrics, endpoint_tolerance_m),
          "corresponding ownership edges remain matched when sub-tolerance vertex drift straddles adjacent quantization keys");
    const float nearby_first_y = endpoint_tolerance_m * 100.49f;
    const float nearby_second_y = endpoint_tolerance_m * 100.51f;
    const auto nearby_strip = combine(
        quad(-0.625f, 0.625f, nearby_first_y, -0.5f, 0.5f),
        quad(-0.625f, 0.625f, nearby_second_y, -0.5f, 0.5f));
    const auto nearby_downstream = combine(
        quad(0.625f, 1.625f, nearby_first_y, -0.5f, 0.5f),
        quad(0.625f, 1.625f, nearby_second_y, -0.5f, 0.5f));
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              nearby_strip, nearby_downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.first_points == 4u &&
              metrics.second_points == 4u &&
              metrics.symmetric_hausdorff_m == 0.0f &&
              metrics.unmatched_open_edges == 0u,
          "distinct contour vertices remain distinct even when they are geometrically closer than the cross-mesh weld tolerance");
    const float split_y = endpoint_tolerance_m * 100.49f;
    const auto split_strip = combine(
        quad(-0.625f, 0.625f, split_y, -0.5f, 0.0f),
        quad(-0.625f, 0.625f,
             split_y + endpoint_tolerance_m * 0.02f, 0.0f, 0.5f));
    const auto split_downstream = combine(
        quad(0.625f, 1.625f, split_y, -0.5f, 0.0f),
        quad(0.625f, 1.625f, split_y, 0.0f, 0.5f));
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              split_strip, split_downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.symmetric_hausdorff_m < endpoint_tolerance_m &&
              metrics.unmatched_open_edges == 0u,
          "sub-tolerance duplicate endpoints that continue one contour edge remain topologically welded");

    const float graph_y = 4.0f;
    const auto unsplit_strip = cell_face_segment(
        0.625f, true, graph_y, -0.5f, graph_y, 0.5f);
    const auto subdivided_downstream = combine(
        cell_face_segment(0.625f, false, graph_y, -0.5f,
                          graph_y, 0.0f),
        cell_face_segment(0.625f, false, graph_y, 0.0f,
                          graph_y, 0.5f));
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              unsplit_strip, subdivided_downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.symmetric_hausdorff_m == 0.0f &&
              metrics.unmatched_open_edges == 0u,
          "equivalent contour curves remain welded when independent marching-cubes packing retains a different edge subdivision");

    const auto missing_subdivision = cell_face_segment(
        0.625f, false, graph_y, -0.5f, graph_y, 0.0f);
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              unsplit_strip, missing_subdivision,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.unmatched_open_edges != 0u,
          "bidirectional contour coverage rejects a genuine missing segment");

    const auto branched_downstream = combine(
        subdivided_downstream,
        cell_face_segment(0.625f, false, graph_y, 0.0f,
                          graph_y + 0.25f, 0.0f));
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              unsplit_strip, branched_downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.unmatched_open_edges != 0u,
          "contour topology rejects an extra branch even when the main path is covered");

    const auto closed_strip = combine(
        combine(
            cell_face_segment(0.625f, true, graph_y, -0.5f,
                              graph_y + 0.25f, 0.0f),
            cell_face_segment(0.625f, true, graph_y + 0.25f, 0.0f,
                              graph_y, 0.5f)),
        cell_face_segment(0.625f, true, graph_y, 0.5f,
                          graph_y, -0.5f));
    const auto open_downstream = combine(
        cell_face_segment(0.625f, false, graph_y, -0.5f,
                          graph_y + 0.25f, 0.0f),
        cell_face_segment(0.625f, false, graph_y + 0.25f, 0.0f,
                          graph_y, 0.5f));
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              closed_strip, open_downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.unmatched_open_edges != 0u,
          "contour topology rejects a closed-loop versus open-path mismatch");

    const float quotient_y = 7.0f;
    const float quotient_mid_y =
        quotient_y + endpoint_tolerance_m * 0.8f;
    const auto narrow_closed_strip = combine(
        combine(
            cell_face_segment(0.625f, true, quotient_y, -0.5f,
                              quotient_y, 0.5f),
            cell_face_segment(0.625f, true, quotient_y, -0.5f,
                              quotient_mid_y, 0.0f)),
        cell_face_segment(0.625f, true, quotient_mid_y, 0.0f,
                          quotient_y, 0.5f));
    const auto narrow_open_downstream = cell_face_segment(
        0.625f, false, quotient_y, -0.5f, quotient_y, 0.5f);
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              narrow_closed_strip, narrow_open_downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.symmetric_hausdorff_m < endpoint_tolerance_m &&
              metrics.unmatched_open_edges == 0u,
          "one-to-one components whose complete curves mutually cover at the fixed tolerance normalize a narrow closed triangle to its chord");

    const auto closed_downstream = combine(
        combine(
            cell_face_segment(0.625f, false, graph_y, -0.5f,
                              graph_y + 0.25f, 0.0f),
            cell_face_segment(0.625f, false, graph_y + 0.25f, 0.0f,
                              graph_y, 0.5f)),
        cell_face_segment(0.625f, false, graph_y, 0.5f,
                          graph_y, -0.5f));
    auto quantized_duplicate_edge = cell_face_segment(
        0.625f, true, graph_y, -0.5f, graph_y + 0.25f, 0.0f);
    for (std::size_t index = 1u;
         index < quantized_duplicate_edge.positions.size(); index += 3u)
        quantized_duplicate_edge.positions[index] +=
            endpoint_tolerance_m * 0.02f;
    quantized_duplicate_edge.content_digest =
        gpu_meshing::mesh_content_digest(quantized_duplicate_edge);
    const auto closed_with_duplicate_strip = combine(
        closed_strip, quantized_duplicate_edge);
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              closed_with_duplicate_strip, closed_downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.symmetric_hausdorff_m < endpoint_tolerance_m &&
              metrics.unmatched_open_edges == 0u,
          "a quantized duplicate contour segment remains one geometric edge instead of cancelling a closed loop open");

    const auto packed_forward_incidence = cell_face_segment(
        0.625f, true, 6.0f, -0.5f, 6.0f, 0.5f);
    const auto packed_reverse_incidence = cell_face_segment(
        0.625f, true,
        6.0f + endpoint_tolerance_m * 0.2f, 0.5f,
        6.0f + endpoint_tolerance_m * 0.2f, -0.5f);
    const auto closed_with_packed_shared_edge = combine(
        closed_strip,
        combine(packed_forward_incidence, packed_reverse_incidence));
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              closed_with_packed_shared_edge, closed_downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.unmatched_open_edges == 0u,
          "opposite-winding packed incidences cancel one shared edge even when their endpoints drift within a quantization bin");

    auto diagonal_handoff = handoff;
    diagonal_handoff.lip_origin_m = {};
    diagonal_handoff.tangent = {0.0f, 0.70710677f, 0.70710677f};
    diagonal_handoff.upstream_visual_cut_m = -0.5f;
    diagonal_handoff.downstream_visual_cut_m = 0.05f;
    diagonal_handoff.semantic_key =
        hydrology::spillway_handoff_semantic_key(diagonal_handoff);
    const auto lattice_face_edge = [](float y_shift, bool reverse) {
        gpu_meshing::MeshResult mesh{};
        mesh.positions = {
            reverse ? 0.5f : -0.5f,
            (reverse ? 0.5f : -0.5f) + y_shift, 0.0f,
            reverse ? -0.5f : 0.5f,
            (reverse ? -0.5f : 0.5f) + y_shift, 0.0f,
            0.0f, y_shift + 0.05f, 0.05f};
        mesh.normals = {
            0.0f, 1.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 1.0f, 0.0f};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = 4u;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
        return mesh;
    };
    const auto cut_boundary = lattice_face_edge(
        endpoint_tolerance_m * 0.2f, false);
    const auto off_cut_forward = lattice_face_edge(
        -endpoint_tolerance_m * 0.2f, false);
    const auto off_cut_reverse = lattice_face_edge(
        -endpoint_tolerance_m * 0.2f, true);
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              cut_boundary, cut_boundary,
              {lattice, diagonal_handoff,
               diagonal_handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error),
          error.message.c_str());
    CHECK(!hydrology::measure_water_cell_boundary_continuity(
              off_cut_forward, off_cut_forward,
              {lattice, diagonal_handoff,
               diagonal_handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error),
          "the diagonal synthetic shared edge is outside the ownership cut");
    const auto boundary_with_off_cut_shared_edge = combine(
        cut_boundary, combine(off_cut_forward, off_cut_reverse));
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              boundary_with_off_cut_shared_edge, cut_boundary,
              {lattice, diagonal_handoff,
               diagonal_handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.symmetric_hausdorff_m == 0.0f &&
              metrics.unmatched_open_edges == 0u,
          "off-cut triangle incidences in the same packing bin cannot cancel a real ownership-boundary edge");

    const float duplicate_mid_y =
        graph_y + 0.125f + endpoint_tolerance_m * 0.25f;
    const auto closed_with_subdivided_duplicate_path = combine(
        closed_strip,
        combine(
            cell_face_segment(0.625f, true, graph_y, -0.5f,
                              duplicate_mid_y, -0.25f),
            cell_face_segment(0.625f, true, duplicate_mid_y, -0.25f,
                              graph_y + 0.25f, 0.0f)));
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              closed_with_subdivided_duplicate_path, closed_downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.symmetric_hausdorff_m < endpoint_tolerance_m &&
              metrics.unmatched_open_edges == 0u,
          "a sub-tolerance subdivided duplicate path cannot turn one closed contour into a figure-eight topology");

    const float paired_y = 5.0f;
    const auto paired_outer_strip = combine(
        cell_face_segment(0.625f, true, paired_y, 0.5f,
                          paired_y + 0.25f, 0.0f),
        cell_face_segment(0.625f, true, paired_y + 0.25f, 0.0f,
                          paired_y, -0.5f));
    const auto paired_theta_strip = combine(
        combine(
            paired_outer_strip,
            cell_face_segment(0.625f, true, paired_y, -0.5f,
                              paired_y, 0.5f)),
        combine(
            cell_face_segment(
                0.625f, true, paired_y, -0.5f,
                paired_y + endpoint_tolerance_m * 1.5f, 0.0f),
            cell_face_segment(
                0.625f, true,
                paired_y + endpoint_tolerance_m * 1.5f, 0.0f,
                paired_y, 0.5f)));
    const auto paired_target = combine(
        combine(
            cell_face_segment(0.625f, false, paired_y, 0.5f,
                              paired_y + 0.25f, 0.0f),
            cell_face_segment(0.625f, false, paired_y + 0.25f, 0.0f,
                              paired_y, -0.5f)),
        combine(
            cell_face_segment(
                0.625f, false, paired_y, -0.5f,
                paired_y + endpoint_tolerance_m * 0.75f, 0.0f),
            cell_face_segment(
                0.625f, false,
                paired_y + endpoint_tolerance_m * 0.75f, 0.0f,
                paired_y, 0.5f)));
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              paired_theta_strip, paired_target,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.symmetric_hausdorff_m < endpoint_tolerance_m &&
              metrics.unmatched_open_edges == 0u,
          "a quantized alternate path is normalized only when the paired contour bidirectionally covers it and the retained topology");

    const auto covered_spur_strip = combine(
        closed_strip,
        cell_face_segment(
            0.625f, true, graph_y, -0.5f,
            graph_y - endpoint_tolerance_m * 0.55f, -0.25f));
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              covered_spur_strip, closed_downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.symmetric_hausdorff_m < endpoint_tolerance_m &&
              metrics.unmatched_open_edges == 0u,
          "a target-covered dangling duplicate path is normalized independent of endpoint key ordering");

    const float close_component_y = endpoint_tolerance_m * 300.49f;
    const auto two_close_components = combine(
        cell_face_segment(0.625f, true, close_component_y, -0.5f,
                          close_component_y, 0.5f),
        cell_face_segment(
            0.625f, true,
            close_component_y + endpoint_tolerance_m * 0.02f, -0.5f,
            close_component_y + endpoint_tolerance_m * 0.02f, 0.5f));
    const auto one_close_component = cell_face_segment(
        0.625f, false, close_component_y, -0.5f,
        close_component_y, 0.5f);
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              two_close_components, one_close_component,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.unmatched_open_edges != 0u,
          "topology rejects a missing nearby parallel component even when geometric distance alone is sub-tolerance");
    const auto second_strip = quad(-0.625f, 0.625f, 2.0f,
                                   -0.5f, 0.5f);
    auto second_downstream = quad(0.625f, 1.625f,
                                  2.0f + endpoint_tolerance_m * 0.51f,
                                  -0.5f, 0.5f);
    const auto two_strip = combine(strip, second_strip);
    auto tolerance_straddled_missing = combine(
        tolerance_straddled, second_downstream);
    tolerance_straddled_missing.indices.resize(6u);
    tolerance_straddled_missing.content_digest =
        gpu_meshing::mesh_content_digest(tolerance_straddled_missing);
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              two_strip, tolerance_straddled_missing,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error),
          error.message.c_str());
    CHECK(metrics.unmatched_open_edges != 0u &&
              !hydrology::water_cut_is_assertion_weldable(
                  metrics, endpoint_tolerance_m),
          "tolerance-aware vertex matching still rejects a genuinely missing ownership edge");
    auto off_boundary_duplicate = strip;
    append_duplicate_coplanar_triangle(off_boundary_duplicate, -2.0f);
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              off_boundary_duplicate, downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              lattice.voxel_m / 16.0f, metrics, error),
          error.message.c_str());
    CHECK(metrics.duplicate_coplanar_triangles == 0u,
          "duplicate geometry away from the ownership face cannot pollute a cell-boundary continuity decision");
    auto on_boundary_duplicate = strip;
    append_duplicate_coplanar_triangle(on_boundary_duplicate, 0.625f);
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              on_boundary_duplicate, downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              lattice.voxel_m / 16.0f, metrics, error),
          error.message.c_str());
    CHECK(metrics.duplicate_coplanar_triangles != 0u,
          "duplicate coplanar geometry on the ownership face remains a hard continuity failure");
    auto collapsed_face_triangle_strip = strip;
    auto collapsed_face_triangle_downstream = downstream;
    append_sub_quantization_cell_face_triangle(
        collapsed_face_triangle_strip, 0.625f, endpoint_tolerance_m);
    append_sub_quantization_cell_face_triangle(
        collapsed_face_triangle_downstream, 0.625f,
        endpoint_tolerance_m);
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              collapsed_face_triangle_strip,
              collapsed_face_triangle_downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              metrics.duplicate_coplanar_triangles == 0u,
          "a cell-face triangle whose three vertices collapse to one contour key is not duplicate geometry at the fixed contour resolution");
    auto quantized_sliver = strip;
    append_quantized_cell_face_sliver(
        quantized_sliver, 0.625f, lattice.voxel_m / 16.0f);
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              quantized_sliver, downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              lattice.voxel_m / 16.0f, metrics, error) &&
              hydrology::water_cut_is_assertion_weldable(
                  metrics, lattice.voxel_m / 16.0f),
          "a sub-quantization cell-face sliver cannot create a zero-length contour segment or invalidate an otherwise weldable boundary");
    auto adjacent_key_sliver = strip;
    append_quantized_cell_face_sliver(
        adjacent_key_sliver, 0.625f, endpoint_tolerance_m,
        2.0f + endpoint_tolerance_m * 0.49f);
    CHECK(hydrology::measure_water_cell_boundary_continuity(
              adjacent_key_sliver, downstream,
              {lattice, handoff, handoff.downstream_visual_cut_m},
              endpoint_tolerance_m, metrics, error) &&
              hydrology::water_cut_is_assertion_weldable(
                  metrics, endpoint_tolerance_m),
          "a sub-tolerance cell-face sliver remains degenerate when its endpoints straddle adjacent quantization keys");
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
              slow_surface.aeration < lip_surface.aeration &&
              lip_surface.aeration < downstream_surface.aeration &&
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

void test_measures_bounded_field_continuity_at_both_visual_cuts() {
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
    hydrology::HandoffFieldContinuityMetrics upstream_cut{};
    hydrology::HandoffFieldContinuityMetrics downstream_cut{};
    CHECK(hydrology::measure_handoff_field_continuity(
              products, handoff, upstream_cut, downstream_cut, error),
          error.message.c_str());
    for (const auto* metrics : {&upstream_cut, &downstream_cut}) {
        CHECK(metrics->sample_pairs > 0u &&
                  metrics->maximum_height_delta_m <= 0.009375f &&
                  metrics->minimum_normal_dot >= 0.995f &&
                  metrics->maximum_turbulence_delta <= 0.01f &&
                  metrics->maximum_aeration_delta <= 0.01f &&
                  metrics->maximum_foam_delta <= 0.01f &&
                  metrics->feature_labels_deterministic,
              "height, normal, turbulence, aeration, foam, and feature labels remain continuous at a visual cut");
    }
}

void test_builds_thirty_shared_field_frames_without_static_geometry() {
    auto fixture = shared_field_fixture();
    bind_shared_field_fixture(fixture);
    struct ObservedJob {
        std::vector<gpu_meshing::ParticleSample> particles;
        gpu_meshing::ParticlePhaseBlend phase{};
        gpu_meshing::ParticleLongitudinalFieldBlend source_blend{};
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
                job.phase_blend, job.longitudinal_field_blend});
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
    const auto& source_blend = observed.front().source_blend;
    constexpr float support_radius_m = 0.05f * 2.5f + 0.01f * 4.0f;
    const auto ends_at = [](const gpu_meshing::ParticleSourcePhaseSpan& span,
                            bool primary) {
        return (primary ? span.primary_begin : span.secondary_begin) +
            (primary ? span.primary_count : span.secondary_count);
    };
    CHECK(source_blend.enabled &&
              source_blend.origin_m.x ==
                  fixture.input.handoff.lip_origin_m.x &&
              source_blend.origin_m.y ==
                  fixture.input.handoff.lip_origin_m.y &&
              source_blend.origin_m.z ==
                  fixture.input.handoff.lip_origin_m.z &&
              source_blend.direction.x == fixture.input.handoff.tangent.x &&
              source_blend.direction.y == fixture.input.handoff.tangent.y &&
              source_blend.direction.z == fixture.input.handoff.tangent.z &&
              std::fabs(source_blend.upstream_full_m -
                        (fixture.input.handoff.upstream_visual_cut_m +
                         support_radius_m)) < 1.0e-6f &&
              std::fabs(source_blend.downstream_full_m -
                        (fixture.input.handoff.downstream_visual_cut_m -
                         support_radius_m)) < 1.0e-6f &&
              source_blend.upstream_full_m <
                  source_blend.downstream_full_m,
          "handoff source blending reaches full-source endpoints one complete support halo inside both outer cuts");
    CHECK(source_blend.source[0].primary_begin == 0u &&
              ends_at(source_blend.source[0], true) ==
                  source_blend.source[1].primary_begin &&
              ends_at(source_blend.source[1], true) ==
                  observed.front().phase.split_index &&
              source_blend.source[0].secondary_begin ==
                  observed.front().phase.split_index &&
              ends_at(source_blend.source[0], false) ==
                  source_blend.source[1].secondary_begin &&
              ends_at(source_blend.source[1], false) ==
                  observed.front().particles.size(),
          "source spans preserve upstream/downstream ownership in both temporal phases");
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
    CHECK(diagnostics.peak_decoded_boundary_frames == 4u &&
              diagnostics.source_blend_required,
          "one output frame retains only its four cropped phase-source decodes and records the triggered field blend");

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

void test_joint_builder_retains_accepted_temporary_dam_support() {
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
    CHECK(saw_excluded_upstream_support &&
              diagnostics.retained_temporary_dam_support_contributors > 0u &&
              saw_downstream_interior,
          "the joint builder retains accepted upstream field support touching the former dam together with downstream water");
}

void test_partitioned_union_matches_unfiltered_field_coverage_at_both_cuts() {
    gpu_meshing::ParticleSamplingLattice lattice{};
    gpu_meshing::Error mesh_error{};
    CHECK(gpu_meshing::make_particle_sampling_lattice(
              {0.0f, 0.0f, 0.0f}, 0.1f, lattice, mesh_error),
          mesh_error.message.c_str());
    auto handoff = shared_spillway();
    handoff.upstream_visual_cut_m = -0.5f;
    handoff.downstream_visual_cut_m = 0.5f;
    handoff.temporary_dam_exclusion_bounds_m =
        {{-0.55f, 1.05f, -0.45f}, {-0.45f, 1.15f, 0.45f}};
    handoff.semantic_key = hydrology::spillway_handoff_semantic_key(handoff);

    const auto dense_capture = [](float x_begin, float x_end) {
        hydrology::FluidParticleAnimationCapture capture{};
        capture.frames_per_second = 30u;
        capture.phase_offset_frames = 15u;
        capture.frames.resize(30u);
        for (std::uint32_t frame = 0u; frame != 30u; ++frame) {
            auto& output = capture.frames[frame];
            output.simulation_step = 100u + frame;
            for (float x = x_begin; x <= x_end + 1.0e-5f; x += 0.1f) {
                for (float z = -0.4f; z <= 0.4f + 1.0e-5f; z += 0.1f)
                    output.positions_m.push_back({x, 1.1f, z});
            }
        }
        return capture;
    };
    const auto upstream_capture = dense_capture(-1.0f, 0.2f);
    const auto downstream_capture = dense_capture(-0.1f, 1.0f);
    constexpr float particle_radius_m = 0.08f;
    constexpr float blend_width_m = 0.01f;
    hydrology::WaterBoundaryAnimationSource upstream_source{};
    hydrology::WaterBoundaryAnimationSource downstream_source{};
    CHECK(hydrology::build_water_boundary_animation_source(
              upstream_capture, "upper", 1001u, handoff, lattice,
              particle_radius_m, blend_width_m, true, upstream_source,
              mesh_error) &&
              hydrology::build_water_boundary_animation_source(
                  downstream_capture, "lower", 2002u, handoff, lattice,
                  particle_radius_m, blend_width_m, false,
                  downstream_source, mesh_error),
          mesh_error.message.c_str());

    gpu_meshing::ParticleJob visual{};
    visual.bounds_m = {
        {std::min(upstream_source.crop_bounds_m.min_m.x,
                  downstream_source.crop_bounds_m.min_m.x),
         std::min(upstream_source.crop_bounds_m.min_m.y,
                  downstream_source.crop_bounds_m.min_m.y),
         std::min(upstream_source.crop_bounds_m.min_m.z,
                  downstream_source.crop_bounds_m.min_m.z)},
        {std::max(upstream_source.crop_bounds_m.max_m.x,
                  downstream_source.crop_bounds_m.max_m.x),
         std::max(upstream_source.crop_bounds_m.max_m.y,
                  downstream_source.crop_bounds_m.max_m.y),
         std::max(upstream_source.crop_bounds_m.max_m.z,
                  downstream_source.crop_bounds_m.max_m.z)}};
    visual.voxel_m = lattice.voxel_m;
    visual.blend_width_m = blend_width_m;
    visual.iso_value = 0.0f;
    visual.material = 4u;
    visual.limits = {2048u, 1u << 20u, 1u << 22u, 1u << 23u};
    visual.sampling_lattice = lattice;
    const hydrology::WaterMeshAnimationMesher section_mesher =
        [](const gpu_meshing::ParticleJob& job,
           gpu_meshing::MeshResult& mesh, gpu_meshing::Stats& stats,
           gpu_meshing::Error& error) {
            return sampled_field_coverage_mesher(
                job, mesh, stats, error);
        };
    std::vector<gpu_meshing::MeshResult> observed_joint_fields;
    const hydrology::PhysxFluidBake::VisualMesher handoff_mesher =
        [&](const gpu_meshing::ParticleJob& job,
           gpu_meshing::MeshResult& mesh, gpu_meshing::Stats& stats,
           gpu_meshing::Error& error,
           const gpu_meshing::BuildControl&) {
            const bool meshed = sampled_field_coverage_mesher(
                job, mesh, stats, error);
            if (meshed) observed_joint_fields.push_back(mesh);
            return meshed;
        };

    hydrology::WaterMeshAnimation upstream_reference{};
    hydrology::WaterMeshAnimation downstream_reference{};
    CHECK(hydrology::build_water_mesh_animation(
              upstream_capture, particle_radius_m, visual, section_mesher,
              upstream_reference, mesh_error) &&
              hydrology::build_water_mesh_animation(
                  downstream_capture, particle_radius_m, visual,
                  section_mesher, downstream_reference, mesh_error),
          mesh_error.message.c_str());

    const auto owned_artifact = [&](
        const hydrology::FluidParticleAnimationCapture& capture,
        const hydrology::WaterBoundaryAnimationSource& source,
        const char* section_id, std::uint64_t semantic,
        std::uint64_t source_digest,
        hydrology::WaterMeshAnimationArtifact& artifact) {
        hydrology::WaterMeshAnimation raw{};
        hydrology::WaterMeshAnimation owned{};
        hydrology::FluidBakeError fluid_error{};
        if (!hydrology::build_water_mesh_animation(
                capture, particle_radius_m, visual, section_mesher, raw,
                mesh_error, {&source, 1u}) ||
            !hydrology::clip_section_water_mesh_animation(
                raw, section_id, {handoff}, lattice, owned, fluid_error))
            return false;
        if (!hydrology::pack_water_mesh_animation_artifact(
                {section_id, semantic, source_digest, 0u, lattice.voxel_m,
                 lattice},
                owned, artifact, mesh_error))
            return false;
        std::vector<std::uint8_t> bytes;
        return hydrology::serialize_water_mesh_animation_artifact(
                   artifact, bytes, mesh_error) &&
               hydrology::deserialize_water_mesh_animation_artifact(
                   bytes, artifact, mesh_error);
    };
    hydrology::WaterMeshAnimationArtifact upstream_bulk{};
    hydrology::WaterMeshAnimationArtifact downstream_bulk{};
    CHECK(owned_artifact(upstream_capture, upstream_source, "upper", 1011u,
                         1001u, upstream_bulk) &&
              owned_artifact(downstream_capture, downstream_source, "lower",
                             2022u, 2002u, downstream_bulk),
          mesh_error.message.c_str());
    std::ostringstream bulk_state;
    bulk_state << "field-coverage bulk state: upstream payload="
               << upstream_bulk.payload_digest << " frames="
               << upstream_bulk.frames.size() << ", downstream payload="
               << downstream_bulk.payload_digest << " frames="
               << downstream_bulk.frames.size();
    CHECK(upstream_bulk.payload_digest != 0u &&
              downstream_bulk.payload_digest != 0u &&
              upstream_bulk.frames.size() == 30u &&
              downstream_bulk.frames.size() == 30u,
          bulk_state.str().c_str());

    hydrology::HandoffAnimationBuildInput input{};
    input.upstream = &upstream_source;
    input.downstream = &downstream_source;
    input.upstream_bulk = &upstream_bulk;
    input.downstream_bulk = &downstream_bulk;
    input.handoff = handoff;
    input.lattice = lattice;
    input.visual_template = visual;
    hydrology::WaterMeshAnimation handoff_animation{};
    hydrology::HandoffAnimationBuildDiagnostics diagnostics{};
    hydrology::FluidBakeError fluid_error{};
    const bool built = hydrology::build_handoff_animation_frames(
        input, handoff_mesher, handoff_animation, diagnostics, fluid_error);
    CHECK(!observed_joint_fields.empty(),
          "the handoff builder exposes at least frame zero's real shared scalar field to the mesher");
    if (observed_joint_fields.empty()) return;

    const std::vector<std::uint32_t> frames = built
        ? std::vector<std::uint32_t>{0u, 7u, 15u, 22u, 29u}
        : std::vector<std::uint32_t>{0u};
    for (const std::uint32_t frame : frames) {
        gpu_meshing::MeshResult upper{};
        gpu_meshing::MeshResult lower{};
        CHECK(hydrology::decode_water_mesh_animation_frame(
                  upstream_bulk, frame, upper, mesh_error) &&
                  hydrology::decode_water_mesh_animation_frame(
                      downstream_bulk, frame, lower, mesh_error),
              mesh_error.message.c_str());
        const gpu_meshing::MeshResult& handoff_field = built
            ? handoff_animation.frames[frame]
            : observed_joint_fields.front();
        const std::vector<const gpu_meshing::MeshResult*> partitioned{
            &upper, &handoff_field, &lower};
        const auto expected_upstream = covered_cells_near_cut(
            {&upstream_reference.frames[frame]}, lattice,
            handoff.upstream_visual_cut_m);
        const auto expected_downstream = covered_cells_near_cut(
            {&downstream_reference.frames[frame]}, lattice,
            handoff.downstream_visual_cut_m);
        CHECK(expected_upstream.size() >= 8u &&
                  covered_cells_near_cut(
                      partitioned, lattice,
                      handoff.upstream_visual_cut_m) == expected_upstream,
              "the upper plus shared handoff plus lower partition has the unfiltered upstream field-derived canonical coverage at the former dam");
        CHECK(expected_downstream.size() >= 8u &&
                  covered_cells_near_cut(
                      partitioned, lattice,
                      handoff.downstream_visual_cut_m) == expected_downstream,
              "the partitioned union preserves the unfiltered downstream field-derived canonical coverage at the second ownership cut");
    }
    CHECK(built, fluid_error.message.c_str());
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

void test_late_empty_workset_failure_clears_every_partial_product() {
    auto fixture = shared_field_fixture();
    const auto make_late_rejected_source = [&](bool upstream) {
        auto capture = boundary_capture(
            upstream, upstream ? fixture.upstream_source.crop_bounds_m
                               : fixture.downstream_source.crop_bounds_m);
        const matter::Float3 crossing{
            upstream ? 0.49f : -0.49f,
            upstream ? 1.10f : 1.20f, 0.0f};
        capture.frames[7u].positions_m = {crossing};
        capture.frames[22u].positions_m = {crossing};
        hydrology::WaterBoundaryAnimationSource source{};
        gpu_meshing::Error source_error{};
        CHECK(hydrology::build_water_boundary_animation_source(
                  capture, upstream ? "upper" : "lower",
                  upstream ? 1001u : 2002u, fixture.input.handoff,
                  fixture.input.lattice, 0.05f, 0.01f, upstream,
                  source, source_error),
              source_error.message.c_str());
        return source;
    };
    fixture.upstream_source = make_late_rejected_source(true);
    fixture.downstream_source = make_late_rejected_source(false);
    bind_shared_field_fixture(fixture);

    hydrology::WaterMeshAnimation output{};
    hydrology::HandoffAnimationBuildDiagnostics diagnostics{};
    hydrology::FluidBakeError error{};
    const bool built = hydrology::build_handoff_animation_frames(
        fixture.input, canonical_plane_mesher, output, diagnostics, error);
    bool diagnostics_empty =
        diagnostics.artifact_file_bytes == 0u &&
        diagnostics.peak_build_cpu_payload_bytes == 0u &&
        diagnostics.peak_decoded_boundary_frames == 0u &&
        diagnostics.retained_temporary_dam_support_contributors == 0u &&
        !diagnostics.source_blend_required;
    for (std::size_t frame = 0u;
         frame != diagnostics.frame_mesh_ms.size(); ++frame) {
        diagnostics_empty = diagnostics_empty &&
            diagnostics.frame_mesh_ms[frame] == 0.0 &&
            diagnostics.upstream_cut[frame].first_points == 0u &&
            diagnostics.downstream_cut[frame].first_points == 0u &&
            diagnostics.upstream_band[frame].index_count == 0u &&
            diagnostics.collar[frame].index_count == 0u &&
            diagnostics.downstream_band[frame].index_count == 0u;
    }
    CHECK(!built &&
              error.code == hydrology::FluidBakeCode::ProductFailure &&
              error.message.find("no bounded particles") !=
                  std::string::npos,
          "frame seven reports a well-defined direct ProductFailure after earlier frames were accepted");
    CHECK(output.frames.empty() && output.frames_per_second == 0u &&
              output.phase_offset_frames == 0u &&
              output.duration_seconds == 0.0f && diagnostics_empty,
          "every direct late-frame failure clears all partial animation frames and diagnostics");
}

void test_checked_handoff_animation_workset_accounting() {
    static_assert(sizeof(matter::Float3) == 12u,
                  "decoded positions are three packed floats");
    static_assert(sizeof(gpu_meshing::ParticleSample) == 16u,
                  "retained contributors are one Float3 and one radius");

    std::uint64_t bytes = 99u;
    CHECK(hydrology::checked_handoff_animation_workset_bytes(
              {2u, 3u, 5u, 7u}, 11u, bytes) &&
              bytes == 17u * 12u + 11u * 16u,
          "the live workset exactly counts four decoded Float3 arrays and ParticleSample vector capacity");

    const std::uint64_t decoded_overflow =
        std::numeric_limits<std::uint64_t>::max() /
            sizeof(matter::Float3) + 1u;
    bytes = 99u;
    CHECK(!hydrology::checked_handoff_animation_workset_bytes(
              {decoded_overflow, 0u, 0u, 0u}, 0u, bytes) &&
              bytes == 0u,
          "decoded-position multiplication overflow fails closed");

    const std::uint64_t particle_overflow =
        std::numeric_limits<std::uint64_t>::max() /
            sizeof(gpu_meshing::ParticleSample) + 1u;
    bytes = 99u;
    CHECK(!hydrology::checked_handoff_animation_workset_bytes(
              {0u, 0u, 0u, 0u}, particle_overflow, bytes) &&
              bytes == 0u,
          "retained-particle multiplication overflow fails closed");

    const std::uint64_t decoded_near_limit =
        std::numeric_limits<std::uint64_t>::max() /
            sizeof(matter::Float3);
    bytes = 99u;
    CHECK(!hydrology::checked_handoff_animation_workset_bytes(
              {decoded_near_limit, 0u, 0u, 0u}, 1u, bytes) &&
              bytes == 0u,
          "the decoded-plus-particle total uses checked addition");
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
    timing.boundary_source_semantic_keys = {0x90abu};
    timing.boundary_source_payload_digests = {0xcdefu};
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
    handoff_timing.semantic_key = 0x1111u;
    handoff_timing.payload_digest = 0x2222u;
    handoff_timing.animation_semantic_key = 0x3333u;
    handoff_timing.animation_payload_digest = 0x4444u;
    handoff_timing.loop_frame_29_to_0_synchronized = true;
    handoff_timing.retained_temporary_dam_support_contributors = 12u;
    handoff_timing.upstream_field = {
        8u, 0.004f, 0.999f, 0.002f, 0.003f, 0.004f, true};
    handoff_timing.downstream_field = {
        9u, 0.005f, 0.998f, 0.003f, 0.004f, 0.005f, true};
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
    CHECK(json.find("\"boundarySourceSemanticKeys\":[\"00000000000090ab\"]") !=
                  std::string::npos &&
              json.find("\"boundarySourcePayloadDigests\":[\"000000000000cdef\"]") !=
                  std::string::npos,
          "timing trace exposes immutable boundary sidecar identities");
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
    CHECK(json.find("\"semanticKey\":\"0000000000001111\"") !=
                  std::string::npos &&
              json.find("\"payloadDigest\":\"0000000000002222\"") !=
                  std::string::npos &&
              json.find("\"animationSemanticKey\":\"0000000000003333\"") !=
                  std::string::npos &&
              json.find("\"animationPayloadDigest\":\"0000000000004444\"") !=
                  std::string::npos &&
              json.find("\"loopFrame29To0Synchronized\":true") !=
                  std::string::npos &&
              json.find("\"retainedTemporaryDamSupportContributors\":12") !=
                  std::string::npos,
          "timing trace exposes cache-local handoff identity, loop state, and retained temporary-dam support");
    CHECK(json.find("\"upstreamField\":{\"samplePairs\":8") !=
                  std::string::npos &&
              json.find("\"maximumAerationDelta\":0.003") !=
                  std::string::npos &&
              json.find("\"downstreamField\":{\"samplePairs\":9") !=
                  std::string::npos &&
              json.find("\"featureLabelsDeterministic\":true") !=
                  std::string::npos,
          "timing trace exposes bounded field and feature continuity at both cuts");
}

}  // namespace

int main() {
    test_measures_section_cut_continuity_without_welding();
    test_cut_continuity_fails_closed_on_invalid_meshes();
    test_cell_ownership_uses_one_exact_equality_convention();
    test_builds_deterministic_seam_without_dam_curtain();
    test_measures_bounded_field_continuity_at_both_visual_cuts();
    test_stitches_independently_meshed_cut_contours();
    test_keeps_water_in_the_removed_dam_footprint();
    test_stitches_split_section_contours_to_one_smoothed_collar();
    test_builds_thirty_shared_field_frames_without_static_geometry();
    test_shared_cell_ownership_passes_every_frame_boundary();
    test_joint_builder_retains_accepted_temporary_dam_support();
    test_partitioned_union_matches_unfiltered_field_coverage_at_both_cuts();
    test_shared_strip_chunking_is_geometry_equivalent();
    test_late_empty_workset_failure_clears_every_partial_product();
    test_checked_handoff_animation_workset_accounting();
    test_formats_animation_acceptance_timing_trace();
    return check_summary();
}
