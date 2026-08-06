#pragma once

// Bake-time subtree flattening: merge a root part's whole child hierarchy
// (transforms applied, TriEx carried) into ONE mesh, split it into spatial
// clusters, build a per-cluster error-bounded LOD ladder, and save the result
// as a v3 FLAT section of the part bundle at cache_path_flat(root_hash). The viewer
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

    // BENEFIT FLOOR (M1.5, redesign doc §3.3). A coarser rung is admitted only
    // when decimation removed at least this FRACTION of the previous surviving
    // rung's triangles. Until 2026-08-04 the test was merely
    // `geo.size() < prev_count` — one triangle qualified — which is why a rock's
    // LOD tint cycled through colours while its silhouette stood still: the
    // coarse tail ran 214 -> 206 -> 190 (4 %, then 8 %), three rungs and one
    // shape. The ladder was driven by error tolerance and never once asked
    // whether a rung bought anything.
    //
    // 0.30 was measured, not guessed. Sweeping every divisor over the 19 distinct
    // parts of RockGallery + PomProofBrick (MATTER_FLATTEN_LADDER=2) and
    // histogramming the 116 rungs the old rule admitted gives a bimodal
    // distribution: 29 rungs under 10 % (down to 0.5 % — 392 -> 390 triangles),
    // a thin 24-rung tail from 10-29 %, then 63 rungs bunched at 30-54 %. The
    // density steps up 2.5x at 30 %, which is the boundary between the ladder
    // working and the ladder idling. It agrees with theory: radius_divisor is a
    // ratio-2 schedule on epsilon, and QEM in its linear regime sheds ~50 % of
    // its triangles per doubling of tolerance, so 0.30 sits at 60 % of the
    // schedule's design intent — permissive enough that a mesh already near its
    // topological floor still earns a couple of coarse rungs.
    //
    // Tighter was tried and rejected on evidence: at 0.40 the small rocks
    // collapse to near-degenerate coarsest levels (94 -> 42 -> 12 tris) and the
    // 4258-triangle hero boulder takes a 54 % first step at close range. At 0.30
    // that boulder's ladder is bit-for-bit the one it had before, because every
    // step it already took exceeded 30 % — direct evidence the floor is not
    // over-tightening.
    //
    // Dropping a rung is error-CONSERVATIVE, not a fidelity cut: level i is
    // selected while its own eps still projects under pixel_budget (see the
    // threshold fill in part_flatten.cpp), so a removed rung means the FINER
    // surviving rung is drawn over that band instead. The one place fidelity can
    // fall is the coarsest rung, where a ladder that used to be truncated by
    // kMaxSerializedLodLevels now reaches a genuinely coarser terminal.
    //
    // There is deliberately no artifact field recording where the ladder bottoms
    // out: once admission terminates on benefit exhaustion, the LAST rung IS the
    // bottom-out point, which is what M2.5's impostor terminal keys off. The
    // .flat.part format is unchanged.
    float min_level_benefit = 0.30f;

    // TERMINAL IMPOSTOR (M2.5, redesign §2/§3.3). When set, the rung the
    // benefit floor above left LAST gets one more rung after it that is not a
    // mesh: a two-triangle camera-facing billboard sampling a baked view atlas,
    // written to a `.fimp` sidecar beside the .flat.part. It is an ordinary
    // entry in this same ladder -- same blas_indices, same threshold table --
    // so runtime selection, the indirect draw and the LOD trace need no new
    // code. Eligibility and atlas sizing live in impostor_bake.h with their
    // derivations. Set false to bake a mesh-only ladder (what shipped before).
    bool impostor_terminal = true;

    // How far out the terminal impostor takes over, as a multiplier on the
    // one-more-ratio-2-step the default ladder would otherwise use. 1.0 keeps
    // the historical placement; SMALLER brings billboards CLOSER to the camera.
    //
    // The switch distance scales with the rung's error epsilon, so this scales
    // eps directly: 0.5 halves the distance at which every default-ladder part
    // becomes a billboard, without touching any mesh rung's own distance.
    //
    // That independence is the point. The global dials that already existed —
    // pixel_budget and lod_bias — move the WHOLE ladder, so pulling impostors
    // in with them drags every mesh rung coarser at the same time, which is
    // exactly the trade Jack rejected ("I'd have to dial back the pixel budget
    // dramatically, which brings everything down in detail").
    //
    // Bake-time, so it needs a re-bake to take effect. Per-part control is
    // `LOD.impostor({ at })` (design 3.4), which overrides this entirely.
    float impostor_distance_scale = 1.0f;

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
    // M2.5: clusters that earned a terminal impostor rung (0 = none, which is
    // the correct outcome for a part whose terminal mesh is already tiny).
    size_t      impostors = 0;
};

// Flatten the subtree rooted at root_hash. Reads parts from
// the REP0 sections under <cache_root>/parts/, writes the root bundle's FLAT section
// (atomic). Idempotent and content-addressed: callers should skip the call when
// the flat file already exists, since any subtree change changes root_hash.
FlattenResult flatten_part(const std::string& cache_root, uint64_t root_hash,
                           const FlattenTargets& targets = FlattenTargets());

} // namespace part_flatten
