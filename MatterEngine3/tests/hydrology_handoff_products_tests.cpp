#include "check.h"
#include "hydrology/hydrology_handoff_products.h"
#include "hydrology/river_presentation_field.h"

#include <cmath>
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
               upstream ? 1001u : 2002u, 0u, 0.5f},
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
              {"lower", 2022u, 2002u, 0u, 0.5f},
              animation, artifact, error), error.message.c_str());
    return artifact;
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

void test_builds_frame_aligned_handoff_animation() {
    const auto upstream = animation_artifact("upper", true);
    const auto downstream = animation_artifact("lower", false);
    hydrology::WaterMeshAnimationArtifact handoff_animation{};
    hydrology::FluidBakeError error{};
    CHECK(hydrology::build_handoff_water_animation_artifact(
              upstream, downstream, spillway(), 0.5f,
              handoff_animation, error), error.message.c_str());
    CHECK(handoff_animation.identity == "pool-one" &&
              handoff_animation.frames.size() == 30u &&
              handoff_animation.frames_per_second == 30u &&
              handoff_animation.source_primary_payload_digest == 1001u &&
              handoff_animation.source_secondary_payload_digest == 2002u,
          "handoff animation preserves frame alignment and both static sources");
    gpu_meshing::MeshResult frame{};
    gpu_meshing::Error artifact_error{};
    CHECK(hydrology::decode_water_mesh_animation_frame(
              handoff_animation, 17u, frame, artifact_error),
          artifact_error.message.c_str());
    float minimum = 1000.0f;
    float maximum = -1000.0f;
    for (std::size_t index = 0u; index < frame.positions.size(); index += 3u) {
        minimum = std::min(minimum, frame.positions[index]);
        maximum = std::max(maximum, frame.positions[index]);
    }
    CHECK(std::fabs(minimum + 2.5f) <= 0.01f &&
              std::fabs(maximum - 2.5f) <= 0.01f,
          "handoff frame bridge terminates on the authored ownership cuts");

    auto mismatched = downstream;
    mismatched.frames.pop_back();
    CHECK(!hydrology::build_handoff_water_animation_artifact(
              upstream, mismatched, spillway(), 0.5f,
              handoff_animation, error),
          "a missing adjacent frame prevents handoff animation publication");

    const auto branched = branched_animation_artifact();
    CHECK(hydrology::build_handoff_water_animation_artifact(
              upstream, branched, spillway(), 0.5f,
              handoff_animation, error), error.message.c_str());
    CHECK(handoff_animation.frames.size() == 30u,
          "a marching-cubes cut junction is decomposed into deterministic edge-disjoint bridge paths");
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
}

}  // namespace

int main() {
    test_builds_deterministic_seam_without_dam_curtain();
    test_stitches_independently_meshed_cut_contours();
    test_keeps_water_in_the_removed_dam_footprint();
    test_stitches_split_section_contours_to_one_smoothed_collar();
    test_builds_frame_aligned_handoff_animation();
    test_formats_animation_acceptance_timing_trace();
    return check_summary();
}
