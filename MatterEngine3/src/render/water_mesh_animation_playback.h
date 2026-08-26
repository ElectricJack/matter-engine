#pragma once

#include "hydrology/hydrology_network_artifact.h"
#include "hydrology/water_mesh_animation_artifact.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

enum class WaterAnimationFallbackReason : std::uint8_t {
    None,
    AnimationDisabled,
    InvalidConfiguration,
    MissingArtifact,
    CorruptArtifact,
    MismatchedReference,
    CpuBudgetExceeded,
    InvalidFrameSlot,
    InvalidSelection,
};

struct WaterAnimationFallback {
    WaterAnimationFallbackReason reason = WaterAnimationFallbackReason::None;
    std::string message;
};

struct WaterAnimationFrameDraw {
    std::string_view identity;
    bool handoff = false;
    std::uint32_t frame_index = 0u;
    std::uint32_t material_index = 0u;
    gpu_meshing::Aabb quantization_bounds_m{};
    hydrology::WaterMeshAnimationFrameSpan packed{};
};

struct WaterAnimationFrameSelection {
    std::uint32_t frame_index = 0u;
    bool upload_required = false;
    std::vector<WaterAnimationFrameDraw> draws;
};

// Largest synchronized frame across every section and handoff in a loaded
// network. The renderer uses this once at publication to allocate fixed
// per-frame-slot buffers; steady-state selection never grows storage.
struct WaterAnimationPlaybackCapacity {
    std::uint64_t packed_vertex_bytes = 0u;
    std::uint64_t decoded_vertex_count = 0u;
    std::uint64_t index_bytes = 0u;
    std::uint32_t draw_count = 0u;

    bool valid() const noexcept {
        return packed_vertex_bytes != 0u && decoded_vertex_count != 0u &&
               index_bytes != 0u && draw_count != 0u;
    }
};

struct DecodedWaterAnimationVertex {
    matter::Float3 position{};
    matter::Float3 normal{};
    std::uint32_t material_index = 0u;
};

std::uint32_t water_animation_frame(double network_seconds) noexcept;

class WaterMeshAnimationPlayback {
public:
    WaterMeshAnimationPlayback() = default;

    bool active() const noexcept { return !assets_.empty(); }
    std::uint64_t compressed_bytes() const noexcept {
        return compressed_bytes_;
    }
    std::size_t asset_count() const noexcept { return assets_.size(); }
    WaterAnimationPlaybackCapacity maximum_frame_capacity() const noexcept {
        return capacity_;
    }

    WaterAnimationFrameSelection make_selection() const;

    bool select(double network_seconds,
                std::uint32_t frame_slot,
                WaterAnimationFrameSelection& selection,
                WaterAnimationFallback& fallback) noexcept;

    bool decode_frame_for_test(
        std::size_t asset_index,
        std::uint32_t frame_index,
        std::vector<DecodedWaterAnimationVertex>& vertices,
        std::vector<std::uint32_t>& indices,
        WaterAnimationFallback& fallback) const;

private:
    friend bool activate_water_mesh_animation_playback(
        const hydrology::HydrologyNetworkArtifact&,
        const std::filesystem::path&, std::uint32_t, std::uint64_t,
        WaterMeshAnimationPlayback&, WaterAnimationFallback&);

    struct Asset {
        hydrology::HydrologyWaterAnimationReference reference{};
        std::shared_ptr<const hydrology::WaterMeshAnimationArtifact> artifact;
        bool handoff = false;
        std::uint64_t serialized_bytes = 0u;
    };

    std::vector<Asset> assets_;
    std::vector<std::int32_t> last_uploaded_frame_;
    std::uint64_t compressed_bytes_ = 0u;
    WaterAnimationPlaybackCapacity capacity_{};
};

bool activate_water_mesh_animation_playback(
    const hydrology::HydrologyNetworkArtifact& manifest,
    const std::filesystem::path& cache_root,
    std::uint32_t vulkan_frame_slots,
    std::uint64_t cpu_budget_bytes,
    WaterMeshAnimationPlayback& playback,
    WaterAnimationFallback& fallback);

}  // namespace viewer
