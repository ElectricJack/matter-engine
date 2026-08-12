#include "sector_streaming_coordinator.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace matter::streaming::detail {

namespace {

// SELECTION TICK TIMING (MATTER_STREAM_TICK_TRACE=1).
//
// `SectorStreamer::update` is the whole selection pass -- the descent, the
// restriction fixpoint and the eviction sweep -- and until now nothing timed
// it. The nested-sector work quotes 0.367 ms on StreamMountain as the number to
// beat, but that figure came from an external profile capture; there was no
// counter in the tree that would tell you whether the octree's 8-child descent
// and 6-face probes had cost anything at all.
//
// Deliberately its OWN env rather than riding MATTER_SEAM_TRACE: the seam soak
// prints a line per frame already, and a tick histogram interleaved into that
// is unreadable. Deliberately a SUMMARY rather than per-tick lines, because a
// per-tick printf on this path would itself be a meaningful share of the thing
// being measured -- the mistake the per-sector bake logging already made once
// (docs/sector-bake-time-findings-2026-07-30.md: logging halved throughput).
bool stream_tick_trace() {
    static const bool value = [] {
        const char* env = std::getenv("MATTER_STREAM_TICK_TRACE");
        return env != nullptr && env[0] == '1';
    }();
    return value;
}

struct TickStats {
    long long calls = 0;
    double    total_us = 0.0;
    double    max_us = 0.0;
    // Reported every `kReportEvery` ticks so a long soak yields a series rather
    // than one number at exit -- fill and steady state cost different amounts,
    // and a single average hides which one you are looking at.
    static constexpr long long kReportEvery = 600;
};
TickStats& tick_stats() {
    static TickStats s;
    return s;
}

bool same_request(const TaggedRequest& lhs, const TaggedRequest& rhs) {
    return lhs.owner == rhs.owner && lhs.generation == rhs.generation &&
           lhs.issuance == rhs.issuance &&
           lhs.sector.tx == rhs.sector.tx &&
           lhs.sector.ty == rhs.sector.ty &&
           lhs.sector.tz == rhs.sector.tz &&
           lhs.sector.rung == rhs.sector.rung;
}

bool same_eviction(const TaggedEviction& lhs, const TaggedEviction& rhs) {
    return lhs.owner == rhs.owner && lhs.generation == rhs.generation &&
           lhs.issuance == rhs.issuance &&
           lhs.sector.tx == rhs.sector.tx &&
           lhs.sector.ty == rhs.sector.ty &&
           lhs.sector.tz == rhs.sector.tz &&
           lhs.sector.rung == rhs.sector.rung;
}

bool same_sector(
    const TaggedRequest& request,
    const matter_stream::Eviction& eviction) {
    return request.sector.tx == eviction.tx &&
           request.sector.ty == eviction.ty &&
           request.sector.tz == eviction.tz &&
           request.sector.rung == eviction.rung;
}

uint32_t snapshot_count(size_t count) {
    const size_t maximum = std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(std::min(count, maximum));
}

void set_error_noexcept(std::string& error, const char* message) noexcept {
    try {
        if (error.empty()) error = message;
    } catch (...) {
        // Error text is diagnostic only. Retaining ownership is the contract.
    }
}

} // namespace

void run_idle_worker_step_noexcept(
    void* step_context,
    IdleWorkerStep step,
    void* failure_context,
    IdleWorkerFailureHandler failure_handler) noexcept {
    const auto report = [&](IdleWorkerFailure failure, const char* message) {
        if (!failure_handler) return;
        try {
            failure_handler(failure_context, failure, message);
        } catch (...) {
            // Reporting is best-effort; it must never terminate the worker.
        }
    };
    try {
        if (step) step(step_context);
    } catch (const std::bad_alloc&) {
        report(IdleWorkerFailure::OutOfMemory, "std::bad_alloc");
    } catch (const std::exception& exception) {
        report(IdleWorkerFailure::Internal, exception.what());
    } catch (...) {
        report(IdleWorkerFailure::Internal, "unknown idle worker failure");
    }
}

bool PublicationCompletionCapacity::try_reserve(size_t& slot) noexcept {
    if (size_ == kCapacity) return false;
    for (size_t index = 0; index < occupied_.size(); ++index) {
        if (occupied_[index]) continue;
        occupied_[index] = true;
        ++size_;
        slot = index;
        return true;
    }
    return false;
}

void PublicationCompletionCapacity::release(size_t slot) noexcept {
    if (slot >= occupied_.size() || !occupied_[slot]) return;
    occupied_[slot] = false;
    --size_;
}

void PublicationCompletionCapacity::clear() noexcept {
    occupied_.fill(false);
    size_ = 0;
}

bool PublicationCompletionCapacity::empty() const noexcept {
    return size_ == 0;
}

bool PublicationCompletionCapacity::full() const noexcept {
    return size_ == kCapacity;
}

size_t PublicationCompletionCapacity::size() const noexcept {
    return size_;
}

bool PendingEvictionBatch::append(
    const std::vector<TaggedEviction>& evictions,
    void* fault_context,
    EvictionTransferFault fault) noexcept {
    static_assert(
        std::is_nothrow_copy_constructible_v<TaggedEviction>,
        "reserved eviction copies must not throw");
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t additions = 0;
        for (size_t index = 0; index < evictions.size(); ++index) {
            const auto& eviction = evictions[index];
            const bool already_pending = std::any_of(
                pending_.begin(), pending_.end(),
                [&](const TaggedEviction& current) {
                    return same_eviction(current, eviction);
                });
            const bool repeated_in_source = std::any_of(
                evictions.begin(), evictions.begin() + index,
                [&](const TaggedEviction& current) {
                    return same_eviction(current, eviction);
                });
            if (!already_pending && !repeated_in_source) ++additions;
        }

        if (additions > pending_.max_size() - pending_.size()) return false;
        if (fault) {
            fault(
                fault_context,
                EvictionTransferStage::CoordinatorToPendingBatchReserve);
        }
        pending_.reserve(pending_.size() + additions);
        for (size_t index = 0; index < evictions.size(); ++index) {
            const auto& eviction = evictions[index];
            const bool duplicate = std::any_of(
                pending_.begin(), pending_.end(),
                [&](const TaggedEviction& current) {
                    return same_eviction(current, eviction);
                });
            if (!duplicate) pending_.push_back(eviction);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool PendingEvictionBatch::apply(
    const Endpoint& endpoint,
    std::string& error) noexcept {
    try {
        std::vector<TaggedEviction> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = pending_;
        }
        if (snapshot.empty()) return true;
        if (!endpoint(snapshot, error)) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(
            std::remove_if(
                pending_.begin(), pending_.end(),
                [&](const TaggedEviction& current) {
                    return std::any_of(
                        snapshot.begin(), snapshot.end(),
                        [&](const TaggedEviction& applied) {
                            return same_eviction(current, applied);
                        });
                }),
            pending_.end());
        return true;
    } catch (const std::exception& exception) {
        set_error_noexcept(error, exception.what());
    } catch (...) {
        set_error_noexcept(error, "unknown eviction batch failure");
    }
    return false;
}

bool PendingEvictionBatch::apply_tag(
    const TaggedEviction& eviction,
    const Endpoint& endpoint,
    std::string& error) noexcept {
    try {
        // Inline publication ownership is deliberately disjoint from the
        // lifecycle FIFO retained in pending_. Its durable completion slot
        // owns this tag until the endpoint reports success.
        const std::vector<TaggedEviction> snapshot{eviction};
        return endpoint(snapshot, error);
    } catch (const std::exception& exception) {
        set_error_noexcept(error, exception.what());
    } catch (...) {
        set_error_noexcept(error, "unknown tagged eviction failure");
    }
    return false;
}

void PendingEvictionBatch::abandon_noexcept() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
    } catch (...) {
        // Called only after the worker is joined. A valid mutex does not fail;
        // the catch preserves the terminal no-throw boundary defensively.
    }
}

bool PendingEvictionBatch::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.empty();
}

size_t PendingEvictionBatch::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

PublicationTransaction::PublicationTransaction(
    void* context,
    Rollback rollback,
    Acknowledge acknowledge) noexcept
    : context_(context),
      rollback_(rollback),
      acknowledge_(acknowledge) {}

PublicationTransaction::~PublicationTransaction() noexcept {
    if (!active_) return;
    if (publish_intent_) {
        commit();
    } else {
        std::string ignored;
        fail(ignored);
    }
}

bool PublicationTransaction::fail(std::string& error) noexcept {
    if (!active_) return true;
    if (!rollback_complete_) {
        if (rollback_ && !rollback_(context_, error)) return false;
        rollback_complete_ = true;
    }
    if (acknowledge_ && !acknowledge_(context_, false)) return false;
    active_ = false;
    return true;
}

bool PublicationTransaction::commit() noexcept {
    if (!active_) return true;
    publish_intent_ = true;
    if (acknowledge_ && !acknowledge_(context_, true)) return false;
    active_ = false;
    return true;
}

bool PublicationTransaction::active() const noexcept {
    return active_;
}

void Coordinator::submit_visible(flecs::entity_t owner, uint64_t frame,
                                 std::vector<matter_stream::SectorRequest> drawn) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Dropped outright if it is not for the attached owner. A visibility batch
    // from a detached or superseded owner describes a world the streamer is no
    // longer serving, and applying it would stamp keys that mean something else
    // now -- the same reason submit_anchor checks.
    if (owner == 0 || owner != intended_owner_) return;
    intended_visible_ = std::move(drawn);
    intended_visible_frame_ = frame;
    intended_visible_pending_ = true;
}

bool Coordinator::attach(flecs::entity_t owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner == 0 || intended_owner_ != 0) return false;
    intended_owner_ = owner;
    intended_anchor_.reset();
    ++attachment_revision_;
    return true;
}

flecs::entity_t Coordinator::intended_owner() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return intended_owner_;
}

void Coordinator::set_profile(
    const matter_stream::Config* profile,
    SectorStreamingErrorCode profile_error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (profile) {
        intended_profile_ = *profile;
        intended_profile_error_ = SectorStreamingErrorCode::None;
    } else {
        intended_profile_.reset();
        intended_profile_error_ = profile_error;
    }
    ++profile_revision_;
}

void Coordinator::submit_anchor(flecs::entity_t owner, float x, float y,
                                float z) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner == 0 || intended_owner_ != owner) return;
    const uint64_t generation = published_snapshot_.owner == owner
        ? published_snapshot_.status.generation
        : 0;
    intended_anchor_ = AnchorSample{owner, generation, x, y, z};
}

void Coordinator::clear_anchor(flecs::entity_t owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner == 0 || intended_owner_ != owner || !intended_anchor_) return;
    intended_anchor_.reset();
    ++anchor_reset_revision_;
}

void Coordinator::detach(flecs::entity_t owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner == 0 || intended_owner_ != owner) return;
    intended_owner_ = 0;
    intended_anchor_.reset();
    ++attachment_revision_;
    published_snapshot_ = Snapshot{};
}

void Coordinator::restart_if_attached() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (intended_owner_ != 0) ++restart_revision_;
}

bool Coordinator::acknowledge(
    const TaggedRequest& request,
    bool published) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        acknowledgement_inbox_.push_back(Acknowledgement{request, published});
        return true;
    } catch (...) {
        return false;
    }
}

Snapshot Coordinator::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_snapshot_;
}

uint64_t Coordinator::allocate_generation() {
    ++last_generation_;
    if (last_generation_ == 0) ++last_generation_;
    return last_generation_;
}

uint64_t Coordinator::allocate_issuance() {
    ++last_issuance_;
    if (last_issuance_ == 0) ++last_issuance_;
    return last_issuance_;
}

void Coordinator::collect_streamer_evictions(
    void* fault_context,
    EvictionTransferFault fault) {
    if (!streamer_) return;
    static_assert(
        std::is_nothrow_copy_constructible_v<TaggedEviction>,
        "reserved tagged eviction copies must not throw");
    const auto& evictions = streamer_->peek_evictions();
    const size_t source_count = evictions.size();
    if (source_count == 0) return;
    if (fault) {
        fault(
            fault_context,
            EvictionTransferStage::StreamerToCoordinatorReserve);
    }
    const size_t destination_start = pending_evictions_.size();
    pending_evictions_.reserve(pending_evictions_.size() + evictions.size());
    for (const auto& eviction : evictions) {
        const auto resident = std::find_if(
            resident_requests_.begin(), resident_requests_.end(),
            [&](const TaggedRequest& request) {
                return same_sector(request, eviction);
            });
        const uint64_t issuance = resident == resident_requests_.end()
            ? 0
            : resident->issuance;
        pending_evictions_.push_back(
            TaggedEviction{
                worker_owner_, worker_generation_, issuance, eviction});
    }
    if (!streamer_->commit_evictions(evictions, source_count)) {
        pending_evictions_.resize(destination_start);
        return;
    }
    for (auto first = pending_evictions_.begin() + destination_start;
         first != pending_evictions_.end(); ++first) {
        const auto resident = std::find_if(
            resident_requests_.begin(), resident_requests_.end(),
            [&](const TaggedRequest& request) {
                return same_sector(request, first->sector);
            });
        if (resident != resident_requests_.end()) resident_requests_.erase(resident);
    }
}

void Coordinator::invalidate_worker_publications() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& request : publishing_requests_) {
        if (request.owner != worker_owner_ ||
            request.generation != worker_generation_) {
            continue;
        }
        pending_evictions_.push_back(TaggedEviction{
            request.owner,
            request.generation,
            request.issuance,
            matter_stream::Eviction{
                request.sector.tx,
                request.sector.ty,
                request.sector.tz,
                request.sector.rung}});
    }
    publication_candidates_.clear();
    publishing_requests_.clear();
}

void Coordinator::clear_worker_streamer(
    void* fault_context,
    EvictionTransferFault fault) {
    invalidate_worker_publications();
    if (streamer_) {
        streamer_->clear();
        collect_streamer_evictions(fault_context, fault);
    }
    streamer_.reset();
    issued_requests_.clear();
    resident_requests_.clear();
    worker_generation_ = 0;
}

void Coordinator::publish_snapshot(
    uint64_t attachment_revision,
    const std::optional<matter_stream::Config>& profile,
    SectorStreamingErrorCode profile_error) {
    Snapshot next{};
    next.owner = worker_owner_;
    next.error.code = profile_error;
    if (worker_owner_ == 0) {
        next.status.state = SectorStreamingState::Detached;
    } else if (!profile) {
        next.status.state = SectorStreamingState::PendingProfile;
    } else if (!worker_anchor_) {
        next.status.state = SectorStreamingState::PendingTransform;
    } else {
        next.status.state = SectorStreamingState::Active;
        next.status.generation = worker_generation_;
        next.status.resident_sectors = snapshot_count(streamer_->resident_count());
        next.status.inflight_sectors = snapshot_count(streamer_->inflight_count());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (attachment_revision_ == attachment_revision) published_snapshot_ = next;
}

void Coordinator::worker_step(
    void* fault_context,
    EvictionTransferFault fault) {
    flecs::entity_t intended_owner = 0;
    uint64_t attachment_revision = 0;
    uint64_t profile_revision = 0;
    uint64_t anchor_reset_revision = 0;
    uint64_t restart_revision = 0;
    std::optional<matter_stream::Config> profile;
    SectorStreamingErrorCode profile_error = SectorStreamingErrorCode::None;
    std::optional<AnchorSample> anchor;
    std::vector<Acknowledgement> acknowledgements;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        intended_owner = intended_owner_;
        attachment_revision = attachment_revision_;
        profile_revision = profile_revision_;
        anchor_reset_revision = anchor_reset_revision_;
        restart_revision = restart_revision_;
        profile = intended_profile_;
        profile_error = intended_profile_error_;
        anchor = intended_anchor_;
        acknowledgements.swap(acknowledgement_inbox_);
    }

    const bool owner_changed = worker_owner_ != intended_owner;
    const bool attachment_changed =
        applied_attachment_revision_ != attachment_revision;
    const bool profile_changed = applied_profile_revision_ != profile_revision;
    const bool anchor_reset_requested =
        applied_anchor_reset_revision_ != anchor_reset_revision;
    const bool restart_requested = applied_restart_revision_ != restart_revision;
    const bool anchor_lost = worker_anchor_.has_value() && !anchor.has_value();

    if (owner_changed || attachment_changed || profile_changed ||
        anchor_reset_requested || restart_requested || anchor_lost) {
        clear_worker_streamer(fault_context, fault);
    }
    worker_owner_ = intended_owner;
    worker_anchor_ = anchor;
    applied_attachment_revision_ = attachment_revision;
    applied_profile_revision_ = profile_revision;
    applied_anchor_reset_revision_ = anchor_reset_revision;
    applied_restart_revision_ = restart_revision;

    if (worker_owner_ != 0 && profile && worker_anchor_) {
        if (!streamer_) {
            streamer_ = std::make_unique<matter_stream::SectorStreamer>(*profile);
            worker_generation_ = allocate_generation();
        }
        // Apply the pending visible batch BEFORE the tick that will read it,
        // so a frame's report affects the very next selection rather than the
        // one after -- one tick of avoidable lag on an input that is already
        // several frames stale by construction.
        if (intended_visible_pending_) {
            streamer_->set_visibility_frame(intended_visible_frame_);
            for (const auto& tile : intended_visible_)
                streamer_->mark_visible(tile.tx, tile.ty, tile.tz, tile.rung);
            intended_visible_.clear();
            intended_visible_pending_ = false;
        }
        if (stream_tick_trace()) {
            const auto t0 = std::chrono::steady_clock::now();
            streamer_->update(worker_anchor_->x, worker_anchor_->y,
                              worker_anchor_->z);
            const double us =
                std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - t0).count();
            TickStats& s = tick_stats();
            ++s.calls;
            s.total_us += us;
            s.max_us = s.max_us > us ? s.max_us : us;
            if (s.calls % TickStats::kReportEvery == 0) {
                std::fprintf(stderr,
                             "[stream-tick] %lld ticks | mean %.3f ms | max "
                             "%.3f ms | resident %zu\n",
                             s.calls, s.total_us / double(s.calls) / 1000.0,
                             s.max_us / 1000.0, streamer_->resident_count());
            }
        } else {
            streamer_->update(worker_anchor_->x, worker_anchor_->y,
                              worker_anchor_->z);
        }
    }

    for (const auto& acknowledgement : acknowledgements) {
        if (!streamer_ || acknowledgement.request.owner != worker_owner_ ||
            acknowledgement.request.generation != worker_generation_) {
            continue;
        }
        const auto issued = std::find_if(
            issued_requests_.begin(), issued_requests_.end(),
            [&](const TaggedRequest& request) {
                return same_request(request, acknowledgement.request);
            });
        if (issued == issued_requests_.end()) continue;

        const TaggedRequest completed = *issued;
        const auto sector = completed.sector;
        issued_requests_.erase(issued);
        bool began_publication = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto candidate = std::find_if(
                publication_candidates_.begin(), publication_candidates_.end(),
                [&](const TaggedRequest& request) {
                    return same_request(request, completed);
                });
            if (candidate != publication_candidates_.end()) {
                publication_candidates_.erase(candidate);
            }
            const auto publishing = std::find_if(
                publishing_requests_.begin(), publishing_requests_.end(),
                [&](const TaggedRequest& request) {
                    return same_request(request, completed);
                });
            if (publishing != publishing_requests_.end()) {
                began_publication = true;
                publishing_requests_.erase(publishing);
            }
        }
        if (acknowledgement.published) {
            if (streamer_->on_published(
                    sector.tx, sector.ty, sector.tz, sector.rung)) {
                resident_requests_.push_back(completed);
            } else if (began_publication) {
                pending_evictions_.push_back(TaggedEviction{
                    completed.owner,
                    completed.generation,
                    completed.issuance,
                    matter_stream::Eviction{
                        sector.tx, sector.ty, sector.tz, sector.rung}});
            }
        } else {
            streamer_->on_failed(sector.tx, sector.ty, sector.tz, sector.rung);
        }
    }

    collect_streamer_evictions(fault_context, fault);
    publish_snapshot(attachment_revision, profile, profile_error);
}

bool Coordinator::next_request(
    TaggedRequest& out,
    void* fault_context,
    RequestTrackingFault fault) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!streamer_ || intended_owner_ != worker_owner_ ||
            attachment_revision_ != applied_attachment_revision_ ||
            anchor_reset_revision_ != applied_anchor_reset_revision_) {
            return false;
        }

        // Complete every allocation before SectorStreamer mutates inflight.
        // TaggedRequest is trivially movable, so the two push_back calls cannot
        // allocate or throw after these reserves succeed.
        issued_requests_.reserve(issued_requests_.size() + 1);
        publication_candidates_.reserve(publication_candidates_.size() + 1);

        matter_stream::SectorRequest sector{};
        if (!streamer_->next_request(sector)) return false;
        const TaggedRequest request{
            worker_owner_, worker_generation_, allocate_issuance(), sector};
        out = request;

        try {
            if (fault) fault(fault_context, RequestTrackingStage::IssuedRequest);
            issued_requests_.push_back(request);
            if (fault) {
                fault(fault_context, RequestTrackingStage::PublicationCandidate);
            }
            publication_candidates_.push_back(request);
            return true;
        } catch (...) {
            const auto erase_request = [&](std::vector<TaggedRequest>& tracked) {
                const auto match = std::find_if(
                    tracked.begin(), tracked.end(),
                    [&](const TaggedRequest& current) {
                        return same_request(current, request);
                    });
                if (match != tracked.end()) tracked.erase(match);
            };
            erase_request(publication_candidates_);
            erase_request(issued_requests_);
            streamer_->cancel_request(
                sector.tx, sector.ty, sector.tz, sector.rung);
            return false;
        }
    } catch (...) {
        return false;
    }
}

bool Coordinator::begin_publication(
    const TaggedRequest& request) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (published_snapshot_.owner != request.owner ||
            published_snapshot_.status.state != SectorStreamingState::Active ||
            published_snapshot_.status.generation != request.generation) {
            return false;
        }
        const auto candidate = std::find_if(
            publication_candidates_.begin(), publication_candidates_.end(),
            [&](const TaggedRequest& current) {
                return same_request(current, request);
            });
        if (candidate == publication_candidates_.end()) return false;
        // Copy before erasing. If allocation fails, the request remains a
        // publication candidate and the caller can durably acknowledge false.
        publishing_requests_.push_back(*candidate);
        publication_candidates_.erase(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool Coordinator::transfer_evictions(
    PendingEvictionBatch& destination,
    void* fault_context,
    EvictionTransferFault fault) noexcept {
    if (!destination.append(pending_evictions_, fault_context, fault)) {
        return false;
    }
    pending_evictions_.clear();
    return true;
}

std::vector<TaggedEviction> Coordinator::take_evictions() {
    std::vector<TaggedEviction> result;
    result.swap(pending_evictions_);
    return result;
}

void Coordinator::terminal_clear() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        intended_owner_ = 0;
        intended_profile_.reset();
        intended_profile_error_ = SectorStreamingErrorCode::None;
        intended_anchor_.reset();
        acknowledgement_inbox_.clear();
        published_snapshot_ = Snapshot{};
        worker_owner_ = 0;
        worker_generation_ = 0;
        worker_anchor_.reset();
        streamer_.reset();
        issued_requests_.clear();
        publication_candidates_.clear();
        publishing_requests_.clear();
        resident_requests_.clear();
        pending_evictions_.clear();
    } catch (...) {
        // This runs only after worker ownership has ended. Clearing these
        // trivial/value containers is non-allocating on all supported STLs.
    }
}

void ProfileActivationGate::stage(const matter_stream::Config& profile) {
    staged_ = profile;
}

void ProfileActivationGate::fail(Coordinator& coordinator) {
    staged_.reset();
    active_.reset();
    clearing_ = false;
    coordinator.set_profile(nullptr);
}

bool ProfileActivationGate::publish(Coordinator& coordinator) {
    if (!staged_) return false;
    try {
        coordinator.set_profile(&*staged_);
        active_ = std::move(staged_);
        staged_.reset();
        clearing_ = false;
        return true;
    } catch (...) {
        return false;
    }
}

void ProfileActivationGate::begin_clear(Coordinator& coordinator) {
    staged_.reset();
    clearing_ = true;
    coordinator.set_profile(nullptr);
}

void ProfileActivationGate::finish_clear() noexcept {
    staged_.reset();
    active_.reset();
    clearing_ = false;
}

bool ProfileActivationGate::abort_clear(Coordinator& coordinator) {
    if (!clearing_) return true;
    try {
        if (active_) coordinator.set_profile(&*active_);
        else coordinator.set_profile(nullptr);
        staged_.reset();
        clearing_ = false;
        return true;
    } catch (...) {
        return false;
    }
}

bool ProfileActivationGate::pending() const noexcept {
    return staged_.has_value();
}

} // namespace matter::streaming::detail
