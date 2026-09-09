#pragma once

// MatterEngine3/src/streaming/sector_streaming_coordinator.h — the private
// machinery behind sector streaming.
//
// Everything here lives in matter::streaming::detail: it is an implementation
// detail of WorldSession's streaming lane and of the focused lifecycle tests,
// not a public engine API (that is matter/streaming.h). Coordinator owns the
// matter_stream::SectorStreamer; nothing outside this file touches one.
//
// ---------------------------------------------------------------------------
// The central design: INTENT vs WORKER
// ---------------------------------------------------------------------------
// Callers never drive the streamer. attach/detach/set_profile/submit_anchor/
// clear_anchor/restart_if_attached only record INTENT under Coordinator's
// mutex and bump a revision counter. worker_step(), on the streaming lane,
// samples that intent, compares each revision against the copy it last applied,
// and reconciles.
//
// Reconciliation is all-or-nothing on purpose: an owner change, a moved
// revision, or a lost anchor tears the streamer down completely and the next
// tick builds a fresh one with a NEW generation. There is no incremental
// reconfiguration, because a new generation is exactly what makes every request
// issued by the old streamer recognizably stale.
//
// An anchor MOVE is the one thing that deliberately bumps nothing — it is the
// steady-state case and must not restart streaming.
//
// ---------------------------------------------------------------------------
// Tagging and stale completions
// ---------------------------------------------------------------------------
// Every request and eviction that leaves the coordinator carries
// (owner, generation, issuance) alongside its sector coordinates. Work is
// asynchronous, so a completion can arrive after the world it belonged to was
// detached, restarted or reprofiled; the tag is what lets that completion be
// dropped instead of publishing into a world that no longer exists. Comparisons
// use the FULL tag, so a re-issued request for the same sector after a restart
// is a different entry.
//
// ---------------------------------------------------------------------------
// The publication protocol
// ---------------------------------------------------------------------------
//   claim a PublicationCompletionCapacity slot
//   -> Coordinator::next_request()      (issues a tagged request)
//   -> ... the sector is baked/loaded asynchronously ...
//   -> Coordinator::begin_publication() (claims it; may refuse if now stale)
//   -> publish into app/GPU state
//   -> Coordinator::acknowledge(request, published)
//
// PublicationTransaction enforces the one ordering that matters on the failure
// path: roll the partial publication back BEFORE acknowledging false, so the
// coordinator never learns of a failure while GPU/app state still holds part of
// it. Evictions the worker collects are moved into a session-owned
// PendingEvictionBatch, which only forgets a tag after the endpoint has
// confirmed it applied — a failed apply retries the same set rather than
// leaking a sector that nothing will ever release.
//
// ---------------------------------------------------------------------------
// Failure discipline
// ---------------------------------------------------------------------------
// Nearly every entry point here is noexcept and reports failure by returning
// false. The pattern throughout is: reserve all storage BEFORE mutating any
// state, so an allocation failure leaves the structures exactly as they were
// and the caller can retry. The `*Fault` function-pointer hooks
// (EvictionTransferFault, RequestTrackingFault) exist purely so tests can throw
// at those precise points and assert nothing is lost or double-counted; they
// are null in production.
//
// ---------------------------------------------------------------------------
// Threading
// ---------------------------------------------------------------------------
// Coordinator's mutex guards the fields that cross lanes (intent, revisions,
// the acknowledgement inbox, the published snapshot, and the publication
// candidate/publishing lists). The worker-lane state — the streamer, the
// worker_* mirror, issued/resident requests and the local eviction queue — is
// NOT protected by it and must only be touched from the streaming lane.
// PendingEvictionBatch has its own mutex and is genuinely thread-safe;
// PublicationCompletionCapacity has none and relies on the session's
// serialization.

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

// One streaming anchor position (world space) with the tag it was recorded
// under. `owner` is the entity that submitted it and `generation` is the
// streaming generation the submitter believed was active, taken from the
// published snapshot at submit time.
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

// A sector request/eviction plus the tag that identifies WHICH streaming era it
// belongs to. `owner` + `generation` name the attachment and the streamer
// instance; `issuance` is a monotonic per-coordinator counter that
// distinguishes two requests for the same sector within one generation.
//
// The tag is the whole point: work completes asynchronously, so a completion
// carrying a generation the worker has moved past is dropped rather than
// applied. Identity comparisons use every field, coordinates included.
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

// ---------------------------------------------------------------------------
// Test-only fault-injection seams.
// ---------------------------------------------------------------------------
// Each of the paths below reserves storage at an exact point before it mutates
// anything, and the hook fires at that point so a test can throw there and
// assert the structure was left untouched. The stages name the individual
// reserve sites. All of these are null in production, and passing null disables
// the hook entirely.
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

// The externally visible streaming status, republished by the worker each tick
// and read by anyone through Coordinator::snapshot(). Its `status.state` walks
// Detached -> PendingProfile -> PendingTransform -> Active, and only Active
// carries a generation and the resident/inflight counts.
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
// Scope guard over one publication attempt. Constructed with two C-function
// callbacks (a rollback and an acknowledge) plus their shared context.
//
// The invariant it exists to enforce: on failure the partial publication is
// rolled back BEFORE the coordinator is told the sector did not publish.
//
// fail() and commit() return false when their callback fails, and in that case
// the transaction stays ACTIVE — the destructor is the retry, re-committing
// when a commit was already intended and otherwise rolling back and
// acknowledging false. rollback_complete_ makes that retry idempotent: the
// rollback runs at most once even if the acknowledge is what kept failing.
// Non-copyable; the destructor is noexcept and must never be allowed to throw.
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

// Owns the SectorStreamer and reconciles caller INTENT with worker state (see
// the file header). One instance per WorldSession; not copyable in practice
// (it holds a mutex) and not designed to be moved.
//
// Two groups of members, and the distinction is the class's whole contract:
//
//   * Intent + handoff, guarded by mutex_ — intended_owner_/profile_/anchor_,
//     the four *_revision_ counters, acknowledgement_inbox_,
//     published_snapshot_, publication_candidates_ and publishing_requests_.
//     These are written from the caller's thread and read by the worker.
//   * Worker-lane state, NOT guarded — worker_owner_/generation_/anchor_, the
//     applied_*_revision_ mirror, streamer_, issued_requests_,
//     resident_requests_ and pending_evictions_. Only the streaming lane may
//     touch these. next_request() and begin_publication() run on that lane too
//     and take the mutex only for the shared lists they also touch.
//
// Method thread affinity: attach/detach/set_profile/submit_anchor/clear_anchor/
// restart_if_attached/acknowledge/snapshot are the caller-side, lock-taking
// entry points. worker_step/next_request/begin_publication/transfer_evictions/
// take_evictions belong to the streaming lane. terminal_clear() is teardown and
// assumes the worker has already stopped.
class Coordinator {
public:
    bool attach(flecs::entity_t owner);
    flecs::entity_t intended_owner() const;
    void set_profile(
        const matter_stream::Config* profile,
        SectorStreamingErrorCode profile_error =
            SectorStreamingErrorCode::None);
    void submit_anchor(flecs::entity_t owner, float x, float y, float z);

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
// Three states, held in two optionals plus a flag:
//   staged_   — a profile built by an installation stage but not yet authored;
//               invisible to Coordinator.
//   active_   — the profile last published into Coordinator intent.
//   clearing_ — a clear is in flight: Coordinator has been given a null profile
//               but active_ is retained so abort_clear() can restore it.
// publish() promotes staged -> active and pushes it; finish_clear() commits the
// clear by dropping both; fail() drops everything and nulls the coordinator's
// profile.
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
