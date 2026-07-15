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

GiTemporalResult GiTemporalState::accumulate(
    const GiTemporalSurface& current, matter::Float3 velocity_pixels,
    VkExtent2D extent, GiPixelCoord pixel, bool reset,
    std::uint64_t attempt_token) {
    if (has_candidate_) force_reset_ = true;
    has_candidate_ = true;
    candidate_token_ = attempt_token;
    candidate_extent_ = extent;
    candidate_ = {};
    candidate_.valid = true;
    candidate_.pixel = pixel;
    candidate_.surface = current;

    GiTemporalResult& result = candidate_.result;
    result.radiance = current.radiance;
    result.previous_pixel = {
        static_cast<int>(std::floor(static_cast<float>(pixel.x) -
                                    velocity_pixels.x + 0.5f)),
        static_cast<int>(std::floor(static_cast<float>(pixel.y) -
                                    velocity_pixels.y + 0.5f))};
    const float luminance = 0.2126f * current.radiance.x +
                            0.7152f * current.radiance.y +
                            0.0722f * current.radiance.z;
    result.first_moment = luminance;
    result.second_moment = luminance * luminance;
    result.history_length = 1;

    const bool extent_changed = extent.width != presented_extent_.width ||
                                extent.height != presented_extent_.height;
    if (reset || force_reset_ || extent_changed || !presented_.valid) {
        result.rejection_bits = kGiRejectReset;
        return result;
    }
    if (result.previous_pixel.x < 0 || result.previous_pixel.y < 0 ||
        result.previous_pixel.x >= static_cast<int>(extent.width) ||
        result.previous_pixel.y >= static_cast<int>(extent.height) ||
        result.previous_pixel.x != presented_.pixel.x ||
        result.previous_pixel.y != presented_.pixel.y) {
        result.rejection_bits = kGiRejectBounds;
        return result;
    }
    const float depth_threshold =
        std::max(0.01f, 0.02f * std::max(std::fabs(current.depth),
                                        std::fabs(presented_.surface.depth)));
    if (std::fabs(current.depth - presented_.surface.depth) > depth_threshold) {
        result.rejection_bits = kGiRejectDepth;
        return result;
    }
    const float normal_dot = current.normal.x * presented_.surface.normal.x +
                             current.normal.y * presented_.surface.normal.y +
                             current.normal.z * presented_.surface.normal.z;
    if (normal_dot < 0.85f) {
        result.rejection_bits = kGiRejectNormal;
        return result;
    }
    if (current.material_index != presented_.surface.material_index) {
        result.rejection_bits = kGiRejectMaterial;
        return result;
    }
    if (current.instance_token != presented_.surface.instance_token) {
        result.rejection_bits = kGiRejectInstance;
        return result;
    }

    result.rejection_bits = 0;
    result.history_length = std::min(32u, presented_.result.history_length + 1u);
    const float alpha = std::max(1.0f / static_cast<float>(result.history_length),
                                 0.05f);
    const auto blend = [alpha](float previous, float value) {
        return previous + alpha * (value - previous);
    };
    result.radiance = {blend(presented_.result.radiance.x, current.radiance.x),
                       blend(presented_.result.radiance.y, current.radiance.y),
                       blend(presented_.result.radiance.z, current.radiance.z)};
    result.first_moment = blend(presented_.result.first_moment, luminance);
    result.second_moment =
        blend(presented_.result.second_moment, luminance * luminance);
    return result;
}

bool GiTemporalState::commit_presented(std::uint64_t attempt_token) {
    if (!has_candidate_ || candidate_token_ != attempt_token) return false;
    presented_ = candidate_;
    presented_extent_ = candidate_extent_;
    has_candidate_ = false;
    force_reset_ = false;
    presented_index_ ^= 1u;
    return true;
}

bool GiTemporalState::discard_failed_attempt(std::uint64_t attempt_token) {
    if (!has_candidate_ || candidate_token_ != attempt_token) return false;
    has_candidate_ = false;
    return true;
}

void GiTemporalState::invalidate() noexcept {
    has_candidate_ = false;
    force_reset_ = true;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
void GiTemporalState::seed_presented_for_test(
    VkExtent2D extent, GiPixelCoord pixel, const GiTemporalSurface& surface,
    std::uint32_t history_length) {
    presented_ = {};
    presented_.valid = true;
    presented_.pixel = pixel;
    presented_.surface = surface;
    presented_.result.radiance = surface.radiance;
    const float luminance = 0.2126f * surface.radiance.x +
                            0.7152f * surface.radiance.y +
                            0.0722f * surface.radiance.z;
    presented_.result.first_moment = luminance;
    presented_.result.second_moment = luminance * luminance;
    presented_.result.history_length = std::min(32u, history_length);
    presented_.result.rejection_bits = 0;
    presented_extent_ = extent;
    has_candidate_ = false;
    force_reset_ = false;
}
#endif

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
    frame.presented_frame_index = presented_frame_index_;
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
    frame.previous_jittered = frame.reset
                                  ? frame.current_jittered
                                  : presented_.jittered;
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
                -0.5f * static_cast<float>(frame.internal_extent.height),
            0.0f};
}

std::uint64_t temporal_instance_id(std::uint64_t source_instance_id,
                                   std::uint64_t part_hash,
                                   std::uint32_t child_ordinal) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto fold = [&hash](const void* bytes, std::size_t size) {
        const auto* input = static_cast<const unsigned char*>(bytes);
        for (std::size_t index = 0; index < size; ++index)
            hash = (hash ^ input[index]) * 1099511628211ull;
    };
    fold(&source_instance_id, sizeof(source_instance_id));
    fold(&part_hash, sizeof(part_hash));
    fold(&child_ordinal, sizeof(child_ordinal));
    return hash == 0 ? 1 : hash;
}

}  // namespace viewer
