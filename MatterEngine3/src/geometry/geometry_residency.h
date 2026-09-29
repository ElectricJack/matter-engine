#pragma once
#include "geometry_hierarchy.h"
#include <map>
#include <memory>

namespace geometry {
// Driven by the world owner's publication lane. This class owns no threads and
// performs no disk/Vulkan calls: dispatch its tickets through the existing
// worker queue, then complete them on the publication lane. A lease corresponds
// to one admitted world attachment, not one camera view or detail level.
struct AssetLease { uint64_t id = 0; };
struct PageTicket {
    asset_store::BlobHash page;
    uint64_t issuance = 0;
    explicit operator bool() const { return issuance != 0; }
};
struct ResidencyConfig {
    uint32_t max_assets = 4096, max_pages = 65536, max_inflight = 32;
    uint32_t max_attempts = 3, max_known_nodes = 262144;
    uint64_t gpu_bytes = 256ull << 20, scratch_bytes = 64ull << 20;
};
struct ResidencyStats {
    uint64_t gpu_bytes = 0, scratch_bytes = 0;
    uint64_t gpu_budget = 0, scratch_budget = 0;
    uint32_t assets = 0, pages = 0, inflight = 0, known_nodes = 0;
    uint64_t stale_completions = 0, budget_rejections = 0;
    uint64_t failed_retries = 0;
};
// The backend creates this resource only after raster upload AND triangle BLAS
// are ready. Its deleter must honor the renderer's in-flight retirement fence.
// Shared snapshots retain this resource and its reservation after page eviction.
struct ResidentNode {
    NodeView node;
    std::shared_ptr<const void> raster_and_rt;
    std::shared_ptr<const void> reservation;
};
using ResidentHandle = std::shared_ptr<const ResidentNode>;
struct ResidentCut {
    Cut cut;
    std::vector<ResidentHandle> resources;
};
struct ResidentHierarchy {
    std::vector<NodeRef> roots;
    // Missing children are descriptors with ready=false. Published nodes keep
    // ready=true after their CPU page handle is released.
    std::vector<NodeView> nodes;
    std::vector<ResidentHandle> resources;
};
class Residency {
public:
    explicit Residency(ResidencyConfig = {});
    ~Residency();
    Residency(const Residency&) = delete;
    Residency& operator=(const Residency&) = delete;
    // The manifest must be a validated geometry manifest. Attaching does not
    // make roots visible. ready() becomes true only when ALL roots are ready.
    AssetLease attach(asset_store::PageHandle manifest, std::string& error);
    void detach(AssetLease);
    bool ready(AssetLease) const;
    // Replace current-view membership (not an accumulating request priority).
    // Any visible owner promotes a shared page. Queued work is reprioritized;
    // in-flight tickets and resource lifetimes are unchanged.
    void set_visible_assets(std::vector<uint64_t> leases);
    bool page_visible(const asset_store::BlobHash&) const;
    // One request can serve multiple world attachments. Only roots and children
    // discovered through this asset's accepted pages can be requested. A failed
    // page starts a fresh bounded retry cycle when explicitly requested again.
    bool request(AssetLease, const asset_store::BlobHash&, float priority = 0);
    std::vector<PageTicket> dispatch(uint32_t count, uint64_t epoch);
    bool complete_read(PageTicket, asset_store::PageHandle, std::string& error);
    // Call before allocating/uploading GPU geometry or BLAS scratch. Admission
    // includes externally retained snapshots and already reserved uploads.
    bool staged_node(PageTicket, NodeView&);
    bool reserve_upload(PageTicket, uint64_t gpu_bytes, uint64_t scratch_bytes);
    // Upload work must retain this claim until its GPU allocations and scratch
    // are retired, including cancellation before publication.
    std::shared_ptr<const void> upload_reservation(PageTicket);
    bool publish(PageTicket, std::shared_ptr<const void> raster_and_rt);
    void fail(PageTicket, uint64_t retry_epoch);
    // Budget contention is not a failed read. Release its slot and retry with
    // a fresh issuance without exhausting the corruption/I/O attempt bound.
    void defer(PageTicket, uint64_t retry_epoch);
    // Evicts fine pages only; roots of live attachments stay mandatory. An
    // in-flight frame's immutable handles keep bytes/resources charged.
    bool evict(const asset_store::BlobHash&);
    bool select(AssetLease, const std::function<bool(const NodeRef&)>&,
                const CutConfig&, ResidentCut&, std::string& error, bool queue_requests = true);
    bool pending(PageTicket) const;
    bool contains_page(const asset_store::BlobHash&) const;
    ResidentHandle resident(const asset_store::BlobHash&) const;
    ResidencyStats stats() const;
    bool snapshot(AssetLease, ResidentHierarchy&, std::string& error) const;
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
} // namespace geometry
