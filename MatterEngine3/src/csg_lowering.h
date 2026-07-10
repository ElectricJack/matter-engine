#pragma once
#include "dsl_state.h"
#include "cluster.h"        // StaticParticle
#include "particle.h"       // Particle
#include "fat_primitive.h"  // FatPrim (typed iso-primitives)
#include "csg_stages.h"     // CsgStageOp (ordered CSG)
#include <vector>

namespace dsl {

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
