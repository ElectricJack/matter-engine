#include "sector_streaming_coordinator.h"

#include <algorithm>
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

uint32_t snapshot_count(size_t count) {
    const size_t maximum = std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(std::min(count, maximum));
}

} // namespace

bool Coordinator::attach(flecs::entity_t owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner == 0 || intended_owner_ != 0) return false;
    intended_owner_ = owner;
    intended_anchor_.reset();
    ++attachment_revision_;
    return true;
}

void Coordinator::set_profile(const matter_stream::Config* profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (profile) intended_profile_ = *profile;
    else intended_profile_.reset();
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
        pending_evictions_.push_back(
            TaggedEviction{worker_owner_, worker_generation_, eviction});
    }
}

void Coordinator::clear_worker_streamer() {
    if (streamer_) {
        streamer_->clear();
        collect_streamer_evictions();
    }
    streamer_.reset();
    issued_requests_.clear();
    worker_generation_ = 0;
}

void Coordinator::publish_snapshot(
    uint64_t attachment_revision,
    const std::optional<matter_stream::Config>& profile) {
    Snapshot next{};
    next.owner = worker_owner_;
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
    uint64_t restart_revision = 0;
    std::optional<matter_stream::Config> profile;
    std::optional<AnchorSample> anchor;
    std::vector<Acknowledgement> acknowledgements;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        intended_owner = intended_owner_;
        attachment_revision = attachment_revision_;
        profile_revision = profile_revision_;
        restart_revision = restart_revision_;
        profile = intended_profile_;
        anchor = intended_anchor_;
        acknowledgements.swap(acknowledgement_inbox_);
    }

    const bool owner_changed = worker_owner_ != intended_owner;
    const bool attachment_changed =
        applied_attachment_revision_ != attachment_revision;
    const bool profile_changed = applied_profile_revision_ != profile_revision;
    const bool restart_requested = applied_restart_revision_ != restart_revision;

    if (owner_changed || attachment_changed || profile_changed ||
        restart_requested) {
        clear_worker_streamer();
    }
    worker_owner_ = intended_owner;
    worker_anchor_ = anchor;
    applied_attachment_revision_ = attachment_revision;
    applied_profile_revision_ = profile_revision;
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

        const auto sector = issued->sector;
        issued_requests_.erase(issued);
        if (acknowledgement.published) {
            streamer_->on_published(sector.tx, sector.tz, sector.rung);
        } else {
            streamer_->on_failed(sector.tx, sector.tz, sector.rung);
        }
    }

    collect_streamer_evictions();
    publish_snapshot(attachment_revision, profile);
}

bool Coordinator::next_request(TaggedRequest& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!streamer_ || intended_owner_ != worker_owner_ ||
        attachment_revision_ != applied_attachment_revision_) {
        return false;
    }
    matter_stream::SectorRequest sector{};
    if (!streamer_->next_request(sector)) return false;
    out = TaggedRequest{
        worker_owner_, worker_generation_, allocate_issuance(), sector};
    issued_requests_.push_back(out);
    return true;
}

std::vector<TaggedEviction> Coordinator::take_evictions() {
    std::vector<TaggedEviction> result;
    result.swap(pending_evictions_);
    return result;
}

} // namespace matter::streaming::detail
