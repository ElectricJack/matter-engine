#pragma once
#include "dsl_state.h"
#include "cluster.h"   // StaticParticle
#include "particle.h"  // Particle
#include <vector>

namespace dsl {

struct LoweredField {
    std::vector<StaticParticle> additive;  // union/intersection brushes
    std::vector<Particle>       carve;     // difference brushes
    float smoothing = 0.0f;                // whole-expression smooth-min k
};

// Lowers the flat CSG op list to the mesher input contract. sphere = 1 particle;
// box = analytic-SDF sphere stamp at op.spacing (crisp corners to spacing floor).
// Transform stack top is applied to each brush center.
LoweredField lower_build_buffer(const BuildBuffer& buf);

// Analytic occupancy oracle: evaluates the CSG expression's solidity (>0 inside)
// at a world point. Used by tests to assert primitive/CSG occupancy without GL.
bool field_is_solid(const BuildBuffer& buf, const Vector3& worldPoint);

} // namespace dsl
