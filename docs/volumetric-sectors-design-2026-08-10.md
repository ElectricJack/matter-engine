# Volumetric sectors: 3D nested streaming, an invariant seam contract, and occlusion

**Date:** 2026-08-10
**Status:** design — not yet implemented
**Test world:** `projects/world_demo/scenes/StreamCaverns` (built for exactly this)
**Prior art this builds on:** `docs/terrain-nested-sector-lod-2026-08-08.md` (the XZ nested
system this generalizes), `docs/lod-vt-redesign-2026-08-04.md` §6 (the unbuilt occlusion
sketch this adopts and concretizes)

---

## 1. Goal

Replace the XZ-column streaming grid (each sector meshes all of `[yMin, surface]`) with a
fully volumetric grid: cube sectors around the viewer, doubling in size with distance in
all three axes. Same coverage-band scheme as today — **not** a strict octree: several
rings of same-size cells exist before the size doubles, but all sizes are powers of two
of each other and grids nest exactly.

Three deliverables, in priority order:

1. **The 3D grid itself**, as a minimal delta over the merged nested-sector-LOD system.
2. **A seam contract that is constant** — the current contract depends on a baked-in
   guess about the neighbor's LOD, and every transient disagreement opens a strip-shaped
   hole. This design makes every bake independent of neighbor state and generates
   cross-level seams at runtime from the drawn pair.
3. **Dynamic occlusion** of sectors (draw-side and streaming-side), generic but motivated
   by underground scenes where distance-only selection pays for a mountain you cannot see.

## 2. The current system, condensed

*(File references are to main @ `1239f564`.)*

### 2.1 Selection

- Anchor is 2D: `Coordinator::submit_anchor(owner, x, z)` — camera Y is discarded
  (`src/ecs/streaming_systems.cpp:95`, `streaming/sector_streaming_coordinator.h:169`).
- Nested mode builds the desired set by a **coverage-rule quadtree**: `update_nested` →
  `descend` splits a tile into 4 children when its nearest point is inside the next
  band's radius (`sector_streamer.cpp:418, 276`); `restrict_levels` then enforces 2:1
  across cardinal neighbors (`:320`); `assign_nested_masks` writes a 4-bit edge mask
  meaning "this cardinal neighbor is exactly one level coarser" (`:365`).
- Level L has size `S0 << L` and voxel `2 << L`, so every tile is 32 cells across
  (`sector_streamer.h:36-59`). Bands (`terrainBands`) give multiple same-size rings
  before a size jump — the "not exactly an octree" property already exists in 2D.

### 2.2 Keying

- The nested key packs `4 bits level | 30 bits tx | 30 bits tz` into a `uint64` with
  **zero spare bits** (`sector_streamer.h:182-195`). `SectorRequest{tx, tz, rung}` has no
  Y (`:107`); the level and edge mask ride inside the packed `rung` variant
  (`pack_variant`, `:25-33`: bits 0-3 scatter, 4-6 terrain LOD, 7-10 edge mask,
  11 marker, 12-14 reserved).
- Engine-side identity is `SectorKey{tx, tz, rung}` (`matter_engine.cpp:1180`) and
  `sector_instance_id(tx, tz, rung, part_hash)` (`:1311`).

### 2.3 The Y assumption

- `yMin`/`yMax` are **world-global** (`world_definition.h:305-311`), never per-request.
  The mesher's slab for a volumetric field runs from `yMin` to just above the surface at
  the tile's voxel (`terrain_mesher.cpp:232-240`): a StreamCaverns level-0 tile meshes a
  64×1280×64 m column, ~590 samples deep where a heightfield tile needs ~10. The scene
  file itself documents "yMin IS THE COST DIAL" (`StreamCaverns.js:49-52`).
- The published instance transform sets only `transform[3] = tx*S` and
  `transform[11] = tz*S`; mesh Y is world-absolute (`matter_engine.cpp:5206-5215`).
- One streamed sector = one part with **one cluster** whose AABB is the whole column
  (`part_store.cpp:1160-1196`) — a 1280 m tall cull unit, useless for occlusion.

### 2.4 Why strips of triangles go missing today

The causal chain, established by reading `terrain_mesher.cpp` and the streamer:

1. **Ownership is direction-asymmetric.** Face ownership is `i ∈ [1..n]`, and a quad at
   sample `i` consumes cells `{i-1, i}`, so a tile reaches ~1 voxel back past its −x/−z
   border and stops ~½ voxel short of its +x/+z border: every shared plane is bridged by
   the **east/north** tile at *that tile's* voxel size (`terrain_mesher.cpp:88-105`).
   Across a 2:1 step this closes one orientation and opens the other: coarse-west /
   fine-east leaves a ~1-fine-voxel strip emitted by **neither** tile.
2. **The fix is mask-gated.** `reach_x/reach_z = (edge_mask & kEdgeNeg*) ? 2 : 0`
   (`:127-128`, commit `bb3c4350`) extends ownership one coarse voxel past the masked
   −x/−z face. With the correct mask, the hole is closed (by an *overlap*, not a shared
   edge — established and tested doctrine, `terrain_mesher_tests.cpp:106-116`).
3. **The mask is a promise about the *desired* map, not the *drawn* one.**
   `assign_nested_masks` reads `desired_at(level+1, …)`. While the neighbor is mid-split,
   mid-merge, held by the transition rule, or parked, the drawn neighbor is a different
   level than the mask assumed. Then `reach` is 0 where it should be 2 → the strip is
   back. This matches the open report ("minor gaps at times while terrain is baking /
   changing LOD", filed *after* the gap fix and parking landed).
4. **The mask is part of the bake identity**, so every neighbor level change forces a
   full rebake of this tile, and the two neighbors' rebakes finish seconds apart —
   maximizing the window in which drawn state and baked assumption disagree.
5. Skirts were deliberately removed 2026-07-30 (POM showed them edge-on), so a mask
   mismatch now prints as a hole to the background, not a dark band.

The published-but-unbuilt mitigation was a group-atomic `WorldDelta` swap
(`docs/terrain-nested-sector-lod-2026-08-08.md:416-437`). This design goes further and
removes the need for cross-bake agreement altogether (§4).

## 3. Design: the volumetric grid

### 3.1 Key and coordinates

New packed key: `4 bits level | 20 bits ty | 20 bits tx | 20 bits tz` (offset-encoded
signed, `& 0xFFFFF`). Range ±524,288 tiles per axis — at `S0 = 64 m` that is
±33,500 km, ~3000× the largest streamed reach in the repo (StreamMountain, 10 km).
If that bound is ever a problem the fallback is a 16-byte struct key; not worth it now.

- `SectorRequest` / `Eviction` gain `int64_t ty` (`sector_streamer.h:107-108`), and the
  `ty` follows through `TaggedRequest`, `on_published` / `on_failed` / `cancel_request`,
  `same_request` / `same_publication_tag`, `SectorKey` / `SectorKeyHash` / `sector_map`,
  and `sector_instance_id` (which must mix `ty`, or vertically stacked tiles collide).
- `sector_footprints_overlap` (`matter_engine.cpp:1221`) becomes octree containment:
  shift all three coords by the level delta and compare.
- The packed variant simplifies rather than grows — see §4.1: the edge mask **leaves**
  the variant, so bits 7-10 free up. Layout becomes: 0-3 scatter, 4-6 terrain LOD,
  11 marker; 7-10 and 12-14 reserved.

### 3.2 Selection: coverage-rule octree

Direct generalization of the existing nested path — every function keeps its name and
role:

- **Anchor** gains Y: `submit_anchor(owner, m[3], m[7], m[11])`, and
  `SectorStreamer::update(ax, ay, az)`. All distance helpers (`sector_dist`,
  `tile_centre_dist`, `tile_near_dist`) become 3D point-to-AABB distances. The band radii
  keep their authored meanings; bands become spheres instead of cylinders. **No world
  needs new band tables** — StreamCaverns' six bands work unchanged, and a camera 900 m
  underground now gets level-0 tiles around *it* instead of around its column.
- **`update_nested`** iterates the coarsest grid over the reach cube, with `ty` clamped
  to the authored world Y range: `yMin`/`yMax` stop being the mesher's cost dial and
  become the octree's vertical extent (they must round outward to coarsest-tile
  multiples). For StreamCaverns that is 2 coarsest rows (−2048..0, 0..2048) — the
  top-level scan stays trivially cheap.
- **`descend`** recurses into 8 children instead of 4; the split/merge hysteresis
  asymmetry and whole-sibling-group merge rule carry over unchanged.
- **`restrict_levels`** enforces 2:1 over the 6 face neighbors; `min_edge_level` and
  `desired_level_at` take `(wx, wy, wz)`. "Exactly one level covers any column" becomes
  "exactly one level covers any point".
- **Transition groups and parking** generalize mechanically: `scan_footprint` /
  `scan_subtree` walk 8 children; parking's `sector_blocked_by_visible` uses the 3D
  footprint test. Parking semantics (fail-open after 30 s, parked entries don't block)
  are unchanged.

The "multiple same-size cells before a size jump" requirement is exactly what the band
radii already provide; nothing new is needed. Power-of-two nesting is preserved because
child coords are `2t .. 2t+1` on all three axes.

### 3.3 The mesher: Y becomes a tiled axis

`mesh_sector` signature changes from `(tx, tz, rung, edge_mask, sector_size, y_min,
y_max, …)` to `(tx, ty, tz, rung, sector_size, …)` — the edge mask disappears (§4) and
the Y slab is replaced by the tile's own extent:

- Tile origin `oy = double(ty) * double(sector_size)`, sample
  `y = oy + (j - 1) * voxel` — the same dyadic-`double` construction the X/Z
  axes use (`terrain_mesher.cpp:151-153`), so a shared lattice point is bitwise-identical
  from any tile at any level. Mesh positions become tile-local in Y as well; the publish
  transform gains `transform[7] = ty * sector_size`.
- Y gets the identical ownership treatment as X/Z: `n+3` samples (one ghost ring each
  side), ownership `j ∈ [1 .. n]`, the **top** tile bridges each horizontal shared
  plane (the natural extension of the east/north rule). No reach/snap machinery exists
  in any axis anymore — cross-level faces are the welder's job (§4.1).
- Boundary-record export (§4.1) covers all six faces symmetrically.
- The `h_min < y_min` validation ("sampled height outside authored Y range",
  `terrain_mesher.cpp:215`) moves to world-load-time validation of the octree Y extent.

**Empty tiles.** Most volumetric tiles are all-air or all-solid. The mesher already
yields zero quads for them; the publish path needs an explicit "resident, no geometry"
outcome: the streamer sees a normal `on_published` (so the desired map converges and
transition groups complete), the engine records a `SectorEntry` with no manifest entry
and no part. Since the bake cache keys on `(key, fieldHash)`, an empty classification
persists across runs — each empty tile costs one field-sampling pass ever, not per
session. If that first pass is still too hot (33³ samples × thousands of air tiles), the
follow-up is a parse-time conservative interval analysis of the field program to classify
pure-air / pure-solid tiles without dense sampling — deliberately **out of scope** for
the first cut; measure first (the ColumnCache y-independent prefix already makes all-air
columns cheap in heightfield-dominated regions).

**Cost model flip.** Today a StreamCaverns level-0 tile samples 35×~590×35 (whole
column); after this change it samples ≤ 35×35×35, and tiles outside the surface/cavern
shell are near-free. That is the point of the migration. Note `ColumnCache` amortizes its
y-independent prefix over 33 samples instead of ~590 — the *relative* prefix cost rises
even as total cost falls; re-measure in situ, don't microbench (see
`memory/measure-in-situ-not-in-a-microbench.md`).

### 3.4 Scatter ownership

Scatter is column-based (`heightAt`) and must not double-place when a column crosses
several stacked tiles. One deterministic rule: **a tile scatters column (x, z) iff
`height(x, z) ∈ [tile.y0, tile.y1)`**. Exactly one tile per column satisfies it at any
single level, and the 2:1 restriction plus the existing fixed-64 m scatter sub-grid
(`StreamMountain/objects/WorldSector.js:168-210`) carry over unchanged. The warp anchor
stays XZ (`warp_field.h:43-53`) because scatter columns remain XZ entities — flagged for
audit in M3, not redesign.

### 3.5 What deliberately does not change

- Band/ring config schema in world JS (`nestedSectors`, `terrainBands`, `rings`).
- The bake worker pipeline, `stage_load`, publish transactions, acknowledgments.
- The LOD-distance rule (`lod_distance.h`) — sector rungs and part LODs stay decoupled.
- The uniform (non-nested) path: it remains 2D and becomes legacy; volumetric requires
  `nestedSectors`.
- `sector_grid.h`'s render-binning `SectorCoord{x,y,z}` is already 3D; terrain bins
  simply start landing at real `c.y`. The activation-sphere radius derivation
  (`provider/resolvers.cpp:79-85`) gets an audit because today every terrain bin sits at
  `c.y == 0` — with real Y bins its altitude behavior finally becomes correct rather
  than accidentally wrong.

## 4. Design: the seam contract

This is the load-bearing part of the design. The structure follows dual contouring's
own split: **per-cell meshing is a per-tile (baked) concern; the geometry that crosses
tile boundaries at mismatched resolutions is a cross-tile concern and is generated at
runtime from the two actual meshes** — never baked against an assumption about the
neighbor.

First, the observation that scopes the problem: **equal-level seams need nothing.**
The tile on the +side of every shared plane bridges it today, recomputing the
neighbor's border-cell vertices from shared density samples whose dyadic-`double`
world coordinates are bitwise-identical from either side
(`terrain_mesher.cpp:151-153`, `:413-416`). That path is exact, proven, and untouched
by this design (extended to ±y: the **top** tile bridges horizontal planes). Only
**cross-level** faces — a sparse set living exactly on band boundaries — need any
machinery, and they are the entire subject of §4.1.

A note on why no baked scheme can be neighbor-independent, recorded so the idea stays
dead: a fixed boundary vertex set shared by both sides requires a local rule `f(level)`
for the face's canonical resolution with `f(L) = f(L+1)` for every legal pairing —
forcing `f` constant across all levels, i.e. every border in the world meshed at
level-0 resolution. (The retired heightfield path could share a border polyline
because it was a *primal* grid with vertices on the border —
`stitch_edge`/`corner_patch`, `terrain_mesher.cpp:696-754`. Surface nets places
vertices inside cells, so cross-level agreement requires knowing the pairing.) Baking
per-face *variants* and selecting at draw time was this design's previous revision; it
died on the corner coupling — a band quad near a tile corner uses the *other* axis's
ghost vertices, so face variants are not independent and the combination count grows
like the classic 2ᵏ patch explosion. Runtime generation sees the actual pairing and
has no combinatorics.

### 4.1 The seam welder

**Bake side — export, don't stitch.** Each tile bake exports, alongside the mesh, its
sparse per-face boundary record: for every boundary cell that produced a vertex,
`(cell index, position, normal, material)`. Only surface-crossing cells appear —
typically tens of entries per face, kilobytes per tile — carried on the staged
artifact and the `SectorEntry`. The mesher itself **loses** every cross-level feature:
the `edge_mask` parameter, `reach_x/reach_z` (`terrain_mesher.cpp:127-128`), and both
boundary snaps (`:200-213`, `:306-334`) are deleted, not extended to 3D. What remains
is pure per-tile surface nets plus the export.

**Engine side — weld the drawn pair.** A seam welder runs whenever the drawn
neighborhood changes — publish, evict, unpark: the same events parking already hooks
(`matter_engine.cpp:5217-5243`, `:4104-4174`). For each cross-level face pair in the
drawn set it emits the crossing band connecting the two exported vertex sets: one fan
per coarse border cell onto its (≤ 2×2) fine counterparts, with crossing edges decided
at the finer resolution (the dual-contouring `faceProc` rule). Equal-level pairs are
skipped — baked geometry already covers them. Because welds are built *from* the drawn
set at the moment it mutates, drawn state and seam geometry cannot diverge; there is
no baked assumption to go stale. The transient-hole class is gone by construction.

**Corners and edges are the same code, not special cases.** The welder is engine-side
and sees the whole `sector_map`, so tile-edge and tile-corner adjacencies are just
further weld cases over 4 (edge) or 8 (corner) tiles' exported corner cells — run by
the same fan logic, baked nowhere. It also degrades gracefully when a *diagonal*
neighbor is two levels away (legal — `restrict_levels` balances faces only), so no
strengthening to full octree balance is needed.

**Honest residue.** Where the two resolutions disagree about topology — a fine-only
sign change on the shared plane whose coarse cell produced no vertex — the fan has no
landing site; the welder emits a collapsed cap or drops the sliver. This is the
irreducible residue of any 2:1 scheme (Transvoxel has its own version); the defect is
sub-fine-voxel, versus today's full one-voxel strips. Do not chase watertightness
past it.

**What happens when a face pair changes level.** The no-strip ↔ strip transition never
touches the unchanged tile — the strip lives in the seam pool, not in either mesh:

- *Neighbor merges coarser:* the neighbor is rebaked (that **is** the level change,
  unavoidable under any scheme); this tile's mesh and exported boundary record are
  reused as-is; the weld is added in the same visibility transaction that swaps the
  new neighbor in. Frame N draws old pair, no strip; frame N+1 draws new pair with
  strip; this tile is identical in both.
- *Neighbor splits back to equal:* the weld is removed in the swap transaction, and
  this tile's baked border band — which never went away — is **immediately bitwise
  exact** against the new equal children, because its ghost vertices come from the
  same shared lattice samples the children use. Zero work, zero inconsistent frames.
- Symmetric cases (neighbor gets finer; this tile on the owning side of the plane or
  not) reduce to the above. In every case exactly one tile rebakes: the one whose
  level changed.

The single engineering obligation this creates: weld add/remove must be
**transactionally coupled** to the publish/evict batch it belongs to — one more
passenger on the parking gate's batched swap (§4.2), not new machinery.

**Consequences, in order of importance:**

1. **Seams always match what is drawn** — correctness during baking, splitting,
   merging, parking, and fast flight follows from construction, not from timing.
2. **The edge mask leaves the bake identity.** Neighbor level changes stop forcing
   rebakes (`docs/terrain-nested-sector-lod-2026-08-08.md:154-159` documents today's
   cascade), and `assign_nested_masks` / `assign_terrain_lods` pass 3 are deleted —
   the streamer no longer knows seams exist. When a pair changes level in either
   direction, the fix is a microsecond-scale weld update, not a two-sided rebake
   racing itself.
3. **The mesher gets simpler than it is today** — the direction-asymmetric ownership
   subtleties stay (they serve equal-level seams), but every masked-face branch goes.
4. **v1 needs zero renderer changes.** The tiles' own baked border bands (the fine
   crossing quads that today under-reach a coarse neighbor) remain drawn; they are
   valid on-surface geometry and the weld closes the gap over them — the proven
   overlap doctrine (`terrain_mesher_tests.cpp:106-116`). A v2 refinement may add a
   per-instance face bit to suppress a tile's border band under an active weld
   (exactness over overlap); that is an optimization, not a correctness need.

### 4.2 Weld runtime plumbing

- **Geometry pool.** Welds live in one engine-owned dynamic vertex/index pool (a "seam
  part"), keyed by face pair, appended and retired with the same batched paths the
  static-buffer append work established — per-publish `O(world)` main-thread cascades
  are the known failure mode here (`issues/bfb5f13e`); weld updates must be
  incremental and per-face. Individual welds are tens-to-hundreds of quads; building
  one is microseconds on the app thread, and the visibility gate below removes any
  need to do it synchronously with publish.
- **Visibility gate.** A tile becomes visible only when the welds along its
  cross-level faces are built — one more clause in the parking predicate
  (`sector_blocked_by_visible`), sharing its sweep, its fail-open valve, and its
  transactional withhold-from-`WorldState` behavior.
- **RT.** Weld geometry is view-independent world state, so feeding the TLAS is
  doctrine-legal (`draw_overrides.h:16-22`). Plan: one pooled seam BLAS, refitted on
  weld changes. v1 may ship raster-only welds — a one-voxel strip absent from
  reflections and shadows is far below today's artifact floor — but the RT replay
  diffing loop will flag the difference, so the choice must be explicit and tested,
  not accidental.
- **Texturing.** Welds do not enter the per-part chart/VT pipeline. v1 shades a weld
  by flat-sampling the coarse side's material at the fan centroid — welds sit exactly
  where the LOD ladder already drops fidelity. Revisit only if a captured A/B shows
  the band.

### 4.3 What remains of parking and transition groups

Parking and the transition-group hold keep their current job — preventing double-draw
and holes during split/merge — with 3D footprints, and gain the weld-ready clause
(§4.2). They are no longer load-bearing for *seam correctness*, only for *coverage
continuity*. The group-atomic `WorldDelta` design from the nested doc stays unbuilt;
with seams generated from drawn state, there is nothing left for cross-bake atomicity
to protect.

### 4.4 The 2:1 restriction is still mandatory

The welder's fan logic implements exactly one level of difference across faces;
`restrict_levels` over 6 faces remains the invariant that makes it sufficient.
Diagonal-only contacts two levels apart remain permitted, as today.

### 4.5 The drawn ±1 invariant: staged refinement

`restrict_levels` constrains the **desired** map only. Nothing constrains the **drawn**
map: under fast flight a region's desired level can jump several levels at once (3 → 0),
tiles rebake independently, and a tile that finishes early is drawn at level 0 beside a
neighbor still showing level 3. The welder only implements 2:1 fans — a multi-level
face is outside the seam machinery's domain entirely, and it is also the 8×-detail
pop. This transient is a distinct failure class from the stale-mask bug of §2.4 and
survives the §4.1 fix; it needs its own constraint.

Two halves:

1. **Staged refinement (streamer).** Clamp requests so a tile is never *requested* more
   than one level finer than its neighborhood's resident coverage: in `descend` /
   `mark_desired`, `effective_level = max(desired_level, resident_neighborhood_level −
   1)`. A multi-level jump becomes monotone waves (…3, then 2, then 1, then 0), each
   wave fully resident before the next is requested. **Cost is bounded and small**:
   cells-per-tile is constant (32 per axis), so a wave at level k+1 covers 8× the volume
   per tile of the wave at level k — the intermediate waves cost Σ 8⁻ᵏ ≈ **+14% bake
   work** worst case over jumping directly. In exchange, fast flight shows progressively
   refining terrain instead of a long hold followed by an 8ⁿ pop.
2. **Publish gate (engine, backstop).** A tile may become visible only if every *drawn*
   face neighbor is within ±1 of its level — a generalization of the parking predicate
   (`sector_blocked_by_visible`, `matter_engine.cpp:1236`), needed because parking and
   transition holds make the drawn set lag the streamer's residency model. Deadlock-free
   because staged refinement makes level changes monotone coarse→fine per region, so the
   gate always has a satisfiable unpark order; the existing 30 s fail-open valve
   (`kMaxParkedTime`) remains the last resort, and firing it now signals a streamer bug
   rather than an accepted race.

With both halves, ±1 across drawn faces stops being an assumption the seam code hopes
for and becomes a guaranteed invariant — in steady state and in every transient. The
welder then only ever needs to span exactly one level, which is all it implements.

## 5. Design: occlusion

Two independent layers sharing one measurement. Both follow the doctrine written in
`docs/lod-vt-redesign-2026-08-04.md` §6.3: **visibility drives priority and detail,
never existence** — disocclusion must reveal something coarse, never a hole.

### 5.1 Draw-side: two-phase HZB culling

Standard two-phase GPU occlusion culling, slotted into the existing cull pass:

- **Pyramid:** min-reduction depth pyramid (reversed-Z, `D32_SFLOAT` — mind the swapped
  near/far labels noted in `frame_matrices.cpp:99-102`). New compute shader; the
  renderer has no HZB today, but the plumbing is pre-provisioned: `hiz_culled` is a
  declared-never-written stat (`cull.comp:103`) already surfaced through the UI and
  asserted zero in the smoke tests, the HZB contract comment exists at `cull.comp:50-54`,
  and set-1 binding 16 is the next free slot (`vk_scene_renderer.cpp:1863-1898`).
- **Phase 1** draws clusters visible last frame (per-cluster visibility bit buffer).
  **Build** the HZB from that depth. **Phase 2** tests the remaining clusters' AABBs
  against the HZB and draws the newly visible; update the bits. No frame-latency popping,
  no reprojection heuristics.
- **Granularity is fixed by this migration itself:** today a streamed sector is a single
  1280 m tall cluster — un-occludable. Volumetric cube tiles give the cull pass real
  boxes to reject (and the mesher can still split a tile into sub-clusters for finer
  AABBs — the nested doc's "4 clusters per tile" option — if it ever shows). This is
  why occlusion is sequenced *after* the grid change, not before.
- **RT is excluded**, per the `draw_overrides.h` doctrine: a screen-space cull must not
  remove shadow casters or reflection geometry. The TLAS path keeps its existing
  distance/billboard early-outs only.

### 5.2 Streaming-side: visibility-informed priority and detail cap

The streamer's priority today is holes-then-upgrades, nearest-first
(`sector_streamer.h:118-120`), and its detail choice is distance bands. Add one input:

- **Readback:** per-sector "last visible frame" via the proven `capture_lod_trace`
  pattern (`vk_scene_renderer.cpp:10729-10798`) — fenced, a-few-frames-stale, already
  inverts buckets back to instance tokens. The cull pass additionally writes a
  per-instance visible bit; the readback aggregates to sector keys and hands the
  coordinator a small `(key → last_visible_frame)` map. Latency is harmless because this
  feeds priority, not correctness.
- **Priority:** visible holes → offscreen holes → occluded holes, nearest-first within
  each class. (Frustum-only feedback is a shippable first increment with identical
  plumbing, before the HZB exists.)
- **Detail cap:** a sector occluded for more than a grace period is **demoted to a
  coarser band cap** (one or two levels coarser than the band's desire — a tunable),
  never evicted below coverage. Disocclusion then shows coarse terrain that refines in
  seconds — same behavior the system already exhibits when you outrun the streamer,
  which parking/hold already smooths. For a camera deep in StreamCaverns this stops the
  mountain overhead from ever holding fine rungs, which is the single biggest win
  available underground; on the surface it quietly demotes the far side of every ridge.
- The occlusion signal must never gate *existence* of coverage (the desired set), so a
  wrong/stale bit costs quality for a few seconds, never a hole — keeping the failure
  mode strictly gentler than the current seam bug, and keeping streaming deterministic
  when the readback is absent (headless tests: cap simply never engages).

## 6. Migration plan — five independently-landable stages

Ordered to de-risk: the seam fix lands **first, in the current 2D system**, because it
is the shipping bug and shrinks the 3D diff.

**M0 — Seam welder (2D).**
Mesher: delete `edge_mask`/`reach_*`/both snaps; add the sparse boundary-record export
(§4.1). Engine: the welder (fan logic + seam pool + weld-ready parking clause, §4.1-4.2)
driven from publish/evict/unpark; drawn-±1 publish gate (§4.5). Streamer: delete mask
assignment (mask leaves bake identity); staged-refinement clamp (§4.5).
*Tests:* extend the union-coverage rows harness (`terrain_mesher_tests.cpp:~296-410`) to
run mesh + weld and assert zero-gap for **every** drawn cross-level pairing, including
mid-transition configurations that are untestable today; a welder unit suite (equal
skipped, 2:1 fans, corner adjacency, topology-disagreement caps); a StreamCaverns +
StreamMountain LOD-churn soak (scripted fly-through toggling bands) with a
missing-strip counter derived from `seam_stats`. *Acceptance:* the open "gaps while
baking/changing LOD" issue closes; weld build time and pool churn stay off the frame
spike list (§7 risk 1).

**M1 — 3D keys and plumbing, `ty = 0`.**
Key repack (4/20/20/20), `ty` through `SectorRequest`/`Eviction`/`TaggedRequest`/
`SectorKey`/`sector_instance_id`/publication tags, 3D footprint math, anchor carries Y
(unused). Pure refactor; every existing world byte-identical.
*Tests:* key round-trip incl. negative coords; existing streamer/coordinator suites
updated mechanically; determinism hash unchanged on StreamMountain.

**M2 — Y-tiled mesher.**
`mesh_sector(tx, ty, tz, …)`, tile-local Y, top-tile-bridges ownership, ±y boundary
export and welds, empty-tile outcome through stage/publish. Driven by a test harness
requesting fixed vertical stacks; no streamer changes yet.
*Tests:* vertical seam union-coverage (new, mirrors the horizontal harness); horizontal
weld fans across `ty` boundaries with POM on (§7 risk 4); empty-tile publish/evict
round-trip.

**M3 — Octree selection; StreamCaverns goes volumetric.**
3D anchor submit, 8-child `descend`, 6-face `restrict_levels`, 3D parking/transition
groups, scatter column-ownership rule, `yMin`/`yMax` reinterpreted as octree extent,
`transform[7]`. Gate on a world flag (`volumetricSectors: true`) defaulting off;
StreamCaverns flips first, StreamMountain after soak.
*Tests/acceptance:* StreamCaverns fly-through descending to −1000 m — bake cost vs the
column baseline (expect the ~590-sample slab term gone), part/instance counts, the M0
seam soak re-run in 3D (this is the scene that exercises vertical seams hard), tick-time
budget vs the nested baseline (0.367 ms on StreamMountain today).

**M4 — Occlusion.**
Phase A: frustum-visibility readback → streaming priority + detail cap (no HZB yet).
Phase B: HZB build + two-phase cull, `hiz_culled` finally written, smoke-test
assertions updated from `== 0` to invariants (`hiz_culled + emitted + frustum_culled`
conservation).
*Acceptance:* StreamCaverns underground: measure resident fine-tile count and GPU
frame time with the cap on/off; StreamMountain regression: no visible popping on ridge
disocclusion; RT output byte-identical with occlusion on/off (doctrine check).

## 7. Risks and open questions

1. **The seam pool is a new dynamic-geometry path on the streaming hot path.** The
   engine's history says per-publish main-thread work cascades (`issues/bfb5f13e`, the
   parking sweep's 683/881 ms hitch suspicion). Welds are tiny, but the *pool
   management* must be incremental per face-pair, never O(world); profile the soak with
   ProfileLib lanes before calling M0 done. Weld builds can run on the worker side —
   the weld-ready parking clause means nothing waits synchronously.
2. **Weld texturing is the visible compromise.** Welds bypass the chart/VT pipeline and
   flat-sample the coarse side (§4.2). The band is one coarse cell wide at an LOD
   boundary — likely invisible, but this is the design's main quality gamble now;
   capture A/Bs at a masked-vs-welded seam in M0.
3. **RT coverage of welds.** v1 raster-only welds will show up in the RT replay diffing
   loop as raster/RT divergence; either land the pooled seam BLAS in M0 or explicitly
   annotate the replay baseline. An unflagged divergence will burn a future bisect
   (the "invalidate_part logged as a failed bisect" lesson).
4. **Welds vs Ground POM.** Skirts died because POM exposed vertical curtains edge-on.
   Weld fans are on-surface transition geometry, not curtains, but a horizontal weld
   under a cave ceiling is exactly the geometry POM displaces most; verify in M2's
   harness with POM on, and fall back to excluding welds from POM (flat shading) if it
   shows.
5. **Empty-tile bake cost** before the interval-analysis follow-up: thousands of air
   tiles × one 33³ sampling pass on first visit. Mitigated by persistent cache and the
   ColumnCache prefix; measure in M3 before building the analyzer.
6. **Instance/part count growth.** Nested 2D took StreamMountain 78k → 1.2k parts; 3D
   multiplies occupied tiles by the vertical shell (surface sheet ≈ 1-2 tiles per
   column + cavern shells). Estimate during M3; the TLAS instance ceiling
   (`TLASManager::draw` drops past its ceiling with only a printf — known trap) needs a
   hard check, not a hope.
7. **Activation-sphere semantics** (`resolvers.cpp:79-85`) once terrain bins carry real
   `c.y` — audit in M3.
8. **20-bit coordinate range** is a real (if huge) reduction from 30 bits; assert at
   world load that the authored reach fits.
9. **Staged refinement trades transient detail for correctness**: after a fast approach,
   time-to-finest is the sum of the wave bakes rather than one wave. The +14% total work
   bound (§4.5) caps the cost, and each wave is visible as it lands, so perceived
   latency improves even as final-detail latency grows slightly. Measure in the M3
   fly-through; the dial is `max_inflight` and wave overlap (a wave may be *requested*
   when its predecessor is resident, not yet visible).

## 8. Explicitly rejected alternatives

- **Group-atomic `WorldDelta` swap as the seam fix** (the nested doc's §"deferred
  visibility"): treats the symptom (visibility timing) while keeping the disease (baked
  neighbor assumptions + rebake cascades). Kept unbuilt.
- **Baked per-face seam variants with draw-time selection** (this doc's previous
  revision: always-baked "apron" bands, rim-pinned to reconstructed coarse vertices,
  toggled by per-instance bits): killed by corner coupling — a band quad near a tile
  corner uses the other axis's ghost vertices, so face variants are not independent and
  exact corners need per-combination index variants (the classic 2ᵏ patch explosion).
  It also required bitwise-faithful vertex reconstruction and a `GpuInstance` layout
  change. The welder needs none of that; the impossibility argument in §4 records why
  no simpler baked scheme exists either.
- **Transvoxel-style transition cells (baked):** same fate as the above for the same
  reason — any *baked* transition needs the neighbor's level at bake time. The welder
  *is* the transition-cell idea, executed at runtime where the pairing is known.
- **Strict octree (one cell per size ring):** the band tables already express "several
  same-size rings before doubling" and worlds depend on tuning them; no reason to
  restrict.
- **128-bit sector keys:** not needed within any plausible world size; 4/20/20/20 keeps
  the `uint64` map plumbing intact.
