# Sector-LOD Instanced World — Design

**Status:** Approved for planning (2026-06-24)
**Context:** Supersedes the imposter direction as the *primary* strategy for rendering
massive scenes. The voxel-box imposter
(`2026-06-22-voxel-box-imposter-design.md`) is **not** discarded — it is demoted to one
possible implementation of the far-distance "merged proxy" slot described below, not the
mechanism for near/mid geometry.

## Goal

Render outdoor scenes with **billions of instanced triangles** at interactive rates by
exploiting the fact that the world is built from a **small library of unique, high-detail
parts** instanced many times. Keep a **single render pipeline** (raytraced triangle BLASes
— no parallel imposter shader path for the common case) and make the raytracer fast enough
by bounding the one quantity that actually scales with world size: the **instance count in
the per-frame TLAS**.

## The key insight

Two facts from the current codebase drive the entire design:

1. **The TLAS has one entry per instance, not per triangle.** `TLAS::Build`
   (`src/bvh.cpp:496`) iterates over `blasCount` (instances); each leaf stores
   `node.BLAS = instance index` (`bvh.cpp:543`). Triangles live entirely inside each
   BLAS's own BVH. A 100k-triangle part and a 10-triangle part are both a single TLAS leaf.
2. **Therefore triangle count is essentially free; instance count is the budget.**
   - Unique geometry: 100–1000 parts × ~100k tris = 10M–100M triangles total ≈ 0.5–7 GB of
     BLAS data. Static, built once, fits in VRAM.
   - "Billions of triangles" = billions of *transforms* pointing at those shared BLASes. A
     billion instances at ~150+ bytes each (`BVHInstance` = transform + invTransform +
     world AABB + id) is ~150+ GB — so the full instance set can **never** live in the TLAS
     at once.

The whole architecture is machinery to keep the **active (resident-in-TLAS) instance
count** small enough to build and trace each frame, regardless of how large the world is.

## Target scene model

- 100–1000 unique high-resolution parts produced by the existing procedural part system.
- Each part ~100k triangles in its BLAS.
- The world is assembled by instancing those parts (potentially billions of instances).
- Geometry is **mostly static**; the **camera moves** and drives all LOD/culling churn.

## Tech Stack

C++17, raylib/rlgl + OpenGL 3.3, GLSL fragment-shader ray tracer (TLAS/BLAS in
`bvh_tlas_common.glsl`). Existing `BLASManager`, `TLASManager`, and `mesh_simplifier`.
GL-free CPU build steps unit-tested under `MatterEngine3/tests/`.

---

## Architecture

Four layers:

### 1. Part LOD library (static, built once)

- Each of the 100–1000 unique parts is decimated into a small set of LOD meshes (target:
  ~3 levels, e.g. LOD0 = full ~100k, LOD1 = ~10k, LOD2 = ~1k) using `mesh_simplifier`.
- Each LOD is its own BLAS in `BLASManager`. The unique-geometry budget stays tiny:
  100–1000 parts × ~3 LODs.
- LODs are rotation-invariant decimations of the same mesh, so one LOD BLAS serves all
  instances of that part at any orientation — instancing-perfect, no per-orientation data
  (this is why decimated meshes beat baked-lighting imposters: no instancing breakage).

### 2. Sector grid (static spatial partition of the world)

- The world is carved into spatial **sectors**. Sectors are **finer near where detail
  matters and coarser far away** — a quadtree/octree (or nested grid) so near regions
  subdivide into small sectors and distant regions stay large.
- Each sector precomputes and stores:
  - its full instance list (part id + LOD-agnostic transform per instance),
  - a precomputed **merged far-proxy** (see layer 4),
  - its world-space bounds (for distance tests).

### 3. Per-frame sector resolution (the only per-frame work that scales)

For each sector, by **distance from the camera**, choose one of:

| State | What enters the TLAS |
|---|---|
| Near | full-res LOD0 instances (one TLAS leaf per part instance) |
| Mid | decimated LOD1/LOD2 instances (same parts, coarser BLAS) |
| Far | a **single merged proxy** instance for the whole sector |
| Out of range | nothing |

- **LOD is sector-uniform**: a whole sector switches LOD level as a unit. One decision per
  sector, not per instance. Smaller sectors near the camera keep the popping from
  sector-uniform LOD acceptable (the partly-near/partly-far sector is small, so its LOD
  boundary is close-grained where it matters).
- The chosen instances from all active sectors are appended to the TLAS, which is then
  rebuilt for the frame.

This bounds the active instance count: near sectors contribute many instances, far sectors
contribute one proxy each, out-of-range sectors contribute none — **independent of total
world size.**

### 4. Merged far-proxy (collapses a sector to one instance)

- A far sector becomes a **single TLAS instance** that stands in for all the parts in it.
- This is also where "hide small instances at distance" is implemented: distant sub-pixel
  parts are folded *into* the proxy, not deleted (see RT gotcha #2).
- Implementation of the proxy is **deferred / pluggable**: it can be a merged+decimated
  mesh of the sector, or the voxel-box imposter from the prior design, or another proxy
  form. The architecture only requires "one instance represents the sector."

### 5. TLAS builder fixes (prerequisite — current builder cannot scale)

Two defects in the current TLAS block the active-set sizes this design needs:

- **16-bit node-index cap.** `node.leftRight` packs child indices as `& 0xFFFF`
  (`bvh.cpp:559`), capping ~32k nodes ≈ ~16k instances before silent corruption. Must be
  widened (32-bit indices) — the visible set will exceed 16k instances immediately.
- **Index-order median split.** `BuildRecursive` (`bvh.cpp:524`) splits the instance array
  in half by *array index*, not by space — no SAH, no spatial sort. This produces a
  low-quality TLAS with overlapping nodes, and every *ray* pays for it every frame. Replace
  with a spatial median (or SAH) split.

---

## Raytracing-specific gotchas (baked into the design)

These are the traps when porting a rasterizer's sector/LOD scheme to a ray tracer:

1. **Cull by distance, not by camera frustum.** Secondary rays (shadows, reflections, GI
   bounces) can hit geometry behind or beside the camera. Frustum-culling a sector out of
   the TLAS makes off-screen objects stop casting shadows and vanish from reflections. So
   **sector activation is a distance sphere around the camera**, not the view frustum.
   (A cheaper frustum-culled *primary* pass is allowed later, but the TLAS must hold the
   surroundings.)

2. **Merge, don't drop, for "hide small instances."** Deleting distant small instances
   removes their shadows and silhouettes too — a distant forest would shimmer and vanish.
   Folding small instances into the sector's merged proxy preserves the aggregate shadow and
   silhouette while collapsing thousands of instances to one. "Hide small instances at
   distance" and "far sector → one proxy" are the same operation.

---

## Performance grounding (current code)

Measured/derived from the existing CPU median-split builder + GPU texture re-upload path:

| Instances rebuilt/frame | Build + upload | Viable for 60 fps? |
|---|---|---|
| ~10k | ~1–3 ms (build is sub-ms; cost is instance-texture re-upload) | Yes |
| ~32k | hits 16-bit node cap | Breaks until indices widened |
| Millions | tens+ ms CPU + huge upload | No — never rebuild this many per frame |

The design keeps the per-frame active set in the low tens of thousands at most, which is the
"yes" row once the builder is fixed.

---

## Goals / Non-goals

**v1 goals**
- Part LOD library: ~3 decimated BLAS levels per part via `mesh_simplifier`.
- Static sector grid, finer near camera (quadtree/octree or nested grid).
- Per-frame, sector-uniform LOD selection by distance; active instances → TLAS rebuild.
- Distance-sphere sector activation (not frustum).
- TLAS builder: widen node indices past 16-bit; spatial split instead of index median.
- A merged far-proxy per sector (simplest viable form first; voxel-box imposter is an
  allowed later swap-in).

**Explicit non-goals for v1 (deferred)**
- Per-instance-within-sector LOD (smoother, but reintroduces per-instance per-frame work).
- LOD cross-fade / dithered transitions to mask sector-boundary popping.
- Streaming sectors from disk / out-of-core world (v1 may assume the world description fits
  in RAM even if not in the TLAS).
- Frustum-culled primary pass / hybrid raster primary.
- TLAS refit (vs full rebuild) optimizations.

---

## Open questions (resolve during planning)

- Sector granularity: octree vs nested fixed grid; exact subdivision rule near camera.
- Distance thresholds for LOD0/LOD1/LOD2/proxy bands, and hysteresis to avoid thrash when
  the camera hovers on a boundary.
- Far-proxy form for v1: merged+decimated sector mesh vs voxel-box imposter.
- Whether the active set is rebuilt every frame or only when the camera crosses sector/LOD
  thresholds (the latter cuts rebuild frequency dramatically for a slow camera).
