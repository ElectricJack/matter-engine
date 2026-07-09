# Geometry Modifier Regions Design

**Date:** 2026-07-08
**Status:** Approved (brainstorm with Jack)
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
- **Failure = skip with warning:** a modifier that fails at flatten (retopo
  abort/blacklist, OOM, timeout) is skipped; the rest of the stack still runs. No special
  fallback syntax — a stack of `[{smooth}, {retopo}]` degrades to the smoothed mesh.
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
- Geometry outside any region passes through flatten untouched (identity).
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

## Engine Flow

1. **Build:** script host records region boundaries + stacks alongside emitted geometry
   (region id tagged onto chunks; stacks serialized with the part's build output the same
   way retopo settings flow today via `eval_*_settings` in script_host.cpp — NOT
   dsl_bindings.cpp, which owns runtime verb bindings only).
2. **Flatten:** per-region sub-meshes are kept separate; each region's stack runs in
   list order on its chunk; then chunks (modified + untouched) merge. This **replaces**
   the whole-merged-mesh `apply_retopo_hook` in part_flatten.cpp.
3. **Caching:** `.retopo.part` cache re-keys **per region** — fold(region mesh hash,
   full stack settings, extraction-sync constant, platform) — so any stack edit
   invalidates only that region's cached output.
4. **Blacklist:** the crash-recovery journal (`retopo_blacklist`) keys on the same
   per-region hash. A crashing region blacklists itself; other regions and parts proceed.
5. **No-autoremesher builds:** as today — retopo modifier logs a one-line warning and is
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
- Engine: region plumbing tests at the flatten level — region chunk isolated from
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
