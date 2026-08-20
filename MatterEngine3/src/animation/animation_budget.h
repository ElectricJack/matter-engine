// Central, renderer-neutral animation budgets and cosmetic presentation LOD.
// Fixed simulation is intentionally absent: fixed graph clocks, controllers,
// markers, and root motion always run at the simulation cadence.
#pragma once

// MatterEngine3/src/animation/animation_budget.h
//
// How it fits
// -----------
// This is the leaf of the animation tree: it depends on nothing but the
// standard library, and `animation_ir.h`, `animation_evaluator.h` and the
// service layer all pull their hard caps (`kMaxJoints`, `kMaxGraphNodes`,
// `kMaxControllers`) from `AnimationBudgetConfig`'s `kHardMax*` constants so
// there is exactly one place where a limit is declared.
//
// Two separate things live here:
//
// - `AnimationBudgetConfig` / `AnimationBudgetRuntimeStats` -- admission
//   control and counters. The evaluator refuses work that would push it past
//   the configured limits and records why in `AnimationFallbackReason`.
// - `PoseLodScheduler` -- the *cosmetic* refresh-rate policy. It decides on
//   which render frames an instance's pose is re-evaluated (60/30/15 Hz or
//   frozen), never what the simulation does.
//
// Conventions
// -----------
// - Distances (`near_distance`, `mid_distance`, `frozen_distance`,
//   `distance_hysteresis`) are world-space metres from the viewer.
// - `presentation_seconds` is a monotonic wall clock in seconds, not a frame
//   count; the scheduler's cadence is therefore independent of the render
//   frame rate.
// - `instance_key` is the caller's opaque instance identity. Zero is reserved
//   as "no instance" and is rejected by `PoseLodScheduler::schedule`.
//
// Gotchas
// -------
// - Nothing here takes a lock. `PoseLodScheduler` mutates a `std::map` on
//   every `schedule()` call, so one scheduler belongs to one thread.
// - `PoseLodScheduler` grows an entry per distinct `instance_key` it has ever
//   seen and never expires them; callers must call `forget()` when an
//   instance dies or the map leaks for the lifetime of the scheduler.
// - Budget/LOD state is deliberately absent from checkpoints: it is a
//   presentation concern, so a play/stop transaction does not restore it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

namespace matter::animation {

// Why a requested pose was not produced. Recorded by
// `AnimationBudgetRuntimeStats::record_fallback`; `Count` is the array bound,
// not a reason, and `None` is never recorded.
enum class AnimationFallbackReason : uint8_t {
    None,
    AssetLimit,
    RuntimeInstanceLimit,
    EvaluationBudget,
    SkinWorkBudget,
    SkinVertexBudget,
    InvalidSkinSubmission,
    InvalidEvaluationRequest,
    EvaluationFailure,
    Count
};

// Presentation refresh rate for one instance. The enumerator ORDER is load
// bearing: `PoseLodScheduler` compares the underlying integers to detect a
// move toward a *finer* tier (a smaller value) and only then asks for a graph
// resample. `Frozen` means "hold the last published pose indefinitely" and its
// evaluation interval is +infinity.
enum class AnimationPoseLodTier : uint8_t { Hz60, Hz30, Hz15, Frozen };
// Which pose a consumer should draw when the freshest one is unusable, in
// descending order of fidelity. See `select_pose_fallback`.
enum class AnimationPoseSource : uint8_t { Current, LastComplete, BindPose };

// What the caller actually has on hand for one instance this frame. Fed to
// `select_pose_fallback` to pick a source without the policy being restated
// at each call site.
struct AnimationPoseAvailability {
    bool current_complete = false;
    bool last_complete = false;
};

// This is deliberately a build-once policy rather than a Vulkan flag type.
// It prevents the animation layer from ever widening the RT contract toward
// BLAS update/refit while raster deformation remains exact.
struct SkinnedRtBuildContract {
    bool build_once = true;
    bool allow_update = false;
    bool allow_refit = false;
};

// Configured animation limits, plus the compile-time ceilings they are
// validated against.
//
// The `kHardMax*` constants are the absolute ceiling of the whole animation
// subsystem and are referenced directly from `animation_ir.h`; the non-static
// fields are the *tunable* limits, defaulted below the ceiling. `valid()`
// enforces `field <= kHardMax` for every pair, so a config can only ever
// tighten policy, never widen it beyond what the code was sized for.
//
// A default-constructed instance is always valid, which is why the
// constructors of `PoseLodScheduler` and `AnimationEvaluator` fall back to
// `{}` on an invalid config rather than failing.
struct AnimationBudgetConfig {
    static constexpr uint32_t kHardMaxAssets = 4096;
    static constexpr uint32_t kHardMaxRuntimeInstances = 4096;
    static constexpr uint32_t kHardMaxJointsPerAsset = 256;
    static constexpr uint32_t kHardMaxGraphNodes = 128;
    static constexpr uint32_t kHardMaxControllerNodes = 128;
    static constexpr uint32_t kHardMaxSkinWorkItems = 256;
    static constexpr uint32_t kHardMaxSkinnedVertices = 2'000'000;
    static constexpr uint32_t kHardMaxEvaluatedJointsPerFrame = 1'000'000;
    static constexpr uint32_t kHardMaxWorldQueriesPerFixedTick = 2048;
    static constexpr size_t kHardMaxMutableBytes = 64u * 1024u * 1024u;

    uint32_t max_assets = 1024;
    uint32_t max_runtime_instances = 4096;
    uint32_t max_joints_per_asset = 256;
    uint32_t max_graph_nodes = 128;
    uint32_t max_controller_nodes = 128;
    uint32_t max_skin_work_items = 256;
    uint32_t max_skinned_vertices = 2'000'000;
    uint32_t max_evaluated_joints_per_frame = 65'536;
    uint32_t max_world_queries_per_fixed_tick = 2'048;
    size_t max_mutable_bytes = 64u * 1024u * 1024u;

    // Distance bands select 60/30/15/frozen, in that order. A stateful
    // selector applies distance_hysteresis on both sides of every boundary.
    float near_distance = 12.0f;
    float mid_distance = 35.0f;
    float frozen_distance = 120.0f;
    float distance_hysteresis = 2.0f;

    // True when every tunable is non-zero (except `max_controller_nodes`,
    // which may be zero to forbid native controllers outright), within its
    // `kHardMax` ceiling, and the distance bands are finite and strictly
    // ascending (`0 <= near < mid < frozen`).
    bool valid() const noexcept;
};

// The public AnimationService already exposes a compact allocation snapshot as
// `matter::AnimationRuntimeStats`. This detailed private/runtime view augments
// it with evaluator and renderer counters without widening that stable API.
struct AnimationBudgetRuntimeStats {
    uint64_t evaluated_pose_count = 0;
    uint64_t evaluated_joint_count = 0;
    uint64_t submitted_skin_work_items = 0;
    uint64_t submitted_skinned_vertices = 0;
    uint64_t evaluated_presentation_pose_count = 0;
    uint64_t frozen_pose_count = 0;
    uint64_t resampled_pose_count = 0;
    uint64_t last_complete_fallback_count = 0;
    uint64_t bind_pose_fallback_count = 0;
    uint64_t fallback_count = 0;
    std::array<uint64_t, static_cast<size_t>(AnimationFallbackReason::Count)> fallbacks{};

    // Bumps both `fallback_count` and the per-reason bucket. `None` and
    // out-of-range reasons are silently ignored rather than counted.
    void record_fallback(AnimationFallbackReason reason) noexcept;
    // Adds every counter of `other` into this one, for folding per-worker
    // stats into a frame total. Not idempotent -- merging the same source
    // twice double counts.
    void merge(const AnimationBudgetRuntimeStats& other) noexcept;
};

// One instance's presentation state for a single render frame.
//
// `frame_serial` is the de-duplication key: the scheduler returns its cached
// decision unchanged if it is asked twice for the same instance within one
// frame serial, so several consumers may query it without perturbing the
// cadence. `distance` is metres from the viewer; a negative or non-finite
// distance is treated as `Frozen`.
struct PoseLodRequest {
    uint64_t instance_key = 0;
    bool visible = false;
    float distance = 0.0f;
    int32_t explicit_priority = 0;
    // Monotonic presentation time. This deliberately is not a render-frame
    // divisor, so a 30 Hz render loop and a 144 Hz render loop agree.
    double presentation_seconds = 0.0;
    uint64_t frame_serial = 0;
};

// The scheduler's answer for one frame.
//
// - `evaluate_now` -- this frame is on or past the tier's deadline; re-solve.
// - `newly_visible` -- the instance is inside the short forced-60 Hz grace
//   window that follows a visibility transition.
// - `resample_current_graph_time` -- the pose became visible, or moved to a
//   finer tier, so the held sample is stale relative to the graph clock and
//   should be re-sampled rather than interpolated forward.
struct PoseLodDecision {
    AnimationPoseLodTier tier = AnimationPoseLodTier::Frozen;
    bool evaluate_now = false;
    bool newly_visible = false;
    bool resample_current_graph_time = false;
};

// Stateful, per-instance presentation-rate selector.
//
// Owned by whoever drives presentation (one per consumer); it holds no engine
// or GPU resources and is destroyed with its owner. All state is the
// `states_` map, so it is safe to copy-construct but not to share across
// threads -- every call mutates.
//
// Behaviour worth knowing before reading the implementation:
// - Tier selection is hysteretic in both directions (`distance_hysteresis`
//   metres on each side of every band boundary) so an instance hovering on a
//   boundary does not oscillate.
// - Becoming visible forces two frames at `Hz60` regardless of distance; the
//   raw distance band is then latched and applied on the frame the grace
//   expires, so a distant newly visible instance does not inherit 60 Hz
//   hysteresis.
// - Deadlines are carried forward from the previous deadline, not rebased on
//   "now", so a render loop at almost exactly the tier rate does not halve
//   the effective refresh rate. See the comment in
//   `PoseLodScheduler::schedule`.
class PoseLodScheduler {
public:
    explicit PoseLodScheduler(AnimationBudgetConfig config = {});
    static size_t state_mutable_bytes() noexcept;
    // Advances (or replays) the decision for one instance. Mutating: it
    // creates the instance's state on first sight and updates its tier and
    // deadline. Calling it twice with the same `frame_serial` returns the
    // cached decision and changes nothing. A zero `instance_key` or a
    // non-finite/negative `presentation_seconds` yields a default-constructed
    // (Frozen, do-not-evaluate) decision.
    PoseLodDecision schedule(const PoseLodRequest& request);
    // Drops all tracking for an instance. Must be called when an instance
    // dies: nothing else ever removes an entry.
    void forget(uint64_t instance_key) noexcept;

private:
    struct State {
        AnimationPoseLodTier tier = AnimationPoseLodTier::Frozen;
        AnimationPoseLodTier post_grace_tier = AnimationPoseLodTier::Frozen;
        bool visible = false;
        bool has_post_grace_tier = false;
        uint8_t visible_grace_frames = 0;
        double next_due_seconds = 0.0;
        uint64_t last_frame_serial = UINT64_MAX;
        PoseLodDecision last_decision{};
    };

    AnimationPoseLodTier select_tier(AnimationPoseLodTier previous,
                                     float distance) const noexcept;
    static double interval(AnimationPoseLodTier tier) noexcept;

    AnimationBudgetConfig config_;
    std::map<uint64_t, State> states_;
};

// The single fallback policy: freshest complete pose, else the last complete
// one, else the bind pose. Pure; it exists so no consumer invents its own
// ordering.
AnimationPoseSource select_pose_fallback(AnimationPoseAvailability availability) noexcept;
// The process-wide, immutable build contract for skinned ray-tracing geometry.
// Returns a reference to a function-local static, so it is safe from any
// thread and lives for the whole process.
const SkinnedRtBuildContract& skinned_rt_build_contract() noexcept;

}  // namespace matter::animation
