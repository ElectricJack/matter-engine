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

void test_packed_vertex_and_direct_decode_abi() {
    CHECK(sizeof(hydrology::PackedWaterAnimationVertex) == 12u,
          "baked water animation vertices retain the 12-byte file ABI");
    CHECK(viewer::kWaterAnimationRasterVertexStride == 12u,
          "the raster lane consumes packed vertices without a 28-byte expansion");
}

void test_direct_raster_buffers_prefer_device_local_coherent_memory() {
    CHECK(viewer::kWaterAnimationRequiredMemory ==
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
          "direct raster buffers remain host visible for fence-owned uploads");
    CHECK((viewer::kWaterAnimationPreferredMemory &
           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u &&
              (viewer::kWaterAnimationPreferredMemory &
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u,
          "direct packed raster reads prefer a coherent device-local BAR heap");
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
    CHECK(frame && frame->upload_required && frame->draws.size() == 1u,
          "a newly selected frame uploads and emits one direct packed draw");
    CHECK(frame->barriers.host_write_to_vertex_read &&
              frame->barriers.host_write_to_index_read,
          "mapped packed vertex/index uploads publish direct raster-read barriers");
    CHECK(frame->draws[0].proxy_transform_slot == transforms[0] &&
              frame->draws[0].index_count == 3u &&
              frame->draws[0].material_index == 7u &&
              frame->draws[0].quantization_bounds_m.min_m.x == -2.0f &&
              frame->draws[0].quantization_bounds_m.max_m.y == 7.0f,
          "direct draw retains its proxy, material, and decode bounds");
    CHECK((frame->raster_vertex_buffer_usage &
           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) == 0u,
          "dynamic water vertices are never eligible for a BLAS build");
    CHECK(capacity.gpu_bytes_per_slot() ==
              capacity.packed_vertex_bytes + capacity.index_bytes,
          "a slot contains one GPU-visible packed vertex/index pair without transfer duplicates");

    CHECK(schedule.prepare(41u, 0u, selected, transforms, error),
          error.message.c_str());
    frame = schedule.frame(0u);
    CHECK(frame && !frame->upload_required && frame->draws.size() == 1u,
          "an unchanged frame slot reuses its packed raster buffers without upload");
    CHECK(schedule.prepare(41u, 1u, selected, transforms, error) &&
              schedule.frame(1u) && schedule.frame(1u)->upload_required,
          "a different Vulkan frame slot receives its own first upload");
}

void test_multiple_draws_pack_indices_at_element_offsets() {
    const std::vector<hydrology::PackedWaterAnimationVertex> first_vertices{
        packed_vertex(0u, 0u, 0u, 0, 0),
        packed_vertex(65535u, 0u, 0u, 0, 0),
        packed_vertex(0u, 65535u, 0u, 0, 0)};
    const std::vector<hydrology::PackedWaterAnimationVertex> second_vertices{
        packed_vertex(65535u, 65535u, 0u, 0, 0),
        packed_vertex(65535u, 0u, 0u, 0, 0),
        packed_vertex(0u, 65535u, 0u, 0, 0)};
    const std::vector<std::uint32_t> first_indices{0u, 1u, 2u};
    const std::vector<std::uint32_t> second_indices{2u, 1u, 0u};
    auto selected = selection(3u, first_vertices, first_indices);
    auto second = selection(3u, second_vertices, second_indices).draws[0];
    second.identity = "section-1";
    selected.draws.push_back(second);

    viewer::WaterAnimationGpuSchedule schedule;
    viewer::WaterAnimationGpuError error{};
    const viewer::WaterAnimationGpuCapacity capacity{
        static_cast<std::uint64_t>((first_vertices.size() +
                                    second_vertices.size()) *
                                   sizeof(first_vertices[0])),
        static_cast<std::uint64_t>(first_vertices.size() +
                                   second_vertices.size()),
        static_cast<std::uint64_t>((first_indices.size() +
                                    second_indices.size()) *
                                   sizeof(first_indices[0])),
        2u};
    CHECK(schedule.publish(51u, 1u, capacity, 0u, error),
          error.message.c_str());
    CHECK(schedule.prepare(51u, 0u, selected, {4u, 5u}, error),
          error.message.c_str());

    const viewer::WaterAnimationGpuFrame* frame = schedule.frame(0u);
    const std::vector<std::uint32_t> expected{0u, 1u, 2u, 2u, 1u, 0u};
    CHECK(frame && frame->indices == expected,
          "multiple animation draws concatenate index payloads contiguously");
    CHECK(frame && frame->draws.size() == 2u &&
              frame->draws[0].first_index == 0u &&
              frame->draws[1].first_index == 3u,
          "multiple animation draws publish element-based first-index offsets");
    CHECK(schedule.steady_state_allocation_count() == 0u,
          "published maximum capacities prevent steady-state frame allocations");
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
    test_packed_vertex_and_direct_decode_abi();
    test_direct_raster_buffers_prefer_device_local_coherent_memory();
    test_decode_matches_known_triangle_oracle();
    test_per_slot_upload_draw_and_barrier_contract();
    test_multiple_draws_pack_indices_at_element_offsets();
    test_generation_checked_fence_retirement();
    return check_summary();
}
