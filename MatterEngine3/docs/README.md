# MatterEngine3 documentation

These are the concise, current engine references:

- **[Architecture](architecture.md)** — JS DSL through deterministic bake,
  artifacts, PartGraph composition, and known constraints.
- **[Rendering](rendering.md)** — per-frame Vulkan composition, raster and
  native ray-tracing lanes, GPU data layout, and performance characteristics.
- **[Local lighting](local-lighting.md)** — World JavaScript API, resolved
  CPU/GPU record packing, world-space light index, attenuation, and renderer
  publication/reload contract.
- **[Authoring](authoring.md)** — part schemas, worlds, shared libraries, and
  test coverage.
- **[Event system](event-system.md)** — typed notifications, commands, and
  observable editor models.
- **[Part Workbench](part-workbench.md)** — asset isolation, LOD inspection,
  and source-persisted authoring workflow.
- **[Vulkan RT GTEX bake](vulkan-rt-gtex-bake.md)** — current GPU tileset bake
  contract.
- **[Chart VT glossary](chart-vt-glossary.md)** — virtual-texture terms used by
  the renderer and authoring docs.

Active but unfinished designs:

- **[Settle tick optimizer](settle-tick-optimizer.md)** — engine tooling is
  present; the interactive Settle Lab remains deferred.
- **[Volumetric emission sampling](volumetric-emission-sampling.md)** —
  proposed emissive-mesh lighting of fog; not implemented.
- **[Render eligibility and raster-only water](../../docs/superpowers/specs/2026-08-28-render-eligibility-and-document-lifecycle-design.md)**
  — approved engine direction awaiting implementation.

The approved renderer direction is mixed eligibility: parts and instances may
opt out of ray tracing while staying in raster rendering. Water is the first
raster-only default. Until that implementation lands, current source may still
contain the animated-water BLAS path described by the gap audit.

Completed milestone documents moved to `../../docs/completed/`; superseded
engine notes moved to `../../docs/deprecated/`. Their manifests preserve the
original paths.
