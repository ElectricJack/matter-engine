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

- **[Virtualized procedural geometry](superpowers/specs/2026-09-18-virtualized-procedural-geometry-design.md)**
  — design for large unique meshes, displacement and debris through a connected
  triangle hierarchy, geometry-page streaming and ray-tracing integration;
  [implementation plan](superpowers/plans/2026-09-18-virtualized-procedural-geometry.md).
  Builds on world streaming and replaces further POM expansion as the proposed
  direction for new geometric detail. The opt-in mountain rock pilot connects
  CPU hierarchy/page foundations to world streaming, GPU selection and RT.
  Production scaling and terrain acceptance remain open.
  [BLAS cache, performance and terrain follow-up](superpowers/plans/2026-09-18-blas-cache-and-terrain-geometry.md)
  records the next implementation sequence.

- **[Binary asset-page cache](superpowers/specs/2026-09-18-binary-asset-page-cache-design.md)**
  — AssetStoreLib adoption for spatially organized binary pages, bounded bulk
  reads, retained RAM views, append-only updates and coherent manifest commits.
  Includes the storage checklist for geometry streaming and cold/warm benchmark
  requirements; storage foundations are implemented, with production integration pending.

- **[Asset export](superpowers/specs/2026-09-16-asset-export.md)** — static OBJ/MTL,
  GLB and conventional PBR/height images; [usage](agent/asset-export.md) and
  [frozen asset-authoring build](agent/asset-handoff.md).

- **[VT reliability and throughput](superpowers/specs/2026-09-14-vt-reliability-and-throughput-design.md)**
  — stable residency, local replacement, protected coarse coverage and measured
  update latency; [implementation plan](superpowers/plans/2026-09-14-vt-reliability-and-throughput.md).
  This foundation remains under implementation; strict performance acceptance is deferred
  while procedural materials and visual results take priority. Its targets are unchanged.
  [Current native evidence](agent/evidence/2026-09-15-vt-feedback/README.md) covers retained
  raster/RT/POM inputs and ordinary VT wait removal; measured performance targets remain open.
- **[Layered surface texturing](superpowers/specs/2026-09-14-layered-surface-texturing-design.md)**
  — current visual-development priority: direct DSP/SDF generation, analytic
  geometry placement, optional cached physics, generated/authored splats,
  coherent POM/height blending and terrain/building contacts;
  [implementation plan and revised working objective](superpowers/plans/2026-09-14-layered-surface-texturing.md).
  The expanded [Streaming Mountains environment plan](superpowers/plans/2026-09-17-streammountain-environment.md)
  adds rock/boulder families and fields, vegetation, roads, houses, tunnels and
  power lines while retaining unresolved VT, material and acceptance work.
  Wang sources remain optional for suitable repeating materials and legacy content.
  [StreamMountain material pass](agent/evidence/2026-09-16-streammountain-materials/README.md)
  is the current scene priority: continuous GPU terrain materials, removal of
  five terrain atlas jobs, footprint filtering and authored terrain VT density.
  [Geometry-derived clay brick review](agent/evidence/2026-09-15-clay-brick/README.md)
  covers rounded/chipped source solids, denser pockmarks/scratches, native finite
  projection and textured source previews; production wall stamps remain open.
  [Finite material bake and filtering](agent/evidence/2026-09-15-face-material/README.md)
  adds all-six-face GPU material evaluation and footprint-filtered sampling;
  compressed source artifacts and wall VT binding remain open.
  [Finite sources through VT](agent/evidence/2026-09-15-stamp-vt/README.md)
  verifies projection-height conversion, bounded uploads, immutable source
  edits and eight real clay variants across compressed VT pages. It also fixes
  a BC7 color fringe at brick/mortar boundaries. JS/provider wall hookup and
  general layered splats remain open.
  [Part-local finite recipes](agent/evidence/2026-09-15-part-surface/README.md)
  adds the shared JS material declaration, six-face planning and a native
  twelve-triangle brick receiver.
  [Whole-wall integration and native evidence](agent/evidence/2026-09-15-wall-surface/README.md)
  now draws three box receivers (36 triangles), with geometry-baked clay and
  recessed mortar composed into VT. The earlier flat per-brick proof is superseded.
  [Whole-wall VT composition](superpowers/specs/2026-09-15-whole-wall-vt-composition.md)
  records the receiver, wrapping and projection contracts.
  [Brick maze examples](agent/evidence/2026-09-15-brick-maze/README.md)
  add 20 walls with three heights, L/U corners and two curve radii using shared
  brick sources and wall-scale weathering.
  [Connected POM validation](agent/evidence/2026-09-16-connected-pom/README.md)
  covers internal planar, folded and diagonal chart boundaries; streamed-part
  seams and complete visual acceptance remain open.
  [Shared material-page storage](agent/evidence/2026-09-16-shared-vt-pixels/README.md)
  separates private coverage/geometry from material pixels with copy-on-write
  and native ownership/sampling checks. Periodic wall mapping, sparse weathering
  overrides and reduced reserved memory remain open.
  [Explicit periodic brick modules](agent/evidence/2026-09-16-periodic-material-module/README.md)
  define repeat counts/phase separately from finite wall dimensions, with whole
  ends and native 1×/2×/4× box proofs. The
  [material-domain plan](superpowers/plans/2026-09-16-periodic-material-domains.md)
  covers independent module addressing and sparse instance layers; native
  composition still uses receiver-space pages at this checkpoint.
  [Native periodic material sampling](agent/evidence/2026-09-16-periodic-material-sampling/README.md)
  verifies independent receiver/material lookup and odd-sized mip fallback in
  741 GPU probes.
  [Native periodic material production](agent/evidence/2026-09-16-periodic-material-producer/README.md)
  bakes real brick sources into an independent wrapped module, with exact
  repeated-channel equality through compressed mip tails.
  [Independent module residency](agent/evidence/2026-09-16-periodic-material-residency/README.md)
  adds shared ownership, tail-gated activation and GPU retirement/reuse checks.
  Scene binding and sparse overrides remain open.
  [Paired receiver/material feedback](agent/evidence/2026-09-16-periodic-material-feedback/README.md)
  carries both independent page requests through one visible-fragment attachment;
  it records the memory cost and native validation status. Wall mapping and POM
  still need to populate the material request from actual draws.
  [Receiver material mapping](agent/evidence/2026-09-16-receiver-material-mapping/README.md)
  records the planar mapping implementation and its native validation status;
  generic scene authoring and mapped corner/curve captures remain open.
- **[Sparse voxel forest implementation](superpowers/plans/2026-09-13-sparse-voxel-forest.md)**
  — generic dense-geometry representations and GPU hierarchy targeting 250,000
  procedural trees within 5 ms of tree-rendering GPU time (GI optional); endpoint unachieved.
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
- **[OBJ export](export-obj.md)** — `matter export obj`: taking a baked Part out
  of the engine as OBJ + MTL + PBR maps, and what that format cannot carry.

## Findings and measurements

- [VT memory density](findings/vt-memory-density-2026-09-14.md)
  measures reserved capacity, occupied pages, chart padding and triangle coverage
  in native terrain/building captures; identifies packing and coarse-tail costs.
- [VT stability and latency review](findings/vt-stability-and-latency-review-2026-09-14.md)
  audits global invalidation, mandatory-tail queue loss, preparation-cache churn,
  and the performance/continuity requirements preceding layered surface materials.
- [Texturing appearance, blending, and generation review](findings/texturing-system-review-2026-09-14.md)
  traces terrain and finished-surface materials, records CPU preparation timings
  and cached-asset repetition measurements, and ranks improvements against UE5 techniques.
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

### Sector resolution paging (2026-09-18)

- [Design: sector-owned resolution bundles and preallocated memory banks](superpowers/specs/2026-09-18-sector-resolution-paging.md)
- [Implementation plan and allocation/performance gates](superpowers/plans/2026-09-18-sector-resolution-paging.md)
- [Indexed geometry/POM-off measurements](agent/evidence/2026-09-18-indexed-geometry/README.md)
