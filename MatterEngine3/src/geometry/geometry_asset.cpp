#include "geometry_asset.h"
#include <mutex>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <map>
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
    // Pending assets must be loadable before their refs are visible on disk.
    // These pins use the same bounded page bank as disk-loaded roots.
    std::map<std::string, std::shared_ptr<const CachedAsset>> pending;
    mutable std::mutex writer_mutex;
    std::unique_ptr<asset_store::BlobStore> writer;
    std::unique_ptr<asset_store::RefTable> writer_refs;
    WriterStats writer_stats;
    std::chrono::steady_clock::time_point last_commit{};
    static constexpr uint32_t kCommitEveryAssets = 32;
    static constexpr std::chrono::seconds kCommitEvery{2};
    bool open_writer(std::string& error) {
        if (writer) return true;
        writer = asset_store::BlobStore::open(config.store, &error);
        if (!writer) return false;
        writer_refs = asset_store::RefTable::open(*writer, {}, &error);
        if (!writer_refs) { writer.reset(); return false; }
        last_commit = std::chrono::steady_clock::now();
        return true;
    }
    // Caller owns writer_mutex; lock order is always writer_mutex -> mutex.
    bool commit(std::string& error, bool in_write = false) {
        if (!writer || !writer_stats.pending_assets) { error.clear(); return true; }
        const auto start = std::chrono::steady_clock::now();
        if (!writer->flush_index()) { error = writer->last_error(); return false; }
        if (!writer_refs->flush()) { error = "geometry manifest reference commit failed"; return false; }
        if (std::getenv("MATTER_GEOMETRY_PAGES_PROFILE"))
            MATTER_LOGI("geometry", "cache_commit_profile assets=%llu in_write=%u total_ms=%.3f",
                static_cast<unsigned long long>(writer_stats.pending_assets), unsigned(in_write),
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count());
        writer_stats.pending_assets = 0;
        ++writer_stats.commits;
        last_commit = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        pending.clear();
        error.clear(); return true;
    }
};
RootCache::RootCache(std::string directory, uint64_t bytes) : d_(new Impl) {
    d_->config.store.dir = std::move(directory); d_->config.resident_bytes = bytes;
}
RootCache::~RootCache() {
    std::string error;
    if (!flush_writer(error)) MATTER_LOGW("geometry", "writer shutdown commit failed: %s", error.c_str());
}
bool RootCache::flush_writer(std::string& error) {
    std::lock_guard<std::mutex> lock(d_->writer_mutex);
    return d_->commit(error);
}
RootCache::WriterStats RootCache::writer_stats() const {
    std::lock_guard<std::mutex> lock(d_->writer_mutex);
    return d_->writer_stats;
}
const std::string& RootCache::directory() const { return d_->config.store.dir; }
asset_store::PageCacheStats RootCache::stats() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    auto result = d_->cache ? d_->cache->stats() : asset_store::PageCacheStats{};
    if (d_->config.bank) result.bank = d_->config.bank->stats();
    return result;
}
std::shared_ptr<const CachedAsset> RootCache::load(const std::string& key, std::string& error,
                                                   CacheLoadStatus* status, bool load_root_payloads) {
    if (status) *status = CacheLoadStatus::Failed;
    std::lock_guard<std::mutex> lock(d_->mutex);
    const auto pending = d_->pending.find(key);
    if (pending != d_->pending.end()) {
        if (load_root_payloads && pending->second->roots.empty() && !pending->second->root_refs.empty()) {
            error = "geometry pending root payloads unavailable";
            return {};
        }
        if (status) *status = CacheLoadStatus::Hit;
        error.clear(); return pending->second;
    }
    // PartStore owns this cache even with VG disabled. Commit its aggregate
    // payload bank only when a paging-enabled consumer actually loads roots.
    if (!d_->config.bank)
        d_->config.bank = asset_store::PageBank::create(static_cast<size_t>(d_->config.resident_bytes));
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
    result->root_refs = roots;
    if (!load_root_payloads) {
        if (status) *status = CacheLoadStatus::Hit;
        error.clear(); return result;
    }
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
std::shared_ptr<const CachedAsset> RootCache::write_asset(const std::string& key, const Hierarchy& hierarchy,
    const std::vector<asset_store::PageSection>& metadata, std::string& error, double* write_ms,
    bool load_root_payloads) {
    const auto start = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> writer_lock(d_->writer_mutex);
    if (!d_->open_writer(error)) return {};
    CacheLoadStatus status;
    if (auto existing = load(key, error, &status, load_root_payloads)) return existing;
    if (status != CacheLoadStatus::Missing) return {};
    std::vector<NodeRef> roots, all_refs;
    std::vector<uint8_t> manifest;
    if (!write_hierarchy(hierarchy, *d_->writer, *d_->writer_refs, key, d_->config.limits,
                         roots, error, metadata, false, &all_refs, &manifest)) return {};
    ++d_->writer_stats.assets_written;
    ++d_->writer_stats.pending_assets;
    if ((d_->writer_stats.pending_assets >= Impl::kCommitEveryAssets ||
         std::chrono::steady_clock::now() - d_->last_commit >= Impl::kCommitEvery) && !d_->commit(error, true)) return {};
    // The reader index may not include these bytes yet. Admit the in-memory
    // encoding through the reader's bank to preserve dedup and aggregate caps.
    std::lock_guard<std::mutex> lock(d_->mutex);
    if (!d_->cache) d_->cache = asset_store::PageCache::open(d_->config, error);
    if (!d_->cache) return {};
    auto result = std::make_shared<CachedAsset>();
    result->key = key; result->directory = directory();
    result->manifest = d_->cache->insert(manifest, error).page;
    if (!result->manifest) return {};
    result->root_refs = roots;
    for (size_t i = 0; load_root_payloads && i < hierarchy.roots.size(); ++i) {
        const auto& node = hierarchy.nodes[hierarchy.roots[i]];
        std::vector<NodeRef> children;
        for (auto child : node.children) children.push_back(all_refs[child]);
        std::vector<uint8_t> bytes;
        if (!encode_node(node, children, d_->config.limits, bytes, error)) return {};
        auto page = d_->cache->insert(bytes, error).page;
        if (!page) return {};
        NodeView root;
        if (!decode_node(page, root, error)) return {};
        result->roots.push_back(std::move(root));
    }
    if (d_->writer_stats.pending_assets) d_->pending[key] = result;
    if (write_ms) *write_ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count();
    error.clear(); return result;
}
std::shared_ptr<const CachedAsset> cache_asset(const std::string& directory,
    const std::string& key, const MeshIndexed& source, const CompileConfig& config,
    const std::vector<asset_store::PageSection>& metadata, std::string& error, RootCache* roots, CacheReport* report,
    bool cache_only, bool load_root_payloads) {
    CacheReport local; if (!report) report = &local; *report = {};
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](Clock::time_point t) { return std::chrono::duration<double,std::milli>(Clock::now()-t).count(); };
    if (!roots) { error = "geometry cache write requires a RootCache"; return {}; }
    if (roots->directory() != directory) { error = "geometry root cache directory mismatch"; return {}; }
    auto start = Clock::now();
    auto cached = roots->load(key, error, &report->lookup, load_root_payloads); report->lookup_ms = elapsed(start);
    if (cached) return cached;
    report->reason = error;
    if (cache_only || report->lookup != CacheLoadStatus::Missing) return {};
    start = Clock::now(); report->compiled = true;
    Hierarchy hierarchy;
    if (!compile(source, config, hierarchy, error)) { report->compile_ms = elapsed(start); return {}; }
    report->compile_ms = elapsed(start);
    start = Clock::now();
    double hierarchy_write_ms = 0;
    auto result = roots->write_asset(key, hierarchy, metadata, error, &hierarchy_write_ms, load_root_payloads);
    report->write_ms = elapsed(start);
    if(std::getenv("MATTER_GEOMETRY_PAGES_PROFILE"))
        MATTER_LOGI("geometry", "cache_write_profile key=%s hierarchy_write_ms=%.3f total_ms=%.3f pending=%llu",
            key.c_str(), hierarchy_write_ms, report->write_ms,
            static_cast<unsigned long long>(roots->writer_stats().pending_assets));
    return result;
}
} // namespace geometry
