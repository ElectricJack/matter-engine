# Geometry Modifier Regions Design

**Date:** 2026-07-08
**Status:** Approved (brainstorm with Jack; Engine Flow amended same day — modifiers run at
part **bake**, not flatten, after pipeline research)
**Depends on:** Phase B async bake landing first (part_flatten/local_provider under active rewrite)

## Motivation

Retopo (Phase 5 autoremesher integration) currently operates on a part's **whole merged
flattened mesh**, opted in via a `static retopo = {...}` class block. This fails for the
Meadow Tree two ways:

1. The merged mesh mixes isosurface output (trunk + bark voxel field) with instanced
   child meshes (TreeBranch twigs/leaves) — only the isosurface portion is the
   closed-organic-surface input the cross-field quad remesher was designed for.
2. QEM `simplify(0.3)` runs **before** retopo sees the mesh, and QEM collapse introduces
   exactly the non-manifold edges that trip geogram's `geo_assert` → `abort()`
   (Tree.js ships `retopo: enabled: false`; the on-disk blacklist journal is the only net).

Separately, per-part post-processing is scattered: `simplify()` is a one-off part-level
call, retopo is a static class block, and there is no mesh smoothing at all (MSL only has
SDF-level smooth-min blending). One mechanism should own all of it.

## Decisions (settled during brainstorm)

- **Explicit region markers, independent of session type** (`beginModifier`/`endModifier`),
  not per-voxel-session flags and not a scope rule on a static block.
- **Ordered modifier list = execution order.** The simplify-vs-retopo ordering question
  dissolves: the stack is explicit per region.
- **Clean cut, no legacy paths:** `this.simplify()` and `static retopo` are **deleted**,
  not aliased. All callers migrate in the same change (Tree.js is the only live
  `simplify()` caller; Rock.js the only `static retopo` user).
- **Retopo consumes raw (optionally smoothed) isosurface output**, never post-QEM meshes.
- **Failure = skip with warning:** a modifier that fails at bake (retopo
  abort/blacklist, OOM, timeout) is skipped; the rest of the stack still runs. No special
  fallback syntax — a stack of `[{smooth}, {retopo}]` degrades to the smoothed mesh.
- **Modifiers run at part bake, not flatten** (amendment): geometry first exists as
  triangle meshes during `script_host::bake_source` (BuildOps → csg_lowering → per-cell
  marching cubes → BLAS → `.part`). Regions are meshed, cross-cell welded into one
  indexed mesh, stack-processed, and registered as BLAS before the asset is written.
  The flatten-time retopo hook was only ever there because that's where a merged mesh
  happened to exist; it is deleted, and the separate `.retopo.part` cache goes with it —
  baked parts are already cached by resolved hash.
- **TreeBranch converts to isosurface** as part of the migration (its `line()` tube pass
  becomes a voxel session; `line()` is already an SDF capsule brush in-session per the
  session-polymorphic DSL convention). Leaves stay mesh.
- **Implementation waits for Phase B.** Spec + plan now; execution after Phase B lands.

## DSL Surface

```js
class Tree extends Part {
  build(p) {
    this.beginModifier();
      this.beginVoxels(VOX);
      // ... trunk core + bark strands ...
      this.endVoxels();
    this.endModifier([
      { smooth: { iterations: 2 } },
      { retopo: { target_ratio: 1.0, iterations: 3, seed: 42, timeout_seconds: 120 } },
    ]);
  }
}
```

- `beginModifier()` — opens a region. All geometry the part emits until `endModifier`
  (including the meshed output of voxel sessions begun inside) belongs to the region.
- `endModifier(list)` — closes the region and attaches its ordered modifier stack.
- **Regions do not nest** (clean error). Multiple sequential regions per part are fine.
- Geometry outside any region passes through the bake untouched (identity).
- Child parts placed inside a region are **not** captured — `placeChild` instances are
  governed by the child schema's own regions. Tree cannot reach into TreeBranch.
- Unbalanced markers, unknown modifier names, or malformed params are clean script errors
  at build time (fail the bake of that part, not the process).

### v1 Modifiers

| Modifier | Params | Semantics |
|---|---|---|
| `simplify` | `ratio` (0..1] | QEM simplification to `ratio` of input tris (existing MSL QEM machinery) |
| `smooth` | `iterations` (default 2), `lambda` (default 0.5), `mu` (default -0.53) | Taubin λ/μ smoothing — volume-preserving; NEW MSL pass on MeshIndexed |
| `retopo` | `target_ratio`, `iterations`, `seed`, `timeout_seconds` | autoremesher cross-field quad remesh (existing MSL `retopo()` wrapper) |

Shorthand `{ simplify: 0.3 }` (bare number) is accepted for simplify.

## Engine Flow (amended: bake-time, not flatten-time)

1. **Build:** `beginModifier`/`endModifier` bindings record region boundaries as index
   ranges over the DslState build buffer (and direct-triangle buffer) plus the parsed
   modifier stack. Pure bookkeeping; no geometry work during build().
2. **Bake (`script_host::bake_source`):** region BuildOps are lowered and cell-meshed
   separately from non-region ops. Per region, per material-merge-group: cell meshes are
   accumulated across all cells and welded into one indexed mesh (cross-cell seams become
   interior edges), the ordered stack runs on it, and the result registers as one BLAS
   entry. Non-region geometry takes the existing per-cell path untouched.
3. **Caching:** none beyond the existing `.part` asset cache — baked parts are keyed by
   resolved hash, so modifiers (including retopo) rerun only when the part actually
   changes. The `.retopo.part` sibling cache is deleted along with the flatten hook.
4. **Blacklist:** the crash-recovery journal (`retopo_blacklist`) moves to bake time,
   keyed per region chunk — fold(welded region mesh hash, stack settings). A crashing
   chunk blacklists itself; other chunks, regions, and parts proceed on restart.
5. **Flatten:** `apply_retopo_hook`, its cache helpers, `FlattenTargets::retopo`,
   `eval_retopo_settings`, and `RetopoSettings` are all deleted (clean cut).
6. **No-autoremesher builds:** as today — retopo modifier logs a one-line warning and is
   skipped (Windows cross-build stays clean without autoremesher_core).

## Smooth Implementation (new MSL pass)

- Taubin λ/μ (shrink-free Laplacian): alternate positive `lambda` and negative `mu`
  steps per iteration, uniform (umbrella) weights over the vertex 1-ring from the
  indexed mesh adjacency. Operates on MeshIndexed in place.
- Boundary vertices (if any — isosurface output is normally closed) are held fixed.
- Standalone per-suite test binary at `MatterSurfaceLib/tests/mesh_smooth_tests.cpp`
  per MSL convention (own `main()`, registered in tests/Makefile, SIMP_TARGET pattern).
- Note: this adds a new file to MatterSurfaceLib. MSL is read-only-except-genuine-need;
  this is new capability, surfaced here deliberately as an approved scope decision.

## Schema Migrations (same change, no legacy)

- **Tree.js:** delete `this.simplify(0.3)` and the `static retopo` block; wrap the
  trunk+bark voxel session in a region with `[{smooth}, {retopo}]`. Retopo re-enabled.
- **TreeBranch.js:** wrap the tube pass in `beginVoxels(...)`/`endVoxels()` (choosing a
  voxel spacing that resolves the thinnest twig tips) inside its own modifier region;
  leaves remain mesh and untouched.
- **Rock.js:** `static retopo` block → modifier region around its voxel geometry.
- Remove `simplify`/`retopo` static plumbing from script_host eval paths once no schema
  references them.

## Testing

- MSL: `mesh_smooth_tests` — volume preservation within tolerance, closed-mesh
  invariants (vert/index counts unchanged, no NaNs), determinism byte-identity.
- Engine: region plumbing tests at the bake level — region chunk isolated from
  non-region geometry; stack order respected (simplify-then-smooth ≠ smooth-then-simplify
  fixture); failure-skip (forced retopo failure still yields smoothed chunk); multiple
  regions independent; cache key changes when stack edited.
- Retopo integration tests (Task 14 fixture) updated to region form.
- Real gate: Meadow bake with Tree retopo enabled through the region path
  (GALLIUM_DRIVER=d3d12; scripted viewer run must self-terminate per tools/viewer_shots.sh).

## Non-goals

- Subprocess isolation for retopo crash safety — still the cleaner long-term net,
  remains on ROADMAP.md as an independent follow-up (blacklist journal covers v1).
- Nested regions, per-instance modifier variation, modifiers on child-part output.
- Additional modifiers (decimate-to-count, remesh-uniform, displacement) — additive later.
- Migrating any pipeline outside the three schemas named above.
