# docs/ Index

Index of this directory. `MatterEngine3/docs/` has its own separate index
(`MatterEngine3/docs/README.md`) for engine-internal architecture docs
(`architecture.md` — bake pipeline/PartGraph/determinism; `rendering.md` — the
viewer's per-frame composition and TLAS/BLAS ray tracer; `authoring.md` —
part/world/shared-lib schemas and test coverage) plus `event-system.md`
(the notification/command/observable-model design spec) — not duplicated here.

## docs/agent/ — agent-facing reference docs

Written for AI agents/scripts driving the engine from the command line, as
opposed to the design/findings docs below, which are written for human
engineers reading in context.

- **[agent/control-surface.md](agent/control-surface.md)** — the complete
  external control surface: env vars, the `MATTER_CMD_FIFO` command grammar,
  events, and the three launch rules.
- **[agent/qa-cookbook.md](agent/qa-cookbook.md)** — copy-paste recipes for
  builds, screenshots, FIFO sessions, replay/diff, the Vulkan smoke gate, the
  seam suite, perf runs, fly-through soaks, and headless test targets.
- **[agent/issue-system.md](agent/issue-system.md)** — the in-editor capture/
  file/replay pipeline: `issue.md`/`state.json` schema, GUID scheme, and the
  capture-vs-submit timing gotcha.

## Design docs

- **[contour-seam-design-2026-08-13.md](contour-seam-design-2026-08-13.md)** —
  "Seams by shared contour: no overlap, no welder." The decision (2026-08-13,
  from issue `736f92da`) to mesh each tile's boundary against a canonically
  shared contour instead of the runtime overlap-band welder.
- **[lod-vt-redesign-2026-08-04.md](lod-vt-redesign-2026-08-04.md)** —
  "Representation": a from-scratch redesign of LOD + virtual texturing,
  written against `lod-vt-system-walkthrough-2026-08-04.md`'s audit.
- **[rt-tlas-cpu-mirror-redesign-2026-08-07.md](rt-tlas-cpu-mirror-redesign-2026-08-07.md)**
  — design note for moving the ray-tracing TLAS CPU mirror from O(world) to
  O(changed) per frame, prompted by a measured 26 ms/38 fps StreamMountain
  fly-through frame that was 82% TLAS-rebuild-bound.
- **[terrain-nested-sector-lod-2026-08-08.md](terrain-nested-sector-lod-2026-08-08.md)**
  — design note: double sector size as LOD resolution halves, so the streamed
  disc stops paying a fixed per-part cost O(R²) times.
- **[volumetric-sectors-design-2026-08-10.md](volumetric-sectors-design-2026-08-10.md)**
  — design for 3D (fully volumetric, not just XZ-columnar) nested sector
  streaming, an invariant seam contract, and occlusion.
- **[volumetric-sectors-m0-resolutions.md](volumetric-sectors-m0-resolutions.md)**
  — decisions made resolving friction between the design above and the code as
  actually implemented at M0; meant to fold back into the design doc.
- **[streammountain-refactor-plan-2026-08-09.md](streammountain-refactor-plan-2026-08-09.md)**
  — analysis/goals for refactoring the StreamMountain scene tier and its
  JS↔native boundary (less code, push compute native, readability).
- **[streammountain-refactor-implementation-2026-08-09.md](streammountain-refactor-implementation-2026-08-09.md)**
  — the executable companion to the plan above: ordered steps, anchors, gates,
  stop conditions.
- **[habitat-tape-sketch-2026-08-08.md](habitat-tape-sketch-2026-08-08.md)** —
  sketch (no code changed) for evaluating habitat/ecology sampling as tape-
  machine data instead of per-candidate JS `fbm` calls.

## Findings / measurements

- **[ao-bake-findings-2026-07-16.md](ao-bake-findings-2026-07-16.md)** — root
  cause and rollback notes for dark AO seams at chunk/cluster boundaries after
  the Vulkan+RTX port.
- **[asset-store-benchmark-2026-08-05.md](asset-store-benchmark-2026-08-05.md)**
  — measures AssetStoreLib's packed-blob reads against the small-file storm it
  replaces (M5 first half).
- **[code-review-2026-07-07.md](code-review-2026-07-07.md)** — repo-wide code
  quality/performance review (~74k lines, five parallel passes); cross-cutting
  themes like copy-paste vendoring drift.
- **[lod-vt-system-walkthrough-2026-08-04.md](lod-vt-system-walkthrough-2026-08-04.md)**
  — read-only audit of the whole JS-to-lit-pixels LOD/VT path: why it's
  complicated and what the minimum version could be. Feeds the redesign doc
  above.
- **[scatter-bake-profile-2026-08-09.md](scatter-bake-profile-2026-08-09.md)**
  — where a sector bake's scatter time actually goes; supersedes two earlier,
  each-convincing-but-wrong answers to the same question.
- **[sector-bake-time-findings-2026-07-30.md](sector-bake-time-findings-2026-07-30.md)**
  — where the 5,000-sector StreamMountain fill's per-sector time actually
  goes; corrects an earlier "the bake is 47.7 ms" figure that measured about a
  third of the real cost.
- **[streaming-fill-throughput-findings-2026-07-30.md](streaming-fill-throughput-findings-2026-07-30.md)**
  — full measurement chain behind moving `VkScenePart` build to the bake
  worker (publish 13.9 ms → 1.46 ms).
- **[vt-mesh-entry-allocation-2026-08-09.md](vt-mesh-entry-allocation-2026-08-09.md)**
  — traces render-thread frame spikes (worst 56.6 ms vs. 3.25 ms median) to
  per-entry Vulkan allocations.
- **[seam-suite-2026-08-13.md](seam-suite-2026-08-13.md)** — what the terrain
  seam suite (`MatterEngine3/tools/seam_suite.sh`) found on SeamLab, and why a
  new cave-free world was needed to answer the question at all.

## Runbooks / how-tos

- **[debugging-feedback-loop.md](debugging-feedback-loop.md)** — the
  screenshot feedback-loop technique for debugging opaque GPU/rendering bugs
  (render → read exact pixels → one hypothesis → repeat), generalized from two
  specific raytracer bugs.
- **[occlusion-cull-demo-2026-08-12.md](occlusion-cull-demo-2026-08-12.md)** —
  how to reproduce and see the M4 occlusion cull working on StreamCaverns, and
  what shipped vs. what the design sketch originally proposed.

## Baselines

- **[baselines/README.md](baselines/README.md)** — why baseline PNGs aren't
  committed (GPU/driver/machine-specific, and RT isn't bit-exact run to run),
  the per-shot diff gates (PomProofBrick primary, ChartVtProof secondary), and
  the measured noise floor/failability proof behind them.
- **[baselines/capture-replay-baseline.sh](baselines/capture-replay-baseline.sh)**
  — captures one `MATTER_REPLAY` shot for milestone gating, wiping the world's
  cache first so the comparison exercises the whole bake+render path.
- **[baselines/seam-soak.sh](baselines/seam-soak.sh)** — the volumetric-sectors
  M0 acceptance run: a scripted `MATTER_CAM_PATH` fly-through with
  `MATTER_SEAM_TRACE` on, over StreamCaverns/StreamMountain.

## Other

- **`perf/`** — raw perf-run captures (e.g.
  `streammountain-baseline-2026-08-09.txt`), referenced by the findings docs
  above rather than read standalone.
- **`screenshots/`** — reference screenshots for the sub-projects (mostly the
  `Prototypes/` examples), not itemized here.
- **`superpowers/`** — planning/spec workspace: **81** dated implementation
  plans in `plans/`, **88** dated design specs in `specs/`, plus top-level
  `backlog.md` and `REVIEW-LOG.md`. Not itemized here — treat as a searchable
  archive, not a curated index.
