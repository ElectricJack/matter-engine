#pragma once

// Bake-time subtree flattening: merge a root part's whole child hierarchy
// (transforms applied, TriEx carried) into ONE mesh, split it into spatial
// clusters, build a per-cluster error-bounded LOD ladder, and save the result
// as a v3 .flat.part artifact under cache_path_flat(root_hash). The viewer
// then renders the root as a single flat instance per cluster instead of
// re-expanding hundreds of child instances every frame.
//
// GL-free: consumes .part files from the cache and writes one back.

#include <cstdint>
#include <string>
#include <vector>

namespace part_flatten {

struct FlattenTargets {
    // eps_i = bound_radius / radius_divisor[i], finest ladder step first.
    // Level 0 is always the full cluster mesh (no decimation).
    // bound_radius is computed from the WHOLE flattened mesh so epsilon is
    // consistent across clusters (a cluster's local radius would give very
    // different ladder spacing for small vs. large clusters).
    // Ratio-2 schedule (Stage 2): finer near rungs (switch sooner, smaller pops),
    // coarser far rungs (a terrain tile drops to tens of tris at distance).
    std::vector<float> radius_divisor = {512.0f, 256.0f, 128.0f, 64.0f,
                                         32.0f, 16.0f, 8.0f, 4.0f, 2.0f};

    // Selection thresholds are derived from eps: a level becomes eligible when
    // its world-space error projects below pixel_budget pixels.
    // pixel_angle ~= vertical fov (rad) / vertical resolution.
    float pixel_angle  = 1.047f / 720.0f;
    float pixel_budget = 1.0f;

    // Stop adding coarser rungs once a level lands at/below this triangle
    // count, or when a rung stops shrinking. Replaces the old min_tris=2000
    // floor that froze small parts at LOD0 forever (Stage 2).
    int min_level_tris = 32;

    // Cluster size target: split_clusters targets at most this many tris per
    // cluster. 16000 is the Task 11 default (matches the brief).
    uint32_t cluster_target_tris = 16000;

    // Child recursion cap; mirrors the viewer WorldComposer's depth cap.
    int max_depth = 8;

    // Bake-hardening #2: budget for a single part's inline flatten, expressed
    // as the max bytes we're willing to hold in the intermediate TriEx buffer
    // while merging. When a subtree's estimated post-expansion size exceeds
    // this, the parent stays as an "instance boundary" and its .flat.part
    // stores the children as instance_refs (see FlatInstanceRef) instead of
    // inlining their triangles. Default 512 MB is a rough ceiling for the
    // biggest hand-authored composite (a full mesh model with dense LOD0);
    // scatter-heavy roots (StressForest) trip it and land on the instance-ref
    // path automatically. Schemas / build scripts may override.
    size_t budget_tri_bytes = 512ull * 1024ull * 1024ull;
};

// Bake-hardening #2: decision recorded per part during the bottom-up pass.
// INLINE  : this part's subtree is small enough to merge into a single mesh.
// BOUNDARY: this part stays as a stand-alone artifact; every parent that
//           references it emits a FlatInstanceRef pointing at its .flat.part
//           instead of inlining its geometry.
enum class FlattenDecision : uint8_t { INLINE = 0, BOUNDARY = 1 };

struct FlattenResult {
    bool        ok = false;
    std::string error;
    size_t      levels = 0;         // max LOD levels over all clusters (incl. level 0)
    size_t      clusters = 0;       // number of spatial clusters written
    size_t      full_tris = 0;      // triangle count of the merged level-0 mesh
    size_t      coarsest_tris = 0;  // triangle count of the last ladder level (last cluster)
    // Bake-hardening #2: instance-boundary children not expanded into the
    // merged mesh; recorded as FlatInstanceRefs in the .flat.part trailer for
    // the runtime consumer to expand into world instances.
    size_t      instance_refs = 0;
};

// Flatten the subtree rooted at root_hash. Reads parts from
// <cache_root>/parts/<hash>.part, writes <cache_root>/parts/<root>.flat.part
// (atomic). Idempotent and content-addressed: callers should skip the call when
// the flat file already exists, since any subtree change changes root_hash.
FlattenResult flatten_part(const std::string& cache_root, uint64_t root_hash,
                           const FlattenTargets& targets = FlattenTargets());

} // namespace part_flatten
