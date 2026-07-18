#include "sector_streaming_coordinator.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace matter::streaming::detail {

namespace {

bool same_request(const TaggedRequest& lhs, const TaggedRequest& rhs) {
    return lhs.owner == rhs.owner && lhs.generation == rhs.generation &&
           lhs.issuance == rhs.issuance &&
           lhs.sector.tx == rhs.sector.tx &&
           lhs.sector.tz == rhs.sector.tz &&
           lhs.sector.rung == rhs.sector.rung;
}

bool same_eviction(const TaggedEviction& lhs, const TaggedEviction& rhs) {
    return lhs.owner == rhs.owner && lhs.generation == rhs.generation &&
           lhs.issuance == rhs.issuance &&
           lhs.sector.tx == rhs.sector.tx &&
           lhs.sector.tz == rhs.sector.tz &&
           lhs.sector.rung == rhs.sector.rung;
}

bool same_sector(
    const TaggedRequest& request,
    const matter_stream::Eviction& eviction) {
    return request.sector.tx == eviction.tx &&
           request.sector.tz == eviction.tz &&
           request.sector.rung == eviction.rung;
}

uint32_t snapshot_count(size_t count) {
    const size_t maximum = std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(std::min(count, maximum));
}

} // namespace

void PendingEvictionBatch::append(std::vector<TaggedEviction> evictions) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& eviction : evictions) {
        const auto duplicate = std::find_if(
            pending_.begin(), pending_.end(),
            [&](const TaggedEviction& current) {
                return same_eviction(current, eviction);
            });
        if (duplicate == pending_.end()) {
            pending_.push_back(std::move(eviction));
        }
    }
}

bool PendingEvictionBatch::apply(
    const Endpoint& endpoint,
    std::string& error) {
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
    Rollback rollback,
    Acknowledge acknowledge)
    : rollback_(std::move(rollback)),
      acknowledge_(std::move(acknowledge)) {}

PublicationTransaction::~PublicationTransaction() {
    if (!active_) return;
    std::string ignored;
    fail(ignored);
}

bool PublicationTransaction::fail(std::string& error) {
    if (!active_) return true;
    bool rolled_back = false;
    try {
        rolled_back = rollback_(error);
    } catch (const std::exception& exception) {
        if (error.empty()) error = exception.what();
    } catch (...) {
        if (error.empty()) error = "unknown publication rollback failure";
    }

    bool acknowledged = true;
    try {
        acknowledge_(false);
    } catch (const std::exception& exception) {
        acknowledged = false;
        if (error.empty()) error = exception.what();
    } catch (...) {
        acknowledged = false;
        if (error.empty()) error = "unknown publication acknowledgement failure";
    }
    active_ = false;
    return rolled_back && acknowledged;
}

void PublicationTransaction::commit() {
    acknowledge_(true);
    active_ = false;
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

void Coordinator::submit_anchor(flecs::entity_t owner, float x, float z) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner == 0 || intended_owner_ != owner) return;
    const uint64_t generation = published_snapshot_.owner == owner
        ? published_snapshot_.status.generation
        : 0;
    intended_anchor_ = AnchorSample{owner, generation, x, z};
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

void Coordinator::acknowledge(const TaggedRequest& request, bool published) {
    std::lock_guard<std::mutex> lock(mutex_);
    acknowledgement_inbox_.push_back(Acknowledgement{request, published});
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

void Coordinator::collect_streamer_evictions() {
    if (!streamer_) return;
    auto evictions = streamer_->take_evictions();
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
        if (resident != resident_requests_.end()) {
            resident_requests_.erase(resident);
        }
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
                request.sector.tz,
                request.sector.rung}});
    }
    publication_candidates_.clear();
    publishing_requests_.clear();
}

void Coordinator::clear_worker_streamer() {
    invalidate_worker_publications();
    if (streamer_) {
        streamer_->clear();
        collect_streamer_evictions();
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

void Coordinator::worker_step() {
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
        clear_worker_streamer();
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
        streamer_->update(worker_anchor_->x, worker_anchor_->z);
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
                    sector.tx, sector.tz, sector.rung)) {
                resident_requests_.push_back(completed);
            } else if (began_publication) {
                pending_evictions_.push_back(TaggedEviction{
                    completed.owner,
                    completed.generation,
                    completed.issuance,
                    matter_stream::Eviction{
                        sector.tx, sector.tz, sector.rung}});
            }
        } else {
            streamer_->on_failed(sector.tx, sector.tz, sector.rung);
        }
    }

    collect_streamer_evictions();
    publish_snapshot(attachment_revision, profile, profile_error);
}

bool Coordinator::next_request(TaggedRequest& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!streamer_ || intended_owner_ != worker_owner_ ||
        attachment_revision_ != applied_attachment_revision_ ||
        anchor_reset_revision_ != applied_anchor_reset_revision_) {
        return false;
    }
    matter_stream::SectorRequest sector{};
    if (!streamer_->next_request(sector)) return false;
    out = TaggedRequest{
        worker_owner_, worker_generation_, allocate_issuance(), sector};
    issued_requests_.push_back(out);
    publication_candidates_.push_back(out);
    return true;
}

bool Coordinator::begin_publication(const TaggedRequest& request) {
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
    publishing_requests_.push_back(*candidate);
    publication_candidates_.erase(candidate);
    return true;
}

std::vector<TaggedEviction> Coordinator::take_evictions() {
    std::vector<TaggedEviction> result;
    result.swap(pending_evictions_);
    return result;
}

void ProfileActivationGate::stage(const matter_stream::Config& profile) {
    staged_ = profile;
}

void ProfileActivationGate::fail(Coordinator& coordinator) {
    staged_.reset();
    coordinator.set_profile(nullptr);
}

bool ProfileActivationGate::publish(Coordinator& coordinator) {
    if (!staged_) return false;
    coordinator.set_profile(&*staged_);
    staged_.reset();
    return true;
}

bool ProfileActivationGate::pending() const noexcept {
    return staged_.has_value();
}

} // namespace matter::streaming::detail
