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
