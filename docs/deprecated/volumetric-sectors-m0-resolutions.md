# M0 design resolutions (decisions made during implementation)

These resolve friction between `docs/volumetric-sectors-design-2026-08-10.md` and the
code as it actually is. Fold into the design doc when M0 lands.

## R1 — The weld-ready visibility gate is circular; drop it, build welds in-transaction

**Design says** (§4.2): "A tile becomes visible only when the welds along its cross-level
faces are built — one more clause in the parking predicate."

**Problem found in the code:** a weld needs *both* sides drawn, and "drawn" is
`!parked && resources.world_state_attempted`. So tile A would wait for a weld that cannot
be built until A is drawn. The predicate would need a "pending-drawn" notion that neither
`parked` nor `world_state_attempted` expresses.

**Resolution:** do not add the weld-ready clause. §4.2 itself says a weld is
"tens-to-hundreds of quads; building one is microseconds on the app thread" and offers the
gate only so the build need not be synchronous with publish. Build it synchronously
*inside* the same transaction that makes the tile visible. Then weld geometry and drawn
state are atomic by construction — which is the actual requirement (§4.1: "weld add/remove
must be transactionally coupled to the publish/evict batch it belongs to") — and the
circularity never arises.

Revisit only if measurement shows weld build on the publish path (it is O(1) per publish,
see R2). The drawn-±1 gate of §4.5 half 2 is a *separate* clause, is not circular, and
still lands.

## R2 — `rebuild_welds_for(key)` fans out internally; that is where O(1) is won

A publish of tile K changes the drawn neighbourhood of K's face neighbours too. The call
sites pass one key; the welder enumerates from it:

- 4 faces in M0 (±x, ±z), 6 in M2.
- Per face, the neighbour is either one tile at K's level, one coarser tile, or (in 2D)
  2 finer tiles — bounded by the 2:1 `restrict_levels` invariant.

So ≤ ~12 face pairs touched per publish, independent of world size. **This bound is the
whole reason the seam pool does not become the next `issues/bfb5f13e` per-publish
`O(world)` cascade.** Any implementation that sweeps `sector_map` per publish is wrong.

## R3 — Seam geometry ships as one content-addressed part per face pair (v1)

**Design says** (§4.2): "one engine-owned dynamic vertex/index pool (a 'seam part')".
**Design also says** (§4.1 consequence 4): "v1 needs zero renderer changes."

**Problem found in the code:** `VkSceneRenderer` has no in-place part mutation.
`release_part` + `ensure_part` under a new hash is the only update path; `registered_part_slot`
early-outs make re-registering the same hash a no-op. A true dynamic pool is a new renderer
capability, not plumbing.

**Resolution:** v1 = one content-addressed part per cross-level face pair, registered
through the existing `ensure_part` path, instance id content-derived with the `0x80000000`
range tag (`sector_instance_id`'s discipline — never a counter, the determinism gate at
`matter_engine.cpp:1267-1310` depends on it). Naturally incremental per face pair, which is
what R2 requires. The dynamic pool becomes an optimization once measured.

**Hard check before this lands:** part/instance count growth against the TLAS instance
ceiling — `TLASManager::draw` drops instances past its ceiling with only a `printf`
(known trap). Count it, do not hope.

## R4 — A missing record is fail-soft, not a welder bug

The disk fallback (`staged_load->ok == false` → `get_or_load`) yields no boundary record —
the `.part` artifact deliberately carries no seam section, since the bake can always
recompute it. Such a sector draws with today's overlap behaviour and no weld.

Consequence for debugging: **the absence of a weld is not by itself evidence of a welder
bug.** `WeldStats` must count records-missing separately from `missing_landing`, or a
future bisect burns on this (cf. the "invalidate_part logged as a failed bisect" lesson).

## R6 — Staged refinement costs +33% in 2D, not +14%; measured +18.9%

**Design says** (§4.5, §7.9): intermediate waves cost `Σ 8⁻ᵏ ≈ +14%` worst case.

**That is the volumetric number and does not apply to M0.** The `8×` assumes one level
coarser is 8× the volume per tile — true once Y is tiled (M3). M0's descent is still the
2D quadtree, where the same argument gives `Σ 4⁻ᵏ = 1/3`, so the honest M0 ceiling is
**+33%**. Measured on a four-level teleport: **+18.9%** (1296 requests staged vs 1090
unstaged). The test asserts `< 1.35` — pinned at the derived ceiling rather than tight to
the measurement, so band tuning does not make it flaky. +14% arrives with the third axis.

## R7 — Two behaviours staged refinement introduces that nothing else in the tree expects

1. **The resident set can transiently stack three levels over one column** (worst
   simultaneous depth 2 → 3), even though total residency overlap *fell* 46098 → 30646
   probe-ticks. Residency is not drawn-ness — parking is what prevents double-draw — but
   the drawn-±1 gate must not assume ±1 holds in the drawn set merely because staging is
   on.
2. **A quiet update no longer means "settled".** A wave is admitted by the *absence* of
   coarser residency, and the coarse tile is evicted at the *end* of the update in which
   its replacement quad completes — so there is exactly one silent update per wave. The
   engine pumps every frame and never notices, but any harness looping "update until no
   requests" stops mid-jump (this one did, at 852 of 1188 tiles, until it waited for 8
   consecutive silent updates). Anything else in the tree that settles that way is now
   wrong.

Also, answering the clamp with the **coarsest** resident ancestor rather than the finest
is load-bearing, not a style choice: the finest answer let wave *k+1* be requested before
wave *k* retired anything, holding the coarse tile through every remaining wave — 68 ticks
of dwell against the 1 that is inherent, and the drawn set ended up holding exactly the
multi-level spread staging exists to prevent.

## R8 — §4.1's "v1 needs zero renderer changes" is true; "zero ENGINE render changes" is not

`WorldSession::render`'s expansion loop resolves every manifest entry through
`store->get_or_load` and `continue`s on null. A weld has no PartStore artifact **by design**, so
every weld instance was silently dropped — the one way this stage could have failed invisibly.
Fixed with a `weld_parts` lookup ahead of `get_or_load`, in `matter_engine.cpp`; no renderer file
was touched, so the doc's actual promise holds.

Related, and the same shape of hazard: the record's route from the mesher to the engine runs
`j_terrainVolume` → `DslState::set_sector_boundary` → `BakedGeometry` → `stage_from_bake` →
`LoadedPart` → `SectorEntry`. **Every seam test drives `mesh_sector` directly and never touches the
DSL path**, so a missing `set_sector_boundary` call leaves the entire runtime welder as dead code
with the whole suite green. There is no test that would catch it. If the welder ever reports
`drawn_without_record` for everything, check that call first.

## R9 — The overlap band is copied per `weld_face` CALL, not per pair

`weld_face` copies the fine side's band once on entry, before it walks the region — the band is not
indexed by anchor, so there is nothing to clip it against. The engine calls `weld_face` once per
contiguous **run of anchors**, so handing the band to every call stamps coincident copies, once per
run. That is invisible to a coverage probe (the copies are exactly coplanar) and shows only as
z-fighting plus a triangle count that scales with how fragmented the record happens to be. The band
must ride exactly one call per fine tile, and a tile whose face record has a band but no verts needs
its own empty-region call or its band is dropped.

## R13 — The lateral staging term closes the REFINE direction; merge stays open

R12's 28 violations were the refine direction. The footprint clamp deliberately ignores
neighbours; nothing stopped a tile refining two levels ahead of a still-drawn lateral neighbour.
Added a third term to the `descend()` clamp:

```
effective_level = max(desired,
                      resident_level_over(own footprint) - 1,
                      coarsest face-neighbour resident level - 1)
```

Expressed as a stop condition only the *existence* of a coarser resident neighbour is needed, so
it is four ancestor chains with an early-out, `4 × (max_level - level)` lookups on nodes the
descent was going to split anyway. Measured `update()` cost +2.6/+2.7 %.

**It cannot fight the band gradient or stall the fill, structurally:** the term is
`neighbour - 1`, hence a no-op on any ±1-balanced residency, and every settled state is
±1-balanced (settled residency = the desired map, which `restrict_levels` balances). It can only
fire on a transient. Measured: settled tiles and *minimum residency during the fill* are
byte-identical across unstaged / footprint-only / +lateral (1188 and 1141). It costs path, not
tiles — +0.3 % requests on top of the footprint clamp (total staged overhead +18.9 % → +19.3 %
against R6's derived +33 % 2D ceiling).

**Deadlock-free by a strict order:** at the coarsest resident level M anywhere in reach, a node
can have no neighbour at M+1 or coarser, so it is always free to split; M then falls. A tile only
ever waits on something strictly coarser than itself.

Harness gate: refine gaps **21341 → 0**, with `CHECK(footprint_only.refine_gaps > 1000)` as the
failability arm. Note the test needs a publish **budget** — with instant service every transition
group completes in its issuing tick, neighbours are in lockstep for free, and the defect cannot
appear. That is why every pre-existing streamer block was green while the real world was not.

**Still open — the merge direction (~2900 gaps, unchanged).** A multi-level *merge* lands one
coarse tile while the neighbouring footprint still draws fine tiles. The lateral term withholds
*requests*, and on a merge the level in question is the one already being asked for, so there is
nothing to withhold. Staging the coarsening direction is not the answer: intermediate merge waves
are *finer* than the target, so a four-level merge costing one bake today would cost 4+16+64.
**R1's coverage objection does NOT apply here**, which is the useful part: at the moment a merge
target publishes, its own footprint is still fully covered by the fine tiles the streamer has not
yet evicted, so parking the coarse newcomer until its faces are within ±1 withholds nothing that
is not already drawn. That is the engine-side fix, and unlike the refine case it is available.

Also measured, confirming R7's warning: footprint staging **alone** sometimes shows *more* drawn
violations than staging fully off (21341 vs 16462) — it can widen the drawn spread it exists to
narrow. Only the lateral term closes it.

## R14 — Two spurious causes of weld-pool churn, one of them a determinism hole

Chasing R12's `noop_rebuilds : parts_registered` = 0.9 : 1 found three things, and the header's
expectation was itself wrong for the fan-out that got built (`rebuild_welds_for` is *precise* —
it touches only pairs the published tile is a side of — so a rebuild nearly always coincides with
a real change). `parts_registered` also conflates pair *creation* with pair *churn*: ~2
registrations per live pair is the lifecycle (band sweeps on; the 2:1 fine side arrives one
sibling at a time), not re-welding. Split into `pairs_created` / `pairs_recontented`.

The two real bugs:

1. **The content hash was not a function of the geometry.** `weld_part_hash` walks
   `mesh.buckets` in order, and that order came from the process-wide `seam_scratch`, whose
   `clear_geometry()` deliberately keeps its buckets (a capacity optimisation) — so `bucket_for`
   reused whatever material slots earlier welds had created, in whatever order. Identical
   triangles could hash differently. This is a hole straight through R3's premise, which assumes
   identical geometry ⇒ identical hash. Fixed by sorting buckets by material before hashing.
2. **The scatter tier was inside the pair identity.** `WeldPairKey` carried the coarse tile's
   packed rung, whose low four bits are the scatter detail tier — but a boundary record depends
   on the terrain LOD alone. A tile crossing a *scatter ring* republished under a new variant and
   retired/recreated every weld on its coarse faces with byte-identical geometry (the `reg=1
   rel=1` flicker). Re-keyed to `(tx, tz, level, face)`. Honest bound: StreamCaverns declares one
   scatter ring and is immune, so this cannot explain its ratio.

New counter `content_changed_inputs_stable` — a live pair whose input fingerprint (the identity,
never the pointer, of every record consulted including diagonals that were *looked for and not
found*) did not move but whose geometry did. It must be 0; non-zero means a further
nondeterminism.

## R12 — M0 acceptance soak: the welder passes, the drawn ±1 invariant does not

Settle-then-fly, `MATTER_SEAM_TRACE=1`, `MATTER_CAM_PATH_WARMUP=4000`.

| | StreamCaverns (volumetric) | StreamMountain (heightfield) |
|---|---|---|
| samples / settled residency | 6454 / 590 | 7426 / 778 |
| pairs = drawn = parts | 97 | 92 |
| weld triangles | 333,375 | 45,566 |
| `sign_conflicts` / `level_gap_pairs` / `build_errors` / `id_collisions` | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| identity, `drawn_pairs <= pairs` | held every row | held every row |
| `level_holds` | 73 | 2 |
| **`drawn_level_violations`** | **28** (first #f818) | **0** |
| `noop_rebuilds : parts_registered` | 0.9 : 1 | 0.8 : 1 |
| engine `[stream]` violation warnings | 21 | 0 |

**The seam welder is sound.** Zero sign conflicts across ~7k samples in two worlds is direct
evidence both sides of every plane read the same world samples; `level_gap_pairs = 0` means the
welder correctly *rejects* every >1-level face rather than approximating it; `drawn_pairs ==
pairs == registered_parts` on every row means the fail-soft draw gap never opened.

**M0's stated acceptance is still NOT met.** StreamCaverns logs 28 tiles drawn beside a face
neighbour **two** levels away — `sector (3,-2 r2096) shown at level 2 beside a DRAWN level-4
face neighbour … this face gets no seam`. Those faces draw unwelded, which is the missing-strip
class the stage exists to remove. The cause is upstream of the welder: **staged refinement does
not guarantee the ±1 invariant in the DRAWN set under fast flight**, and per R1 the engine-side
park cannot fix it (it is subordinate to coverage). The fix is streamer-side — defer the
parent's eviction on a lateral level conflict — and `drawn_level_violations` is the number to
drive to zero.

Two secondary findings worth acting on:
- **`noop_rebuilds` is ~0.9 : 1 against `parts_registered` in both worlds.** `world_session.h`
  calls this "the one to watch" and expects almost every rebuild to be a no-op in steady state.
  Roughly half instead churn a renderer part — ~1 part registration+release per few frames on
  the publish path, which is R2's stated concern showing up in practice.
- **A volumetric world costs ~7× the weld geometry per pair** (333k tris over 97 pairs vs 45.5k
  over 92): a volumetric plane has far more crossings than a heightfield one. Matches the
  harness measurement that the cave fixture carries ~5× the crossings per plane.

Only StreamMountain reproduced run-to-run. StreamCaverns is sensitive to how settled it is (R10),
so its numbers must always be quoted with residency.

## R10 — A soak on an UNSETTLED streaming world passes vacuously. Settle first.

The single most important thing learned running M0's acceptance soak, and it invalidated the
first three attempts.

`MATTER_CAM_PATH` moves the camera one pose per rendered frame. The authored StreamCaverns
path runs at 3.0 m/pose. StreamCaverns is the voxel stress world (~590 samples per column at
the 2 m rung), so at that speed **the camera outruns the baker completely**: `inflight_sectors`
pins at its cap, `resident_sectors` sticks around 52–73 against a settled value of **452**, and
tiles are evicted before their neighbours arrive. With no stable cross-level neighbours there
is nothing to weld — `pairs` sits at 0 — and **every invariant passes because the welder never
ran**. The summary prints `VERDICT: PASS`.

The poller's `NO SAMPLES — the world never drew` guard does **not** catch this: there were 2770
samples. There just was not a world.

Measured, same path, same binary, only `MATTER_CAM_PATH_WARMUP` changed:

| | warmup 300 (unsettled) | warmup 4000 (settled) |
|---|---|---|
| `resident_sectors` | 52 → 73, never stable | **452, stable** |
| `pairs` peak | **1** | **72** |
| weld triangles | 1,161 | **262,794** |
| verdict printed | PASS | PASS |

Only the second is a measurement. 72 pairs also matches an independent geometric estimate of
50–70 for this band profile derived from the band radii alone — two methods agreeing.

**Any future seam/streaming soak must settle to a stable `resident_sectors` before it moves,
and must report residency alongside its verdict.** A pass with a residency far below the
settled value is a null result wearing a pass's clothes.

## R11 — Two counters that read wrong

1. **`sect` (resident sectors) is not in `MATTER_SEAM_TRACE`'s change key.** Rows print only
   when a *seam* field changes, so residency can move freely without emitting a row. Absence of
   rows means "no seam change", never "nothing happened".
2. **`SeamWeldStatus::coarse_side_nulls` is documented as "live pairs, not cumulative" but is a
   per-sample sum.** `matter_engine.cpp` assigns `rec.coarse_side_nulls = cctx.nulls`, and
   `cctx.nulls` increments once per failed landing lookup — hence 9,054 against 72 pairs. Read
   as a pair count it is nonsense. Fix the comment or the field; a bisect will misread it.

## R5 — `edgeMask` removal invalidates less cache than expected

Not "every sector hash changes": `WorldSector.js` still declares `edgeMask: 0` in
`static params`, so interior tiles (mask already 0) keep their hashes. Only tiles that
carried a **non-zero** mask — i.e. tiles on a level boundary — rehash. Cheaper cold start
than feared.
