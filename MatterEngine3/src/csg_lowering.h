#pragma once
// MatterEngine3/src/csg_lowering.h
//
// Lowering from the authoring DSL to the mesher. A part script produces a flat
// list of CSG ops (dsl::BuildBuffer, dsl_state.h); this converts it into the
// shapes MatterSurfaceLib's field evaluator actually consumes -- hashed sphere
// particles, carve particles, typed fat primitives (fat_primitive.h) and the
// ordered stage list (csg_stages.h).
//
// Also declares the analytic oracles over the same op list: field_is_solid()
// answers "is this world point inside?" and field_distance() returns the signed
// distance, both with no mesher and no GPU. Tests use them to assert what the
// field should be; DslState::raycast uses field_distance() to sphere-trace the
// authored field.
//
// Conventions: world metres, distances negative inside, `smoothing` is the
// smooth-min fillet k (k <= 1e-5 behaves as hard boolean ops). Sphere and box
// brushes are center-relative while capsule and cylinder carry their own segment
// endpoints -- csg_lowering.cpp's header has the full note, and getting it wrong
// misplaces only the segment brushes.
//
// All three entry points are pure functions of their arguments: they allocate,
// but touch no globals, no GPU state and no files, so they are safe to call from
// any bake worker thread.
#include "dsl_state.h"
#include "cluster.h"        // StaticParticle
#include "particle.h"       // Particle
#include "fat_primitive.h"  // FatPrim (typed iso-primitives)
#include "csg_stages.h"     // CsgStageOp (ordered CSG)
#include <vector>

namespace dsl {

// One part's authored CSG expression, split into the streams the field
// evaluator knows how to walk. Everything is in WORLD space: lowering has
// already applied each brush's transform-stack top, so nothing downstream needs
// the DSL's matrix stack.
//
// Some brushes deliberately appear in two streams, because two evaluation paths
// read this struct. The legacy hot path reads `additive` plus the trailing
// `carve` scan; the ordered/staged path reads `staged_spheres` and `fat` with
// their stage indices. Which one runs is decided by `stages` (see below), so
// both sets must be filled on every lowering.
//
// Plain value type: owns its vectors, cheap to move, holds no GPU or file
// resources.
struct LoweredField {
    std::vector<StaticParticle> additive;  // union/intersection brushes (sphere hot path)
    std::vector<int>            additive_stage;  // CSG stage index per `additive` entry
    std::vector<Particle>       carve;     // difference brushes (legacy carve scan)
    float smoothing = 0.0f;                // whole-expression smooth-min k

    // --- Typed iso-primitives + ordered CSG (Phase 1) ---------------------
    // Non-sphere brushes (oriented box). Borrowed/linear-scanned by the field
    // eval; NOT inserted into the spatial hash.
    std::vector<FatPrim> fat;

    // Ordered CSG stage ops, in authored order (consecutive same-op ops merged
    // into one stage). fat[j].stage tags fat primitive j. When stages.size()<=1
    // and no Difference stage exists the field eval uses the legacy byte-identical
    // single-union path.
    std::vector<CsgStageOp> stages;

    // Unified hashed-sphere stream for the STAGED field eval. Unlike `additive`
    // (additive-only, the legacy cell hot path), this carries EVERY sphere brush
    // (Union AND Difference AND Intersection) so the staged eval can fold a sphere
    // Difference as a real Difference stage (smax(field,-d)) rather than a trailing
    // carve. `staged_stage[i]` is the stage index of staged_spheres[i].
    std::vector<Particle>   staged_spheres;
    std::vector<int>        staged_stage;    // parallel to `staged_spheres`
};

// Lowers the flat CSG op list to the mesher input contract. Additive sphere = 1
// StaticParticle (now carrying its invTransform-derived scaled radius); box = one
// oriented FatPrim. Difference ops become carve particles AND/OR Difference stages.
// Consecutive same-op brushes merge into one ordered CSG stage. Transform stack top
// is applied to each brush.
LoweredField lower_build_buffer(const BuildBuffer& buf);

// Analytic occupancy oracle: evaluates the CSG expression's solidity (>0 inside)
// at a world point. Used by tests to assert primitive/CSG occupancy without GL.
bool field_is_solid(const BuildBuffer& buf, const Vector3& worldPoint);

// Analytic signed-distance oracle over ops[opBegin, opEnd), mirroring the
// mesher's staged smooth-min field (surface.c): per-stage log-sum-exp smin
// with fillet k, stages folded in authored order (consecutive same-op ops =
// one stage; field starts at +INFINITY and every stage applies its op, so an
// opening Difference/Intersection yields nothing). k <= 1e-5 = hard ops.
// Returns +INFINITY (1e9f) when the range is empty. Distances under
// non-uniform brush transforms are distorted (same caveat as field_is_solid)
// — callers must trace conservatively.
float field_distance(const BuildBuffer& buf, size_t opBegin, size_t opEnd,
                     float k, const Vector3& worldPoint);

} // namespace dsl
