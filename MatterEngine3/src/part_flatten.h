#pragma once

// Bake-time subtree flattening: merge a root part's whole child hierarchy
// (transforms applied, TriEx carried) into ONE mesh, split it into spatial
// clusters, build a per-cluster error-bounded LOD ladder, and save the result
// as a v3 .flat.part artifact under cache_path_flat(root_hash). The viewer
// then renders the root as a single flat instance per cluster instead of
// re-expanding hundreds of child instances every frame.
//
// GL-free: consumes .part files from the cache and writes one back.

#include <cmath>
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

// --------------- cutover math helpers (header-only, usable from gpu code) ----

// Column-0 length of a row-major 4x4 float matrix — extracts the uniform scale
// factor from a transform (assuming no shear).
inline float transform_uniform_scale(const float t[16]) {
    return std::sqrt(t[0]*t[0] + t[4]*t[4] + t[8]*t[8]);
}

// Compute the parent-ladder cutover threshold from a child's pixel-size inline
// threshold. The result is the world-space error value at which the parent LOD
// ladder should switch from inlining the child's geometry to referencing it as
// an instance.
//
// Formula: cutover = inline_below_px * pa * pb * parent_radius
//                    / (child_radius_local * ref_scale)
//
// Returns 0 on degenerate inputs (child_radius_local * ref_scale <= 0 or
// parent_radius <= 0) so callers can treat 0 as "always keep as instance ref".
inline float ref_cutover_threshold(float inline_below_px, float parent_radius,
                                   float child_radius_local, float ref_scale,
                                   const FlattenTargets& t) {
    const float denom = child_radius_local * ref_scale;
    if (denom <= 0.0f || parent_radius <= 0.0f) return 0.0f;
    return inline_below_px * t.pixel_angle * t.pixel_budget * parent_radius / denom;
}

// Map a cutover_threshold to a ladder level index: returns the smallest i such
// that cutover_threshold >= pb*pa*radius_divisor[i] (the level's nominal
// threshold). Returns (int)div.size() when no level qualifies (cutover is below
// all ladder rungs), meaning the entire ladder is "coarse" relative to this
// cutover — the part stays as an instance ref at all LODs.
//
// Levels [0, L*) are the fine segment (kept as instance refs); [L*, end) are
// the coarse segment (child geometry inlined into the merged mesh).
inline int cutover_level_index(float cutover_threshold, const FlattenTargets& t) {
    for (size_t i = 0; i < t.radius_divisor.size(); ++i)
        if (cutover_threshold >= t.pixel_budget * t.pixel_angle * t.radius_divisor[i])
            return (int)i;
    return (int)t.radius_divisor.size();
}

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
    // LOD-instanced-children: triangle counts for the fine/coarse segments
    // used by the cutover math helpers to split the ladder.
    size_t      fine_tris = 0;          // trunk-only QEM input (segmented flats)
    size_t      coarse_input_tris = 0;  // merged coarse-segment input
};

// Flatten the subtree rooted at root_hash. Reads parts from
// <cache_root>/parts/<hash>.part, writes <cache_root>/parts/<root>.flat.part
// (atomic). Idempotent and content-addressed: callers should skip the call when
// the flat file already exists, since any subtree change changes root_hash.
FlattenResult flatten_part(const std::string& cache_root, uint64_t root_hash,
                           const FlattenTargets& targets = FlattenTargets());

} // namespace part_flatten
