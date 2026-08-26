#include "check.h"

#include "render/water_animation_gpu.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

hydrology::PackedWaterAnimationVertex packed_vertex(
    std::uint16_t x, std::uint16_t y, std::uint16_t z,
    std::int16_t normal_x, std::int16_t normal_y) {
    return {
        static_cast<std::uint32_t>(x) |
            (static_cast<std::uint32_t>(y) << 16u),
        static_cast<std::uint32_t>(z) |
            (static_cast<std::uint32_t>(
                 static_cast<std::uint16_t>(normal_x)) << 16u),
        static_cast<std::uint32_t>(
            static_cast<std::uint16_t>(normal_y)),
    };
}

viewer::WaterAnimationFrameSelection selection(
    std::uint32_t frame_index,
    const std::vector<hydrology::PackedWaterAnimationVertex>& vertices,
    const std::vector<std::uint32_t>& indices) {
    viewer::WaterAnimationFrameSelection selected{};
    selected.frame_index = frame_index;
    selected.upload_required = true;
    selected.draws.resize(1u);
    selected.draws[0] = {
        "section-0", false, frame_index, 7u,
        {{-2.0f, 3.0f, 1.0f}, {4.0f, 7.0f, 5.0f}},
        {reinterpret_cast<const std::uint8_t*>(vertices.data()),
         reinterpret_cast<const std::uint8_t*>(indices.data()),
         vertices.size() * sizeof(vertices[0]),
         indices.size() * sizeof(indices[0]),
         static_cast<std::uint32_t>(vertices.size()),
         static_cast<std::uint32_t>(indices.size())}};
    return selected;
}

void test_packed_and_decoded_vertex_abi() {
    CHECK(sizeof(hydrology::PackedWaterAnimationVertex) == 12u,
          "baked water animation vertices retain the 12-byte file ABI");
    CHECK(sizeof(viewer::VkWaterAnimationVertex) == 28u,
          "GPU-decoded water vertices retain the 28-byte raster ABI");
    CHECK(offsetof(viewer::VkWaterAnimationVertex, position) == 0u &&
              offsetof(viewer::VkWaterAnimationVertex, normal) == 12u &&
              offsetof(viewer::VkWaterAnimationVertex, material_index) == 24u,
          "water vertex fields match the Vulkan attribute offsets");
}

void test_decode_matches_known_triangle_oracle() {
    const auto packed = packed_vertex(0u, 32768u, 65535u, 0, 0);
    const gpu_meshing::Aabb bounds{
        {-2.0f, 3.0f, 1.0f}, {4.0f, 7.0f, 5.0f}};
    const viewer::VkWaterAnimationVertex decoded =
        viewer::decode_water_animation_vertex_cpu(packed, bounds, 7u);
    CHECK(std::fabs(decoded.position.x - -2.0f) < 1e-6f &&
              std::fabs(decoded.position.y - 5.00003f) < 1e-4f &&
              std::fabs(decoded.position.z - 5.0f) < 1e-6f,
          "packed unorm16 positions decode inside the authored bounds");
    CHECK(std::fabs(decoded.normal.x) < 1e-6f &&
              std::fabs(decoded.normal.y) < 1e-6f &&
              std::fabs(decoded.normal.z - 1.0f) < 1e-6f &&
              decoded.material_index == 7u,
          "octahedral normal and material match the CPU oracle");
}

void test_per_slot_upload_draw_and_barrier_contract() {
    const std::vector<hydrology::PackedWaterAnimationVertex> vertices{
        packed_vertex(0u, 0u, 0u, 0, 0),
        packed_vertex(65535u, 0u, 0u, 0, 0),
        packed_vertex(0u, 65535u, 0u, 0, 0)};
    const std::vector<std::uint32_t> indices{0u, 1u, 2u};
    auto selected = selection(8u, vertices, indices);

    viewer::WaterAnimationGpuSchedule schedule;
    viewer::WaterAnimationGpuError error{};
    const viewer::WaterAnimationGpuCapacity capacity{
        static_cast<std::uint64_t>(vertices.size() * sizeof(vertices[0])),
        static_cast<std::uint64_t>(vertices.size()),
        static_cast<std::uint64_t>(indices.size() * sizeof(indices[0])), 1u};
    CHECK(schedule.publish(41u, 2u, capacity, 0u, error),
          error.message.c_str());

    const std::vector<std::uint32_t> transforms{19u};
    CHECK(schedule.prepare(41u, 0u, selected, transforms, error),
          error.message.c_str());
    const viewer::WaterAnimationGpuFrame* frame = schedule.frame(0u);
    CHECK(frame && frame->upload_required &&
              frame->decode_dispatches.size() == 1u &&
              frame->draws.size() == 1u,
          "a newly selected frame uploads, decodes, and emits one direct draw");
    CHECK(frame->barriers.compute_write_to_vertex_read &&
              frame->barriers.transfer_write_to_index_read,
          "decode and index uploads publish explicit raster-read barriers");
    CHECK(frame->draws[0].proxy_transform_slot == transforms[0] &&
              frame->draws[0].index_count == 3u &&
              frame->draws[0].material_index == 7u,
          "direct draw retains the static proxy transform and water material");
    CHECK((frame->decoded_buffer_usage &
           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) == 0u,
          "dynamic water vertices are never eligible for a BLAS build");

    CHECK(schedule.prepare(41u, 0u, selected, transforms, error),
          error.message.c_str());
    frame = schedule.frame(0u);
    CHECK(frame && !frame->upload_required &&
              frame->decode_dispatches.empty() && frame->draws.size() == 1u,
          "an unchanged frame slot reuses its decoded buffers without dispatch");
    CHECK(schedule.prepare(41u, 1u, selected, transforms, error) &&
              schedule.frame(1u) && schedule.frame(1u)->upload_required,
          "a different Vulkan frame slot receives its own first upload");
}

void test_generation_checked_fence_retirement() {
    viewer::WaterAnimationGpuSchedule schedule;
    viewer::WaterAnimationGpuError error{};
    const viewer::WaterAnimationGpuCapacity capacity{36u, 3u, 12u, 1u};
    CHECK(schedule.publish(7u, 2u, capacity, 0u, error),
          error.message.c_str());
    CHECK(!schedule.publish(7u, 2u, capacity, 4u, error),
          "a duplicate generation cannot replace live resources");
    CHECK(schedule.generation() == 7u && schedule.retired_count() == 0u,
          "failed replacement preserves the accepted generation");
    CHECK(schedule.publish(8u, 2u, capacity, 12u, error) &&
              schedule.generation() == 8u && schedule.retired_count() == 1u,
          "a newer generation retires the old frame resources");
    schedule.collect(11u);
    CHECK(schedule.retired_count() == 1u,
          "old animation resources survive until their fence serial retires");
    schedule.collect(12u);
    CHECK(schedule.retired_count() == 0u,
          "completed fence collection releases retired animation resources");
}

}  // namespace

int main() {
    test_packed_and_decoded_vertex_abi();
    test_decode_matches_known_triangle_oracle();
    test_per_slot_upload_draw_and_barrier_contract();
    test_generation_checked_fence_retirement();
    return check_summary();
}
