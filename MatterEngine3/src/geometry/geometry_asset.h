#pragma once
#include "geometry_hierarchy.h"

namespace geometry {
// Terrain contour output can contain zero-area faces. Remove only exact
// degenerates, preserving triangle-corner attributes and every nonzero face.
uint32_t remove_zero_area_triangles(MeshIndexed& mesh);
enum class CacheLoadStatus { Hit, Missing, Failed };
struct CacheReport {
    CacheLoadStatus lookup = CacheLoadStatus::Failed;
    bool compiled = false;
    double lookup_ms = 0, compile_ms = 0, write_ms = 0;
    std::string reason;
};
struct CachedAsset {
    std::string key;
    std::string directory;
    asset_store::PageHandle manifest;
    std::vector<NodeView> roots;
};
// One retained index and aggregate root-payload budget per PartStore, shared by
// concurrent staging jobs. Pinned roots remain charged across cache eviction.
class RootCache {
public:
    explicit RootCache(std::string directory, uint64_t bytes = 16ull << 20);
    ~RootCache();
    std::shared_ptr<const CachedAsset> load(const std::string& key, std::string& error,
                                            CacheLoadStatus* status = nullptr);
    const std::string& directory() const;
    asset_store::PageCacheStats stats() const;
    struct WriterStats { uint64_t assets_written = 0, commits = 0, pending_assets = 0; };
    // Commit staged assets at bake-batch boundaries. Also called at teardown;
    // cheap when no writes are pending. Readers see only complete commits.
    bool flush_writer(std::string& error);
    WriterStats writer_stats() const;
private:
    friend std::shared_ptr<const CachedAsset> cache_asset(const std::string&,
        const std::string&, const MeshIndexed&, const CompileConfig&,
        const std::vector<asset_store::PageSection>&, std::string&, RootCache*, CacheReport*, bool);
    std::shared_ptr<const CachedAsset> write_asset(const std::string&, const Hierarchy&,
        const std::vector<asset_store::PageSection>&, std::string&, double* write_ms);
    struct Impl;
    std::unique_ptr<Impl> d_;
};
// Worker-only disk operations. RootCache is required; returned immutable root
// and manifest views remain valid across commits and cache teardown.
std::shared_ptr<const CachedAsset> cache_asset(const std::string& directory,
    const std::string& key, const MeshIndexed&, const CompileConfig&,
    const std::vector<asset_store::PageSection>& metadata, std::string& error,
    RootCache* roots = nullptr, CacheReport* report = nullptr,
    bool cache_only = false);
} // namespace geometry
