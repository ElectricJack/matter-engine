#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
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
    // Count of frames that were successfully presented before this candidate.
    // Failed/retried attempts therefore keep the same stochastic frame seed.
    std::uint64_t presented_frame_index = 0;
};

enum GiTemporalRejection : std::uint32_t {
    kGiRejectBounds = 1u << 0,
    kGiRejectDepth = 1u << 1,
    kGiRejectNormal = 1u << 2,
    kGiRejectMaterial = 1u << 3,
    kGiRejectInstance = 1u << 4,
    kGiRejectReset = 1u << 5,
};

struct GiPixelCoord {
    int x = 0;
    int y = 0;
};

struct GiTemporalSurface {
    matter::Float3 radiance{};
    float depth = 1.0f;
    matter::Float3 normal{};
    std::uint32_t material_index = UINT32_MAX;
    std::uint32_t instance_token = UINT32_MAX;
};

struct GiTemporalResult {
    matter::Float3 radiance{};
    float first_moment = 0.0f;
    float second_moment = 0.0f;
    std::uint32_t history_length = 1;
    std::uint32_t rejection_bits = kGiRejectReset;
    GiPixelCoord previous_pixel{};
};

// CPU mirror of gi_temporal.comp's candidate/commit rules. Besides making the
// shader contract deterministic in tests, this owns the presentation token
// semantics used to select the renderer's ping-pong history set.
class GiTemporalState {
public:
    GiTemporalResult accumulate(const GiTemporalSurface& current,
                                matter::Float3 velocity_pixels,
                                VkExtent2D extent, GiPixelCoord pixel,
                                bool reset, std::uint64_t attempt_token);
    bool commit_presented(std::uint64_t attempt_token);
    bool discard_failed_attempt(std::uint64_t attempt_token);
    void invalidate() noexcept;
    std::uint32_t presented_index() const noexcept { return presented_index_; }
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    void seed_presented_for_test(VkExtent2D extent, GiPixelCoord pixel,
                                 const GiTemporalSurface& surface,
                                 std::uint32_t history_length);
#endif

private:
    struct HistoryPixel {
        GiTemporalSurface surface{};
        GiTemporalResult result{};
        GiPixelCoord pixel{};
        bool valid = false;
    };
    HistoryPixel presented_{};
    HistoryPixel candidate_{};
    VkExtent2D presented_extent_{};
    VkExtent2D candidate_extent_{};
    std::uint64_t candidate_token_ = 0;
    std::uint32_t presented_index_ = 0;
    bool has_candidate_ = false;
    bool force_reset_ = true;
};

class TemporalState {
public:
    // Returns a reference into the internal candidate state; it stays valid
    // until the next begin() call. (It used to return by value — at ~60k
    // streaming instances that copied ~8 MB of TemporalInstanceFrame records
    // out, and the caller's copy-assignments doubled it.)
    const TemporalFrame& begin(const FrameMatrices& current_unjittered,
                        VkExtent2D internal_extent, VkExtent2D output_extent,
                        const std::vector<TemporalInstance>& instances,
                        bool jitter_enabled,
                        TemporalInvalidation invalidation);
    bool commit_presented(std::uint64_t attempt_token);
    bool discard_failed_attempt(std::uint64_t attempt_token);
    void invalidate() noexcept;

private:
    // Per-instance transforms carried from one presented frame to the next.
    //
    // Perf: the keyed index used to be a std::unordered_map rebuilt from
    // scratch every frame — one node allocation plus two hashed lookups per
    // instance, which at ~59k instances was measured at ~17 ms of the ~18 ms
    // begin() spends. A parked camera over a static world re-derives an
    // identical map every frame.
    //
    // So the authoritative storage is the index-aligned (ids, values) pair,
    // which is trivially cheap to fill, and the keyed index is materialised
    // only when a frame actually needs keyed lookup (i.e. when the instance
    // set changed). The index itself is a flat linear-probe table over the
    // entry indices — one contiguous fill instead of ~n node allocations,
    // which matters during sector streaming where the set changes every
    // frame. build_map() replays the assignments in order and therefore keeps
    // the original last-writer-wins behaviour for repeated ids; `unique`
    // records whether that ever mattered.
    struct TransformTable {
        std::vector<std::uint64_t> ids;
        std::vector<matter::Mat4f> values;   // values[i] belongs to ids[i]
        std::vector<std::uint64_t> slot_keys;
        std::vector<std::int32_t> slot_entries;  // -1 empty, else values index
        std::uint32_t slot_mask = 0;
        bool map_built = false;
        // Whether `ids` holds no repeats. Known once build_map() has run, and
        // carried forward across frames that reuse the same id sequence.
        bool unique_known = false;
        bool unique = false;
        void build_map();
        const matter::Mat4f* find(std::uint64_t id) const;
    };

    struct PresentedState {
        FrameMatrices unjittered{};
        FrameMatrices jittered{};
        VkExtent2D internal_extent{};
        VkExtent2D output_extent{};
        TransformTable transforms;
    };

    struct CandidateState {
        TemporalFrame frame{};
        TransformTable transforms;
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
std::uint64_t temporal_instance_id(std::uint64_t source_instance_id,
                                   std::uint64_t part_hash,
                                   std::uint32_t child_ordinal);

}  // namespace viewer
