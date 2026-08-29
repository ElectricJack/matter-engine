#include "check.h"

#include "hydrology/water_mesh_animation_artifact.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

namespace {

hydrology::WaterMeshAnimation animation_fixture() {
    hydrology::WaterMeshAnimation animation{};
    animation.frames_per_second = 30u;
    animation.phase_offset_frames = 15u;
    animation.duration_seconds = 1.0f;
    animation.frames.resize(30u);
    for (std::uint32_t frame = 0u; frame != 30u; ++frame) {
        const float x = -4.0f + 0.1f * static_cast<float>(frame);
        auto& mesh = animation.frames[frame];
        mesh.positions = {x, 1.0f, -2.0f,
                          x + 0.75f, 1.05f, -2.0f,
                          x, 1.5f, -1.5f};
        mesh.normals = {0.0f, 1.0f, 0.0f,
                        0.15f, 0.977241f, 0.15f,
                        -0.2f, 0.959166f, 0.2f};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = 4u;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
    }
    return animation;
}

hydrology::WaterMeshAnimationArtifactMetadata metadata_fixture() {
    hydrology::WaterMeshAnimationArtifactMetadata metadata{
        "upper-main", 0xabcdu, 0x1111u, 0u, 0.2f};
    metadata.lattice = {{-31.2f, 7.4f, 11.8f}, 0.2f, 1u};
    return metadata;
}

void test_pack_round_trip_and_frame_spans() {
    const auto animation = animation_fixture();
    hydrology::WaterMeshAnimationArtifact artifact{};
    gpu_meshing::Error error{};
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              metadata_fixture(), animation, artifact, error),
          error.message.c_str());
    CHECK(sizeof(hydrology::PackedWaterAnimationVertex) == 12u,
          "the v2 packed vertex ABI remains twelve bytes");
    CHECK(artifact.frames.size() == 30u && artifact.material == 4u &&
              artifact.frame_payload.size() == 30u * (3u * 12u + 3u * 4u),
          "thirty frames retain compact vertices and uint32 indices only");

    std::vector<std::uint8_t> bytes;
    CHECK(hydrology::serialize_water_mesh_animation_artifact(
              artifact, bytes, error),
          error.message.c_str());
    hydrology::WaterMeshAnimationArtifact loaded{};
    CHECK(hydrology::deserialize_water_mesh_animation_artifact(
              bytes, loaded, error),
          error.message.c_str());
    CHECK(loaded.identity == artifact.identity &&
              loaded.frames_per_second == 30u &&
              loaded.phase_offset_frames == 15u &&
              loaded.lattice.origin_m.x == -31.2f &&
              loaded.lattice.origin_m.y == 7.4f &&
              loaded.lattice.origin_m.z == 11.8f &&
              loaded.lattice.voxel_m == 0.2f &&
              loaded.lattice.version == 1u &&
              loaded.frames == artifact.frames &&
              loaded.frame_payload == artifact.frame_payload &&
              loaded.payload_digest != 0u,
          "v2 serialize/deserialize preserves lattice, directory, payload, and digest");

    hydrology::WaterMeshAnimationFrameSpan span{};
    CHECK(hydrology::water_mesh_animation_frame_span(
              loaded, 7u, span, error) &&
              span.vertex_count == 3u && span.index_count == 3u &&
              span.vertex_bytes == 36u && span.index_bytes == 12u,
          "one compressed frame is exposed without decoding the animation");
    gpu_meshing::MeshResult decoded{};
    CHECK(hydrology::decode_water_mesh_animation_frame(
              loaded, 7u, decoded, error),
          error.message.c_str());
    const auto& source = animation.frames[7u];
    const matter::Float3 extent{
        loaded.quantization_bounds_m.max_m.x -
            loaded.quantization_bounds_m.min_m.x,
        loaded.quantization_bounds_m.max_m.y -
            loaded.quantization_bounds_m.min_m.y,
        loaded.quantization_bounds_m.max_m.z -
            loaded.quantization_bounds_m.min_m.z,
    };
    const float extents[3] = {extent.x, extent.y, extent.z};
    for (std::size_t value = 0u; value != source.positions.size(); ++value) {
        const float half_step =
            extents[value % 3u] / (2.0f * 65535.0f) + 2e-6f;
        CHECK(std::fabs(decoded.positions[value] - source.positions[value]) <=
                  half_step,
              "position quantization stays within half a UNORM16 step");
    }
    for (std::size_t vertex = 0u; vertex != 3u; ++vertex) {
        const float dot =
            decoded.normals[vertex * 3u + 0u] *
                source.normals[vertex * 3u + 0u] +
            decoded.normals[vertex * 3u + 1u] *
                source.normals[vertex * 3u + 1u] +
            decoded.normals[vertex * 3u + 2u] *
                source.normals[vertex * 3u + 2u];
        CHECK(dot >= std::cos(3.14159265358979323846f / 180.0f),
              "octahedral normal round-trip remains within one degree");
    }
    CHECK(decoded.indices == source.indices,
          "packed frames preserve exact uint32 topology");
}

void test_lattice_metadata_changes_animation_payload_identity() {
    const auto animation = animation_fixture();
    gpu_meshing::Error error{};
    hydrology::WaterMeshAnimationArtifact baseline{};
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              metadata_fixture(), animation, baseline, error),
          error.message.c_str());
    std::vector<std::uint8_t> baseline_bytes;
    CHECK(hydrology::serialize_water_mesh_animation_artifact(
              baseline, baseline_bytes, error),
          error.message.c_str());

    auto origin_metadata = metadata_fixture();
    origin_metadata.lattice.origin_m.x += 4.0f;
    hydrology::WaterMeshAnimationArtifact origin_changed{};
    std::vector<std::uint8_t> origin_bytes;
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              origin_metadata, animation, origin_changed, error) &&
              hydrology::serialize_water_mesh_animation_artifact(
                  origin_changed, origin_bytes, error) &&
              origin_bytes != baseline_bytes,
          "changing the lattice origin changes the v2 animation payload identity");

    auto voxel_metadata = metadata_fixture();
    voxel_metadata.visual_voxel_m = 0.25f;
    voxel_metadata.lattice.voxel_m = 0.25f;
    hydrology::WaterMeshAnimationArtifact voxel_changed{};
    std::vector<std::uint8_t> voxel_bytes;
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              voxel_metadata, animation, voxel_changed, error) &&
              hydrology::serialize_water_mesh_animation_artifact(
                  voxel_changed, voxel_bytes, error) &&
              voxel_bytes != baseline_bytes,
          "changing the lattice voxel changes the v2 animation payload identity");

    auto version_metadata = metadata_fixture();
    version_metadata.lattice.version = 0u;
    hydrology::WaterMeshAnimationArtifact version_changed{};
    std::vector<std::uint8_t> version_bytes;
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              version_metadata, animation, version_changed, error) &&
              hydrology::serialize_water_mesh_animation_artifact(
                  version_changed, version_bytes, error) &&
              version_bytes != baseline_bytes,
          "changing the persisted lattice contract version changes animation payload identity");
}

void test_corruption_and_invalid_meshes_fail_closed() {
    auto animation = animation_fixture();
    hydrology::WaterMeshAnimationArtifact artifact{};
    gpu_meshing::Error error{};
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              metadata_fixture(), animation, artifact, error),
          error.message.c_str());
    std::vector<std::uint8_t> bytes;
    CHECK(hydrology::serialize_water_mesh_animation_artifact(
              artifact, bytes, error),
          error.message.c_str());
    hydrology::WaterMeshAnimationArtifact loaded{};

    auto corrupt = bytes;
    corrupt[0] ^= 0xffu;
    CHECK(!hydrology::deserialize_water_mesh_animation_artifact(
              corrupt, loaded, error),
          "bad animation magic is rejected");
    corrupt = bytes;
    corrupt[8] = 3u;
    CHECK(!hydrology::deserialize_water_mesh_animation_artifact(
              corrupt, loaded, error),
          "unknown animation version is rejected");
    corrupt = bytes;
    corrupt[7] = '1';
    corrupt[8] = 1u;
    corrupt[9] = corrupt[10] = corrupt[11] = 0u;
    CHECK(!hydrology::deserialize_water_mesh_animation_artifact(
              corrupt, loaded, error) &&
              loaded.identity.empty() && loaded.frames.empty(),
          "legacy MHYDWAN1 bytes miss safely as stale instead of being decoded as v2");
    corrupt = bytes;
    corrupt.pop_back();
    CHECK(!hydrology::deserialize_water_mesh_animation_artifact(
              corrupt, loaded, error),
          "truncated animation payload is rejected");
    corrupt = bytes;
    corrupt.back() ^= 1u;
    CHECK(!hydrology::deserialize_water_mesh_animation_artifact(
              corrupt, loaded, error),
          "animation payload digest mismatch is rejected");
    corrupt = bytes;
    const std::uint64_t too_large = (1ull << 30u) + 1u;
    for (unsigned byte = 0u; byte != 8u; ++byte)
        corrupt[16u + byte] =
            static_cast<std::uint8_t>(too_large >> (byte * 8u));
    CHECK(!hydrology::deserialize_water_mesh_animation_artifact(
              corrupt, loaded, error) &&
              error.code == gpu_meshing::ErrorCode::LimitExceeded,
          "declared files above one GiB fail before allocation");

    auto invalid_artifact = artifact;
    invalid_artifact.frames[0].index_payload_offset =
        std::numeric_limits<std::uint64_t>::max();
    CHECK(!hydrology::serialize_water_mesh_animation_artifact(
              invalid_artifact, corrupt, error),
          "overflowed frame directory offsets are rejected");
    invalid_artifact = artifact;
    invalid_artifact.quantization_bounds_m.min_m.x =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(!hydrology::serialize_water_mesh_animation_artifact(
              invalid_artifact, corrupt, error),
          "non-finite quantization bounds are rejected");
    invalid_artifact = artifact;
    invalid_artifact.frames[0].content_digest ^= 1u;
    CHECK(!hydrology::serialize_water_mesh_animation_artifact(
              invalid_artifact, corrupt, error),
          "per-frame packed-payload digest mismatch is rejected");

    animation.frames[3].indices[2] = 99u;
    CHECK(!hydrology::pack_water_mesh_animation_artifact(
              metadata_fixture(), animation, artifact, error),
          "out-of-range animation indices are rejected");
    animation = animation_fixture();
    animation.frames[4].positions[0] =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(!hydrology::pack_water_mesh_animation_artifact(
              metadata_fixture(), animation, artifact, error),
          "non-finite animation geometry is rejected");

    animation = animation_fixture();
    auto& collapsing = animation.frames[0];
    collapsing.positions.insert(collapsing.positions.end(), {
        -4.0f, 1.0f, -2.0f,
        -4.0f + 1e-7f, 1.0f, -2.0f,
        -4.0f, 1.0f + 1e-7f, -2.0f,
    });
    collapsing.normals.insert(collapsing.normals.end(), {
        0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
    });
    collapsing.indices.insert(collapsing.indices.end(), {3u, 4u, 5u});
    collapsing.content_digest = gpu_meshing::mesh_content_digest(collapsing);
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              metadata_fixture(), animation, artifact, error),
          error.message.c_str());
    CHECK(artifact.frames[0].vertex_count == 6u &&
              artifact.frames[0].index_count == 3u,
          "quantization omits only collapsed zero-area triangles and retains the usable surface");
}

void test_immutable_save_never_replaces_different_content() {
    const auto animation = animation_fixture();
    hydrology::WaterMeshAnimationArtifact artifact{};
    gpu_meshing::Error error{};
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              metadata_fixture(), animation, artifact, error),
          error.message.c_str());
    const auto path = std::filesystem::temp_directory_path() /
        "matter-water-animation-immutable-test.mhwa";
    std::error_code filesystem_error;
    std::filesystem::remove(path, filesystem_error);
    CHECK(hydrology::save_water_mesh_animation_artifact_immutable(
              path, artifact, error),
          error.message.c_str());
    CHECK(hydrology::save_water_mesh_animation_artifact_immutable(
              path, artifact, error),
          "saving identical bytes to an immutable path succeeds");

    auto different_metadata = metadata_fixture();
    different_metadata.semantic_key += 1u;
    hydrology::WaterMeshAnimationArtifact different{};
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              different_metadata, animation, different, error),
          error.message.c_str());
    CHECK(!hydrology::save_water_mesh_animation_artifact_immutable(
              path, different, error),
          "differing bytes never replace an immutable animation artifact");
    hydrology::WaterMeshAnimationArtifact reopened{};
    CHECK(hydrology::load_water_mesh_animation_artifact(
              path, reopened, error) &&
              reopened.semantic_key == artifact.semantic_key,
          "a rejected immutable save leaves the installed artifact intact");
    std::filesystem::remove(path, filesystem_error);
}

}  // namespace

int main() {
    test_pack_round_trip_and_frame_spans();
    test_lattice_metadata_changes_animation_payload_identity();
    test_corruption_and_invalid_meshes_fail_closed();
    test_immutable_save_never_replaces_different_content();
    return check_summary();
}
