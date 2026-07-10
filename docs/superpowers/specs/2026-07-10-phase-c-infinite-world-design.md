# Phase C Redesign: Infinite Voxel World — Design Spec

**Date:** 2026-07-10
**Status:** Approved by Jack (supersedes the world-assembly portions of
`2026-07-08-phase-c-explorer-demo-design.md`; the explorer app-shell, packaging,
and demand-bake machinery from that spec carry forward)

## Motivation

Profiling the closed 51×51 Meadow world showed the bottom-up root-part model
cannot meet the Phase C exit criteria:

- The root must enumerate every child up front (70,841 placements, 2,629
  variants with scatter; 5,203 with terrain only), producing a ~134 s
  resolve/hash wall before any bake starts. Cost is O(world size), paid at
  launch, even for tiles never seen.
- Per-bake fixed overhead (fold 3.0 ms + ctx/eval/free 0.9 ms) is 66% of a
  coarse tile's 5.9 ms bake; JS-interpreted noise is ~100× native.
- Disk cache on the 9p mount inflates in-app bakes ~8× (6 ms → 46 ms).
- A closed placement graph caps world size; infinite worlds cannot enumerate
  their requires.

The redesign inverts the model: a tile-parameterized **WorldSector** entry
point generates everything in its footprint on demand, streamed around the
camera. Cost becomes O(visible sectors). Minecraft's chunk/density-function
architecture is the validated precedent for every major piece (per-chunk
deterministic generation from `(seed, cx, cz)`, data-driven density graph
evaluated natively, coarse-sampled 3D field, staged neighbor-aware
generation).

## Exit Criteria (Phase C, restated)

1. **Instant world** — launch → flying over visible terrain in a few seconds,
   cold, no shipped cache.
2. **Endless flight** — fly any direction indefinitely; sectors stream in
   ahead of the camera; no hitching, no world edge.
3. **Detail follows camera** — near sectors full detail (terrain + scatter),
   far sectors coarse.
4. **Reroll** — new seed → new world, same instant startup.
5. **Stable memory** — sectors evict behind the camera; RAM/VRAM bounded
   regardless of flight distance.

Non-goals for Phase C: large cross-sector structures (castles/roads/bridges —
the architecture must not preclude them; see World Plan note), user terrain
editing (the field abstraction must support it later), true 3D infinite Y.

## Decisions of Record

- **Approach:** sectors are real parts through the real bake pipeline, with
  transient (memory-only) artifacts. One geometry pipeline; no parallel
  native-terrain subsystem. (Chosen over: all-disk-cached sector parts;
  fully native Minecraft-style chunk subsystem separate from parts.)
- **World shape:** infinite in XZ, bounded Y (world height range −64…+192).
  Sectors are 16×16-unit columns; the streamer is a 2D grid.
- **Terrain representation:** full voxel terrain now, extracted from a native
  SDF density field. v1 field is heightfield-shaped
  (`density = height(x,z) − y`, no caves), but all consumers use only
  `densityAt`, so caves/overhangs/edits are later field-graph changes, not
  architecture changes.
- **Future voxel editing:** base field + per-sector edit-delta overlay,
  composited before extraction; deltas are hashed inputs (edited sectors are
  cache misses, rest of world shared). Not built in Phase C; the interfaces
  must not preclude it.
- **Content:** noise-driven biomes reusing existing assets — ocean, meadow
  (lush), rocky foothills, mountains — varying continuously between heavy
  vegetation and rocky. Global water plane at sea level (no per-sector water
  geometry).
- **Hermetic bakes stay:** one fresh JSRuntime per bake. Amortization comes
  from fold/bytecode caches, not persistent contexts.

## Architecture — Four Layers

### 1. World definition (DSL — `MeadowWorld.js`)

New entry-point kind: `class MeadowWorld extends World`. Declares (does NOT
enumerate children):

- the **field graph** — composition of native noise primitives (declarative,
  hashable)
- the **biome table** — biome id → scatter rules (modules, per-area
  densities, slope limits, rung gates) and surface material rules
- world constants — sector size 16, height range −64…+192, `worldSeed`

Sketch:

```js
class MeadowWorld extends World {
  static params = { worldSeed: 20260709 };
  field(p) {
    const relief   = noise2(p.worldSeed ^ 1, 1/900, 3);
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 3);
    const plains   = noise2(p.worldSeed ^ 3, 1/160, 4).mul(8);
    const mounts   = ridge2(p.worldSeed ^ 4, 1/340, 5).mul(110);
    const height   = blend(plains, mounts, relief.smoothstep(0.45, 0.75)).add(-6);
    return { density: heightToDensity(height), moisture, relief, seaLevel: 0.0 };
  }
  biomes() { /* ocean / meadow / foothills / mountains scatter+material rules */ }
}
```

The world def evals once at load. Its canonical hash (graph + biome table +
constants) folds into every sector variant hash, so `regenerate(seed)` and
world-def edits invalidate correctly while scatter assets stay cache-shared.

### 2. Field runtime (C++ — new `terrain_field` module in MatterEngine3)

Compiles the declared graph into a native evaluator. API (C++ and DSL):
`densityAt(x,y,z)`, `heightAt(x,z)` (v1: direct height-branch evaluation;
later: bisection against density), `biomeAt(x,z)`, `materialAt(...)`.
All per-sample math is native; the DSL never evaluates noise.

**Primitive set (v1):** `noise2` (2D fBm, port of terrain_noise.js math),
`ridge2`, `warp2`, `blend`, `smoothstep`, `add/mul/min/max/clamp`, constants,
`heightToDensity`.

**Biomes:** two low-frequency control noises — `moisture` (lush ↔ rocky) and
`relief` (plains ↔ mountains). Biome id is a pure function of
(moisture, relief, height): height < seaLevel → ocean; high relief →
mountains; low moisture → rocky foothills; else meadow.

**Materials:** native rule stack per surface sample — slope > threshold →
rock; optional snowline; else biome surface (grass/dirt/rocky). Feeds the
existing material-bucket pipeline unchanged.

### 3. WorldSector part (DSL class, engine-instantiated)

`WorldSector(tx, tz, rung, worldSeed)` — a real part baked by the real
pipeline. `build()`:

1. `this.terrainVolume(rung)` — native verb, end-to-end C++:
   - **Slab bounds:** `heightAt` on a coarse lattice over footprint+halo →
     `[hMin, hMax]`; extraction covers only that y-slab (cost scales with
     surface, not volume).
   - **Bulk fill:** evaluate density over the slab grid straight into the
     voxel session's scalar field, one native loop.
   - **Extract:** existing MatterSurfaceLib isosurface path — same mesher,
     buckets, artifact format as voxel parts today.
   - **Materials + normals:** `materialAt` per surface sample; normals from
     field gradient (finite difference).
   - **Skirt:** short downward apron at sector borders (Phase C crack
     answer; transvoxel stitching is a later upgrade behind the same verb).
2. Deterministic scatter for its footprint (below), `placeChild`ing shared
   asset parts.

**Resolution ladder** (rung → voxel size for a 16 u sector):

| Rung | Voxel | Grid (XZ) | ~Tris | Role |
|---|---|---|---|---|
| 0 | 2.0 | 8² × slab | ~200 | horizon fill, first frame |
| 1 | 1.0 | 16² × slab | ~800 | mid distance |
| 2 | 0.5 | 32² × slab | ~3K | near |
| 3 | 0.25 | 64² × slab | ~12K | around camera |

Budget: rung 3 ≈ 130–260K field samples ≈ 10–25 ms native + extraction of the
same order → ~30–60 ms/sector; rung 0 in low single-digit ms. Coarse-first
ladder buys perceived latency (instant silhouette), not total work — finest
rung dominates the sum (~1.15× finest).

**Scatter LOD rides the rungs:** rung 0–1 place nothing/rocks-only; grass
exists only at rung 2–3. Rung climb re-bakes the sector with its scatter in
one artifact, so terrain and scatter always appear consistently. Per-sector
instance budgets replace the old global scatter budget.

### 4. Sector streamer (C++, evolution of RefineController)

Owns the **desired set**: for the camera position, which `(tx, tz, rung)`
variants should exist. Concentric rings (tuning constants): rung 3 ≤ ~48 u,
rung 2 ≤ ~120 u, rung 1 ≤ ~300 u, rung 0 to horizon ~800 u. Per throttled
tick, diff desired vs resident:

- **enqueue** missing sectors into demand-bake (Tasks 13–14 machinery),
  priority = distance, weighted toward camera heading
- **swap** rungs refine-style: old rung visible until new publishes, then
  release — no holes in either ladder direction
- **evict** outside the horizon ring: `PartStore::release` +
  `GpuCuller::release_part` + free the transient artifact; memory bounded by
  ring area
- **hysteresis** ~1 sector of slack per ring edge to prevent thrash

**Open placement set:** the world composer's closed-child-table contract
dissolves at the top level only. The engine root becomes a streamer-owned
dynamic placement table (`add_placement` / `remove_placement` on
world_composer/sector_grid). Each sector's own child table (scatter) is still
closed inside its artifact. Resolve is O(1) per request:
hash(sector source, tx, tz, rung, worldSeed, fieldHash) → residency check →
bake. No enumeration, no manifest, no resolve wall.

**Budgets:** in-flight sector bakes ≈ worker count ×2; publishes ride the
per-frame GPU budget (pump_gpu_jobs throttle becomes a core tuning knob).

**Reroll:** drop all resident sectors, recompile field graph, streamer
restarts from the coarse ring. No disk-cache interplay (sectors never on
disk).

## Scatter

**Local scatter (grass, pebbles, small rocks):** sector-local RNG streamed
from `hash(worldSeed, tx, tz)`; positions jittered in footprint, snapped via
`heightAt`, filtered by point queries — `biomeAt`, slope (`GRASS_SLOPE_MAX`
carries over), rung gate. Counts derive from the tuned per-area densities
(≈156 grass, ≈16 pebbles, ≈2 rocks per meadow sector at top rung), scaled
continuously by moisture (the lush↔rocky gradient).

**Spaced scatter (oaks, landmark boulders) — deterministic jittered grid:**
a virtual cell grid of size `TREE_MIN_DIST` (24 u; boulders ~70 u). Each
cell derives one candidate position + acceptance roll purely from
`hash(worldSeed, cellX, cellZ)` — any sector can compute any cell's candidate
without baking anything. A sector places accepted candidates inside its own
footprint (after biome/slope checks). Min spacing holds globally by
construction (≤1 per cell); deterministic under any bake order; no seams.

**Trees:** placement changes; the Tree bake does not. Still one shared
disk-cached part, baked once asynchronously on first demand; oaks pop in when
it lands. The geo_assert shipped-disabled gate is unchanged and separate.

**Water:** one global animated water surface at `seaLevel` rendered by the
explorer; ocean-biome sectors simply have terrain below it.

## Bake-Path Amortization & Transient Policy

- **Fold cache** (ScriptHost, keyed by part-source hash): kills 3.0 ms of
  redundant shared-lib disk reads per bake. Applies to all parts.
- **Bytecode cache** (optional): compiled class bytecode reuse (−0.7 ms
  ctx/eval). Fresh JSRuntime per bake is retained.
- **Transient BakePolicy bit** (engine-side, set by the streamer for sector
  variants): skip `cached()` disk probes and `save_v2`; artifacts live only
  in the part store. Removes all 9p traffic from the hot loop. Scatter
  assets keep disk caching.

## Error Handling

- Field-graph compile error → fail at world load, error toast naming the
  offending graph node. Never a half-world.
- Sector bake failure → classify_error + toast; the slot keeps its current
  lower rung (never a hole); retry with backoff; after N failures leave the
  coarse rung standing.
- Scatter child failures (e.g., Tree geo_assert) → today's handling,
  unchanged.

## Testing

- **terrain_field unit (C++):** primitive determinism; canonical graph-hash
  stability; `heightAt`/`densityAt` consistency; biome classification at
  known points.
- **terrainVolume:** slab bounds contain the surface; per-rung tri budgets;
  skirts present at borders; adjacent-sector seam gap bounded at equal and
  unequal rungs.
- **Jittered-grid scatter (property tests):** min distance holds across
  sector borders under any bake order; output identical under permuted bake
  order.
- **Streamer (headless logic):** desired-set diffing; hysteresis; eviction
  correctness; resident-count bound over a simulated long flight.
- **Integration smoke:** headless explorer flies a straight line N
  sim-seconds — no missing-sector holes ahead of camera, bounded RSS,
  wall-clock t_ready gate.
- Closed-world suites (meadow_bake_check child/variant counts, 51×51
  layout tests) retire with Meadow.js.

## Migration / Scope

**Dies:** Meadow.js requires-enumeration and 51×51 layout; radial bands;
resolve/manifest warm-relaunch cache (Task 17 — nothing to enumerate);
closed-world count tests.

**Survives as foundation (same branch):** bake workers + demand-bake
(Tasks 13–14), publish/residency (3–5), refine swap mechanics (4/6 →
generalized to the rung ladder), escape menu/HUD/reroll (7/9/10), explorer
app shell + smoke mode (8), Windows packaging (11), scatter assets and their
tests, JS `terrain_noise.js` math (ported to native primitives).

**Re-adjudicated:** old Task 12 wall-clock gates re-target the new
architecture.

**Future phases enabled, not built:** world plan layer for cross-sector
structures (anchored parts + plan-queryable sliced features); voxel edit
deltas + second extractor; caves/overhangs via field-graph changes;
transvoxel LOD stitching; true 3D sectors if ever needed.
