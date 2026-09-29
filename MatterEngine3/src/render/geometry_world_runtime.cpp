#include "geometry_world_runtime.h"
#include "geometry_raster_adapter.h"
#include "geometry/geometry_indexed_cut.h"
#include "geometry/geometry_page_dependencies.h"
#include "streaming/async_stage_pipeline.h"
#include "part_store.h"
#include "frame_matrices.h"
#include "raster_cull.h"
#include "profile.h"
#include "lod_distance.h"
#include "matter/vulkan_device.h"
#include "matter/log.h"
#include <algorithm>
#include <deque>
#include <chrono>
#include <mutex>
#include <set>
#include <cstdlib>

namespace viewer {
namespace {
using PagingClock = std::chrono::steady_clock;
double elapsed_ms(PagingClock::time_point start) {
    return std::chrono::duration<double,std::milli>(PagingClock::now()-start).count();
}
uint64_t geometry_budget(const char* name, uint64_t fallback) {
    const char* value = std::getenv(name); if (!value || !*value) return fallback;
    char* end = nullptr; const auto mb = std::strtoull(value, &end, 10);
    return end && !*end && mb >= 1 && mb <= 4096 ? mb << 20 : fallback;
}
double geometry_upload_budget() {
    const char* value=std::getenv("MATTER_GEOMETRY_UPLOAD_CPU_MS");
    if(!value || !*value)return 4.0;
    char* end=nullptr;const auto ms=std::strtod(value,&end);
    return end && !*end && std::isfinite(ms) && ms>=0.1 && ms<=8.0 ? ms : 4.0;
}
uint32_t geometry_queue_setting(const char* name, uint32_t fallback, uint32_t maximum) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const auto count = std::strtoul(value, &end, 10);
    return end && !*end && count >= 1 && count <= maximum ? static_cast<uint32_t>(count) : fallback;
}
uint32_t geometry_inflight_limit() {
    return geometry_queue_setting("MATTER_GEOMETRY_MAX_INFLIGHT", 128, 4096);
}
geometry::ResidencyConfig geometry_config() {
    geometry::ResidencyConfig config; config.max_inflight = geometry_inflight_limit();
    const char* terrain = std::getenv("MATTER_GEOMETRY_TERRAIN");
    if (terrain && std::string(terrain)=="1") {
        config.max_pages=262144; config.max_known_nodes=1048576;
    }
    config.gpu_bytes = geometry_budget("MATTER_GEOMETRY_GPU_MB", config.gpu_bytes);
    config.scratch_bytes = geometry_budget("MATTER_GEOMETRY_SCRATCH_MB", config.scratch_bytes);
    return config;
}
}
struct GeometryWorldRuntime::Impl {
    struct Asset {
        geometry::AssetLease lease;
        std::shared_ptr<const geometry::CachedAsset> source;
        uint64_t seen = 0;
        bool source_vt = false;
        geometry::Bounds bounds;
        bool snapshot_valid = false;
        bool hierarchy_pending = false;
        uint64_t hierarchy_revision = 0;
        std::vector<asset_store::BlobHash> pending_pages;
        std::shared_ptr<geometry::ResidentHierarchy> snapshot;
        std::vector<asset_store::BlobHash> dependency_pages;
        std::vector<geometry::IndexedCutNode> indexed_nodes;
        // Owner-lane-only LRU writes, captured once instead of a tree lookup
        // per selected node. Shared cells also survive eviction/republication
        // of the same page while an older snapshot still exists.
        std::vector<std::shared_ptr<uint64_t>> page_touches;
        std::shared_ptr<std::vector<VkGeometryCutNode>> gpu_nodes = std::make_shared<std::vector<VkGeometryCutNode>>();
        std::vector<uint32_t> roots;
        geometry::IndexedCutScratch cut;
        std::vector<uint8_t> traced;
    };
    struct HierarchyBuild {
        uint64_t asset = 0, lease = 0, revision = 0;
        Asset data;
        std::string error;
    };
    static void build_hierarchy(HierarchyBuild& work) {
        auto& cached = work.data;
        const auto& nodes = cached.snapshot->nodes;
        std::map<asset_store::BlobHash,uint32_t> indices;
        for (size_t i=0;i<nodes.size();++i) indices[nodes[i].self.page]=static_cast<uint32_t>(i);
        cached.dependency_pages.reserve(indices.size());
        for(const auto& entry:indices) cached.dependency_pages.push_back(entry.first);
        for (const auto& root:cached.snapshot->roots) cached.roots.push_back(indices.at(root.page));
        cached.indexed_nodes.reserve(nodes.size());
        for (size_t i=0;i<nodes.size();++i) {
            const auto& node=nodes[i];
            geometry::IndexedCutNode indexed; indexed.self=node.self; indexed.ready=node.ready;
            indexed.child_count=static_cast<uint32_t>(node.children.size());
            auto& gpu=(*cached.gpu_nodes)[i];
            gpu.page_lo=node.self.page.lo; gpu.page_hi=node.self.page.hi;
            std::copy(node.self.bounds.lo,node.self.bounds.lo+3,gpu.lo);
            std::copy(node.self.bounds.hi,node.self.bounds.hi+3,gpu.hi);
            gpu.error=static_cast<float>(node.self.error);
            if (double(gpu.error)<node.self.error) gpu.error=std::nextafter(gpu.error,INFINITY);
            for (size_t c=0;c<node.children.size();++c)
                gpu.children[c]=indexed.children[c]=indices.at(node.children[c].page);
            cached.indexed_nodes.push_back(indexed);
        }
    }
    static void adopt_hierarchy(Asset& asset, HierarchyBuild& work) {
        asset.snapshot=std::move(work.data.snapshot);
        asset.dependency_pages=std::move(work.data.dependency_pages);
        asset.indexed_nodes=std::move(work.data.indexed_nodes);
        asset.page_touches=std::move(work.data.page_touches);
        asset.gpu_nodes=std::move(work.data.gpu_nodes);
        asset.roots=std::move(work.data.roots);
        asset.snapshot_valid=asset.hierarchy_revision==work.revision;
        asset.hierarchy_pending=false;
        asset.pending_pages.clear();
    }
    struct ReadWork {
        geometry::PageTicket ticket;
        std::string directory;
        uint64_t gpu_id = 0;
        asset_store::PageHandle cached_root;
        PagingClock::time_point requested = PagingClock::now();
    };
    struct SceneChunk {
        std::shared_ptr<const std::vector<VkGeometryCutNode>> nodes;
        std::vector<uint32_t> roots;
        VkGeometryCutJob job;
    };
    struct SceneBuild {
        std::vector<SceneChunk> chunks;
        std::vector<VkGeometryCutNode> nodes;
        std::vector<uint32_t> roots;
        std::vector<VkGeometryCutJob> jobs;
        std::vector<VkSceneInstance> sources, draws;
        std::vector<float> bias;
        std::vector<std::shared_ptr<void>> resources;
        uint64_t revision=0;
        uint32_t height=0, source_fallbacks=0;
        float fov=0, detail=0;
        std::string error;
    };
    static void assemble_scene(SceneBuild& work) {
        size_t count=0;
        for(const auto& chunk:work.chunks) if(chunk.nodes) count+=chunk.nodes->size();
        if(count>UINT32_MAX){work.error="geometry scene exceeds node index capacity";return;}
        work.nodes.clear();work.roots.clear();work.jobs.clear();work.nodes.reserve(count);
        for(const auto& chunk:work.chunks) {
            auto job=chunk.job;
            job.node_base=static_cast<uint32_t>(work.nodes.size());
            job.first_root=static_cast<uint32_t>(work.roots.size());
            for(auto root:chunk.roots) work.roots.push_back(job.node_base+root);
            work.jobs.push_back(job);
            if(chunk.nodes) work.nodes.insert(work.nodes.end(),chunk.nodes->begin(),chunk.nodes->end());
        }
        if(work.nodes.empty()) work.jobs.clear();
    }
    struct ReadDone {
        ReadWork work;
        asset_store::PageHandle page;
        VkScenePart part;
        std::string error;
        PagingClock::time_point completed{}, prepared{};
        asset_store::PageStatus status = asset_store::PageStatus::IoError;
    };
    struct Upload {
        geometry::PageTicket ticket;
        uint64_t gpu_id;
        std::shared_ptr<const void> claim;
        PagingClock::time_point requested{}, submitted{};
    };
    struct GpuPage {
        uint64_t id = 0;
        std::shared_ptr<uint64_t> touched;
        std::weak_ptr<const geometry::ResidentNode> resident;
    };
    geometry::Residency residency{geometry_config()};
    const double upload_cpu_budget_ms=geometry_upload_budget();
    const uint64_t cpu_budget = geometry_budget("MATTER_GEOMETRY_CPU_MB", 64ull << 20);
    // Preallocate once with the runtime, retain across cache directory switches.
    const std::shared_ptr<asset_store::PageBank> page_bank = asset_store::PageBank::create(static_cast<size_t>(cpu_budget), 256);
    std::map<uint64_t, Asset> assets;
    struct RejectedAdmission {
        asset_store::BlobHash manifest;
        uint64_t revision = 0, seen = 0;
        bool hierarchy_failed = false;
    };
    std::map<uint64_t, RejectedAdmission> rejected_admissions;
    uint64_t admission_revision = 0;
    uint32_t source_fallbacks = 0;
    std::map<asset_store::BlobHash, GpuPage> gpu_pages;
    std::vector<GpuPage> retired_gpu_pages;
    bool collection_needed = false;
    std::map<asset_store::BlobHash, std::string> locations;
    std::map<asset_store::BlobHash, asset_store::PageHandle> root_bytes;
    std::map<uint64_t,uint64_t> source_by_instance;
    std::map<uint64_t,std::pair<geometry::PageTicket,std::chrono::steady_clock::time_point>> reading;
    // Upload drains from the front; do not shift large decoded parts per page.
    std::deque<ReadDone> prepared;
    std::vector<Upload> uploads;
    // Reuse publication scratch; a batch invalidates each affected asset once.
    std::vector<asset_store::BlobHash> published_pages;
    // Raster selection uses the live GPU camera. Repack its immutable scene
    // description only when membership, residency, transforms or LOD policy
    // changes, rather than copying every resident node on every frame.
    const bool raster_only = [] { const char* v=std::getenv("MATTER_GEOMETRY_RASTER_ONLY"); return v && std::string(v)=="1"; }();
    const bool async_hierarchy = [] { const char* v=std::getenv("MATTER_GEOMETRY_HIERARCHY_ASYNC"); return !v || std::string(v)!="0"; }();
    bool scene_valid=false;
    bool scene_pending=false;
    uint64_t scene_revision=0;
    uint32_t scene_source_fallbacks=0;
    // Keep the measured faster path as default until the complete publication
    // workload moves off-thread; assembly alone currently adds handoff cost.
    const bool async_scene = [] { const char* v=std::getenv("MATTER_GEOMETRY_SCENE_ASYNC"); return v && std::string(v)=="1"; }();
    float scene_fov=0, scene_detail=0;
    uint32_t scene_height=0;
    std::vector<VkSceneInstance> scene_sources, scene_draws;
    std::vector<float> scene_bias;
    std::vector<std::shared_ptr<void>> scene_resources;
    // Scratch stays on the publication lane until a complete replacement is
    // ready. Retain capacity across streaming updates instead of rebuilding
    // large temporary node/root arrays from empty vectors every frame.
    std::vector<VkGeometryCutNode> packing_nodes;
    std::vector<uint32_t> packing_roots;
    std::vector<VkGeometryCutJob> packing_jobs;
    void invalidate_scene() { scene_valid=false; ++scene_revision; }
    GeometryPagingProfile profile, worker_profile;
    const bool profiling = std::getenv("MATTER_GEOMETRY_PAGES_PROFILE") != nullptr;
    std::mutex queue_mutex;
    void read_pages(std::vector<ReadDone>& work);
    void prepare_page(ReadDone& result);
    // Worker-confined cache and index; app never opens a file.
    std::string cache_directory;
    std::unique_ptr<asset_store::PageCache> cache;
    uint64_t epoch = 0, revision = 0, next_id = 0xf100000000000001ull;
    std::vector<std::pair<uint64_t,uint64_t>> last_membership;
    // Declared after the state used by callbacks, so threads join before that state is destroyed.
    std::unique_ptr<streaming::AsyncStagePipeline<ReadDone>> pipeline;
    std::unique_ptr<streaming::AsyncStagePipeline<HierarchyBuild>> hierarchy_pipeline;
    std::unique_ptr<streaming::AsyncStagePipeline<SceneBuild>> scene_pipeline;
    uint64_t allocate_id(VkSceneRenderer& renderer) {
        while (next_id && renderer.registered_part_slot(next_id) >= 0) ++next_id;
        return next_id ? next_id++ : 0;
    }
    static void invalidate(Asset& asset) {
        asset.snapshot_valid=false; ++asset.hierarchy_revision;
    }
    void invalidate_snapshots() {invalidate_scene();for(auto& entry:assets)invalidate(entry.second);}
    void invalidate_published_pages() {
        if (published_pages.empty()) return;
        PROFILE_SCOPE("geometry.invalidate_published");
        invalidate_scene();
        std::sort(published_pages.begin(), published_pages.end());
        for (auto& entry : assets) {
            auto& asset = entry.second;
            if (!asset.snapshot_valid && !asset.hierarchy_pending) continue;
            // Include the displayed hierarchy and the pending replacement's
            // frontier, including missing children and shared-page owners.
            const bool affected = geometry::page_sets_intersect(published_pages,asset.pending_pages) ||
                geometry::page_sets_intersect(published_pages,asset.dependency_pages);
            if (affected) invalidate(asset);
        }
        published_pages.clear();
    }
    void collect(VkSceneRenderer& renderer) {
        if (!collection_needed) return;
        collection_needed = false;
        for (auto it = retired_gpu_pages.begin(); it != retired_gpu_pages.end();) {
            if (it->resident.expired()) {
                invalidate_scene(); renderer.release_part(it->id); it = retired_gpu_pages.erase(it);
            } else { collection_needed = true; ++it; }
        }
        for (auto it = gpu_pages.begin(); it != gpu_pages.end();) {
            if (it->second.resident.expired()) {
                invalidate_scene(); renderer.release_part(it->second.id); it = gpu_pages.erase(it);
            } else {
                // Detached/evicted pages can remain pinned by submitted frame
                // snapshots. Continue collecting until those leases retire.
                if (!residency.resident(it->first)) collection_needed = true;
                ++it;
            }
        }
    }
};
GeometryWorldRuntime::GeometryWorldRuntime() : d_(new Impl) {
    auto* state=d_.get();
    state->scene_pipeline=std::make_unique<streaming::AsyncStagePipeline<Impl::SceneBuild>>(
        1,1,[](auto&){},[state](auto& work){
            const auto start=PagingClock::now();Impl::assemble_scene(work);
            if(state->profiling){std::lock_guard<std::mutex> lock(state->queue_mutex);state->worker_profile.scene_worker.add(elapsed_ms(start));}
        },[](auto& work,const char* error){work.error=error;});
    state->hierarchy_pipeline=std::make_unique<streaming::AsyncStagePipeline<Impl::HierarchyBuild>>(
        64,64,[](auto&){},[state](auto& work){
            const auto start=PagingClock::now();
            Impl::build_hierarchy(work);
            if(state->profiling){std::lock_guard<std::mutex> lock(state->queue_mutex);state->worker_profile.hierarchy_worker.add(elapsed_ms(start));}
        },
        [](auto& work,const char* error){work.error=error;});
    state->pipeline=std::make_unique<streaming::AsyncStagePipeline<Impl::ReadDone>>(geometry_inflight_limit(),
        std::min(geometry_inflight_limit(), geometry_queue_setting("MATTER_GEOMETRY_READ_BATCH", 32, 1024)),
        [state](auto& work){state->read_pages(work);},
        [state](auto& result){state->prepare_page(result);},
        [](auto& result,const char* error){result.page.reset();result.status=asset_store::PageStatus::IoError;result.error=error;});
}
GeometryWorldRuntime::~GeometryWorldRuntime() = default;
void GeometryWorldRuntime::reset(VkSceneRenderer& renderer) {
    auto& d = *d_;
    for (const auto& item : d.assets) d.residency.detach(item.second.lease);
    d.invalidate_scene(); d.scene_sources.clear(); d.scene_draws.clear(); d.scene_bias.clear();
    d.scene_resources.clear();d.scene_pending=false;d.scene_pipeline->cancel();
    d.assets.clear(); d.rejected_admissions.clear(); ++d.admission_revision; d.root_bytes.clear(); d.locations.clear(); d.reading.clear(); d.source_by_instance.clear();
    for (const auto& upload : d.uploads) renderer.release_part(upload.gpu_id);
    d.uploads.clear(); d.prepared.clear();
    for (const auto& item : d.gpu_pages) renderer.release_part(item.second.id);
    for (const auto& page : d.retired_gpu_pages) renderer.release_part(page.id);
    d.retired_gpu_pages.clear();
    d.gpu_pages.clear(); d.collection_needed=false; d.last_membership.clear(); ++d.revision;
    d.pipeline->cancel();
    d.hierarchy_pipeline->cancel();
    // Active I/O retains its bank leases. Cancelled generations never publish.
}
void GeometryWorldRuntime::Impl::read_pages(std::vector<ReadDone>& work) {
    GeometryPagingProfile batch;
    if(profiling)for(const auto& result:work)batch.worker_queue.add(elapsed_ms(result.work.requested));
    for(size_t begin=0;begin<work.size();) {
        size_t end=begin+1;
        while(end<work.size() && work[end].work.directory==work[begin].work.directory)++end;
        std::string error;
        auto stage_start=PagingClock::now();
        if(!cache || cache_directory!=work[begin].work.directory) {
            cache.reset();cache_directory=work[begin].work.directory;
            asset_store::PageCacheConfig config;config.store.dir=cache_directory;
            config.resident_bytes=cpu_budget;config.bank=page_bank;
            config.read_ahead_bytes=static_cast<uint32_t>(std::min<uint64_t>(16ull<<20,geometry_budget("MATTER_GEOMETRY_READ_AHEAD_MB",4ull<<20)));
            const char* ahead=std::getenv("MATTER_GEOMETRY_READ_AHEAD_MB");
            if(ahead && std::string(ahead)=="0")config.read_ahead_bytes=0;
            if(config.read_ahead_bytes)config.store.batch_max_bytes=config.read_ahead_bytes;
            config.max_read_bytes=std::max<uint64_t>(8ull<<20,uint64_t(config.read_ahead_bytes)*2);
            if(page_bank)cache=asset_store::PageCache::open(config,error);
            else error="geometry page bank initialization failed";
            if(profiling)batch.cache_open.add(elapsed_ms(stage_start));
        }
        std::vector<asset_store::BlobHash> hashes;
        for(size_t i=begin;i<end;++i)if(!work[i].work.cached_root)hashes.push_back(work[i].work.ticket.page);
        const auto before=cache?cache->stats():asset_store::PageCacheStats{};
        std::vector<asset_store::PageResult> pages;
        if(cache && !hashes.empty()) {
            stage_start=PagingClock::now();const bool refreshed=cache->refresh();
            if(profiling)batch.cache_refresh.add(elapsed_ms(stage_start));
            if(!refreshed)error="geometry page index refresh failed";
            else {stage_start=PagingClock::now();pages=cache->read_partitioned(hashes);if(profiling)batch.page_read.add(elapsed_ms(stage_start));}
        }
        if(profiling && cache) {
            const auto after=cache->stats();
            batch.disk_reads+=after.disk_reads-before.disk_reads;batch.disk_bytes+=after.disk_bytes-before.disk_bytes;
            batch.cache_hits+=after.hits-before.hits;batch.read_requests+=after.requests-before.requests;
            batch.prefetched_pages+=after.prefetched_pages-before.prefetched_pages;
            batch.cpu_payload_bytes=after.resident_payload_bytes;
        }
        size_t read_index=0;
        for(size_t i=begin;i<end;++i) {
            auto& result=work[i];result.page=result.work.cached_root;
            if(result.page)result.status=asset_store::PageStatus::Ok;
            else {
                if(read_index<pages.size()){result.page=pages[read_index].page;result.status=pages[read_index].status;}
                ++read_index;
            }
            if(!result.page)result.error=error.empty()?"geometry page read failed":error;
        }
        begin=end;
    }
    if(profiling) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        const auto merge=[](GeometryPagingTiming& a,const GeometryPagingTiming& b){a.count+=b.count;a.total_ms+=b.total_ms;a.max_ms=std::max(a.max_ms,b.max_ms);};
        auto& w=worker_profile;
        merge(w.worker_queue,batch.worker_queue);merge(w.cache_open,batch.cache_open);
        merge(w.cache_refresh,batch.cache_refresh);merge(w.page_read,batch.page_read);
        w.disk_reads+=batch.disk_reads;w.disk_bytes+=batch.disk_bytes;w.cache_hits+=batch.cache_hits;w.read_requests+=batch.read_requests;
        w.prefetched_pages+=batch.prefetched_pages;w.cpu_payload_bytes=batch.cpu_payload_bytes;
    }
}
void GeometryWorldRuntime::Impl::prepare_page(ReadDone& result) {
    const auto start=PagingClock::now();
    geometry::NodeView node; node.page=result.page;
    if(!result.page || !build_geometry_page_part(result.work.gpu_id,node,result.part,result.error)) {
        if(result.error.empty())result.error="geometry page preparation failed";
        result.page.reset();
    }
    const char* raster_only=std::getenv("MATTER_GEOMETRY_RASTER_ONLY");
    result.part.geometry_raster_only=raster_only && std::string(raster_only)=="1";
    const char* blas_cache=std::getenv("MATTER_GEOMETRY_BLAS_CACHE");
    if(result.page && !result.part.geometry_raster_only && (!blas_cache || std::string(blas_cache)!="0")) {
        result.part.blas_cache_directory=result.work.directory+"/blas";
        result.part.blas_cache_content="adapter-v1/"+asset_store::hash_to_string(result.work.ticket.page);
    }
    result.completed=PagingClock::now();
    if(profiling){std::lock_guard<std::mutex> lock(queue_mutex);worker_profile.decode.add(elapsed_ms(start));}
}
bool GeometryWorldRuntime::update(PartStore& store, VkSceneRenderer& renderer, matter::VulkanDevice& vulkan,
    const matter::VulkanFrame& frame, const matter::CameraDesc& camera, float detail_scale,
    const std::vector<VkSceneInstance>& admitted, std::vector<VkSceneInstance>& output, std::string& error) {
    // Preparation audit: cook/cache assets through PartStore, display source
    // receivers, and avoid registering every geometry page during the cook.
    const char* prepare_only = std::getenv("MATTER_GEOMETRY_PREPARE_ONLY");
    if (prepare_only && std::string(prepare_only) == "1") {
        output = admitted; error.clear(); return true;
    }
    auto& d = *d_; ++d.epoch;
    bool detached = false;
    PROFILE_SCOPE_NAMED(adopt_scope, "geometry.adopt_hierarchies");
    for (auto& ready : d.hierarchy_pipeline->take()) {
        auto asset=d.assets.find(ready.asset);
        if (asset==d.assets.end() || asset->second.lease.id!=ready.lease) continue;
        if (!ready.error.empty()) {
            MATTER_LOGW("geometry", "hierarchy build for asset %016llx failed: %s; asset falls back to its source part",
                        static_cast<unsigned long long>(ready.asset), ready.error.c_str());
            // Remove the live lease as well as rejecting readmission; leaving
            // it in assets would continue drawing its old hierarchy forever.
            d.rejected_admissions[ready.asset] = {
                asset->second.source->manifest->hash, d.admission_revision, d.epoch, true};
            d.residency.detach(asset->second.lease);
            d.assets.erase(asset);
            detached = true;
            if (d.profiling) ++d.profile.hierarchy_failures;
            continue;
        }
        Impl::adopt_hierarchy(asset->second,ready);
        d.invalidate_scene();
    }
    if(const auto dropped=d.hierarchy_pipeline->take_dropped()) {
        // A failed allocation/move may lose the completion payload itself.
        // Reconcile every pending owner when cancelling that generation so no
        // hierarchy_pending flag can prevent future refinement indefinitely.
        d.hierarchy_pipeline->cancel();
        for(auto& entry:d.assets)if(entry.second.hierarchy_pending) {
            auto& asset=entry.second;
            asset.hierarchy_pending=false;
            asset.pending_pages.clear();
            Impl::invalidate(asset);
        }
        d.invalidate_scene();
        MATTER_LOGW("geometry", "hierarchy worker dropped %zu items; retrying pending hierarchies with previous coverage", dropped);
        if(d.profiling)d.profile.hierarchy_failures+=dropped;
    }
    adopt_scope.stop();
    PROFILE_SCOPE_NAMED(admission_scope, "geometry.admission");
    struct UpdateTimer { Impl& d; PagingClock::time_point start=PagingClock::now();
        ~UpdateTimer(){if(d.profiling)d.profile.update.add(elapsed_ms(start));} } timer{d};
    const char* snapshot_cache = std::getenv("MATTER_GEOMETRY_SNAPSHOT_CACHE");
    if (snapshot_cache && std::string(snapshot_cache) == "0") d.invalidate_snapshots();
    const auto admission_start = PagingClock::now();
    FrameMatrices priority_view;
    std::string view_error;
    const bool valid_view=build_frame_matrices(camera,frame.extent.width,frame.extent.height,priority_view,view_error);
    std::vector<uint64_t> visible_leases;
    visible_leases.reserve(admitted.size());
    for (const auto& instance : admitted) {
        const auto* part = store.find(instance.part_hash);
        if (!part || !part->geometry_pages) continue;
        auto found = d.assets.find(instance.part_hash);
        if (found == d.assets.end()) {
            const auto manifest = part->geometry_pages->manifest->hash;
            auto rejected = d.rejected_admissions.find(instance.part_hash);
            if (rejected != d.rejected_admissions.end() && rejected->second.manifest == manifest &&
                (rejected->second.hierarchy_failed || rejected->second.revision == d.admission_revision)) {
                rejected->second.seen = d.epoch;
                if(d.profiling)++d.profile.admission_deferred;
                continue;
            }
            auto lease = d.residency.attach(part->geometry_pages->manifest, error);
            if (!lease.id) {
                // Admissions can only recover after a release or a changed
                // manifest. Re-decoding thousands of roots each frame while
                // capacity only grows starves useful streaming work.
                d.rejected_admissions[instance.part_hash] = {manifest, d.admission_revision, d.epoch};
                if(d.profiling)++d.profile.admission_rejections;
                error.clear(); continue; // retain ordinary coarse coverage
            }
            d.rejected_admissions.erase(instance.part_hash);
            d.invalidate_scene();
            found = d.assets.emplace(instance.part_hash, Impl::Asset{lease, part->geometry_pages, d.epoch}).first;
            bool first_root=true;
            for (const auto& root : part->geometry_pages->root_refs) {
                auto& bounds=found->second.bounds;
                for(int axis=0;axis<3;++axis) {
                    bounds.lo[axis]=first_root?root.bounds.lo[axis]:std::min(bounds.lo[axis],root.bounds.lo[axis]);
                    bounds.hi[axis]=first_root?root.bounds.hi[axis]:std::max(bounds.hi[axis],root.bounds.hi[axis]);
                }
                first_root=false;
                d.locations[root.page] = part->geometry_pages->directory;
            }
            for (const auto& root : part->geometry_pages->roots) {
                d.root_bytes[root.self.page] = root.page;
            }
        }
        found->second.source_vt = part->geometry_source_vt;
        found->second.seen = d.epoch;
        const auto& bounds=found->second.bounds;
        if(!valid_view || !aabb_culled(bounds.lo,bounds.hi,instance.object_to_world.m,priority_view.frustum_planes))
            visible_leases.push_back(found->second.lease.id);
    }
    if(d.profiling)d.profile.admission.add(elapsed_ms(admission_start));
    for (auto it = d.assets.begin(); it != d.assets.end();) {
        if (it->second.seen != d.epoch) { d.residency.detach(it->second.lease); it = d.assets.erase(it); detached = true; }
        else ++it;
    }
    // Another asset may have cached descriptors for pages owned only by the
    // departing lease. Rebuild snapshots from residency, even while old frame
    // pins keep those allocations alive.
    if (detached) { ++d.admission_revision; d.invalidate_snapshots(); d.collection_needed=true; }
    for(auto it=d.rejected_admissions.begin();it!=d.rejected_admissions.end();) {
        if(it->second.seen!=d.epoch)it=d.rejected_admissions.erase(it);
        else ++it;
    }
    const auto priority_start=PagingClock::now();
    d.residency.set_visible_assets(std::move(visible_leases));
    if(d.profiling)d.profile.reprioritize.add(elapsed_ms(priority_start));
    admission_scope.stop();
    PROFILE_SCOPE_NAMED(stream_scope, "geometry.stream_pages");
    auto stage_clock = PagingClock::now();
    const auto feedback = renderer.take_geometry_page_requests();
    if (!feedback.empty()) {
        std::map<uint64_t, const Impl::Asset*> owners;
        for (const auto& asset : d.assets) owners[asset.second.lease.id] = &asset.second;
        for (const auto& request : feedback) {
            const auto owner = owners.find(request.owner_lease); if (owner == owners.end()) continue;
            const asset_store::BlobHash page{request.page_lo, request.page_hi};
            if (d.residency.request(owner->second->lease, page)) d.locations[page] = owner->second->source->directory;
        }
    }
    if(d.profiling)d.profile.feedback.add(elapsed_ms(stage_clock));
    // New assets add root pins at attachment. Only removals need a rebuild;
    // unchanged roots do not require map allocation and reference churn.
    if(detached){
        d.root_bytes.clear();
        for(const auto& asset:d.assets){
            for(const auto& root:asset.second.source->root_refs)
                d.locations[root.page]=asset.second.source->directory;
            for(const auto& root:asset.second.source->roots)
                d.root_bytes[root.self.page]=root.page;
        }
    }
    if (d.assets.empty()) d.locations.clear();
    const auto now = std::chrono::steady_clock::now();
    stage_clock = PagingClock::now();
    d.collect(renderer);
    if(d.profiling)d.profile.collect_cpu.add(elapsed_ms(stage_clock));
    stage_clock = PagingClock::now();
    auto done=d.pipeline->take();
    bool cpu_pressure = false;
    for (auto& result : done) {
        if(d.profiling)d.profile.completion_wait.add(elapsed_ms(result.completed));
        d.reading.erase(result.work.ticket.issuance);
        if (!result.page) {
            if (result.status == asset_store::PageStatus::BudgetExceeded) {
                d.residency.defer(result.work.ticket, d.epoch + 30); cpu_pressure = true;
                if(d.profiling)++d.profile.cpu_budget_deferrals;
            } else {d.residency.fail(result.work.ticket, d.epoch + 30);if(d.profiling)++d.profile.read_failures;}
            continue;
        }
        if (!d.residency.complete_read(result.work.ticket, result.page, error)) {
            if (d.residency.pending(result.work.ticket)) d.residency.fail(result.work.ticket, d.epoch + 30);
            error.clear(); continue;
        }
        result.prepared=PagingClock::now();
        d.prepared.push_back(std::move(result));
    }
    if(d.profiling)d.profile.accept_cpu.add(elapsed_ms(stage_clock));
    // Consume completed reads before watchdogs: a slow pipeline compilation
    // on the app lane must not expire work already completed by the worker.
    for (auto it = d.reading.begin(); it != d.reading.end();) {
        if (!d.residency.pending(it->second.first)) it = d.reading.erase(it);
        else if (now - it->second.second > std::chrono::seconds(10)) {
            if(d.profiling)++d.profile.watchdogs;
            d.residency.fail(it->second.first, d.epoch + 30); it = d.reading.erase(it);
        } else ++it;
    }
    // Bounded work per frame; the remaining decoded pages retain their slots.
    uint32_t submitted = 0;
    uint64_t upload_bytes=0;
    const auto upload_budget_start=PagingClock::now();
    bool upload_budget_exhausted=false;
    bool geometry_pressure = false;
    // The decoded queue obeys the current view too, including work whose
    // read started before a camera turn. Two passes avoid a large sort buffer.
    for(int visible_pass=1;visible_pass>=0 && !upload_budget_exhausted;--visible_pass)
    for (auto it = d.prepared.begin(); it != d.prepared.end();) {
        if(d.residency.page_visible(it->work.ticket.page)!=bool(visible_pass)){++it;continue;}
        if(elapsed_ms(upload_budget_start)>=d.upload_cpu_budget_ms){upload_budget_exhausted=true;break;}
        geometry::NodeView node;
        if (!d.residency.staged_node(it->work.ticket, node)) { it = d.prepared.erase(it); continue; }
        uint64_t bytes = 0, scratch = 0;
        if (!renderer.geometry_page_upload_cost(it->part, bytes, scratch, error)) {
            d.residency.fail(it->work.ticket, d.epoch + 30); it = d.prepared.erase(it); error.clear(); continue;
        }
        if(submitted && bytes>(8ull<<20)-std::min<uint64_t>(upload_bytes,8ull<<20)){upload_budget_exhausted=true;break;}
        if (!d.residency.reserve_upload(it->work.ticket, bytes, scratch)) {
            if(d.profiling)++d.profile.reservation_stalls;
            // A queue is not memory pressure. Scratch contention resolves when
            // pending builds finish and must not evict otherwise useful pages.
            const auto stats = d.residency.stats();
            const bool gpu_stall=bytes > stats.gpu_budget - std::min(stats.gpu_bytes, stats.gpu_budget);
            geometry_pressure |= gpu_stall;
            if(d.profiling){
                d.profile.gpu_budget_stalls+=gpu_stall;
                d.profile.scratch_budget_stalls+=scratch > stats.scratch_budget-std::min(stats.scratch_bytes,stats.scratch_budget);
            }
            ++it; continue;
        }
        const auto upload_start=PagingClock::now();
        if(d.profiling)d.profile.prepared_wait.add(elapsed_ms(it->prepared));
        auto claim = d.residency.upload_reservation(it->work.ticket);
        it->part.geometry_budget_claim = claim;
        if (renderer.ensure_part(it->part, error) < 0 || !renderer.queue_geometry_page_warmup(it->part.part_hash, error)) {
            renderer.release_part(it->part.part_hash); d.residency.fail(it->work.ticket, d.epoch + 30);
            it = d.prepared.erase(it); error.clear(); continue;
        }
        if(d.profiling)d.profile.upload_cpu.add(elapsed_ms(upload_start));
        d.uploads.push_back({it->work.ticket, it->part.part_hash, std::move(claim),it->work.requested,PagingClock::now()});
        if(d.profiling) {
            if(visible_pass)++d.profile.visible_uploaded;else ++d.profile.background_uploaded;
        }
        it = d.prepared.erase(it); ++submitted;upload_bytes+=bytes;
    }
    if(d.profiling&&upload_budget_exhausted&&!d.prepared.empty())++d.profile.upload_limit_frames;
    if(d.profiling)d.profile.upload_loop_cpu.add(elapsed_ms(upload_budget_start));
    stage_clock = PagingClock::now();
    for (auto it = d.uploads.begin(); it != d.uploads.end();) {
        geometry::NodeView node;
        if (!d.residency.staged_node(it->ticket, node)) {
            renderer.release_part(it->gpu_id); it = d.uploads.erase(it); continue;
        }
        if (!vulkan.retain_for_frame(frame, {std::const_pointer_cast<void>(it->claim)}, error)) {
            d.invalidate_published_pages();
            return false;
        }
        if (!renderer.geometry_page_render_ready(it->gpu_id)) { ++it; continue; }
        if (!d.residency.publish(it->ticket, renderer.geometry_page_resources(it->gpu_id))) { ++it; continue; }
        if(d.profiling){++d.profile.published;d.profile.ready_wait.add(elapsed_ms(it->submitted));d.profile.end_to_end.add(elapsed_ms(it->requested));}
        d.published_pages.push_back(it->ticket.page);
        auto previous = d.gpu_pages.find(it->ticket.page);
        if (previous != d.gpu_pages.end() && previous->second.id != it->gpu_id) {
            // The old part may still be pinned by a submitted snapshot; hand it
            // to collect(), which releases it once that lease retires.
            d.retired_gpu_pages.push_back(previous->second);
            d.collection_needed = true;
        }
        d.gpu_pages[it->ticket.page] = {it->gpu_id, std::make_shared<uint64_t>(d.epoch), d.residency.resident(it->ticket.page)};
        it = d.uploads.erase(it);
    }
    d.invalidate_published_pages();
    if(d.profiling)d.profile.publish_cpu.add(elapsed_ms(stage_clock));
    const auto dispatch_pending = [&]() -> bool {
    const auto queue_room=static_cast<uint32_t>(d.pipeline->available());
    const auto dispatch_start=PagingClock::now();
    const auto tickets=d.residency.dispatch(queue_room, d.epoch);
    if(d.profiling)d.profile.dispatch.add(elapsed_ms(dispatch_start));
    for (const auto ticket : tickets) {
        auto location = d.locations.find(ticket.page);
        if (location == d.locations.end()) { d.residency.fail(ticket, d.epoch + 30); continue; }
        const auto id = d.allocate_id(renderer);
        if (!id) { error = "geometry renderer identity exhausted"; return false; }
        Impl::ReadDone result;result.work={ticket,location->second,id,{}};
        auto root=d.root_bytes.find(ticket.page);if(root!=d.root_bytes.end())result.work.cached_root=root->second;
        d.reading[ticket.issuance]={ticket,now};
        if(!d.pipeline->submit(std::move(result))){d.reading.erase(ticket.issuance);d.residency.defer(ticket,d.epoch+1);}
        else if(d.profiling) {
            if(d.residency.page_visible(ticket.page))++d.profile.visible_dispatched;
            else ++d.profile.background_dispatched;
        }
    }
        return true;
    };
    stream_scope.stop();
    PROFILE_SCOPE_NAMED(scene_scope, "geometry.scene_completion");
    stage_clock = PagingClock::now();
    const auto matches_scene = [&](const std::vector<VkSceneInstance>& sources,
        const std::vector<float>& biases,uint32_t height,float fov,float detail) {
        if(!d.raster_only || geometry_pressure || cpu_pressure || sources.size()!=admitted.size() ||
            biases.size()!=admitted.size() || height!=frame.extent.height ||
            fov!=camera.vertical_fov_radians || detail!=detail_scale) return false;
        for(size_t i=0;i<admitted.size();++i) {
            const auto& a=admitted[i];const auto& b=sources[i];
            if(a.part_hash!=b.part_hash || a.instance_id!=b.instance_id ||
                a.animation_instance_slot!=b.animation_instance_slot || a.ray_traced!=b.ray_traced ||
                a.rt_proxy_only!=b.rt_proxy_only || a.rt_vt_source_hash!=b.rt_vt_source_hash || std::memcmp(a.object_to_world.m,b.object_to_world.m,sizeof(a.object_to_world.m)) ||
                renderer.part_draw_override(a.part_hash).lod_bias!=biases[i]) return false;
        }
        return true;
    };
    for(auto& ready:d.scene_pipeline->take()) {
        d.scene_pending=false;
        if(!ready.error.empty()) {
            MATTER_LOGW("geometry", "scene assembly failed: %s; keeping the previous scene", ready.error.c_str());
            if(d.profiling)++d.profile.scene_discarded;
            continue;
        }
        if(matches_scene(ready.sources,ready.bias,ready.height,ready.fov,ready.detail)) {
            const auto cut_start=PagingClock::now();
            if(!renderer.set_geometry_cut(ready.nodes,ready.roots,ready.jobs,error,true)) return false;
            if(d.profiling)d.profile.cut_upload.add(elapsed_ms(cut_start));
            d.scene_draws=std::move(ready.draws);d.scene_sources=std::move(ready.sources);
            d.scene_resources=std::move(ready.resources);d.scene_bias=std::move(ready.bias);
            d.scene_height=ready.height;d.scene_fov=ready.fov;d.scene_detail=ready.detail;
            d.scene_source_fallbacks=d.source_fallbacks=ready.source_fallbacks;
            d.scene_valid=ready.revision==d.scene_revision;
            if(d.profiling)++d.profile.scene_published;
        } else if(d.profiling)++d.profile.scene_discarded;
        // Keep the large assembly allocation for the next worker submission.
        d.packing_nodes.swap(ready.nodes);d.packing_roots.swap(ready.roots);d.packing_jobs.swap(ready.jobs);
    }
    if(const auto dropped=d.scene_pipeline->take_dropped()) {
        d.scene_pipeline->cancel();
        d.scene_pending=false;
        MATTER_LOGW("geometry", "scene worker dropped %zu items; keeping the previous scene and retrying", dropped);
        if(d.profiling)d.profile.scene_discarded+=dropped;
    }
    stage_clock=PagingClock::now();
    const bool scene_matches=matches_scene(d.scene_sources,d.scene_bias,d.scene_height,d.scene_fov,d.scene_detail);
    if(d.profiling)d.profile.scene_check.add(elapsed_ms(stage_clock));
    if(scene_matches && (d.scene_valid || d.scene_pending)) {
        if(d.profiling && d.scene_pending)++d.profile.scene_reused;
        if(!dispatch_pending() || !vulkan.retain_for_frame(frame,d.scene_resources,error)) return false;
        output=d.scene_draws;error.clear();return true;
    }
    if(d.scene_pending) { d.scene_pipeline->cancel();d.scene_pending=false;if(d.profiling)++d.profile.scene_discarded; }
    scene_scope.stop();
    PROFILE_SCOPE_NAMED(assembly_scope, "geometry.scene_assembly");
    const bool assemble_async=d.async_scene && scene_matches && d.scene_pipeline->available();
    Impl::SceneBuild pending_scene;
    if(assemble_async) pending_scene.chunks.reserve(admitted.size());
    d.invalidate_scene();
    d.source_by_instance.clear();
    d.source_fallbacks = 0;
    std::vector<VkSceneInstance> selected;
    auto& gpu_nodes = d.packing_nodes; gpu_nodes.clear();
    auto& gpu_roots = d.packing_roots; gpu_roots.clear();
    auto& gpu_jobs = d.packing_jobs; gpu_jobs.clear();
    std::vector<std::shared_ptr<void>> retained;
    std::set<asset_store::BlobHash> used;
    for (const auto& instance : admitted) {
        const auto setup_start=d.profiling ? PagingClock::now() : PagingClock::time_point{};
        auto asset = d.assets.find(instance.part_hash);
        const auto ordinary = [&] {
            const auto* source = store.find(instance.part_hash);
            if(source && source->geometry_pages)++d.source_fallbacks;
            VkGeometryCutJob job{static_cast<uint32_t>(selected.size()),0,0,1,0};
            if(assemble_async) pending_scene.chunks.push_back({{}, {}, job});
            else gpu_jobs.push_back(job);
            selected.push_back(instance);
        };
        if (asset == d.assets.end() || !d.residency.ready(asset->second.lease)) {
            ordinary();
            if(d.profiling)d.profile.instance_setup.add(elapsed_ms(setup_start));
            continue;
        }
        const float scale = static_cast<float>(lod::error_transform_scale(instance.object_to_world.m));
        const double pixel_angle = camera.vertical_fov_radians / std::max(frame.extent.height, 1u);
        const auto draw_override = renderer.part_draw_override(instance.part_hash);
        const float error_reach = static_cast<float>(lod::error_switch_distance(1, scale, pixel_angle, detail_scale * draw_override.lod_bias));
        const auto gpu_error = [](double error) {
            float value = static_cast<float>(error);
            return double(value) < error ? std::nextafter(value, INFINITY) : value;
        };
        const auto centre_distance = [&](const geometry::NodeRef& node) {
            double centre[3]{};
            for (int r = 0; r < 3; ++r) {
                centre[r] = instance.object_to_world.m[r*4+3];
                for (int c = 0; c < 3; ++c) centre[r] += instance.object_to_world.m[r*4+c] *
                    (double(node.bounds.lo[c]) + node.bounds.hi[c]) * .5;
            }
            const double dx = centre[0]-camera.position.x, dy = centre[1]-camera.position.y, dz = centre[2]-camera.position.z;
            return std::sqrt(dx*dx+dy*dy+dz*dz);
        };
        const auto refine = [&](const geometry::NodeRef& node) {
            double radius2 = 0;
            for (int c = 0; c < 3; ++c) { double extent = node.bounds.hi[c]-node.bounds.lo[c]; radius2 += extent*extent; }
            const double distance = std::max(.01, centre_distance(node) - .5*std::sqrt(radius2)*scale);
            return node.error > 0 && distance < gpu_error(node.error) * error_reach;
        };
        auto& cached = asset->second;
        if(d.profiling)d.profile.instance_setup.add(elapsed_ms(setup_start));
        // Detailed per-asset clocks are opt-in; do not pay thousands of
        // timer reads per frame when only aggregate frame profiling is active.
        auto stage_start=d.profiling ? PagingClock::now() : PagingClock::time_point{};
        if (!cached.snapshot_valid && !cached.hierarchy_pending &&
            (!cached.snapshot || d.hierarchy_pipeline->available())) {
            Impl::HierarchyBuild work;
            work.asset=instance.part_hash;work.lease=cached.lease.id;work.revision=cached.hierarchy_revision;
            work.data.snapshot=std::make_shared<geometry::ResidentHierarchy>();
            if (!d.residency.snapshot(cached.lease,*work.data.snapshot,error)) { ordinary();error.clear();continue; }
            const auto& nodes=work.data.snapshot->nodes;
            work.data.gpu_nodes->resize(nodes.size());
            work.data.page_touches.resize(nodes.size(),nullptr);
            cached.pending_pages.clear();cached.pending_pages.reserve(nodes.size());
            for (size_t i=0;i<nodes.size();++i) {
                cached.pending_pages.push_back(nodes[i].self.page);
                if (!nodes[i].ready) continue;
                auto found=d.gpu_pages.find(nodes[i].self.page);
                if(found==d.gpu_pages.end()){error="resident geometry page has no renderer allocation";return false;}
                work.data.page_touches[i]=found->second.touched;
                (*work.data.gpu_nodes)[i].ready_part_hash=found->second.id;
                if(!renderer.resolve_geometry_page((*work.data.gpu_nodes)[i])) {
                    error="resident geometry page lost its ready renderer binding";return false;
                }
            }
            std::sort(cached.pending_pages.begin(),cached.pending_pages.end());
            // First coverage stays immediate. Later replacements use immutable
            // resource-pinned inputs; the worker never reads renderer/residency.
            if (!cached.snapshot || !d.async_hierarchy || !d.raster_only || geometry_pressure || cpu_pressure) {
                Impl::build_hierarchy(work);Impl::adopt_hierarchy(cached,work);
            } else {
                cached.hierarchy_pending=d.hierarchy_pipeline->submit(std::move(work));
                if (!cached.hierarchy_pending) cached.pending_pages.clear();
            }
        }
        if(d.profiling)d.profile.snapshot.add(elapsed_ms(stage_start));
        if(d.profiling)stage_start=PagingClock::now();
        const bool selected_cut=geometry::select_indexed_cut(cached.indexed_nodes,cached.roots,refine,{},cached.cut);
        if(d.profiling)d.profile.cpu_cut.add(elapsed_ms(stage_start));
        if(!selected_cut){ordinary();continue;}
        if(d.profiling)stage_start=PagingClock::now();
        const auto& hierarchy=*cached.snapshot;
        cached.traced.assign(hierarchy.nodes.size(),0);
        for(auto index:cached.cut.selected){
            const auto& node=cached.indexed_nodes[index].self;
            if(draw_override.max_draw_distance>0&&centre_distance(node)>draw_override.max_draw_distance)continue;
            cached.traced[index]=1;
            if(geometry_pressure||cpu_pressure)used.insert(node.page);
            if(const auto& touched=cached.page_touches[index]) *touched=d.epoch;
        }
        const uint32_t base = static_cast<uint32_t>(gpu_nodes.size());
        VkGeometryCutJob job;
        job.instance_index = static_cast<uint32_t>(selected.size()); job.first_root = static_cast<uint32_t>(gpu_roots.size());
        job.root_count = static_cast<uint32_t>(hierarchy.roots.size()); job.scale = scale; job.error_reach = error_reach;
        job.owner_lease = asset->second.lease.id;
        job.source_vt = asset->second.source_vt;
        job.node_base = base;
        if(assemble_async) pending_scene.chunks.push_back({cached.gpu_nodes,cached.roots,job});
        else {
            for (auto root : cached.roots) gpu_roots.push_back(base + root);
            gpu_jobs.push_back(job);
        }
        auto controller = instance; controller.ray_traced = false;
        selected.push_back(controller);
        d.source_by_instance[instance.instance_id]=instance.part_hash;
        if (!d.raster_only) for(size_t i=0;i<cached.gpu_nodes->size();++i){
            const auto& gpu=(*cached.gpu_nodes)[i];
            if(gpu.ready_part_hash){
                auto proxy=instance;proxy.part_hash=gpu.ready_part_hash;
                proxy.rt_vt_source_hash = asset->second.source_vt ? instance.part_hash : 0;
                proxy.rt_proxy_only=true;proxy.ray_traced=!d.raster_only&&instance.ray_traced&&cached.traced[i];
                selected.push_back(proxy);
            }
        }
        if(!assemble_async) gpu_nodes.insert(gpu_nodes.end(),cached.gpu_nodes->begin(),cached.gpu_nodes->end());
        // One immutable snapshot pin retains all page resources for the frame.
        retained.push_back(cached.snapshot);
        if(d.profiling)d.profile.hierarchy_pack.add(elapsed_ms(stage_start));
    }
    assembly_scope.stop();
    PROFILE_SCOPE_NAMED(retain_scope, "geometry.retain_snapshots");
    if (!assemble_async) {
        if(d.raster_only) d.scene_resources=retained;
        if(!retained.empty() && !vulkan.retain_for_frame(frame,std::move(retained),error)) return false;
    }
    retain_scope.stop();
    PROFILE_SCOPE_NAMED(reclaim_scope, "geometry.reclaim_dispatch");
    // Reclaim least-recently-used fine pages under upload pressure. A frame
    // that still owns a snapshot delays both reservation and GPU retirement.
    if (geometry_pressure || cpu_pressure) {
        // Ancestors are needed to reach a selected descendant on the next
        // traversal. Evicting one would force a visible fallback even though
        // the selected descendant's allocation is still resident.
        std::map<asset_store::BlobHash, std::vector<asset_store::BlobHash>> parents;
        for (const auto& page : d.gpu_pages) {
            const auto node = d.residency.resident(page.first);
            if (node) for (const auto& child : node->node.children) parents[child.page].push_back(page.first);
        }
        std::vector<asset_store::BlobHash> protected_pages(used.begin(), used.end());
        for (size_t i = 0; i < protected_pages.size(); ++i) {
            auto found = parents.find(protected_pages[i]);
            if (found != parents.end()) for (const auto& parent : found->second)
                if (used.insert(parent).second) protected_pages.push_back(parent);
        }
        std::vector<std::pair<uint64_t,asset_store::BlobHash>> candidates;
        for (const auto& page : d.gpu_pages) if (!used.count(page.first)) candidates.push_back({*page.second.touched,page.first});
        std::sort(candidates.begin(), candidates.end());
        uint32_t removed = 0;
        for (const auto& candidate : candidates) if (d.residency.evict(candidate.second)) {
            ++d.admission_revision;
            if(++removed==8)break;
        }
        if(d.profiling)d.profile.evictions+=removed;
        if (removed) { d.invalidate_snapshots(); d.collection_needed=true; }
        d.collect(renderer);
    }
    if (!dispatch_pending()) return false;
    if(detached)for(auto it=d.locations.begin();it!=d.locations.end();)
        if(!d.residency.contains_page(it->first))it=d.locations.erase(it);else ++it;
    if(assemble_async) {
        pending_scene.sources=admitted;pending_scene.draws=std::move(selected);
        pending_scene.resources=std::move(retained);pending_scene.revision=d.scene_revision;
        pending_scene.height=frame.extent.height;pending_scene.fov=camera.vertical_fov_radians;
        pending_scene.detail=detail_scale;pending_scene.source_fallbacks=d.source_fallbacks;
        for(const auto& source:admitted) pending_scene.bias.push_back(renderer.part_draw_override(source.part_hash).lod_bias);
        pending_scene.nodes.swap(d.packing_nodes);pending_scene.roots.swap(d.packing_roots);pending_scene.jobs.swap(d.packing_jobs);
        d.scene_pending=d.scene_pipeline->submit(std::move(pending_scene));
        if(!d.scene_pending){error="geometry scene worker admission lost its reserved capacity";return false;}
        if(d.profiling){++d.profile.scene_submitted;++d.profile.scene_reused;}
        d.source_fallbacks=d.scene_source_fallbacks;
        if(!vulkan.retain_for_frame(frame,d.scene_resources,error)) return false;
        output=d.scene_draws;error.clear();return true;
    }
    reclaim_scope.stop();
    PROFILE_SCOPE_NAMED(cut_scope, "geometry.publish_scene");
    std::vector<std::pair<uint64_t,uint64_t>> membership;
    for (const auto& instance : selected) membership.push_back({instance.part_hash, instance.instance_id});
    if (membership != d.last_membership) { d.last_membership = std::move(membership); ++d.revision; }
    if (gpu_nodes.empty()) gpu_jobs.clear();
    const auto cut_start=PagingClock::now();
    const bool cut_ready=renderer.set_geometry_cut(gpu_nodes, gpu_roots, gpu_jobs, error, d.raster_only);
    if(d.profiling)d.profile.cut_upload.add(elapsed_ms(cut_start));
    if (!cut_ready) return false;
    if (d.raster_only && !geometry_pressure && !cpu_pressure) {
        d.scene_sources=admitted; d.scene_draws=selected;d.scene_source_fallbacks=d.source_fallbacks;
        d.scene_bias.clear(); d.scene_bias.reserve(admitted.size());
        for (const auto& source:admitted) d.scene_bias.push_back(renderer.part_draw_override(source.part_hash).lod_bias);
        d.scene_height=frame.extent.height; d.scene_fov=camera.vertical_fov_radians; d.scene_detail=detail_scale;
        d.scene_valid=true;
    }
    output = std::move(selected); error.clear(); return true;
}
uint64_t GeometryWorldRuntime::source_part(uint64_t instance_id, uint64_t fallback) const {
    auto it = d_->source_by_instance.find(instance_id);
    return it == d_->source_by_instance.end() ? fallback : it->second;
}
uint64_t GeometryWorldRuntime::revision() const { return d_->revision; }
geometry::ResidencyStats GeometryWorldRuntime::stats() const { return d_->residency.stats(); }
GeometryPagingProfile GeometryWorldRuntime::take_profile() {
    auto& d=*d_;auto result=d.profile;d.profile={};
    result.scene_pending=d.scene_pending;
    result.rejected_assets=static_cast<uint32_t>(d.rejected_admissions.size());
    result.source_fallbacks=d.source_fallbacks;
    for(const auto& entry:d.assets)if(!d.residency.ready(entry.second.lease))++result.unready_assets;
    std::lock_guard<std::mutex> lock(d.queue_mutex);
    auto& w=d.worker_profile;
    result.worker_queue=w.worker_queue;result.cache_open=w.cache_open;result.cache_refresh=w.cache_refresh;
    result.page_read=w.page_read;result.decode=w.decode;result.disk_reads=w.disk_reads;result.disk_bytes=w.disk_bytes;
    result.hierarchy_worker=w.hierarchy_worker;result.scene_worker=w.scene_worker;
    const auto bank = d_->page_bank ? d_->page_bank->stats() : MemBankStats{};
    result.bank_capacity=bank.capacity; result.bank_occupied=bank.occupied;
    result.bank_largest_free=bank.largest_free; result.bank_backing_allocations=bank.backing_allocations;
    result.prefetched_pages=w.prefetched_pages;result.cache_hits=w.cache_hits;result.read_requests=w.read_requests;result.cpu_payload_bytes=w.cpu_payload_bytes;
    const auto bytes=w.cpu_payload_bytes;w={};w.cpu_payload_bytes=bytes;
    size_t queued=0,preparing=0,completed=0;d.pipeline->counts(queued,preparing,completed);
    result.queued_reads=static_cast<uint32_t>(queued);result.completed_reads=static_cast<uint32_t>(completed);
    result.queued_prepare=static_cast<uint32_t>(preparing);
    result.prepared_pages=static_cast<uint32_t>(d.prepared.size());result.pending_uploads=static_cast<uint32_t>(d.uploads.size());
    return result;
}
} // namespace viewer
