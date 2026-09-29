#pragma once
#include "geometry/geometry_asset.h"
#include "geometry/geometry_residency.h"
#include "vk_scene_renderer.h"
#include <memory>

namespace viewer {
class PartStore;
struct GeometryPagingTiming {
    uint64_t count = 0;
    double total_ms = 0, max_ms = 0;
    void add(double ms) { ++count; total_ms += ms; if(ms > max_ms) max_ms = ms; }
};
// Windowed CPU wall times. ready_wait includes cache lookup, GPU submission,
// execution and fence retirement; it is NOT a GPU timestamp measurement.
struct GeometryPagingProfile {
    GeometryPagingTiming worker_queue, cache_open, cache_refresh, page_read, decode;
    GeometryPagingTiming completion_wait, prepared_wait, upload_cpu, ready_wait, end_to_end, update;
    GeometryPagingTiming instance_setup, cpu_cut, snapshot, hierarchy_pack, cut_upload;
    GeometryPagingTiming hierarchy_worker;
    uint64_t hierarchy_failures = 0;
    GeometryPagingTiming scene_worker;
    uint64_t scene_submitted=0, scene_published=0, scene_discarded=0, scene_reused=0;
    bool scene_pending=false;
    GeometryPagingTiming admission, dispatch, reprioritize;
    uint64_t visible_dispatched=0, background_dispatched=0;
    uint64_t visible_uploaded=0, background_uploaded=0;
    GeometryPagingTiming feedback, collect_cpu, accept_cpu, upload_loop_cpu, publish_cpu, scene_check;
    uint64_t admission_rejections = 0, admission_deferred = 0;
    uint32_t rejected_assets = 0, unready_assets = 0, source_fallbacks = 0;
    geometry::ResidencyVisibleStats visible;
    uint64_t prefetched_pages = 0;
    uint64_t disk_reads = 0, disk_bytes = 0, cache_hits = 0, read_requests = 0;
    uint64_t bank_capacity = 0, bank_occupied = 0, bank_largest_free = 0, bank_backing_allocations = 0;
    uint64_t cpu_payload_bytes = 0, read_failures = 0, cpu_budget_deferrals = 0;
    uint64_t reservation_stalls = 0, upload_limit_frames = 0, watchdogs = 0, published = 0;
    uint64_t budget_deferred = 0;
    uint64_t gpu_budget_stalls = 0, scratch_budget_stalls = 0, evictions = 0;
    uint32_t queued_prepare = 0;
    uint32_t queued_reads = 0, completed_reads = 0, prepared_pages = 0, pending_uploads = 0;
};
// Page refinement underneath existing world admission. The app lane supplies
// admitted instances. Dedicated bounded I/O and preparation lanes overlap
// loading without blocking rendering or the world-streaming coordinator.
class GeometryWorldRuntime {
public:
    GeometryWorldRuntime();
    ~GeometryWorldRuntime();
    // App/renderer owner lane only.
    void reset(VkSceneRenderer&);
    bool update(PartStore&, VkSceneRenderer&, matter::VulkanDevice&,
                const matter::VulkanFrame&, const matter::CameraDesc&, float detail_scale,
                const std::vector<VkSceneInstance>& admitted,
                std::vector<VkSceneInstance>& output, std::string& error);
    uint64_t revision() const;
    uint64_t source_part(uint64_t instance_id, uint64_t fallback) const;
    geometry::ResidencyStats stats() const;
    GeometryPagingProfile take_profile(); // app lane; drains completed samples
    // Existing world-worker lane only. Results cross a bounded mutex queue.
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
} // namespace viewer
