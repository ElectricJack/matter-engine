# Photoreal object scripts: combined pipeline recommendation

Date: 2026-09-28. Task: `crisp-rapids`. Status: proposed design, research only.
No object was generated, engine capture was run, or model was benchmarked for this
document. Every numerical threshold and budget below is a proposed starting
policy to calibrate on real references. Engine observations in the two source
proposals were made against `33aa006c4028359b2949634f46d9c80a61d5b97f`.

## Decision

Build a small external controller around Matter's JS object system and existing
agent/capture tools. A strong vision/code model chooses an anatomical construction
and writes a readable parameterized generator. Ordinary code validates it, tunes
bounded numeric parameters, invokes Matter, measures fixed-view renders, and
keeps or rolls back each candidate. A model receives localized evidence only
when a structural or material diagnosis is needed. After acceptance, simplify
the source under visual and variation regression checks.

Begin with an angular rock and bark fragment; add a pine cone only after the
first two expose a dependable loop. The first implementation decision is capture
and scoring reliability, then generator quality, then the cheapest model route
that reaches it. Photorealism is an empirical goal, not an asserted result.

### Source and review status

- [Codex proposal](2026-09-28-ai-object-script-pipeline-codex.md), task
  `clear-summit-89`, review `rev-amber-current`, revision 1, SHA-256
  `0b08ffb139c537af58fd89635616f4d8b321008160e08bc4278aba3891f0c46a`.
  Its task was titled Fable, but AQ rerouted it to `deep-high-codex`; this is
  **not** Fable-authored evidence.
- [Astra proposal](2026-09-28-ai-object-script-pipeline-astra.md), task
  `steady-stone-51`, review `rev-bold-meadow`, revision 1, SHA-256
  `750555985233e14fb57dc9e40c48410c2f8561e8265a29bfac45161b514bebba`.
- Both Reviews entries were `in_review` with no comments when read for this
  synthesis. Neither proposal reports an executed Matter accuracy, speed, or
  cost comparison. A future Fable trial must run Fable and record its observed
  model ID; the task label cannot stand in for that trial.

## Agreement and decisions where the proposals differ

Both proposals recommend a real Matter JS family generator, an external
render–measure–edit controller, parameter search before repeated model rewrites,
separate geometry/material/light diagnosis, fixed multi-view captures, locked
tests, unseen seeds, and a source/readability objective. Both identify missing
camera control, numeric render channels, visible-detail readiness, and durable
parameter persistence. Both warn that VG admission, VT residency, and RT
availability must be observed on the intended renderer; more geometry or VT
texels alone do not establish image quality.

| Decision | Codex proposal | Astra proposal | Combined pick and reason |
| --- | --- | --- | --- |
| Initial author/model mix | Opus 5.5 authors and edits; Astra/Fable diagnose hard cases | Astra proposes structure; cheaper local editor after screening | Start the *prototype* with one available strong author, without claiming it wins. Benchmark all-Opus, all-Astra, all-Fable, and one **fixed** mixed route. Use Opus author plus deterministic search and Astra structural escalation as the first mixed route: it directly tests the lower-price author hypothesis. A Sol local-edit route is a separately named later ablation, not silently folded into it. |
| Reference set | Eight natural specimens, 12 calibrated views each | Two development specimens, then six locked evaluation specimens with eight same-specimen views | Use 2–3 prototype objects, then six *new* locked specimens for the model matrix. Six is enough to expose failure modes at bounded cost, not enough to declare universal superiority; expand if intervals overlap. Target eight fixed-light views plus relighting for each benchmark specimen; add top/underside where accessible rather than requiring impossible views. |
| Acceptance bands | Opaque pilot mean/worst IoU 0.90/0.85, contour error 0.02 | Mean/worst IoU 0.95/0.90, contour error 0.01 | Treat 0.90/0.85 and 0.02 as provisional **pilot diagnostic targets**, not a final quality claim. Calibrate final class-specific bands from repeat noise and blind human judgments, then freeze before benchmark. A higher arbitrary IoU can misclassify masks or uncertain boundaries as model failure. |
| Optimization budget | 45 minutes, $15, 16 model calls, 60 fitting sweeps | 30 minutes, $15, 12 calls, 64 bake evaluations including validation | Pilot one candidate under a 45-minute, $15, 12-call, 64-bake cap, counting failed attempts and reserving a final suite. Measure whether a complete run fits; revise once for **all arms** before locking the benchmark. A sweep and a bake are different units, so report both rather than pretending the source budgets match. |
| Early engine work | Calibrated fixture/readiness, then controller, then benchmark | Capture/scoring pilot, one-family optimizer, then benchmark | First implement a fixture and capture confidence contract using existing tooling. Build the external controller on it. Add live visible-set readiness and numeric channels before claiming fully automatic photo-accuracy gates; manual audited captures can support a provisional prototype meanwhile. |

## Asset and reference contract

The deliverable is a `class ... extends Part` JS generator with `static params`
and `build(p)`, its versioned material/helpers, a human-readable parameter
contract, presets, and reproducible evaluation records. Use existing Matter
patterns such as [Rock.js](../../projects/world_demo/objects/terrain/Rock.js),
[MountainRock.js](../../projects/world_demo/objects/terrain/MountainRock.js),
and [ConiferTree.js](../../projects/world_demo/objects/vegetation/conifer/ConiferTree.js).
Do not invent a new DSL, a generic JS displacement API, or a material graph.
Significant silhouette changes require geometry; surface height/parallax is for
shallow detail. The output must retain family identity over new seeds.

The initial contract permits at most eight influential semantic scalar parameters
besides seeds and sampling quality. Each has a name, physical unit, type, default,
bounds, expected visual effect, and allowed correlations. Examples: overall size,
aspect ratio, fracture depth, bark groove pitch, erosion, cone scale density.
Keep separate integer `shapeSeed` and `surfaceSeed` streams with explicit
`rng(seed ^ salt)` calls. Matter's host seed includes merged parameter JSON,
so using implicit `Math.random()` would reroll shape during a size adjustment.
Sampling resolution must refine the same construction, not select a new specimen.
Test defaults, extrema and representative correlated presets; no claim of a
complete Cartesian sweep is made.

Compactness is constrained by fidelity and editability. Report object-local
non-comment tokens/bytes, unique reachable authored helper tokens/bytes,
parameter count, and bake/mesh cost against a frozen shared-library baseline.
Charge a new helper to the asset until it is independently reused. Minification,
encoded scans, giant numeric arrays, camera-dependent geometry, and hiding
complexity in an uncounted import do not qualify. A human unfamiliar with the
generator must be able to make a named semantic change and explain the effect;
record time and errors. Simplify only after visual acceptance, one feature at a
time, with the same view/seed suite rerun. The smallest *passing readable*
generator wins, rather than the shortest text unconditionally.

Record original reference hashes and provenance, family/specimen IDs, measured
dimensions, camera intrinsics/extrinsics and uncertainty, illumination/exposure,
checked masks and uncertain boundaries, and fit/validation/test assignments.
Several views of one specimen may be pixel-aligned to one fixed seed; photos of
different specimens constrain a family distribution and must not be scored as
one object's pixel targets. A single uncalibrated photo supports a plausible
extension, not measured hidden-side reconstruction. Calibrate scale/camera/light
once and freeze them across candidates; per-candidate reframing, color correction
or shadow-painted albedo must not mask object errors.

## Controller: capture, compare, edit

Use a dedicated ordinary production world with one generated root and a fixed
stage. [RockGallery.js](../../projects/world_demo/scenes/terrain/RockGallery/RockGallery.js)
shows the world-root pattern. Workbench is useful interactively, but its private
view is not the production scene the typed scene APIs attest. Keep references,
scorer, cameras and acceptance rules outside the model's write scope. Give each
candidate an immutable source/parameter hash and unique capture paths.

1. **Plan and validate.** Retrieve a small, pinned capability pack and at most
   two relevant examples. A model proposes one preferred construction and up to
   two alternatives with observable predictions. Reject unsupported APIs,
   invalid JS/geometry, impossible bounds, missing children and resource limits
   before a render. Retain the last verified incumbent.
2. **Bake and capture.** On MSVC Windows, use
   [drive.py](../../MatterEngine3/tools/drive.py) for fixed timelines and
   [matter_agent.py](../../tools/matter_agent.py) for discovery, typed root
   updates/jobs and `viewport.capture`. `procedural.update` only changes declared
   flat scalar fields in the current session; source edits require atomic file
   replacement and reload. Reacquire root identity after rebake, check job state
   and provenance, and persist accepted overrides to source/presets. Fresh-launch
   reproduction is required. `drive.py` needs an explicit
   `MatterEditor/build/windows-msvc/editor.exe` path in this setup.
3. **Prove the pixels belong to this candidate.** Record source closure and
   engine/executable hashes, canonical params and seeds, full camera *projection*
   and pose, framebuffer/viewport mapping, lighting/exposure, render path,
   VG/VT/LOD/DLSS settings, GPU/driver, cache state, jobs, command receipts,
   image hashes and the `captured` presented-frame metadata. Decode the PNG and
   verify dimensions and nonempty foreground. Reject timeouts, stale files,
   property refusal, failed bake, unavailable RT and capture transport errors.
   Place `wait_event bake.finished` before `wait_idle`; neither idle nor a fixed
   90-frame hold proves VG/VT detail residency. Repeat unchanged captures on the
   same device to measure noise, including a cold restart. Use post-exit VT
   traces, geometry logs and two separated captures for provisional readiness;
   label missing live visible-set evidence as provisional.
4. **Measure by cause.** Fit silhouette IoU, symmetric contour distance,
   landmarks and dimensions before materials; then compare masked, consistently
   preprocessed LPIPS, broad color/roughness response and local detail at matched
   light. Check second-light highlights, raw-albedo/normal/depth *diagnostic*
   views, grazing angles and far LOD. Displayed debug PNGs are not metric float
   AOVs. An independently checked mask and uncertainty region are required.
   Keep a per-view metric vector and worst-view result; don't let a good front
   view average out a broken back. Failed captures have unavailable quality
   scores, never a favorable loss.
5. **Choose one action.** With a fixed seed, use bounded coordinate/pattern
   search on at most eight active parameters and paired renders. Do not assume
   the JS–CSG–meshing–VG–VT pipeline is differentiable. When parameter fitting
   plateaus, send the VLM a labeled reference/render packet, failure crops,
   relevant diagnostics, score deltas and a small source excerpt. Require at most
   three ranked defects, each naming views, geometry/material/light/readiness
   cause, confidence, edit scope and predicted measured effect. Apply one scoped
   patch against an exact base source hash. Re-render A/B at identical settings;
   retain it only on measured improvement beyond the repeat-noise band and no
   mandatory view/resource regression. Otherwise roll back. Keep a short ledger
   of rejected hypotheses to prevent cycles. Infrastructure errors get bounded
   infrastructure retries, not creative JS edits.
6. **Checkpoint and stop.** Use cheap 512-pixel fit views to screen candidates
   and fixed 1024-pixel/all-view plus RT close-ups for promotion, but calibrate
   screen ranking and never compare scores across fidelity profiles directly.
   Development validation views select checkpoints; locked test views and seeds
   remain hidden until the frozen final artifact. If a test failure causes an
   edit, record that failure and use a new locked set for any later unbiased
   claim. Recheck persisted reload, seeds, parameter boundaries, raster/RT
   behavior and resource limits before acceptance.

Calibrate metric weights and object-class bands on the prototype, then freeze
them. For the opaque pilot, 0.90 mean/0.85 worst-view IoU, 0.02 image-diagonal
mean contour distance and major dimensions within 5% are *diagnostic starting
values*. LPIPS has no universal pass threshold: estimate it from acceptable
same-specimen renders and renderer noise. A blind human rubric rates identity,
material believability and cross-view consistency; the provisional passing band
is median at least 4/5 on each and no major defect confirmed by two of three
raters. Final acceptance requires all visual, geometry, resource, readability,
variation and persisted-reload gates; a scalar loss alone is insufficient.

For numeric candidate acceptance, estimate unchanged-render paired score
differences from at least five repeats. Use the larger of 1% normalized loss and
the empirical 95th percentile repeat difference as a provisional minimum gain;
rerender borderline finalists. Stop with the best verified artifact and explicit
unresolved defects if three complete rounds fail to clear that minimum, at most
one structural escalation fails, a readiness check stays untrustworthy, or any
time/cost/call/bake cap binds. A budget stop is not visual convergence. Test ten
fresh seeds with fixed views and semantic boundary presets; report every invalid
seed, family-identity failure and pairwise structural diversity, not a curated
contact sheet. Calibrate a diversity threshold against renderer noise and real
within-family differences before calling the generator varied.

## Model and tier plan; measured comparison

No proposal or review supplies observed Matter-script accuracy, speed or cost
for Opus 5.5, Astra or Fable. Model identity must be the observed API result,
with provider/version and fallback recorded for every call. If Fable is
unavailable, mark its arm incomplete; a Codex reroute is not a Fable result.

| Tier | Proposed role | Promotion condition |
| --- | --- | --- |
| Deterministic code | Schema validation, paired numeric search, scoring, cache, rollback and budget ledger | Default; no model call for arithmetic tuning |
| Opus 5.5 | First practical author/editor candidate and full single-model baseline | Keep in production route only if held-out quality meets the frozen band at lower cost/time per accepted asset |
| Astra | Full baseline and one structural escalation in the fixed mixed route | Escalate when two validated local edits fail, representation/topology must change, or cross-view evidence conflicts |
| Fable | Full baseline; optional separately labeled finalist critique | Use actual Fable API identity; call as critic only if it improves blind acceptance per added cost |
| Sol or similar cheaper vision/code tier | Later local-patch ablation | Admit only after measured false-accept/regression rate is acceptable; do not replace deterministic checks with a model |

As of 2026-09-28, official standard short-context API lists show input/output
prices per million tokens of Astra $10/$50 and Sol $2/$10
([OpenAI pricing](https://developers.openai.com/api/docs/pricing)); Fable 5.1
$10/$50 and Opus 5.5 $4/$20
([Anthropic pricing](https://platform.claude.com/docs/en/about-claude/pricing)).
These are planning rates, not measured run costs. Verify access, rate cards,
context/service tiers and billing at execution. As a text-only illustration,
20,000 uncached input plus 4,000 output tokens cost $0.40 for Astra or Fable,
$0.16 for Opus, and $0.08 for Sol at those rates. Image, cached/write, tool,
reasoning and other billed usage can change totals; use actual provider usage,
not this example, for decisions.

After the prototype, freeze six new real specimens: angular rock, river pebble,
bark fragment, broken branch, pine cone and curled dry leaf. The last is a
challenge case reported separately if renderer limitations prevent fair
comparison. For each, reserve four calibrated same-specimen fit views, two
development validation views and two locked test views across angles/elevations,
plus a second-light validation and test view. Keep family images and ten unseen
generator seeds out of pixel targets. Run all-Astra, all-Fable, all-Opus and the
fixed Opus-author/Astra-escalation route, three independent authoring attempts
per specimen: **6 × 4 × 3 = 72 attempts**. The controller, examples, hardware,
view splits, semantic contract, metric versions and budgets are identical.
Randomize arm order and serialize rendering on one GPU. Record first valid
candidate as a one-shot baseline; prototype-only ablations compare VLM-only,
metric-only and full feedback under matched starts.

Begin with a common proposed cap per attempt of $15 billed model spend, 45
active minutes, 12 model calls and 64 bake evaluations, whichever binds first.
Reserve enough bakes/time for a final reload, held-out captures and variation
checks; failed attempts and readiness retries consume their caps. Two prototype
probes must show that a complete final suite fits. If not, change the caps once
and relock them for every arm before collecting benchmark results. Do not compare
an arm given a longer repair budget to a shorter one.

The scorecard reports: valid-bake and accepted-generator rates with failures in
denominators; fit, validation and locked-test per-view geometry/appearance and
worst-view metrics; three-rater blinded identity/material/cross-view judgments;
ten-seed plausibility/diversity and human editability; time to first valid render
and first accepted checkpoint; model, bake, VG/BLAS/VT wait, capture, scoring and
queue latency separately, with cold/warm status; and actual provider token/image/
cache/tool/retry charges, GPU time and human review time. Include quality versus
time/spend curves and **total spend across all attempts divided by accepted
generators**. Count censored budget failures. Use paired per-specimen comparisons
and intervals clustered by specimen, not image count. Predeclare a practical
noninferiority margin (initial proposal: at most 10 percentage points lower
acceptance and 0.25/5 lower blind score). Choose the least costly qualifying
route, then compare time; if six specimens cannot resolve the margin, report no
winner and expand the set. Do not use a model's self-critique as the verdict.

## First prototype, engine gaps, and exit gates

| Phase | Work | Evidence to advance |
| --- | --- | --- |
| 0. Capture fixture | Photograph angular rock and bark with scale/color target; make a single-root production world, fixed camera/light manifest, MSVC build and unique capture paths. Run five unchanged captures including a cold restart and injected stale PNG/timeout/unsupported-RT cases. | Proven source-to-frame identity, measured repeat band, decoded images, and explicit provisional readiness classification. Deliberately invalid input fails quality scoring. |
| 1. One-family loop | Author an angular-rock generator, contract and controller candidate store. Fit multiple views, patch one scoped structural defect, simplify, persist, reopen and test ten fresh seeds. | Geometry/material/worst-view gain beyond noise without validation regression; no invalid outputs at registered parameter boundaries; readable source closure and resource evidence. |
| 2. Transfer | Run bark fragment with the same controller and a second real specimen; optionally add pine cone to exercise repeated correlated structure. Freeze masks, rubrics and benchmark caps. | Useful family variation and same-view reproducibility across two families; failures attributed to script, capture or renderer rather than hidden by retries. |
| 3. Model comparison | Run the locked 72-attempt matrix only after phases 0–2. | Full accuracy, time, cost and failure report with observed model IDs and uncertainty; routing choice only if evidence supports it. |

The external controller can begin with today's [agent protocol](../agent/agent-protocol.md),
[control surface](../agent/control-surface.md), and
[QA cookbook](../agent/qa-cookbook.md). It should preserve source closure,
capture receipts and exact renderer configuration in each candidate manifest.
The engine-facing gaps are:

1. A visible-set readiness/status barrier tied to the captured frame for desired
   VG cut, BLAS and VT content, refinement and temporal state. `wait_idle` and
   pixel stability can accept a stably coarse object; post-exit traces are only
   provisional evidence.
2. A bounded camera setter for projection/intrinsics and synchronized object-ID
   mask, linear color, depth, normal and material channels. Existing debug PNGs
   are display diagnostics, so until these exports exist, use audited masks and
   label numeric photo alignment confidence accordingly.
3. Portable parameter ranges, integer/correlation constraints and durable
   overrides. Today's `procedural.update` has typed session edits, not physical
   ranges or automatic source persistence. Reload must reproduce chosen values.
4. A repeatable temporal freeze/reset and asset-level VG/VT/BLAS resource report.
   Treat unavailable RT as an unavailable track, and verify VG admission and VT
   density/fallback on target hardware. `MATTER_GEOMETRY_RASTER_ONLY=1` requires
   restart before a meaningful page-RT pass.

These gaps are proposed follow-up implementation work. This document requests
approval of the capture/scoring prototype and bounded controller, with the
model comparison as a measured next decision. It contains no engine code.
