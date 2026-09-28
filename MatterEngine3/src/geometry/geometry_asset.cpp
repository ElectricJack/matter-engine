#include "geometry_asset.h"
#include <mutex>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include "matter/log.h"

namespace geometry {
uint32_t remove_zero_area_triangles(MeshIndexed& mesh) {
    if (mesh.indices.size() != mesh.triex.size()*3) return 0;
    for (auto index : mesh.indices) if (index >= mesh.positions.size()) return 0;
    size_t keep = 0;
    const size_t count = mesh.triex.size();
    for (size_t t = 0; t < count; ++t) {
        const auto a = mesh.positions[mesh.indices[t*3]];
        const auto b = mesh.positions[mesh.indices[t*3+1]];
        const auto c = mesh.positions[mesh.indices[t*3+2]];
        const double ux=double(b.x)-a.x, uy=double(b.y)-a.y, uz=double(b.z)-a.z;
        const double vx=double(c.x)-a.x, vy=double(c.y)-a.y, vz=double(c.z)-a.z;
        if (uy*vz-uz*vy == 0 && uz*vx-ux*vz == 0 && ux*vy-uy*vx == 0) continue;
        if (keep != t) {
            for (size_t k=0; k<3; ++k) mesh.indices[keep*3+k]=mesh.indices[t*3+k];
            mesh.triex[keep]=mesh.triex[t];
        }
        ++keep;
    }
    mesh.indices.resize(keep*3); mesh.triex.resize(keep);
    return static_cast<uint32_t>(count-keep);
}
struct RootCache::Impl {
    asset_store::PageCacheConfig config;
    std::unique_ptr<asset_store::PageCache> cache;
    mutable std::mutex mutex;
};
RootCache::RootCache(std::string directory, uint64_t bytes) : d_(new Impl) {
    d_->config.store.dir = std::move(directory); d_->config.resident_bytes = bytes;
    d_->config.bank = asset_store::PageBank::create(static_cast<size_t>(bytes));
}
RootCache::~RootCache() = default;
const std::string& RootCache::directory() const { return d_->config.store.dir; }
asset_store::PageCacheStats RootCache::stats() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return d_->cache ? d_->cache->stats() : asset_store::PageCacheStats{};
}
std::shared_ptr<const CachedAsset> RootCache::load(const std::string& key, std::string& error, CacheLoadStatus* status) {
    if (status) *status = CacheLoadStatus::Failed;
    std::lock_guard<std::mutex> lock(d_->mutex);
    if (!d_->config.bank) { error = "geometry root bank allocation failed"; return {}; }
    if (!d_->cache) {
        std::error_code ec;
        const bool exists = std::filesystem::exists(d_->config.store.dir, ec);
        if (!exists && !ec) {
            if (status) *status = CacheLoadStatus::Missing;
            error = "geometry cache directory absent"; return {};
        }
        d_->cache = asset_store::PageCache::open(d_->config, error);
    }
    if (!d_->cache) return {};
    auto& cache = d_->cache;
    auto result = std::make_shared<CachedAsset>(); result->directory = directory(); result->key = key;
    const auto manifest = cache->read_manifest(key);
    if (!manifest.page) {
        if (status && manifest.status == asset_store::PageStatus::Missing) *status = CacheLoadStatus::Missing;
        error = manifest.status == asset_store::PageStatus::BudgetExceeded
            ? "geometry root manifest budget exceeded" : "geometry manifest unavailable";
        return {};
    }
    result->manifest = manifest.page;
    std::vector<NodeRef> roots;
    if (!decode_roots(result->manifest, roots, error)) return {};
    // Roots are mandatory, but only roots: never walk the fine-page directory
    // at asset admission. Their explicit RAM cap may reject a poorly reduced
    // asset instead of secretly loading its complete source hierarchy.
    std::vector<asset_store::PageResult> pages;
    for (size_t begin = 0; begin < roots.size(); begin += d_->config.max_requests) {
        std::vector<asset_store::BlobHash> hashes;
        for (size_t i = begin; i < roots.size() && i < begin + d_->config.max_requests; ++i) hashes.push_back(roots[i].page);
        auto batch = cache->read_partitioned(hashes);
        pages.insert(pages.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
    }
    for (size_t i = 0; i < roots.size(); ++i) {
        const auto& ref = roots[i];
        NodeView root;
        if (i >= pages.size() || !pages[i].page) {
            error = i < pages.size() && pages[i].status == asset_store::PageStatus::BudgetExceeded
                ? "geometry root payload budget exceeded" : "geometry root payload unavailable";
            return {};
        }
        if (!decode_node(pages[i].page, root, error)) return {};
        if (root.self.page != ref.page || root.self.triangles != ref.triangles ||
            root.self.source_triangles != ref.source_triangles || root.self.error != ref.error) {
            error = "geometry root does not match manifest"; return {};
        }
        for (int k = 0; k < 3; ++k)
            if (root.self.bounds.lo[k] != ref.bounds.lo[k] || root.self.bounds.hi[k] != ref.bounds.hi[k]) {
                error = "geometry root bounds do not match manifest"; return {};
            }
        result->roots.push_back(std::move(root));
    }
    if (status) *status = CacheLoadStatus::Hit;
    error.clear(); return result;
}
std::shared_ptr<const CachedAsset> load_asset(const std::string& directory,
                                            const std::string& key, std::string& error) {
    RootCache cache(directory); return cache.load(key, error);
}
std::shared_ptr<const CachedAsset> cache_asset(const std::string& directory,
    const std::string& key, const MeshIndexed& source, const CompileConfig& config,
    const std::vector<asset_store::PageSection>& metadata, std::string& error, RootCache* roots, CacheReport* report, bool cache_only) {
    CacheReport local; if (!report) report = &local; *report = {};
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](Clock::time_point t) { return std::chrono::duration<double,std::milli>(Clock::now()-t).count(); };
    if (roots && roots->directory() != directory) { error = "geometry root cache directory mismatch"; return {}; }
    const auto load = [&] {
        if (roots) return roots->load(key, error, &report->lookup);
        RootCache temporary(directory); return temporary.load(key, error, &report->lookup);
    };
    auto start = Clock::now();
    auto cached = load(); report->lookup_ms = elapsed(start);
    if (cached) return cached;
    report->reason = error;
    if (cache_only || report->lookup != CacheLoadStatus::Missing) return {};
    start = Clock::now(); report->compiled = true;
    Hierarchy hierarchy;
    if (!compile(source, config, hierarchy, error)) { report->compile_ms = elapsed(start); return {}; }
    report->compile_ms = elapsed(start);
    start = Clock::now();
    // Compile in parallel; serialize only the writer session and its commit.
    // Existing BlobStore's cross-process lease remains the disk authority.
    static std::mutex writer_mutex;
    double writer_wait_ms=0, recheck_ms=0, open_ms=0, hierarchy_write_ms=0;
    {
        std::lock_guard<std::mutex> lock(writer_mutex);
        writer_wait_ms=elapsed(start);
        auto step=Clock::now();
        const auto initial = report->lookup;
        if (auto cached = load()) { report->lookup = initial; return cached; }
        recheck_ms=elapsed(step); step=Clock::now();
        const auto second = report->lookup; report->lookup = initial;
        if (second != CacheLoadStatus::Missing) return {};
        asset_store::StoreConfig store_config; store_config.dir = directory;
        auto store = asset_store::BlobStore::open(store_config, &error);
        if (!store) return {};
        auto refs = asset_store::RefTable::open(*store, {}, &error);
        if (!refs) return {};
        open_ms=elapsed(step);step=Clock::now();
        std::vector<NodeRef> roots;
        if (!write_hierarchy(hierarchy, *store, *refs, key, {}, roots, error, metadata)) return {};
        hierarchy_write_ms=elapsed(step);
    }
    report->write_ms = elapsed(start);
    if(std::getenv("MATTER_GEOMETRY_PAGES_PROFILE"))
        MATTER_LOGI("geometry", "cache_write_profile key=%s writer_wait_ms=%.3f recheck_ms=%.3f open_ms=%.3f hierarchy_write_ms=%.3f total_ms=%.3f",key.c_str(),writer_wait_ms,recheck_ms,open_ms,hierarchy_write_ms,report->write_ms);
    const auto initial = report->lookup;
    auto result = load(); report->lookup = initial; return result;
}
} // namespace geometry
