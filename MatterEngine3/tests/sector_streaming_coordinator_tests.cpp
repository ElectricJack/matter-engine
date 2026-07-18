#include "check.h"
#include "../src/streaming/sector_streaming_coordinator.h"

#include <cstdint>
#include <vector>

using matter::streaming::SectorStreamingState;
using matter::streaming::detail::Coordinator;
using matter::streaming::detail::TaggedRequest;

static matter_stream::Config tiny_profile() {
    matter_stream::Config value;
    value.sector_size = 16.0f;
    value.rings = {{24.0f, 1}};
    value.hysteresis = 4.0f;
    value.max_inflight = 2;
    value.fail_cooldown_updates = 2;
    return value;
}

static constexpr flecs::entity_t kOwnerA =
    (flecs::entity_t{7} << 32) | flecs::entity_t{41};
static constexpr flecs::entity_t kOwnerB =
    (flecs::entity_t{9} << 32) | flecs::entity_t{41};

static bool same_sector(const TaggedRequest& lhs, const TaggedRequest& rhs) {
    return lhs.sector.tx == rhs.sector.tx &&
           lhs.sector.tz == rhs.sector.tz &&
           lhs.sector.rung == rhs.sector.rung;
}

static std::vector<TaggedRequest> drain_requests(Coordinator& coordinator) {
    std::vector<TaggedRequest> requests;
    TaggedRequest request{};
    while (coordinator.next_request(request)) requests.push_back(request);
    return requests;
}

int main() {
    // Profile and anchor intents do not stream without an attached owner.
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        const auto snapshot = coordinator.snapshot();
        CHECK(snapshot.owner == 0 &&
                  snapshot.status.state == SectorStreamingState::Detached &&
                  snapshot.status.generation == 0 &&
                  snapshot.status.resident_sectors == 0 &&
                  snapshot.status.inflight_sectors == 0,
              "unattached coordinator remains detached with zero counts");
        CHECK(drain_requests(coordinator).empty(),
              "unattached profile-ready coordinator emits no requests");
    }

    // Attachment waits independently for profile and anchor readiness.
    {
        Coordinator coordinator;
        CHECK(coordinator.attach(kOwnerA), "first owner attaches");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.state ==
                  SectorStreamingState::PendingProfile,
              "attached owner waits for profile");
        CHECK(drain_requests(coordinator).empty(),
              "pending-profile owner emits no requests");

        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.state == SectorStreamingState::Active,
              "profile completes readiness when anchor already exists");
    }
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        profile.rings.clear(); // Proves set_profile copied before returning.
        CHECK(coordinator.attach(kOwnerA), "owner attaches with copied profile ready");
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.state ==
                  SectorStreamingState::PendingTransform,
              "attached profiled owner waits for anchor");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.state == SectorStreamingState::Active,
              "anchor activates copied nonempty profile");
        CHECK(!drain_requests(coordinator).empty(),
              "copied profile remains usable after caller mutation");
    }

    // A full generational owner ID is exclusive and anchors coalesce newest-wins.
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA), "first generational owner attaches");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        const auto initial = coordinator.snapshot();
        CHECK(!coordinator.attach(kOwnerB), "second owner is rejected");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.submit_anchor(kOwnerA, 1000.0f, 8.0f);
        coordinator.worker_step();
        const auto after = coordinator.snapshot();
        CHECK(after.owner == kOwnerA &&
                  after.status.generation == initial.status.generation,
              "duplicate attach preserves first owner and generation");
        auto requests = drain_requests(coordinator);
        CHECK(!requests.empty() && requests.front().owner == kOwnerA &&
                  requests.front().generation == initial.status.generation &&
                  requests.front().sector.tx == 62,
              "latest coalesced anchor drives tagged requests");
    }

    // Detach invalidates the public snapshot immediately, evicts residents, and
    // rejects acknowledgements from the cleared generation.
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA), "detach test owner attaches");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        auto first = drain_requests(coordinator);
        CHECK(first.size() == 2, "detach test allocates capped initial requests");
        for (const auto& request : first) coordinator.acknowledge(request, true);
        coordinator.worker_step();
        const auto active = coordinator.snapshot();
        CHECK(active.status.resident_sectors == first.size(),
              "published acknowledgements establish residency on worker step");
        auto late = drain_requests(coordinator);
        CHECK(!late.empty(), "detach test retains an in-flight late request");

        coordinator.detach(kOwnerB);
        CHECK(coordinator.snapshot().owner == kOwnerA,
              "detach from nonowner is ignored");
        coordinator.detach(kOwnerA);
        const auto immediate = coordinator.snapshot();
        CHECK(immediate.owner == 0 &&
                  immediate.status.state == SectorStreamingState::Detached &&
                  immediate.status.generation == 0,
              "owner detach invalidates visible generation immediately");
        coordinator.acknowledge(late.front(), true);
        coordinator.worker_step();
        const auto evictions = coordinator.take_evictions();
        CHECK(evictions.size() == active.status.resident_sectors,
              "detach emits every resident eviction");
        bool all_tagged = true;
        for (const auto& eviction : evictions) {
            all_tagged = all_tagged && eviction.owner == kOwnerA &&
                         eviction.generation == active.status.generation;
        }
        CHECK(all_tagged, "detach evictions retain full owner and old generation");
        CHECK(coordinator.snapshot().status.resident_sectors == 0,
              "late publish acknowledgement cannot restore detached residency");
    }

    // Failure stays nonresident and the same request returns only after its
    // configured number of worker updates.
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA), "failure test owner attaches");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        auto requests = drain_requests(coordinator);
        CHECK(!requests.empty(), "failure test obtains request");
        const TaggedRequest failed = requests.front();
        coordinator.acknowledge(failed, false);
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.resident_sectors == 0,
              "failed acknowledgement does not create residency");

        bool early_retry = false;
        for (int update = 0; update < 2; ++update) {
            auto current = drain_requests(coordinator);
            for (const auto& request : current) {
                early_retry = early_retry || same_sector(request, failed);
                coordinator.acknowledge(request, true);
            }
            coordinator.worker_step();
        }
        CHECK(!early_retry, "failed sector is withheld throughout cooldown");

        bool retried = false;
        for (int update = 0; update < 16 && !retried; ++update) {
            auto current = drain_requests(coordinator);
            for (const auto& request : current) {
                if (same_sector(request, failed)) retried = true;
                coordinator.acknowledge(request, true);
            }
            coordinator.worker_step();
        }
        CHECK(retried, "failed sector becomes requestable after cooldown");
    }

    // Restart coalesces into one new generation while preserving attachment.
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA), "restart test owner attaches");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        const uint64_t first_generation = coordinator.snapshot().status.generation;
        coordinator.restart_if_attached();
        coordinator.restart_if_attached();
        coordinator.worker_step();
        const auto restarted = coordinator.snapshot();
        CHECK(restarted.owner == kOwnerA &&
                  restarted.status.state == SectorStreamingState::Active &&
                  restarted.status.generation == first_generation + 1,
              "coalesced restart retains owner and advances generation once");
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.generation == restarted.status.generation,
              "restart does not recreate generation on later worker steps");
    }

    // A detach intent supersedes an unprocessed restart.
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA), "restart-detach test owner attaches");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        coordinator.restart_if_attached();
        coordinator.detach(kOwnerA);
        coordinator.worker_step();
        const auto snapshot = coordinator.snapshot();
        CHECK(snapshot.owner == 0 &&
                  snapshot.status.state == SectorStreamingState::Detached &&
                  snapshot.status.generation == 0,
              "detach before restart step prevents recreation");
        CHECK(drain_requests(coordinator).empty(),
              "detached restart intent emits no requests");
    }

    return check_summary();
}
