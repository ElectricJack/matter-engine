# Migration plan: from the current LOD+VT system to the Representation design

Date: 2026-08-04
Design: `docs/lod-vt-redesign-2026-08-04.md`
Audit of the starting point: `docs/lod-vt-system-walkthrough-2026-08-04.md`

Twelve milestones, M0–M9 with M1.5 and M2.5 inserted as work reshaped them. Every milestone leaves the branch shippable and ends with an
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

> **Base decision (2026-08-04).** This work starts from **main**, on `feature/representation`
> in `.worktrees/representation` — not from `codex/far-field-impostors`. Main has no
> `bake_adaptive_static_lods` and no impostor files at all: those 134 branch commits *are*
> the adaptive-ladder + far-field-impostor programme, which M1/M3/M4/M7 replace. Starting
> from main means never merging code in order to delete it, and several of the branch's
> hardest fixes (the stacked-shell staging bug, the `.gtex` version bump, the archive
> repairs) exist only to repair damage that programme caused, so they are moot rather than
> lost. The branch remains in git as reference; its durable output — the three design
> documents — is cherry-picked here.
>
> **Consequences for this plan.** Original M0 items 2–4 (terrain adaptive ladder, impostor
> load logging, orphaned-link repair) describe branch-only code and **do not apply**. M0 is
> correspondingly smaller. M1 now *builds* the impostor as rep N rather than converting an
> existing one, which is strictly simpler — we implement it once, correctly, instead of
> repairing a system we were about to restructure.

Scope — pure deletion; no behaviour change at all:

1. **Delete the unreachable GL path.** Verified 2026-08-04: `MATTER_VULKAN_ONLY` is defined
   unconditionally by every build (`MatterEngine3/Makefile:74`,
   `MatterEditor/Makefile:337`, `MatterEngine3/tests/Makefile:422`), so every
   `#ifndef MATTER_VULKAN_ONLY` block is provably unreachable — 23 sites in
   `matter_engine.cpp`, 3 in `local_provider.cpp`, 1 each in `vulkan_only_compat.cpp` and
   `tileset_bake_vk.cpp`. Removing them strands, and so also deletes:
   `world_composer.{cpp,h}`, `gpu_cull_types.h`, `shaders_gpu/cull.comp`,
   `tileset_provider.{cpp,h}`, `raster_cull.h::cluster_lod_select`. Drop the corresponding
   objects from `ME3_OBJ`.
2. ~~**Stop generating the write-only artifacts**: `.static_lods` + `LMSK`.~~
   **WITHDRAWN 2026-08-04 — neither is write-only. Verified, not assumed:**

   - **`.static_lods` is a working bake cache, not a dead artifact.**
     `HostBaker::bake_static_lods` (`part_graph.cpp:730-744`) reads the sidecar back
     through `load_static_lod_plan`, checks every referenced `.part` still exists, and
     `return true`s — skipping the whole per-level rebake. The audit's wording ("the only
     reader is the writer's own cache probe") is literally accurate, but a cache probe *is*
     a reader: deleting it would force every static-LOD part to rebake on every bake. The
     surrounding function is also live work — it is gated on a part opting in via
     `eval_lods`, and it writes real per-level geometry.
   - **`LMSK` is a trailer inside the `.part` format**, with save, load, skip-on-load and
     byte-identical-when-empty compatibility paths in `part_asset_v2.cpp` plus round-trip
     tests. Removing it is a format change forcing a version bump and full cache
     invalidation. It is also written *by* `bake_static_lods` (`part_graph.cpp:858`), so the
     two are coupled.

   Both are folded into **M4**, which rewrites the artifact set into PartBundle and
   invalidates every cache anyway — there they cost nothing. This keeps M0 honestly what it
   claims to be: deletion of provably unreachable code, with no format, cache, or behaviour
   change. Three of the audit's §4.5 "confirmed dead" entries have now failed verification
   (`world_composer`/`tileset_provider` were reachable-looking but macro-dead, and needed
   the enclosing blocks removed first; `ResolvedInstance::segment` and `tileset_macro_slot`
   are still written; `.static_lods` and `LMSK` are live) — treat that list as leads, not
   findings.
3. Delete `stress_forest_tests.cpp` (dead), and update `viewer_logic_tests.cpp` /
   `tileset_provider_tests.cpp`, which are the only remaining includers of the deleted
   headers.

**Corrections to the audit's §4.5 list, established by verification rather than assumed:**
`world_composer` and `tileset_provider` are *not* unreferenced — `matter_engine.cpp`
constructs `WorldComposer` at four sites and `local_provider.cpp` calls
`tileset_provider::unload_all()` at two. Every one of those sites is inside
`#ifndef MATTER_VULKAN_ONLY`, which is what makes them dead; deleting the files without
removing the guarded blocks would not compile. **`ResolvedInstance::segment` is written at
three sites in `resolvers.cpp` and was NOT confirmed dead** — it is deferred out of M0 until
a reader search proves it unread (`segment` appears widely on unrelated types, e.g.
`FlatCluster::segment` and `rigid_segments`, which is how the audit's claim went wrong).
`tileset_macro_slot` likewise has live references in `material_registry.{h,c}` and
`vk_gi_contract.h` and is deferred pending the same proof.

Acceptance:
- Kernel + editor build green; headless suites (`run-world-definition`, `run-script`,
  `run-evalworld`) ALL PASS; Vulkan smoke suite at its known baseline.
- **M0 is pure subtraction of unreachable code, so a shot replay must be pixel-identical
  to the M0 baseline.** Any pixel diff means something deleted was reachable — investigate,
  do not re-baseline.
- `nm` the archive: no `world_composer` / `tileset_provider` / `cluster_lod_select` symbols
  remain.

Baseline recorded 2026-08-04 on `feature/representation` before any deletion: kernel builds
clean; `run-world-definition`, `run-script`, `run-evalworld` all report ALL PASS.

**Known-red baseline — `run-viewer-logic` fails on pristine main.** One assertion,
`test_partstore_cluster_loading` → `FAIL: cluster test: v3 flat part loads`, preceded by
`PartStore: coherent load failed for c1c2c3c4d1d2d3d4` on a freshly written `save_flat_v3`.
Attributed by running the suite on an unmodified `main` checkout (2026-08-04): the failure
set is **byte-identical** to `feature/representation`'s — same single FAIL, same pass count.
So this suite is red before this effort starts and must not be read as a regression signal
until it is fixed on its own terms. Note it is *not* the TMP/TEMP sandbox trap: it
reproduces with `TMP`/`TEMP` passed as the exe's own env prefix. Two cautions for whoever
picks it up: the test exes are dynamically linked, so they need the MSYS2 UCRT64 PATH *and*
an explicit `TMP`/`TEMP` prefix (the static editor needs neither); and piping a suite run
through `tail` hides the FAIL line, which is why the first attribution attempt was
inconclusive.

Tests retired here: `stress_forest_tests.cpp`, `tileset_provider_tests.cpp`, the
`.static_lods` cache-probe tests, and the GL-selection portions of `viewer_logic_tests.cpp`.

---

## M1 — Distance authority (runtime selection only; artifacts untouched)

> **Status 2026-08-04 — selection unification COMPLETE; only M1d (the determinism gate)
> remains.** Landed as `583bc17b` (the rule + its equivalence proof), `e2a3c980`
> (radius normalization), `daca884b` (GPU + both CPU mirrors), `b43b81fc` (smoke fixture),
> `5c4d4bc8` (the `lod_select` sector family) and `045ff57c` (the zero-reach contract fix).
>
> A repo-wide sweep now finds exactly ONE `radius * scale / distance` site left —
> `vk_scene_renderer.cpp`'s VT *fetch priority*, which is a ranking rather than a selection
> and correctly keeps that form. Every actual selection goes through `lod_distance.h`.
>
> Three things worth carrying forward, because each was found the hard way:
>
> - **There were THREE CPU copies of the shader's pick**, not two — the mesh rung, the
>   planning mirror, and a VT rung-demand loop nobody had counted. The replay gate caught
>   the third at 6.611 % against a 0.069 % floor, after a mechanical field rename silently
>   turned `projected_size >= threshold` into `projected_size >= switch_distance`. That
>   compiles cleanly (both are floats) and compares incommensurate quantities. **Every
>   suite stayed green.** After any rename in this area, re-run the pixel gate.
> - **Radius must stay a RUNTIME factor.** `cull.comp` computes `local_radius` per instance
>   for dynamic-bound clusters, a value that does not exist at bake time; folding a baked
>   radius into the stored distance would have mis-selected exactly the animated parts.
> - **`pixel_budget` is not the dial it looks like.** `part_flatten.cpp` bakes it into the
>   threshold while `cull.comp` multiplies the projected size by it again, so for parts
>   flattened through that path the factor CANCELS and the dial does nothing, while it works
>   normally for parts on `lod_bake.h`'s hardcoded ladder. M1 preserves this rather than
>   fixing it; unifying it is a deliberate, separately-verified change (it moves pixels).
>
> Verified: kernel + editor build clean; `run-lod-distance`, `run-comp`, `run-partstore`,
> `run-world-definition`, `run-script`, `run-evalworld` pass; Vulkan smoke ALL PASS with 0
> validation errors; both replay gates on their noise floors (PomProofBrick 0.065 % against
> 0.069 %, ChartVtProof 0.352 % against 0.353 %). Jack accepted M0 and the Meadow world —
> the one live path the replay gates do not cover, since Meadow is the sole user of the
> SectorLod resolver and of a non-zero `min_projected_size` floor — interactively.

Scope (adjusted for the main base — there is no impostor system to convert, so rep N is
built once, correctly, rather than repaired):
1. At stage time, convert each part's existing threshold table into a **distance table**
   (parity mapping: `d_k = radius × scale × pixel_budget / threshold_k`, so visuals are
   unchanged by construction where thresholds were sane).
2. One selection function (`desired_rep`) in a single header, evaluated CPU-side (planning)
   and GPU-side (cull); delete the other call paths as consumers switch.
3. ~~**Introduce the impostor as rep N of that same table.**~~ **MOVED to M2.5 (2026-08-04).**
   On the branch this was a *conversion* of an existing impostor system, which is why the
   plan filed it under M1. On a main base there is no impostor system at all, so it is a
   net-new rendering tier — an atlas bake, an artifact, a draw path, and a selection
   integration. That is a milestone, not a line item, and bundling it here would make M1
   both large and visually non-inert, destroying the one property that makes M1 safe.

   Moving it after M2 is also the better order on its own merits: the redesign wants the
   impostor to be "rep N of the same table" (§5.3), and M2 builds the atomic
   residency-gated commit. Landing impostors *after* that means the new tier gets
   whole-or-nothing switching from its first frame, rather than having it retrofitted —
   which is precisely how the branch ended up with a rendering tier that never drew and
   reported nothing when it failed.
4. Residency clamp `[min_resident, max_resident]` per cluster (groundwork for M2's commits).
5. Salvage triage from `codex/far-field-impostors`: cherry-pick the LOD-tint and wireframe
   debug views and the editor prefab-view fixes — diagnostics this milestone actively needs.
   Everything else on that branch stays unmerged.

Acceptance:
- Shot-replay diff vs M0 baseline within stated tolerance on Demo + PomProofBrick at rep-0
  distances; document and review any far-field diffs (threshold-crowding cases *should*
  change — that is the point).
- **New: fly-through determinism test** — design settled 2026-08-04, see below.
- ~~Frame capture shows the ~14 MB/frame instance copy+upload is gone~~ / ~~a far tree costs
  one impostor draw~~ — both describe branch-only machinery that does not exist on a main
  base. Moved to M2.5 with the impostor tier.

Tests rewritten here: the Vulkan smoke fixture that encoded a projected-size constant is
re-authored in distance units (done, `M1c`).

### The fly-through determinism test (M1d)

**Design: GPU readback, frame-indexed camera path, symbolic switch-event log, two warm runs
diffed on one machine.** Established by investigation 2026-08-04; the rejected alternatives
matter as much as the choice.

- **Not headless-CPU.** Cheapest to build and *unsound as the keystone*: it can only test a
  pure function of pose and static tables, so it is green by construction and stays green
  through exactly the M2/M7/M9 regressions it exists to catch — residency gating, hysteresis,
  visibility priority, budget throttling all live in state a headless harness does not have.
- **Not replay-based.** `MATTER_REPLAY` is a single pose, and its output is pixels with a
  ~0.07 % noise floor — far too blunt to see one instance switch a rung one frame early.
- **GPU readback is sound and mostly built.** `cull.comp` already computes
  `bucket = cluster_index * MAX_LOD + lod` and writes `draw_transforms[...].instance_token`,
  so pairing an occupied transform slot back to its bucket recovers
  `(instance_token, cluster_index, selected_lod)` — the *draw authority's own answer*, not a
  mirror's prediction. `readback_commands` / `readback_draw_transforms` already exist; they
  are behind `MATTER_VK_TEST_FAULT_INJECTION` and the transform readback currently discards
  `instance_token`. Both are small changes.

Pieces to build: a `MATTER_CAM_PATH` camera path consumed **one pose per rendered frame**
(frame-indexed, never wall-clock — the existing `MATTER_CMD_FIFO` drains every buffered line
in one frame and `tools/viewer_shots.sh` paces it with `sleep`, which a determinism test must
never depend on); a `MATTER_LOD_TRACE` emitter shaped exactly like the existing
`MATTER_SKIN_PROBE` (env latch, diff against last frame, print only on change — it is already
a switch-event log); and a comparator.

Constraints the code forces, each of which would otherwise produce a permanently red or
permanently green test:

- **Canonicalize order before diffing.** Transform slots come from an `atomicAdd`, so slot
  assignment races even when the selection is identical. Sort by
  `(instance_token, cluster_index)`.
- **Record "not drawn" as its own symbol.** A frustum- or distance-culled cluster emits no
  bucket write, so absence must be distinguishable from a rung change — otherwise a culling
  change reads as a storm of LOD switches.
- **Assert switch ORDER, not frame index.** Streaming and publish timing differ run to run
  even warm; M2's degradation gate makes the same demand.
- **Assert `instance_token != 0`.** The token falls back to `source_index + 1`, an
  order-dependent key that would look perfectly stable while meaning nothing.
- **Emit a per-frame census** (instance count, drawn-cluster count) beside the events.
  Without it the test is satisfiable by rendering *nothing* — two runs that both draw an
  empty world diff identically.
- **Do not commit a golden trace**, for the same reason `docs/baselines/` does not commit
  golden PNGs: float ties at switch boundaries can differ across GPU and compiler. Compare
  two runs on one machine.

What it catches: any change to the rule, tables, `reach`, or budget plumbing that moves an
instance's rung at a given pose; boundary flicker (A→B→A appears as extra events — arguably
the actual R4 complaint, and nothing else in the repo can see it); order changes injected by
M2 hysteresis, M7 visibility priority, or M9 throttling, which is precisely the
"budgets shape *when*, never *what*" contract; and CPU-mirror-vs-GPU divergence, because the
trace is readback rather than mirror.

What it does not catch, and must not be trusted for: visual correctness of a rung (that stays
the replay gate), switch *atomicity* across mesh/VT/BLAS (that is M2's pop-coherence test),
bake nondeterminism (warm runs exclude it by design; that is M3/M4's golden-bake gates), and
cross-machine equivalence.

An optional cheap pre-filter — a single-TU `run-lod-flythrough` driving `SectorLodResolver`
over the same pose list twice — catches resolver-layer regressions in seconds with no GPU.
Useful as a smoke test; never as the gate, for the headless reason above.

#### Delivered (M1d), and how to run it

```
cd MatterEditor
TMP=... TEMP=... MATTER_WORLD=RockGallery \
  MATTER_CAM_PATH=path.txt MATTER_LOD_TRACE=run_a.trace MATTER_CAM_PATH_EXIT=1 \
  ./build/windows/editor.exe          # repeat into run_b.trace
python ../MatterEngine3/tools/lod_trace_diff.py run_a.trace run_b.trace
```

`path.txt` is one pose per line, `eye_x eye_y eye_z target_x target_y target_z`;
`#` comments and blank lines are ignored. `MATTER_CAM_PATH_WARMUP=n` (default 30) holds
the first pose for n frames after the world becomes drawable, then the path advances one
pose per RENDERED frame. The trace is `MatterEngine3/src/render/lod_trace.{h,cpp}`; the
readback that feeds it is `VkSceneRenderer::capture_lod_trace`, at the top of
`prepare_frame`, which is the one point where the slot's fence has been waited and
`upload_scene_buffers` has not yet restored `command_template_` over the GPU's counts.

Three findings the design did not anticipate:

1. **The frame serial is not a usable clock, but the pose index is.** Two warm runs put
   the same first switch on serial 37 and 36 — how many frames a world takes to become
   drawable varies. Stamping with the `MATTER_CAM_PATH` pose index instead makes two warm
   runs BYTE-identical, which buys back the case order-only comparison cannot see: a
   uniform rescale of `reach` or the budget moves every switch without reordering any of
   them. `--ignore-frames` restores the design's order-only comparison.
2. **The capture gate must be evaluated when a frame RECORDS, not when it is read back.**
   Gating at capture let the two or three frames still in flight when the path opened the
   gate into the stream — default-pose frames whose count depends on slot rotation.
3. **`instance_token != 0` is not a strong enough identity assertion.** On PomProofBrick
   the tokens were all nonzero and still differed between runs: only 5 of ~132
   (token, cluster) pairs survived. `instance_token` derives from `WorldSession`'s
   `instance_id`, which for streamed terrain sectors was allocation-ordered rather than
   content-derived, so two runs whose sectors published in a different order (worker-pool
   completion order, which streaming does not fix) gave the same geometry different
   identities. This was a **BLOCKER for M2, M7 and M9**, all three of which are about
   streaming and all three of whose acceptance leans on this gate to show that "budgets
   shape *when*, never *what*".

   **RESOLVED 2026-08-04.** Both halves of the trace key were allocation-ordered, and both
   are now content-derived:

   * `matter_engine.cpp`'s `sector_instance_id()` replaces `sector_next_id++` with a hash
     of (tile coord, variant rung, part hash), mixed exactly the way
     `resolvers.cpp::child_stable_id` mixes (parent, part hash, ordinal). It is folded to
     31 bits under a set high bit, which keeps streamed ids disjoint from the authored
     counters and makes zero unrepresentable — a zero `stable_id` falls back to
     `source_index + 1` in `vk_scene_renderer.cpp`, which would have reintroduced the bug
     silently.
   * The trace's `cluster_index` was the renderer's GLOBAL cluster slot, i.e.
     `PartRecord::cluster_start` (handed out in part-registration order) plus a local
     offset. `capture_lod_trace` now reports the cluster's index within its own part,
     through a `FrameResources::lod_trace_local_cluster` snapshot taken at record time.

   Measured on PomProofBrick over `tools/lod_flythrough_pomproofbrick.path`: key overlap
   between two warm runs went from **9 of 48** to **48 of 48**, and 58 of 58 across four
   runs of the descending path. RockGallery is unaffected and still MATCHes.

   **Still open, and now visible because the keys hold still:** exactly one streamed sector
   per run is drawn at rung 2 while the other ~57 sit at rung 0, and *which* sector it is
   varies run to run. Present in traces taken before the identity fix as well, so it is a
   separate defect — a sector's baked LOD ladder, not its identity. It is the last thing
   between this gate and a green MATCH on a streamed world.

---

## M1.5 — The ladder benefit rule *(pulled forward from M3, 2026-08-04)*

**Why out of order.** This is the defect Jack observed on the build and diagnosed himself,
it is self-contained, and it produces the number M2.5 needs. Waiting for all of M3 would
leave the engine baking rungs that provably change nothing for two more milestones.

Scope — bake-side only, one decision changed:

1. Replace `part_flatten.cpp`'s level-admission test. Today a rung is kept when it has merely
   fewer triangles than the previous one (a single triangle qualifies), which is why the
   coarse tail runs 26 -> 24 -> 22 across three rungs and one silhouette. Require a **benefit
   floor** instead: a substantial reduction, not a non-zero one.
2. **Record where the ladder bottomed out** in the artifact. That index is the answer M2.5's
   `LOD.impostor` terminal otherwise has to guess at (design §3.3).
3. Leave the `radius_divisor` sequence itself alone for now — the admission rule is the
   defect; re-deriving the epsilon ladder is M3's measured-error work.

Acceptance:
- **Every surviving rung visibly changes the wireframe.** This is the acceptance, and the
  debug views landed in `6b156699`/`e24de58c` are how it is checked: step a part down its
  rungs with `lod_bias` + `pixel_budget` and confirm each step moves triangle density. A rung
  that looks identical to its neighbour is a rung that should not exist.
- Bake time and cache size measurably down. Record both — the wiped-cache rebake of
  2026-08-04 gives a clean before (world_demo `.cache` was 730 MB: StreamMountain 386.3,
  ChartVtProof 159.2, StreamMeadow 67.7, LightingGarden 57.5, PomProofBrick 48.8,
  RockGallery 5.4, AnimatedRigGallery 3.9).
- **A deliberate re-baseline, reviewed before it lands.** Removing rungs changes the
  distance→rung mapping, so at some distances a coarser mesh now draws. Expect a real diff,
  review it, and do not re-baseline silently. Do NOT assume it is visually neutral just
  because the removed rungs were individually invisible — that reasoning covers the removed
  rungs, not the remapping.
- Fly-through determinism and the smoke suite green afterwards.

Not in scope: close-range fidelity. Rep 0 is the raw undecimated mesh, so this milestone
cannot add near detail — that is bounded by the part's build resolution and belongs to M3's
recipe work (design §3.3, closing note).

> **A downstream consumer this rule silently invalidated, found 2026-08-05.** The benefit
> floor does not only delete rungs — it moves where a ladder's FIRST switch happens, and
> anything that picks a rung by pixel size feels that. `test_flatten_segmented` had been red
> on this branch (cold cache and warm, with and without `MATTER_IMPOSTOR`) on
> `coarse L0 < trunk + 2 * child full-res`, failing by exactly zero: 722 vs 722.
>
> The seg child's ladder now reads
> `512:360- 256:360- 128:332- 64:312- 32:280- 16:156+ 8:90+ 4:54+ 2:28+` — every rung down to
> divisor 32 rejected, because 360 → 332 → 312 → 280 never buys 30%. Those near-duplicate
> rungs used to exist, and the fixture's 64 px hint landed on one. With them gone the child's
> first real switch is a 16 px-equivalent, so at 64 px `select_level_local` correctly returns
> level 0, `src = min(C,E)` is 0, and the coarse segment inlines full-res.
>
> **The mechanism was never broken; the fixture stopped exercising it.** The hint moved to
> 8 px, which lands inside the admitted ladder (the 156-tri rung) — the thing the test exists
> to prove gets sourced. The ladder listing and the reasoning are in the test so the next
> person to move a benefit floor sees what it costs downstream.
>
> **Not a production regression:** no schema in the repo passes `inlineBelowPx` (checked
> across `projects/` and `MatterEditor/`), so the segmented coarse path has no live caller —
> this test is its only consumer today. Worth remembering when one appears: a hint px chosen
> against a pre-M1.5 ladder can now select full-res.

---

## M2 — Atomic switches

> **Prerequisite (2026-08-04): make streamed-sector `stable_id` content-derived** —
> **DONE.** Both allocation-ordered halves of M1d's trace key (the sector instance id and
> the global cluster slot) are now content-derived, and key overlap between two warm runs
> of PomProofBrick went from 9/48 to 48/48. See the note at the end of M1d.
>
> **The gate then found M2's first bug, before M2 started.** Reproduced independently on
> PomProofBrick over two warm runs of one camera path, with `zero_tokens=0` and
> `duplicate_keys=0` confirming it is a real divergence rather than an identity artefact.
> Two distinct causes:
>
> 1. ~~**A freshly published sector's rung is nondeterministic.**~~ **FIXED (`95faead2`), and
>    the suspected cause recorded here was WRONG.** The symptom was real — the same sector
>    first appearing at rung 0 in one run and rung 2 in the other, a different sector each
>    time — but it had nothing to do with the publish path, `commit_staged`, or
>    `get_or_load`. **It was the draw-override SSBO.** `part_draw_overrides.length()` in
>    `cull.comp` returns the buffer's CAPACITY, not the number of entries the CPU wrote, and
>    `ensure_buffer` rounds allocations up from 16 bytes — so a one-entry neutral table left
>    readable garbage past its payload, and a cluster whose `part_slot` landed in that garbage
>    took `lod_bias = 0`. That makes `reach` zero, nothing clears its switch distance, and the
>    cluster clamps to the coarsest rung. Which cluster it hit depended on slot assignment,
>    hence one sector per run, varying.
>
>    Two lessons worth more than the fix. **An SSBO's `.length()` is a capacity, not a count**
>    — every shader reading a variable-length table must be given a real count or a fully
>    initialised buffer. And this is a reminder that a *suspected* cause written into a plan
>    reads like a finding to whoever picks it up next; the publish path was never implicated
>    by evidence, only by plausibility.
> 2. **Arrival timing varies** — sectors publish on different frames between runs (events at
>    `@f1`/`@f2`/`@f3` differ), worker-pool completion order reaching the frame stream. That
>    is timing rather than selection, and is what "budgets shape *when*, never *what*"
>    governs; for streamed worlds the gate must therefore compare the ordered switch sequence
>    per instance rather than raw frame stamps.
>
> Encouraging: the steady state converges exactly — final census lines are identical across
> runs (`C 8 8 1`, `C 12 12 1`, `C 22 22 1`). It is the transient that differs, not where the
> world settles.

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

## M2.5 — Object impostors as rep N *(moved out of M1; see the note there)*

> **Where the impostor goes is now answered, not guessed (2026-08-04).** Jack observed on the
> shipping build, with the LOD-tint and wireframe views on, that a rock keeps changing rung
> while its silhouette stops changing. Cause: `part_flatten.h:27` gives every part the same
> nine-rung geometric error ladder and admits a rung on `geo.size() < prev_count` — ONE fewer
> triangle qualifies — so the coarse tail runs 26 -> 24 -> 22 triangles, three rungs and one
> silhouette. Nothing ever asks whether a rung bought anything.
>
> The rule (design §3.3): keep adding mesh reps while decimation still buys a REAL reduction;
> the moment it does not, stop, and **that index is where the impostor goes**. Self-terminating
> per part, so a 4,258-triangle boulder and a 58-triangle pebble get different depths
> automatically. The ladder change is M3's; this milestone consumes its answer.

Scope: one impostor producer baking a view atlas per eligible part; the atlas stored as the
LAST rep of the part's existing distance table; selection by the same `lod::select_rep` walk
with no parallel comparison; rep-indexed subtree replacement (design §5.3) rather than the
order-dependent instance-record encoding; commits ride M2's atomic gate.

Acceptance:
- A part's impostor is visibly selected at its authored distance and nowhere else, shown by
  the LOD debug view.
- **Every load failure is logged, once, naming the part and the reason** — the single
  cheapest lesson from the branch, where an entire tier was silently absent for a full
  generation of artifacts and produced zero diagnostics. Proven failable by corrupting one
  atlas.
- Resident impostor count is on the editor stats overlay.
- Fly-through determinism and pop-coherence tests still green with the new tier present.

---

## M3 — DSL recipes and lazy staged baking

> **STATUS 2026-08-05: items 1–2 landed (`7ce0d045`, `1a9b4606`); items 3–4 NOT started.**
> Deliberately, on the brief's own instruction to land a coherent increment rather than
> half-wire the rest.
>
> **Done.** Authored switch distances in metres (`at`) and named generators (`gen`), added to
> the existing `static lods` surface rather than replacing it. `LOD.decimate({error|divisor})`
> returns a plain **data descriptor, never a closure** — which is the whole point, since a
> closure cannot be read without evaluating the class. Fail-closed parsing extended to
> whole-table rules (rep 0 takes no generator; distances must strictly increase; `params` and
> `gen` together is a conflict; a function in `gen` is rejected). Metres convert at the one
> place the cluster radius exists, with `pixel_budget` folded in so it cancels against
> `cull.comp` exactly as the default ladder's does — leaving `at` as honest metres.
>
> Verified: RockGallery cold-rebaked to **23 md5-identical artifacts** (existing worlds are
> untouched, byte for byte), a second cold bake reproduced them, an authored 0/18/45 m ladder
> was checked against `lod::select_rep` itself (17.9→rep 0, 18.1→rep 1, 44.9→rep 1,
> 45.1→rep 2), 14 fail-closed parse cases, and every suite at its known baseline.
>
> **Not started — carried as M3b:** stages (`ctx.stage()`, content-addressed memo, dependency
> traces, cycle detection), lazy per-rep baking (the biggest win — it removes the eager
> full-ladder bake), and custom script builders naming a method. The authored path is also
> single-cluster, gets no terminal impostor, and ignores `exclude`.
>
> **A near-miss worth keeping:** the Part Workbench's "Save lods to source" would have
> silently *deleted* an authored ladder, and its own parse-verify would have approved the
> result — because that check compares level COUNT, not content. Fixed in `1a9b4606`.


> **M3 is NOT greenfield — there is already a `static lods` authoring surface, and its
> shape is better than the design doc's (2026-08-05).** `ScriptHost::eval_lods`
> (`script_host.cpp:882`, spec at `script_host.h:226`) reads a part's `static lods` array
> **without building the part**, fail-closed on any shape violation. Each entry can already
> declare `params` (opting that level into a fresh `build()` with overridden params rather
> than pure decimation from LOD 0) and `exclude` (child modules dropped at that level).
> `static lodBudgets` is a sibling; `Grass.js` and `AnimatedRigGallery.js` use it today.
>
> **The no-build read is a feature, not an accident, and the redesign doc under-values it.**
> §3.1 sketches `lods(p)` returning entries with `gen:` closures — but a closure cannot be
> read without evaluating the class, and **lazy per-rep baking (§3.4) requires knowing what
> reps exist before baking any of them**. A method returning closures forces an evaluation
> just to answer "what reps does this part have?".
>
> So M3 should adopt a **hybrid** rather than replacing the static surface: keep `static lods`
> as the declarative shape — how many reps, their switch distances, which generator and with
> what parameters — readable without a build; and let a generator *name* a method on the class
> whose body runs only when that rep is actually baked. That keeps no-build introspection,
> keeps fail-closed parsing, and still gets custom per-rep builders and stages. Extending this
> surface is also far less disruptive to existing worlds than replacing it.

> **Two ladder defects to fix here, both observed on the build rather than inferred:**
> (a) rungs are admitted on any reduction at all, producing visually identical coarse rungs —
> replace with a benefit floor whose exhaustion terminates the ladder (design §3.3, feeds
> M2.5); (b) **close-range fidelity is not a LOD problem at all** — rep 0 is the raw
> undecimated mesh, so "more detail up close" is bounded by the part's own build resolution,
> which is fixed engine-wide today. A `lods()`/build recipe must be able to declare it.

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

## M3.5 — The authored impostor terminal *(the M3/M2.5 seam)*

> **STATUS 2026-08-05: DONE.** M3 landed authored distances and M2.5 landed the impostor,
> but on separate paths: the default (divisor-schedule) ladder grew a terminal billboard
> automatically, and the authored ladder could not have one at all. That gap is why "trees
> render geometry way out past where I'd want impostors" had no answer short of dialling the
> global pixel budget down and dragging everything else coarse with it.

Scope: `LOD.impostor({ at })` as design §3.4's explicit terminal on the `static lods`
surface, desugaring to `{ impostor: true, at }` so it stays readable without evaluating the
class — the property lazy per-rep baking (M3b) and the M6.5 shadow hand-off both depend on.

**Four fail-closed rules**, enforced in `eval_lods` and re-checked in `part_flatten` because
the plan is a file an older binary or a hand edit can write:
1. At most one impostor, only in last position — a mesh rep after the billboard is an error,
   since the billboard is where the ladder *ends*.
2. Never rep 0: the impostor is a picture of the coarsest mesh rung, so one must exist.
3. No `gen`, no `params`, no `exclude` — it has no geometry recipe of its own.
4. `views` is **rejected**, not accepted-and-ignored. `impostor::kViews` is a format constant
   the atlas layout, the vertex stage and the format version are built around.

**Two things that would have been silent had they been got wrong**, both now asserted:

- **The billboard must not enter the cluster AABB.** `build_quad` squares the card off at
  1.10× the bounding-sphere radius, so a 0.4 m-wide, 6 m-tall trunk would gain a 6.6 m
  horizontal extent. `cluster_radius` is what every authored `at` is normalized against, so
  the ladder's authored metres would all quietly move. The test bakes the same ladder with
  and without the terminal on a deliberately tall, thin fixture and requires the radius to
  match bit for bit; deleting the guard fails it by ~55%.
- **The Part Workbench's "Save lods to source" would have deleted the terminal.** Its
  parse-verify compared level COUNT, and dropping `impostor` leaves the count unchanged —
  the same near-miss shape as `1a9b4606`, one field later. The verify now compares CONTENT
  (`at`, `gen`, `params`, `exclude`, `impostor`) on both the pre-write and post-write passes,
  the renderer re-emits the terminal, and the panel offers it only where the parser would
  accept it. A missing `, ` before `params:` in the same renderer was fixed alongside.

`MATTER_IMPOSTOR=0` drops a declared terminal rather than failing the bake: the ladder ends
at the last mesh rung, exactly as if none had been declared. Same switch, same meaning, on
both ladders — the env-var lambda that was private to the default path is now one function
serving both.

Acceptance (all green): `run-flatten`'s `test_authored_ladder_impostor_terminal` —
rung N is the billboard by `is_billboard_rung`, the atlas loads against the depicts-hash
PartStore recomputes, `select_rep` flips at 139/141 m for an authored 140, the AABB is
untouched, `MATTER_IMPOSTOR=0` degrades to mesh, and two cold bakes are byte-identical in
both the ladder and the atlas. `run-script`'s parse cases cover all four rules plus
`impostor: false` reading as absent.

Still open: `LOD.vanish`, and `replaces: 'subtree'` for assemblies — the authored path is
single-cluster, so a subtree impostor has nowhere to attach yet.

---

## M4 — Artifact consolidation and the version vector

> **STATUS 2026-08-05: DONE.** Landed as `e9d16341` (the version vector), `976ba3af` (the
> resolve census + a resolve cache that could never be updated), `bd1fd2bf` (PartBundle and
> the LMSK removal) and `152cd596` (peek's question, and the suites following the sections).
>
> **The vector.** `MatterEngine3/src/version_vector.h` holds every component
> (`kEngineBake`, `kBox3d`, `kRepresentation`, `kPartFormat`, `kFlatFormat`,
> `kImpostorFormat`, `kBundleFormat`, and `kStageFormat` reserved for M3b), and
> `matter_version::fold` is the only way one enters a key. Four key sites call it:
> `compute_resolved_hash` (which folded NO version before — the hole), `resolve_cache::
> compute_key`, `tileset::settle_cache_key`, and `gtex_content_hash`, which used to TAKE
> the versions as parameters. That signature change is the shape that mattered: a version
> passed as an argument is one a caller can get wrong, and a component added later would
> never have reached it. `kFormatVersionV2`/`Flat`, `impostor_bake::kFormatVersion` and
> tileset's `kEngineBakeVersion`/`kBox3dVersion` are now aliases of components; the only
> two remaining uses of a component name outside the header write `GTexHeader` provenance
> fields, which `gtex_cache_hit` does not compare.
>
> **The bundle.** `parts/<hash>.bundle` — REP0, FLAT, IMPO, PLAN, HINT, VARS, and a
> reserved STGE. A section's payload is byte-for-byte what its file held, so every body
> serializer and trailer grammar is untouched and a section still validates its own
> header. The directory is sorted by tag, making the bytes a pure function of the section
> SET rather than of the order the bake wrote them — which is what makes the double-bake
> gate stable when flatten and impostor land in different orders.
>
> **Three defects the milestone's own acceptance surfaced**, none of them predicted:
>
> 1. **`resolve_cache::save` published with `std::rename`, which on Windows fails when the
>    target exists** — and the resolve cache lives at a FIXED path. It could be written
>    exactly once, ever; every later save logged "save failed (non-fatal)" and the world
>    re-resolved from scratch forever. Invisible because the first write of a fresh cache
>    always succeeds and a warm hit never saves: the only routine way to rewrite an entry
>    is to invalidate it, which is what M4 does.
> 2. **The animation bundle's `part_body_checksum` was "the file past its 40-byte header"**,
>    which was the part body only while a part owned a file. In a bundle it would also
>    cover the flat and the impostor, so flattening a linked part would break an animation
>    commit it has nothing to do with. It checksums the REP0 section now.
> 3. **`peek_format_version` answered the wrong question.** "The version of the file here"
>    and "is there a flattened artifact here" were the same answer only while `.flat.part`
>    existed exactly when the answer was yes. Reporting v2 for a bundle sent PartStore's
>    flat-preferred load down its legacy-v2-flat branch, loading the compositional body AS
>    a flat and dropping the child table.
>
> Also absorbed from M0: **`.static_lods` survives as the PLAN section** (its cache-probe
> function is intact — a probe is a reader), and **LMSK is deleted**, confirmed write-only:
> its one reader was a `load_v2` overload with no production caller, and the plan already
> carries the same masks.

Scope: the **PartBundle** (manifest + per-rep blobs + parameterisation + impostor atlas +
stage outputs) replaces `.part`/`.lods`/`.fimp`/`.impostor`/`.hints`; one **version vector**
defined in one place and folded into every cache key (closes the part-resolved-hash hole
permanently). One-time full rebake.

Acceptance — measured on RockGallery, 13 parts:
- **Double-bake determinism:** two cold bakes produce 14/14 byte-identical artifacts;
  a warm run touches no file, mtimes included.
- **Failability proof of the version vector:** `kRepresentation` 1 → 2 moves the digest
  `2fb0b46c73e560e9` → `25c4f06e96d49409`, and the census goes from `0 baked, 12 hits` to
  `13 baked, 0 hits`. Every part filename is NEW (36 of 36 pre-bundle part artifacts, 0
  shared with the pre-bump set) and every one was written in that run — re-BAKED, not
  merely re-resolved. Reverting restores the digest and gives `0 baked, 13 hits` with no
  part artifact touched.
- **Cache inventory:** 37 artifacts become 14 — 13 `.bundle` + 1 `.resolve`. Sections
  REP0 13 / FLAT 12 / IMPO 11, exactly the old `.part`/`.flat.part`/`.fimp` counts. No
  `.part`, `.flat.part`, `.fimp`, `.static_lods`, `.hints` or `.lods` file is written
  anywhere. `.resolve` remains: it is a WORLD-level artifact, not a part's, so it has no
  bundle to live in — it moves into the store at M5.

---

## M5 — MatterStore

> **STATUS 2026-08-05: the standalone library is DONE; cache adoption is NOT started.**
> Split deliberately — the library's first appearance and a change to how the engine reads
> every artifact should not land together, least of all unattended.
>
> **THE BENCHMARK MISSED THE PLAN'S ≥5× CRITERION, AND ONE CASE IS A REGRESSION.**
> Measured on this machine (`docs/asset-store-benchmark-2026-08-05.md`):
>
> | case | small files | packs | ratio |
> |---|---|---|---|
> | cold, whole corpus (400 blobs, 98.5 MiB) | 190–200 ms | 42–45 ms | 4.3–4.7× |
> | **cold, one sector revisit (40 contiguous)** | 9.4–10.5 ms | 3.15 ms | **3.0–3.3×** |
> | cold, scattered (40 spread wide) | 9.7–10.5 ms | 6.4–6.5 ms | 1.5–1.6× |
> | **warm (page cache hot)** | 28.9–29.5 ms | 52.0–53.5 ms | **0.54× — the store LOSES** |
>
> The case the criterion actually names — sector revisit — came in at 3.0–3.3×, not ≥5×. The
> ratio falls as the request shrinks and as locality worsens; the scattered floor of 1.5× is
> `open`/`close` cost rather than locality. **And warm reads are roughly twice as slow through
> the store.**
>
> **This is a live risk to the second half, and to R7 ("returning to an area already baked is
> near-instant").** The store's win is on cold reads; a revisit whose pages are still hot is
> exactly the case it loses. Before adopting it as the cache, establish which of the two a
> real revisit actually is — if warm-hot is common, adoption needs a path that does not pay
> the store's per-read cost on cached data, or R7 gets worse rather than better.
>
> Two real defects the measurement forced out (both fixed): CRC32 was a byte-at-a-time loop
> at ~575 MB/s consuming **82 % of warm read time**, and `ReadBatch` staged then copied every
> payload. The remaining warm gap is what is left after both.
>
> Caveat on method: "cold" is an unbuffered proxy — the Windows page cache cannot be dropped
> without admin — applied identically to both paths and labelled as such.


> **STATUS 2026-08-05: FIRST HALF DONE, SECOND HALF NOT STARTED.** The standalone
> library landed as `23cb5fbb` (BlobStore, RefTable, ReadBatch, the suite) and
> `a5c91892` (the benchmark and two performance fixes it forced). `libs/AssetStoreLib`
> depends on MemoryLib and nothing else; no engine code references it.
>
> **Acceptance, measured.** All five lib tests pass — crash-mid-write recovery,
> corruption-is-a-miss, LRU eviction to a disk budget, a concurrent reader soak, and
> determinism — plus batching, arena landing, read-only enforcement and a CRC32
> correctness suite. 244 checks, 0 failures. The crash test spawns a real child that
> `_exit(3)`s mid-payload and asserts the pack physically grew past its committed
> extent *before* asserting that reopening erases it, so the tear it recovers from is
> a real one.
>
> **The benchmark missed its target, and that is the finding.** The plan asked for
> ≥ 5×. Measured (docs/asset-store-benchmark-2026-08-05.md): whole-corpus cold load
> **4.3–4.7×**, the sector-revisit case the criterion actually names **3.0–3.3×**,
> scattered access **1.5–1.6×**, and with the OS page cache hot the store **loses at
> 0.54×**. The ratio falls as the request shrinks and as locality worsens, and it
> inverts when everything is cached.
>
> Two fixes came out of measuring: CRC32 was a byte-at-a-time loop at ~575 MB/s and
> was 82% of the store's warm read time (now slice-by-eight, ~2.8 GB/s, same
> polynomial and same bytes on disk), and ReadBatch was staging each chunk before
> copying payloads into the arena (now reads straight into the arena).
>
> **What this means for the second half.** The throughput case is real but smaller
> than assumed, and the warm regression is a genuine adoption risk: if the engine's
> revisit path is mostly page-cache-hot, adoption could make it slower until the
> checksum gets cheaper (hardware CRC32C would fix it, and changing the polynomial is
> cheap *now* while nothing has adopted the format). The instant-revisit acceptance
> below is the measurement that settles it and should be read against that table.
> The stronger arguments for adoption are the ones the benchmark does not measure:
> crash safety by construction, per-blob checksums, budget eviction, and 600× fewer
> file operations on a machine that — unlike this one — has an antivirus filter, a
> network share or a spinning disc in the path.

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

> **SURVEYED 2026-08-05, and the scope is SMALLER than this milestone assumed.** The plan
> says "chart rep 0, **reproject** other reps at bake", which reads as a closest-point UV
> transfer — the expensive, seam-fragile kind. It is not needed, because **the
> parameterisation is already analytic**.
>
> `build_chart_rung` (`lod_bake.cpp:120`) segments charts by normal cone, gives each chart a
> planar basis from its area-weighted average normal, and emits a `ChartEntry` carrying
> `origin`, `tangent`, `bitangent`, `rect_*` and `tpm`. `vt_chart_resolve.glsl:117` states
> the mapping outright:
>
>     texel = rect + gutter + (dot(p, T/B) - dot(origin, T/B)) * tpm
>
> A point's UV is therefore a pure function of its WORLD POSITION and its chart — not of the
> mesh it belongs to. So a coarser rung does not need reprojected UVs at all; it needs a
> **chart id per triangle**, and the UV falls out of the same formula. A decimated vertex
> sits slightly off rep 0's surface and lands on a slightly different texel, which is the
> behaviour we want: the texture stays glued to the surface while the geometry moves under it.
>
> **`reproject_triex` is NOT the tool here, contrary to first appearances.** Its doc comment
> says it carries "materialId/tint/uv/AO", but the implementation does
> `TriEx ex = source.triex[match]` — it copies the nearest source triangle's TriEx *wholesale*,
> including all three corner UVs verbatim. Correct for per-triangle constants, wrong for a
> per-corner UV: a large coarse triangle would inherit a small source triangle's UV range
> stretched across it. Do not reach for it.
>
> **Revised scope**, in dependency order:
> 1. Build the chart table ONCE per part from rep 0; store it per part, not per rung.
> 2. Assign every coarser rung's triangles a chart id (nearest rep-0 chart by face normal +
>    centroid). A coarse triangle spanning two charts takes one of them; its UVs then reach
>    outside that chart's packed rect, so the gutter/clamp policy is what bounds the error —
>    **measure it, it is the one real risk left.**
> 3. Key the VT variant on `part_hash` alone (`variant_key(variant_hash, rung)` at
>    `vt_residency.cpp:618` is the per-rung churn, and `register_variant`'s `rung` parameter
>    is the whole of it).
> 4. Composite pages from rep 0's mesh — one authority, so page texels stop depending on the
>    selected rung. This is what makes the horizon query rung-invariant and is the actual fix
>    for the dome patches.
>
> Item 2 is the only step with a genuine unknown. Items 1, 3 and 4 are re-plumbing.

> **PROGRESS 2026-08-05: steps 1 and 2 landed (`b1bd873d`, `a3001140`).**
> `lod_bake::apply_chart_rung` gives a coarser rung rep 0's table analytically, and
> `ChartBakeOptions::unify_parameterisation` applies that across a whole ladder on both
> `bake_lods` and `bake_terrain_lods`. Off by default. Measured on the way:
> - Adopted UVs agree with the builder's to **0.000046 texels**. They cannot be bit-identical:
>   the builder uses its local `minU`, while `ChartEntry` stores only `origin`, so this code
>   *and the GPU* recover `minU` as `dot(origin,T)` — exact in real arithmetic, `minU+O(eps)`
>   in float. Bit-identity was the wrong target; this path matches the shader at least as
>   closely as the builder's own vertex UVs do.
> - **The straddling-triangle risk measured ZERO** on cylinder-overhang at 40% decimation:
>   worst UV overshoot 0.00000, every adopted UV inside the atlas. One fixture, one ratio —
>   not yet a general result, but the gutter is not obviously the problem the survey feared.
> - The unification test was **vacuous on its first fixture** and the failability control
>   caught it: a 12-triangle cube barely decimates, so its rungs chart identically with or
>   without the flag and the positive assertion alone would have read as proof.
>
> **Step 3 is NOT the one-line key change it looks like.** `variant_key(hash, rung)` mixes the
> rung in, but `rung` is *also* a real index — `matter_engine.cpp:3546` uses it to select
> `lod_charts[rung]` and `lod_mesh_data[rung]`. So the key and the mesh selector have to be
> separated (a `param_id` alongside the rung, defaulting to it, ideally *derived from the
> chart table's content* so a unified ladder collapses to one key with no flag to desync).
>
> **Step 3a landed (`94fea81a`), and it corrected step 2's own write-up.** Step 2 claimed to
> cover the bake; it covered two of FIVE chart sites. `part_store.cpp` charts at three more
> that `ChartBakeOptions` never reaches (`:618` flat v3 legacy-view, `:694` flat v3 cluster,
> `:800` flat v2) — and the flat path is the one **authored props** take, so the parts this
> whole effort is about would have kept churning while streamed sectors stopped. The rule now
> lives once, in `lod_bake::chart_rung_unified`, and all five call it.
> `MATTER_VT_UNIFY=1` is the switch, default off, read per call (not cached in a static) so a
> test can A/B it inside one process — the same choice `impostors_enabled()` makes.
>
> **Step 3b — the runtime key — is lifetime-critical, and here is its exact shape.**
> `register_variant` already receives the atlas, so `param_id` can be derived from the chart
> table's CONTENT there and both callers (`vk_scene_renderer.cpp:4259`,
> `matter_engine.cpp:3546`) need no change: equal tables collapse to one key, unequal ones
> stay separate, with no flag to desync. The cost is that `slot_for(hash, rung)` and
> `release_variant(hash, rung)` do not have the atlas, so they need a `(hash,rung) → key`
> alias map.
>
> The dangerous part is release. `VkSceneRenderer::evict_vt_rung` (`:4192`) releases
> **per rung**, driven by `record.vt_slots[rung]`. Under one shared variant every rung's slot
> is the SAME slot, so evicting rung 3 would free a layer rungs 0–2 still point at — a
> dangling variant slot, i.e. GPU use-after-free, in the subsystem that already produced one
> DEVICE_LOST investigation. **A refcount of aliasing rungs is required, not optional**, and
> it has to interact correctly with the retirement graveyard (`kVtRetireHorizonFrames`) rather
> than beside it.
>
> **And that surfaces the real step-4 question.** With one variant shared across rungs, the
> variant holds ONE mesh — whichever rung registered first — and pages are composited from
> it. If a far sector registers at rung 3 first, page texels get baked from coarse geometry
> and a later close-up shows them. So a finer rung arriving for an existing variant has to
> UPGRADE the variant's mesh and re-composite. That upgrade path is the substance of "page
> pool becomes a per-part mip chain", and it lands in `VtResidency` — the subsystem with the
> subtlest invariants in this engine (tail gates, retirement graveyard, table generations).
> Budget for it accordingly; it is not re-plumbing.

> **STRUCTURE COMPLETE 2026-08-05, BEHIND `MATTER_VT_UNIFY=1` (default OFF).** All four
> steps landed: `b1bd873d` (adopt rep 0's table analytically), `a3001140` + `94fea81a` (one
> rule, all five chart sites), `6af8593a` (variant layer keyed by parameterisation, with the
> alias refcount), `be9dd1fb` (a finer rung rebuilds the layer it would otherwise inherit).
> Green: run-chart-atlas, run-vt-residency, run-partstore, run-flatten, run-demandbake,
> run-vk-scene-renderer 109/109.
>
> **Nothing here is live until the switch is set**, which is deliberate: this milestone is a
> visual re-baseline and the plan's own rule is not to re-baseline silently. The switch is
> read per call, so it can be A/B'd inside one process.
>
> **Two defects this work created and then closed, both invisible to a headless suite:**
> - `release_variant(hash)` walks rungs 0..31 releasing `variant_key(hash, rung)`. Under a
>   content-derived key those are no longer keys in `layer_of_` at all, so that loop would
>   have freed nothing and silently leaked every variant of every released part — no crash,
>   no log. It walks the alias table now.
> - Registration is demand-driven by default, so a sector first seen at distance registers a
>   COARSE rung. Without step 4 that coarse mesh would composite the shared pages
>   permanently — strictly worse than what M6 replaced, and visible only by flying in.
>
> **The Vulkan smoke suite is ALL PASS with the switch ON, 0 validation errors — and that
> proves less than it sounds like.** Diffing the two runs (switch off vs on) gives ZERO
> differing lines, which means the suite's fixtures never took the unified path: nothing in
> them has a multi-rung charted ladder. So it is a real NO-REGRESSION gate for the refcount
> and rebuild paths on actual GPU state (which is worth having — that is where a dangling
> variant slot would have shown up as a validation error or a hang) and it is NOT evidence
> that unification does anything. Do not cite it as such.
>
> **What is left is measurement, and it needs a world with multi-rung charted parts — i.e.
> the GPU and Jack's eyes**: the acceptance list below is unchanged and none of it has run.
> StreamMountain and the PomProofBrick dome repro are the two that matter.

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

## M6.5 — Distant shadows in the receiver's horizon map

An impostor cannot cast a correct shadow (design §6.5) — it is a camera-facing card whose
plane passes through the object's centre, so a traced ray starts inside the volume it
depicts. M2.5 therefore ships impostors as non-tracing, and a tree loses its shadow the
moment it impostors. This milestone gives the shadow back by baking it at the **receiving**
end: not into the pine's page (keyed per VARIANT — every pine would share one shadow) but
into the terrain **sector's** page, which has exactly one placement.

**Prerequisites, both hard:**
- **M6.** The horizon channel has already produced one visual defect in this engine — the
  dark dome patches were the baked horizon queried through a per-rung, mesh-dependent basis.
  Do not add data to that channel until M6's single parameterisation makes the query
  rung-invariant, or this builds on the bug.
- ~~**Authorable impostor distances.**~~ **MET 2026-08-05.** M3's authored `at` and M2.5's
  impostor now compose: `LOD.impostor({ at })` is the explicit terminal entry (design §3.4),
  and its `at` IS the impostor switch distance, readable off `static lods` without building
  anything — which is what this milestone needs, since the fold-in decision has to be made
  before the bake runs. See M3.5 below.

Scope:

1. Extend the sector horizon bake to treat impostored props as occluders, writing into the
   sector's own `CHAN_HORIZON_A`/`CHAN_HORIZON_B` — 8 azimuths of `sin(elevation)` per texel,
   already consumed by `gbuffer.frag` → `out_orm.w` → `rt_shadow.rgen`. **A horizon stores
   the occluder's elevation profile, not the lighting**, so the sun angle is applied at
   runtime and stays a live property. That is the whole reason to bake a horizon rather than
   a shadow.
2. Gate it on the prop's impostor switch distance: a prop is either casting RT rays or folded
   into the horizon, never both and never neither.
3. Include casters slightly **outside** the sector bounds, or shadows will not cross sector
   seams.
4. Fold the new dependencies into the version vector (`version_vector.h`; `fold()` is the
   only entry point) — horizon content now depends on prop placement and on the impostor
   distance, which it did not before.

Acceptance:
- **Measure the angular-resolution question BEFORE claiming shadows.** 45° azimuth bins at
  quarter resolution will render a canopy as soft darkening, not as trunk shadows. Capture a
  sector with and without the fold-in and report what is actually resolvable. If it reads as
  ambient darkening, say so — that may well be right at impostor distance, but it must be
  described honestly rather than sold as shadows.
- **No double-darkening at the handoff and no gap.** Fly the switch distance; sun visibility
  must be continuous across it.
- **The sun stays live.** Drag `sun_azimuth_deg` / `sun_elevation_deg` and show the baked
  occlusion responds correctly. This is the one property that must not regress — freezing it
  would defeat the entire design.
- **No seam** where a prop straddles a sector boundary.
- Double-bake determinism; the fly-through determinism gate green; replay gates re-baselined
  deliberately (this is a visual change by intent).
- **Measure the RT compute actually saved.** The claim is that far-field shadow rays go away;
  if the saving is small, that is a finding.

Not in scope: near-field shadows. A horizon map is a coarse directional approximation and
contact shadows are exactly what 45° bins cannot represent. Near stays RT — this is the far
half of a hybrid, and the switch is the impostor distance.

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
