# MatterSurfaceLib Mesh Simplification Design

**Date:** 2026-06-13
**Status:** Approved (design), pending implementation plan
**ROADMAP goal:** "fast mesh simplification algorithm that can maintain mesh boundary conditions for each cell" (ROADMAP.md, MatterSurfaceLib section)

## Goal

Generate low-polygon proxy meshes that capture the appearance of MatterSurfaceLib's
high-resolution marching-cubes cell meshes at much lower render cost — the
ray-tracing-correct equivalent of the originally-envisioned "imposters."

## Why geometric decimation, not classic imposters

Classic imposters (view-dependent billboards, octahedral/texture atlases) break under
ray tracing: secondary rays (reflection, shadow, AO) strike geometry from arbitrary
directions, so a view-aligned proxy is wrong for every ray that isn't the primary camera
ray. The RT-correct equivalent is real, view-independent low-poly geometry. Fewer
triangles produce a smaller, shallower BLAS — faster traversal and fewer
ray-triangle intersection tests for *all* ray types.

Fine surface detail (which decimation discards) is recoverable later via normal-map
baking, which works in RT because it only perturbs the shading normal at the hit point.
Normal/detail baking is explicitly deferred to a follow-up pass and is out of scope here.

## Scope

In scope:
- In-repo QEM (Quadric Error Metrics) edge-collapse decimation.
- Hard-locked cell-boundary vertices to guarantee watertight seams between
  same-level neighbor cells.
- Geometric simplification only (positions + recomputed smooth normals).
- Uniform simplification level across all cells, with an integration hook ready for
  per-cell distance-based LOD selection later.

Out of scope (future work):
- Normal/detail baking.
- Per-cell / distance-based LOD selection (hook only; selection logic deferred).
- Cross-LOD-level seam stitching (uniform level means all neighbors share a level).

## Architecture

New standalone module, no new third-party dependencies:

- `MatterSurfaceLib/include/mesh_simplifier.hpp`
- `MatterSurfaceLib/src/mesh_simplifier.cpp`

The simplifier is pure CPU geometry: it consumes a raylib `Mesh` and produces a new
raylib `Mesh`. It has no GL dependency, so it runs headless in unit tests.

### Public interface

```cpp
struct SimplifyOptions {
    float target_ratio  = 0.5f;     // fraction of triangles to keep, (0..1]
    float max_error     = FLT_MAX;  // stop once min collapse cost exceeds this
    bool  lock_boundary = true;     // freeze vertices on a cell face plane
};

struct CellBounds { Vector3 min_bound, max_bound; };

// Returns a NEW indexed Mesh. Does not mutate input. Caller owns the result.
Mesh simplify_mesh(const Mesh& input,
                   const SimplifyOptions& opts,
                   const CellBounds* bounds = nullptr);
```

## Algorithm (QEM edge-collapse, Garland–Heckbert)

1. **Build working topology** from the indexed input mesh: vertex list, triangle list
   (index triples), per-vertex adjacency (incident triangles).
2. **Per-vertex quadric** `Q = Σ Kp` over incident triangle planes, where
   `Kp = p·pᵀ` and `p = [a,b,c,d]` for the plane `ax+by+cz+d=0` (normalized so
   `a²+b²+c²=1`).
3. **Per-edge collapse target & cost.** Try the QEM-minimizing position by solving the
   3×3 system from the summed quadric `Qi+Qj`. If singular, fall back to the cheaper of
   the two endpoints or the midpoint. Cost = `vᵀ(Qi+Qj)v`.
4. **Min-heap** of all unique edges keyed by cost.
5. **Greedy collapse loop.** Pop lowest-cost edge, collapse (merge `j` into `i` at the
   target), mark touched triangles removed/updated, recompute affected vertices' quadrics
   and re-push affected edges. Stale heap entries are skipped using a per-vertex version
   stamp.
6. **Stop conditions:** triangle count ≤ `target_ratio * input_triangles`, OR next min
   cost > `max_error`, OR no valid collapses remain.
7. **Triangle-flip prevention.** Before accepting a collapse, for each affected triangle
   compare its post-collapse normal to its pre-collapse normal; reject the collapse if any
   triangle would flip (dot < 0) or become degenerate (near-zero area). Move to the next
   edge.
8. **Rebuild output.** Compact surviving vertices/triangles into a new indexed `Mesh`,
   then recompute smooth area-weighted vertex normals, matching the marching-cubes
   convention in `surface.c`.

## Boundary locking & watertight guarantee

A vertex is a **boundary vertex** if it lies on any of the 6 cell face planes — within an
epsilon of `min_bound` or `max_bound` on the x, y, or z axis. When `bounds` is supplied
and `lock_boundary` is true:

- A locked vertex is **never moved** (position frozen).
- An edge with **both** endpoints locked is **never collapsed** (would alter the boundary
  polyline).
- An edge with **one** locked endpoint may collapse, but the merged vertex is forced to
  the locked endpoint's position (the interior vertex snaps onto the boundary; boundary
  geometry is unchanged).

**Watertight argument:** two same-level neighbor cells share a face. The marching-cubes
scalar field is identical along that shared plane, so both cells produce the *same*
boundary vertices there. If neither cell ever moves or removes those vertices, the
simplified meshes still meet exactly along the seam — no cracks. This holds only for
neighbors at the same simplification level; cross-level stitching is future work.

## Integration

- `Cluster` gains `float simplification_ratio_ = 1.0f` and a setter that re-marks all
  cells dirty so they regenerate.
- After `Cell::generate_mesh_for_material()` produces a mesh, when `ratio < 1.0` the cell
  calls `simplify_mesh(mesh, {ratio, ...}, &cellBounds)` using its own
  `min_bound`/`max_bound`. Ratio `1.0` is identity and is skipped entirely.
- UI: a "Simplification" slider (0.05–1.0) in `render_ui()` drives the setter.

This slider is the **distance-LOD-ready hook**: per-cell distance-based ratio selection
later replaces the single uniform value without touching the simplifier itself.

## Error handling & edge cases

- Empty mesh (0 triangles) → returns an empty mesh.
- Single triangle / already at or below target → returns a copy of the input.
- Non-indexed input → build an index by welding identical positions before decimation.
- Singular QEM solve → endpoint/midpoint fallback (never abort).
- All remaining collapses rejected (flips/locked) → stop early and return current state.

## Testing

`MatterSurfaceLib/tests/mesh_simplifier_tests.cpp`, plain `assert` + `printf`, headless
(Mesh is CPU-side, no GL upload). Wired into `tests/Makefile` and `build-all.sh test`.

1. **Triangle reduction:** output triangles ≤ `ratio * input` for a known mesh at ratio 0.5.
2. **Identity:** ratio 1.0 returns a geometrically equivalent mesh.
3. **Boundary preservation:** every input boundary vertex position is still present in the
   output; boundary triangle count unchanged.
4. **No flips/degenerates:** every output triangle has positive area and consistent winding.
5. **Determinism:** same input + opts → byte-identical output (stable heap tie-breaking).
6. **Degenerate input:** empty mesh, single triangle, already-minimal mesh — no crash.
7. **Watertight seam:** build two adjacent cells from a shared scalar field, simplify both,
   assert shared-face boundary vertices match exactly (no gap).
