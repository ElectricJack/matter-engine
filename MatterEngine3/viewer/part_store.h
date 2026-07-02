#ifndef VIEWER_PART_STORE_H
#define VIEWER_PART_STORE_H

#include "blas_manager.hpp"     // MSL BLASManager / BLASHandle
#include "lod_select.h"         // lod_select::PartLodTable
#include "part_asset_v2.h"      // part_asset::ChildInstance
#include "raster_mesh.h"        // RasterMeshData

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace viewer {

// A part loaded into the shared BLASManager: one BLAS handle per LOD level
// (regenerated via lod_bake, since .part stores only LOD0), plus the LOD
// metadata the SectorResolver needs.
struct LoadedPart {
    std::vector<BLASHandle> lod_blas;       // lod_blas[i] -> BLAS for LOD level i
    float                   bound_radius = 0.0f;
    std::vector<float>      thresholds;      // per-LOD screen-size thresholds
    std::vector<part_asset::ChildInstance> children;   // baked child-instance table (may be empty)
    std::vector<RasterMeshData> lod_mesh_data;  // parallel to lod_blas (CPU-only; GL upload is lazy)
};

// Owns one BLASManager shared across all loaded parts. Content-addressed and
// durable: a .part baked on a prior run is found on disk under cache_root/parts/.
class PartStore {
public:
    explicit PartStore(std::string cache_root);

    // True if the part is loaded in memory OR a .part exists on disk. Drives reconcile.
    bool has(uint64_t part_hash) const;

    // Load (memoized) a part: load_v2 -> lod_bake LODs -> register in the shared
    // BLASManager. Returns nullptr on failure (logged once per hash). idempotent.
    const LoadedPart* get_or_load(uint64_t part_hash);

    BLASManager& blas() { return blas_; }
    const std::string& cache_root() const { return cache_root_; }
    size_t loaded_count() const { return loaded_.size(); }

    // LOD table for the SectorResolver: radius + thresholds per loaded part.
    lod_select::PartLodTable part_lod_table() const;

private:
    std::string disk_path(uint64_t part_hash) const;   // cache_root_ + "/parts/<hash>.part"

    // Load a bake-time flattened artifact (<hash>.flat.part) if present: uses its
    // stored LOD ladder directly (no re-bake) and leaves children empty. Returns
    // false when absent/unusable so get_or_load falls back to the compositional path.
    bool load_flat(uint64_t part_hash, LoadedPart& lp);

    std::string                       cache_root_;
    BLASManager                       blas_;
    std::map<uint64_t, LoadedPart>    loaded_;
    std::set<uint64_t>                load_failed_;      // suppress repeat logging
};

} // namespace viewer

#endif // VIEWER_PART_STORE_H
