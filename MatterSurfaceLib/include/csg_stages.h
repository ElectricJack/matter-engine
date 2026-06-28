#ifndef CSG_STAGES_H
#define CSG_STAGES_H

// Ordered-CSG stage description for the field evaluator (Phase 1).
//
// Today's pipeline sorted ops into two buckets (additive union, then carve) which
// discards interleaving, so add->subtract->add wrongly subtracts from the later
// add. This describes an ORDERED list of stages derived from the authored op
// order. Per sample the field eval folds stages in order:
//   field = +inf
//   for each stage: d = stage SDF(p)   (smin over the stage's spheres + fat prims)
//     Union:        field = smin(field, d)
//     Difference:   field = smax(field, -d)
//     Intersection: field = smax(field, d)
//
// The sphere hot path stays byte-identical: when there is a SINGLE additive stage
// (and optional legacy carve), the eval takes the original union-then-subtract
// branch and never touches the stage machinery. The stage path activates only when
// 2+ ordered stages exist (interleaved add/subtract/add).

#include "raylib.h"

#ifdef __cplusplus
extern "C" {
#endif

// CSG op per stage; matches dsl::CsgOp ordering (Union=0, Difference=1, Intersection=2).
typedef enum {
    CSG_STAGE_UNION        = 0,
    CSG_STAGE_DIFFERENCE   = 1,
    CSG_STAGE_INTERSECTION = 2
} CsgStageOp;

// Ordered stage list bound to the hashed sphere stream. `particleStage[i]` gives
// the stage index of hashed particle i (parallel to the Particle* array); fat
// primitives carry their own `stage`. `stageOp[k]` is stage k's CSG op.
//
// A NULL FieldStages* (or stageCount<=1) means "single union stage": the eval runs
// the legacy byte-identical union-then-carve path.
typedef struct {
    const CsgStageOp* stageOp;        // [stageCount]
    int               stageCount;
    const int*        particleStage;  // [particleCount], stage index per hashed sphere

    // The contiguous hashed-particle array base + count. The spatial hash stores
    // pointers into this array; the field eval recovers a found particle's index
    // (and therefore its stage) by pointer subtraction against this base. Set both
    // to the same Particle* / count passed to the mesher.
    const void* _particleBase;
    long        _particleCount;
} FieldStages;

#ifdef __cplusplus
}
#endif

#endif // CSG_STAGES_H
