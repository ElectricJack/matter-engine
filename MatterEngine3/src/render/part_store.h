#ifndef VIEWER_PART_STORE_H
#define VIEWER_PART_STORE_H

#include "blas_manager.hpp"     // MSL BLASManager / BLASHandle
#include "lod_select.h"         // lod_select::PartLodTable
#include "part_asset_v2.h"      // part_asset::ChildInstance
#include "raster_mesh.h"        // RasterMeshData

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace viewer {

// A single drawable node in a part's precomputed expansion table.
// Produced by build_expansion(); consumed by the GPU culler (Task 5+).
struct ExpandedNode {
    uint64_t part_hash;          // hash of the part that owns the drawable lod_mesh_data
    float    rel_transform[16];  // engine row-major, relative to instance root
};

// Per-cluster data loaded from a v3 flat artifact.
// aabb_min/aabb_max and radius describe the cluster's spatial extent (for Task 13
// per-cluster GPU culling).  thresholds / lod_blas / lod_mesh are parallel arrays
// (one entry per LOD level in the cluster's own ladder).
// lod_blas[i]  : BLASHandle in the shared BLASManager (legacy lod_blas entries may
//                re-use the SAME handle when the whole-part and per-cluster entries
//                are identical, but typically the whole-part legacy level is a new
//                merged registration).
// lod_mesh[i]  : index into LoadedPart::lod_mesh_data for the cluster's i-th level
//                mesh-data (stored there to keep mesh-data ownership in one place).
struct LoadedCluster {
    float aabb_min[3];
    float aabb_max[3];
    float radius;                          // half AABB diagonal
    std::vector<float>    thresholds;      // per ladder level
    std::vector<uint32_t> lod_blas;        // per level: BLAS index in the SHARED blas_
    std::vector<int>      lod_mesh;        // per level: index into lp.lod_mesh_data
};

// A part loaded into the shared BLASManager: one BLAS handle per LOD level
// (regenerated via lod_bake, since .part stores only LOD0), plus the LOD
// metadata the SectorResolver needs.
struct LoadedPart {
    std::vector<BLASHandle> lod_blas;       // lod_blas[i] -> BLAS for LOD level i
    float                   bound_radius = 0.0f;
    std::vector<float>      thresholds;      // per-LOD screen-size thresholds
    std::vector<part_asset::ChildInstance> children;   // baked child-instance table (may be empty)
    std::vector<RasterMeshData> lod_mesh_data;  // parallel to lod_blas (CPU-only; GL upload is lazy)
    std::vector<LoadedCluster>  clusters;        // non-empty iff a v3 flat was loaded
    std::vector<ExpandedNode>   expansion;       // precomputed flattened drawable nodes (Task 4)
};

// Walk the part tree rooted at root_hash depth-first, depth-capped at 8.
// Calls visitor(lp, hash, rel_transform, depth) for every reachable node;
// rel_transform is engine row-major, accumulated from the root (identity at
// depth 0). The visitor may filter on geometry predicates (e.g. lod_blas /
// lod_mesh_data) as needed for its specific use case.
// getter returns nullptr for unloadable parts (their subtrees are skipped).
// GL-free: suitable for headless tests and the GPU culler compute path.
void walk_part_tree(uint64_t root_hash,
                    const std::function<const LoadedPart*(uint64_t)>& getter,
                    const std::function<void(const LoadedPart*, uint64_t,
                                            const float /*rel*/[16], int /*depth*/)>& visitor);

// Precompute the flat list of drawable (part_hash, rel_transform) nodes for a
// compositional part tree rooted at root_hash. Implemented via walk_part_tree;
// visits only nodes with non-empty lod_mesh_data. Depth cap 8.
// GL-free: suitable for headless tests and the GPU culler compute path.
void build_expansion(uint64_t root_hash,
                     const std::function<const LoadedPart*(uint64_t)>& getter,
                     std::vector<ExpandedNode>& out);

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

    // Release a loaded part from CPU memory (lod_mesh_data, BLAS handles, clusters).
    // After this call get_or_load(part_hash) will re-read from disk on next access.
    // Safe no-op if part_hash is not currently loaded.
    void release(uint64_t part_hash);

    // Task 2: set the scratch directory for transient parts.
    // get_or_load probes scratch first, then falls back to cache_root_.
    void set_scratch_dir(std::string dir) { scratch_dir_ = std::move(dir); }

    // TEST-ONLY: register a pre-built LoadedPart under a hash without any disk I/O.
    // Used by gpu_cull_tests to build synthetic fixtures. Not for production use.
    void inject_for_test(uint64_t part_hash, LoadedPart lp) {
        loaded_[part_hash] = std::move(lp);
    }

private:
    std::string disk_path(uint64_t part_hash) const;   // cache_root_ + "/parts/<hash>.part"

    // Load a bake-time flattened artifact (<hash>.flat.part) if present: uses its
    // stored LOD ladder directly (no re-bake) and leaves children empty. Returns
    // false when absent/unusable so get_or_load falls back to the compositional path.
    bool load_flat(uint64_t part_hash, LoadedPart& lp);

    std::string                       cache_root_;
    std::string                       scratch_dir_;     // Task 2: transient scratch dir
    BLASManager                       blas_;
    std::map<uint64_t, LoadedPart>    loaded_;
    std::set<uint64_t>                load_failed_;      // suppress repeat logging
};

} // namespace viewer

#endif // VIEWER_PART_STORE_H
