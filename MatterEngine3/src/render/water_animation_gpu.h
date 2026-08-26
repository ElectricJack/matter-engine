#pragma once

#include "hydrology/water_mesh_animation_artifact.h"
#include "render/water_mesh_animation_playback.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace viewer {

// This is the only vertex ABI consumed by raster_water.vert. The file keeps
// the much smaller 12-byte PackedWaterAnimationVertex until a selected frame
// is decoded on the GPU.
struct VkWaterAnimationVertex {
    matter::Float3 position{};
    matter::Float3 normal{};
    std::uint32_t material_index = 0u;
};

static_assert(sizeof(VkWaterAnimationVertex) == 28u,
              "GPU water animation vertex ABI must be 28 bytes");
static_assert(offsetof(VkWaterAnimationVertex, position) == 0u &&
                  offsetof(VkWaterAnimationVertex, normal) == 12u &&
                  offsetof(VkWaterAnimationVertex, material_index) == 24u,
              "GPU water animation attributes changed ABI");

struct alignas(16) VkWaterAnimationDecodePush {
    float bounds_min[4]{};
    float bounds_extent[4]{};
    std::uint32_t packed_word_offset = 0u;
    std::uint32_t output_vertex = 0u;
    std::uint32_t vertex_count = 0u;
    std::uint32_t material_index = 0u;
};

static_assert(sizeof(VkWaterAnimationDecodePush) == 48u,
              "water animation decode push ABI must be three vec4 records");

struct VkWaterAnimationDecodeDispatch {
    VkWaterAnimationDecodePush push{};
    std::uint32_t group_count_x = 0u;
};

struct VkWaterAnimationRasterDraw {
    std::uint32_t first_index = 0u;
    std::uint32_t index_count = 0u;
    std::uint32_t vertex_offset = 0u;
    std::uint32_t vertex_count = 0u;
    std::uint32_t proxy_transform_slot = 0u;
    std::uint32_t material_index = 0u;
    bool handoff = false;
};

struct WaterAnimationGpuBarriers {
    bool host_write_to_compute_read = false;
    bool host_write_to_transfer_read = false;
    bool compute_write_to_vertex_read = false;
    bool transfer_write_to_index_read = false;
};

struct WaterAnimationGpuCapacity {
    std::uint64_t packed_vertex_bytes = 0u;
    std::uint64_t decoded_vertex_count = 0u;
    std::uint64_t index_bytes = 0u;
    std::uint32_t draw_count = 0u;

    bool valid() const noexcept;
    std::uint64_t gpu_bytes_per_slot() const noexcept;
};

enum class WaterAnimationGpuErrorCode : std::uint8_t {
    None,
    InvalidConfiguration,
    StaleGeneration,
    InvalidFrameSlot,
    InvalidSelection,
    CapacityExceeded,
};

struct WaterAnimationGpuError {
    WaterAnimationGpuErrorCode code = WaterAnimationGpuErrorCode::None;
    std::string message;
};

struct WaterAnimationGpuFrame {
    std::uint32_t frame_index = 0u;
    bool upload_required = false;
    std::vector<std::uint8_t> packed_vertices;
    std::vector<std::uint32_t> indices;
    std::vector<VkWaterAnimationDecodeDispatch> decode_dispatches;
    std::vector<VkWaterAnimationRasterDraw> draws;
    WaterAnimationGpuBarriers barriers{};
    VkBufferUsageFlags decoded_buffer_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
};

VkWaterAnimationVertex decode_water_animation_vertex_cpu(
    const hydrology::PackedWaterAnimationVertex& packed,
    const gpu_meshing::Aabb& bounds,
    std::uint32_t material_index) noexcept;

// CPU transaction/scheduling half of the Vulkan path. It owns no Vulkan
// handles; VkSceneRenderer mirrors each accepted generation into fence-owned
// per-slot buffers. Keeping admission here makes stale generations, capacities,
// uploads, and resource retirement unit-testable without a presentation
// surface.
class WaterAnimationGpuSchedule {
public:
    bool publish(std::uint64_t generation,
                 std::uint32_t frame_slots,
                 WaterAnimationGpuCapacity capacity,
                 std::uint64_t retire_after_serial,
                 WaterAnimationGpuError& error);

    bool prepare(std::uint64_t generation,
                 std::uint32_t frame_slot,
                 const WaterAnimationFrameSelection& selection,
                 const std::vector<std::uint32_t>& proxy_transform_slots,
                 WaterAnimationGpuError& error);

    const WaterAnimationGpuFrame* frame(std::uint32_t frame_slot) const noexcept;
    void collect(std::uint64_t completed_serial) noexcept;
    void clear(std::uint64_t retire_after_serial);

    std::uint64_t generation() const noexcept { return generation_; }
    std::size_t retired_count() const noexcept { return retired_.size(); }
    const WaterAnimationGpuCapacity& capacity() const noexcept {
        return capacity_;
    }

private:
    struct Retired {
        std::uint64_t retire_after_serial = 0u;
        std::vector<WaterAnimationGpuFrame> frames;
    };

    std::uint64_t generation_ = 0u;
    WaterAnimationGpuCapacity capacity_{};
    std::vector<WaterAnimationGpuFrame> frames_;
    std::vector<std::int32_t> selected_frames_;
    std::vector<Retired> retired_;
};

}  // namespace viewer
