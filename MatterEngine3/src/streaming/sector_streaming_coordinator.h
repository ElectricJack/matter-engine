#pragma once

#include "../sector_streamer.h"
#include "matter/streaming.h"

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

struct Snapshot {
    flecs::entity_t owner = 0;
    SectorStreamingStatus status{};
    SectorStreamingError error{};
};

// Thread-safe session-owned retention for coordinator evictions after they
// cross take_evictions(). The endpoint sees a value snapshot; tags are erased
// only after that complete snapshot applies successfully.
class PendingEvictionBatch {
public:
    using Endpoint = std::function<bool(
        const std::vector<TaggedEviction>&, std::string&)>;

    void append(std::vector<TaggedEviction> evictions);
    bool apply(const Endpoint& endpoint, std::string& error);
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
    using Rollback = std::function<bool(std::string&)>;
    using Acknowledge = std::function<void(bool)>;

    PublicationTransaction(Rollback rollback, Acknowledge acknowledge);
    ~PublicationTransaction();
    PublicationTransaction(const PublicationTransaction&) = delete;
    PublicationTransaction& operator=(const PublicationTransaction&) = delete;

    bool fail(std::string& error);
    void commit();

private:
    Rollback rollback_;
    Acknowledge acknowledge_;
    bool active_ = true;
};

class Coordinator {
public:
    bool attach(flecs::entity_t owner);
    flecs::entity_t intended_owner() const;
    void set_profile(
        const matter_stream::Config* profile,
        SectorStreamingErrorCode profile_error =
            SectorStreamingErrorCode::None);
    void submit_anchor(flecs::entity_t owner, float x, float z);
    void clear_anchor(flecs::entity_t owner);
    void detach(flecs::entity_t owner);
    void restart_if_attached();
    void worker_step();
    bool next_request(TaggedRequest& out);
    bool begin_publication(const TaggedRequest& request);
    std::vector<TaggedEviction> take_evictions();
    void acknowledge(const TaggedRequest& request, bool published);
    Snapshot snapshot() const;

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
    void clear_worker_streamer();
    void collect_streamer_evictions();
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
    bool pending() const noexcept;

private:
    std::optional<matter_stream::Config> staged_;
};

} // namespace matter::streaming::detail
