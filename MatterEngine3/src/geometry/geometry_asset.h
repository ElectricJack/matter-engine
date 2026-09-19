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
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
// Worker-only disk operations. Loaded roots own their page allocations; the
// temporary cache/index may retire without invalidating these immutable views.
std::shared_ptr<const CachedAsset> load_asset(const std::string& directory,
                                            const std::string& key, std::string& error);
std::shared_ptr<const CachedAsset> cache_asset(const std::string& directory,
    const std::string& key, const MeshIndexed&, const CompileConfig&,
    const std::vector<asset_store::PageSection>& metadata, std::string& error,
    RootCache* roots = nullptr, CacheReport* report = nullptr,
    bool cache_only = false);
} // namespace geometry
