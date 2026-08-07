#pragma once

// Bake-time subtree flattening: merge a root part's whole child hierarchy
// (transforms applied, TriEx carried) into ONE mesh, split it into spatial
// clusters, build a per-cluster error-bounded LOD ladder, and save the result
// as a v3 FLAT section of the part bundle at cache_path_flat(root_hash). The viewer
// then renders the root as a single flat instance per cluster instead of
// re-expanding hundreds of child instances every frame.
//
// GL-free: consumes .part files from the cache and writes one back.

#include "matter/lod_contract.h"   // kMaxSerializedLodLevels (the rung cap clamp)
#include "impostor_bake.h"        // cell_px(): the ladder shape folds it

#include <cmath>
#include <cstdint>
#include <cstdlib>   // std::getenv, std::strtol, std::atof (ambient bake config)
#include <cstring>   // std::memcpy (float -> bits in the ladder digest)
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

    // MESH RUNG CAP. How many MESH reps the DEFAULT ladder may publish,
    // counting rep 0. 0 = unlimited, which is the shipped default and the
    // behaviour every flat already on disk was baked with.
    //
    // WHY, when min_level_benefit already stops the ladder. The benefit floor
    // asks whether a rung REMOVED enough triangles. It never asks whether what
    // survived still reads as the thing: a QEM tail four rungs down shares the
    // original's bounding box and very little else, and the ladder happily
    // keeps admitting those because each step does shed its 30 %. Past that
    // point a billboard baked from REP 0 is a strictly better picture -- it
    // carries the authored silhouette and shading rather than the accumulated
    // decimation error -- and it costs two triangles instead of two hundred.
    // Hence: more rungs is not better, so cap the mesh ladder and hand over to
    // the impostor earlier.
    //
    // The impostor append needs no knowledge of this. It keys off the LAST
    // rung the loop left (level_metas.back()), so capping the mesh ladder
    // pulls the billboard in to one ratio-2 step past that rung by
    // construction -- exactly "switch directly from a higher-resolution LOD to
    // an impostor", with no second placement rule to keep in sync.
    //
    // Env override: MATTER_LOD_MAX_MESH_RUNGS (see effective_max_mesh_rungs).
    // Bake-time, and folded into ladder_shape_digest below, so changing it
    // forces the affected flats to rebake instead of being served stale.
    //
    // Authored per-part ladders (`static lods`) are NOT capped, and neither is
    // the procedural budget-variant ladder: both NAME their reps, one per
    // authored entry, so a cap would silently delete something an author
    // asked for rather than trim a schedule the bake invented. `static lods`
    // IS the per-part mechanism. This knob is the global default-ladder dial
    // and nothing else. (Both of those paths are still bounded by
    // kMaxSerializedLodLevels, as they were before.)
    //
    // Enforced in BOTH default-ladder builders -- the classic per-cluster loop
    // and the segmented one. They are independent copies of the same rule, and
    // capping only one would make a segmented flat quietly disagree with an
    // unsegmented one about how many rungs a part has.
    uint32_t max_mesh_rungs = 0;

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

// --------------- ambient bake configuration (compiled defaults + env) -------
//
// These live in the HEADER, inline, for one reason: part_asset_v2 has to reach
// them to compute a flat's identity (see ladder_shape_digest below), and a
// header-only definition means that costs no link dependency from the
// serializer onto the baker.
//
// Every one of them reads getenv PER CALL and caches nothing in a static. That
// is deliberate and load-bearing: a test must be able to flip a knob between
// two bakes inside ONE process, which is the only way the staleness gate below
// can be exercised at all. It also matches how MATTER_FLATTEN_RETAIN_MB is
// read in part_flatten.cpp.

// MATTER_IMPOSTOR_DISTANCE: how far out the terminal impostor takes over, as a
// multiplier on the ratio-2 step the ladder would otherwise use. 0.5 makes
// every default-ladder part become a billboard at HALF the distance, without
// moving any mesh rung. That independence is the point -- pixel_budget and
// lod_bias move the WHOLE ladder, so pulling impostors in with them coarsens
// every mesh at the same time.
inline float env_impostor_distance_scale() {
    const char* v = std::getenv("MATTER_IMPOSTOR_DISTANCE");
    if (!v || !v[0]) return 1.0f;
    const float f = static_cast<float>(std::atof(v));
    // A non-positive or absurd value would silently move every switch in the
    // world; clamp to a sane window and ignore garbage rather than trust it.
    if (!(f > 0.0f) || f > 8.0f) return 1.0f;
    return f;
}

// MATTER_IMPOSTOR=0 bakes a mesh-only ladder, so the tier can be A/B'd against
// itself on one cache without a rebuild. Applies to BOTH the default ladder
// and the authored one, so the switch cannot mean different things on the two
// paths.
inline bool env_impostors_enabled() {
    const char* v = std::getenv("MATTER_IMPOSTOR");
    return !(v && v[0] == '0');
}

// The EFFECTIVE mesh rung cap: MATTER_LOD_MAX_MESH_RUNGS overrides
// FlattenTargets::max_mesh_rungs when it parses, already clamped. 0 out means
// "unlimited".
//
// FAIL CLOSED. Anything that is not a whole non-negative decimal number leaves
// the compiled value in place rather than being guessed at -- "", "3x", "-1",
// "abc" and "1e3" all mean "no override". A cap is a global shape change; a
// typo in an env var must not silently reshape every ladder in the world.
//
// A cap AT OR ABOVE kMaxSerializedLodLevels normalizes back to 0. Two reasons.
// First, it is not a cap: the serialized/renderer capacity already binds there
// (and cull.comp returns WITHOUT EMITTING ANY DRAW for a cluster whose
// lod_count exceeds MAX_LOD, so an over-large value must be clamped, never
// trusted). Second, normalizing keeps such a value digesting identically to
// the default, so a pointless setting does not invalidate every cache.
inline uint32_t effective_max_mesh_rungs(const FlattenTargets& t) {
    uint32_t n = t.max_mesh_rungs;
    if (const char* v = std::getenv("MATTER_LOD_MAX_MESH_RUNGS")) {
        if (v[0]) {
            char* end = nullptr;
            const long parsed = std::strtol(v, &end, 10);
            if (end && end != v && *end == '\0' && parsed >= 0 &&
                parsed <= 1000000L)
                n = static_cast<uint32_t>(parsed);
        }
    }
    if (n >= static_cast<uint32_t>(matter::kMaxSerializedLodLevels)) return 0;
    return n;
}

// targets x env. Multiplicative because both express the same thing (a
// multiplier on the ratio-2 step), so a per-bake setting and an operator's
// A/B compose instead of one silently winning.
inline float effective_impostor_distance_scale(const FlattenTargets& t) {
    const float s = (t.impostor_distance_scale > 0.0f)
                        ? t.impostor_distance_scale : 1.0f;
    return s * env_impostor_distance_scale();
}

inline bool effective_impostor_terminal(const FlattenTargets& t) {
    return t.impostor_terminal && env_impostors_enabled();
}

// --------------- the ladder-shape digest (the staleness gate) ---------------
//
// THE BUG THIS CLOSES. A flat artifact's cache identity was the resolved part
// hash plus the FLAT format version, and nothing else -- see
// LocalProvider::ensure_part_flattened, which returns early the moment
// is_cache_artifact_header_compatible says those two match. Both are blind to
// the ladder's SHAPE. Change a knob that admits fewer rungs, or that moves the
// impostor, and the bytes a bake WOULD write change while neither the hash nor
// the version does; the provider then finds a "compatible" flat and never
// bakes. The knob appears to do nothing -- silently, only on a warm cache, and
// therefore only for the person who already ran the engine once.
// MATTER_IMPOSTOR_DISTANCE shipped with exactly this defect.
//
// THE FIX is the one impostor_bake already applies to its atlas sidecar: make
// the artifact answer to what produced it, and REJECT it when it answers
// wrongly rather than drawing it (impostor_bake.cpp's depicts_hash check). So
// this digest is folded into the identity the FLAT section header carries.
// Both the cheap probe and the full load then reject a flat baked under a
// different ladder, and the provider re-flattens.
//
// 0 IS THE SHIPPED DEFAULT SHAPE, deliberately. It keeps a default bake
// BYTE-IDENTICAL to what shipped, so this change needs no
// matter_version::components bump and invalidates no existing cache on its
// own. Only a NON-default shape gets a non-zero digest, and only those
// artifacts are rejected.
//
// WHAT IS NOT IN HERE, and why. Only the AMBIENT configuration -- compiled
// defaults overridden by env -- is folded, never a caller's per-call
// FlattenTargets overrides. That is forced, not lazy: the reader of a flat
// never sees the writer's struct, so an identity the reader cannot reproduce
// is not a staleness gate, it is a file nothing can ever load. Writer and
// reader calling the SAME nullary function is what makes agreement structural
// instead of a plumbing invariant that every call site has to honour -- which
// is the failure mode version_vector.h's "one place a version enters a key"
// rule exists to prevent, and the one this whole fix is about.
//
// The consequence, stated plainly: passing a non-default SHAPE field
// programmatically still writes an artifact LABELLED with the ambient shape.
// Nothing in the engine does that (local_provider and live_edit_prod both call
// flatten_part with defaults) and flatten_part warns when a caller does. A
// knob that needs to reach the identity belongs in the env-backed set above.

inline uint64_t ladder_shape_mix(uint64_t h, uint64_t x) {
    uint64_t v = h ^ x;
    v += 0x9E3779B97F4A7C15ull;
    v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ull;
    v = (v ^ (v >> 27)) * 0x94D049BB133111EBull;
    return v ^ (v >> 31);
}

// Fold the BIT PATTERN, not the value: a change of any magnitude then changes
// the digest, and -0.0 / NaN cannot alias 0.0 the way a comparison would.
inline uint64_t ladder_shape_float_bits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return static_cast<uint64_t>(u);
}

inline uint64_t ladder_shape_raw(const std::vector<float>& divisors,
                                 float min_level_benefit, int min_level_tris,
                                 uint32_t max_mesh_rungs,
                                 float impostor_distance_scale,
                                 bool impostor_terminal,
                                 uint32_t impostor_cell_px) {
    uint64_t h = 0x4C41444445523031ull;   // "LADDER01"
    h = ladder_shape_mix(h, divisors.size());
    for (float d : divisors) h = ladder_shape_mix(h, ladder_shape_float_bits(d));
    h = ladder_shape_mix(h, ladder_shape_float_bits(min_level_benefit));
    h = ladder_shape_mix(h, static_cast<uint64_t>(
                                static_cast<int64_t>(min_level_tris)));
    h = ladder_shape_mix(h, max_mesh_rungs);
    h = ladder_shape_mix(h, ladder_shape_float_bits(impostor_distance_scale));
    h = ladder_shape_mix(h, impostor_terminal ? 1ull : 0ull);
    // The cell resolution reaches the FLAT, not just the atlas: guard_band()
    // is derived from it and scales half_extent, which is the billboard quad's
    // actual geometry. An atlas-only invalidation would leave a quad sized for
    // the old band.
    h = ladder_shape_mix(h, impostor_cell_px);
    return h;
}

inline uint64_t ladder_shape_digest(const FlattenTargets& t) {
    const uint64_t raw =
        ladder_shape_raw(t.radius_divisor, t.min_level_benefit,
                         t.min_level_tris, effective_max_mesh_rungs(t),
                         effective_impostor_distance_scale(t),
                         effective_impostor_terminal(t), impostor::cell_px());
    // The reference is the PRISTINE default -- the struct's own initializers,
    // with no env applied -- because that is the shape every flat already on
    // disk was baked with. Recomputed rather than cached in a static: the
    // struct's defaults are compile-time constants, so this costs a handful of
    // multiplies, and a static here would be one more thing to get wrong.
    const FlattenTargets d{};
    const uint64_t shipped =
        ladder_shape_raw(d.radius_divisor, d.min_level_benefit,
                         d.min_level_tris, d.max_mesh_rungs,
                         d.impostor_distance_scale, d.impostor_terminal,
                         impostor::kDefaultCellPx);
    if (raw == shipped) return 0;
    return raw ? raw : 1ull;   // never land on the sentinel by accident
}

// The digest of the configuration THIS PROCESS bakes with. This is the one
// both the writer and every reader call; see the note above for why it takes
// no arguments.
inline uint64_t active_ladder_shape_digest() {
    return ladder_shape_digest(FlattenTargets{});
}

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
