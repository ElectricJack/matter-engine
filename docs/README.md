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

- **[River character integration](superpowers/specs/2026-08-30-river-character-controller-integration-design.md)**
  — fixed-step walking and editor controls are implemented; real-world proof is in progress.
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

Remaining dated files under `superpowers/plans/` and `superpowers/specs/` are
unfinished or explicitly deferred. They do not outrank the roadmap.

## Agent and QA references

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
