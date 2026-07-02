#pragma once

// Bake-time subtree flattening: merge a root part's whole child hierarchy
// (transforms applied, TriEx carried) into ONE mesh, build an error-bounded
// LOD ladder from it, and save the result as a normal v2 .part with an empty
// child table under cache_path_flat(root_hash). The viewer then renders the
// root as a single instance per LOD instead of re-expanding hundreds of child
// instances every frame.
//
// GL-free: consumes .part files from the cache and writes one back.

#include <cstdint>
#include <string>
#include <vector>

namespace part_flatten {

struct FlattenTargets {
    // eps_i = bound_radius / radius_divisor[i], finest ladder step first.
    // Level 0 is always the full merged mesh (no decimation).
    std::vector<float> radius_divisor = {256.0f, 64.0f, 16.0f, 4.0f};

    // Selection thresholds are derived from eps: a level becomes eligible when
    // its world-space error projects below pixel_budget pixels.
    // pixel_angle ~= vertical fov (rad) / vertical resolution.
    float pixel_angle  = 1.047f / 720.0f;
    float pixel_budget = 1.0f;

    // Stop adding coarser levels once a level lands below this triangle count
    // (the far field belongs to imposters, not ever-coarser meshes).
    int min_tris = 2000;

    // Child recursion cap; mirrors the viewer WorldComposer's depth cap.
    int max_depth = 8;
};

struct FlattenResult {
    bool        ok = false;
    std::string error;
    size_t      levels = 0;         // LOD levels written (incl. full-res level 0)
    size_t      full_tris = 0;      // triangle count of the merged level-0 mesh
    size_t      coarsest_tris = 0;  // triangle count of the last ladder level
};

// Flatten the subtree rooted at root_hash. Reads parts from
// <cache_root>/parts/<hash>.part, writes <cache_root>/parts/<root>.flat.part
// (atomic). Idempotent and content-addressed: callers should skip the call when
// the flat file already exists, since any subtree change changes root_hash.
FlattenResult flatten_part(const std::string& cache_root, uint64_t root_hash,
                           const FlattenTargets& targets = FlattenTargets());

} // namespace part_flatten
