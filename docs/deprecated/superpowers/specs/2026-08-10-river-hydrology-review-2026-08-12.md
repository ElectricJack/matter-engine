# Adversarial Review: River Hydrology Design (revision 2026-08-12)

**Reviewed:** `2026-08-10-river-hydrology-design.md` at commit `145f8310` ("align river
hydrology with volumetric sectors", +543/-179).
**Code baseline:** the spec's declared target, `claude/volumetric-sectors-impl-fe4677`
@ `ed9f6ea2`. All code citations are against that tree. **Note:** the spec's own
worktree is based on `main@5d5b7bbe`, which does not contain the branch code the spec
describes; two of its three engine-doc references are dead links there.

**Review history:** the 2026-08-10 review (13 findings) was addressed by `7a1329d0`.
Today's `145f8310` additionally introduced, unreviewed: the dependency-baseline section,
the typed boundary-record/water-weld story, the **entire terrain-plan erosion pipeline**,
and the **dyadic-summary/ribbon far-LOD water extraction**. Those last two are where most
of what follows lives.

---

## 1. Verdict

Substantially honest and mostly implementable; the new dependency-baseline section is
verified accurate line by line. Not implementable as written in one place: the far-LOD
water extraction is internally inconsistent (three definitions of coarse water in
adjacent bullets) and its "minimum visual ribbon" structurally breaks the runtime
seam-weld contract it must simultaneously satisfy. Second, the update added a full
hydraulic-erosion pipeline whose determinism is asserted as a test rather than
constructed, and whose cost is the only major cost with no arithmetic. Third, shipping
cross-level water is gated on an open engine defect whose fix is "subject to proof" and
which no delivery track owns.

---

## 2. Blocking findings

### B1. Far-LOD water: three incompatible definitions, and the ribbon breaks the weld contract

**Claims** (§Water LOD and seam ownership, 984–996, 1000–1016; all new today):
(a) level L "resamples the same world-aligned field at `finalCellSize * 2^L`";
(b) coarse extraction "consumes the artifact's dyadic volume/head/velocity/connectivity
summaries"; (c) a sub-cell stream "may render at a minimum visual ribbon width... point
sampling may not erase it"; (d) "equal-level pairs need no weld because they use
identical world samples and ownership"; (e) cross-level pairs weld "through the same
bounded neighborhood... pattern as terrain."

**Why it fails.** (a) and (b) are different mechanisms — point/field resampling vs.
reconstruction from per-brick aggregates — left side by side because today's update added
(b) on top of a sentence surviving from `7a1329d0`. More seriously, (c) contradicts
(d)/(e). The weld fan requires both sides of a face to be iso-surfaces of one shared
dyadic lattice with per-cell corner signs: enumeration at fine resolution, coarse cells
via `floor_div2`, signs from the fine side's sparse `corner_signs`
(`seam_weld.h:28–52`, `seam_boundary.h:102–118`). A ribbon widened beyond physical fill
is *not* an iso-surface of the shared field — the coarse cell has no fill-sign change, so
it produces geometry the fan cannot attach to. And (d)'s exactness claim would require
ribbon synthesis to be a pure world-aligned function under `[1..n]` ownership, never
specified.

Conversely, without the ribbon, a stream narrower than a coarse cell is exactly the
`missing_coarse_pair` case terrain measured (0.75–0.88-voxel holes) and could only close
with the M0-WP7 overlap band (`seam_weld.h:54–105`, `terrain_mesher.h:82–92`). For
terrain that case is rare; for water — a 2 m stream crossing a 32 m cell — it is the
*defining* case. The spec never mentions a water overlap band or any substitute.

**Fix.** Pick one definition and design it: coarse water as surface nets over a summed-fill
field on the level-L lattice (satisfies a, d, e exactly), plus a *separate, weld-exempt*
ribbon/impostor driven by the connectivity masks, with an explicit statement of how ribbon
geometry terminates at cube faces (fade/clip inside the face band, as the displacement
section already does) rather than participating in welds. Specify the water overlap band,
or prove water does not need one.

### B2. Byte-identical double-bake is required, but the coordinator's schedule is not a function of solver state

**Claim.** "Two cold bakes must produce byte-identical artifact payloads and checkpoints"
including checkpoint-resume (§Determinism, 1198–1216), while the tributary-first
coordinator sleeps sections "after a checkpoint is available" (767–768), activates them
"when the wet frontier reaches a boundary," and lets tributaries "progress independently."

**Why it fails.** The determinism section fixes GPU-internal ordering (scans not atomics,
fixed reduction trees — all good) but says nothing about the coordinator. "After a
checkpoint is available" is an I/O-completion condition: run to run, checkpoint writes
finish at different wall times, so the batch at which a section sleeps — and how long its
frozen boundary feeds neighbours — differs, and the trajectory diverges. Independent
tributary progression has the same property unless batch interleaving is a deterministic
function of section state alone. Relatedly, sections advancing "independently" are at
different simulated times when they exchange ghost populations; that is only meaningful as
a steady-state relaxation, which the spec never states — while it simultaneously allows an
"accumulating" artifact for a continuously fed closed basin (800–804), which is a
*transient* and is incoherent under asynchronous section times.

**Fix.** Three sentences: (1) every activate/sleep/wake/expand decision is a deterministic
function of (batch index, solver state) only — checkpoint I/O may lag but never gates a
transition; (2) section batch interleaving follows a canonical order (lexicographic
section key per round); (3) the converged artifact is a steady state, and any
accumulating/preview artifact is explicitly outside the byte-identity gate — or runs
synchronous rounds.

### B3. Cross-level water is gated on an open engine defect no workstream owns

**Claim.** "Before cross-level water ships, the base streamer/engine must drive
`drawn_level_violations` to zero in settle-gated refine and merge soaks" (190–196,
1020–1022).

The spec's account of the defect is accurate — verified against `eea2ded1`: deferral ON
4/3 violations vs OFF 8/6, every survivor `offender-held=YES`, and the candidate fix
matches that commit's follow-up note. But: the fix is a hypothesis, explicitly "subject to
proof and measurement"; a canyon river *by construction* crosses every LOD shell (the
spec's own soak at 1404 demands it), so every water body of interest has cross-level
faces; and no track owns driving the count to zero — Track A is authoring, Track B the
solver, Track C builds against synthetic artifacts, and Phase 0 only "pins contracts." If
the candidate fix fails, visible acceptance is indefinitely blocked, and the spec rightly
forbids widening the welder.

**Fix.** Put the zero-violation item inside this project's Phase 0 with an owner, and state
the fallback: water welds plus a terrain-style overlap band tolerating a residual ≤N
transient violations, with the soak bound as the gate instead of absolute zero.

---

## 3. Important findings

**I1 — Erosion determinism is a wish expressed as a test.** 503–510 recomputes
"incrementally until affected chunk boundaries stabilize"; the test at 1325–1330 demands
identical digests regardless of visitation order. That is a fixed-point-uniqueness claim
about a nonlinear relaxation over a re-extracted surface graph — generally false, and
nothing in steps 1–6 constructs order-independence. The cache story also needs
incremental ≡ cold-bake, never stated. *Fix:* a canonical global schedule (lexicographic
chunk order per wave, fixed wave count over the dirty closure).

**I2 — Terrain-plan bake cost is unbudgeted and on the world-load critical path.** Every
hydrology-intersecting cube's bake key needs its chunk digests (573–579), so on a cold
start no river-corridor sector can bake until erosion completes. The LBM gets careful
arithmetic; erosion (32 iterations of flux routing + interface re-extraction over 4 m
bricks) gets none, and blocking-vs-async is never stated.

**I3 — Equal-level seam exactness across the two mesher paths is an unstated bitwise
requirement.** Non-intersecting cubes use `mesh_sector_tiled` unchanged; intersecting
neighbours use the composition-aware source (555–564). Equal-level pairs meet with no weld
purely because shared samples are bitwise identical (`terrain_mesher.h:66–72, 167–171`).
At a face between the two paths the composite evaluation must be bit-identical to the
plain base-field evaluation — "base + zero contribution" through smooth-min and a different
instruction sequence is not automatically the same float. Failure mode: hairlines tracing
provider AABB boundaries — this repo's exact history. *Fix:* short-circuit to the base
field's own evaluation outside every provider's support, and add the cross-path test.

**I4 — The velocity envelope converts most canyon hydraulics into authored transfers.**
With `maxResolvedVelocity` 6 m/s, an undrowned drop becomes a `WaterfallTransfer` above
v²/2g ≈ **1.8 m** (722–727) — in a steep canyon that reclassifies most rapids into
interfaces with authored loss coefficients, weakening the headline claim. Also: dt uses
`predictedResolvedVelocity` (690) while the classifier guarantees only
`maxResolvedVelocity`, and mid-solve exceedance has no defined behaviour.

**I5 — The "typed boundary collection" is a wider refactor than "two extensions" admits.**
It is `SectorEntry::boundary` (`matter_engine.cpp:1353`), `drawn_boundary_with_rung`
(:1667–1675), fine-tile enumeration in weld rebuild (:5813–5824), the weld-pair identity
hash (:1841), staged-artifact adoption (:7869–7880), and a `Missing`-vs-`Empty` semantics
change the code currently documents as fail-soft by design (:7877–7879). Size it as a
transport refactor with the pinned-bitwise terrain tests named as the do-not-move gate.

**I6 — Underwater sun attenuation contradicts the ray-mask policy.** Sun-shadow rays
"exclude the water mask" (1109–1111) but submerged rays are "attenuated by water
absorption along their underwater distance" (1118–1119) — a submerged shadow ray cannot
measure that distance without intersecting the surface the mask excludes. Pick one:
vertical head−y depth from resident summaries, or include the mask with a
distance-recording hit. (The froxel sun ray does trace `gl_RayFlagsOpaqueEXT`,
`vol_scatter.comp:209`, so the no-binary-shadow-on-mist half is consistent.)

**I7 — Two hash-coupling leaks.** (1) `hydrologyHash` is built from *payload* brick
digests (949–953), which are only same-device deterministic — wet-sector bake keys diverge
across GPUs whenever the artifact is rebaked, killing shared caches. (2) The summary
hierarchy is built "through the maximum streamed sector level" (914–916), a function of the
band table, which is not in the artifact input hash (1150–1159) — retuning bands could
require summaries that do not exist. Build to a fixed depth.

---

## 4. Minor / polish

- `envelopeHeadMargin` defined twice and inconsistently: `2.0` (346) vs `2 * finalCellSize`
  (830–831).
- 948 says params sit alongside "habitat"; the actual key is `"biomes"`
  (`matter_engine.cpp:7315`).
- Unstated whether the new hash params are present-but-empty for non-hydrology worlds; if
  present, every world cold-rebakes once (accepted R5 precedent — say so).
- Two of three engine-doc references are dead links in the spec's own worktree.
- Water seam tests are records/welds + image regressions; the repo's proven output-defined
  detector (`crack_scan.py`, which found 22 holes every geometric check missed) is not
  cited. Adapt it for water surfaces.
- Terrain-plan "believability" fixtures are geometric self-checks; no image regression for
  eroded terrain.
- "Static two-sided water-air boundary geometry" should note the composite backface-culling
  shared-state trap this repo already hit.

---

## 5. What is solid

The dependency-baseline section is the best part of the update, verified claim by claim
against `ed9f6ea2`: the params JSON (`matter_engine.cpp:7315`), `rung_voxel`'s 2 m base
(`seam_boundary.h:93`), the single boundary record and its fail-soft fallback (:1353,
:7877–7880), the welder's ±1 rejection (`seam_weld.h:363–370`), the merge-hold soak
numbers from `eea2ded1`, the occlusion loop's 105-tokens/0-mapped/1020-resident
ineffectiveness from `ed9f6ea2`, six-face records, empty-tile publication, and yMin/yMax as
octree extent — all correct, including the honest "not yet effective" caveats.

Also right: neighbour state stays out of bake identity everywhere; `Missing ≠ Empty`; the
dyadic pitch validation (S0=64 in every shipped world); reconnaissance-as-geometry
replacing the coarse LBM pass; the cost-decision timing fix; MRT+Smagorinsky with correct
arithmetic throughout (dt, bytes/cell, MLUPS, checkpoint sizes, ballistic speeds all
check out); freeze-after-spike; and scrupulously honest renderer claims
(`MATERIAL_VOLUME_BOUNDARY` read by nothing, `VolumeEmitterGatherer` test-only, masks
0x01/0x02 at `vk_scene_renderer.cpp:11490`, material 7 water at `material_registry.c:43`).

---

## 6. Open questions before implementation

1. What, mechanically, is a coarse water mesh — iso-surface of aggregated fill, summary
   reconstruction, or ribbon — and how does each terminate at cube faces and cross-level
   welds? (B1)
2. Does water get an overlap band, and if not, what closes sub-coarse-voxel water at
   cross-level faces?
3. Who drives `drawn_level_violations` to zero, by when, and what ships if the candidate
   fix does not get there? (B3)
4. Is the coordinator's schedule a pure function of (state, batch index)? Is the
   accumulating-basin artifact inside or outside the byte-identity gate? (B2)
5. Does world install block on the terrain-plan bake, and what does a cold-start hydrology
   world show while it runs? What does the plan bake cost for the reference canyon? (I2)
6. What canonical erosion iteration schedule makes incremental == cold byte-identical? (I1)
7. What happens when resolved velocity exceeds the envelope mid-solve, and is dt derived
   from the guarantee or a prediction? (I4)
8. Is a 1.8 m resolved-drop ceiling acceptable for the showcase canyon, or does the default
   move with a stated dt cost?
9. Are the new hash params conditionally present, and is cross-device wet-sector cache
   divergence accepted? (I7)
10. Convergence horizon: gravity-wave transit for a 2 km domain is ~300–600 s ≈ 20–40k
    steps at the derived dt — is the 5,000–20,000-step estimate per *section* rather than
    per connected domain, and does the feasibility gate measure the one that matters?
