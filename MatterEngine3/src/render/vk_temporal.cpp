#include "vk_temporal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "matrix_math.h"
#include "vk_build_profile.h"

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
    // Negative-height raster viewport puts NDC +Y at the top of the screen, so
    // screen-space (Y-down) jitter pixels need a sign flip here. DLSS receives
    // jitter_pixels directly and expects the Y-down convention.
    const float y_ndc = -2.0f * y_pixels / static_cast<float>(extent.height);
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
    // Reversed-Z: scale by the depth complement (1 - d), which equals the
    // standard-Z depth for the same planes — keeps the pre-migration
    // tolerance instead of collapsing to the floor scene-wide. Mirrors
    // gi_temporal.comp.
    const float depth_threshold = std::max(
        0.01f, 0.02f * std::max(std::fabs(1.0f - current.depth),
                                std::fabs(1.0f - presented_.surface.depth)));
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

namespace {
inline std::uint32_t transform_slot_hash(std::uint64_t id) {
    // Fibonacci-style mix; the ids are already hashes but cheap insurance
    // against clustered low bits costs one multiply.
    std::uint64_t mixed = id * 0x9E3779B97F4A7C15ull;
    return static_cast<std::uint32_t>(mixed >> 32);
}
}  // namespace

void TemporalState::TransformTable::build_map() {
    if (map_built) return;
    std::size_t capacity = 16;
    while (capacity < ids.size() * 2) capacity *= 2;
    slot_keys.assign(capacity, 0);
    slot_entries.assign(capacity, -1);
    slot_mask = static_cast<std::uint32_t>(capacity - 1);
    bool repeated = false;
    // In-order assignment, so a repeated id keeps its LAST value — the exact
    // behaviour of the `next.transforms[id] = transform` loop this replaces.
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const std::uint64_t id = ids[index];
        std::uint32_t slot = transform_slot_hash(id) & slot_mask;
        while (true) {
            if (slot_entries[slot] < 0) {
                slot_keys[slot] = id;
                slot_entries[slot] = static_cast<std::int32_t>(index);
                break;
            }
            if (slot_keys[slot] == id) {
                slot_entries[slot] = static_cast<std::int32_t>(index);
                repeated = true;
                break;
            }
            slot = (slot + 1) & slot_mask;
        }
    }
    unique = !repeated;
    unique_known = true;
    map_built = true;
}

std::int32_t TemporalState::TransformTable::find_index(
    std::uint64_t id) const {
    if (!map_built || slot_entries.empty()) return -1;
    std::uint32_t slot = transform_slot_hash(id) & slot_mask;
    while (true) {
        const std::int32_t entry = slot_entries[slot];
        if (entry < 0) return -1;
        if (slot_keys[slot] == id) return entry;
        slot = (slot + 1) & slot_mask;
    }
}

const matter::Mat4f* TemporalState::TransformTable::find(
    std::uint64_t id) const {
    const std::int32_t entry = find_index(id);
    return entry < 0 ? nullptr : &values[static_cast<std::size_t>(entry)];
}

namespace {

// Kill switch for begin()'s candidate-storage recycling (see
// TemporalState::spare_). MATTER_VK_TEMPORAL_RECYCLE=0 restores the original
// "build into a fresh CandidateState and move it in" behaviour.
bool temporal_recycle_enabled() {
    static const bool value = [] {
        const char* env = std::getenv("MATTER_VK_TEMPORAL_RECYCLE");
        return env == nullptr || env[0] == '\0' || env[0] != '0';
    }();
    return value;
}

// Kill switch for the cursor-tracked history lookup in begin().
// MATTER_VK_TEMPORAL_CURSOR=0 restores the unconditional hashed find().
bool temporal_cursor_enabled() {
    static const bool value = [] {
        const char* env = std::getenv("MATTER_VK_TEMPORAL_CURSOR");
        return env == nullptr || env[0] == '\0' || env[0] != '0';
    }();
    return value;
}

}  // namespace

const TemporalFrame& TemporalState::begin(
    const FrameMatrices& current_unjittered, VkExtent2D internal_extent,
    VkExtent2D output_extent, const std::vector<TemporalInstance>& instances,
    bool jitter_enabled, TemporalInvalidation invalidation) {
    if (has_candidate_) {
        has_candidate_ = false;
        force_reset_ = true;
    }

    const bool recycle = temporal_recycle_enabled();
    CandidateState fresh;                       // unused when recycling
    CandidateState& next = recycle ? spare_ : fresh;
    if (recycle) {
        // spare_ carries a retired frame's storage. Reset exactly the fields a
        // value-initialised CandidateState{} would have zeroed and that the
        // code below does not unconditionally overwrite; everything else is
        // assigned outright further down.
        next.frame.instances.clear();
        next.transforms.map_built = false;
        next.transforms.unique_known = false;
        next.transforms.unique = false;
        next.transforms.slot_mask = 0;
    }
    TemporalFrame& frame = next.frame;
    frame.current_unjittered = current_unjittered;
    frame.internal_extent = internal_extent;
    frame.output_extent = output_extent;
    frame.attempt_token = next_attempt_token_++;
    frame.presented_frame_index = presented_frame_index_;
    if (jitter_enabled) {
        const std::uint64_t jitter_index = presented_frame_index_ + 1;
        frame.jitter_pixels[0] = halton(jitter_index, 2) - 0.5f;
        frame.jitter_pixels[1] = halton(jitter_index, 3) - 0.5f;
        frame.current_jittered = jitter_frame(
            current_unjittered, frame.jitter_pixels[0], frame.jitter_pixels[1],
            internal_extent);
    } else {
        // The value-initialised CandidateState this used to build into left
        // these at zero on the un-jittered path; recycled storage must not
        // inherit the previous frame's offsets.
        frame.jitter_pixels[0] = 0.0f;
        frame.jitter_pixels[1] = 0.0f;
        frame.current_jittered = current_unjittered;
    }

    // Fast path: the incoming id sequence is element-for-element the one the
    // presented frame was built from, and those ids are known to be distinct.
    // Then presented_.transforms.find(instances[i].instance_id) is guaranteed
    // to hit, and — because no id repeats, so no entry was ever overwritten —
    // the value it returns is exactly presented_.transforms.values[i]. Every
    // hashed lookup below therefore has a known answer and can be skipped.
    // Anything else (a changed instance set, or an id sequence whose repeats
    // make the index ambiguous) falls through to the original keyed path.
    bool aligned = has_presented_ && presented_.transforms.unique_known &&
                   presented_.transforms.unique &&
                   presented_.transforms.ids.size() == instances.size();
    if (aligned) {
        vk_build_profile::Scope align_scope(vk_build_profile::kTemporalAlign);
        for (std::size_t index = 0; index < instances.size(); ++index) {
            if (presented_.transforms.ids[index] !=
                instances[index].instance_id) {
                aligned = false;
                break;
            }
        }
    }

    {
        vk_build_profile::Scope fill_scope(vk_build_profile::kTemporalFill);
        vk_perf::resize_geometric(next.transforms.ids, instances.size());
        vk_perf::resize_geometric(next.transforms.values, instances.size());
        for (std::size_t index = 0; index < instances.size(); ++index) {
            next.transforms.ids[index] = instances[index].instance_id;
            next.transforms.values[index] = instances[index].object_to_world;
        }
    }

    if (aligned) {
        // Same id sequence as a set that was already proven repeat-free, so the
        // next frame can take this path too without materialising the map.
        next.transforms.unique_known = true;
        next.transforms.unique = true;
    } else {
        // Keyed path: presented_ may have reached here through a run of fast
        // frames, so its map is materialised on demand (identical contents —
        // build_map() replays the same in-order assignments).
        vk_build_profile::Scope map_scope(vk_build_profile::kTemporalMap);
        presented_.transforms.build_map();
        next.transforms.build_map();
    }
    // An id with no presented transform — a streamed-in sector, a part
    // republished under a new hash — is deliberately NOT a global cut: the
    // newcomer enters with history_valid=false below while every surviving
    // instance keeps its history. A streaming world introduces new ids
    // nearly every frame, so escalating a newcomer to a full reset pinned
    // DLSS and GI accumulation at their first frame forever and the
    // presented image degenerated to the raw internal-resolution render
    // (issues/render-dlss-not-applied).
    frame.reset = force_reset_ || !has_presented_ ||
                  invalidation.camera_cut || invalidation.world_reload ||
                  invalidation.renderer_reset ||
                  !same_extent(internal_extent, presented_.internal_extent) ||
                  !same_extent(output_extent, presented_.output_extent);

    frame.previous_unjittered =
        frame.reset ? frame.current_unjittered : presented_.unjittered;
    frame.previous_jittered = frame.reset
                                  ? frame.current_jittered
                                  : presented_.jittered;
    vk_perf::reserve_geometric(frame.instances, instances.size());
    // Perf: cursor-tracked history resolution.
    //
    // `aligned` demands that the WHOLE id sequence is unchanged, which a
    // streaming world breaks on almost every frame — one published sector
    // inserts ids and every later index shifts. That dropped the entire pass
    // onto the hashed find(), ~90k random probes across a ~9 MB table per
    // frame, for a set whose surviving members are still in their original
    // relative order.
    //
    // So walk presented_ with an independent cursor. `unique` (established by
    // build_map above, and required for this path) means no id repeats, so
    // ids[cursor] == id implies find_index(id) == cursor EXACTLY — the
    // shortcut and the hashed lookup cannot disagree. Anything else falls
    // through to find_index, which is authoritative and also resyncs the
    // cursor, so an insertion costs one probe per NEW id and a deletion one
    // probe to step over it, instead of one probe per instance.
    const bool cursor_path = !aligned && temporal_cursor_enabled() &&
                             presented_.transforms.unique_known &&
                             presented_.transforms.unique;
    const std::size_t presented_count = presented_.transforms.ids.size();
    std::size_t cursor = 0;
    {
        vk_build_profile::Scope history_scope(
            vk_build_profile::kTemporalHistory);
        for (std::size_t index = 0; index < instances.size(); ++index) {
            const TemporalInstance& instance = instances[index];
            const matter::Mat4f* previous = nullptr;
            if (aligned) {
                previous = &presented_.transforms.values[index];
            } else if (cursor_path) {
                if (cursor < presented_count &&
                    presented_.transforms.ids[cursor] == instance.instance_id) {
                    previous = &presented_.transforms.values[cursor];
                    ++cursor;
                } else {
                    const std::int32_t entry =
                        presented_.transforms.find_index(instance.instance_id);
                    if (entry >= 0) {
                        previous =
                            &presented_.transforms.values[
                                static_cast<std::size_t>(entry)];
                        cursor = static_cast<std::size_t>(entry) + 1;
                    }
                }
            } else {
                previous = presented_.transforms.find(instance.instance_id);
            }
            const bool valid = !frame.reset && previous != nullptr;
            frame.instances.push_back(
                {instance.instance_id, instance.object_to_world,
                 valid ? *previous : instance.object_to_world, valid});
        }
    }

    if (recycle) {
        // next IS spare_; after the swap candidate_ holds this frame and
        // spare_ holds the retired storage for the next one.
        std::swap(candidate_, next);
    } else {
        candidate_ = std::move(next);
    }
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
    // Swap rather than move: the retired presented table's storage goes back
    // to the candidate, which begin() recycles (see spare_). Observably
    // identical — candidate_.transforms is fully rewritten by the next
    // begin() and is never read in between.
    std::swap(presented_.transforms, candidate_.transforms);
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
