#include "check.h"

#include "hydrology/water_boundary_animation_source.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kFrameCount = 30u;

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes,
                       std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes,
                       std::size_t offset) {
    std::uint64_t value = 0u;
    for (unsigned byte = 0u; byte != 8u; ++byte)
        value |= static_cast<std::uint64_t>(bytes[offset + byte]) <<
                 (byte * 8u);
    return value;
}

void write_u64(std::vector<std::uint8_t>& bytes,
               std::size_t offset,
               std::uint64_t value) {
    for (unsigned byte = 0u; byte != 8u; ++byte)
        bytes[offset + byte] =
            static_cast<std::uint8_t>(value >> (byte * 8u));
}

bool near(float a, float b, float tolerance = 1.0e-5f) {
    return std::fabs(a - b) <= tolerance;
}

bool same_point(matter::Float3 a,
                matter::Float3 b,
                float tolerance = 1.0e-5f) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) &&
           near(a.z, b.z, tolerance);
}

hydrology::SpillwayHandoffRecord handoff_fixture() {
    hydrology::SpillwayHandoffRecord handoff{};
    handoff.id = "pool-one";
    handoff.upstream_section_id = "upper";
    handoff.downstream_section_id = "lower";
    handoff.lip_origin_m = {10.0f, 20.0f, 30.0f};
    handoff.tangent = {1.0f, 0.0f, 0.0f};
    handoff.lateral = {0.0f, 0.0f, 1.0f};
    handoff.up = {0.0f, 1.0f, 0.0f};
    handoff.discharge_m3s = 12.0f;
    handoff.width_m = 4.0f;
    handoff.effective_depth_m = 2.0f;
    handoff.channel_depth_m = 3.0f;
    handoff.initial_speed_mps = 1.5f;
    handoff.overlap_m = 2.0f;
    handoff.upstream_visual_cut_m = -1.0f;
    handoff.downstream_visual_cut_m = 1.0f;
    handoff.temporary_dam_exclusion_bounds_m = {
        {9.75f, 19.5f, 29.75f}, {10.25f, 20.5f, 30.25f}};
    handoff.semantic_key =
        hydrology::spillway_handoff_semantic_key(handoff);
    return handoff;
}

gpu_meshing::ParticleSamplingLattice lattice_fixture() {
    gpu_meshing::ParticleSamplingLattice lattice{};
    gpu_meshing::Error error{};
    CHECK(gpu_meshing::make_particle_sampling_lattice(
              {0.0f, 0.0f, 0.0f}, 0.2f, lattice, error),
          error.message.c_str());
    return lattice;
}

hydrology::FluidParticleAnimationCapture capture_fixture(bool reverse) {
    hydrology::FluidParticleAnimationCapture capture{};
    capture.frames_per_second = 30u;
    capture.phase_offset_frames = 15u;
    capture.frames.resize(kFrameCount);
    for (std::uint32_t frame = 0u; frame != kFrameCount; ++frame) {
        auto& output = capture.frames[frame];
        output.simulation_step = 1000u + frame * 11u;
        const float motion = static_cast<float>(frame) * 0.003f;
        output.positions_m = {
            {10.4f + motion, 20.1f, 30.2f},
            {8.4f + motion, 18.2f, 28.4f},
            {9.1f + motion, 22.7f, 31.8f},
            {90.0f, 90.0f, 90.0f},
        };
        if (reverse) std::reverse(output.positions_m.begin(),
                                  output.positions_m.end());
    }
    return capture;
}

hydrology::WaterBoundaryAnimationSource build_fixture(bool reverse = false,
                                                       bool upstream = false) {
    hydrology::WaterBoundaryAnimationSource source{};
    gpu_meshing::Error error{};
    const auto handoff = handoff_fixture();
    CHECK(hydrology::build_water_boundary_animation_source(
              capture_fixture(reverse), upstream ? "upper" : "lower",
              0x8877665544332211ull, handoff, lattice_fixture(), 0.5f,
              0.1f, upstream, source, error),
          error.message.c_str());
    return source;
}

void test_deterministic_round_trip_and_requested_frame_decode() {
    gpu_meshing::Error error{};
    const auto source = build_fixture();
    const auto reversed = build_fixture(true);
    CHECK(source.section_id == "lower" &&
              source.source_section_payload_digest == 0x8877665544332211ull &&
              source.handoff_semantic_key == handoff_fixture().semantic_key &&
              source.frames_per_second == 30u &&
              source.phase_offset_frames == 15u &&
              source.particle_radius_m == 0.5f &&
              source.blend_width_m == 0.1f &&
              source.lattice.version == 1u &&
              source.lattice.voxel_m == 0.2f &&
              source.frames.size() == kFrameCount &&
              source.payload_digest != 0u,
          "boundary source preserves exact endpoint, temporal, lattice, and particle metadata");
    CHECK(source.payload_digest == reversed.payload_digest &&
              source.quantized_positions == reversed.quantized_positions,
          "input particle order cannot change deterministic packed bytes or payload digest");

    std::uint64_t expected_offset = 0u;
    for (std::uint32_t frame = 0u; frame != kFrameCount; ++frame) {
        const auto& record = source.frames[frame];
        CHECK(record.simulation_step == 1000u + frame * 11u &&
                  record.position_byte_offset == expected_offset &&
                  record.particle_count == 3u &&
                  record.content_digest != 0u,
              "each ordered frame preserves its simulation step and contiguous six-byte particle directory");
        expected_offset += static_cast<std::uint64_t>(record.particle_count) *
                           6u;
    }
    CHECK(source.quantized_positions.size() == expected_offset,
          "boundary source stores only six local UNORM16 position bytes per retained particle");

    std::vector<matter::Float3> decoded{{-1.0f, -1.0f, -1.0f}};
    CHECK(hydrology::decode_water_boundary_frame(source, 17u, decoded,
                                                  error),
          error.message.c_str());
    std::vector<matter::Float3> expected =
        capture_fixture(false).frames[17u].positions_m;
    expected.pop_back();
    std::sort(expected.begin(), expected.end(), [](matter::Float3 a,
                                                   matter::Float3 b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    CHECK(decoded.size() == expected.size(),
          "requested-frame decode materializes only that frame's cropped positions");
    for (std::size_t index = 0u; index != decoded.size(); ++index) {
        const float dx = decoded[index].x - expected[index].x;
        const float dy = decoded[index].y - expected[index].y;
        const float dz = decoded[index].z - expected[index].z;
        CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) <=
                  source.lattice.voxel_m / 16.0f,
              "local UNORM16 decoded position stays within one sixteenth voxel");
    }
    auto unrelated_corrupt = source;
    unrelated_corrupt.frames.back().content_digest ^= 1u;
    std::vector<matter::Float3> selected_only;
    CHECK(hydrology::decode_water_boundary_frame(
              unrelated_corrupt, 17u, selected_only, error) &&
              selected_only.size() == decoded.size(),
          "requested-frame decode does not scan or materialize an unrelated frame payload");
    CHECK(!hydrology::decode_water_boundary_frame(
              unrelated_corrupt, 29u, selected_only, error) &&
              selected_only.empty() &&
              error.code == gpu_meshing::ErrorCode::ArtifactFailure,
          "requested-frame decode still validates the selected frame digest");
    decoded.push_back({1.0f, 2.0f, 3.0f});
    CHECK(!hydrology::decode_water_boundary_frame(
              source, kFrameCount, decoded, error) && decoded.empty() &&
              error.code == gpu_meshing::ErrorCode::ArtifactFailure,
          "out-of-range requested frame fails closed with empty output");

    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> reversed_bytes;
    CHECK(hydrology::serialize_water_boundary_animation_source(
              source, bytes, error) &&
              hydrology::serialize_water_boundary_animation_source(
                  reversed, reversed_bytes, error) &&
              bytes == reversed_bytes,
          "equivalent capture orderings serialize to byte-identical sidecars");
    CHECK(bytes.size() >= 32u &&
              std::equal(bytes.begin(), bytes.begin() + 8u,
                         std::array<std::uint8_t, 8u>{
                             'M', 'H', 'Y', 'D', 'W', 'B', 'S', '1'}.begin()) &&
              read_u32(bytes, 8u) == 1u &&
              read_u64(bytes, 16u) == bytes.size() &&
              bytes.size() <= (1ull << 30u),
          "MHYDWBS1 v1 header reports the complete bounded file size");

    hydrology::WaterBoundaryAnimationSource loaded{};
    CHECK(hydrology::deserialize_water_boundary_animation_source(
              bytes, loaded, error),
          error.message.c_str());
    CHECK(loaded.section_id == source.section_id &&
              loaded.source_section_payload_digest ==
                  source.source_section_payload_digest &&
              loaded.handoff_semantic_key == source.handoff_semantic_key &&
              loaded.lattice.origin_m.x == source.lattice.origin_m.x &&
              loaded.lattice.origin_m.y == source.lattice.origin_m.y &&
              loaded.lattice.origin_m.z == source.lattice.origin_m.z &&
              loaded.lattice.voxel_m == source.lattice.voxel_m &&
              loaded.lattice.version == source.lattice.version &&
              same_point(loaded.crop_bounds_m.min_m,
                         source.crop_bounds_m.min_m) &&
              same_point(loaded.crop_bounds_m.max_m,
                         source.crop_bounds_m.max_m) &&
              loaded.quantized_positions == source.quantized_positions &&
              loaded.payload_digest == source.payload_digest,
          "v1 round trip preserves all source metadata, directory, payload, and identity");
    CHECK(loaded.frames.size() == source.frames.size(),
          "v1 round trip preserves exactly thirty frame records");
    for (std::size_t index = 0u; index != loaded.frames.size(); ++index) {
        const auto& a = loaded.frames[index];
        const auto& b = source.frames[index];
        CHECK(a.simulation_step == b.simulation_step &&
                  a.position_byte_offset == b.position_byte_offset &&
                  a.particle_count == b.particle_count &&
                  a.content_digest == b.content_digest,
              "v1 round trip preserves frame order, step, range, and digest");
    }
}

hydrology::FluidParticleAnimationCapture crop_capture(
    const std::vector<matter::Float3>& positions) {
    hydrology::FluidParticleAnimationCapture capture{};
    capture.frames_per_second = 30u;
    capture.phase_offset_frames = 15u;
    capture.frames.resize(kFrameCount);
    for (std::uint32_t frame = 0u; frame != kFrameCount; ++frame) {
        capture.frames[frame].simulation_step = 2000u + frame;
        capture.frames[frame].positions_m = positions;
    }
    return capture;
}

void test_snapped_crop_keeps_dense_wet_support_across_temporary_dam() {
    const auto handoff = handoff_fixture();
    const auto lattice = lattice_fixture();
    float support = 0.0f;
    gpu_meshing::Error error{};
    CHECK(gpu_meshing::particle_field_support_radius_m(
              0.5f, 0.1f, support, error) && near(support, 1.65f),
          "crop test consumes the public visual-field support helper");
    const matter::Float3 minimum_face{
        lattice.voxel_m * 36.0f, lattice.voxel_m * 76.0f,
        lattice.voxel_m * 131.0f};
    const matter::Float3 maximum_face{
        lattice.voxel_m * 64.0f, lattice.voxel_m * 119.0f,
        lattice.voxel_m * 169.0f};
    const matter::Float3 support_touch{11.0f + support, 20.0f, 32.0f};
    const matter::Float3 dam_support_touch{
        handoff.temporary_dam_exclusion_bounds_m.maximum.x + support,
        20.0f, 30.0f};
    const matter::Float3 dam_expanded_corner_only{
        handoff.temporary_dam_exclusion_bounds_m.maximum.x + support * 0.8f,
        handoff.temporary_dam_exclusion_bounds_m.maximum.y + support * 0.8f,
        handoff.temporary_dam_exclusion_bounds_m.maximum.z + support * 0.8f};
    const matter::Float3 snapped_minimum = minimum_face;
    const matter::Float3 snapped_maximum = maximum_face;
    const matter::Float3 unrelated{13.0f, 20.0f, 30.0f};
    const std::array<matter::Float3, 3u> maximum_inside{
        matter::Float3{std::nextafter(maximum_face.x, minimum_face.x),
                       22.0f, 32.0f},
        matter::Float3{11.0f,
                       std::nextafter(maximum_face.y, minimum_face.y),
                       32.0f},
        matter::Float3{11.0f, 22.0f,
                       std::nextafter(maximum_face.z, minimum_face.z)},
    };
    std::vector<matter::Float3> dense_dam_cross_section;
    for (std::uint32_t along = 0u; along != 5u; ++along) {
        for (std::uint32_t across = 0u; across != 5u; ++across) {
            dense_dam_cross_section.push_back({
                9.6f + static_cast<float>(along) * 0.2f,
                20.0f,
                29.6f + static_cast<float>(across) * 0.2f});
        }
    }
    std::vector<matter::Float3> capture_positions{
        support_touch, dam_support_touch, dam_expanded_corner_only,
        snapped_minimum, snapped_maximum, unrelated, maximum_inside[0],
        maximum_inside[1], maximum_inside[2]};
    capture_positions.insert(capture_positions.end(),
                             dense_dam_cross_section.begin(),
                             dense_dam_cross_section.end());
    const auto capture = crop_capture(capture_positions);

    hydrology::WaterBoundaryAnimationSource downstream{};
    CHECK(hydrology::build_water_boundary_animation_source(
              capture, "lower", 0x111u, handoff, lattice, 0.5f, 0.1f,
              false, downstream, error),
          error.message.c_str());
    CHECK(same_point(downstream.crop_bounds_m.min_m, minimum_face) &&
              same_point(downstream.crop_bounds_m.max_m, maximum_face),
          "oriented handoff strip, full width/vertical range, and complete support halo snap outward to canonical faces");
    CHECK(hydrology::water_boundary_source_contains(downstream,
                                                     snapped_minimum) &&
              !hydrology::water_boundary_source_contains(downstream,
                                                          snapped_maximum) &&
              hydrology::water_boundary_source_contains(downstream,
                                                         support_touch) &&
              !hydrology::water_boundary_source_contains(downstream,
                                                          unrelated),
          "one public half-open snapped-crop predicate owns minimum faces, excludes maximum faces, and covers support contributors");
    CHECK(downstream.frames[0].particle_count == 32u,
          "downstream extraction uses the same half-open crop and keeps the dense dam-crossing wet section");
    std::vector<std::uint8_t> downstream_bytes;
    hydrology::WaterBoundaryAnimationSource reopened{};
    CHECK(hydrology::serialize_water_boundary_animation_source(
              downstream, downstream_bytes, error) &&
              hydrology::deserialize_water_boundary_animation_source(
                  downstream_bytes, reopened, error),
          error.message.c_str());
    std::vector<matter::Float3> reopened_positions;
    CHECK(hydrology::decode_water_boundary_frame(
              reopened, 0u, reopened_positions, error),
          error.message.c_str());
    CHECK(std::all_of(reopened_positions.begin(), reopened_positions.end(),
                      [&](matter::Float3 position) {
                          return hydrology::water_boundary_source_contains(
                              reopened, position);
                      }),
          "every retained point, including nextafter of each exclusive maximum face, decodes inside the same half-open crop after round trip");
    CHECK(std::any_of(reopened_positions.begin(), reopened_positions.end(),
                      [&](matter::Float3 position) {
                          return position.x == minimum_face.x &&
                                 position.y == minimum_face.y &&
                                 position.z == minimum_face.z;
                      }),
          "the inclusive minimum crop endpoint survives quantization exactly");
    for (std::size_t index = 1u; index != reopened_positions.size(); ++index) {
        const matter::Float3 previous = reopened_positions[index - 1u];
        const matter::Float3 current = reopened_positions[index];
        const bool ordered =
            previous.x < current.x ||
            (previous.x == current.x &&
             (previous.y < current.y ||
              (previous.y == current.y && previous.z <= current.z)));
        CHECK(ordered,
              "decoded local UNORM16 coordinates preserve lexicographic monotonicity");
    }
    for (matter::Float3 expected : maximum_inside) {
        float nearest_distance = std::numeric_limits<float>::infinity();
        for (matter::Float3 decoded : reopened_positions) {
            const float dx = decoded.x - expected.x;
            const float dy = decoded.y - expected.y;
            const float dz = decoded.z - expected.z;
            nearest_distance = std::min(
                nearest_distance, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        CHECK(nearest_distance <= reopened.lattice.voxel_m / 16.0f,
              "exclusive-maximum interior quantization remains within one sixteenth voxel on every axis");
    }

    hydrology::WaterBoundaryAnimationSource upstream{};
    CHECK(hydrology::build_water_boundary_animation_source(
              capture, "upper", 0x222u, handoff, lattice, 0.5f, 0.1f,
              true, upstream, error),
          error.message.c_str());
    bool every_frame_retains_dense_cross_section = true;
    bool every_frame_retains_tangent_support = true;
    for (std::uint32_t frame = 0u; frame != kFrameCount; ++frame) {
        std::vector<matter::Float3> upstream_positions;
        if (!hydrology::decode_water_boundary_frame(
                upstream, frame, upstream_positions, error)) {
            every_frame_retains_dense_cross_section = false;
            every_frame_retains_tangent_support = false;
            continue;
        }
        every_frame_retains_tangent_support =
            every_frame_retains_tangent_support &&
            std::any_of(upstream_positions.begin(), upstream_positions.end(),
                        [&](matter::Float3 value) {
                            return same_point(
                                value, dam_support_touch,
                                upstream.lattice.voxel_m / 16.0f);
                        });
        every_frame_retains_dense_cross_section =
            every_frame_retains_dense_cross_section &&
            upstream_positions.size() == 32u &&
            std::all_of(
                dense_dam_cross_section.begin(),
                dense_dam_cross_section.end(),
                [&](matter::Float3 expected) {
                    return std::any_of(
                        upstream_positions.begin(), upstream_positions.end(),
                        [&](matter::Float3 decoded) {
                            return same_point(
                                decoded, expected,
                                upstream.lattice.voxel_m / 16.0f);
                        });
                });
    }
    CHECK(every_frame_retains_tangent_support,
          "accepted upstream support tangent to the former dam survives every quantized frame");
    CHECK(every_frame_retains_dense_cross_section,
          "all particles in the dense wet cross-section, including centers inside and supports straddling the former dam, survive every frame");
}

void test_partial_corrupt_overflow_and_lattice_mismatch_fail_closed() {
    gpu_meshing::Error error{};
    auto source = build_fixture();
    std::vector<std::uint8_t> bytes;
    CHECK(hydrology::serialize_water_boundary_animation_source(
              source, bytes, error),
          error.message.c_str());

    auto partial_capture = capture_fixture(false);
    partial_capture.frames.pop_back();
    hydrology::WaterBoundaryAnimationSource rejected = source;
    CHECK(!hydrology::build_water_boundary_animation_source(
              partial_capture, "lower", 0x8877665544332211ull,
              handoff_fixture(), lattice_fixture(), 0.5f, 0.1f, false,
              rejected, error) &&
              rejected.frames.empty() && rejected.quantized_positions.empty() &&
              error.code == gpu_meshing::ErrorCode::ArtifactFailure,
          "a partial 29-frame capture fails closed with no source output");

    hydrology::WaterBoundaryAnimationSource loaded = source;
    auto truncated_directory = bytes;
    const std::size_t directory_start =
        32u + 4u + source.section_id.size() + 16u + 20u + 16u + 24u + 4u;
    truncated_directory.resize(directory_start + 10u);
    CHECK(!hydrology::deserialize_water_boundary_animation_source(
              truncated_directory, loaded, error) &&
              loaded.frames.empty() && loaded.quantized_positions.empty() &&
              error.code == gpu_meshing::ErrorCode::ArtifactFailure,
          "a truncated frame directory fails closed with precise artifact failure");

    auto corrupt_frame_digest = bytes;
    corrupt_frame_digest[directory_start + 16u] ^= 1u;
    CHECK(!hydrology::deserialize_water_boundary_animation_source(
              corrupt_frame_digest, loaded, error) && loaded.frames.empty() &&
              error.code == gpu_meshing::ErrorCode::ArtifactFailure,
          "a corrupt per-frame digest fails closed");
    auto corrupt_frame_data = bytes;
    corrupt_frame_data.back() ^= 1u;
    CHECK(!hydrology::deserialize_water_boundary_animation_source(
              corrupt_frame_data, loaded, error) && loaded.frames.empty() &&
              error.code == gpu_meshing::ErrorCode::ArtifactFailure,
          "corrupt quantized frame data fails closed");

    auto overflowed = source;
    overflowed.frames[0].position_byte_offset =
        std::numeric_limits<std::uint64_t>::max();
    std::vector<std::uint8_t> output{1u, 2u, 3u};
    CHECK(!hydrology::serialize_water_boundary_animation_source(
              overflowed, output, error) && output.empty() &&
              error.code == gpu_meshing::ErrorCode::ArtifactFailure,
          "overflowed frame offset/size arithmetic fails before serialization");

    auto lattice_changed = bytes;
    const std::size_t lattice_origin_offset =
        32u + 4u + source.section_id.size() + 16u;
    lattice_changed[lattice_origin_offset] ^= 1u;
    CHECK(!hydrology::deserialize_water_boundary_animation_source(
              lattice_changed, loaded, error) && loaded.frames.empty() &&
              error.code == gpu_meshing::ErrorCode::ArtifactFailure,
          "changed persisted lattice metadata cannot be admitted under the old payload identity");
    auto mismatched_source = source;
    mismatched_source.crop_bounds_m.min_m.x +=
        mismatched_source.lattice.voxel_m * 0.25f;
    CHECK(!hydrology::serialize_water_boundary_animation_source(
              mismatched_source, output, error) && output.empty() &&
              error.code == gpu_meshing::ErrorCode::ArtifactFailure,
          "crop bounds that no longer lie on canonical lattice faces are rejected");
    auto degenerate_source = source;
    degenerate_source.crop_bounds_m.max_m.x =
        degenerate_source.crop_bounds_m.min_m.x;
    CHECK(!hydrology::serialize_water_boundary_animation_source(
              degenerate_source, output, error) && output.empty() &&
              error.code == gpu_meshing::ErrorCode::ArtifactFailure,
          "a degenerate local quantization extent fails closed");

    auto too_large = bytes;
    write_u64(too_large, 16u, (1ull << 30u) + 1u);
    CHECK(!hydrology::deserialize_water_boundary_animation_source(
              too_large, loaded, error) && loaded.frames.empty() &&
              error.code == gpu_meshing::ErrorCode::LimitExceeded &&
              error.message.find("1 GiB") != std::string::npos,
          "a declared complete sidecar over one GiB fails with a precise limit error before allocation");
}

void test_immutable_save_is_idempotent_and_never_replaces() {
    gpu_meshing::Error error{};
    const auto source = build_fixture();
    const auto path = std::filesystem::temp_directory_path() /
        "matter-water-boundary-source-immutable-test.mhwb";
    std::error_code filesystem_error;
    std::filesystem::remove(path, filesystem_error);
    CHECK(hydrology::save_water_boundary_animation_source_immutable(
              path, source, error),
          error.message.c_str());
    const auto size = std::filesystem::file_size(path, filesystem_error);
    CHECK(!filesystem_error && size > 0u,
          "immutable boundary source save installs a nonempty artifact");
    CHECK(hydrology::save_water_boundary_animation_source_immutable(
              path, source, error) &&
              std::filesystem::file_size(path, filesystem_error) == size,
          "saving the same boundary source content to its immutable path is idempotent");

    hydrology::WaterBoundaryAnimationSource different{};
    CHECK(hydrology::build_water_boundary_animation_source(
              capture_fixture(false), "lower",
              source.source_section_payload_digest + 1u,
              handoff_fixture(), lattice_fixture(), 0.5f, 0.1f, false,
              different, error),
          error.message.c_str());
    CHECK(!hydrology::save_water_boundary_animation_source_immutable(
              path, different, error) &&
              error.code == gpu_meshing::ErrorCode::ArtifactFailure,
          "different content cannot replace an installed immutable boundary sidecar");
    hydrology::WaterBoundaryAnimationSource loaded{};
    CHECK(hydrology::load_water_boundary_animation_source(
              path, loaded, error) &&
              loaded.source_section_payload_digest ==
                  source.source_section_payload_digest,
          "a rejected immutable save leaves the original sidecar intact and loadable");
    std::filesystem::remove(path, filesystem_error);
}

}  // namespace

int main() {
    test_deterministic_round_trip_and_requested_frame_decode();
    test_snapped_crop_keeps_dense_wet_support_across_temporary_dam();
    test_partial_corrupt_overflow_and_lattice_mismatch_fail_closed();
    test_immutable_save_is_idempotent_and_never_replaces();
    return check_summary();
}
