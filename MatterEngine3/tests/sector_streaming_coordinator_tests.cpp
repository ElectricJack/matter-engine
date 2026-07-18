#include "check.h"
#include "../src/streaming/sector_streaming_coordinator.h"

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

using matter::streaming::SectorStreamingState;
using matter::streaming::detail::Coordinator;
using matter::streaming::detail::PendingEvictionBatch;
using matter::streaming::detail::PublicationCompletionCapacity;
using matter::streaming::detail::ProfileActivationGate;
using matter::streaming::detail::PublicationResources;
using matter::streaming::detail::PublicationTransaction;
using matter::streaming::detail::TaggedEviction;
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

template <typename Request, typename = void>
struct has_issuance : std::false_type {};

template <typename Request>
struct has_issuance<Request,
                    std::void_t<decltype(std::declval<Request>().issuance)>>
    : std::true_type {};

template <typename Request>
static bool distinct_issuance(const Request& lhs, const Request& rhs) {
    if constexpr (has_issuance<Request>::value) {
        return lhs.issuance != rhs.issuance;
    }
    return false;
}

static std::vector<TaggedRequest> drain_requests(Coordinator& coordinator) {
    std::vector<TaggedRequest> requests;
    TaggedRequest request{};
    while (coordinator.next_request(request)) requests.push_back(request);
    return requests;
}

static bool same_publication(
    const TaggedRequest& request,
    const TaggedEviction& eviction) {
    return request.owner == eviction.owner &&
           request.generation == eviction.generation &&
           request.issuance == eviction.issuance &&
           request.sector.tx == eviction.sector.tx &&
           request.sector.tz == eviction.sector.tz &&
           request.sector.rung == eviction.sector.rung;
}

// CPU-only stand-in for app-thread resource ownership. Coordinator behavior is
// real; this ledger merely records whether a complete tagged publication would
// exist on the app side and applies the coordinator's tagged evictions.
struct FakePublicationLedger {
    std::vector<TaggedRequest> resident;

    bool publish(
        Coordinator& coordinator,
        const TaggedRequest& request,
        bool resources_succeed) {
        if (!coordinator.begin_publication(request)) {
            coordinator.acknowledge(request, false);
            return false;
        }

        resident.push_back(request); // provisional app mutation
        if (!resources_succeed) {
            resident.pop_back(); // complete rollback before false acknowledgement
            coordinator.acknowledge(request, false);
            return false;
        }

        coordinator.acknowledge(request, true);
        return true;
    }

    void apply(const std::vector<TaggedEviction>& evictions) {
        for (const auto& eviction : evictions) {
            const auto match = std::find_if(
                resident.begin(), resident.end(),
                [&](const TaggedRequest& request) {
                    return same_publication(request, eviction);
                });
            if (match != resident.end()) resident.erase(match);
        }
    }
};

struct TransactionProbe {
    PublicationResources resources{};
    int sequence = 0;
    int rollback_order = 0;
    int acknowledge_order = 0;
    int rollback_calls = 0;
    int acknowledge_calls = 0;
    int rollback_failures = 0;
    int acknowledge_failures = 0;
    bool acknowledged_value = false;
};

static bool transaction_probe_rollback(
    void* opaque,
    std::string&) noexcept {
    auto& probe = *static_cast<TransactionProbe*>(opaque);
    ++probe.rollback_calls;
    probe.rollback_order = ++probe.sequence;
    if (probe.rollback_failures > 0) {
        --probe.rollback_failures;
        return false;
    }
    probe.resources = {};
    return true;
}

static bool transaction_probe_acknowledge(
    void* opaque,
    bool published) noexcept {
    auto& probe = *static_cast<TransactionProbe*>(opaque);
    ++probe.acknowledge_calls;
    probe.acknowledge_order = ++probe.sequence;
    probe.acknowledged_value = published;
    if (probe.acknowledge_failures > 0) {
        --probe.acknowledge_failures;
        return false;
    }
    return true;
}

static_assert(
    noexcept(std::declval<Coordinator&>().begin_publication(
        std::declval<const TaggedRequest&>())),
    "publication reservation must contain allocation failure");
static_assert(
    std::is_same_v<
        decltype(std::declval<Coordinator&>().acknowledge(
            std::declval<const TaggedRequest&>(), false)),
        bool>,
    "publication acknowledgement must report durable enqueue failure");
static_assert(
    noexcept(std::declval<Coordinator&>().acknowledge(
        std::declval<const TaggedRequest&>(), false)),
    "publication acknowledgement must not throw through the GPU pump");

int main() {
    // A failed app endpoint keeps the complete tagged batch durable. Work that
    // completed before a mid-batch fault is harmlessly seen again, and the tags
    // disappear only after the production batch seam reports full success.
    {
        PendingEvictionBatch pending;
        const TaggedEviction first{
            kOwnerA, 11, 101, matter_stream::Eviction{0, 0, 1}};
        const TaggedEviction second{
            kOwnerA, 11, 102, matter_stream::Eviction{1, 0, 1}};
        pending.append({first, second});

        int endpoint_calls = 0;
        std::vector<uint64_t> cleaned;
        std::string error;
        CHECK(!pending.apply(
                  [&](const std::vector<TaggedEviction>& batch,
                      std::string& endpoint_error) {
                      ++endpoint_calls;
                      cleaned.push_back(batch.front().issuance);
                      endpoint_error = "injected mid-batch eviction failure";
                      return false;
                  },
                  error) &&
                  pending.size() == 2,
              "mid-batch failure retains every tagged eviction for retry");
        CHECK(pending.apply(
                  [&](const std::vector<TaggedEviction>& batch,
                      std::string&) {
                      ++endpoint_calls;
                      for (const auto& eviction : batch) {
                          if (std::find(cleaned.begin(), cleaned.end(),
                                        eviction.issuance) == cleaned.end()) {
                              cleaned.push_back(eviction.issuance);
                          }
                      }
                      return true;
                  },
                  error) &&
                  pending.empty() && endpoint_calls == 2 &&
                  cleaned.size() == 2,
              "retry is idempotent and clears tags only on full success");
    }

    // Inline publication rollback owns only its own tag. Older lifecycle tags
    // stay pending in FIFO order for the worker-enqueued blocking endpoint.
    {
        PendingEvictionBatch pending;
        const TaggedEviction movement{
            kOwnerA, 21, 201, matter_stream::Eviction{0, 0, 1}};
        const TaggedEviction detach{
            kOwnerA, 21, 202, matter_stream::Eviction{1, 0, 1}};
        const TaggedEviction publication{
            kOwnerA, 21, 203, matter_stream::Eviction{2, 0, 1}};
        pending.append({movement, detach});

        std::vector<uint64_t> applied;
        std::string error;
        CHECK(pending.apply_tag(
                  publication,
                  [&](const std::vector<TaggedEviction>& batch,
                      std::string&) {
                      for (const auto& eviction : batch) {
                          applied.push_back(eviction.issuance);
                      }
                      return true;
                  },
                  error) &&
                  applied == std::vector<uint64_t>{203} &&
                  pending.size() == 2,
              "publication rollback applies and retires only its own tag");
        applied.clear();
        CHECK(pending.apply(
                  [&](const std::vector<TaggedEviction>& batch,
                      std::string&) {
                      for (const auto& eviction : batch) {
                          applied.push_back(eviction.issuance);
                      }
                      return true;
                  },
                  error) &&
                  applied.size() == 2 && applied[0] == 201 &&
                  applied[1] == 202,
              "worker lifecycle endpoint retains original FIFO ownership");
    }

    // Persistent endpoint failure is terminally abandonable for no-fail owner
    // teardown; shutdown cannot spin forever on an unreleasable resource.
    {
        PendingEvictionBatch pending;
        pending.append({TaggedEviction{
            kOwnerA, 31, 301, matter_stream::Eviction{0, 0, 1}}});
        std::string error;
        CHECK(!pending.apply(
                  [](const std::vector<TaggedEviction>&,
                     std::string& endpoint_error) {
                      endpoint_error = "persistent endpoint failure";
                      return false;
                  },
                  error) && pending.size() == 1,
              "persistent lifecycle failure remains pending before teardown");
        pending.abandon_noexcept();
        CHECK(pending.empty(),
              "terminal no-fail teardown clears retained lifecycle tags");
    }

    // The production publication guard owns rollback-before-false-ack ordering
    // for faults at every attempted resource stage.
    {
        for (int fail_stage = 0; fail_stage < 5; ++fail_stage) {
            TransactionProbe probe;
            PublicationTransaction transaction(
                &probe,
                transaction_probe_rollback,
                transaction_probe_acknowledge);
            static_assert(noexcept(PublicationTransaction(
                nullptr, nullptr, nullptr)),
                "publication cleanup scope construction must not allocate/throw");

            probe.resources.transient_artifact = true;
            if (fail_stage >= 1) probe.resources.store_attempted = true;
            if (fail_stage >= 2) probe.resources.world_state_attempted = true;
            if (fail_stage >= 3) probe.resources.culler_attempted = true;
            if (fail_stage >= 4) probe.resources.vulkan_attempted = true;
            std::string error;
            CHECK(transaction.fail(error) &&
                      probe.rollback_order < probe.acknowledge_order &&
                      !probe.acknowledged_value &&
                      !probe.resources.transient_artifact &&
                      !probe.resources.store_attempted &&
                      !probe.resources.world_state_attempted &&
                      !probe.resources.culler_attempted &&
                      !probe.resources.vulkan_attempted,
                  "stage fault rolls back every attempted resource before false ack");
        }

        TransactionProbe probe;
        PublicationTransaction transaction(
            &probe,
            transaction_probe_rollback,
            transaction_probe_acknowledge);
        CHECK(transaction.commit() && probe.acknowledged_value &&
                  probe.rollback_calls == 0,
              "successful transaction acknowledges only after explicit commit");
    }

    // Reservation/ack/transient failures are contained by the no-throw scope.
    // Failed completion remains active and is retryable without rerunning an
    // already completed rollback.
    {
        TransactionProbe true_ack;
        true_ack.acknowledge_failures = 1;
        PublicationTransaction transaction(
            &true_ack,
            transaction_probe_rollback,
            transaction_probe_acknowledge);
        CHECK(!transaction.commit() && transaction.active() &&
                  transaction.commit() && !transaction.active() &&
                  true_ack.acknowledge_calls == 2 &&
                  true_ack.rollback_calls == 0,
              "true acknowledgement failure is retained and retried");

        TransactionProbe destructor_ack;
        destructor_ack.acknowledge_failures = 1;
        {
            PublicationTransaction guarded_commit(
                &destructor_ack,
                transaction_probe_rollback,
                transaction_probe_acknowledge);
            CHECK(!guarded_commit.commit() && guarded_commit.active(),
                  "failed commit remains owned until scope teardown");
        }
        CHECK(destructor_ack.acknowledge_calls == 2 &&
                  destructor_ack.acknowledged_value &&
                  destructor_ack.rollback_calls == 0,
              "scope teardown retries true ack without changing its intent");

        TransactionProbe failed_publication;
        failed_publication.rollback_failures = 1;
        failed_publication.acknowledge_failures = 1;
        PublicationTransaction failed(
            &failed_publication,
            transaction_probe_rollback,
            transaction_probe_acknowledge);
        std::string error;
        CHECK(!failed.fail(error) && failed.active() &&
                  failed_publication.acknowledge_calls == 0 &&
                  !failed.fail(error) && failed.active() &&
                  failed_publication.rollback_calls == 2 &&
                  failed_publication.acknowledge_calls == 1 &&
                  failed.fail(error) && !failed.active() &&
                  failed_publication.rollback_calls == 2 &&
                  failed_publication.acknowledge_calls == 2 &&
                  !failed_publication.acknowledged_value,
              "orphan cleanup and false ack failures retry in order without escape");
    }

    // Completion capacity is claimed before next_request. Retained completion
    // work from many superseded generations may fill every fixed slot, but the
    // next coordinator request is then left unallocated rather than orphaned.
    // Once durable false acknowledgements enqueue, every claim is reusable and
    // current-generation inflight state drains normally.
    {
        Coordinator coordinator;
        PublicationCompletionCapacity capacity;
        auto profile = tiny_profile();
        profile.max_inflight = 1;
        CHECK(coordinator.attach(kOwnerA),
              "capacity regression attaches owner");
        coordinator.set_profile(&profile);
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();

        struct RetainedCompletion {
            size_t slot = PublicationCompletionCapacity::kCapacity;
            TaggedRequest request{};
        };
        std::vector<RetainedCompletion> retained;
        retained.reserve(PublicationCompletionCapacity::kCapacity);

        auto take_tracked_request = [&](RetainedCompletion& tracked) {
            if (!capacity.try_reserve(tracked.slot)) return false;
            if (!coordinator.next_request(tracked.request)) {
                capacity.release(tracked.slot);
                tracked.slot = PublicationCompletionCapacity::kCapacity;
                return false;
            }
            return true;
        };

        for (size_t generation = 0;
             generation < PublicationCompletionCapacity::kCapacity;
             ++generation) {
            RetainedCompletion tracked;
            CHECK(take_tracked_request(tracked),
                  "each available slot owns its request before restart");
            retained.push_back(tracked);
            coordinator.restart_if_attached();
            coordinator.worker_step();
        }

        RetainedCompletion overflow;
        CHECK(capacity.full() && !take_tracked_request(overflow) &&
                  coordinator.snapshot().status.inflight_sectors == 0,
              "full durable capacity stops before allocating an untracked request");

        for (const auto& tracked : retained) {
            CHECK(coordinator.acknowledge(tracked.request, false),
                  "retained false acknowledgement durably enqueues");
            capacity.release(tracked.slot);
        }
        coordinator.worker_step();
        CHECK(capacity.empty() &&
                  coordinator.snapshot().status.inflight_sectors == 0,
              "stale-generation false acknowledgements and claims all drain");

        RetainedCompletion current;
        CHECK(take_tracked_request(current) &&
                  coordinator.acknowledge(current.request, false),
              "released capacity admits a new tracked request");
        capacity.release(current.slot);
        coordinator.worker_step();
        CHECK(capacity.empty() &&
                  coordinator.snapshot().status.inflight_sectors == 0,
              "current false acknowledgement drains inflight state");
    }

    // A procedural profile is staged privately and cannot allocate sector work
    // when authored finalization fails before the gate is committed.
    {
        Coordinator coordinator;
        ProfileActivationGate activation;
        auto profile = tiny_profile();
        CHECK(coordinator.attach(kOwnerA),
              "profile-readiness test attaches owner");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        activation.stage(profile);
        activation.fail(coordinator);
        coordinator.worker_step();
        TaggedRequest request{};
        CHECK(coordinator.snapshot().status.state ==
                  SectorStreamingState::PendingProfile &&
                  coordinator.snapshot().status.resident_sectors == 0 &&
                  coordinator.snapshot().status.inflight_sectors == 0 &&
                  !coordinator.next_request(request),
              "failed authored finalize leaves zero profile work or residency");

        activation.stage(profile);
        activation.publish(coordinator);
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.state == SectorStreamingState::Active &&
                  coordinator.next_request(request),
              "authored success publishes the staged procedural profile");
    }

    // Reload clearing is provisional. A persistent release failure aborts the
    // replacement and restores the old active profile instead of retrying the
    // barrier forever or leaving the session profile-less.
    {
        Coordinator coordinator;
        ProfileActivationGate activation;
        auto profile = tiny_profile();
        CHECK(coordinator.attach(kOwnerA),
              "reload-abort test attaches owner");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        activation.stage(profile);
        CHECK(activation.publish(coordinator),
              "reload-abort test publishes initial profile");
        coordinator.worker_step();
        const auto old_generation =
            coordinator.snapshot().status.generation;
        CHECK(coordinator.snapshot().status.state ==
                  SectorStreamingState::Active,
              "reload-abort test begins from an active profile");

        activation.begin_clear(coordinator);
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.state ==
                  SectorStreamingState::PendingProfile,
              "reload barrier provisionally clears coordinator profile");

        const bool cleanup_succeeded = false;
        bool replacement_installed = false;
        if (cleanup_succeeded) {
            activation.finish_clear();
            replacement_installed = true;
        } else {
            activation.abort_clear(coordinator);
        }
        coordinator.worker_step();
        const auto restored = coordinator.snapshot();
        CHECK(!replacement_installed &&
                  restored.status.state == SectorStreamingState::Active &&
                  restored.status.generation != 0 &&
                  restored.status.generation != old_generation,
              "failed reload cleanup reuses old profile and aborts replacement");
    }

    // App resources precede a successful acknowledgement, while rollback and
    // false acknowledgement can never create coordinator residency.
    {
        Coordinator coordinator;
        FakePublicationLedger ledger;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA),
              "publication-order test attaches owner");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();

        TaggedRequest successful{};
        CHECK(coordinator.next_request(successful),
              "publication-order test obtains successful request");
        CHECK(coordinator.snapshot().status.resident_sectors == 0,
              "request is not resident before app publication");
        CHECK(ledger.publish(coordinator, successful, true) &&
                  ledger.resident.size() == 1,
              "successful fake endpoint mutates resources before acknowledgement");
        CHECK(coordinator.snapshot().status.resident_sectors == 0,
              "queued acknowledgement is not early residency");
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.resident_sectors == 1,
              "successful publication becomes resident on the worker step");

        TaggedRequest failed{};
        CHECK(coordinator.next_request(failed),
              "publication-order test obtains failing request");
        CHECK(!ledger.publish(coordinator, failed, false) &&
                  ledger.resident.size() == 1,
              "failed endpoint rolls back its provisional app mutation");
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.resident_sectors == 1,
              "failed publication cannot create phantom residency");
    }

    // Detach invalidates a publication in progress, emits FIFO cleanup with the
    // complete issuance tag, and stale cleanup cannot erase a newer replacement.
    {
        Coordinator coordinator;
        FakePublicationLedger ledger;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA),
              "publication-detach test attaches owner");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        TaggedRequest old_request{};
        CHECK(coordinator.next_request(old_request) &&
                  coordinator.begin_publication(old_request),
              "publication-detach test reserves old tagged publication");
        ledger.resident.push_back(old_request);

        coordinator.detach(kOwnerA);
        coordinator.worker_step();
        const auto old_evictions = coordinator.take_evictions();
        CHECK(old_evictions.size() == 1 &&
                  same_publication(old_request, old_evictions.front()),
              "detach emits cleanup for publication in progress");
        ledger.apply(old_evictions);
        CHECK(ledger.resident.empty() &&
                  coordinator.snapshot().status.resident_sectors == 0,
              "detach FIFO cleanup leaves no app or coordinator residency");

        CHECK(coordinator.attach(kOwnerA),
              "same owner reattaches after detach cleanup");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        TaggedRequest replacement{};
        CHECK(coordinator.next_request(replacement) &&
                  ledger.publish(coordinator, replacement, true),
              "fresh generation publishes replacement");
        coordinator.worker_step();
        CHECK(replacement.generation != old_request.generation &&
                  replacement.issuance != old_request.issuance,
              "replacement has fresh lifecycle and issuance identity");
        ledger.apply(old_evictions);
        CHECK(ledger.resident.size() == 1 &&
                  ledger.resident.front().issuance == replacement.issuance,
              "stale tagged eviction fails closed on newer replacement");
    }

    // Profile replacement keeps the same attachment and starts one fresh
    // generation; removal while the profile is absent prevents any restart.
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA),
              "profile-reload test attaches owner");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        const uint64_t first_generation =
            coordinator.snapshot().status.generation;

        coordinator.set_profile(nullptr);
        coordinator.worker_step();
        CHECK(coordinator.intended_owner() == kOwnerA &&
                  coordinator.snapshot().status.state ==
                      SectorStreamingState::PendingProfile &&
                  coordinator.snapshot().status.resident_sectors == 0,
              "reload barrier preserves attachment with an empty profile");
        coordinator.set_profile(&profile);
        coordinator.worker_step();
        const uint64_t reloaded_generation =
            coordinator.snapshot().status.generation;
        CHECK(coordinator.snapshot().status.state ==
                  SectorStreamingState::Active &&
                  reloaded_generation == first_generation + 1,
              "profile reload starts exactly one fresh generation");
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.generation == reloaded_generation,
              "profile reload does not restart twice");

        coordinator.set_profile(nullptr);
        coordinator.worker_step();
        coordinator.detach(kOwnerA);
        coordinator.set_profile(&profile);
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.state ==
                  SectorStreamingState::Detached &&
                  coordinator.snapshot().status.generation == 0 &&
                  drain_requests(coordinator).empty(),
              "owner removal during reload prevents restart");
    }

    // Intended ownership is synchronously authoritative before worker snapshots.
    {
        Coordinator coordinator;
        CHECK(coordinator.intended_owner() == 0,
              "new coordinator has no intended owner");
        CHECK(coordinator.attach(kOwnerA) &&
                  coordinator.intended_owner() == kOwnerA,
              "successful attach synchronously publishes intended owner");
        CHECK(!coordinator.attach(kOwnerB) &&
                  coordinator.intended_owner() == kOwnerA,
              "rejected attach preserves authoritative intended owner");
        coordinator.detach(kOwnerA);
        CHECK(coordinator.intended_owner() == 0,
              "detach synchronously clears authoritative intended owner");
    }

    // A clear boundary is durable even when a newer anchor sample is coalesced
    // before the worker observes either intent.
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        profile.rings = {{24.0f, 1}};
        profile.max_inflight = 4;
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA),
              "coalesced-clear test attaches owner");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        const auto active = coordinator.snapshot();
        auto old_requests = drain_requests(coordinator);
        CHECK(active.status.generation != 0 && old_requests.size() >= 2,
              "coalesced-clear test starts issued old-generation work");
        coordinator.acknowledge(old_requests.front(), true);
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.resident_sectors == 1,
              "coalesced-clear test establishes old-generation residency");

        coordinator.clear_anchor(kOwnerA);
        TaggedRequest invalidated{};
        CHECK(!coordinator.next_request(invalidated),
              "clear return blocks old-generation request allocation");
        coordinator.acknowledge(old_requests.back(), true);
        coordinator.submit_anchor(kOwnerA, 168.0f, 24.0f);
        coordinator.worker_step();

        const auto restarted = coordinator.snapshot();
        CHECK(restarted.owner == kOwnerA &&
                  restarted.status.state == SectorStreamingState::Active &&
                  restarted.status.generation == active.status.generation + 1 &&
                  restarted.status.resident_sectors == 0,
              "coalesced restore starts clean next generation in one worker step");
        const auto evictions = coordinator.take_evictions();
        CHECK(evictions.size() == 1 &&
                  evictions.front().owner == kOwnerA &&
                  evictions.front().generation == active.status.generation,
              "coalesced clear emits tagged old-generation eviction");
        const auto fresh_requests = drain_requests(coordinator);
        bool all_fresh = !fresh_requests.empty();
        for (const auto& request : fresh_requests) {
            all_fresh = all_fresh && request.owner == kOwnerA &&
                        request.generation == restarted.status.generation;
        }
        CHECK(all_fresh && fresh_requests.front().sector.tx == 10 &&
                  fresh_requests.front().sector.tz == 1,
              "restored anchor drives only fresh-generation requests");
    }

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

    // Losing the resolved transform clears active worker state and a later
    // sample creates a fresh generation.
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        profile.rings = {{24.0f, 1}};
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA), "anchor-clear test attaches");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        const auto active = coordinator.snapshot();
        auto requests = drain_requests(coordinator);
        CHECK(requests.size() >= 2,
              "anchor-clear test starts with multiple requests");
        coordinator.acknowledge(requests.front(), true);
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.resident_sectors == 1,
              "anchor-clear test establishes residency");

        coordinator.clear_anchor(kOwnerB);
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.state == SectorStreamingState::Active,
              "nonowner cannot clear the active anchor");

        coordinator.clear_anchor(kOwnerA);
        coordinator.acknowledge(requests.back(), true);
        coordinator.worker_step();
        const auto pending = coordinator.snapshot();
        TaggedRequest stale{};
        CHECK(pending.owner == kOwnerA &&
                  pending.status.state == SectorStreamingState::PendingTransform &&
                  pending.status.generation == 0 &&
                  pending.status.resident_sectors == 0 &&
                  pending.status.inflight_sectors == 0,
              "anchor clear publishes empty pending-transform state");
        CHECK(!coordinator.next_request(stale),
              "anchor clear rejects stale request allocation");
        const auto evictions = coordinator.take_evictions();
        CHECK(evictions.size() == 1 &&
                  evictions.front().owner == kOwnerA &&
                  evictions.front().generation == active.status.generation,
              "anchor clear emits old-generation resident eviction");

        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        const auto restarted = coordinator.snapshot();
        CHECK(restarted.status.state == SectorStreamingState::Active &&
                  restarted.status.generation == active.status.generation + 1,
              "new anchor after clear starts a fresh generation");
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


    // A detach boundary resets worker state even if the same full owner ID is
    // reattached before the worker observes the intermediate detached intent.
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA), "same-owner boundary test attaches");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        TaggedRequest resident{};
        CHECK(coordinator.next_request(resident),
              "same-owner boundary test obtains resident request");
        coordinator.acknowledge(resident, true);
        coordinator.worker_step();
        const auto before = coordinator.snapshot();
        CHECK(before.status.resident_sectors == 1,
              "same-owner boundary test establishes residency");

        coordinator.detach(kOwnerA);
        CHECK(coordinator.attach(kOwnerA),
              "same full owner ID reattaches before worker step");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        const auto after = coordinator.snapshot();
        CHECK(after.owner == kOwnerA &&
                  after.status.generation == before.status.generation + 1 &&
                  after.status.resident_sectors == 0,
              "same-owner detach boundary recreates an empty generation");
        const auto evictions = coordinator.take_evictions();
        CHECK(evictions.size() == 1 && evictions.front().owner == kOwnerA &&
                  evictions.front().generation == before.status.generation,
              "same-owner detach boundary emits old-generation evictions");
    }

    // Once detach returns, the old worker generation cannot allocate more work
    // while it waits for the next clearing worker step.
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA), "detach-allocation test attaches");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        coordinator.detach(kOwnerA);
        TaggedRequest request{};
        CHECK(!coordinator.next_request(request),
              "detach return prevents request allocation before worker clear");
    }

    // A request rejected after moving away may later be reissued for the same
    // tuple. Its stale acknowledgement must not consume the newer issuance.
    {
        Coordinator coordinator;
        auto profile = tiny_profile();
        coordinator.set_profile(&profile);
        CHECK(coordinator.attach(kOwnerA), "issuance ABA test attaches");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        TaggedRequest old_request{};
        CHECK(coordinator.next_request(old_request),
              "issuance ABA test obtains original request");

        coordinator.submit_anchor(kOwnerA, 1000.0f, 8.0f);
        coordinator.acknowledge(old_request, true);
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.resident_sectors == 0,
              "away anchor rejects original publication");
        coordinator.submit_anchor(kOwnerA, 8.0f, 8.0f);
        coordinator.worker_step();
        TaggedRequest new_request{};
        CHECK(coordinator.next_request(new_request) &&
                  same_sector(new_request, old_request),
              "anchor return reissues the same sector tuple");
        CHECK(distinct_issuance(old_request, new_request),
              "reissued tuple has a distinct issuance token");

        coordinator.acknowledge(old_request, true);
        coordinator.worker_step();
        const auto after_stale = coordinator.snapshot();
        CHECK(after_stale.status.resident_sectors == 0 &&
                  after_stale.status.inflight_sectors == 1,
              "stale issuance cannot consume the later request");
        coordinator.acknowledge(new_request, true);
        coordinator.worker_step();
        CHECK(coordinator.snapshot().status.resident_sectors == 1,
              "current issuance acknowledgement publishes normally");
    }

    return check_summary();
}
