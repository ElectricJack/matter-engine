# Surface Scratch Context Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move surface.c's reusable mesh-build state (memory pool + spatial hash) into a caller-owned `SurfaceScratch` context, share one hash per cell across all consumers, and replace the O(triangles × particles) per-triangle material/tint scan with O(triangles) hash queries — keeping mesh geometry byte-identical and unblocking parallel per-cell meshing.

**Architecture:** A single opaque `SurfaceScratch` owns the (formerly file-global) `MemoryPool` and a reused `SpatialHash`. New `*WithScratch` entry points take the context; legacy `GenerateMesh`/`ComputeSurfaceNormals` delegate to a lazily-created default scratch so existing callers are unaffected. cell.cpp owns one scratch and reuses its hash for per-triangle nearest-particle lookups.

**Tech Stack:** C (surface.c/spatial_hash.c), C++17 (cell.cpp/cluster.cpp), raylib PODs, headless test suites via `MatterSurfaceLib/tests/Makefile`.

**Spec:** `docs/superpowers/specs/2026-06-15-surface-scratch-context-design.md`

---

## Conventions / environment notes

- **Full build:** `cd MatterSurfaceLib && WSL_LINUX=1 make` (rebuilds raylib; produces `./matter_surface_lib`).
- **Headless suites** (run from `MatterSurfaceLib/tests/`): `make run-cont`, `make run-cell`,
  `make run-tint`, `make run-simp`, `make run-cull`. The continuity (`cont`), cell (`cell`), and tint
  (`tint`) suites are the load-bearing regression gates for this change.
- **WSL/DrvFs caveat:** freshly-linked binaries on the `/mnt/d` DrvFs mount can be silently corrupted
  (all-zero output). If a freshly-built test binary fails with "Exec format error"/"Permission
  denied", copy the sources+objects to `/tmp` and build/run there. The test Makefile links
  `../../Libraries/raylib/src/libraylib.a`, which must be the **Linux ELF** archive — a prior
  `WSL_LINUX=1 make` at repo root refreshes it; verify with `file` if linking fails with PE/`__imp_`
  errors.
- **Byte-identical bar:** "identical geometry" = the `cont` and `cell` suites report the same
  triangle/vertex counts and pass all continuity assertions as before the change. Capture a baseline
  *before* Task 1 (Task 0).

---

## Task 0: Capture the regression baseline

**Files:** none (measurement only)

- [ ] **Step 1: Build and run all relevant headless suites on the current (pre-change) tree**

Run:
```bash
cd "MatterSurfaceLib/tests"
for t in cont cell tint simp cull; do echo "=== $t ==="; make run-$t 2>&1 | tail -25; done
```
Expected: all suites print PASS / their success summaries. Save the full output to a scratch file
(e.g. `/tmp/scratch_baseline.txt`) so later tasks can diff against it.

- [ ] **Step 2: Record the continuity harness's reported triangle/vertex counts**

These numbers are the byte-identical reference. Note them in the task tracker. Do **not** commit
anything in this task.

---

## Task 1: Move `MemoryPool` into a `SurfaceScratch`; route legacy API through a default scratch

This task introduces the context and relocates the pool **without** changing the hash (still per-call)
— a pure refactor that must leave output identical.

**Files:**
- Modify: `MatterSurfaceLib/include/surface.h`
- Modify: `MatterSurfaceLib/src/surface.c`

- [ ] **Step 1: Declare the context type and lifecycle in `surface.h`**

Inside the `extern "C"` block (after the `MeshGenerationConfig` typedef / before `GenerateMesh`):
```c
// Opaque per-thread scratch context owning all reusable mesh-build buffers (the
// scalar/mesh/edge memory pool and the particle spatial hash). One per thread.
typedef struct SurfaceScratch SurfaceScratch;
SurfaceScratch* CreateSurfaceScratch(void);
void            DestroySurfaceScratch(SurfaceScratch* scratch);
```

- [ ] **Step 2: Define the struct and relocate the pool in `surface.c`**

Replace the file-global `static MemoryPool g_memoryPool = {0};` (surface.c:95) with the context
struct plus a default scratch pointer:
```c
struct SurfaceScratch {
    MemoryPool   pool;
    SpatialHash* hash;          // reused across cells; NULL until first use (Task 2)
    Particle*    hashParticles; // identity guard for hash reuse (Task 2)
    int          hashCount;     // (Task 2)
    float        hashCellSize;  // cellSize the hash was created with (Task 2)
};

// Legacy single-threaded API delegates here. Lazily created; freed by SurfaceLibCleanup.
static SurfaceScratch* g_defaultScratch = NULL;
```

- [ ] **Step 3: Make the pool helpers operate on a `MemoryPool*`**

Change `EnsureFieldCapacity`, `EnsureMeshCapacity`, `EnsureHashTableCapacity`, and `CleanupMemoryPool`
(surface.c:98-151) to take a `MemoryPool* pool` parameter and reference `pool->...` instead of
`g_memoryPool....`. Signatures become e.g. `static void EnsureFieldCapacity(MemoryPool* pool, size_t requiredCells)`.

- [ ] **Step 4: Add the lifecycle functions**

```c
SurfaceScratch* CreateSurfaceScratch(void) {
    SurfaceScratch* s = (SurfaceScratch*)calloc(1, sizeof(SurfaceScratch));
    return s; // pool zero-initialized; hash created lazily
}

void DestroySurfaceScratch(SurfaceScratch* s) {
    if (!s) return;
    CleanupMemoryPool(&s->pool);
    if (s->hash) sh_destroy(s->hash);
    free(s);
}

static SurfaceScratch* DefaultScratch(void) {
    if (!g_defaultScratch) g_defaultScratch = CreateSurfaceScratch();
    return g_defaultScratch;
}
```

- [ ] **Step 5: Thread the scratch through `GenerateMeshInternal`**

Add a leading `SurfaceScratch* scratch` parameter to `GenerateMeshInternal` (surface.c:405). Replace
every `g_memoryPool.` reference in its body (the `config.enableMemoryReuse` branches at surface.c:430-545
and any frees) with `scratch->pool.`, and pass `&scratch->pool` to the `Ensure*Capacity` helpers.
Leave the per-call `sh_create`/`sh_destroy` of the spatial hash exactly as-is for this task.

- [ ] **Step 6: Update the public wrappers to pass the default scratch**

`GenerateMesh` (surface.c:199) and `GenerateMeshWithConfig` (surface.c:205) call
`GenerateMeshInternal(DefaultScratch(), ...)`. `SurfaceLibCleanup` (surface.c:210) becomes:
```c
void SurfaceLibCleanup(void) {
    if (g_defaultScratch) { DestroySurfaceScratch(g_defaultScratch); g_defaultScratch = NULL; }
}
```
(`ComputeSurfaceNormals` still builds its own hash; untouched this task.)

- [ ] **Step 7: Build the full program**

Run: `cd MatterSurfaceLib && WSL_LINUX=1 make 2>&1 | tail -5`
Expected: `Built executable for linux` with no errors/warnings about `g_memoryPool`.

- [ ] **Step 8: Run the regression suites; confirm identical output**

Run:
```bash
cd MatterSurfaceLib/tests
for t in cont cell tint simp cull; do echo "=== $t ==="; make run-$t 2>&1 | tail -25; done
```
Expected: all PASS; continuity triangle/vertex counts identical to Task 0's baseline.

- [ ] **Step 9: Commit**

```bash
git add MatterSurfaceLib/include/surface.h MatterSurfaceLib/src/surface.c
git commit -m "refactor: move surface memory pool into caller-owned SurfaceScratch context"
```

---

## Task 2: Move the spatial hash into the scratch and share it across the mesh path

Now share one hash within `GenerateMeshInternal` and its internal `ComputeSurfaceNormals`, and add the
public scratch-aware API.

**Files:**
- Modify: `MatterSurfaceLib/include/surface.h`
- Modify: `MatterSurfaceLib/src/surface.c`

- [ ] **Step 1: Add a hash-ensure helper in `surface.c`**

Place above `GenerateMeshInternal`:
```c
// Ensure scratch->hash holds exactly `particles` at `cellSize`, reusing storage
// when possible. Recreates only when cellSize changes (the AABB query is correct
// at any cellSize, but matching it to the search radius keeps queries cheap).
static SpatialHash* scratch_ensure_hash(SurfaceScratch* s, Particle* particles,
                                        int count, float cellSize) {
    if (!s->hash || s->hashCellSize != cellSize) {
        if (s->hash) sh_destroy(s->hash);
        s->hash = sh_create(cellSize, count > 0 ? count : 1);
        s->hashCellSize = cellSize;
    } else {
        sh_clear(s->hash);
    }
    for (int i = 0; i < count; i++)
        sh_insert(s->hash, particles[i].position.x, particles[i].position.y,
                  particles[i].position.z, &particles[i]);
    s->hashParticles = particles;
    s->hashCount = count;
    return s->hash;
}
```

- [ ] **Step 2: Extract the normals body into a scratch+hash impl**

Refactor `ComputeSurfaceNormals` (surface.c:214) so its body lives in:
```c
static void compute_surface_normals_impl(SpatialHash* hash, Mesh* mesh, Particle* particles,
        float particleRadius, int particleCount, float blendWidth,
        Particle* clipParticles, int clipCount,
        Particle* carveParticles, int carveCount, float carveBlend);
```
The impl uses the **passed** `hash` instead of calling `sh_create`/`sh_insert`/`sh_destroy`. Move the
`gradSearch`/query loop verbatim, swapping the local hash for the parameter. Do **not** destroy the
hash in the impl (the scratch owns it).

- [ ] **Step 3: Point `GenerateMeshInternal` at the shared hash**

In `GenerateMeshInternal`, replace the per-call `sh_create`/insert loop (surface.c ~450-460) with:
```c
float spatialCellSize = particleRadius * 2.5f + blendWidth * 4.0f;
SpatialHash* spatialHash = scratch_ensure_hash(scratch, particles, particleCount, spatialCellSize);
```
Delete the matching `sh_destroy(spatialHash);` near the end (surface.c:883) — the scratch owns it.
Change the internal normals call (surface.c:856) to
`compute_surface_normals_impl(spatialHash, &mesh, particles, particleRadius, particleCount, blendWidth, clipParticles, clipCount, carveParticles, carveCount, carveBlend);`

- [ ] **Step 4: Add the public scratch-aware API in `surface.h`**

```c
Mesh GenerateMeshWithScratch(SurfaceScratch* scratch, Particle* particles, float particleRadius,
                             int particleCount, Bounds volume, float blendWidth,
                             Particle* clipParticles, int clipCount,
                             Particle* carveParticles, int carveCount, float carveBlend);
void ComputeSurfaceNormalsWithScratch(SurfaceScratch* scratch, Mesh* mesh, Particle* particles,
                             float particleRadius, int particleCount, float blendWidth,
                             Particle* clipParticles, int clipCount,
                             Particle* carveParticles, int carveCount, float carveBlend);
SpatialHash* SurfaceScratchHash(SurfaceScratch* scratch);
```

- [ ] **Step 5: Implement the public scratch-aware API in `surface.c`**

```c
Mesh GenerateMeshWithScratch(SurfaceScratch* scratch, Particle* particles, float particleRadius,
        int particleCount, Bounds volume, float blendWidth, Particle* clipParticles, int clipCount,
        Particle* carveParticles, int carveCount, float carveBlend) {
    return GenerateMeshInternal(scratch, particles, particleRadius, particleCount, volume, blendWidth,
        GetDefaultMeshConfig(), clipParticles, clipCount, carveParticles, carveCount, carveBlend);
}

void ComputeSurfaceNormalsWithScratch(SurfaceScratch* scratch, Mesh* mesh, Particle* particles,
        float particleRadius, int particleCount, float blendWidth, Particle* clipParticles,
        int clipCount, Particle* carveParticles, int carveCount, float carveBlend) {
    if (!mesh || !mesh->vertices || !mesh->normals || mesh->vertexCount <= 0 ||
        !particles || particleCount <= 0) return;
    float cellSize = particleRadius * 2.5f + blendWidth * 4.0f;
    SpatialHash* hash;
    if (scratch->hash && scratch->hashParticles == particles && scratch->hashCount == particleCount
        && scratch->hashCellSize == cellSize) {
        hash = scratch->hash;                 // reuse the hash GenerateMesh just built
    } else {
        hash = scratch_ensure_hash(scratch, particles, particleCount, cellSize);
    }
    compute_surface_normals_impl(hash, mesh, particles, particleRadius, particleCount, blendWidth,
        clipParticles, clipCount, carveParticles, carveCount, carveBlend);
}

SpatialHash* SurfaceScratchHash(SurfaceScratch* scratch) { return scratch ? scratch->hash : NULL; }
```

- [ ] **Step 6: Make legacy `ComputeSurfaceNormals` delegate**

Rewrite the public `ComputeSurfaceNormals` (surface.c:214) body to a single call:
`ComputeSurfaceNormalsWithScratch(DefaultScratch(), mesh, particles, particleRadius, particleCount, blendWidth, clipParticles, clipCount, carveParticles, carveCount, carveBlend);`

- [ ] **Step 7: Build the full program**

Run: `cd MatterSurfaceLib && WSL_LINUX=1 make 2>&1 | tail -5`
Expected: builds clean.

- [ ] **Step 8: Run the regression suites; confirm identical output**

Run:
```bash
cd MatterSurfaceLib/tests
for t in cont cell tint simp cull; do echo "=== $t ==="; make run-$t 2>&1 | tail -25; done
```
Expected: all PASS; continuity counts identical to baseline (geometry unchanged — the hash holds the
same particles, only its construction is shared/reused).

- [ ] **Step 9: Commit**

```bash
git add MatterSurfaceLib/include/surface.h MatterSurfaceLib/src/surface.c
git commit -m "perf: share one spatial hash per cell across mesh + normals via SurfaceScratch"
```

---

## Task 3: cell.cpp uses a shared scratch + hash; replace the per-triangle brute-force scan (#4)

**Files:**
- Modify: `MatterSurfaceLib/include/cluster.h` (own a `SurfaceScratch*`)
- Modify: `MatterSurfaceLib/src/cluster.cpp` (create/destroy + pass it down)
- Modify: `MatterSurfaceLib/include/cell.h` (thread the scratch into `generate_mesh_for_group`/`rebuild_meshes`)
- Modify: `MatterSurfaceLib/src/cell.cpp` (use scratch API + hash query)

- [ ] **Step 1: Give `Cluster` a scratch and pass it to cells**

In `cluster.h` add a member `SurfaceScratch* surface_scratch_ = nullptr;` (forward-declare
`struct SurfaceScratch;`). In `cluster.cpp`, create it where the cluster initializes
(`surface_scratch_ = CreateSurfaceScratch();`) and `DestroySurfaceScratch(surface_scratch_);` in the
destructor. Pass `surface_scratch_` through the rebuild call chain that reaches
`Cell::generate_mesh_for_group` (`update_cell_meshes` → `cell->rebuild_meshes` →
`generate_mesh_for_group`). Add a `SurfaceScratch* scratch` parameter to those `Cell` methods in
`cell.h`/`cell.cpp`.

- [ ] **Step 2: Use the scratch-aware surface calls in `generate_mesh_for_group`**

In `cell.cpp:395`, change `GenerateMesh(...)` → `GenerateMeshWithScratch(scratch, particles.data(), max_radius, ...)`
(same trailing args). In `cell.cpp:414`, change the post-simplify `ComputeSurfaceNormals(...)` →
`ComputeSurfaceNormalsWithScratch(scratch, &simplified, particles.data(), max_radius, ...)`.

- [ ] **Step 3: Replace the brute-force nearest loop with a hash query**

Replace the per-triangle nearest-particle scan (cell.cpp:446-454) with a query against the hash the
`GenerateMeshWithScratch` call just populated (it still holds `particles`):
```c
SpatialHash* tri_hash = SurfaceScratchHash(scratch);
float tri_search = max_radius * 2.5f + blend_width * 4.0f;
// ... inside the per-triangle loop, replacing the bestIdx/bestD scan:
int bestIdx = 0;                          // default seed (a real particle's material)
Particle* nearest = NULL;
int nfound = tri_hash
    ? sh_query_radius_nearest(tri_hash, c.x, c.y, c.z, tri_search, (void**)&nearest, 1)
    : 0;
if (nfound > 0 && nearest) {
    bestIdx = (int)(nearest - particles.data());
}
triangle_normals[t].materialId = particles[bestIdx].materialId;
triangle_normals[t].tint = particle_tints[bestIdx];
```
Ensure `sh_query_radius_nearest` is declared (include `spatial_hash.h` in cell.cpp if not already).

- [ ] **Step 4: Build the full program**

Run: `cd MatterSurfaceLib && WSL_LINUX=1 make 2>&1 | tail -5`
Expected: builds clean.

- [ ] **Step 5: Run the regression suites**

Run:
```bash
cd MatterSurfaceLib/tests
for t in cont cell tint simp cull; do echo "=== $t ==="; make run-$t 2>&1 | tail -25; done
```
Expected: `cont`/`cell`/`simp`/`cull` identical to baseline. `tint` passes; if it asserts on exact
per-triangle tint values that differ only by tie-breaking on the symmetric scene, confirm the diff is
limited to ties (accepted per spec) — otherwise investigate.

- [ ] **Step 6: Manual GUI smoke test**

Build is at `./matter_surface_lib`. Ask the user to launch it (harness reaps backgrounded GUI apps —
they run it via `!` or their own terminal). Confirm: lattice scene renders the same surface, carve /
lumpiness regeneration works and is faster, no crash on repeated rebuilds.

- [ ] **Step 7: Commit**

```bash
git add MatterSurfaceLib/include/cluster.h MatterSurfaceLib/src/cluster.cpp \
        MatterSurfaceLib/include/cell.h MatterSurfaceLib/src/cell.cpp
git commit -m "perf: reuse per-cell hash for triangle material/tint; share cluster SurfaceScratch"
```

---

## Self-review (run before handing off)

- **Spec coverage:** Task 1 = pool→scratch + legacy delegation; Task 2 = hash sharing + scratch API +
  `SurfaceScratchHash`; Task 3 = cell.cpp integration + #4. All spec goals covered.
- **Type consistency:** `SurfaceScratch`, `CreateSurfaceScratch`/`DestroySurfaceScratch`,
  `GenerateMeshWithScratch`, `ComputeSurfaceNormalsWithScratch`, `SurfaceScratchHash`,
  `scratch_ensure_hash`, `compute_surface_normals_impl` are used consistently across tasks.
- **No placeholders:** every step has concrete code or an exact command + expected result.
- **Open risk to watch:** the `tint` suite is the one place an accepted tie-break deviation could trip
  an assertion; Step 3.5 calls this out explicitly.
```
