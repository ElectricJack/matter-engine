# MatterEngine documentation

This index points to current authority. Historical implementation records are
separated from superseded designs so old plans do not silently become new
requirements.

## Current priorities

- **[Roadmap](../ROADMAP.md)** — the short, ordered product roadmap.
- **[Implementation-gap audit](findings/spec-implementation-gap-audit-2026-08-28.md)**
  — evidence behind the retained gaps and retired work.
- **[Render eligibility and documentation lifecycle](superpowers/specs/2026-08-28-render-eligibility-and-document-lifecycle-design.md)**
  — approved part/instance `rayTraced` policy and raster-only water direction.

## Active engine designs

- **[Water-animation memory admission](superpowers/specs/2026-08-30-water-animation-memory-gates-design.md)**
  — remaining shared publication/cache/playback limits, independent of waterfall visuals.
- **[LOD/VT redesign](lod-vt-redesign-2026-08-04.md)** — incomplete
  visibility, proxy-world, and unified-budget endpoint.
- **[RT TLAS CPU mirror redesign](rt-tlas-cpu-mirror-redesign-2026-08-07.md)**
  — the missing O(changed) stable-slot RT world update.
- **[Shared contour seams](contour-seam-design-2026-08-13.md)** — current
  terrain boundary contract.
- **[Volumetric sectors](volumetric-sectors-design-2026-08-10.md)** — current
  volumetric streaming architecture.
- **[Engine-internal architecture index](../MatterEngine3/docs/README.md)** —
  bake pipeline, rendering, authoring, events, tools, and active engine notes.
- **[Baked GI lightmaps](bake-gi.md)** — `matter bake gi`: offline per-instance
  lightmap bake (sun, sky, bounce), outputs, UV sidecar contract for the OBJ
  exporter, three.js usage, validation and limits.

Remaining dated files under `superpowers/plans/` and `superpowers/specs/` are
unfinished or explicitly deferred. They do not outrank the roadmap.

## Agent and QA references

- **[Agent protocol v1](agent/agent-protocol.md)** — structured discovery,
  request/result envelopes, identities, revisions, limits, and script client.
- **[Control surface](agent/control-surface.md)** — environment variables,
  command FIFO grammar, and events.
- **[QA cookbook](agent/qa-cookbook.md)** — builds, screenshots, replay/diff,
  smoke, seam, and performance gates.
- **[Issue system](agent/issue-system.md)** — capture, file, and replay
  workflow.
- **[Debugging feedback loop](debugging-feedback-loop.md)** — visual GPU bug
  diagnosis workflow.
- **[Baselines](baselines/README.md)** — screenshot baseline policy and tools.

## Findings and measurements

- [Castle frame pacing](frame-pacing-2026-09-12.md) records acquisition stalls,
  Remote Desktop limits, presentation comparisons, the live FPS limiter and
  CPU interval diagnostics.
- [Primary light culling](primary-light-culling-2026-09-12.md) records conservative
  receiver masks, shader isolation, rejection audits and visible comparisons.
- [Lighting quality, adaptive shadows and pass costs](lighting-quality-and-adaptive-shadows-2026-09-12.md)
  records output-aware material sampling, optional adaptive primary shadows,
  profiling timers, and the descriptor-layout regression investigation.
- [DLSS build and remaining optimizations](dlss-build-and-next-optimizations-2026-09-12.md)
  documents the native Streamline build, staged runtime, visible comparison
  and next performance priorities.
- [GI reconstruction and independent reflection resolution](gi-reconstruction-2026-09-12.md)

- [GI quality options](gi-quality-options-2026-09-12.md)
  compares full GI, reduced indirect resolution, diffuse-off and direct-only
  lighting, including measured cost and glass/reflection tradeoffs.
- [RT lighting roadmap status](rt-lighting-roadmap-status-2026-09-12.md)
  compares all eight original optimization items with current code and
  acceptance evidence, and orders the remaining implementation work.
- [RT lighting implementation and measurements](rt-lighting-implementation-2026-09-12.md)
  records the first optimization milestone, visible benchmark methodology,
  raw per-pass timing, and the limits of weighted secondary light sampling.
- [RT, GI, and lighting performance review](rt-gi-lighting-performance-review-2026-09-11.md)
  audits the castle worktree's renderer, distinguishes measured costs from
  shader-cost hypotheses, and ranks proposed lighting optimizations.
- [Water-field interpolation at mesh edges](findings/water-field-fringe-interpolation-2026-08-30.md)
  records the bounded wet-neighbor fix and its remaining visual limits.
- [River character integration acceptance](findings/river-character-controller-integration-acceptance-2026-08-30.md)
  records the two-process bank-path proof; its [design](completed/superpowers/specs/2026-08-30-river-character-controller-integration-design.md)
  and [implementation plan](completed/superpowers/plans/2026-08-30-river-character-controller-integration.md)
  are completed records, not remaining roadmap work.

`findings/` contains acceptance records and root-cause reports. Other current
measurement notes remain at the top of `docs/`, including LOD/VT,
StreamMountain, seam, allocation, and bake-throughput investigations. They are
evidence, not roadmap commitments.

## Documentation lifecycle

- **[Completed](completed/README.md)** — implemented plans and milestone
  designs retained as history. See its [manifest](completed/MANIFEST.md).
- **[Deprecated](deprecated/README.md)** — superseded or retired decisions,
  ready for later removal after reference review. See its
  [manifest](deprecated/MANIFEST.md).

Documents that exist only on historical branches were audited but were not
copied into either archive.
