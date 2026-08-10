# StreamMountain refactoring plan — 2026-08-09

Scope: the StreamMountain scene tier (`projects/world_demo/scenes/StreamMountain/*`,
`projects/world_demo/shared-lib/alpine_ecology.js`, and the JS↔native boundary it
crosses). Goals, in priority order: (1) less code, (2) push compute native without
game-specific logic entering the engine, (3) readability. Output does not need to be
bit-identical; each work package below states its expected visual drift.

This is a plan only — no code has been changed. The working tree at analysis time
carried unrelated uncommitted edits (whitespace-only reformat of
`alpine_ecology.js`, deletion of the four `StressForest*.js` fixtures); everything
below refers to committed `main` content.

---

## 1. Measured hot-spot breakdown

**How obtained:** built `libmatter_engine3.a` and ran the committed investigation
harness — `make -C MatterEngine3/tests run-scatterprof` (8 bakes per row,
single-threaded, MSYS2 UCRT64, this machine, 2026-08-09). The harness
(`MatterEngine3/tests/sector_scatter_profile.cpp`) drives the real
`scenes/StreamMountain/objects/WorldSector.js`, the real world definition, and the
real compiled habitat tape (12 channels) through the same
`eval_world → SurfaceProgram::parse` path `install_world` uses, with
`MATTER_SCRIPT_PROFILE=1` set internally. These are fresh numbers, not estimates.
Caveat carried over from the harness itself: this is the **composition of one
bake**, not fill throughput (fill is stage_load-dominated per
`sector-bake-time-breakdown` memory; that is out of scope here).

Per-bake numbers (profile sums ÷ 8 bakes), against wall ms/bake:

| slot                      | band 5 (near, rung 2) | band 4 (rung 1) | band 3 (far veg edge, rung 0) | band 2 (no vegetation, 128 m) |
|---------------------------|------:|------:|------:|------:|
| **wall ms/bake**          | **508.7** | **177.2** | **119.1** | **59.4** |
| plan.habitat (self)       | 151.8 | 39.1 | 25.6 | — |
| plan.otherFamilies (self) | 95.6  | 10.1 | —    | — |
| plan.select (self)        | 69.7  | 26.2 | 18.7 | — |
| sector.terrain            | 62.2  | 17.0 | 4.5  | 5.1 |
| plan.evaluate (self)      | 21.8  | 23.5 | 22.3 | — |
| plan.exclude              | 19.4  | 19.8 | 19.7 | — |
| plan.candidates           | 12.4  | 3.4  | 2.2  | — |
| sector.place              | 8.9   | 4.6  | 0.3  | — |
| sector.rocks              | 1.1   | 1.3  | —    | — |
| profiled self total       | 444.0 | 145.4| 93.6 | 5.1 |
| **unprofiled gap**        | ~65   | ~32  | ~25  | **~54** |

Call-level detail that shapes the plan:

- `plan.habitat` is **7,899 calls/bake at band 5, 19.2 µs/call** (63,189 calls
  over 8 bakes). The native tape evaluation behind it (`channels_at`: one
  `height_at`, one `slope_at` ≈ 4 more field evals, ~12 fbm ops) is a few µs;
  the rest is the per-call JS→C crossing plus 12 `JS_SetPropertyUint32` writes
  per call (`dsl_bindings.cpp:1197-1218`). The boundary, not the math, is the
  cost.
- `plan.select` is 10–16 µs/call × ~6,900 calls/bake at band 5 — interpreted
  `formSuitabilities` + `selectAlpineAsset` + 5–7 `Math.sin`-based
  `identityChannel` hashes per candidate.
- `plan.exclude` is a near-constant ~19.7 ms/bake in every vegetated band (tree
  planning is identical in bands 3–5). The spatial-grid replacement was already
  tried and measured a wash (+1.4/−1.9/−2.9%, see the note at
  `alpine_ecology.js:705-711`); do not retry that shape.
- Scatter (everything but `sector.terrain`) is **~86% of a band-5 bake** and
  ~95% of a band-3 bake. The native terrain mesher is not the problem.
- The **unprofiled ~54 ms of a band-2 bake** (a bake that does terrain only!) is
  fixed per-bake overhead: fresh QuickJS host + evaluating the whole module
  graph (WorldSector imports the 786-line `alpine_ecology.js` unconditionally,
  even for tiles that will never plant) + params JSON + save. With most of the
  6,547-sector disc in vegetation-free bands, this fixed cost is a real fill
  cost. No WP below fully addresses it; it is listed as a follow-up
  measurement (§6, R6).

---

## 2. Line-count inventory (where the ~1,900 lines live)

| file | lines | content |
|---|---:|---|
| `scenes/StreamMountain/StreamMountain.js` | 721 | materials (5), fog/volumetrics/streaming tables, `field()` (~80 code lines), `surfaces()` classifier tape (90 ops), `habitat()` (3 lines), `biomes()`. Roughly 60% comments, most of them load-bearing measurement records. |
| `scenes/StreamMountain/objects/WorldSector.js` | 419 | terrain call + ALL scatter: boulders, legacy grove/rock/grass paths, alpine planner call, per-cell loop, its own 3-function value-noise stack. |
| `shared-lib/alpine_ecology.js` | 786 | catalog/constants (~100), habitat tape builder (~72), **JS `sampleHabitat` fallback + its own noise stack (~120)**, form suitabilities (~47), selection (~110), planners (~180), profiling plumbing (~40). |
| `shared-lib/vegetation.js` | 170 | geometry helpers for the vegetation assets — fine as is, not touched below. |

Duplication multiplier: `WorldSector.js` exists as **seven byte-identical
scene copies** (StreamMountain, StreamMeadow, ChartVtProof, MetalProof, PomProof,
PomProofBrick, TilesetGallery — md5 `d6d891ff…` for all seven) plus the 430-line
template at `projects/world_demo/objects/WorldSector.js` that nothing resolves
to. That is ~3,360 lines carrying 419 lines of information. Only the
StreamMountain copy is in scope here, but see finding F3.

---

## 3. Findings (pre-existing issues, separate from the refactor)

**F1 — BUG (P0): the alpine vegetation modules are unresolvable for
StreamMountain on main.** Today's scene-layout commit (`ebd226d6`) moved
`AlpineConifer/AlpineDeciduous/AlpineShrub/AlpineGrass/AlpineFlower/AlpineGroundCover.js`
from `projects/world_demo/objects/` into `scenes/VegetationGallery/objects/`.
The module search path is exactly `{scenes/<S>/objects, <project>/objects}`
(`local_provider.h:67-74`, `part_graph.h:205-209`) — there is no cross-scene
tier, so a **cold** StreamMountain bake cannot load the source for any of the 92
alpine child variants its `requires()` declares. A warm `.cache` masks this; a
cache wipe or a fresh checkout will fail world fill. Fix: move the six modules
back to the project tier (they are shared by two scenes, which is precisely what
the project tier is for). File move only — content hashes unchanged, no rebake.

**F2 — the authored scene values are not what runs.**
`scenes/StreamMountain/props.json` carries `draw.overrides` that **hide every
vegetation module and Rock** (`AlpineConifer/hide:true`, … `Rock/hide:true`) and
a `stream.lod` group that replaces the authored rings/bands
(`159:2,624:1,4094:0` / bands ending at 2619 vs the file's 10095). Any visual
acceptance run (MATTER_REPLAY) done without resetting these will "verify"
an empty mountain. Likely leftovers from an investigation; worth clearing or at
least documenting before the verification passes below.

**F3 — seven identical WorldSector copies.** The per-scene-copy design landed
today and is taken as given, but no scene has actually diverged yet, and this
repo has been burned by exactly this pattern before (the `surface.c` story in
CLAUDE.md). WP1 makes the StreamMountain copy earn its existence by deleting
the non-alpine half; the same edit in reverse is available to StreamMeadow. The
other five proof scenes could simply resolve to the project tier again — out of
scope, flagged.

**F4 — dead code on StreamMountain's hot path file.** With
`__vegetation.profile = 'alpine-lush'`, `isAlpineProfile` is always true and
`build()` returns at `WorldSector.js:363` — the legacy tree-grove loop
(365-385), grass-clump loop (394-412), the `GROVE`/`TUFT` patch channels, and
`anyBiomeWantsTrees` + the legacy `Grass`/`Tree` variant list in
`assetVariants()` (built, then discarded by `selectVegetationCatalog`) are
unreachable in this scene. Pebbles are doubly dead (commented out of
`assetVariants` *and* the biomes table's `pebbles: 90` is read by nothing that
survives).

**F5 — the JS `sampleHabitat` fallback duplicates the tape without checking
it.** `alpine_ecology.js:296-353` plus its private noise stack (108-137) and
`environmentalDryness` (151-159) exist only for a world with the alpine profile
but no `habitat()` tape — no shipped world. It is *not* a verifiable mirror of
the tape either: the constants deliberately differ by the [0,1]→[−1,1]
transform documented at lines 199-214, and the underlying hashes differ, so it
can never be diffed against the tape the way `candidatesInRectJs` is diffed
against the native grid. It is ~140 lines of pure liability.

**F6 — `identityChannel` (`alpine_ecology.js:139-149`) hashes with
`Math.sin`.** 5–7 calls per surviving candidate, float-sensitive (a C++ port
would inherit libm last-ulp differences), and it quietly carries a string
fallback path nothing uses. A cheap integer hash would be faster and portable —
at the cost of a one-time reshuffle of species/scale/rotation jitter (positions
unchanged). Bundled into WP6 as a precondition, not done casually.

**F7 — `WorldSector.js` has its own third value-noise implementation**
(`hash2/vnoise/patch`, lines 68-90) — distinct from `alpine_ecology`'s
(F5, to be deleted) and from the native tape fbm. After WP1 its only living
consumer is the `SCREE` channel in `scatterRocks`.

---

## 4. Work packages, in order

Common verification tooling (build once, reuse everywhere): add a
placement-list hash to the existing harness — after `planAlpineSector` /
`build()`, fold (module, params, x, z, rot, scale) into an FNV hash and print
it per band. ~20 lines in `sector_scatter_profile.cpp` (test tier, not engine).
WP1 and WP2 then have a *bitwise* acceptance gate; WP3+ report % placements
changed instead of hand-waving. Visual gate for everything: `MATTER_REPLAY`
before/after **with the F2 props overrides cleared**. Every WP edits scene or
shared-lib sources, so each one re-hashes parts and forces a StreamMountain
rebake — batch the WPs into few commits to pay that cost few times.

### WP0 — fix F1 (module move)
- **What:** move the six Alpine* vegetation modules from
  `scenes/VegetationGallery/objects/` to `projects/world_demo/objects/`.
- **Line delta:** 0. **Speedup:** none. **Drift:** none (content-identical →
  same part hashes, no rebake).
- **Verify:** wipe `.cache/StreamMountain`, load the world, confirm fill
  completes; VegetationGallery still resolves (project tier is on its path).
- **Engine purity:** untouched.

### WP1 — delete the dead halves (F4, F5, F7 partially)
- **What:**
  - `scenes/StreamMountain/objects/WorldSector.js`: remove the legacy grove and
    grass loops, `GROVE`/`TUFT` seeds, `anyBiomeWantsTrees`, the legacy
    vegetation list in `assetVariants`, the pebble remnants. `scatterRocks` and
    boulders stay. The file becomes: terrain call + boulders + rocks + the
    alpine planner + the cell loop.
  - `shared-lib/alpine_ecology.js`: delete `sampleHabitat`, its noise stack,
    `environmentalDryness`, and the `habitatAt === undefined` fallback branch of
    `plannedCandidate` (633-646). Make a missing habitat tape a loud failure for
    the alpine profile (`hasHabitat()` check moves from "pick a path" to
    "assert"). Keep `candidatesInRectJs` — its comment is right, it is the spec
    for the native grid.
  - `biomes()` in StreamMountain.js shrinks to what is actually read
    (`__terrain`, `__vegetation`, and `rocks` per biome).
  - Update `tests/alpine_ecology_tests.mjs` (drops the `sampleHabitat` /
    `environmentalDryness` suites; the catalog/selection suites stay).
- **Line delta:** WorldSector 419 → ~230 (−190); alpine_ecology 786 → ~615
  (−170); StreamMountain.js −10. Net **≈ −370 lines**, plus test trims.
- **Speedup:** a few ms/bake of fixed module-eval cost (smaller source to parse
  per fresh host, ×6,547 bakes per cold fill); no per-candidate change.
- **Visual drift:** **none** — every deleted line is unreachable for this
  scene. Gate: placement hash identical per band; replay pixel-identical after
  the forced rebake (the `shot-replay` loop's "engine-side fix reproduces
  pre-fix pixels" precedent applies).
- **Engine purity:** engine untouched.
- **Risk:** StreamMeadow and the five proof scenes import `alpine_ecology.js`
  through their identical WorldSector copies — the deleted fallback must not be
  referenced there. It is only reached via `planAlpineSector`, which only runs
  under the alpine profile, which only StreamMountain sets. Safe, but run
  StreamMeadow's fill once as a check.

### WP2 — batch the habitat boundary (the measured 30%)
- **What:** one new engine binding, shaped like the sanctioned
  `j_candidatesInRect` precedent: **candidate generation and tape evaluation in
  one crossing**. `__planCandidates(seed, kind, minDist, x0, z0, w, h)` →
  returns the candidate fields and, when a habitat tape is bound, the tape's
  channels per candidate, as **flat typed arrays** (one `Float64Array` of
  stride 5+channelCount, or parallel arrays) instead of one JS object per
  candidate. Internally: the existing `sg_cell_candidate`/`sg_survives` loop
  feeding `SurfaceRuntime::channels_at` per surviving candidate. The JS
  planners iterate indices over the flat array; `HABITAT_OUT`/`HABITAT_SCRATCH`
  and the 10-line channel-copy block in `plannedCandidate` disappear (the
  selection code reads channels by `HABITAT.*` index directly).
- **Why this shape:** the tape cannot express "evaluate me at N points" — that
  is the caller's loop, and the loop is where the money is: 19.2 µs/call × 7,899
  calls at band 5, of which the native math is ~3–4 µs. Batching is the same
  move `candidatesInRect` already made, one level up.
- **Line delta:** engine +~90 (`dsl_bindings.cpp`, reusing both existing
  halves); JS −~40 (scratch plumbing, per-candidate `habitatAt` call, the
  separate `plan.candidates`/`plan.habitat` bracketing collapses). Net ≈ +50
  engine / −40 scene tier.
- **Expected speedup (from the measured table):** eliminates the crossing share
  of `plan.habitat` (~15 of 19.2 µs/call) and the per-candidate object
  allocation in both generation and iteration. Band 5: −~120 ms of 509
  (**−20–25% wall**); bands 4/3: **−15–18%**. Conservative because the loop
  self-time (`plan.evaluate`/`plan.otherFamilies`) only partially shrinks.
- **Visual drift:** **none** — same doubles, same evaluation order, same
  float32 tape path (`channels_at`) the per-call binding uses today. Gate:
  placement hash bitwise-identical, per the `candidatesInRect` precedent that
  proved this is achievable.
- **Engine purity check:** the engine learns "evaluate the bound tape at each
  surviving grid candidate and hand back channels by index". Channel *names*
  never cross into C++ (they already don't — `channel_regs` is index-keyed,
  `terrain_field.h:297`). No family, no conifer, no ecology. This is the same
  abstraction level as `channels_at` itself.
- **Fallback story:** keep `candidatesInRect` + per-point `__habitatAt` working
  (contexts without the new binding, and the JS-spec cross-check test extends to
  the batched form: same candidates, same channels).

### WP3 — move the suitability arithmetic into the habitat tape (measure-gated)
- **What:** `formSuitabilities` (`alpine_ecology.js:384-431`) is ~19 products
  of `smoothstep`/`inRange` terms over channels the tape already computes — all
  expressible as tape ops. Emit each form's suitability as an additional
  anonymous channel (12 + 19 = 31; raise `kMaxHabitatChannels` 16 → 32, which
  its own comment (`terrain_field.h:209-233` region) documents as a stack-budget
  constant with no GPU mirror). JS keeps: identity jitter, argmax, the tree
  mixture sampling, acceptance — cheap scalar work over channels WP2 already
  delivered in a flat array.
- **Gate before doing it:** split `plan.select` with two more prof slots
  (`plan.suitability` vs `plan.identity`) and confirm the suitability products,
  not the `Math.sin` hashes, dominate the 10–16 µs. If `identityChannel` is the
  cost, do F6's integer-hash swap instead (bigger win, one-time jitter
  reshuffle). The profiler exists precisely so this fork is decided by data;
  guessing here has been wrong twice already.
- **Line delta:** ≈ 0 (the JS table becomes a tape-builder table of the same
  size). This WP is for goal 2, not goal 1.
- **Expected speedup:** bounded by `plan.select`'s 13.7% of band-5 wall;
  realistically **−5–8%** band 5, less further out.
- **Visual drift:** small but real — float32 tape vs float64 JS flips the
  argmax at near-ties: a small fraction of placements change *species/form*
  (never position). Report the % via the placement-diff tooling; expect <1–2%.
- **Engine purity:** nothing new — more channels on an existing cap. The
  engine still evaluates anonymous registers.

### WP4 — readability pass on what remains
- **What:** with the dead halves gone: (a) `build()` reads as four named
  phases (terrain / boulders / rocks / alpine plan+place); (b) the
  `put`/`putPlanned` pair collapses to one helper (the legacy `put` users are
  gone except rocks); (c) WorldSector's private `hash2/vnoise/patch` trio (F7)
  shrinks to serve only `SCREE`, with a one-line pointer to why it is not the
  tape (placement stability across the tape hash); (d) the six-paragraph
  historical comments in WorldSector that narrate deleted code go with the
  code. Optionally (flagged, drift): replace the `SCREE` patch channel with a
  habitat-tape channel and delete the trio entirely — **medium drift** (rock
  fields re-pattern; density similar), so keep it a separate commit that can be
  dropped.
- **Line delta:** −40 to −70 (more with the SCREE option).
- **Speedup:** negligible. **Drift:** none for the mandatory part.

### WP5 — (optional, decide with the user) template and sibling-copy cleanup
- Delete `projects/world_demo/objects/WorldSector.js` (−430; nothing resolves
  to it — new scenes can copy from a real scene) **or** re-point the five
  non-diverged proof scenes at the project tier and keep exactly one shared
  copy plus StreamMountain's and StreamMeadow's diverged ones. Either direction
  removes >400 lines of pure duplication; the second removes ~2,000 but
  partially reverses today's layout decision, so it needs a human call.
- **Drift:** none for StreamMountain. Rebake cost for whichever proof scenes
  change resolution source.

### WP6 — (contingent) native family planner
- **What:** only if, after WP2/WP3 land and are re-measured, vegetated-band
  bake time still matters: a declarative native planning loop —
  `__planFamily({kind, minDist, cap, gate: {channel, min}, exclusion:
  {radiusChannel|radiusConst, clearance, priorityPurpose}, rect})` returning
  accepted placements. This absorbs `plan.evaluate`/`plan.otherFamilies` loop
  self-time and `plan.exclude` (together ~28% of band-5 wall, ~45% of band-3
  after WP2).
- **Why the tape cannot express it (the justification the constraint
  demands):** the tape is a per-point pure function; candidate iteration,
  priority-ordered mutual exclusion, and caps are inherently cross-point
  control flow. The proposed primitive contains zero domain nouns — channels by
  index, radii, caps — the same way `candidatesInRect` contains none.
- **Preconditions:** F6's integer-hash swap first (a C++ `Math.sin` twin is a
  libm portability trap; with integer hashes, bitwise identity is provable the
  way it was for the grid). Asset *selection* (which module/form) stays in JS —
  the engine returns per-candidate channel data and survivorship; JS maps
  survivors to catalog rows. That keeps the catalog, the mixture sampling, and
  every species name out of C++.
- **Line delta:** engine +~150; JS −~120 (`planTrees` + the family loop).
- **Drift:** none once F6's one-time reshuffle is accepted.
- This WP is deliberately last and deliberately conditional. Do not start here.

---

## 5. Line-count budget (goal 1 scorecard)

| file | today | after WP1–WP4 | delta |
|---|---:|---:|---:|
| StreamMountain.js | 721 | ~700 | −20 (biomes + dead-path comments) |
| WorldSector.js (SM copy) | 419 | ~200 | −220 |
| alpine_ecology.js | 786 | ~600 | −185 (−170 WP1, scratch plumbing WP2, ±0 WP3) |
| engine (`dsl_bindings.cpp`) | — | +90 | +90 (WP2) |
| **scene+ecology total** | **1,926** | **~1,500** | **≈ −425 (−22%)** |
| template WorldSector (WP5a) | 430 | 0 | −430 |
| sibling copies (WP5b, out of scope) | ~2,100 | — | up to −2,000 |

## 6. Risks

- **R1 — every WP forces a full StreamMountain rebake** (source → part hash).
  Batch commits; budget the disc-fill time; use the replay loop for
  before/after rather than eyeballing.
- **R2 — F2's props overrides will silently fake any visual verification.**
  Clear `draw.overrides` and `stream.lod` in
  `scenes/StreamMountain/props.json` (or verify with a props-less profile)
  before trusting a replay diff.
- **R3 — the seven identical WorldSector copies**: WP1's edits apply to
  StreamMountain's copy only; the other six keep the legacy+alpine union until
  WP5 is decided. Any shared-lib change (WP1's ecology deletions, WP2's new
  binding) must keep those copies loading — they do (they never enter the
  alpine path), but run one non-alpine scene's fill as a regression check.
- **R4 — WP2's typed-array return changes the `candidatesInRect` consumer
  shape** in the boulder loop too if unified carelessly; boulders use the
  object form at 0.13 candidates/cell — leave them on the old binding, it is
  measured at 0.0% of the bake.
- **R5 — float32 vs float64 in WP3** is a real (small) drift; report it as a
  placement-diff percentage, don't assert "identical".
- **R6 — the fixed ~54 ms/bake overhead** on vegetation-free tiles (band-2 row)
  is untouched by this plan and is plausibly the larger cold-fill lever
  (thousands of far tiles × ~54 ms). Separate investigation: host/bytecode
  reuse across bakes, or slimming the module graph far bakes evaluate. Noted so
  this plan's savings are not oversold against total fill time —
  per the sector-bake memory, stage_load's LOD ladder, not scatter, dominates
  the whole-disc fill.

## 7. Explicitly rejected / not worth doing

- **Spatial-grid tree exclusion**: already implemented, proven
  bitwise-identical, measured a wash, reverted (`alpine_ecology.js:705-711`).
  The plan keeps the O(viable²) loop until WP6 absorbs it natively.
- **Optimizing `sector.terrain`**: 12–14% of a near bake, native already, and
  the LOD ladder work owns it.
- **Touching the `surfaces()` classifier tape**: it is GPU/page-bake cost, not
  sector-bake cost, its 90/96 ops are documented op-by-op, and it is the best
  code in the scene. Leave it alone.
- **Porting `alpineHabitat`'s channel *names* or the catalog into C++**: would
  violate the purity constraint for zero measured benefit; the tape boundary is
  in the right place, it is the *crossing frequency* that was wrong (WP2).

## 8. Reproduction

```
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -C MatterEngine3 TMP=... TEMP=...
make -C MatterEngine3/tests run-scatterprof TMP=... TEMP=... GRAPHICS=GRAPHICS_API_OPENGL_43
```
(TEMP/TMP per CLAUDE.md; raw output from this run is summarized in §1.)
