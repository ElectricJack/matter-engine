#include "geometry_residency.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace geometry {
namespace {
bool same(const NodeRef& a, const NodeRef& b) {
    if (a.page != b.page || a.error != b.error || a.triangles != b.triangles ||
        a.source_triangles != b.source_triangles) return false;
    for (int k = 0; k < 3; ++k)
        if (a.bounds.lo[k] != b.bounds.lo[k] || a.bounds.hi[k] != b.bounds.hi[k]) return false;
    return true;
}
}
struct Residency::Impl {
    struct Ledger { std::atomic<uint64_t> gpu{0}, scratch{0}; };
    struct Reservation {
        std::shared_ptr<Ledger> ledger;
        uint64_t gpu = 0, scratch = 0;
        ~Reservation() { ledger->gpu.fetch_sub(gpu); ledger->scratch.fetch_sub(scratch); }
        void release_scratch() { ledger->scratch.fetch_sub(scratch); scratch = 0; }
    };
    enum class State { Queued, Reading, Decoded, Uploading, Ready, Failed };
    struct Page {
        NodeRef ref;
        State state = State::Queued;
        uint64_t issuance = 0, retry_epoch = 0;
        uint32_t attempts = 0;
        float priority = 0;
        std::set<uint64_t> owners, roots;
        NodeView decoded;
        std::shared_ptr<Reservation> reservation;
        ResidentHandle resident;
    };
    struct Asset {
        asset_store::PageHandle manifest;
        std::vector<NodeRef> roots;
        // Ready roots cannot be evicted while this lease owns them.
        // Cache only success; incomplete uploads are checked again.
        mutable bool roots_ready = false;
        std::map<asset_store::BlobHash, NodeRef> known;
    };
    ResidencyConfig cfg;
    std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
    std::map<uint64_t, Asset> assets;
    std::map<asset_store::BlobHash, Page> pages;
    std::vector<uint64_t> visible_assets;
    bool visible(const Page& page) const {
        for(auto owner:page.owners)
            if(std::binary_search(visible_assets.begin(),visible_assets.end(),owner))return true;
        return false;
    }
    struct QueueKey {
        bool visible;
        bool root;
        float priority;
        asset_store::BlobHash hash;
        bool operator<(const QueueKey& other) const {
            if (visible != other.visible) return visible;
            if (root != other.root) return root;
            if (priority != other.priority) return priority > other.priority;
            return hash < other.hash;
        }
    };
    // Keep ordering at mutation time. Dispatch must not scan and sort the
    // entire terrain backlog just to issue a small batch of reads.
    std::set<QueueKey> ready_queue;
    std::set<std::pair<uint64_t, asset_store::BlobHash>> delayed_queue;
    void unqueue(const Page& page) {
        ready_queue.erase({visible(page), !page.roots.empty(), page.priority, page.ref.page});
        delayed_queue.erase({page.retry_epoch, page.ref.page});
    }
    void queue(const Page& page) {
        if (page.state != State::Queued) return;
        if (page.retry_epoch) delayed_queue.emplace(page.retry_epoch, page.ref.page);
        else ready_queue.insert({visible(page), !page.roots.empty(), page.priority, page.ref.page});
    }
    uint32_t active_pages = 0;
    uint64_t next_lease = 1, next_issuance = 1;
    ResidencyStats counters;
    bool active(State s) const { return s == State::Reading || s == State::Decoded || s == State::Uploading; }
    uint32_t inflight() const { return active_pages; }
    Page* ticket(PageTicket t) {
        auto it = pages.find(t.page);
        if (!t || it == pages.end() || it->second.issuance != t.issuance || !active(it->second.state)) {
            ++counters.stale_completions; return nullptr;
        }
        return &it->second;
    }
    void discover(uint64_t owner, const NodeView& node) {
        auto it = assets.find(owner); if (it == assets.end()) return;
        for (const auto& child : node.children) {
            if (it->second.known.count(child.page)) continue;
            if (counters.known_nodes >= cfg.max_known_nodes) { ++counters.budget_rejections; break; }
            it->second.known.emplace(child.page, child); ++counters.known_nodes;
        }
    }
};
Residency::Residency(ResidencyConfig cfg) : d_(new Impl) { d_->cfg = cfg; }
Residency::~Residency() = default;
AssetLease Residency::attach(asset_store::PageHandle manifest, std::string& error) {
    auto& d = *d_;
    if (d.assets.size() >= d.cfg.max_assets || d.next_lease == UINT64_MAX) {
        error = "geometry asset admission limit"; return {};
    }
    Impl::Asset asset; asset.manifest = std::move(manifest);
    if (!decode_roots(asset.manifest, asset.roots, error)) return {};
    if (asset.roots.size() > d.cfg.max_known_nodes - std::min(d.counters.known_nodes, d.cfg.max_known_nodes)) {
        error = "geometry metadata admission limit"; return {};
    }
    size_t additional = 0;
    for (const auto& root : asset.roots) {
        auto existing = d.pages.find(root.page);
        if (existing != d.pages.end() && !same(root, existing->second.ref)) {
            error = "conflicting geometry root descriptor"; return {};
        }
        if (existing == d.pages.end()) ++additional;
        asset.known.emplace(root.page, root);
    }
    if (additional > d.cfg.max_pages - std::min<size_t>(d.pages.size(), d.cfg.max_pages)) {
        error = "geometry root page admission limit"; return {};
    }
    const AssetLease lease{d.next_lease++};
    auto inserted = d.assets.emplace(lease.id, std::move(asset));
    d.counters.known_nodes += static_cast<uint32_t>(inserted.first->second.known.size());
    for (const auto& root : inserted.first->second.roots) {
        auto inserted_page = d.pages.try_emplace(root.page);
        auto& page = inserted_page.first->second;
        if (!inserted_page.second) d.unqueue(page);
        page.ref = root;
        page.owners.insert(lease.id); page.roots.insert(lease.id);
        d.queue(page);
        if (page.resident) d.discover(lease.id, page.resident->node);
    }
    error.clear(); return lease;
}
void Residency::detach(AssetLease lease) {
    auto& d = *d_;
    auto asset = d.assets.find(lease.id);
    if (asset != d.assets.end()) {
        d.counters.known_nodes -= static_cast<uint32_t>(asset->second.known.size());
        d.assets.erase(asset);
    }
    for (auto it = d.pages.begin(); it != d.pages.end();) {
        if (!it->second.owners.count(lease.id)) { ++it; continue; }
        d.unqueue(it->second);
        it->second.owners.erase(lease.id); it->second.roots.erase(lease.id);
        if (it->second.owners.empty()) {
            if (d.active(it->second.state)) --d.active_pages;
            it = d.pages.erase(it);
        } else { d.queue(it->second); ++it; }
    }
}
bool Residency::ready(AssetLease lease) const {
    const auto& d = *d_; auto asset = d.assets.find(lease.id);
    if (asset == d.assets.end()) return false;
    if (asset->second.roots_ready) return true;
    for (const auto& root : asset->second.roots) {
        auto page = d.pages.find(root.page);
        if (page == d.pages.end() || !page->second.resident) return false;
    }
    asset->second.roots_ready = true;
    return true;
}
void Residency::set_visible_assets(std::vector<uint64_t> leases) {
    std::sort(leases.begin(),leases.end());
    leases.erase(std::unique(leases.begin(),leases.end()),leases.end());
    auto& d=*d_;
    if(leases==d.visible_assets)return;
    d.visible_assets=std::move(leases);
    // Reuse tree allocations and only visit queued work, not every resident
    // page. Delayed requests acquire current membership when requeued.
    auto previous=std::move(d.ready_queue);
    d.ready_queue.clear();
    while(!previous.empty()) {
        auto node=previous.extract(previous.begin());
        node.value().visible=d.visible(d.pages.at(node.value().hash));
        d.ready_queue.insert(std::move(node));
    }
}
bool Residency::page_visible(const asset_store::BlobHash& hash) const {
    const auto it=d_->pages.find(hash);
    return it!=d_->pages.end() && d_->visible(it->second);
}
bool Residency::request(AssetLease lease, const asset_store::BlobHash& hash, float priority) {
    auto& d = *d_; auto asset = d.assets.find(lease.id);
    if (asset == d.assets.end() || !std::isfinite(priority)) return false;
    auto known = asset->second.known.find(hash); if (known == asset->second.known.end()) return false;
    auto it = d.pages.find(hash);
    if (it == d.pages.end() && d.pages.size() >= d.cfg.max_pages) { ++d.counters.budget_rejections; return false; }
    if (it != d.pages.end() && !same(it->second.ref, known->second)) return false;
    auto inserted_page = d.pages.try_emplace(hash);
    auto& page = inserted_page.first->second;
    if (!inserted_page.second) d.unqueue(page);
    page.ref = known->second; page.owners.insert(lease.id);
    page.priority = std::max(page.priority, priority);
    if (page.state == Impl::State::Failed) {
        // A parked page is re-armed by an explicit request (new visibility or a
        // refreshed location), never by the retry clock. Attempts start over.
        page.state = Impl::State::Queued; page.attempts = 0; page.retry_epoch = 0;
        ++d.counters.failed_retries;
    }
    d.queue(page);
    if (page.resident) d.discover(lease.id, page.resident->node);
    return true;
}
std::vector<PageTicket> Residency::dispatch(uint32_t count, uint64_t epoch) {
    auto& d = *d_;
    count = std::min(count, d.cfg.max_inflight - std::min(d.inflight(), d.cfg.max_inflight));
    if (!count) return {};
    while (!d.delayed_queue.empty() && d.delayed_queue.begin()->first <= epoch) {
        auto& page = d.pages.at(d.delayed_queue.begin()->second);
        d.delayed_queue.erase(d.delayed_queue.begin());
        page.retry_epoch = 0;
        d.queue(page);
    }
    std::vector<PageTicket> result;
    result.reserve(count);
    while (result.size() < count && !d.ready_queue.empty() && d.next_issuance != UINT64_MAX) {
        auto& page = d.pages.at(d.ready_queue.begin()->hash);
        d.ready_queue.erase(d.ready_queue.begin());
        page.state = Impl::State::Reading; page.issuance = d.next_issuance++; ++page.attempts;
        ++d.active_pages;
        result.push_back({page.ref.page, page.issuance});
    }
    return result;
}
bool Residency::complete_read(PageTicket ticket, asset_store::PageHandle bytes, std::string& error) {
    auto& d = *d_; auto* page = d.ticket(ticket);
    if (!page || page->state != Impl::State::Reading) { error = "stale geometry read"; return false; }
    NodeView decoded;
    if (!decode_node(std::move(bytes), decoded, error) || !same(page->ref, decoded.self)) {
        error = "geometry page does not match its request"; return false;
    }
    page->decoded = std::move(decoded); page->state = Impl::State::Decoded;
    error.clear(); return true;
}
bool Residency::staged_node(PageTicket ticket, NodeView& out) {
    auto* page = d_->ticket(ticket);
    if (!page || (page->state != Impl::State::Decoded && page->state != Impl::State::Uploading)) return false;
    out = page->decoded; return true;
}
bool Residency::reserve_upload(PageTicket ticket, uint64_t gpu, uint64_t scratch) {
    auto& d = *d_; auto* page = d.ticket(ticket);
    if (!page || page->state != Impl::State::Decoded || !gpu) return false;
    if (gpu > d.cfg.gpu_bytes || scratch > d.cfg.scratch_bytes ||
        d.ledger->gpu.load() > d.cfg.gpu_bytes - gpu || d.ledger->scratch.load() > d.cfg.scratch_bytes - scratch) {
        ++d.counters.budget_rejections; return false;
    }
    auto reservation = std::make_shared<Impl::Reservation>(); reservation->ledger = d.ledger;
    reservation->gpu = gpu; reservation->scratch = scratch;
    d.ledger->gpu.fetch_add(gpu); d.ledger->scratch.fetch_add(scratch);
    page->reservation = std::move(reservation); page->state = Impl::State::Uploading; return true;
}
std::shared_ptr<const void> Residency::upload_reservation(PageTicket ticket) {
    auto* page = d_->ticket(ticket);
    return page && page->state == Impl::State::Uploading ? page->reservation : nullptr;
}
bool Residency::publish(PageTicket ticket, std::shared_ptr<const void> resource) {
    auto& d = *d_; auto* page = d.ticket(ticket);
    if (!page || page->state != Impl::State::Uploading || !resource) return false;
    std::shared_ptr<ResidentNode> resident;
    try {
        resident = std::make_shared<ResidentNode>(); resident->node = page->decoded;
        for (auto owner : page->owners) d.discover(owner, resident->node);
    } catch (const std::bad_alloc&) { return false; }
    // Everything that can allocate completed before the visible state changes.
    resident->raster_and_rt = std::move(resource); page->reservation->release_scratch();
    resident->reservation = std::move(page->reservation); page->decoded = {};
    page->resident = std::move(resident); page->state = Impl::State::Ready;
    --d.active_pages;
    return true;
}
void Residency::fail(PageTicket ticket, uint64_t retry_epoch) {
    auto& d = *d_; auto* page = d.ticket(ticket); if (!page) return;
    page->decoded = {}; page->reservation.reset(); page->retry_epoch = retry_epoch;
    page->state = page->attempts < d.cfg.max_attempts ? Impl::State::Queued : Impl::State::Failed;
    --d.active_pages;
    d.queue(*page);
}
void Residency::defer(PageTicket ticket, uint64_t retry_epoch) {
    auto& d = *d_; auto* page = d.ticket(ticket); if (!page) return;
    ++d.counters.budget_rejections;
    page->decoded = {}; page->reservation.reset(); page->retry_epoch = retry_epoch;
    if (page->attempts) --page->attempts;
    page->state = Impl::State::Queued; --d.active_pages; d.queue(*page);
}
bool Residency::evict(const asset_store::BlobHash& hash) {
    auto& d = *d_; auto page = d.pages.find(hash);
    if (page == d.pages.end()) return true;
    if (!page->second.roots.empty() || d.active(page->second.state)) return false;
    d.unqueue(page->second); d.pages.erase(page); return true;
}
bool Residency::select(AssetLease lease, const std::function<bool(const NodeRef&)>& refine,
                       const CutConfig& cfg, ResidentCut& out, std::string& error, bool queue_requests) {
    auto& d = *d_; auto asset = d.assets.find(lease.id);
    if (asset == d.assets.end()) { error = "stale geometry asset"; return false; }
    ResidentCut result;
    auto lookup = [&](const NodeRef& ref, NodeView& node) {
        auto it = d.pages.find(ref.page);
        if (it == d.pages.end() || !it->second.resident) return false;
        if (!same(ref, it->second.resident->node.self)) return false;
        it->second.owners.insert(lease.id);
        d.discover(lease.id, it->second.resident->node);
        node = it->second.resident->node; return true;
    };
    if (!select_cut(asset->second.roots, lookup, refine, cfg, result.cut, error)) return false;
    for (const auto& node : result.cut.selected) result.resources.push_back(d.pages.at(node.self.page).resident);
    if (queue_requests) for (const auto& hash : result.cut.requests) request(lease, hash);
    out = std::move(result); return true;
}
bool Residency::contains_page(const asset_store::BlobHash& hash) const { return d_->pages.count(hash) != 0; }
bool Residency::pending(PageTicket ticket) const {
    auto it = d_->pages.find(ticket.page);
    return ticket && it != d_->pages.end() && it->second.issuance == ticket.issuance && d_->active(it->second.state);
}
ResidentHandle Residency::resident(const asset_store::BlobHash& hash) const {
    auto it = d_->pages.find(hash);
    return it == d_->pages.end() ? ResidentHandle{} : it->second.resident;
}
ResidencyStats Residency::stats() const {
    auto result = d_->counters; result.assets = static_cast<uint32_t>(d_->assets.size());
    result.gpu_budget = d_->cfg.gpu_bytes; result.scratch_budget = d_->cfg.scratch_bytes;
    result.pages = static_cast<uint32_t>(d_->pages.size()); result.inflight = d_->inflight();
    result.gpu_bytes = d_->ledger->gpu.load(); result.scratch_bytes = d_->ledger->scratch.load(); return result;
}
bool Residency::snapshot(AssetLease lease, ResidentHierarchy& out, std::string& error) const {
    const auto asset = d_->assets.find(lease.id);
    if (asset == d_->assets.end()) { error = "stale geometry asset snapshot"; return false; }
    ResidentHierarchy result; result.roots = asset->second.roots;
    std::vector<NodeRef> work = result.roots;
    std::set<asset_store::BlobHash> seen;
    for (size_t i = 0; i < work.size(); ++i) {
        const auto ref = work[i];
        if (!seen.insert(ref.page).second) continue;
        if (seen.size() > d_->cfg.max_known_nodes) { error = "geometry snapshot metadata limit"; return false; }
        NodeView node; node.self = ref;
        auto found = d_->pages.find(ref.page);
        if (found != d_->pages.end() && found->second.resident && same(ref, found->second.resident->node.self)) {
            // Snapshot traversal adopts shared resident descendants for this
            // lease, just as the reference select() path does. The dense CPU
            // traversal must remain read-only between residency changes.
            found->second.owners.insert(lease.id);
            d_->discover(lease.id, found->second.resident->node);
            node = found->second.resident->node;
            result.resources.push_back(found->second.resident);
            work.insert(work.end(), node.children.begin(), node.children.end());
        }
        result.nodes.push_back(std::move(node));
    }
    out = std::move(result); error.clear(); return true;
}
} // namespace geometry
