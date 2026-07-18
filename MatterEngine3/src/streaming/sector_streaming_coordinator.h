#pragma once

#include "../sector_streamer.h"
#include "matter/streaming.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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
    matter_stream::Eviction sector{};
};

struct Snapshot {
    flecs::entity_t owner = 0;
    SectorStreamingStatus status{};
};

class Coordinator {
public:
    bool attach(flecs::entity_t owner);
    flecs::entity_t intended_owner() const;
    void set_profile(const matter_stream::Config* profile);
    void submit_anchor(flecs::entity_t owner, float x, float z);
    void clear_anchor(flecs::entity_t owner);
    void detach(flecs::entity_t owner);
    void restart_if_attached();
    void worker_step();
    bool next_request(TaggedRequest& out);
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
    std::vector<TaggedEviction> pending_evictions_;

    uint64_t allocate_generation();
    uint64_t allocate_issuance();
    void clear_worker_streamer();
    void collect_streamer_evictions();
    void publish_snapshot(uint64_t attachment_revision,
                          const std::optional<matter_stream::Config>& profile);
};

} // namespace matter::streaming::detail
