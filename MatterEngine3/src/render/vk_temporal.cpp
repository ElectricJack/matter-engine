#include "vk_temporal.h"

#include <algorithm>
#include <cmath>

#include "matrix_math.h"

namespace viewer {
namespace {

float halton(std::uint64_t index, std::uint32_t base) {
    float result = 0.0f;
    float fraction = 1.0f;
    while (index != 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

bool same_extent(VkExtent2D a, VkExtent2D b) {
    return a.width == b.width && a.height == b.height;
}

FrameMatrices jitter_frame(const FrameMatrices& source, float x_pixels,
                           float y_pixels, VkExtent2D extent) {
    FrameMatrices result = source;
    if (extent.width == 0 || extent.height == 0) return result;
    const float x_ndc = 2.0f * x_pixels / static_cast<float>(extent.width);
    const float y_ndc = 2.0f * y_pixels / static_cast<float>(extent.height);
    for (std::size_t column = 0; column < 4; ++column) {
        result.view_to_clip.m[column] +=
            x_ndc * result.view_to_clip.m[12 + column];
        result.view_to_clip.m[4 + column] +=
            y_ndc * result.view_to_clip.m[12 + column];
    }
    result.world_to_clip =
        mat4_mul(result.view_to_clip, result.world_to_view);
    (void)mat4_inverse(result.world_to_clip, result.clip_to_world);
    (void)extract_frustum_planes_zo(result.world_to_clip,
                                    result.frustum_planes);
    result.jitter_pixels[0] = x_pixels;
    result.jitter_pixels[1] = y_pixels;
    return result;
}

}  // namespace

TemporalFrame TemporalState::begin(
    const FrameMatrices& current_unjittered, VkExtent2D internal_extent,
    VkExtent2D output_extent, const std::vector<TemporalInstance>& instances,
    TemporalInvalidation invalidation) {
    if (has_candidate_) {
        has_candidate_ = false;
        force_reset_ = true;
    }

    CandidateState next{};
    TemporalFrame& frame = next.frame;
    frame.current_unjittered = current_unjittered;
    frame.internal_extent = internal_extent;
    frame.output_extent = output_extent;
    frame.attempt_token = next_attempt_token_++;
    const std::uint64_t jitter_index = presented_frame_index_ + 1;
    frame.jitter_pixels[0] = halton(jitter_index, 2) - 0.5f;
    frame.jitter_pixels[1] = halton(jitter_index, 3) - 0.5f;
    frame.current_jittered = jitter_frame(
        current_unjittered, frame.jitter_pixels[0], frame.jitter_pixels[1],
        internal_extent);

    bool missing_transform = false;
    for (const TemporalInstance& instance : instances) {
        if (presented_.transforms.find(instance.instance_id) ==
            presented_.transforms.end()) {
            missing_transform = true;
        }
        next.transforms[instance.instance_id] = instance.object_to_world;
    }
    frame.reset = force_reset_ || !has_presented_ || missing_transform ||
                  invalidation.camera_cut || invalidation.world_reload ||
                  invalidation.renderer_reset ||
                  !same_extent(internal_extent, presented_.internal_extent) ||
                  !same_extent(output_extent, presented_.output_extent);

    frame.previous_unjittered =
        frame.reset ? frame.current_unjittered : presented_.unjittered;
    // Project both current and previous geometry through the candidate's
    // Halton offset. This keeps static rigid geometry at exactly zero while
    // retaining a jittered motion-vector convention for Streamline.
    frame.previous_jittered = frame.reset
                                  ? frame.current_jittered
                                  : jitter_frame(presented_.unjittered,
                                                 frame.jitter_pixels[0],
                                                 frame.jitter_pixels[1],
                                                 internal_extent);
    frame.instances.reserve(instances.size());
    for (const TemporalInstance& instance : instances) {
        const auto previous = presented_.transforms.find(instance.instance_id);
        const bool valid = !frame.reset && previous != presented_.transforms.end();
        frame.instances.push_back(
            {instance.instance_id, instance.object_to_world,
             valid ? previous->second : instance.object_to_world, valid});
    }

    candidate_ = std::move(next);
    has_candidate_ = true;
    return candidate_.frame;
}

bool TemporalState::commit_presented(std::uint64_t attempt_token) {
    if (!has_candidate_ || candidate_.frame.attempt_token != attempt_token)
        return false;
    presented_.unjittered = candidate_.frame.current_unjittered;
    presented_.jittered = candidate_.frame.current_jittered;
    presented_.internal_extent = candidate_.frame.internal_extent;
    presented_.output_extent = candidate_.frame.output_extent;
    presented_.transforms = std::move(candidate_.transforms);
    has_presented_ = true;
    has_candidate_ = false;
    force_reset_ = false;
    ++presented_frame_index_;
    return true;
}

bool TemporalState::discard_failed_attempt(std::uint64_t attempt_token) {
    if (!has_candidate_ || candidate_.frame.attempt_token != attempt_token)
        return false;
    has_candidate_ = false;
    force_reset_ = true;
    return true;
}

void TemporalState::invalidate() noexcept {
    has_candidate_ = false;
    force_reset_ = true;
}

matter::Float3 temporal_velocity_pixels(const TemporalFrame& frame,
                                        std::uint64_t instance_id,
                                        matter::Float3 local_position) {
    const auto instance = std::find_if(
        frame.instances.begin(), frame.instances.end(),
        [instance_id](const TemporalInstanceFrame& item) {
            return item.instance_id == instance_id;
        });
    if (frame.reset || instance == frame.instances.end() ||
        !instance->history_valid || frame.internal_extent.width == 0 ||
        frame.internal_extent.height == 0) {
        return {};
    }
    const matter::Float4 local{local_position.x, local_position.y,
                               local_position.z, 1.0f};
    const matter::Float4 current_clip = transform(
        frame.current_jittered.world_to_clip,
        transform(instance->current_object_to_world, local));
    const matter::Float4 previous_clip = transform(
        frame.previous_jittered.world_to_clip,
        transform(instance->previous_object_to_world, local));
    if (current_clip.w == 0.0f || previous_clip.w == 0.0f) return {};
    return {(current_clip.x / current_clip.w -
             previous_clip.x / previous_clip.w) *
                0.5f * static_cast<float>(frame.internal_extent.width),
            (current_clip.y / current_clip.w -
             previous_clip.y / previous_clip.w) *
                0.5f * static_cast<float>(frame.internal_extent.height),
            0.0f};
}

}  // namespace viewer
