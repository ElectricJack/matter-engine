#ifndef VIEWER_LOCAL_PROVIDER_H
#define VIEWER_LOCAL_PROVIDER_H

#include "world_source.h"
#include "part_store.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace viewer {

struct LocalProviderConfig {
    std::string schemas_dir;      // ../examples/world_demo/schemas
    std::string world_data_dir;   // ../examples/world_demo/WorldData
    std::string world_name;       // "Demo"
    std::string shared_lib_dir;   // ../shared-lib
    std::string cache_root;       // persistent parts/ cache (NOT a /tmp throwaway)

    // Invoked during fetch_parts once per part processed (bake or cache hit):
    // module = part module name, done/total = progress through the want list.
    std::function<void(const char* module, int done, int total)> on_part;
};

// Drives the SP-3 install path over a persistent content-addressed cache and
// scatters the example world (terrain/trees/grass) into a WorldManifest. Same
// interface as a future NetworkProvider.
class LocalProvider : public WorldProvider {
public:
    explicit LocalProvider(LocalProviderConfig cfg);

    bool connect(WorldManifest& out, std::string& err) override;
    std::vector<uint64_t> reconcile(const WorldManifest& manifest,
                                    const PartStore& store) override;
    bool fetch_parts(const std::vector<uint64_t>& want,
                     PartStore& store, std::string& err) override;
    bool poll_deltas(WorldDelta& out) override;   // LocalProvider: always false (static world)

    int baked_count() const { return baked_count_; }
    int hit_count()   const { return hit_count_; }
    int baked_tileset_count() const { return baked_tileset_count_; }

private:
    LocalProviderConfig  cfg_;
    int                  baked_count_ = 0;
    int                  hit_count_   = 0;
    int                  baked_tileset_count_ = 0;
    std::set<uint64_t>   baked_hashes_;  // hashes freshly baked by last connect()
};

// Expand an assembly root's baked child-instance table (from its .part in
// cache_root) into individual world-manifest instances — one per child, with
// the child's stored transform. Generic: used for any manifest root flagged
// `expand`. Fails closed if the root artifact is missing or has no children.
bool append_expanded_children(const std::string& cache_root, uint64_t root_hash,
                              uint32_t& next_id,
                              std::vector<WorldManifestEntry>& out_instances,
                              std::string& err);

} // namespace viewer

#endif // VIEWER_LOCAL_PROVIDER_H
