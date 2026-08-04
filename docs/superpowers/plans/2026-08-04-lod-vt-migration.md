# Migration plan: from the current LOD+VT system to the Representation design

Date: 2026-08-04
Design: `docs/lod-vt-redesign-2026-08-04.md`
Audit of the starting point: `docs/lod-vt-system-walkthrough-2026-08-04.md`

Ten milestones, M0–M9. Every milestone leaves the branch shippable and ends with an
acceptance gate a human can run. Order was chosen to (a) honour the requirement that dead-code
removal precedes everything, (b) deliver the user-visible wins (deterministic pops, visible
impostors, smooth frames) early, and (c) never build migration machinery for a format that a
later milestone deletes.

---

## Execution constraints (carried from this effort's hard-won rules)

- **Roll forward, never revert.** Fix regressions in place.
- **Never `git add -A`.** `MatterEngine3/shaders`, `MatterEditor/shaders`,
  `MatterEditor/shaders_gpu` are NTFS junctions git shows as deleted symlinks; never staged.
- **Implementation agents run one at a time** (shared build tree); read-only investigations
  may fan out.
- **Native exes launch as `TMP="C:/Users/webde/AppData/Local/Temp" TEMP="..." ./exe.exe` from
  a shell without the MSYS2 PATH** — or `fs::temp_directory_path()` falls back to
  `C:\WINDOWS` and fixtures fail as "coherent load failed".
- **Every new guard must be proven failable** — demonstrate the red state before trusting the
  green one.
- **`embedded_spirv.h` conflicts on every merge**; regenerate, don't hand-merge.
- **Visual re-baselines are deliberate events**: capture before/after replays
  (`MATTER_REPLAY`), review the diff, then commit new baselines with the evidence noted in
  the commit message. M1 and M6 intentionally move pixels; M7 intentionally changes the far
  field (fog wall → proxy backdrop).

---

## M0 — Ground clearing *(required first, per requirements)*

Scope — pure deletion plus diagnostics; no behaviour redesign:

1. Delete the confirmed-dead list: `shaders_gpu/cull.comp`, `gpu_cull_types.h`,
   `world_composer.*`, `raster_cull.h::cluster_lod_select`, `tileset_provider.*` (also the
   engine build's last GL dependency), `tileset_macro_slot` + its C API,
   `ResolvedInstance::segment`, `stress_forest_tests.cpp`.
2. Stop generating the write-only artifacts: `.static_lods` + `LMSK` and their ~160 lines of
   probe machinery; the adaptive ladder for terrain sectors (`WorldSector` parts skip
   `bake_adaptive_static_lods`); the duplicate impostor producer.
3. **Log the impostor load failure** (`part_store.cpp` `adopt_object_far_imposter`) and print
   the resident-impostor count in the editor stats overlay.
4. Repair the orphaned impostor links (rebake or re-key the 10-of-20 orphans) so impostors
   load again under current rules.

Acceptance:
- `./build-all.sh test` green; Vulkan smoke suite green.
- StreamMountain cache rebake measured: ~6.6 GB → the neighbourhood of 400 MB.
- **Failability proof:** hand-corrupt one `.impostor` link → exactly one log line names the
  part and reason; stats overlay count drops by one.
- Demo world shows impostors (first time ever verified visually).

Tests retired here: `.static_lods` cache-probe tests, `stress_forest_tests.cpp`, any test
referencing deleted GL selection files.

---

## M1 — Distance authority (runtime selection only; artifacts untouched)

Scope:
1. At stage time, convert each part's existing threshold table into a **distance table**
   (parity mapping: `d_k = radius × scale × pixel_budget / threshold_k`, so visuals are
   unchanged by construction where thresholds were sane).
2. One selection function (`desired_rep`) in a single header, evaluated CPU-side (planning)
   and GPU-side (cull); delete the other five call paths as consumers switch.
3. The impostor becomes rep N of the same table: cull selects it in the same walk; delete the
   terminal-impostor CPU comparison, both per-frame partition state machines, the full array
   copy, and the unconditional instance re-upload; suppressed instances leave the dispatch.
4. Residency clamp `[min_resident, max_resident]` per cluster (groundwork for M2's commits).

Acceptance:
- Shot-replay diff vs M0 baseline within stated tolerance on Demo + PomProofBrick at rep-0
  distances; document and review any far-field diffs (threshold-crowding cases *should*
  change — that is the point).
- **New: fly-through determinism test.** Record a camera path; run twice warm; assert the
  per-frame list of `(instance, rep)` switch events is identical. This test is the permanent
  guardian of R4 and survives every later milestone.
- Frame capture shows the ~14 MB/frame instance copy+upload is gone at 90k instances.
- A far tree costs one impostor draw and zero retained subtree records (GPU capture).

Tests rewritten here: the 11 Vulkan smoke fixtures that encode projected-size constants are
recalibrated to distance constants (they encoded the *old* convention numerically; the new
fixtures should assert distances, which are human-checkable).

---

## M2 — Atomic switches

Scope: residency-gated commits — a rep switch commits only when geometry + BLAS + (for now)
its per-rung pages are all resident; batched commits; hysteresis bands; evict-after-commit
with grace frames. Terrain keeps ring-boundary snapping but gains the same
whole-or-nothing commit.

Acceptance:
- **New: pop-coherence test.** Instrument commit events; drive a scripted approach; assert
  mesh rep, VT residency and BLAS selection for a given instance change on the *same frame*,
  for every switch in the run.
- **Degradation test:** artificially throttle the bake/IO workers; assert switches are
  delayed but never partial (no frame draws rep k geometry with rep j pages), and the switch
  event list is unchanged in *order* vs the unthrottled run.
- Fly-through determinism test still green.

---

## M3 — DSL recipes and lazy staged baking

Scope:
1. `lods(p)` on `Part`, `LOD.*` library generators, demand-driven stages with
   content-addressed memoization + dependency traces (design §3.2), per-rep source hashing,
   the explicit `LOD.impostor`/`LOD.vanish` terminals with their placement validation.
2. Default recipe = current adaptive ladder wrapped as a generator, with **measured**
   geometric error replacing the synthetic `prior + 0.9 × remaining` schedule, and default
   distances derived from it. Skinned parts move off hardcoded `BakeTargets` onto a default
   recipe.
3. **Lazy per-rep bake**: the streamer requests `(part, rep)`; nothing else generates.
4. Editor: show each part's effective distance table; one-click copy of defaults into script.

Acceptance:
- **Golden-bake test:** a scripted part (tree with a `skeleton` stage and a custom card rep)
  bakes deterministically twice → bit-identical rep blobs; editing only rep 2's builder
  re-bakes only rep 2 (stage memo hit counts asserted). A second fixture implements the
  design §3.2 worked example — physics-settle stage feeding a placement stage consumed
  differently by two reps — asserting the settle runs exactly once across all rep bakes and
  reloads from the store on a cold process.
- Existing worlds render unmodified under default recipes (replay parity vs M2 baseline,
  bounded diff at far distances where the fixed error schedule legitimately changes rungs —
  and branch LODs now visibly step through >2 usable rungs, the original complaint).
- Initial StreamMountain enter-time measured against M2: lazy baking must not regress it and
  should improve it (far ring bakes far reps only).

Tests retired here: threshold-formula unit tests for the four deleted formulas; the
epsilon-search duplication tests. Tests added: stage memoization, per-rep invalidation.

---

## M4 — Artifact consolidation and the version vector

Scope: the **PartBundle** (manifest + per-rep blobs + parameterisation + impostor atlas +
stage outputs) replaces `.part`/`.lods`/`.fimp`/`.impostor`/`.hints`; one **version vector**
defined in one place and folded into every cache key (closes the part-resolved-hash hole
permanently). One-time full rebake.

Acceptance:
- Double-bake determinism: two cold bakes of Demo produce bit-identical bundles.
- **Failability proof of the version vector:** bump the representation version → every part
  re-resolves *and re-bakes* (the exact scenario that silently failed before); revert →
  clean hits.
- No `.lods`/`.fimp`/`.impostor`/`.static_lods` files are written anywhere (asserted by a
  test that bakes a world and inventories the cache directory).

---

## M5 — MatterStore

Scope, in two halves:
1. **Standalone `libs/AssetStoreLib`**: BlobStore (append-only packs, atomic index swap,
   checksums, compaction), RefTable (semantic keys, LRU vs disk budget), async batched read
   API (IOCP), MemoryLib-arena landing. Own unit tests + benchmark harness. *(This half has
   no engine dependencies and may be developed in parallel any time after M0; it lands here
   so it never stores the pre-M4 formats.)*
2. **Cache adoption**: PartBundle blobs, resolve/settle refs, decoded-BC `.gtex` mirror move
   into the store; sector blobs written contiguously; the old per-file cache tree becomes
   read-never.

Acceptance:
- Lib tests: crash-mid-write recovery (kill during append → reopen → no corruption, blob is
  a miss), eviction to a disk budget, concurrent reader soak.
- Benchmark: warm-sector read via store ≥ 5× faster than the small-file storm on the same
  data (the number that justifies the subsystem — publish the measured figure in the doc).
- **Instant-revisit acceptance (R7):** fly StreamMountain out and back; on return, zero bake
  jobs run and sectors publish from the store within the main-thread budget. Record cold vs
  warm enter times.

---

## M6 — Texture unification *(second deliberate visual re-baseline)*

Scope: one parameterisation per part (chart rep 0, reproject other reps at bake); page pool
becomes a per-part mip chain composited per `(part, mip)`; horizon sampled in the tile frame
via the rung-invariant basis; occlusion reduced to the two-term contract; warp solved once
per sector on rep 0, per-rung reprojection deleted. M2's commit gate switches from per-rung
pages to mip residency.

Acceptance:
- **The dome dark patches are gone** (the standing PomProofBrick repro; boundary no longer
  tracks the rung split).
- **Texture-persistence test:** force a rep switch under capture; assert page texels for the
  shared parameterisation are unchanged across the switch (geometry pops, texture does not).
- Resident page count per sector drops by ~the rung count (measured).
- Occlusion ablation: exactly two contributors (bake one to white → only the RT term
  remains; disable RT → only the baked term).
- Replay re-baselined with before/after evidence.

---

## M7 — Proxy world and visibility-gated streaming

Scope (design §6):
1. **Proxy rep per sector**: coarse terrain march at proxy pitch, plus its **eroded
   occluder variant** baked alongside; whole-world proxy pass at world open; stored
   contiguously in the store; always resident. Retires the terrain far-field `.fimp`
   impostors and the fog-wall ring trim — beyond the rings you see proxy, not fog.
2. **Visibility**: proxy depth → HiZ pyramid → per-sector classification
   (visible / offscreen / occluded), widened predictive frustum, hysteresis on recently
   visible.
3. **Priority**: streamer work order becomes (visibility class, distance) — visible
   nearest-first; occluded-beyond-threshold sectors are neither baked nor read nor
   resident.
4. **Proxy↔real swap** rides the M2 commit machinery: proxy draws exactly where no real
   rep is committed; the handover is atomic — never both, never a gap.

Acceptance:
- **Canyon test (new):** a scripted canyon fly-through in StreamMountain. Assert from job
  logs that occluded-side sectors never bake and never load; total bake+IO measurably below
  the same path with occlusion disabled; and no visual hole when cresting the rim — the
  proxy covers until real sectors commit, nearest-first.
- **Erosion-safety proof (failability):** run with the *un-eroded* proxy as occluder → the
  test must catch a false occlusion (a hole); with erosion → none. Proves the conservative
  direction is doing the work, not luck.
- Whole-disc proxy metrics recorded: cold bake time for all 6547 StreamMountain sectors,
  warm open is O(1–2 sequential reads), resident proxy memory within its stated cap.
- Fly-through determinism test extended with the canyon path: commit order identical
  across runs (visibility is a pure function of pose against a static proxy).
- Far-view screenshot evidence: fog wall replaced by proxy backdrop, shaded in-family from
  the tape.

---

## M8 — One world path

Scope: closed worlds boot through the streamer (`residency: all`, finite grid, no eviction);
`compose_world` deleted; one compiled `WorldProgram` (explicit world export replaces the
regex; per-sector isolates from bytecode); "sector" renamed to one meaning
(`LocalityGrid` for the pitch-16 grid).

Acceptance:
- Demo, Meadow, Primitives, FloorDemo boot via the streamer, replay-identical to M7
  baselines once resident.
- `run-world-definition` rewritten for the streamed path; no `find_world_class_name`
  remains; a world without an explicit export fails with a clear error (failability proof).
- Closed-world boot time within noise of the prior milestone (the streamer with
  `residency: all` must not be slower than the manifest it replaced — warm store from M5
  does the heavy lifting).

---

## M9 — Budget governor and final acceptance

Scope: the single main-thread queue with per-frame ms accounting; `stream.publish` rebuilt as
resumable slices; RAM/VRAM/disk budgets enforced (farthest-first eviction, proxy tier exempt);
M7's visibility priority folds into the unified queue; all remaining per-system knobs either
deleted or demoted to debug-only.

Acceptance:
- **Frame-time conformance:** StreamMountain scripted fly-through; p99 of streaming-side
  main-thread time ≤ `main_thread_budget_ms`; no publish-spike frames (the audit's "one slow
  publish blows a frame" is instrumented and asserted extinct).
- **Budget conformance:** run at half RAM/VRAM/disk budgets → completes without OOM, with
  measurably smaller residency, and the fly-through determinism test *still* produces the
  same switch order (budgets shape when, never what).
- Full suite: build-all, smoke, replay, determinism, pop-coherence, golden bakes, store
  tests — all green. Update `CLAUDE.md`/`ROADMAP.md` to describe the new system.

---

## Test-suite reconciliation

| Suite / test | Fate |
|---|---|
| Fly-through determinism (new, M1) | **The keystone test** — must pass every milestone after M1 |
| Pop-coherence + degradation (new, M2) | Permanent |
| Golden staged bake (new, M3) | Permanent |
| Store crash/eviction/bench (new, M5) | Permanent |
| Texture-persistence, occlusion ablation (new, M6) | Permanent |
| Canyon visibility + erosion safety (new, M7) | Permanent |
| Budget conformance (new, M9) | Permanent |
| Shot replay (`MATTER_REPLAY`) | Keep; re-baseline deliberately at M1, M6, and M7 (far field) only |
| `.gtex` double-bake determinism | Keep unchanged |
| Vulkan smoke suite (~16 modes) | Keep; 11 threshold-constant fixtures recalibrated to distances at M1 |
| `run-world-definition` | Rewrite at M8 for the streamed path |
| `.static_lods` probe tests, `stress_forest_tests.cpp`, GL-selection tests | Retire at M0 |
| Threshold-formula / epsilon-search unit tests | Retire at M3 with their formulas |
| `demand_bake_tests`, `script_host_tests` (`lodPolicy` surface) | Rewrite at M3 against `lods()`; `lodPolicy` becomes the default-recipe config and keeps a thin compat shim until M4 |

## Risk register

- **Three deliberate visual changes (M1, M6, and M7's far field).** Mitigation: replay
  evidence reviewed by Jack before baselines move; everything else must be pixel-stable.
- **M4's one-time full rebake** invalidates every cache on every machine. Schedule it; pair
  it with M5 so the rebuilt cache lands directly in the store.
- **Parity mapping in M1 depends on `pixel_budget = 2.01`** (the shipped value — not 1).
  The conversion must read it from the live config, not assume it.
- **Lazy baking changes *when* bake cost is paid** — watch for hitching at rep boundaries
  before M9's governor lands; M2's clamp already guarantees correctness (old rep holds).
- **False occlusion is the one proxy failure that costs correctness** (a hole instead of
  wasted work). The erosion-safety gate is the guard; it must be proven failable with the
  un-eroded proxy before M7 ships. Proxy staleness against world edits is covered by the
  version vector.
- **The streamer becomes load-bearing for closed worlds at M8** — its bugs get a larger
  blast radius; the M2/M9 instrumentation must land first, which the ordering guarantees.
