# Surface Scratch Context Design

**Date:** 2026-06-15
**Status:** Approved, ready for planning

## Problem

Profiling the mesh-regeneration path (after the spatial-hash query fixes landed) surfaced two
remaining single-threaded inefficiencies in the per-cell mesh build (`Cell::generate_mesh_for_group`
in `MatterSurfaceLib/src/cell.cpp`):

1. **Redundant per-cell allocations (#3).** Within a single cell rebuild the spatial hash over the
   *identical* particle set is constructed 2–3 times: once inside `GenerateMesh`, again inside
   `ComputeSurfaceNormals` (surface.c, the internal call at the end of `GenerateMeshInternal`), and a
   third time for simplified cells (the post-`simplify_mesh` `ComputeSurfaceNormals` call in cell.cpp).
   Each build is a fresh `sh_create` (1024-bucket `calloc` + per-particle entry allocations). On a full
   rebuild of hundreds of cell/groups this is a large volume of small allocations.

2. **Brute-force per-triangle material/tint lookup (#4).** After meshing, cell.cpp tags each triangle
   with the material + tint of the nearest particle to its centroid via an O(triangles × particles)
   linear scan — re-deriving the slow way what a spatial-hash query gives in ~O(1). The `particles`
   set here is per-cell/per-group (a few hundred), not global, but the scan is still pure redundancy:
   `GenerateMesh` already built the perfect structure (a hash of exactly these particles) and threw it
   away.

Additionally, surface.c keeps its reusable buffers in a **file-global** `g_memoryPool`. This is the
real thread-safety blocker for the planned parallelization of per-cell meshing: two threads meshing
two cells would race on the shared pool. Since parallelization is the next step, the cleanest fix is
to thread the reusable state (pool + hash) through a caller-owned context now, so parallelization
becomes "one context per worker thread" with no further API churn.

## Goals

- Build the per-cell spatial hash **once** per cell and share it across all consumers
  (`GenerateMesh`, the internal `ComputeSurfaceNormals`, the post-simplify `ComputeSurfaceNormals`,
  and the per-triangle nearest lookup).
- Reuse the hash storage and the field/mesh/edge buffers **across cells** (clear + refill, not
  re-allocate), collapsing many small allocations into a few large persistent ones.
- Replace the O(triangles × particles) per-triangle material/tint scan with O(triangles) hash queries.
- Move all reusable state off the file-global into a caller-owned `SurfaceScratch`, making the mesh
  path re-entrant: one scratch per thread for the upcoming parallelization, with no further API
  changes required.
- Keep mesh **geometry** byte-identical (verified by the four headless suites). The only accepted
  behavioral deviation is per-triangle tint selection on exact distance ties (see Non-Goals).

## Non-Goals

- Parallelization itself (this change only *enables* it).
- Changing the marching-cubes / SDF math, the carve/clip fields, or simplification.
- Eliminating the per-cell `UploadMesh` / BLAS allocations — those are required by the
  per-cell-BLAS TLAS design and are out of scope.
- Byte-identical *tint*: on exact squared-distance ties (likely on the symmetric lattice scene),
  hash-nearest may select a different particle than the index-ordered brute-force scan, so a small
  number of triangle tints may change. This is accepted (user-confirmed "don't worry about the slight
  change"). Material IDs come from the same nearest-particle resolution, so they shift only on the
  same ties. No geometry impact.

## Design

### The context

A single opaque, caller-owned struct in surface.c owns all reusable mesh-build state:

```c
// surface.h
typedef struct SurfaceScratch SurfaceScratch;
SurfaceScratch* CreateSurfaceScratch(void);
void            DestroySurfaceScratch(SurfaceScratch* scratch);
```

```c
// surface.c (internal layout)
struct SurfaceScratch {
    MemoryPool   pool;          // the fields currently in g_memoryPool, moved here verbatim
    SpatialHash* hash;          // reused across cells; NULL until first use
    Particle*    hashParticles; // identity guard: which particle set the hash currently holds
    int          hashCount;     // count for that set
    float        hashCellSize;  // cellSize the hash was created with
};
```

`MemoryPool` keeps its existing shape and its `Ensure*Capacity` grow logic; those helpers take a
`MemoryPool*` (or `SurfaceScratch*`) instead of touching the global.

### The scratch-aware API

```c
// surface.h
Mesh GenerateMeshWithScratch(SurfaceScratch* scratch,
                             Particle* particles, float particleRadius, int particleCount,
                             Bounds volume, float blendWidth,
                             Particle* clipParticles, int clipCount,
                             Particle* carveParticles, int carveCount, float carveBlend);

void ComputeSurfaceNormalsWithScratch(SurfaceScratch* scratch,
                                      Mesh* mesh, Particle* particles, float particleRadius,
                                      int particleCount, float blendWidth,
                                      Particle* clipParticles, int clipCount,
                                      Particle* carveParticles, int carveCount, float carveBlend);

// Lets cell.cpp reuse the just-built hash for the per-triangle nearest lookup.
SpatialHash* SurfaceScratchHash(SurfaceScratch* scratch);
```

### Hash-sharing semantics

- A helper `static SpatialHash* scratch_ensure_hash(SurfaceScratch* s, Particle* particles, int count, float cellSize)`:
  - If `s->hash` is NULL or was created with a different `cellSize`, destroy/create it at the new
    cellSize. (cellSize varies per cell as `particleRadius*2.5 + blendWidth*4`; the AABB query is
    correct for any cellSize, so a mismatched-but-reused cellSize would only cost performance — we
    recreate to keep it optimal.)
  - Otherwise `sh_clear(s->hash)` to reuse storage.
  - Insert all `particles`, then record `hashParticles=particles`, `hashCount=count`,
    `hashCellSize=cellSize`.
- `GenerateMeshWithScratch` **always** calls `scratch_ensure_hash` for its `particles` at the start,
  then runs the field eval and the internal normal pass against `s->hash` (no further rebuilds).
- `ComputeSurfaceNormalsWithScratch` reuses `s->hash` **iff** `particles == s->hashParticles &&
  count == s->hashCount`; otherwise it calls `scratch_ensure_hash` to (re)build. Within one cell the
  post-simplify call matches (same `particles` vector), so it reuses.
- Identity guard rationale: within one cell rebuild `particles` is a stable `std::vector`, so
  pointer+count is a reliable identity. Across cells the vector is rebuilt, but every cell's first
  call is `GenerateMeshWithScratch`, which unconditionally refreshes the hash — so a recycled heap
  address can never cause a stale reuse.

### Internal refactor

- `GenerateMeshInternal` gains a `SurfaceScratch*` parameter; all `g_memoryPool` references become
  `scratch->pool`; the per-call `sh_create`/`sh_destroy` becomes `scratch_ensure_hash` + (no destroy;
  the scratch owns the hash for its lifetime).
- The internal `ComputeSurfaceNormals` call at the end of `GenerateMeshInternal` becomes the
  scratch-aware path reusing `scratch->hash`.
- `ComputeSurfaceNormals` body moves into a static `compute_surface_normals_impl(SurfaceScratch*, ...)`
  that uses the passed hash instead of creating its own.

### Backward compatibility

- The existing `GenerateMesh`, `GenerateMeshWithConfig`, and `ComputeSurfaceNormals` keep their exact
  signatures and behavior by delegating to a lazily-created `static SurfaceScratch* g_defaultScratch`.
  This is the *only* remaining global, used solely by the legacy single-threaded API; the parallel
  path uses caller-owned scratches.
- `SurfaceLibCleanup` destroys `g_defaultScratch` (and its owned hash + pool).
- All current callers (other sub-projects, headless tests) compile and behave identically with no
  changes.

### cell.cpp integration

- `Cluster` (or the per-rebuild driver) owns one `SurfaceScratch*` for the single-threaded path
  (created once, destroyed on teardown), passed into `generate_mesh_for_group`.
- `generate_mesh_for_group`:
  - Calls `GenerateMeshWithScratch(scratch, ...)` and `ComputeSurfaceNormalsWithScratch(scratch, ...)`
    (post-simplify) instead of the global-pool versions.
  - Replaces the per-triangle brute-force nearest loop with
    `sh_query_radius_nearest(SurfaceScratchHash(scratch), cx, cy, cz, searchRadius, (void**)&nearest, 1)`,
    mapping the returned `Particle*` back to an index via `nearest - particles.data()` to fetch
    `materialId` and `particle_tints`. If `found == 0`, keep the existing default seed.

## Testing

- All four headless suites (`cont`, `cell`, `cull`, `simp`) must pass and produce **identical mesh
  geometry** (triangle/vertex counts and positions) to the pre-change baseline.
- A focused check that `GenerateMeshWithScratch` reused across many cells produces the same per-cell
  meshes as `GenerateMesh` (the legacy path) — i.e. the scratch reuse doesn't leak state between
  cells. This can piggyback on the continuity harness.
- Manual: run the full GUI program, exercise carve/lumpiness regeneration, confirm the surface looks
  the same (modulo accepted tint ties) and rebuilds are faster.

## Risks

- **State leakage between cells via the reused pool/hash.** Mitigated by `scratch_ensure_hash` always
  clearing+refilling and `GenerateMeshWithScratch` always refreshing the hash; covered by the
  continuity test.
- **cellSize churn defeating hash reuse.** If every cell has a distinct cellSize, the hash is
  recreated each cell (back to per-cell `calloc`). Acceptable: the AABB query is cellSize-correct, so
  if profiling shows churn we can later fix a single cellSize and reuse storage unconditionally. Out
  of scope for this change.
