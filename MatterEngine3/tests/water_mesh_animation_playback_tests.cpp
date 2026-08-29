#include "check.h"

#include "render/water_mesh_animation_playback.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace {

hydrology::WaterMeshAnimationArtifact make_artifact(
    const std::string& id, std::uint64_t semantic,
    std::uint64_t primary, std::uint64_t secondary = 0u) {
    hydrology::WaterMeshAnimation animation{};
    animation.frames_per_second = 30u;
    animation.phase_offset_frames = 15u;
    animation.duration_seconds = 1.0f;
    animation.frames.resize(30u);
    for (std::uint32_t frame = 0u; frame != 30u; ++frame) {
        auto& mesh = animation.frames[frame];
        const float offset = static_cast<float>(frame) * 0.01f;
        mesh.positions = {offset, 0.0f, 0.0f,
                          offset + 1.0f, 0.1f, 0.0f,
                          offset, 0.5f, 1.0f};
        mesh.normals = {0,1,0, 0,1,0, 0,1,0};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = 4u;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
    }
    hydrology::WaterMeshAnimationArtifact artifact{};
    gpu_meshing::Error error{};
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              {id, semantic, primary, secondary, 0.2f},
              animation, artifact, error), error.message.c_str());
    return artifact;
}

struct Fixture {
    std::filesystem::path root;
    hydrology::HydrologyNetworkArtifact manifest{};
};

Fixture write_fixture() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    Fixture fixture{};
    fixture.root = std::filesystem::temp_directory_path() /
        ("matter-water-playback-" + std::to_string(stamp));
    fixture.manifest.state = hydrology::HydrologyNetworkState::Ready;
    fixture.manifest.section_animations = {
        {"upper", "hydrology/animations/upper.mhwa", 101u,
         1001u, 0u, 30u, 30u, 0u},
        {"lower", "hydrology/animations/lower.mhwa", 202u,
         2002u, 0u, 30u, 30u, 0u},
    };
    fixture.manifest.handoff_animations = {
        {"pool", "hydrology/animations/handoffs/pool.mhwa", 303u,
         1001u, 2002u, 30u, 30u, 0u},
    };
    const auto write = [&](hydrology::HydrologyWaterAnimationReference& ref) {
        auto artifact = make_artifact(
            ref.id, ref.semantic_key, ref.source_primary_payload_digest,
            ref.source_secondary_payload_digest);
        gpu_meshing::Error error{};
        CHECK(hydrology::save_water_mesh_animation_artifact_immutable(
                  fixture.root / ref.relative_path, artifact, error),
              error.message.c_str());
        CHECK(hydrology::load_water_mesh_animation_artifact(
                  fixture.root / ref.relative_path, artifact, error),
              error.message.c_str());
        ref.payload_digest = artifact.payload_digest;
    };
    for (auto& reference : fixture.manifest.section_animations)
        write(reference);
    for (auto& reference : fixture.manifest.handoff_animations)
        write(reference);
    return fixture;
}

void test_common_clock_and_per_slot_upload_decisions() {
    Fixture fixture = write_fixture();
    viewer::WaterMeshAnimationPlayback playback{};
    viewer::WaterAnimationFallback fallback{};
    CHECK(viewer::activate_water_mesh_animation_playback(
              fixture.manifest, fixture.root, 3u, 700ull * 1024ull * 1024ull,
              playback, fallback), fallback.message.c_str());
    const viewer::WaterAnimationPlaybackCapacity capacity =
        playback.maximum_frame_capacity();
    CHECK(capacity.packed_vertex_bytes == 3u * 3u * 12u &&
              capacity.decoded_vertex_count == 3u * 3u &&
              capacity.index_bytes == 3u * 3u * sizeof(std::uint32_t) &&
              capacity.draw_count == 3u,
          "activation precomputes the maximum synchronized frame capacity");
    CHECK(viewer::water_animation_frame(0.0) == 0u &&
              viewer::water_animation_frame(1.0 / 30.0) == 1u &&
              viewer::water_animation_frame(29.9 / 30.0) == 29u &&
              viewer::water_animation_frame(1.0) == 0u &&
              viewer::water_animation_frame(2.0 + 4.0 / 30.0) == 4u,
          "the common 30 Hz clock wraps exactly at one second");

    auto selection = playback.make_selection();
    const std::uint64_t validations_after_activation =
        hydrology::water_mesh_animation_validation_count();
    constexpr std::uint32_t kAuthoredWaterMaterial = 19u;
    CHECK(playback.select(0.10, 0u, kAuthoredWaterMaterial, selection,
                          fallback) &&
              selection.frame_index == 3u && selection.upload_required &&
              selection.draws.size() == 3u,
          "the first frame-slot use selects one frame for every product");
    for (const auto& draw : selection.draws)
        CHECK(draw.frame_index == selection.frame_index &&
                  draw.material_index == kAuthoredWaterMaterial,
              "sections and handoffs share the selected frame and the authored runtime water material");
    const auto draw_capacity = selection.draws.capacity();
    CHECK(playback.select(0.11, 0u, kAuthoredWaterMaterial, selection,
                          fallback) &&
              !selection.upload_required &&
              selection.draws.capacity() == draw_capacity,
          "paused or same-interval playback performs no upload or allocation");
    CHECK(playback.select(0.11, 1u, kAuthoredWaterMaterial, selection,
                          fallback) &&
              selection.upload_required,
          "a newly acquired Vulkan frame slot uploads its current frame once");
    CHECK(playback.select(0.14, 0u, kAuthoredWaterMaterial, selection,
                          fallback) &&
              selection.upload_required && selection.frame_index == 4u,
          "advancing to the next 30 Hz frame requests one slot-local upload");
    CHECK(hydrology::water_mesh_animation_validation_count() ==
              validations_after_activation,
          "steady-state playback uses frame spans proven during transactional activation without re-hashing artifacts");
    std::filesystem::remove_all(fixture.root);
}

void test_cpu_decode_oracle_and_transactional_fallbacks() {
    Fixture fixture = write_fixture();
    viewer::WaterMeshAnimationPlayback playback{};
    viewer::WaterAnimationFallback fallback{};
    CHECK(viewer::activate_water_mesh_animation_playback(
              fixture.manifest, fixture.root, 2u, 700ull * 1024ull * 1024ull,
              playback, fallback), fallback.message.c_str());
    std::vector<viewer::DecodedWaterAnimationVertex> vertices;
    std::vector<std::uint32_t> indices;
    CHECK(playback.decode_frame_for_test(
              0u, 7u, vertices, indices, fallback) &&
              vertices.size() == 3u && indices ==
                  std::vector<std::uint32_t>({0u, 1u, 2u}) &&
              vertices[0].material_index == 4u &&
              std::fabs(vertices[0].position.x - 0.07f) < 0.001f &&
              vertices[0].normal.y > 0.999f,
          "the CPU oracle decodes packed positions, normals, indices, and material");
    CHECK(playback.compressed_bytes() < 700ull * 1024ull * 1024ull,
          "the loaded two-section fixture remains within the RiverFloat CPU budget");
    const auto active_bytes = playback.compressed_bytes();

    auto mismatched = fixture.manifest;
    mismatched.section_animations[0].semantic_key++;
    CHECK(!viewer::activate_water_mesh_animation_playback(
              mismatched, fixture.root, 2u, 700ull * 1024ull * 1024ull,
              playback, fallback) &&
              fallback.reason ==
                  viewer::WaterAnimationFallbackReason::MismatchedReference &&
              playback.compressed_bytes() == active_bytes,
          "mismatched activation falls back without replacing the active set");
    CHECK(!viewer::activate_water_mesh_animation_playback(
              fixture.manifest, fixture.root, 2u, 1u,
              playback, fallback) &&
              fallback.reason ==
                  viewer::WaterAnimationFallbackReason::CpuBudgetExceeded &&
              playback.compressed_bytes() == active_bytes,
          "over-budget activation is atomic");
    std::filesystem::remove(
        fixture.root / fixture.manifest.section_animations[1].relative_path);
    CHECK(!viewer::activate_water_mesh_animation_playback(
              fixture.manifest, fixture.root, 2u,
              700ull * 1024ull * 1024ull, playback, fallback) &&
              fallback.reason ==
                  viewer::WaterAnimationFallbackReason::MissingArtifact,
          "a missing artifact produces a typed whole-network fallback");
    std::filesystem::remove_all(fixture.root);

    Fixture corrupt_fixture = write_fixture();
    const auto corrupt_path = corrupt_fixture.root /
        corrupt_fixture.manifest.section_animations.front().relative_path;
    {
        std::fstream stream(corrupt_path,
                            std::ios::binary | std::ios::in | std::ios::out);
        stream.seekg(-1, std::ios::end);
        char value = 0;
        stream.read(&value, 1);
        value ^= 1;
        stream.seekp(-1, std::ios::end);
        stream.write(&value, 1);
    }
    CHECK(!viewer::activate_water_mesh_animation_playback(
              corrupt_fixture.manifest, corrupt_fixture.root, 2u,
              700ull * 1024ull * 1024ull, playback, fallback) &&
              fallback.reason ==
                  viewer::WaterAnimationFallbackReason::CorruptArtifact,
          "a corrupt artifact produces a typed whole-network fallback");
    std::filesystem::remove_all(corrupt_fixture.root);
}

}  // namespace

int main() {
    test_common_clock_and_per_slot_upload_decisions();
    test_cpu_decode_oracle_and_transactional_fallbacks();
    return check_summary();
}
