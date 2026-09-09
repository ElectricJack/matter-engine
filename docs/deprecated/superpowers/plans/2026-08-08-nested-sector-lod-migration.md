# Migration plan: nested sector LOD (double the sector as the voxel doubles)

Date: 2026-08-08
Design: `docs/terrain-nested-sector-lod-2026-08-08.md`
Baseline numbers: 78,168 resident sectors at StreamMountain's authored 10,095 m
reach on the uniform 64 m grid; ~1,160 under nesting (coverage rule). Target:
same triangles, same reach, 65-85× fewer parts.

Eight work packages, WP0-WP7. Every WP is independently landable, leaves main
shippable, and names the test that proves it. WP0-WP4 are behaviour-inert
(nothing the user sees changes); WP5 changes bake-time scatter mechanics with a
bitwise-equality gate; WP6-WP7 are where pixels move, each with a visual
acceptance step.

---

## Execution constraints (carried from prior efforts' hard-won rules)

- **Roll forward, never revert.** Fix regressions in place.
- **Never `git add -A`** — the shader junctions must never be staged; agents
  have swept them into commits before (issue-triage 2026-07-31).
- **`make -C MatterEngine3` does NOT rebuild shaders**; none are touched here,
  but if any WP grows a shader edit it needs the `vulkan-spirv` target.
- Native exes launch with explicit `TMP`/`TEMP` (worktree gotchas memo).
- **Every new guard must be proven failable** — demonstrate the red state
  before trusting the green one (this applies to the coverage invariant, the
  restriction pass, and the group-commit atomicity assertions below).
- Version-vector / `components` constants: check main's value before bumping;
  stack, never replace.
- Visual re-baselines are deliberate events: `MATTER_REPLAY` before/after,
  review, then commit with evidence.

## Rollback position

One world-level flag, `streaming.nestedSectors` (default **false**), mirrored
by `MATTER_NESTED_SECTORS` for headless A/B, gates every behavioural change.
Off = today's uniform grid, byte-for-byte: the uniform scan/balance path in
`sector_streamer.cpp` is kept intact (not rewritten in place), the bake JSON
emits `sectorSize` equal to the world scalar (WP2 makes that inert), and the
legacy assertions in `sector_streamer_tests.cpp` (bare rungs, uniform variant
disc) continue to run against the off state forever. There is no durable data
to roll back: sector artifacts are per-process transients
(`local_provider.cpp:1414-1428`), and child/tileset caches are untouched by
design. Retiring the flag is a deliberate later decision, not part of this
plan.

## Cold-rebake / cache story

No durable invalidation. WorldSector artifacts are transient per PID; adding
`sectorSize` to the params JSON (WP2) changes only transient hashes. Child
asset hashes (`requires()` is tx/tz-independent, `WorldSector.js:120-128`)
and detail `.gtex` bakes are unchanged. The first nested session is an
ordinary cold fill of ~1,160 tiles — smaller than the 6,547-sector disc that
filled in 48 s on 2026-07-30.

---

## WP0 — Baseline capture *(no code)*

Record, on main with the flag absent: resident sector count at settle
(streamer `resident_count`), fill wall time, `[stream.task]`/`[stream.stage]`
accumulators, frame ms + TLAS instance count mid-flythrough
(`streammountain_flythrough.path`), and a `MATTER_REPLAY` pixel baseline.
These are the numbers WP7's acceptance compares against.

- Touches: nothing (scratch scripts only; findings appended to the design doc).
- Proof: the numbers exist and reproduce twice.

## WP1 — Nested desired-set in the streamer, behind the flag *(behaviour-inert while off)*

The core algorithm change, entirely inside `SectorStreamer` so it is testable
headless with no engine.

- `Config` gains `nested_sectors` (default false). New internal keyspace
  `(level, tx, tz)` (level bits in the map key, `sector_streamer.h:122-130`);
  the *external* `SectorRequest/Eviction` structs are unchanged — level rides
  the packed variant as `5 − terrain_lod` (design §keyspace).
- `update()` nested path: quadtree descent (nearest-point split test,
  centre-rule band membership), merge hysteresis on whole sibling quads,
  cross-level restriction pass, cross-level edge masks. Uniform path kept
  verbatim.
- `next_request`/`on_published`/`on_failed`/`cancel_request`/`clear` work on
  the new keys; 1:1 swap semantics unchanged (groups come in WP4 — until
  then, nested mode is *not* wired to the engine, so the interim N:1 gap is
  unreachable).
- Touches: `MatterEngine3/src/sector_streamer.{h,cpp}`.
- Tests (`make -C MatterEngine3/tests run-sectorstream`):
  - all existing cases green, untouched;
  - **coverage invariant**: after settle, every world column inside the reach
    is covered by exactly one desired tile (prove it failable by breaking the
    split test first);
  - **restriction invariant**: cardinal neighbours differ by ≤ 1 level, on a
    deliberately pathological authored band table;
  - hysteresis: an anchor parked on a level boundary neither splits nor
    merges across ticks;
  - edge masks name exactly the one-coarser neighbours across grids
    (half-edge and corner cases);
  - legacy: `nested_sectors=false` produces bit-identical request/eviction
    streams to today (record/compare against the current implementation).

## WP2 — Per-request sector size plumbing *(behaviour-inert)*

Make size flow per-request end to end, with the value still always `S_0`.

- Bake JSON gains `"sectorSize"` (`matter_engine.cpp:4246-4254`); the bake job
  overrides `opts.world.sector_size` per request after
  `world_profile.apply()` (`:4261`); warp anchor (`:4436-4438`), placement
  transform (`:4767-4770`), prebuild classifier translation (`:4586-4589`),
  and the two `SectorKey→world` translations (`:3458`, `:3551`) all derive
  size from the request/key variant (`S_0 << (5 − variant_terrain_lod)`),
  which is `S_0` for every value produced today.
- `WorldSector.js`: `static params` gains `sectorSize: 64`; `build()` uses
  `p.sectorSize` for `ox/oz` and rect sizes. (`terrainVolume` already takes
  size from the world binding; the JS constant `SECTOR` becomes the *cell*
  constant for WP5.)
- Chart density: `part_store.cpp:1082` scales terrain `texels_per_meter` by
  `S_0 / warp.sector_size` — a no-op at 64.
- Touches: `matter_engine.cpp`, `render/part_store.cpp`,
  `projects/world_demo/objects/WorldSector.js`.
- Tests: `run-sectorbake` gains a non-default-size bake (128 m, level-1
  rung) asserting mesh extent and double-bake determinism; `run-terrainverb`
  unchanged; full suite sweep for the params-JSON change.
- Note: sector hashes change (transient only). This WP is visually inert;
  confirm with a `MATTER_REPLAY` diff against WP0's baseline (must be
  pixel-identical — the same gate the shot-replay loop uses).

## WP3 — Cross-size seam proof in the mesher *(behaviour-inert; pure test)*

No mesher code is expected to change (design §crack-free proof) — this WP
exists to make that claim a regression gate rather than prose, *before*
anything renders nested tiles.

- `terrain_mesher_tests.cpp`: mesh a (rung −L, S_L) tile against its
  (rung −(L+1), S_{L+1}) coarser neighbour with the mask set; assert the
  shared boundary polylines agree to the bit, including the coarse-edge
  midpoint (fine-fine corner) and a three-tile corner with a diagonal
  two-level difference. Prove failable by clearing the mask.
- Touches: `MatterEngine3/tests/terrain_mesher_tests.cpp` only.
- Test: `run-terrainmesh`.

## WP4 — Transition groups: atomic N:1 / 1:N publish *(behaviour-inert while flag off)*

> **As built (2026-08-08): the invariant is enforced in the STREAMER, not by a
> deferred-commit publish path in the engine.** The rule below — "the old
> residency is torn down only in the same delta that establishes the complete
> new residency" — is implemented as: a superseded tile is held resident and
> drawn until every desired tile covering its footprint is resident. Everything
> the group ledger was for falls out of that one test, with no ledger: a tile
> genuinely out of range has nothing desired over its footprint and goes
> immediately; a partially failed split holds its parent (a stale coarse tile
> beats a hole); an abandoned transition stops being covered and releases itself
> with nothing to unwind.
>
> What this does NOT do is collapse the swap into a single `WorldDelta`. The
> four children are published as they finish, and the parent's eviction lands
> one or two frames after the last of them — so parent and children overlap for
> those frames. That window is the same one every 1:1 rung swap already has
> today (`apply_sector_evictions` runs on the app thread a frame or two after
> the publish, which this plan's own text notes is accepted); what it is NOT is
> the seconds-long artifact the design was written to prevent, because the
> parent is never torn down early and never held past the last child.
>
> Chosen over the engine-side version because it needs no changes to the publish
> job, the completion pool, `PublicationTransaction` or the coordinator — the
> highest-risk surface in the plan — and because it is testable headless: the
> gate below flies a camera for 220 ticks at a deliberately starved 3 publishes
> per tick and probes every world column near it after every tick. Zero
> uncovered columns; with the hold removed, 225,248. Closing the remaining
> one-to-two-frame overlap is the deferred-commit work, and it is now an
> optimisation on top of a correct invariant rather than a prerequisite for one.

- Streamer: group ledger for split/merge (4-publish/1-evict and mirror),
  per-group cooldown on member failure, abandonment when the desired map
  moves on. `peek/commit_evictions` semantics preserved.
- Engine: the publish job learns a *deferred-commit* mode — prepare (stage,
  `commit_staged`, Vulkan part registration) as today, park; a group-complete
  app-thread job applies one batched `WorldDelta` (all adds + all removes,
  `matter_engine.cpp:4772-4775` already applies a delta; it just gets more
  than one entry) and settles all the completions. Rollback of parked members
  goes through the existing `PublicationTransaction` path.
- 1:1 swaps (edge-mask/tier rebakes) keep the existing immediate path.
- Touches: `sector_streamer.{h,cpp}`, `matter_engine.cpp`,
  `streaming/sector_streaming_coordinator.{h,cpp}` only if the tagged-request
  bookkeeping needs a group id (prefer deriving groups in the session layer
  so the coordinator stays untouched and `run-sectorcoord` stays frozen).
- Tests: `run-sectorstream` group ordering (no eviction before the fourth
  publish; failure holds the parent; abandonment releases); `run-worldstream`
  end-to-end split with an injected member failure (prove the hole: disable
  the group gate and watch the invariant test go red, then re-enable).

## WP5 — WorldSector scatter sub-celling *(bake-time change, bitwise-gated)*

- `build()` wraps the scatter body in a loop over the tile's 64 m virtual
  cells: per cell, today's RNG seed formula on *cell* coords
  (`WorldSector.js:171-172` unchanged in form), today's candidate rects,
  today's per-cell `FAMILY_CAPS` (`planAlpineSector` called per cell with
  `sectorSize: 64`). Terrain meshing stays whole-tile.
- Gate: at `sectorSize == 64` (every tile today, and every level-0 tile
  later) the emitted child placement list must be **bitwise identical** to
  pre-WP5 — same order, same transforms. Assert with a sector-bake dump diff
  in `run-sectorbake` (the double-bake determinism harness already compares
  artifacts; add a fixed-seed before/after fixture captured pre-change).
- The legacy non-alpine path (`inSector()` grass/rocks) gets the same
  sub-celling; Meadow-family worlds are stale test targets (memory note) —
  verify with FloorDemo/StreamMeadow bakes, not Meadow.
- Touches: `projects/world_demo/objects/WorldSector.js` (and no engine code).
- Tests: `run-sectorbake` bitwise gate; `run-worldstream` still green.
- Visual acceptance: none needed if the bitwise gate holds (that is the
  point of it).

## WP6 — Wire nested mode: flag, profile, VT density *(first user-visible WP, off by default)*

- `world_definition_loader.cpp`: parse `streaming.nestedSectors`;
  `make_streaming_profile` (`matter_engine.cpp:315-396`) forwards it;
  residency bound moves to the last terrain band in nested mode; rings
  become tier-only.
- `MATTER_NESTED_SECTORS` env override alongside the existing
  `MATTER_STREAM_*` family; editor LOD Settings shows the flag and per-level
  counts (`streaming_lod_config`, `matter_engine.cpp:7934-7949`).
- Enable on a small test world first (FloorDemo-class), not StreamMountain.
- Touches: `script/world_definition_loader.cpp`, `matter_engine.cpp`,
  `MatterEditor` LOD settings panel.
- Tests: `run-world-definition` (flag parse + validation);
  `run-worldstream` nested settle on the test world: resident count matches
  the coverage-rule prediction, zero holes (screenshot + manifest audit).
- Visual acceptance: editor on the test world, fly the level boundaries;
  no cracks (WP3's guarantee, now on screen), no popping worse than today's
  band swaps, atomic splits (no z-fight shimmer — WP4's guarantee).

## WP7 — StreamMountain adoption + measurement *(the payoff; pixels move)*

- Set `nestedSectors: true` in `StreamMountain.js`; retune band radii to
  level-quantized values if the boundary quantization reads badly (bands are
  already authored per-world; this is tuning, not code).
- Measure against WP0: resident parts (expect ~78k → ~1.2k), fill wall time,
  update()-tick cost, frame ms and TLAS instance count on the flythrough
  path, peak memory (the 45 GB OOM axis).
- Visual acceptance (the real gate, per the roll-forward memo: re-run the
  real acceptance after removing scaffolding): full flythrough on
  `streammountain_flythrough.path`, compared shot-by-shot with WP0's replay.
  Expected diffs and only these: tier-boundary quantization (grass/rock
  onset shifts by ≤ one level-0/1 tile) and split/merge timing. Terrain
  silhouettes, classification, and scatter placements must match — the
  design says placements are bitwise-stable; hold it to that.
- Deliberately **not** in this WP (deferred with reasons in the design):
  shrinking the in-part QEM ladder (bound_radius/hysteresis coupling — the
  first-rung oscillation lesson, `matter_engine.cpp:4403-4418`); scatter
  beyond-1 km reduction or impostor handoff (`feature/representation`'s
  remit); adaptive-depth refinement.

---

## Sizing (rough, one engineer)

| WP | size | risk |
|---|---|---|
| WP0 baseline | ½ day | none |
| WP1 nested streamer | 3-4 days | medium — the invariants are new; headless-testable |
| WP2 size plumbing | 1-2 days | low — mechanical, replay-gated |
| WP3 seam tests | 1 day | low — expected to pass; if it fails the design stops here, cheaply |
| WP4 transition groups | 3-5 days | **highest** — publish/rollback machinery; keep the coordinator frozen |
| WP5 scatter sub-cells | 1-2 days | low-medium — JS only, bitwise-gated |
| WP6 wiring + small world | 1-2 days | low |
| WP7 StreamMountain + acceptance | 1-2 days + tuning | medium — visual judgement |

Order is fixed by dependency except WP3 and WP5, which can land any time
after WP0 (WP3 has no dependencies at all; land it first if the seam proof
is doubted). WP1+WP4 are the streamer; WP2 is the engine; they can proceed in
parallel branches if needed, but implementation agents run one at a time per
the shared-build-tree rule.
