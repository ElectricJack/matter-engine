# Frame-Time Reduction Package: CPU Floor + LOD Ladder + Procedural Grass Budgets

**Date:** 2026-07-03
**Status:** Approved design
**Target:** meadow scene < 16 ms/frame at 1280×720 on the raster path

## Context

The meadow benchmark world (44,896 instances: terrain tiles, 40k grass clumps,
rocks, pebbles, 40 oaks) ran at ~97 ms with 16.9M drawn triangles. Those numbers
were partly caused by a matrix-convention bug in the raster cull/LOD path (fixed
in commit `12f8459`): culling was all-or-nothing with camera pitch and every
cluster's LOD tracked the camera's distance to the world origin. Post-fix
baseline (same day): ~146 ms whole-scene aerial, ~78 ms far ground view, and —
critically — **~70 ms for a frame drawing zero triangles**, proving a large
fixed CPU cost independent of scene content.

This package reduces frame time through four measured stages. Each stage ends
with the same benchmark sweep; work stops early if the target is met.

Decisions locked during design:

- Instant LOD switches are accepted (no crossfade/dither); thresholds are tuned
  so pops stay small.
- Default LOD path remains mesh-level (flatten → error-bounded QEM ladder);
  schemas may opt in to procedural triangle-budget LOD. Grass is the only
  schema converted in this package.
- GPU-driven culling/instancing is the **next** package after this one
  (explicit backlog item, ahead of imposters).

## Stage 0 — Measurement Pass

A repeatable headless benchmark using the existing `MATTER_CMD_FIFO` workflow.

**Camera set** (fixed, scripted):

| Name | Purpose |
|---|---|
| aerial | whole-scene view, everything in frustum |
| corner | in-crowd ground view (grass + trees dominate) |
| midfield | mid-distance view (grass tri-share peak) |
| far | far-corner ground view across the world |
| empty | outside the world looking away — isolates fixed CPU cost |

**Enablers:**

1. New FIFO command `stats` — dumps one CSV line to stdout: camera, frame ms,
   drawn tris, batches, active instances, culled clusters, plus the timing
   split below.
2. Per-frame CPU timing split accumulated into the HUD stats: `resolve ms`,
   `build_batches ms`, `draw ms`. This split decides whether remaining cost is
   CPU floor or GPU triangles, and gates each later stage.

**Output:** CSV rows appended to a tracked results file
(`MatterEngine3/docs/perf/meadow_sweep.csv`) so per-stage regressions are
visible in-repo. Baseline is captured immediately, post-convention-fix.

## Stage 1 — CPU Floor

Three fixes, in order of certainty:

1. **`-O2` for the viewer build.** The viewer Makefile's `CFLAGS` has no
   optimization flag (builds at -O0). Add `-O2` to both Linux and Windows
   targets; clean-rebuild all objects. Micro-benchmark evidence: the
   fingerprint + binning hot loops run ~3× faster at -O2.
2. **Cache the resolver's sector binning.** `SectorLodResolver::resolve`
   re-bins all ~44k instances into a `std::map` every frame (~25 ms at -O0)
   even when nothing moves. Fix: `WorldState` gains a monotonic version
   counter bumped by `reset()`/`apply()`; the resolver keeps its sector table
   and rebuilds only when the version differs. Per-frame work becomes walking
   sectors for activation + LOD selection (hundreds of sectors, not 44k
   instances).
3. **Cheapen the rebuild fingerprint.** `build_batches` FNV-hashes ~3.3 MB of
   full 64-byte transforms per frame. Since transforms only change via world
   deltas (which bump the version), the fingerprint shrinks to: camera
   position/target + world state version + per-instance (id, LOD) — ~12 bytes
   per instance; transform bytes drop out entirely.

Constraints: no threading; resolver interface and outputs unchanged — the same
results are simply not recomputed when inputs haven't changed.

**Exit criterion:** empty-frame view drops from ~70 ms to low single-digit ms;
timing split shows resolve + build_batches < ~3 ms combined in the aerial view.

## Stage 2 — Ladder Retune

All knobs live in bake-side `FlattenTargets` or viewer-side runtime dials.

1. **Remove the small-part floor.** `min_tris = 2000` stops ladder generation
   for small parts (a 96-tri grass clump gets one level — LOD0 forever). New
   stop rule: keep adding rungs until a level stops shrinking or reaches
   ~32 tris.
2. **Denser, wider ladder.** Replace ε divisors {256, 64, 16, 4} (ratio-4
   jumps, up to ~4 px error per switch, terrain LOD0 held to ~34 m) with a
   ratio-2 schedule {512, 256, 128, 64, 32, 16, 8, 4, 2}: finer near rungs
   (switch sooner, smaller pops) and coarser far rungs (terrain tile drops to
   tens of tris at distance). ~9 levels; storage cheap; BLAS count bounded.
3. **Runtime `pixel_budget` dial.** Bake keeps thresholds at budget 1.0; the
   viewer applies the dial at selection time (`psize_effective = psize ×
   budget`) — works without re-baking. Exposed as HUD slider + FIFO command so
   the sweep can bisect quality/speed. The same dial scales the sub-pixel cull
   threshold (`kMinProjectedSize`).
4. **Never-invisible guarantee.** With the convention bug fixed, terrain tiles
   (radius ~14 m) never reach sub-pixel inside the 400 m active radius — but
   the guarantee becomes structural: parts with bound radius ≥ 4 m are never
   floor-culled, only clamped to their coarsest rung. Small scatter (grass,
   pebbles) remains floor-cullable. No schema change.

**Invalidation:** flat artifacts are not keyed by `FlattenTargets`; this stage
wipes `cache/parts/*.flat.part` once and adds a bake-version byte to the flat
header so future target changes re-bake automatically.

**Exit criterion:** aerial drawn-tri count drops from the 16.9M class to low
millions at budget 1.0, with pops judged acceptable in an interactive
fly-through; frame times re-measured on the Stage 0 sweep.

## Stage 3 — Opt-in Procedural Triangle-Budget LOD (grass)

QEM fails perceptually on aggregate thin geometry: collapsing a whole grass
blade is near-zero geometric error, so decimated grass looks sparse rather
than coarse. Opted-in schemas regenerate at reduced budgets instead.

**Schema API.** A part schema may export a budget ladder:

```js
export const lodBudgets = [1.0, 0.3, 0.08];  // fractions of full-res tri count
```

and its generator reads an implicit `lodBudget` param (default 1.0). Schemas
without the export are untouched (QEM ladder as today).

**Coherence contract** (the script's responsibility, documented and tested):

- Blade placements are drawn from the seeded RNG *first*; the generator then
  emits the first ⌈k·N⌉ blades — level N+1's blades are a strict prefix subset
  of level N's, so switches don't reshuffle the field.
- Coverage is preserved by widening surviving blades (~1/√k), keeping green
  ground cover at distance.
- TriEx (materials/tint/AO) comes straight from the generator at every level —
  native, no reprojection.

**Pipeline integration.** Budget variants are parameterized bakes: the baker
bakes `hash(schema, params + lodBudget=b)` per level — content-addressed, so
invalidation stays free. `flatten_part` assembles the opted-in part's ladder
from the variant meshes instead of calling `decimate_to_error` for those
levels. Thresholds follow the same ratio-2 schedule as the mesh ladder,
anchored so full-res holds until a blade is ~2 px. Below the last budget
level, the normal sub-pixel floor cull applies (grass may vanish at extreme
distance; the Stage 2 guarantee keeps terrain visible).

**Scope:** Grass.js only. Leaves/twigs are later candidates; trees already
collapse well under QEM + flatten.

**Exit criterion:** overall package target — meadow sweep < 16 ms at
pixel_budget 1.0, with the midfield view checked visually for coverage and
popping on the Windows build.

## Testing & Verification

Headless tests extend existing suites in `MatterEngine3/tests/`:

- **Resolver cache** (viewer_logic_tests): identical resolve output before/
  after caching; world delta bumps version and re-bins; camera move alone does
  not re-bin.
- **Fingerprint** (viewer_logic_tests): cache hit when nothing changes; miss on
  camera move, world delta, and LOD flip (extends the task13 fingerprint
  tests).
- **Ladder shape** (part_flatten_tests): a <2000-tri part gets multiple rungs;
  rung tri-counts monotonically decrease; ratio-2 schedule yields ≥ 6 levels on
  the terrain tile fixture; bake-version mismatch triggers re-flatten.
- **Never-invisible** (viewer_logic_tests): radius ≥ 4 m part at extreme
  distance selects the coarsest rung, never lod = -1; a small part still
  floor-culls.
- **Pixel-budget dial** (viewer_logic_tests): budget 0.5 selects
  coarser-or-equal levels than 1.0 for every cluster in a fixture sweep.
- **Procedural budgets** (new grass_lod_tests): per-level tri count ≤ budget
  fraction; prefix-subset property (level 1 blade root positions ⊂ level 0's,
  exact); determinism (same seed ⇒ byte-identical levels); TriEx present at
  all levels.

**Benchmark gate:** the Stage 0 sweep runs after every stage; CSV is committed.
The package is done when the full sweep is < 16 ms — or we stop and the
remaining gap becomes input to the GPU-driven-culling backlog item.

**Visual verification:** per-stage FIFO screenshot set (same five cameras)
compared side by side; final interactive pass by Jack on a freshly built
`viewer.exe` (fly-through for popping and grass coverage). Per project rule,
`make windows` runs after every engine/viewer change.

## Out of Scope (tracked)

- GPU-driven culling/instancing — **next package** after this one.
- Imposter far-field tier.
- LOD crossfade/dither (instant switches accepted for now).
- Converting leaves/twigs to procedural budgets.
- MatterSurfaceLib changes (read-only; QEM `max_error` mode already suffices).
