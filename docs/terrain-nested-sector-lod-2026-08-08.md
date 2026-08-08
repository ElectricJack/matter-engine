# Nested sector LOD: doubling sector size as resolution halves

Design note — 2026-08-08
Scope: `MatterEngine3/src/sector_streamer.{h,cpp}`, `MatterEngine3/src/terrain_mesher.{h,cpp}`,
the streaming half of `matter_engine.cpp`, and `projects/world_demo/objects/WorldSector.js`.
Documentation only; no code changed by this note. Implementation plan:
`docs/superpowers/plans/2026-08-08-nested-sector-lod-migration.md`.

## Problem: the disc pays a fixed per-part cost O(R²) times

Terrain streams as a uniform grid of fixed-size sectors — `sectorSize: 64`
(`projects/world_demo/worlds/StreamMountain.js:82`). LOD is expressed *inside* a
sector by coarsening the voxel (`terrain_mesher.h:45-51`: `rung 0 → 2 m` down to
`rung -5 → 64 m`, one cell for the whole sector — the ladder floor is exactly
"one cell left to march", `terrain_mesher.cpp:65-66`), never by making the
sector bigger. So triangle count per sector falls with distance, but the count
of *sectors* grows as O(R²), and every one of them is a full part:

- a bake job on the worker pool: a fresh `ScriptHost` + a JS `build()` +
  `save_v2` (`matter_engine.cpp:4258-4285`);
- a staging pass: artifact decode or in-memory adoption, QEM terrain ladder,
  chart tables, warp-field solve (`matter_engine.cpp:4439-4470`,
  `render/part_store.cpp:1097-1210`);
- an app-thread publish: coordinator reservation, ledger insert, `WorldDelta`
  apply, tracer reset, Vulkan part registration (`matter_engine.cpp:4655-4790`);
- a `WorldManifestEntry` instance, a BLAS registration per rung, a TLAS/RT
  instance, chart/VT registration, and residency + eviction bookkeeping.

Counted exactly (script over the authored bands; sector desired iff its centre
is within the ring/band radius, matching `desired_rung_for_dist` /
`desired_lod_for_dist`, `sector_streamer.cpp:66-78`), StreamMountain's
resident disc at the authored 10,095 m reach is:

| band radius (m) | terrain LOD | voxel | cells/sector | sectors |
|---|---|---|---|---|
| ≤ 318 | 5 | 2 m | 32×32 | 80 |
| 318–1,186 | 4 | 4 m | 16×16 | 996 |
| 1,186–2,605 | 3 | 8 m | 8×8 | 4,144 |
| 2,605–4,702 | 2 | 16 m | 4×4 | 11,716 |
| 4,702–7,753 | 1 | 32 m | 2×2 | 29,192 |
| 7,753–10,095 | 0 | 64 m | 1×1 | 32,040 |

**Total: 78,168 resident sectors, 78.3% of them in the two coarsest bands
producing 1–4 quads each.** (The problem statement's estimates — 78,160 total,
78% coarse — were correct to within rounding.) The prior art on this failure
axis: the pre-tuning disc was 6,547 sectors at ~2.9 km and filled in 48 s with
a marginal far-sector cost of 31 ms (`matter_engine.cpp:378-385`,
`docs/sector-bake-time-findings-2026-07-30.md`); pushing the ring out without
the ladder OOMed at 45 GB. The 10 km table above is ~12× the 6,547-sector disc
— the same machine, run twelve times as many times.

The fixed cost is not only per-publish. `SectorStreamer::update()` runs once
per tick and scans the full anchor square at 64 m granularity —
`(2·10,111/64)² ≈ 99,856` distance evaluations (`sector_streamer.cpp:164-198`)
— then iterates the ~78k-entry `sectors_` map three more times in
`assign_terrain_lods()` with four neighbour lookups each
(`sector_streamer.cpp:84-153`). That is derived from the code, not measured;
the measured symptoms it predicts (publish-bound fills, O(world) per-tick
costs) are the subject of two prior findings docs.

## The shape of the fix

**Nested sectors: level L has sector size `S_L = 64·2^L` and native voxel
`v_L = 2·2^L`, so cells-per-sector is constant (32×32) and each level's
annulus holds a roughly constant sector count.** Every level's grid is a
power-of-two subdivision anchored at the world origin, so a level-L sector
nests exactly inside a level-(L+1) sector — "everything is a division of the
largest sector" falls out of the anchoring, it is not a separate constraint to
enforce.

Counted the same way (level-L tile resident iff its centre lies in the level's
annulus; annuli from the same authored radii):

| level | sector | voxel | cells | sectors (centre rule) | sectors (coverage rule) |
|---|---|---|---|---|---|
| 0 | 64 m | 2 m | 32×32 | 80 | 88 |
| 1 | 128 m | 4 m | 32×32 | 252 | 304 |
| 2 | 256 m | 8 m | 32×32 | 264 | 316 |
| 3 | 512 m | 16 m | 32×32 | 176 | 240 |
| 4 | 1,024 m | 32 m | 32×32 | 112 | 156 |
| 5 | 2,048 m | 64 m | 32×32 | 32 | 56 |
| | | | **total** | **916** | **1,160** |

The centre rule is the direct analogue of today's band lookup; the coverage
rule ("tile intersects the annulus and is not fully covered by finer levels")
is what a hole-free quadtree actually resides, and is the honest number:
**~1,100–1,200 sectors instead of 78,168 — 65–85× fewer parts for the same
triangles and the same reach.** (The problem statement's 915 matches the
centre rule; the real resident set is the coverage-rule one.)

Triangle totals are unchanged by construction: a level-L tile at 32×32 cells
emits the same surface the 4^L level-0 tiles under it would emit at the same
voxel. What changes is how many times the per-part machine runs, and the
streamer's own bookkeeping: the desired-set scan becomes a quadtree descent
visiting ~2–5k nodes instead of ~100k grid cells.

### Why distance-banded levels, not the alternatives

**Geometry clipmaps (toroidal per-level rings) — rejected.** Clipmaps update a
persistent per-level ring buffer in place as the camera moves. Everything in
this engine is built on the opposite invariant: a sector is an immutable,
content-addressed part (`br.resolved_hash`, `matter_engine.cpp:4339`) that is
baked once, published atomically through the coordinator transaction machinery
(`streaming/sector_streaming_coordinator.h:135-159`), and evicted whole.
Clipmap-style incremental updates would bypass the part store, the BLAS/TLAS
registration, the chart/VT pipeline, and the publish/evict transaction — i.e.
they demand a dedicated terrain renderer beside the one we have. The nested
grid gets the same asymptotics (constant sectors per annulus) using the
existing machine.

**Error-adaptive sparse quadtree (refine where the field is detailed) —
rejected for this migration.** The field is statistically uniform fbm; there
are no large flats that would let adaptive depth pay. Per-part draw-time error
control already exists (the in-part QEM ladder, `lod_bake.h:260-288`, selected
by `render/lod_distance.h`). Distance-banded levels give the thing we are
actually short of — bounded part count — with a keyspace that stays a
per-level uniform grid. Adaptive depth can be added later as a refinement rule
on the same keyspace (it is a strict superset), so nothing is foreclosed.

**Keep the uniform grid, batch the far sectors (mega-parts merging N bakes) —
rejected.** Merging publishes without merging bakes keeps 78k JS invocations
and 78k streamer entries; merging bakes too is this design with a worse
keyspace (merge groups that don't nest, no natural edge-mask story).

## Corrections to the problem statement, from the code

Established by reading, and load-bearing for the design:

1. **The vertical-extent problem (hard problem 7) does not exist.**
   `mesh_sector` does *not* slab the full `yMin..yMax`: it evaluates
   `height_at` once per X/Z lattice column, takes the sector's `h_min/h_max`,
   and builds the density lattice only over that range ±2 voxels
   (`terrain_mesher.cpp:86-108,154-161`). A level-0 sector is 35×35 height
   evaluations plus a slab a few voxels thick around the local relief — not
   32×32×400 samples. The mesher is surface nets over the height-derived
   density `h(x,z) − y` (`terrain_mesher.cpp:170-173`); the 3D field is
   touched only for gradient normals. Consequence: near levels are already
   cheap in Y, per-tile mesh cost is ~constant across levels (35² height
   evals + surface cells), and this migration needs no Y-bounding work.
2. **The placement transform site is `matter_engine.cpp:4767-4770`**
   (`instance.transform[3/11] = tx/tz * sector_size`); 4587-4589 is the
   VT-classifier translation for the worker prebuild, and 3458/3551 are the
   same translation in the reclassify/demand paths. All four must become
   per-level; they are enumerated in §"sector size becomes per-request" below.
3. **Sector parts are transient, not durably cached.** WorldSector is marked
   transient (`matter_engine.cpp:3158`) and its artifacts go to
   `%TEMP%/matter_transient/<pid>` — a per-process directory
   (`provider/local_provider.cpp:1414-1428`). Every session already cold-bakes
   the whole disc. This defuses most of hard problem 9 (below).
4. **`assign_terrain_lods` order**: band lookup with demotion hysteresis
   (pass 1, `sector_streamer.cpp:86-108`), 2:1 cardinal balance to a monotone
   fixpoint (pass 2, :110-133), coarser-neighbour edge masks + repack
   (pass 3, :135-152) — as stated. One addition: the packed variant, edge mask
   included, is part of the sector's *identity* (`SectorKey{tx,tz,rung}`,
   `matter_engine.cpp:1120-1123`; `same_publication_tag`,
   `matter_engine.cpp:398-407`), so a neighbour's LOD change that only flips
   this sector's edge mask already forces a full rebake + republish of this
   sector today. Nesting inherits that behaviour and must not make it worse
   (see §N:1 transitions).
5. **In StreamMountain specifically, almost all scatter is already
   world-keyed.** The alpine profile is active
   (`StreamMountain.js:671` `__vegetation: alpine-lush`;
   `WorldSector.js:220-227`), and every alpine vegetation placement flows
   through `candidatesInRect` + `placementIdentity(worldSeed, kind, x, z, …)`
   (`shared-lib/alpine_ecology.js:396-424,480-512`;
   `MatterEngine3/shared-lib/scatter_grid.js`). The per-sector RNG
   (`WorldSector.js:171-172`) feeds only `scatterRocks()`
   (`WorldSector.js:207-218`, gated `p.rung >= 1`, i.e. ≤ 500 m) and the
   `put()` rotation it uses. The instability surface of hard problem 2 is
   therefore: rocks, plus the per-sector *caps* and cap-truncation order
   (`FAMILY_CAPS`, `alpine_ecology.js:3-5`, applied per sector at
   :475,:502), plus the legacy non-alpine path (grass/rocks via `inSector()`).

## The design

### Levels, and how they map onto what exists

- Level L ∈ [0, 5]. `S_L = S_0 · 2^L` with `S_0` = the world's authored
  `sectorSize` (64 for StreamMountain). Native voxel `v_L = 2·2^L`, i.e.
  **mesher rung `−L`** — inside the existing `-5..3` rung range
  (`terrain_mesher.cpp:67-70`). A level-L tile meshes as
  `mesh_sector(field, tx, tz, /*rung=*/−L, edge_mask, /*sector_size=*/S_L, …)`
  with `tx,tz` in level-L tile units. **The mesher needs no changes**: origin
  `ox = tx·S_L` and `n = S_L/v_L = 32` follow from its existing arguments
  (`terrain_mesher.cpp:82-84`).
- **Level ≡ 5 − terrain LOD.** The existing packed variant already carries a
  3-bit terrain LOD (`sector_streamer.h:26-33`), and in nested mode the tile's
  size is a pure function of it: `S = S_0 << (5 − variant_terrain_lod(v))`.
  **No new variant bits are needed.** Bits 12-14 are reserved for the day
  level and voxel rung decouple (they do not in this design: cells-per-sector
  constant *is* the design). A bare legacy value (< 16) still decodes as
  terrain LOD 5 → level 0 → size S_0, so the self-identifying encoding
  (`kVariantMarker`, bit 11) keeps working unconditionally.
- Authoring: the existing `streaming.terrainBands` table
  (`StreamMountain.js:147-154`), already validated as increasing radii with
  consecutive descending LODs 5..0
  (`script/world_definition_loader.cpp:1176-1230`), is reinterpreted under a
  new `streaming.nestedSectors: true` flag: band with LOD ℓ = the annulus
  where level (5−ℓ) tiles live. Same table, same tuning UI
  (`WorldSession::streaming_lod_config`, `matter_engine.cpp:7934-7949`), one
  new boolean. With the flag off (default), everything runs exactly today's
  uniform-grid path — that is the rollback position.
- Residency bound: today the outermost *scatter ring* bounds residency
  (`StreamMountain.js:119-131`; `desired_rung_for_dist` returns −1 past it,
  `sector_streamer.cpp:66-70`). In nested mode the outermost *terrain band*
  bounds residency, and rings assign scatter tiers only. This removes the
  authored-agreement trap the StreamMountain comment documents (the world
  "simply stopped" at the outer ring no matter what the bands said).

### The desired set: quadtree descent with a restriction pass

Replaces the flat scan in `update()` (`sector_streamer.cpp:164-198`) when
nested mode is on:

1. **Descent.** Walk the level-5 grid over the disc bounding square. For each
   tile: if the tile lies entirely at level-5 distance (its *nearest point* to
   the anchor is beyond the level-4 band radius), it is desired at level 5;
   otherwise split into its four level-4 children and recurse; at level 0,
   stop. "Distance" is the anchor-to-tile-nearest-point distance for the
   split test and anchor-to-centre for band membership, which makes the
   desired set exactly a coverage-rule quadtree: **every world column inside
   the reach is covered by exactly one desired tile** — no holes, no
   double-cover, by construction. Cost: O(resident count), ~1-5k nodes.
2. **Hysteresis.** Today's demotion hysteresis (resident level held until the
   distance exceeds the band radius + hysteresis, pass 1 of
   `assign_terrain_lods`) becomes *merge* hysteresis: four resident level-L
   children are not merged to their level-(L+1) parent until the parent's
   nearest point exceeds the level-L band + hysteresis. Split (promotion) has
   no hysteresis, mirroring the scatter rule (`sector_streamer.cpp:225-235`).
   Hysteresis operates on whole sibling quads, not single tiles — a tile
   cannot be half-merged.
3. **Restriction (the re-derived balance pass).** Pass 2's invariant becomes:
   cardinal-adjacent desired tiles differ by at most one *level*. Adjacency in
   the multi-grid keyspace: tile (L, tx, tz)'s +x edge abuts either one tile
   at level L (tx+1, tz), one at level L+1 (⌊(tx+1)/2⌋, ⌊tz/2⌋ — arithmetic
   shift, exact for negatives), or two at level L−1 (2(tx+1), 2tz) and
   (2(tx+1), 2tz+1). The fix-up is the same monotone relaxation as today
   ("split the coarser side"), and it terminates for the same reason (levels
   bounded, splits monotone). With the default band table the pass is a no-op
   by construction — every band is wider than one tile of its coarser
   neighbour level (narrowest margin: level-5 band 7,753→10,095 = 2,342 m >
   2,048 m) — but it must exist because bands are authorable and hysteresis
   can locally hold a stale level, exactly why pass 2 exists today
   (`sector_streamer.cpp:41-47` "the explicit balance pass still guards
   custom profiles").
4. **Edge masks.** Pass 3 unchanged in meaning: bit n set iff the cardinal
   neighbour on side n is exactly one level coarser. Under nesting a tile's
   whole edge abuts exactly *one* coarser tile (its edge is half of the
   coarser tile's edge — grids nest), so four bits still suffice; the
   half-edge generalisation is not needed. Proof of crack-freeness below.

Internal keyspace: `sectors_` keys become
`(level << 60) | ((tx & 0x3FFFFFFF) << 30) | (tz & 0x3FFFFFFF)` — ±2^29 tiles
per axis at level 0 (±34,000 km at 64 m) — replacing the two-axis pack at
`sector_streamer.h:124-126`. External identity needs no new fields:
`SectorRequest{tx, tz, rung}` already disambiguates level via the packed
variant's terrain-LOD bits, so `SectorKey` (`matter_engine.cpp:1120-1123`),
`sector_instance_id` (`:1180-1190`), `same_publication_tag` (`:398-407`), and
the whole coordinator (`streaming/sector_streaming_coordinator.h:24-36`, which
copies `SectorRequest` opaquely) work **unchanged**. Two tiles at different
levels can share numeric (tx, tz) but never a packed variant.

### Crack-free across differing sector sizes: the proof

Claim: with 2:1 restriction and the existing edge-mask snap, any pair of
edge-adjacent tiles meets watertight, at any levels L and L+1.

Preconditions the existing machinery relies on (all cited from the mesher):

- **World-anchored sample lattices.** A level-L tile samples heights at
  `ox + (i−1)·v_L` with `ox = tx·S_L = tx·32·v_L` (`terrain_mesher.cpp:82-84,
  96-100`) — every sample lies on the global lattice of multiples of `v_L`.
  This is the same property that makes today's unequal-rung pairs on the
  64 m grid stitchable, and it is preserved because all levels anchor at the
  world origin.
- **2:1 sample nesting.** `v_{L+1} = 2·v_L`, so the coarse tile's boundary
  samples coincide exactly (identical `height_at` world arguments, computed
  through the same `double` lattice expression → identical floats) with the
  fine tile's *even* boundary samples.
- **Corner coincidence.** A fine tile's corners sit at multiples of `S_L =
  16·v_{L+1}` — lattice points of the coarse grid. The midpoint of a coarse
  edge (where two fine tiles meet along it) is the coarse tile's sample 16 of
  32 — a real coarse sample, and both fine tiles' shared corner sample.

Given these, the existing cross-rung closure argument
(`terrain_mesher.cpp:110-147`) carries verbatim: on a masked face the fine
side replaces each odd boundary sample with the average of its even
neighbours, so the fine boundary polyline *is* the coarse side's linear
interpolation between its own samples — agreement by construction. The only
geometrically new situation under nesting is that the coarse edge spans two
fine tiles; but the coarse side's boundary depends only on its own samples,
and each fine tile independently conforms to the sub-segment of the coarse
polyline it abuts, with the shared fine-fine corner pinned to a common coarse
sample. Equal-level neighbours (including two siblings) are the existing
equal-rung case: identical world samples → bitwise-identical border cell rows
under the `[1..n]` ownership rule (`terrain_mesher.cpp:232-239`).

Corners where levels differ diagonally by two: possible today (pass 2 balances
cardinals only) and possible under nesting; safe in both for the same reason —
corner samples lie at sector corners, which are lattice points of *every*
level involved, so all meshes pass through the same point there.

The restriction pass guarantees the "exactly one level coarser" premise; the
mesher rejects nothing new (rung −L ∈ [−5, 0] for L ∈ [0, 5]). One test is
owed: `terrain_mesher_tests.cpp` closes seams for unequal rungs on an equal
grid; a new case must close them for a (rung −L, S_L) tile against a
(rung −L−1, S_{L+1}) neighbour, including the half-edge and the three-tile
corner. That is the acceptance gate for this section, not the prose.

### Scatter: carried by the tile, computed on a fixed 64 m virtual cell

This is the biggest structural decision (hard problem 1), and the chosen
answer is: **scatter stays a property of the terrain part (no separate scatter
grid, no separate part kind), but `WorldSector.build()` computes it per fixed
64 m *virtual cell*, iterating the `4^L` cells a level-L tile covers.** For
each cell: derive the cell's scatter tier from *its own* centre distance…
no — from the tile's baked tier (see below); seed the cell RNG from the cell
coordinates exactly as today
(`rng(seed ^ imul(cellTx,73856093) ^ imul(cellTz,19349663))`,
`WorldSector.js:171-172`); run today's per-sector scatter body over the cell
rect; apply per-cell caps.

Why this wins over the alternatives:

- **Placement stability is bitwise, not approximate.** The candidate grids
  were already world-keyed; the cell-RNG stream, the per-cell cap truncation
  order, and the cell-rect iteration are now *also* invariant across level
  transitions, because the virtual cell grid never changes. A rock placed by
  the level-0 bake is the same rock the level-2 bake places. Without this,
  every `r`-driven placement (rocks ≤ 500 m — exactly the range where a
  reshuffle is most visible) would teleport when its carrier tile crossed the
  318 m level boundary.
- **Cost stays linear in area.** `planTrees` runs an O(viable²) exclusion
  loop per invocation (`alpine_ecology.js:456-476`). Per 64 m cell, `viable`
  is tens; over a whole 2,048 m tile it would be thousands, and the loop
  quadratic in that. Sub-celling reproduces today's per-sector candidate sets
  *and* today's cost profile — the padded-rect neighbourhood
  (`TREE_NEIGHBOR_PADDING = 16`, :394) is per-cell, exactly as it is per-sector
  today, so cross-cell exclusion semantics are also unchanged.
- **Caps keep their meaning.** `FAMILY_CAPS` are per-64 m-sector densities
  today; per-cell application preserves them. Applying them per-tile would
  cut far-field density 4^L-fold.
- **Rejected: a separate fixed 64 m scatter-part grid.** It keeps O(R²) part
  count for the tier-0 reach (trees stream to 10,095 m,
  `StreamMountain.js:131-134`) — 32k+ scatter parts is the disease this
  design exists to cure. Scatter *instances* at that reach are today's
  authored behaviour and today's cost; what must not survive is a *part* per
  64 m cell.
- **Rejected: confine scatter to levels 0-1, impostors beyond.** Changes what
  the user sees (trees vanish beyond ~1.2 km); the impostor programme is a
  separate effort (`feature/representation`) and this migration is required
  to be visually conservative.
- **Rejected: per-level density properties.** Reshuffles placements and adds
  an authoring axis nobody asked for.

Tier granularity: the scatter tier remains per-*request* (baked into
`p.rung`, `matter_engine.cpp:4250`), assigned from the tile centre's ring
lookup as today. The dense tiers (2 at ≤150 m, 1 at ≤500 m) live entirely
inside levels 0-1 (bands 318/1,186 m), so tier boundaries quantize at 64 or
128 m instead of 64 m — a one-tile-wide difference in where grass begins,
tunable by aligning ring radii to level-1 tile boundaries. A tier change
rebakes the tile, exactly as it rebakes a sector today (identity includes the
variant). Placements are tier-stable for the same reason they are today:
candidates and patch channels depend only on worldSeed + position
(`WorldSector.js:18-22`), and the cell-RNG consumption order per tier is
unchanged code.

What must still change in JS (hard problem 2's residue): `scatterRocks`'s
attempt count is `counts.rocks * 3` per cell (unchanged), but the legacy
non-alpine path (Meadow-family worlds) does its grass the same `r`-driven way
— those worlds either stay on the uniform grid (the flag is per-world) or
accept the same sub-celling. `WorldSector.js:24`'s `const SECTOR = 64.0`
becomes the *cell* size constant, and the tile size arrives as a new param
(next section). `planAlpineSector` is called per cell with `sectorSize: 64`,
unchanged; `FAMILY_CAPS` stay per-cell.

### Sector size becomes per-request (hard problem 6)

Size is a world scalar today; the trace of every consumer, and the change at
each:

| site | today | change |
|---|---|---|
| `script_host.h:134` / `script_host.cpp:2663-2667` | `WorldEvalResult.sector_size` from `static world.sectorSize` | unchanged — this is `S_0`, the level-0 size |
| `provider/local_provider.h:186-196` | `ProceduralWorldProfile.sector_size`, `apply()` to bindings | unchanged (`S_0`) |
| `dsl_state.h:25` | `WorldBinding.sector_size` → `terrainVolume` mesher call (`dsl_bindings.cpp:1241-1242`) | set **per bake**: each streamed request builds its own `BakeOptions` + `ScriptHost` (`matter_engine.cpp:4258-4268`), so after `world_profile.apply(opts.world)` the job overrides `opts.world.sector_size = S_req`. No shared state touched. |
| `matter_engine.cpp:1193` `world_sector_size` | the one grid pitch | stays `S_0`; every *use* below derives `S_req = S_0 << (5 − variant_terrain_lod(req.rung))` |
| bake-request JSON, `matter_engine.cpp:4246-4254` | `tx, tz, rung, terrainLod, edgeMask, …` | adds `"sectorSize": S_req`. `WorldSector.build()` reads `p.sectorSize` (default 64) for `ox/oz` and the cell loop; `terrainLod → voxelRung` mapping (`WorldSector.js:160-162`) already yields rung −L unchanged. |
| warp anchor, `matter_engine.cpp:4436-4438` → `part_store.h:206-210` → `warp_field.cpp:185` | `tx·S, sector_size = S` | `tx·S_req`, `S_req` — the WarpAnchor is *already the per-part size channel*; this design reuses it rather than inventing one. |
| placement transform, `matter_engine.cpp:4767-4770` | `tx·sector_size` | `tx·S_req` |
| prebuild classifier translation, `matter_engine.cpp:4586-4589` | `tx·sector_size` | `tx·S_req` |
| reclassify / VT-demand translations, `matter_engine.cpp:3458-3460, 3551-3552` | `kv.first.tx · world_sector_size` | the `SectorKey` carries the packed variant in `.rung` — decode the level from it; both sites become `tx · (S_0 << level(key.rung))` |
| chart density, `part_store.cpp:1082` | `texels_per_meter = 16` for every part | for terrain sectors, `16 / 2^level` (derived from `warp.sector_size / S_0`), so texels-per-*tile* stays constant. See hard problem 8 below. |
| `SectorLodResolver`, `sector_resolver.h:55-79`, constructed `{16.0f, 64.0f}` at `matter_engine.cpp:582` | bins world entries by its own 16 m pitch | **unchanged.** Its pitch is an internal binning granularity unrelated to streaming sector size, its radius is overwritten from `opts.active_radius` every render (comment at :580-582), and StreamMountain runs `PassThrough` anyway (`sector_resolver.h:41-44`). |
| `make_streaming_profile` sector-scaled defaults, `matter_engine.cpp:315-324`, `sector_streamer.cpp:34-48` | rings/bands scaled by `S` | scaled by `S_0`, unchanged meaning |

### N:1 / 1:N transitions (hard problem 4)

Today a level change swaps one sector: `on_published` accepts the new variant
and queues the old one's eviction — publish-then-evict, no hole
(`sector_streamer.cpp:319-343`), with the eviction applied on the app thread
possibly a frame or two later (`apply_sector_evictions`,
`matter_engine.cpp:3806-3840`), so a brief same-footprint double-draw already
exists and is accepted. Under nesting, a split is 4 publishes + 1 evict and a
merge is 1 publish + 4 evicts, and the interim is no longer brief: four
independent bakes complete seconds apart, and a fine tile drawn over its
still-resident coarse parent z-fights over its whole footprint for that long.

Design: **transition groups with a batched commit.**

- The streamer tracks a *group* per split/merge: the set of tiles being
  published and the set to evict, created when the desired map first diverges
  from residency across a parent/child boundary. `next_request` serves group
  members as ordinary requests (nearest-first unchanged,
  `sector_streamer.cpp:273-313`).
- The publish job for a group member runs *prepare* exactly as today (bake,
  stage, `commit_staged` into the part store, Vulkan part build/registration)
  but **defers the `WorldDelta` add**. Prepared members park in the group.
- When the last member prepares, one app-thread job applies a single
  `WorldDelta` containing all adds and all removes
  (`viewer::WorldDelta`, applied at `matter_engine.cpp:4772-4775` — the
  delta type already batches; today's callers just happen to pass one entry).
  One `state.apply` + one tracer reset + one ledger update: the swap is
  atomic at frame granularity. No hole, no double-draw.
- **Partial failure**: a member bake failure marks the group cooling
  (per-sector `on_failed` cooldown, `sector_streamer.cpp:349-359`, lifted to
  the group); the coarse parent stays resident and drawn; prepared siblings
  stay parked (they hold store/Vulkan resources but no manifest entry) until
  retry succeeds or the group is abandoned (anchor moved on → park resources
  released through the existing rollback path,
  `PublicationTransaction`, `sector_streaming_coordinator.h:135-159`). The
  invariant extends cleanly: *the old residency is torn down only in the same
  delta that establishes the complete new residency.*
- Merges are the mirror: one coarse bake prepares, commit removes four
  children in the same delta.
- Edge-mask-only rebakes (identity change without level change) remain 1:1
  swaps and keep today's path untouched.

Cost note: parked fine tiles hold GPU memory before they draw. Group size is
bounded (4 publishes), and groups in flight are bounded by `max_inflight`
(64, `matter_engine.cpp:356`), so the parking overhead is bounded by the same
Little's-law envelope that governs inflight publishes today.

### Keyspace, packing, and consumers (hard problem 5)

Summarised from the sections above, with the consumer census:

- Variant packing **unchanged** (bits 0-3 scatter, 4-6 terrain LOD, 7-10 edge
  mask, 11 marker; `sector_streamer.h:25-33`); nested mode adds only the
  *interpretation* `level = 5 − terrain_lod`. Legacy bare rungs decode to
  level 0. Bits 12+ reserved.
- Consumers of the packed value, verified exhaustive by grep:
  - decode sites: bake-JSON build (`matter_engine.cpp:4250-4252`), bake-profile
    log (`:4296`), first-rung policy (`:4423` — reads scatter bits only,
    unaffected), streamer internals (`sector_streamer.cpp:92,97,150,219`).
  - opaque identity sites: `SectorKey`/`sector_map`
    (`matter_engine.cpp:1120-1132`), `sector_instance_id` (`:1180-1190`),
    `same_publication_tag` (`:398-407`), the coordinator's tagged
    request/eviction structs and every vector of them
    (`sector_streaming_coordinator.h:24-36,219-223`), the streamer tests.
    All of these keep working because equality-is-identity is preserved and
    level is inside the compared value.
  - the two `SectorKey → world translation` sites (`:3458`, `:3551`) are the
    only opaque consumers that must *start* decoding (level → size).
- Streamer-internal grid key gains level bits (internal only,
  `sector_streamer.h:122-130`).

### Downstream footprint assumptions (hard problem 8)

- **Culling.** A staged terrain part is one synthetic cluster whose AABB is
  the whole tile (`part_store.cpp:1146-1181`), so the cull unit grows to
  2,048 m at level 5. At 8-10 km a level-5 tile subtends a few degrees and is
  almost always wholly in or out of the frustum; the loss is marginal
  occlusion granularity on the horizon, paid for by 65-85× fewer cull
  records. Accepted; if it ever shows, the mesher can emit 4 clusters per
  tile without touching anything else (clusters are per-part data).
- **RT/TLAS.** One TLAS instance per resident sector today ⇒ instance count
  drops with part count. This compounds with the TLAS-rebuild findings
  (`docs/rt-tlas-cpu-mirror-redesign-2026-08-07.md`): both the per-frame CPU
  mirror walk and the GPU build scale with instances × clusters.
- **VT / charts.** Density policy today: terrain 16 t/m at rung 0 halving per
  coarser rung, keyed to the true rung index
  (`part_store.cpp:1078-1088`, `lod_bake.h:302-305`). Left alone, a level-5
  tile would want 2,048 m × 16 t/m ≈ 32k texels across — far past the atlas
  and the 2,048-layer array cap on the 4090. The fix is one line of policy:
  scale the terrain base density by `2^{−level}` (derived at the
  `chart_opts` site from `warp.sector_size`), so texels-per-tile is constant
  and texels-per-metre matches the voxel — the same ratio every level-0
  sector has today. Texel density at a given *distance* is then unchanged
  from today's per-rung halving ladder, because level replaces exactly the
  rungs it displaces.
- **Detail tilesets / classifier.** The classifier tape samples world-frame
  noise (`StreamMountain.js:373-375`) and the tape's world inputs are legal
  because sector variants are world-anchored one-instance parts
  (`matter_engine.cpp:4546-4549`) — both properties survive (a tile is still
  a unique world-anchored variant). Detail tilesets are triplanar-sampled in
  world space at page-bake time, so their tiling is size-independent. The
  per-level translation plumbing is the only change (table above).
- **Residency budgets.** VT page residency and the request budget
  (`service_vt_rung_requests`, `matter_engine.cpp:3521-3578`) are keyed by
  (part, rung) and sized in pages; fewer, larger parts means fewer, larger
  registrations under the same page budget. No structural change; watch the
  per-registration cost spike (a level-5 registration touches more pages at
  once) in the visual acceptance pass.

### Interaction with the in-part rung ladder (hard problem 10)

Each terrain part keeps its own 3-rung QEM ladder
(`lod_bake::bake_terrain_lods`, `lod_bake.h:315`, targets
`eps_ratio {0, 0.004, 0.012}` / thresholds `{0.20, 0.05, 0.0125}`,
`lod_bake.h:278-279`), selected at draw time by the single distance rule
(`render/lod_distance.h`). The two ladders compose without double-LODing
because they are different axes: the streaming *level* fixes the surface's
sample rate (which mesh exists); the in-part *rung* trades triangles within
that surface while the tile is resident (which mesh *draws*). `reach` scales
with `bound_radius` (`lod_distance.h:103-106`), which doubles per level, so
switch distances scale with tile size automatically — a level-L tile's rung
ladder covers its residency annulus the same way a 64 m sector's covers its
band today. No second projected-size comparison is introduced anywhere: level
selection is a *streamer-side radius band* (the mechanism that already
exists), not a draw-time size test, and draw-time selection remains
exclusively `lod_distance.h` (`run-lod-distance` must stay green and
untouched).

Should the in-part ladder shrink now that levels carry the coarsening? Not in
this migration. The lesson at `matter_engine.cpp:4403-4418` is explicit:
changing which rungs a sector bakes changes its staged cluster set and
therefore its `bound_radius`, which feeds the promote/demote hysteresis — the
first-rung policy oscillated for exactly that reason and was turned off.
Shrinking the ladder is the same class of change and needs the same missing
prerequisites (demand-driven rung promotion, extent-keyed bound_radius). It
is deferred, with `TerrainBakeTargets::first_rung` (`lod_bake.h:281-288`)
kept as the mechanism for when it lands.

### Cache invalidation and rebake cost (hard problem 9)

Sector parts are transient per-process artifacts
(`local_provider.cpp:1414-1428`); there is no durable sector cache to
invalidate, and every session already cold-bakes its disc. Adding
`sectorSize` to the params JSON changes every sector hash, which costs
nothing durable. The durable caches — child asset `.part`s (Rock/Tree/Alpine*
under `projects/<world>/.cache`) and detail `.gtex` bakes — key on child
module + params (`WorldSector.js:120-128`: the `requires()` list is
deliberately tx/tz-independent) and are untouched: the variant list, the
child hashes, and the tileset plan do not change. Migration rebake cost is
therefore **one ordinary session fill, which the migration itself shrinks**:
~1,160 tile bakes instead of 78,168 (at unchanged triangles per annulus; the
6,547-sector 2.9 km disc filled in 48 s, and the tile count here is ~6× fewer
than that disc for 3.5× the reach). No incremental path is needed because
there is nothing persistent to migrate.

### What guards this design (tests)

Must not change: `run-lod-distance` (the selection-rule equivalence proof);
`run-sectorcoord` (`sector_streaming_coordinator_tests.cpp` — the coordinator
is deliberately untouched); the legacy/uniform cases in `run-sectorstream`
(`sector_streamer_tests.cpp:271-274` asserts bare-rung behaviour with the
ladder off — that is the rollback path's regression gate); the existing
equal-grid seam cases in `run-terrainmesh`.

Must change / grow: `sector_streamer_tests.cpp` gains nested-mode cases
(coverage invariant — exactly one desired tile per world column; restriction
invariant; split/merge hysteresis; group publish-then-evict ordering incl.
partial failure); `terrain_mesher_tests.cpp` gains the cross-*size* seam case
(§proof above); `sector_bake_tests.cpp` bakes a WorldSector at a non-default
`sectorSize` and asserts determinism (double-bake cmp); `world_stream_tests`
gains a nested end-to-end settle.

## WP0 baseline, measured (2026-08-08)

The counts above were derived by a script over the authored bands. Running the
real `SectorStreamer` with StreamMountain's authored config (rings
150/500/10,095, bands 318/1,186/2,605/4,702/7,753/10,095, `sector_size` 64,
`hysteresis` 16, `max_inflight` 64) to settle at anchor (32, 32), with bakes
stubbed to publish instantly:

| quantity | measured |
|---|---|
| resident sectors at settle | **78,161** |
| publishes to fill | 78,161 |
| `update()` tick, settled | **5.37 ms** |
| streamer-only settle wall | 34.8 s (bookkeeping alone — no bakes, no publishes) |

Residency by terrain LOD: 32,064 / 29,136 / 11,768 / 4,108 / 1,016 / 69 for
LOD 0…5 — i.e. **78.4% in the two coarsest bands**, matching the derived 78.3%
to within the balance pass's edge effects (the derivation predicted 78,168;
the balance and hysteresis passes move a handful of tiles between bands).

Two things the derivation understated:

- **`update()` costs 5.37 ms per tick at settle.** This is not bake cost or
  publish cost — it is the desired-set scan plus the three `assign_terrain_lods`
  passes over a 78k-entry map, run every tick, on the thread that calls it. The
  design predicted this shape (≈99,856 distance evaluations plus three map
  walks) but not its size; at 5.37 ms it is a third of a 60 Hz frame spent
  deciding which sectors are wanted.
- **34.8 s of pure streamer bookkeeping to fill**, with every bake, stage and
  publish removed. The real fill cost is this plus all of that.

These are the numbers WP7 compares against. Harness: `wp0_baseline.cpp` in the
session scratchpad (links `sector_streamer.cpp` directly; no engine, no GPU).

## Rejected alternatives (summary)

| alternative | rejected because |
|---|---|
| geometry clipmaps (toroidal rings) | mutable ring buffers contradict immutable content-addressed parts; bypasses part store/BLAS/VT/publish machinery; a second renderer |
| error-adaptive sparse quadtree | field detail is statistically uniform; part count, not per-part error, is the shortage; strict superset addable later on the same keyspace |
| far-sector mega-part batching on the uniform grid | keeps 78k bakes and streamer entries; non-nesting merge groups have no edge-mask story |
| separate fixed 64 m scatter-part grid | keeps O(R²) parts for the 10 km tier-0 reach — the disease itself |
| scatter confined to near levels + impostors | visually regressive; impostors are a different programme (`feature/representation`) |
| per-level scatter density | reshuffles placements; new authoring axis with no demand |
| level bits added to the variant packing | redundant — level ≡ 5 − terrain LOD while cells-per-sector is constant; bits 12-14 reserved if that ever changes |
| Y-slab bounding work | already done — the mesher bounds the slab to sampled relief (`terrain_mesher.cpp:154-161`) |
