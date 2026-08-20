#pragma once

// MatterEngine3/src/render/vk_temporal.h
//
// The renderer's temporal bookkeeping: what the *previous presented* frame
// looked like, so this frame can reproject against it.
//
// Two independent pieces live here.
//
// `TemporalState` is the one the renderer actually runs every frame. It keeps
// the previous presented frame's camera matrices and a table of every
// instance's object-to-world transform, and `begin()` pairs each incoming
// instance with its previous transform. `temporal_velocity_pixels()` turns
// that pair into a screen-space motion vector, which is what DLSS and the
// volumetric/GI reprojection consume.
//
// `GiTemporalState` is a CPU mirror of the accept/reject rules in
// `shaders_vk/gi_temporal.comp`, kept so those rules can be pinned down in a
// headless test. It tracks ONE pixel, not a full image -- it is a contract
// reference, not a second implementation of the pass.
//
// Attempt/commit protocol (both classes)
// --------------------------------------
// A frame is speculative until it is presented, because a swapchain acquire or
// submit can fail and the frame is then re-rendered. So:
//   1. `begin()` / `accumulate()` produces a candidate and stamps it with an
//      `attempt_token`.
//   2. If the frame reaches the screen, `commit_presented(token)` promotes the
//      candidate to the new history.
//   3. If it does not, `discard_failed_attempt(token)` drops it.
// Both return false when the token does not match the outstanding candidate,
// which is how a stale completion is ignored rather than corrupting history.
// Producing a second candidate before committing the first forces the next
// frame to reset -- history from a frame nobody saw is not trustworthy.
// `invalidate()` drops everything and forces a reset (camera cut, world
// reload, renderer rebuild).
//
// Conventions
// -----------
// - Matrices are `matter::Mat4f`, ROW-major on the CPU side.
// - Velocities and jitter are in PIXELS of `internal_extent` (the pre-upscale
//   render target), Y-down screen convention.
// - Instance ids are opaque 64-bit hashes from `temporal_instance_id()`; 0 is
//   never a valid id.
// - Single-threaded. Nothing here takes a lock; drive it from the render
//   thread only.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "frame_matrices.h"
#include "matter/math_types.h"

namespace viewer {

// One drawable instance as the renderer hands it to `TemporalState::begin()`.
// The id must be stable across frames for the same logical instance, otherwise
// its history is lost and it renders without motion vectors for a frame.
struct TemporalInstance {
    std::uint64_t instance_id = 0;
    matter::Mat4f object_to_world{};
};

// The resolved form of a `TemporalInstance` for one frame: this frame's
// transform paired with the one from the previous presented frame.
//
// When `history_valid` is false -- a newly streamed instance, or a frame with
// `TemporalFrame::reset` set -- `previous_object_to_world` is a COPY of
// `current_object_to_world`, so consumers that ignore the flag see zero motion
// rather than garbage.
struct TemporalInstanceFrame {
    std::uint64_t instance_id = 0;
    matter::Mat4f current_object_to_world{};
    matter::Mat4f previous_object_to_world{};
    bool history_valid = false;
};

// Reasons the caller already knows history cannot be reprojected. Any one of
// them makes the next frame a full reset. Note that an instance simply being
// NEW is deliberately not on this list -- see the comment in
// `TemporalState::begin`.
struct TemporalInvalidation {
    bool camera_cut = false;      // teleport / cut, no continuity to reproject
    bool world_reload = false;    // a different world is being rendered
    bool renderer_reset = false;  // swapchain or renderer rebuilt
};

// Everything a frame needs to know about its own temporal situation, produced
// by `TemporalState::begin()`.
//
// Both a jittered and an unjittered matrix set are carried because they answer
// different questions: rasterisation and motion vectors use the jittered pair
// (that is what was actually rendered), while anything reasoning about the
// true camera -- culling, world-space reconstruction -- wants the unjittered
// pair. On a reset frame the "previous" sets are copies of the "current" ones,
// so reprojection degenerates to zero motion instead of reading stale data.
//
// The returned reference is owned by `TemporalState` and is invalidated by the
// next `begin()`.
struct TemporalFrame {
    FrameMatrices current_unjittered{};
    FrameMatrices previous_unjittered{};
    FrameMatrices current_jittered{};
    FrameMatrices previous_jittered{};
    // One entry per instance passed to begin(), in the SAME order.
    std::vector<TemporalInstanceFrame> instances;
    VkExtent2D internal_extent{};   // pixels rendered before upscaling
    VkExtent2D output_extent{};     // pixels presented after upscaling
    // Sub-pixel camera jitter applied this frame, in internal_extent pixels,
    // Y-DOWN (what DLSS expects). Roughly [-0.5, 0.5); zero when jitter is off.
    float jitter_pixels[2]{};
    // No usable history this frame: consumers must not reproject. Defaults to
    // true so a default-constructed frame is safe.
    bool reset = true;
    std::uint64_t attempt_token = 0;
    // Count of frames that were successfully presented before this candidate.
    // Failed/retried attempts therefore keep the same stochastic frame seed.
    std::uint64_t presented_frame_index = 0;
};

// Why a GI history sample was refused. A bit mask by declaration, but
// `accumulate()` returns on the FIRST failing test, so in practice exactly one
// bit is ever set (or zero, meaning the history was accepted). The order of
// the tests is the order of the values below.
enum GiTemporalRejection : std::uint32_t {
    kGiRejectBounds = 1u << 0,    // reprojected pixel left the image / moved
    kGiRejectDepth = 1u << 1,     // depth discontinuity beyond tolerance
    kGiRejectNormal = 1u << 2,    // normals diverge by more than ~32 degrees
    kGiRejectMaterial = 1u << 3,  // identity attachment .x differs
    kGiRejectInstance = 1u << 4,  // a different instance now covers the pixel
    kGiRejectReset = 1u << 5,     // no history at all (first frame, cut, resize)
};

struct GiPixelCoord {
    int x = 0;
    int y = 0;
};

struct GiTemporalSurface {
    matter::Float3 radiance{};
    float depth = 1.0f;
    matter::Float3 normal{};
    // The identity attachment's .x AS WRITTEN, impostor bit and all -- not a
    // masked material index, despite the name. Nothing here ever indexes the
    // material table; accumulate() only compares this against the history's
    // copy, the same equality gi_temporal.comp does, so masking would be
    // wrong as well as unnecessary: it would merge a mesh rung and its
    // impostor card into one surface and accumulate GI across the switch.
    std::uint32_t material_index = UINT32_MAX;
    std::uint32_t instance_token = UINT32_MAX;
};

// The accumulated result for one pixel. On rejection every field describes the
// current sample alone (history_length 1, moments from this frame's
// luminance), which is exactly the behaviour a rejecting shader lane wants.
struct GiTemporalResult {
    matter::Float3 radiance{};           // blended radiance, linear
    // Running mean of luminance and of luminance squared, blended with the
    // same alpha as `radiance`. Their difference is the variance estimate the
    // denoiser drives its filter width from.
    float first_moment = 0.0f;
    float second_moment = 0.0f;
    // Frames accumulated, capped at 32. Drives alpha = max(1/length, 0.05),
    // so the effective blend weight bottoms out at 0.05 (~20 frames) even
    // though the counter keeps reporting up to 32.
    std::uint32_t history_length = 1;
    std::uint32_t rejection_bits = kGiRejectReset;  // GiTemporalRejection bits
    // Where this pixel was in the previous frame: pixel - velocity, rounded to
    // the nearest texel. May be outside the image (then kGiRejectBounds).
    GiPixelCoord previous_pixel{};
};

// CPU mirror of gi_temporal.comp's candidate/commit rules. Besides making the
// shader contract deterministic in tests, this owns the presentation token
// semantics used to select the renderer's ping-pong history set.
class GiTemporalState {
public:
    // Produce this frame's candidate for ONE pixel. `velocity_pixels` is the
    // screen-space motion vector in `extent` pixels; `pixel` is the pixel being
    // shaded. `reset` forces rejection outright.
    //
    // Only one pixel of state exists, so each call REPLACES the outstanding
    // candidate -- and calling it twice before a commit sets the internal
    // force-reset flag, because a candidate that was never presented cannot be
    // valid history. Tests therefore drive one pixel per attempt.
    GiTemporalResult accumulate(const GiTemporalSurface& current,
                                matter::Float3 velocity_pixels,
                                VkExtent2D extent, GiPixelCoord pixel,
                                bool reset, std::uint64_t attempt_token);
    // Promote the candidate stamped with `attempt_token` to history and flip
    // `presented_index()`. False means the token did not match the outstanding
    // candidate and nothing changed.
    bool commit_presented(std::uint64_t attempt_token);
    bool discard_failed_attempt(std::uint64_t attempt_token);
    // Drop the candidate and force the next accumulate() to reject. Use on a
    // camera cut, world reload or renderer rebuild.
    void invalidate() noexcept;
    // Which of the renderer's two ping-pong GI history image sets currently
    // holds the presented result. Flipped by commit_presented(), so a failed
    // frame leaves the renderer pointing at the same set it read from.
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

// Per-instance transform history and camera history for the render thread.
//
// Lifetime: one instance owned by the renderer, alive for the renderer's
// lifetime. Not copyable in practice (it holds multi-megabyte vectors) and not
// thread-safe -- render thread only.
//
// Call order per frame: `begin(...)`, then exactly one of
// `commit_presented(token)` (the frame was presented) or
// `discard_failed_attempt(token)` (it was not), using
// `TemporalFrame::attempt_token`. Skipping the resolution step is not fatal but
// costs a reset: the next `begin()` sees an unresolved candidate and forces
// `TemporalFrame::reset`.
//
// Cost: `begin()` is O(instances) and touches several megabytes at streaming
// scale (~90k instances). The storage-recycling and cursor-lookup schemes
// documented on the private members below exist to keep that from dominating
// the frame; both have `MATTER_VK_TEMPORAL_*` env kill switches in the .cpp.
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
    // Promote the candidate to history: its matrices become "previous" and its
    // transform table becomes the lookup source for the next begin(). False
    // means the token did not match and nothing changed.
    bool commit_presented(std::uint64_t attempt_token);
    // The frame was not presented. Drops the candidate and forces the next
    // frame to reset, since the transforms it reported were never seen.
    bool discard_failed_attempt(std::uint64_t attempt_token);
    // Forget all history and force the next frame to reset. The presented
    // transform table is NOT freed, only bypassed.
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
        // Entry index for `id`, or -1. find() is the same lookup returning the
        // value; begin()'s cursor path needs the index so it can resync.
        std::int32_t find_index(std::uint64_t id) const;
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
    // Retired candidate storage, recycled by begin().
    //
    // Perf: begin() used to build into a fresh `CandidateState next{}` and
    // move it in. At ~90k streaming instances that is ~6.5 MB of (ids, values)
    // plus ~13 MB of TemporalInstanceFrame allocated and released EVERY frame,
    // and on Windows a large block is decommitted on free, so the next frame
    // also takes a soft page fault per 4 KB of the fresh block. Nothing about
    // the candidate's storage is per-frame state, so it cycles here instead:
    // begin() fills `spare_` and swaps it into `candidate_`, and
    // commit_presented() swaps (rather than moves) the transform table into
    // `presented_`, which hands the retired presented table back. Every field
    // that CandidateState{} zero-initialised is reset explicitly in begin().
    CandidateState spare_{};
    bool has_presented_ = false;
    bool has_candidate_ = false;
    bool force_reset_ = true;
    std::uint64_t presented_frame_index_ = 0;
    std::uint64_t next_attempt_token_ = 1;
};

// Screen-space motion of one object-space point on one instance, in pixels of
// `frame.internal_extent`, using the JITTERED matrix pair (x right, y down,
// z always 0).
//
// Returns {0,0,0} for every "no answer" case -- reset frame, unknown instance,
// no valid history, zero extent, or a point behind the camera (w == 0) -- so a
// zero result is not distinguishable from genuinely zero motion.
//
// Cost: this does a LINEAR SEARCH over `frame.instances`. It is fine for a
// handful of probe points; calling it per instance is O(n^2). Walk
// `frame.instances` directly instead.
matter::Float3 temporal_velocity_pixels(const TemporalFrame& frame,
                                        std::uint64_t instance_id,
                                        matter::Float3 local_position);

// Derive the stable per-frame instance id from the pieces that identify a
// drawable: the source instance, the part it draws, and which child of that
// part it is. FNV-1a over the three, never returning 0 (0 is the "unset" id).
//
// Stability is the whole point: the same logical instance must hash the same
// every frame or it loses its history. A part republished under a new hash is
// intentionally a NEW id, because its geometry changed and reprojecting the
// old transform onto it would be wrong.
std::uint64_t temporal_instance_id(std::uint64_t source_instance_id,
                                   std::uint64_t part_hash,
                                   std::uint32_t child_ordinal);

}  // namespace viewer
