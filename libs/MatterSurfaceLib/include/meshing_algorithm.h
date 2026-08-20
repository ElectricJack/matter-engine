#ifndef MESHING_ALGORITHM_H
#define MESHING_ALGORITHM_H

// libs/MatterSurfaceLib/include/meshing_algorithm.h
//
// The strategy interface that turns one merge group's particles into
// triangles, plus the context struct carrying everything a mesher might need.
//
// Where it sits: MatterSurfaceLib. `Cell::build_group_mesh` resolves a merge
// group's particle subset and meshing parameters, fills a `MeshContext`, looks
// the implementation up with `GetMeshingAlgorithm` and calls `generate`. The
// two implementations live in `src/marching_cubes_algorithm.cpp` (the smooth
// isosurface path) and `src/oriented_cube_algorithm.cpp` (the blocky voxel
// path); which one runs is authored per material via
// `MaterialDef.meshingAlgorithm`.
//
// Threading and lifetime: `generate` runs on `MeshWorkerPool` worker threads,
// so implementations must be GL-free, must hold no mutable state of their own
// (they are shared `const` singletons), and must confine scratch memory to
// the `SurfaceScratch*` handed to them in the context. Every pointer and
// reference in `MeshContext` borrows storage owned by the caller and is valid
// only for the duration of the `generate` call — nothing may be captured past
// return.
//
// Adding an algorithm: implement `MeshingAlgorithm`, add a `MeshAlgorithm`
// value, and wire it into `GetMeshingAlgorithm`'s switch. `MeshContext` is
// deliberately a superset of what any single algorithm needs; ignore the
// fields that do not apply, as the existing two do.

#include "surface.h"            // Particle, Bounds, SurfaceScratch
#include "tri.h"                // float4 (via precomp.h)
#include "mesh_simplifier.hpp"  // CellBounds
#include "mesh_worker_pool.h"   // GroupMeshResult
#include <vector>
#include <cstdint>

// Which mesher turns a merge group's particles into geometry. Stored on the
// material (MaterialDef.meshingAlgorithm) and resolved per merge group.
enum class MeshAlgorithm { MarchingCubes = 0, OrientedCubes = 1 };

// Everything an algorithm might need to mesh one merge group. Built by
// Cell::build_group_mesh after it resolves the group's particle subset and
// meshing parameters. References/pointers borrow data owned by the caller and
// are valid only for the duration of the generate() call. Algorithms ignore the
// fields they do not use (e.g. OrientedCubes ignores blend/clip/carve/scratch).
struct MeshContext {
    const std::vector<Particle>& particles;       // resolved group particles (post cull/vis-clamp)
    const std::vector<float4>&   particle_tints;  // parallel to particles
    float  max_radius;                            // max effective radius in the set

    Bounds     bounds;        // center, size, divisionPow
    CellBounds cell_bounds;   // min/max bound for boundary locking
    float      voxel;         // actual_size / (gridSize - 1)

    // Isosurface params (marching cubes uses; cubes ignore)
    float blend_width;
    const Particle* clip;   int clip_count;
    const Particle* carve;  int carve_count;
    float carve_blend;
    float simplification_ratio;

    SurfaceScratch* scratch;  // per-worker scratch (MC uses spatial hash; cubes ignore)

    uint32_t group_id;

    // --- Typed iso-primitives + ordered CSG (Phase 1) ---------------------
    // Borrowed, NOT in the spatial hash. When `stages` is non-NULL with >1
    // stage (or a Difference stage), or `fat_count` > 0, the marching-cubes
    // mesher runs the ordered staged field eval (GenerateMeshStaged) instead of
    // the legacy single-union path. Left zero/NULL by every legacy caller, so
    // the hot path is byte-identical.
    const FieldStages* stages = nullptr;        // ordered CSG stage list (+ particle->stage map)
    const FatPrim*     fat = nullptr;           // non-sphere iso-primitives (oriented box)
    int                fat_count = 0;
};

// Abstract mesher. Implementations must be GL-free (CPU only) and reentrant on
// the supplied scratch, so they run on worker threads.
class MeshingAlgorithm {
public:
    virtual ~MeshingAlgorithm() = default;
    virtual GroupMeshResult generate(const MeshContext& ctx) const = 0;
};

// Returns the process-wide singleton for an algorithm. Defined in
// meshing_algorithm.cpp (Task 4).
//
// The referent is a `const` function-local static, so the reference stays
// valid for the process lifetime and is safe to hold and to call from worker
// threads. An unrecognized `algo` falls back to marching cubes rather than
// failing.
const MeshingAlgorithm& GetMeshingAlgorithm(MeshAlgorithm algo);

#endif // MESHING_ALGORITHM_H
