#include "render/water_animation_gpu.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace viewer {
namespace {

bool reject(WaterAnimationGpuErrorCode code, const char* message,
            WaterAnimationGpuError& error) {
    error = {code, message};
    return false;
}

bool add_u64(std::uint64_t a, std::uint64_t b,
             std::uint64_t& result) noexcept {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) return false;
    result = a + b;
    return true;
}

float unpack_snorm16(std::uint16_t bits) noexcept {
    return std::max(-1.0f,
                    static_cast<float>(static_cast<std::int16_t>(bits)) /
                        32767.0f);
}

float sign_not_zero(float value) noexcept {
    return value < 0.0f ? -1.0f : 1.0f;
}

matter::Float3 decode_octahedral(std::int16_t x, std::int16_t y) noexcept {
    matter::Float3 normal{
        unpack_snorm16(static_cast<std::uint16_t>(x)),
        unpack_snorm16(static_cast<std::uint16_t>(y)), 0.0f};
    normal.z = 1.0f - std::fabs(normal.x) - std::fabs(normal.y);
    if (normal.z < 0.0f) {
        const float old_x = normal.x;
        normal.x = (1.0f - std::fabs(normal.y)) * sign_not_zero(old_x);
        normal.y = (1.0f - std::fabs(old_x)) * sign_not_zero(normal.y);
    }
    const float length_squared = normal.x * normal.x + normal.y * normal.y +
                                 normal.z * normal.z;
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-16f)
        return {0.0f, 1.0f, 0.0f};
    const float inverse = 1.0f / std::sqrt(length_squared);
    return {normal.x * inverse, normal.y * inverse, normal.z * inverse};
}

float unorm16(std::uint16_t value, float minimum, float maximum) noexcept {
    return minimum + (maximum - minimum) *
                         (static_cast<float>(value) / 65535.0f);
}

bool finite_bounds(const gpu_meshing::Aabb& bounds) noexcept {
    return std::isfinite(bounds.min_m.x) &&
           std::isfinite(bounds.min_m.y) &&
           std::isfinite(bounds.min_m.z) &&
           std::isfinite(bounds.max_m.x) &&
           std::isfinite(bounds.max_m.y) &&
           std::isfinite(bounds.max_m.z) &&
           bounds.max_m.x >= bounds.min_m.x &&
           bounds.max_m.y >= bounds.min_m.y &&
           bounds.max_m.z >= bounds.min_m.z;
}

}  // namespace

bool WaterAnimationGpuCapacity::valid() const noexcept {
    if (packed_vertex_bytes == 0u || decoded_vertex_count == 0u ||
        index_bytes == 0u || draw_count == 0u ||
        packed_vertex_bytes % sizeof(hydrology::PackedWaterAnimationVertex) !=
            0u ||
        index_bytes % sizeof(std::uint32_t) != 0u)
        return false;
    return packed_vertex_bytes /
               sizeof(hydrology::PackedWaterAnimationVertex) ==
           decoded_vertex_count;
}

std::uint64_t WaterAnimationGpuCapacity::gpu_bytes_per_slot() const noexcept {
    if (!valid() ||
        decoded_vertex_count >
            std::numeric_limits<std::uint64_t>::max() /
                sizeof(VkWaterAnimationVertex))
        return 0u;
    std::uint64_t result = packed_vertex_bytes;
    if (!add_u64(result,
                 decoded_vertex_count * sizeof(VkWaterAnimationVertex),
                 result) ||
        !add_u64(result, index_bytes, result))
        return 0u;
    return result;
}

VkWaterAnimationVertex decode_water_animation_vertex_cpu(
    const hydrology::PackedWaterAnimationVertex& packed,
    const gpu_meshing::Aabb& bounds,
    std::uint32_t material_index) noexcept {
    const std::uint16_t x =
        static_cast<std::uint16_t>(packed.position_xy_unorm16);
    const std::uint16_t y =
        static_cast<std::uint16_t>(packed.position_xy_unorm16 >> 16u);
    const std::uint16_t z = static_cast<std::uint16_t>(
        packed.position_z_unorm16_normal_x_snorm16);
    const auto normal_x = static_cast<std::int16_t>(
        packed.position_z_unorm16_normal_x_snorm16 >> 16u);
    const auto normal_y = static_cast<std::int16_t>(
        packed.normal_y_snorm16_reserved);
    return {{unorm16(x, bounds.min_m.x, bounds.max_m.x),
             unorm16(y, bounds.min_m.y, bounds.max_m.y),
             unorm16(z, bounds.min_m.z, bounds.max_m.z)},
            decode_octahedral(normal_x, normal_y), material_index};
}

bool WaterAnimationGpuSchedule::publish(
    std::uint64_t generation, std::uint32_t frame_slots,
    WaterAnimationGpuCapacity capacity, std::uint64_t retire_after_serial,
    WaterAnimationGpuError& error) {
    error = {};
    if (generation == 0u || frame_slots == 0u || !capacity.valid() ||
        capacity.gpu_bytes_per_slot() == 0u)
        return reject(WaterAnimationGpuErrorCode::InvalidConfiguration,
                      "water animation GPU publication is invalid", error);
    if (generation_ != 0u && generation <= generation_)
        return reject(WaterAnimationGpuErrorCode::StaleGeneration,
                      "water animation GPU generation is stale", error);

    std::vector<WaterAnimationGpuFrame> candidate(frame_slots);
    std::vector<std::int32_t> selected(frame_slots, -1);
    try {
        for (WaterAnimationGpuFrame& frame : candidate) {
            frame.packed_vertices.reserve(
                static_cast<std::size_t>(capacity.packed_vertex_bytes));
            frame.indices.reserve(static_cast<std::size_t>(
                capacity.index_bytes / sizeof(std::uint32_t)));
            frame.decode_dispatches.reserve(capacity.draw_count);
            frame.draws.reserve(capacity.draw_count);
        }
    } catch (...) {
        return reject(WaterAnimationGpuErrorCode::InvalidConfiguration,
                      "water animation GPU CPU staging allocation failed",
                      error);
    }
    if (!frames_.empty())
        retired_.push_back({retire_after_serial, std::move(frames_)});
    frames_ = std::move(candidate);
    selected_frames_ = std::move(selected);
    capacity_ = capacity;
    generation_ = generation;
    return true;
}

bool WaterAnimationGpuSchedule::prepare(
    std::uint64_t generation, std::uint32_t frame_slot,
    const WaterAnimationFrameSelection& selection,
    const std::vector<std::uint32_t>& proxy_transform_slots,
    WaterAnimationGpuError& error) {
    error = {};
    if (generation == 0u || generation != generation_)
        return reject(WaterAnimationGpuErrorCode::StaleGeneration,
                      "water animation frame generation is stale", error);
    if (frame_slot >= frames_.size())
        return reject(WaterAnimationGpuErrorCode::InvalidFrameSlot,
                      "water animation GPU frame slot is invalid", error);
    if (selection.draws.empty() ||
        selection.draws.size() != proxy_transform_slots.size())
        return reject(WaterAnimationGpuErrorCode::InvalidSelection,
                      "water animation draw-to-proxy mapping is invalid",
                      error);

    WaterAnimationGpuFrame& frame = frames_[frame_slot];
    const bool upload = selected_frames_[frame_slot] !=
                        static_cast<std::int32_t>(selection.frame_index);
    frame.upload_required = upload;
    frame.frame_index = selection.frame_index;
    frame.decode_dispatches.clear();
    frame.barriers = {};
    if (!upload) return true;

    std::uint64_t vertex_bytes = 0u;
    std::uint64_t vertex_count = 0u;
    std::uint64_t index_bytes = 0u;
    for (const WaterAnimationFrameDraw& draw : selection.draws) {
        const auto& packed = draw.packed;
        if (!finite_bounds(draw.quantization_bounds_m) ||
            packed.vertex_data == nullptr || packed.index_data == nullptr ||
            packed.vertex_count == 0u || packed.index_count == 0u ||
            packed.index_count % 3u != 0u ||
            packed.vertex_bytes !=
                static_cast<std::size_t>(packed.vertex_count) *
                    sizeof(hydrology::PackedWaterAnimationVertex) ||
            packed.index_bytes !=
                static_cast<std::size_t>(packed.index_count) *
                    sizeof(std::uint32_t) ||
            !add_u64(vertex_bytes, packed.vertex_bytes, vertex_bytes) ||
            !add_u64(vertex_count, packed.vertex_count, vertex_count) ||
            !add_u64(index_bytes, packed.index_bytes, index_bytes))
            return reject(WaterAnimationGpuErrorCode::InvalidSelection,
                          "water animation frame payload is invalid", error);
    }
    if (vertex_bytes > capacity_.packed_vertex_bytes ||
        vertex_count > capacity_.decoded_vertex_count ||
        index_bytes > capacity_.index_bytes ||
        selection.draws.size() > capacity_.draw_count)
        return reject(WaterAnimationGpuErrorCode::CapacityExceeded,
                      "water animation frame exceeds published GPU capacity",
                      error);

    frame.packed_vertices.resize(static_cast<std::size_t>(vertex_bytes));
    frame.indices.resize(static_cast<std::size_t>(
        index_bytes / sizeof(std::uint32_t)));
    frame.draws.clear();
    std::size_t packed_offset = 0u;
    std::size_t index_offset = 0u;
    std::uint32_t output_vertex = 0u;
    for (std::size_t draw_index = 0u;
         draw_index != selection.draws.size(); ++draw_index) {
        const WaterAnimationFrameDraw& selected = selection.draws[draw_index];
        const auto& packed = selected.packed;
        std::memcpy(frame.packed_vertices.data() + packed_offset,
                    packed.vertex_data, packed.vertex_bytes);
        std::memcpy(frame.indices.data() + index_offset,
                    packed.index_data, packed.index_bytes);
        const auto* local_indices = reinterpret_cast<const std::uint32_t*>(
            packed.index_data);
        for (std::uint32_t i = 0u; i != packed.index_count; ++i) {
            if (local_indices[i] >= packed.vertex_count)
                return reject(WaterAnimationGpuErrorCode::InvalidSelection,
                              "water animation index exceeds its vertex frame",
                              error);
        }

        VkWaterAnimationDecodeDispatch dispatch{};
        dispatch.push.bounds_min[0] = selected.quantization_bounds_m.min_m.x;
        dispatch.push.bounds_min[1] = selected.quantization_bounds_m.min_m.y;
        dispatch.push.bounds_min[2] = selected.quantization_bounds_m.min_m.z;
        dispatch.push.bounds_extent[0] =
            selected.quantization_bounds_m.max_m.x -
            selected.quantization_bounds_m.min_m.x;
        dispatch.push.bounds_extent[1] =
            selected.quantization_bounds_m.max_m.y -
            selected.quantization_bounds_m.min_m.y;
        dispatch.push.bounds_extent[2] =
            selected.quantization_bounds_m.max_m.z -
            selected.quantization_bounds_m.min_m.z;
        dispatch.push.packed_word_offset =
            static_cast<std::uint32_t>(packed_offset / sizeof(std::uint32_t));
        dispatch.push.output_vertex = output_vertex;
        dispatch.push.vertex_count = packed.vertex_count;
        dispatch.push.material_index = selected.material_index;
        dispatch.group_count_x = (packed.vertex_count + 63u) / 64u;
        frame.decode_dispatches.push_back(dispatch);
        frame.draws.push_back(
            {static_cast<std::uint32_t>(index_offset /
                                        sizeof(std::uint32_t)),
             packed.index_count, output_vertex, packed.vertex_count,
             proxy_transform_slots[draw_index], selected.material_index,
             selected.handoff});
        packed_offset += packed.vertex_bytes;
        index_offset += packed.index_bytes;
        output_vertex += packed.vertex_count;
    }
    frame.barriers = {true, true, true, true};
    selected_frames_[frame_slot] =
        static_cast<std::int32_t>(selection.frame_index);
    return true;
}

const WaterAnimationGpuFrame* WaterAnimationGpuSchedule::frame(
    std::uint32_t frame_slot) const noexcept {
    return frame_slot < frames_.size() ? &frames_[frame_slot] : nullptr;
}

void WaterAnimationGpuSchedule::collect(
    std::uint64_t completed_serial) noexcept {
    retired_.erase(
        std::remove_if(retired_.begin(), retired_.end(),
                       [completed_serial](const Retired& retired) {
                           return retired.retire_after_serial <=
                                  completed_serial;
                       }),
        retired_.end());
}

void WaterAnimationGpuSchedule::clear(
    std::uint64_t retire_after_serial) {
    if (!frames_.empty())
        retired_.push_back({retire_after_serial, std::move(frames_)});
    selected_frames_.clear();
    capacity_ = {};
    generation_ = 0u;
}

}  // namespace viewer
