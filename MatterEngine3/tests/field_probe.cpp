// Test/QA glue: drive the production surface.c field eval (ProbeFieldScalar)
// from a dsl::LoweredField, so iso_primitive_tests can compare meshed occupancy
// against the analytic field_is_solid oracle.

#include "csg_lowering.h"
extern "C" {
#include "surface.h"
}
#include <vector>

// Defined in iso_primitive_tests.cpp's contract.
bool mesh_field_is_solid(const dsl::LoweredField& f, Vector3 p);

bool mesh_field_is_solid(const dsl::LoweredField& f, Vector3 p) {
    // The staged eval consumes the UNIFIED sphere stream (every brush, additive +
    // difference + intersection), tagged by stage. Difference spheres are folded as
    // Difference stages, NOT trailing carve, so we pass no carve here.
    std::vector<Particle> particles = f.staged_spheres;
    float maxR = 0.0f;
    for (const Particle& q : particles)
        if (q.radius > maxR) maxR = q.radius;
    for (const FatPrim& fp : f.fat)
        if (fp.boundRadius > maxR) maxR = fp.boundRadius;
    if (maxR <= 0.0f) maxR = 1.0f;

    // Ordered CSG stage list. stageOp parallels f.stages; staged_stage parallels
    // the unified particle array. _particleBase/_count let the eval recover the
    // stage of a hash-found particle by pointer arithmetic.
    std::vector<CsgStageOp> stageOps = f.stages;
    std::vector<int> partStage = f.staged_stage;

    FieldStages stages;
    stages.stageOp = stageOps.empty() ? nullptr : stageOps.data();
    stages.stageCount = (int)stageOps.size();
    stages.particleStage = partStage.empty() ? nullptr : partStage.data();
    stages._particleBase = particles.empty() ? nullptr : particles.data();
    stages._particleCount = (long)particles.size();

    // Hard union (k=0) so the surface is the exact SDF zero-set: the comparison
    // against the analytic oracle is crisp.
    float blendWidth = 0.0f;

    SurfaceScratch* scratch = CreateSurfaceScratch();
    float v = ProbeFieldScalar(scratch, particles.empty() ? nullptr : particles.data(), maxR,
                               (int)particles.size(), blendWidth,
                               &stages, f.fat.empty() ? nullptr : f.fat.data(), (int)f.fat.size(),
                               /*carve*/nullptr, 0, 0.0f, p);
    DestroySurfaceScratch(scratch);
    return v < 0.0f;
}
