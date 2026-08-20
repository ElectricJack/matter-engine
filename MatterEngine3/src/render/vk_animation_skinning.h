#pragma once

// MatterEngine3/src/render/vk_animation_skinning.h
//
// The CPU-side GPU-skinning work queue. `VkAnimationSkinning` takes the
// per-frame list of visible skinned submissions, validates it, admits as much
// as the animation budget allows, and publishes one `VkSkinFrameArenas` per
// frame slot: flattened joint palettes, `VkSkinWorkItem`s for
// `shaders_vk/animation_skin.comp`, the output vertex slices they write, the
// indexed raster draws that consume those outputs, and an explicit fallback
// record for everything that did not make it.
//
// Where it sits: `animation_skin_bridge.h` turns ECS animation state into
// `VkSkinSubmission`s; `VkSceneRenderer` (vk_scene_renderer.cpp) drives the
// frame protocol below and does the actual buffer allocation and upload;
// `vk_animation_bounds.h` consumes the resulting `VkSkinRasterDraw`s to
// decide which bind-pose geometry must be suppressed. The shader-visible
// structs come from `vk_animation_types.h`.
//
// Frame protocol, in order, per frame slot:
//   begin_frame(slot, completed_fence)   reclaim the slot; fails while its
//                                        fence, or a fence that depends on
//                                        its retained output, is outstanding
//   submit_visible(slot, visible)        validate, sort, admit, publish
//   reject_gpu_frame(slot, reason)       optional: allocation/upload failed,
//                                        replace current work with fallbacks
//   mark_submitted(slot, fence)          seal the slot against that fence
//
// Considerations:
//  - Owns no Vulkan handles and takes no locks. It is plain CPU state driven
//    from the render thread; it is not internally synchronized.
//  - Fence values form ONE monotonically increasing timeline across all
//    slots, not a per-slot counter.
//  - Publication is transactional. Every validation failure publishes a
//    complete fallback frame (retained last-complete draws or bind pose) --
//    the queue is never left half-built, and callers downstream never see a
//    work item whose source range or palette was not proven in range.
//  - Retained "last complete" draws can point at an OLDER frame slot's output
//    buffer. That is what `source_dependency_fence_` /
//    `pending_source_users_` exist to protect: a producer slot cannot be
//    recycled while a consumer still references its buffers.
//  - Offsets in the arenas are element indices (vertices, joints), not bytes.
#include "animation/animation_budget.h"
#include "vk_animation_types.h"

#include <cstdint>
#include <map>
#include <vector>

namespace viewer {

// CPU-side presentation of one immutable evaluator snapshot.  C2 changes the
// upload implementation, not this lifetime rule: both palette streams are
// copied from this single snapshot before the frame queue is published.
struct VkSkinPose {
    // Flat joint palette for this frame. Its size is the palette/joint count
    // for the submission and is what `VkSkinInfluence::joint` is validated
    // against.
    std::vector<VkSkinJoint> current;
    // Previous frame's palette, used only for the previous-position output
    // that feeds motion vectors. Read only when the submission sets
    // `history_valid`; otherwise the queue substitutes `current`, which makes
    // the motion vector zero.
    std::vector<VkSkinJoint> previous;
};

// One visible skinned cluster offered to the queue for this frame. The
// producer has already done frustum/LOD selection, so the fields here are the
// result of that decision, not a request for it.
//
// `render_priority` (higher first) then `distance_bucket` (nearer first) then
// instance slot then LOD is the admission order under budget pressure -- see
// `less_submission` in vk_animation_skinning.cpp. Anything past the budget is
// not dropped silently: it gets a `VkSkinFallback` and, where a compatible
// retained output exists, a retained raster draw.
struct VkSkinSubmission {
    uint64_t asset_key = 0;
    // Immutable asset-local influence offset.  Source vertices are global to
    // the renderer arena, so they must not be conflated once parts are
    // compacted or multiple assets are resident.
    uint32_t influence_vertex = 0;
    uint32_t source_vertex = 0;
    // Part-LOCAL first vertex of this range. The shared index buffer stores
    // PART-LOCAL index values (matter_engine.cpp rebases each mesh by its
    // offset within the part and no further; vk_scene_renderer never rewrites
    // them), so a skinned draw must subtract exactly this to land on the
    // compute output. Subtracting the renderer-global source_vertex instead
    // over-rebases by the part's arena base -- invisible whenever that base is
    // 0, which is every fixture but not real content.
    uint32_t local_vertex_base = 0;
    uint32_t vertex_count = 0;
    uint32_t instance_slot = 0;
    uint32_t instance_generation = 0;
    int32_t render_priority = 0;
    uint32_t distance_bucket = 0;
    uint32_t lod = 0;
    uint32_t cluster = 0;
    bool current_frustum_visible = true;
    // Global indexed-raster range selected by the visibility producer.  A
    // zero count deliberately means "compute only": it keeps C1 callers
    // source-compatible, but it must never manufacture a raster draw.
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    bool history_valid = false;
    VkSkinPose pose;
};

// One accepted work item has exactly one immutable raster mapping.  The
// output offsets are vertex indices (not bytes); record_raster converts them
// to VkSkinVertex binding offsets.  Keeping this beside the work queue makes
// sorting and fallback selection transactional.
struct VkSkinRasterDraw {
    uint64_t asset_key = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    uint32_t source_vertex = 0;
    // Part-LOCAL first vertex of this range. The shared index buffer stores
    // PART-LOCAL index values (matter_engine.cpp rebases each mesh by its
    // offset within the part and no further; vk_scene_renderer never rewrites
    // them), so a skinned draw must subtract exactly this to land on the
    // compute output. Subtracting the renderer-global source_vertex instead
    // over-rebases by the part's arena base -- invisible whenever that base is
    // 0, which is every fixture but not real content.
    uint32_t local_vertex_base = 0;
    uint32_t output_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t instance_slot = 0;
    uint32_t instance_generation = 0;
    // Selects the fence-owned frame output buffer. Current work points at the
    // frame being built; retained fallbacks may point at an older, still
    // sealed frame slot.
    uint32_t output_frame_slot = 0;
    uint32_t lod = 0;
    uint32_t cluster = 0;
    uint32_t flags = 0;
};

// A half-open range of vertices inside a frame's skinned output arena.
// `offset` and `count` are vertex counts, not bytes.
struct VkSkinArenaSlice {
    uint32_t offset = 0;
    uint32_t count = 0;
};

// C1 deliberately leaves the actual raster-instance fallback choice to C2;
// this record makes rejection visible and deterministic without emitting a
// partially valid compute work item.
// How a rejected instance is to be drawn instead.
//   LastCompletePose  a still-live skinned output from an earlier frame slot
//                     matches this instance exactly, so keep drawing it.
//   BindPose          nothing usable is retained; fall back to the
//                     unskinned bind-pose geometry.
enum class VkSkinFallbackMode : uint8_t { LastCompletePose, BindPose };
// Which renderer-side GPU step failed, as reported to `reject_gpu_frame`.
// `None` is not a legal argument there -- it is the "no failure" state of
// `VkSkinFrameArenas::gpu_failure`.
enum class VkSkinGpuFailureReason : uint8_t { None, Allocation, Upload };
struct VkSkinFallback {
    uint32_t instance_slot = 0;
    uint32_t instance_generation = 0;
    VkSkinFallbackMode mode = VkSkinFallbackMode::BindPose;
    matter::animation::AnimationFallbackReason reason =
        matter::animation::AnimationFallbackReason::None;
};

// Everything one frame slot publishes, replaced wholesale by each
// `submit_visible` / `reject_gpu_frame`. The renderer uploads
// `palette_current`, `palette_previous` and `work_items`, dispatches the skin
// compute shader over them, then draws `raster_draws`.
//
// `palette_previous` always has the same length as `palette_current`: when a
// submission has no valid history the current palette is duplicated into it.
// `current_output` and `previous_output` are per-work-item slices of the
// frame's skinned vertex arena, in vertices.
//
// `in_flight` plus `submitted_fence` are set by `mark_submitted` and are what
// stop `begin_frame` from recycling the slot early.
struct VkSkinFrameArenas {
    std::vector<VkSkinJoint> palette_current;
    std::vector<VkSkinJoint> palette_previous;
    std::vector<VkSkinWorkItem> work_items;
    // CPU-only identity sidecar for transactional record-time degradation;
    // WorkItem stays at the shipped 32-byte shader ABI.
    std::vector<uint32_t> work_instance_generations;
    std::vector<VkSkinArenaSlice> current_output;
    std::vector<VkSkinArenaSlice> previous_output;
    std::vector<VkSkinRasterDraw> raster_draws;
    std::vector<VkSkinFallback> fallbacks;
    uint32_t current_output_vertices = 0;
    uint32_t previous_output_vertices = 0;
    uint64_t submitted_fence = 0;
    bool in_flight = false;
    // Record-time GPU resource failures are converted atomically to this
    // frame's retained/bind fallback before culling observes the queue.
    VkSkinGpuFailureReason gpu_failure = VkSkinGpuFailureReason::None;
};

// Owns no Vulkan resources in C1. It establishes the allocation/lifetime
// transaction that C2 maps to device-local buffers. Frame slots may only be
// reset after their submitted fence has completed.
class VkAnimationSkinning {
public:
    explicit VkAnimationSkinning(
        uint32_t frame_slots = 3,
        matter::animation::AnimationBudgetConfig budget = {});

    // Appends the asset's influences to the immutable packed arena, which
    // only ever grows for the lifetime of this object. Returns false for a
    // zero key, an empty list, an over-budget asset count, or a re-register
    // of an existing key whose influences are not byte-identical; a
    // byte-identical re-register succeeds and is a no-op.
    bool register_asset(uint64_t asset_key,
                        const std::vector<VkSkinInfluence>& influences);
    // Reclaims a frame slot and clears its arenas. Returns false -- leaving
    // the slot untouched -- while the slot's own fence is still outstanding,
    // while a later frame's fence still depends on output retained from this
    // slot, or while an unsealed consumer references it. Succeeding also
    // retires every `retained_outputs_` entry pointing at this slot, since
    // those buffers are about to be overwritten.
    bool begin_frame(uint32_t frame_slot, uint64_t completed_fence);
    // Validates, sorts, budget-admits and publishes the frame's queue.
    // Submissions with `current_frustum_visible == false` are dropped up
    // front and get no work, output slice or raster draw.
    //
    // Returning false does NOT mean "nothing happened": a malformed
    // submission replaces the whole slot with a fallback-only frame (retained
    // last-complete draws where possible, bind pose otherwise) so no partially
    // valid work can reach the GPU. False is also returned for an
    // out-of-range slot or a slot already sealed by `mark_submitted`.
    bool submit_visible(uint32_t frame_slot,
                        const std::vector<VkSkinSubmission>& visible);
    // Frame slots are sealed exactly once.  A rejected seal leaves the
    // in-flight fence untouched, so a stale caller cannot make an active
    // arena appear reusable.
    bool mark_submitted(uint32_t frame_slot, uint64_t fence);
    // The renderer calls this when allocation/upload of a newly published
    // queue fails. It replaces all current work with only complete retained
    // slices (or bind pose) before the frame is sealed.
    bool reject_gpu_frame(uint32_t frame_slot, VkSkinGpuFailureReason reason);

    // An out-of-range slot returns a shared static empty arena rather than
    // failing, so callers can read it unconditionally.
    const VkSkinFrameArenas& frame(uint32_t frame_slot) const;
    // Immutable packed influence arena. WorkItem::influence indexes this
    // buffer directly, so the renderer can upload it once per asset revision.
    const std::vector<VkSkinInfluence>& influences() const noexcept {
        return influence_arena_;
    }
    uint32_t fallback_count() const noexcept { return fallback_count_; }
    uint64_t gpu_failure_count() const noexcept { return gpu_failure_count_; }
    uint64_t gpu_allocation_failure_count() const noexcept {
        return gpu_allocation_failure_count_;
    }
    uint64_t gpu_upload_failure_count() const noexcept {
        return gpu_upload_failure_count_;
    }
    const matter::animation::AnimationBudgetRuntimeStats& stats() const noexcept {
        return stats_;
    }
    const matter::animation::AnimationBudgetConfig& budget_config() const noexcept {
        return budget_;
    }

private:
    struct AssetInfluences {
        std::vector<VkSkinInfluence> values;
        uint32_t offset = 0;
    };
    std::map<uint64_t, AssetInfluences> assets_;
    // The last successfully sealed skinned output for one (slot, generation),
    // keyed by `instance_key`. A retained draw is only reused when every
    // geometry field below still matches the new submission exactly -- same
    // asset, LOD, cluster, source range and index range -- because the
    // retained vertices were skinned for that exact geometry.
    struct RetainedOutput {
        uint64_t asset_key = 0;
        uint32_t lod = 0;
        uint32_t source_vertex = 0;
        uint32_t local_vertex_base = 0;
        uint32_t vertex_count = 0;
        uint32_t first_index = 0;
        uint32_t index_count = 0;
        uint32_t cluster = 0;
        uint32_t output_vertex = 0;
        uint32_t output_frame_slot = 0;
    };
    std::map<uint64_t, RetainedOutput> retained_outputs_;
    std::vector<VkSkinInfluence> influence_arena_;
    std::vector<VkSkinFrameArenas> frames_;
    // Bounded by frames-in-flight. A producer slot cannot be recycled while
    // an unsealed consumer references it or until every sealed consumer of
    // one of its retained slices has completed.
    std::vector<uint64_t> source_dependency_fence_;
    std::vector<uint32_t> pending_source_users_;
    std::vector<std::vector<uint32_t>> pending_source_dependencies_;
    uint32_t fallback_count_ = 0;
    uint64_t gpu_failure_count_ = 0;
    uint64_t gpu_allocation_failure_count_ = 0;
    uint64_t gpu_upload_failure_count_ = 0;
    matter::animation::AnimationBudgetConfig budget_;
    matter::animation::AnimationBudgetRuntimeStats stats_;
    uint64_t last_submitted_fence_ = 0;
    bool has_submitted_fence_ = false;

    static bool identical(const std::vector<VkSkinInfluence>& a,
                          const std::vector<VkSkinInfluence>& b) noexcept;
    static uint64_t instance_key(uint32_t slot, uint32_t generation) noexcept;
    void release_pending_dependencies(uint32_t frame_slot) noexcept;
};

}  // namespace viewer
