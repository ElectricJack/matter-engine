#pragma once

#include "hydrology/water_mesh_animation_artifact.h"
#include "render/water_mesh_animation_playback.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

// Off-by-default capture diagnostics shared by the cosmetic playback selector
// and the forward water pass. Values are an explicit CPU/GLSL ABI.
enum class WaterDiagnosticView : std::uint32_t {
    None = 0u,
    Identity = 1u,
    GeometryNormal = 2u,
    FoamDriver = 3u,
};

struct WaterDiagnosticSettings {
    bool capture_frame_enabled = false;
    std::uint32_t capture_frame = 0u;
    WaterDiagnosticView view = WaterDiagnosticView::None;
};

// Pure parser used once by WorldSession::open_world. Null means absent; an
// empty or malformed present value is an error rather than an implicit off.
bool parse_water_diagnostic_settings(
    const char* capture_frame, const char* diagnostic_view,
    WaterDiagnosticSettings& settings, std::string& error) noexcept;

inline double water_capture_time_seconds(
    const WaterDiagnosticSettings& settings) noexcept {
    return (static_cast<double>(settings.capture_frame) + 0.5) / 30.0;
}

// Stable FNV-1a over the complete playback identity bytes. Zero is reserved
// for static water and therefore remapped to a fixed nonzero token.
std::uint32_t water_animation_diagnostic_identity(
    std::string_view identity) noexcept;

// CPU decode oracle for artifact validation/tests. Runtime rasterization keeps
// the 12-byte PackedWaterAnimationVertex intact and decodes it in the water
// vertex specialization, so it never allocates this expanded record.
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
inline constexpr std::uint32_t kWaterAnimationRasterVertexStride =
    sizeof(hydrology::PackedWaterAnimationVertex);

// Keep the fence-owned direct-raster buffers host visible for upload and prefer
// a coherent device-local BAR heap so the packed stream stays in GPU-local
// memory while it is consumed. Vulkan may fall back to any HOST_VISIBLE type
// when this combined heap is unavailable.
inline constexpr VkMemoryPropertyFlags kWaterAnimationRequiredMemory =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
inline constexpr VkMemoryPropertyFlags kWaterAnimationPreferredMemory =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

struct VkWaterAnimationRasterDraw {
    std::uint32_t first_index = 0u;
    std::uint32_t index_count = 0u;
    std::uint32_t vertex_offset = 0u;
    std::uint32_t vertex_count = 0u;
    std::uint32_t proxy_transform_slot = 0u;
    std::uint32_t material_index = 0u;
    std::uint32_t diagnostic_identity = 0u;
    gpu_meshing::Aabb quantization_bounds_m{};
    bool handoff = false;
};

struct WaterAnimationGpuBarriers {
    bool host_write_to_vertex_read = false;
    bool host_write_to_index_read = false;
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
    std::vector<VkWaterAnimationRasterDraw> draws;
    WaterAnimationGpuBarriers barriers{};
    VkBufferUsageFlags raster_vertex_buffer_usage =
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
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
    std::uint64_t steady_state_allocation_count() const noexcept {
        return steady_state_allocation_count_;
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
    std::uint64_t steady_state_allocation_count_ = 0u;
};

}  // namespace viewer
