#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <vector>

#include "frame_matrices.h"
#include "matter/math_types.h"

namespace viewer {

struct TemporalInstance {
    std::uint64_t instance_id = 0;
    matter::Mat4f object_to_world{};
};

struct TemporalInstanceFrame {
    std::uint64_t instance_id = 0;
    matter::Mat4f current_object_to_world{};
    matter::Mat4f previous_object_to_world{};
    bool history_valid = false;
};

struct TemporalInvalidation {
    bool camera_cut = false;
    bool world_reload = false;
    bool renderer_reset = false;
};

struct TemporalFrame {
    FrameMatrices current_unjittered{};
    FrameMatrices previous_unjittered{};
    FrameMatrices current_jittered{};
    FrameMatrices previous_jittered{};
    std::vector<TemporalInstanceFrame> instances;
    VkExtent2D internal_extent{};
    VkExtent2D output_extent{};
    float jitter_pixels[2]{};
    bool reset = true;
    std::uint64_t attempt_token = 0;
};

class TemporalState {
public:
    TemporalFrame begin(const FrameMatrices& current_unjittered,
                        VkExtent2D internal_extent, VkExtent2D output_extent,
                        const std::vector<TemporalInstance>& instances,
                        TemporalInvalidation invalidation);
    bool commit_presented(std::uint64_t attempt_token);
    bool discard_failed_attempt(std::uint64_t attempt_token);
    void invalidate() noexcept;

private:
    struct PresentedState {
        FrameMatrices unjittered{};
        FrameMatrices jittered{};
        VkExtent2D internal_extent{};
        VkExtent2D output_extent{};
        std::map<std::uint64_t, matter::Mat4f> transforms;
    };

    struct CandidateState {
        TemporalFrame frame{};
        std::map<std::uint64_t, matter::Mat4f> transforms;
    };

    PresentedState presented_{};
    CandidateState candidate_{};
    bool has_presented_ = false;
    bool has_candidate_ = false;
    bool force_reset_ = true;
    std::uint64_t presented_frame_index_ = 0;
    std::uint64_t next_attempt_token_ = 1;
};

matter::Float3 temporal_velocity_pixels(const TemporalFrame& frame,
                                        std::uint64_t instance_id,
                                        matter::Float3 local_position);

}  // namespace viewer
