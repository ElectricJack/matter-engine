#pragma once

#include "../sector_streamer.h"
#include "matter/streaming.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace matter::streaming::detail {

struct AnchorSample {
    flecs::entity_t owner = 0;
    uint64_t generation = 0;
    float x = 0.0f;
    // Carried, forwarded to SectorStreamer::update, and read by nothing
    // (volumetric-sectors M1). Selection is still an XZ problem; M3's octree
    // is what starts consuming this. See SectorStreamer::update.
    float y = 0.0f;
    float z = 0.0f;
};

struct TaggedRequest {
    flecs::entity_t owner = 0;
    uint64_t generation = 0;
    uint64_t issuance = 0;
    matter_stream::SectorRequest sector{};
};

struct TaggedEviction {
    flecs::entity_t owner = 0;
    uint64_t generation = 0;
    uint64_t issuance = 0;
    matter_stream::Eviction sector{};
};

enum class EvictionTransferStage : uint8_t {
    StreamerToCoordinatorReserve,
    CoordinatorToPendingBatchReserve
};

using EvictionTransferFault = void (*)(void*, EvictionTransferStage);

enum class IdleWorkerFailure : uint8_t {
    OutOfMemory,
    Internal
};

using IdleWorkerStep = void (*)(void*);
using IdleWorkerFailureHandler =
    void (*)(void*, IdleWorkerFailure, const char*);

void run_idle_worker_step_noexcept(
    void* step_context,
    IdleWorkerStep step,
    void* failure_context,
    IdleWorkerFailureHandler failure_handler) noexcept;

enum class RequestTrackingStage : uint8_t {
    IssuedRequest,
    PublicationCandidate
};

using RequestTrackingFault = void (*)(void*, RequestTrackingStage);

struct Snapshot {
    flecs::entity_t owner = 0;
    SectorStreamingStatus status{};
    SectorStreamingError error{};
};

// Fixed, non-allocating admission claims shared by WorldSession and focused
// lifecycle tests. The session serializes calls with its completion mutex and
// must acquire a claim before asking Coordinator for another request.
class PublicationCompletionCapacity {
public:
    // A sector holds a claim from dispatch until its publication is
    // acknowledged, so this is a hard ceiling on max_inflight and therefore on
    // fill throughput (Little's law). 32 bound the pool at ~12 executors; the
    // array is bools plus a fixed PublicationCompletion each, so headroom here
    // is cheap.
    static constexpr size_t kCapacity = 128;

    bool try_reserve(size_t& slot) noexcept;
    void release(size_t slot) noexcept;
    void clear() noexcept;
    bool empty() const noexcept;
    bool full() const noexcept;
    size_t size() const noexcept;

private:
    std::array<bool, kCapacity> occupied_{};
    size_t size_ = 0;
};

// Thread-safe session-owned retention for coordinator evictions after they
// cross take_evictions(). The endpoint sees a value snapshot; tags are erased
// only after that complete snapshot applies successfully.
class PendingEvictionBatch {
public:
    using Endpoint = std::function<bool(
        const std::vector<TaggedEviction>&, std::string&)>;

    bool append(
        const std::vector<TaggedEviction>& evictions,
        void* fault_context = nullptr,
        EvictionTransferFault fault = nullptr) noexcept;
    bool apply(const Endpoint& endpoint, std::string& error) noexcept;
    bool apply_tag(
        const TaggedEviction& eviction,
        const Endpoint& endpoint,
        std::string& error) noexcept;
    void abandon_noexcept() noexcept;
    bool empty() const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<TaggedEviction> pending_;
};

// Resource-attempt flags are set before each potentially partial app/GPU call.
// The matching eviction helper clears a flag only after its release succeeds,
// which makes a retained tagged rollback idempotent on retry.
struct PublicationResources {
    bool transient_artifact = false;
    bool store_attempted = false;
    bool world_state_attempted = false;
    bool culler_attempted = false;
    bool vulkan_attempted = false;
};

// One guard owns rollback-before-false-ack ordering after begin_publication.
class PublicationTransaction {
public:
    using Rollback = bool (*)(void*, std::string&) noexcept;
    using Acknowledge = bool (*)(void*, bool) noexcept;

    PublicationTransaction(
        void* context,
        Rollback rollback,
        Acknowledge acknowledge) noexcept;
    ~PublicationTransaction() noexcept;
    PublicationTransaction(const PublicationTransaction&) = delete;
    PublicationTransaction& operator=(const PublicationTransaction&) = delete;

    bool fail(std::string& error) noexcept;
    bool commit() noexcept;
    bool active() const noexcept;

private:
    void* context_ = nullptr;
    Rollback rollback_;
    Acknowledge acknowledge_;
    bool active_ = true;
    bool rollback_complete_ = false;
    bool publish_intent_ = false;
};

class Coordinator {
public:
    bool attach(flecs::entity_t owner);
    flecs::entity_t intended_owner() const;
    void set_profile(
        const matter_stream::Config* profile,
        SectorStreamingErrorCode profile_error =
            SectorStreamingErrorCode::None);
    void submit_anchor(flecs::entity_t owner, float x, float y, float z);

    // ---- OCCLUSION FEEDBACK (M4 Phase A, design §5.2) ----------------------
    //
    // Report the tiles the renderer actually drew this frame. The streamer
    // folds them into request priority and a coarser detail cap, and into
    // nothing else -- it can never remove coverage, which is what makes a
    // fenced, several-frames-stale readback a safe input (see
    // SectorStreamer::mark_visible).
    //
    // QUEUED, not applied inline, for the reason every other input here is:
    // the streamer lives on the worker and is touched only by worker_step.
    // The render thread calls this; the worker drains it. A direct call would
    // be a data race on the sector map in the one subsystem whose bugs are
    // hardest to reproduce.
    //
    // The batch REPLACES rather than accumulates: a frame's visible set is a
    // complete answer, and merging two frames' answers would make a tile look
    // visible for as long as any frame in the window saw it -- which is a
    // longer grace than the one the config names, arrived at by accident.
    void submit_visible(flecs::entity_t owner, uint64_t frame,
                        std::vector<matter_stream::SectorRequest> drawn);
    void clear_anchor(flecs::entity_t owner);
    void detach(flecs::entity_t owner);
    void restart_if_attached();
    void worker_step(
        void* fault_context = nullptr,
        EvictionTransferFault fault = nullptr);
    bool next_request(
        TaggedRequest& out,
        void* fault_context = nullptr,
        RequestTrackingFault fault = nullptr) noexcept;
    bool begin_publication(const TaggedRequest& request) noexcept;
    bool transfer_evictions(
        PendingEvictionBatch& destination,
        void* fault_context = nullptr,
        EvictionTransferFault fault = nullptr) noexcept;
    std::vector<TaggedEviction> take_evictions();
    bool acknowledge(const TaggedRequest& request, bool published) noexcept;
    Snapshot snapshot() const;
    void terminal_clear() noexcept;

private:
    struct Acknowledgement {
        TaggedRequest request;
        bool published = false;
    };

    mutable std::mutex mutex_;
    flecs::entity_t intended_owner_ = 0;
    uint64_t attachment_revision_ = 0;
    std::optional<matter_stream::Config> intended_profile_;
    SectorStreamingErrorCode intended_profile_error_ =
        SectorStreamingErrorCode::None;
    uint64_t profile_revision_ = 0;
    std::optional<AnchorSample> intended_anchor_;
    // The pending visible batch (M4 Phase A). Guarded by mutex_ like every
    // other intended_* input; worker_step drains it under the same lock it
    // already takes for the anchor, so this adds no new synchronisation.
    std::vector<matter_stream::SectorRequest> intended_visible_;
    uint64_t intended_visible_frame_ = 0;
    bool     intended_visible_pending_ = false;
    uint64_t anchor_reset_revision_ = 0;
    uint64_t restart_revision_ = 0;
    std::vector<Acknowledgement> acknowledgement_inbox_;
    Snapshot published_snapshot_{};

    flecs::entity_t worker_owner_ = 0;
    uint64_t worker_generation_ = 0;
    uint64_t last_generation_ = 0;
    uint64_t last_issuance_ = 0;
    uint64_t applied_attachment_revision_ = 0;
    uint64_t applied_profile_revision_ = 0;
    uint64_t applied_anchor_reset_revision_ = 0;
    uint64_t applied_restart_revision_ = 0;
    std::optional<AnchorSample> worker_anchor_;
    std::unique_ptr<matter_stream::SectorStreamer> streamer_;
    std::vector<TaggedRequest> issued_requests_;
    std::vector<TaggedRequest> publication_candidates_;
    std::vector<TaggedRequest> publishing_requests_;
    std::vector<TaggedRequest> resident_requests_;
    std::vector<TaggedEviction> pending_evictions_;

    uint64_t allocate_generation();
    uint64_t allocate_issuance();
    void invalidate_worker_publications();
    void clear_worker_streamer(
        void* fault_context,
        EvictionTransferFault fault);
    void collect_streamer_evictions(
        void* fault_context,
        EvictionTransferFault fault);
    void publish_snapshot(uint64_t attachment_revision,
                          const std::optional<matter_stream::Config>& profile,
                          SectorStreamingErrorCode profile_error);
};

// Procedural installation stages profile data privately. Only the authored
// finalize/Ready boundary publishes it into Coordinator intent.
class ProfileActivationGate {
public:
    void stage(const matter_stream::Config& profile);
    void fail(Coordinator& coordinator);
    bool publish(Coordinator& coordinator);
    void begin_clear(Coordinator& coordinator);
    void finish_clear() noexcept;
    bool abort_clear(Coordinator& coordinator);
    bool pending() const noexcept;

private:
    std::optional<matter_stream::Config> staged_;
    std::optional<matter_stream::Config> active_;
    bool clearing_ = false;
};

} // namespace matter::streaming::detail
