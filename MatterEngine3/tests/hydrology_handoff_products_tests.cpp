#include "check.h"
#include "hydrology/hydrology_handoff_products.h"

#include <cmath>
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
        hydrology::GameplaySample{2,1,upstream ? 1.0f : 6.0f,0,0,true});
    if (upstream) {
        for (std::uint32_t z = 0; z != 2; ++z)
            for (std::uint32_t x = 9; x != 15; ++x)
                artifact.gameplay_field[z * 15u + x].velocity_x_mps = 3.0f;
    } else {
        for (std::uint32_t z = 0; z != 2; ++z)
            artifact.gameplay_field[z * 15u + 13u] = {};
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
              repeat_products.gameplay_field == products.gameplay_field,
          "handoff aggregation is deterministic across repeated builds");
}

}  // namespace

int main() {
    test_builds_deterministic_seam_without_dam_curtain();
    test_stitches_independently_meshed_cut_contours();
    test_keeps_water_in_the_removed_dam_footprint();
    test_stitches_split_section_contours_to_one_smoothed_collar();
    return check_summary();
}
