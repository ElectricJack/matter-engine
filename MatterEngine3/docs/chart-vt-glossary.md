# Chart-Space Virtual Texturing — Glossary

Reference vocabulary for the chart-VT system, as built (2026-07-30). Design:
`docs/superpowers/specs/2026-07-29-chart-virtual-texturing-design.md`; plan:
`docs/superpowers/plans/2026-07-29-chart-virtual-texturing-plan.md`. Companion
PBR spec: `docs/superpowers/specs/2026-07-29-rt-pbr-ice-snow-design.md`.

Terms are grouped by where each concept lives in the pipeline. Numbers are the
current defaults, most of them env-tunable (see `vt_residency.cpp` /
`chart_atlas.h` for the authoritative constants).

## Content & identity

- **Part** — a procedural object built by a JS module's `build()` (a rock, a
  terrain sector, a tree). The unit of baking.
- **Variant** — a part resolved with specific parameters, identified by its
  `resolved_hash` (module source + params + child hashes). **The unit of
  texture identity**: all placements of the same variant share one set of
  textures.
- **Instance** — one placement of a variant in the world. Thousands of
  instances can share one variant's pages; per-instance texturing does not
  exist (deferred by design; the page key is extensible to `stable_id`).
- **LOD rung** — one of a variant's precomputed detail levels (full-res mesh
  down through decimated meshes). Today each rung has its own texture space;
  the planned *UV family* work makes the decimation chain share one.
- **UV family** *(designed, not yet built)* — a group of rungs sharing one
  parameterization: the decimation chain inherits rung 0's UVs (UV-preserving
  decimation with chart-boundary edge locks); a rung produced by a different
  algorithm (retopo, heightfield LOD) declares its own family with its own
  charts. Family membership is inferred from the producing algorithm, never
  authored. Cross-family transitions accept a texture-space change, mitigated
  by deterministic phase anchoring of chart plane origins.

## Parameterization (bake time)

- **Chart** — a connected group of triangles whose normals fit inside a
  **normal cone** (default 45° half-angle), flattened onto a single best-fit
  plane. Charts partition the mesh's *triangles*, so every surface owns unique
  texels — this is what makes overhangs, tunnels, and stacked floors work
  where world-space projection cannot (projection is not injective).
- **Chart plane / basis** — the chart's projection frame: an origin plus
  tangent (atlas-U direction) and bitangent (atlas-V direction). Part-local
  positions project onto this plane to become texture coordinates.
  "Triplanar generalized to unlimited planes."
- **Virtual atlas** — the variant's private 2D texture address space (up to
  8192² texels at the finest mip) into which all its charts are shelf-packed.
  It has no backing memory of its own; it is an *address space*, realized on
  demand through pages.
- **Chart UV** — the per-vertex coordinate, normalized [0,1] over the virtual
  atlas, stored in the render vertex's `surface.xy` (and `TriEx.uv0/1/2` at
  bake). By runtime, charts are invisible — shaders see a flat UV.
- **Gutter** — 4 texels of padding around each chart's rect, filled with
  dilated (nearest-triangle) content so filtering and BC blocks never bleed
  between charts.

## Texture space (pages)

- **Page** — the fixed-size unit of texture memory and residency: **128²
  payload texels at every mip** (a finer mip means the same texel count over
  less surface). The atom that gets requested, filled, cached, and evicted.
  Uniform size is what makes the memory model fragmentation-free.
- **Page border** — 4 extra texels on each side of the payload (136² stored)
  duplicating neighbor content so bilinear filtering stays inside the page.
- **Virtual mip** — the atlas's mip pyramid at page granularity: each level
  halves the atlas, quartering its page count. Chart packing is page-aligned
  only at the finest mip; coarse pages span many charts (gutter dilation
  covers the space between).
- **Tail** — the one *pinned, always-resident* page per registered variant
  holding the coarsest mips (atlas dims ≤ 64). Every unresolved lookup falls
  back to it — the reason sampling is never-fault.

## Runtime structures

- **Physical pool** — the real GPU memory: four parallel image arrays
  (albedo BC7, normal BC5, ORM BC7, aux RGBA8) divided into a uniform grid of
  136² **slots** (16×16 per array layer). Default 8192 slots ≈ 1 GB
  (`MATTER_VT_POOL_PAGES`). Any page may occupy any slot.
- **Indirection** — the per-variant lookup table (the "page table"): for each
  virtual page at each mip, one 4-byte `R16G16_UINT` entry
  **(physical slot, mapped mip)**. One `texelFetch`, then pure arithmetic
  converts slot → pool rect. Entries always point at the best *resident*
  coverage (worst case the tail), rebuilt coarsest-first on the CPU — no
  fallback-walk loop in the shader. Implemented as one 64×128 image-array
  layer per (variant, rung); the per-format device cap on array layers
  (2048 on NVIDIA for this format) is therefore the cap on simultaneously
  registered variant-rungs. Escape hatches, in order: UV families (~3× fewer
  entries), then multi-variant-per-layer packing or a buffer-based
  indirection (no layer limit).
- **vt_slot** — the per-draw handle (written by `cull.comp` into the draw
  transform, mirrored into `GpuRtPartRecord` for RT) selecting which
  indirection layer the shader samples. `0` = no VT → legacy path.
- **Feedback** — the ⅛-resolution buffer where the G-buffer pass records
  which pages it *wanted* (variant, page, mip); read back async,
  deduplicated, prioritized by mip distance, drained as bounded page fills
  (`MATTER_VT_FILLS_PER_FRAME`, default 8). Rays never write feedback.
- **Working set / demand registration** — variants register with the
  residency layer only when the renderer's demand pass (mirroring cull's LOD
  selection) actually wants them, then **linger** (`MATTER_VT_LINGER_FRAMES`,
  240) after last use before release, with LRU evict-to-admit. Replaced the
  eager whole-world registration that could not fit the 2048-layer cap.

## Producing texels

- **Filler** — anything that writes pages, behind one seam (`VtPageFiller`,
  `vt_types.h`), with per-request success reporting (map-or-rollback: a
  failed fill never leaves a mapped-but-unwritten page). Two exist: a
  flat-color stub (debug/fallback) and the compositor.
- **Compositor (tier-1)** — the real filler (`vt_compositor.*`,
  `vt_composite.comp`): per page texel it reconstructs the surface point from
  the chart table, evaluates material weights, samples the detail tilesets
  **triplanar in part-local space** at the texel's footprint LOD,
  height-blends the top-2 materials, re-projects normals into the decoder's
  frame, and GPU-encodes to BC7/BC5 (`vt_bc_encode.comp`, deterministic
  fast-tier mode 6). This is where all authored complexity is paid — once per
  page, not per frame.
- **Tier-2 enrichment** — a background pass (`vt_enrich_ao.comp`) that
  revisits filled pages and bakes **contact-scale hemisphere AO** (32–64
  `rayQueryEXT` rays against the variant's own part-local BLAS, cap
  ≈ min(4 texels, 0.5 m), mip fade, 0.15 floor) into the ORM occlusion
  channel via BC7 read-modify-write. Self-occlusion only — world-context
  occlusion is the live RT lighting's job. Skipped when RT is unavailable.
- **Detail tileset (`.gtex`)** — *not* part of VT: the Wang-tiled
  micro-detail atlases baked by ray-tracing authored miniature scenes
  (ForestFloor litter, etc.) — 6 channels including height and horizon maps,
  BC-compressed, 8 runtime slots allocated LRU by content hash. The
  compositor's *source material* at page-bake time, and still sampled live in
  the near band.
- **Near band / handoff** — inside the POM fade distance (~50 m), the
  G-buffer samples the detail tileset live and modulates the VT base with a
  mean-preserving ratio (VT carries low/mid frequencies, detail + POM carry
  micro). Beyond it: pure VT. Secondary rays always use pure VT (the ratio
  term is mean-preserving, so this is unbiased).

## Authoring inputs

- **`defineMaterial`** — JS declaration of a material (full `MaterialDef`
  spec + optional `detail:` tileset module + `detailDensity`). Returns the
  integer **handle** (≥ 30, deterministic per world) that the classifier
  references. Declaring a detail scene is what schedules its `.gtex` bake and
  slot binding — authors never touch slots.
- **`surfaces()` tape** — the world's classifier: recorded ops (slope,
  normalY, altitude, world-space noise/ridge, fieldSlope, curvature,
  arithmetic) compiled native alongside the terrain field, producing
  per-vertex **weights** over declared materials. World-space inputs are
  legal only on world-anchored (single-instance) variants — terrain sectors —
  with a warn-once fallback elsewhere. The tape hash rides the page content
  key, so tape edits invalidate and re-fill.
- **Aux channel** — the page channel carrying (dominant material id,
  secondary id, blend) so the near-band shader knows which detail tileset to
  layer on each texel, and so the legacy fallback can bake the per-vertex
  argmax material.
- **Height blend** — material transitions weighted by the tilesets' height
  channels (grass fills between stones rather than alpha-crossfading over
  them). Exact passthrough at weight 0/1.

## Bookkeeping

- **Content key / invalidation** — pages and tails are pure functions of
  (variant content, chart table, tape hash, material + tileset content
  hashes, engine bake version). Editing any input invalidates and re-fills
  (in place for pinned tails); nothing stale survives an edit, nothing
  re-bakes without one.
- **Fail-closed / legacy path** — any VT failure (no charts, budget refusal,
  device limits) drops that surface to the pre-VT pipeline — which now bakes
  the tape's per-vertex dominant material into the legacy vertex stream, so
  fallback geometry renders *classified-but-flat*, never pre-tape uniform
  dirt.
- **Ray cone** — the RT path's texture-footprint tracker: an angle spawned
  from the traced pixel's angular size, widened per bounce (diffuse +0.35
  rad, specular +2r²), converted at each hit to a world footprint that
  selects VT and tileset mips — so ray hits sample the same mips screen
  derivatives select in raster.
