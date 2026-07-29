# Chart-Space Virtual Texturing (Per-Variant Megatexture) — Design

**Date:** 2026-07-29
**Status:** Draft for review

## Summary

Give every surface in the engine — terrain sectors, buildings, tunnels, props,
overhangs — a unique, cached, compressed texture space, produced on demand by
the engine's own bake machinery rather than streamed from authored content.
Six phases: **(1)** automatic UV charting folded into the part bake,
**(2)** the virtual-texture runtime (page table, physical pool, resident mip
tails, feedback) with a tier-1 page compositor that reproduces today's Wang
tileset look, **(3)** JS material authoring (`defineMaterial`) with automated
`.gtex` bakes and content-hashed slot allocation, **(4)** the `surfaces()`
classifier tape compiled native and driving multi-material page composition,
**(5)** RT hit-shader sampling of the same pages with ray-cone mip selection,
**(6)** tier-2 page enrichment — per-texel hemisphere AO/horizon baked with
the existing HW-RT machinery, applied progressively.

Three decisions anchor the design:

- **Charts, not projection, are the storage parameterization.** World-space
  projection (planar or triplanar) is not injective: tunnel ceilings, stacked
  floors, and overhangs alias to the same projected texels. Normal-cone charts
  partition *triangles*, not space, so every surface owns its texels. Each
  chart projects onto its own best-fit plane — triplanar generalized to
  unlimited planes, with the cone threshold guaranteeing the projection never
  degenerates. Triplanar *sampling* of source detail tilesets still happens,
  but at page-bake time inside the compositor, where position and normal are
  known per texel. Runtime samples one 2D UV.
- **Pages are keyed per part variant (`resolved_hash`), not per instance.**
  All placements of the same resolved part share pages. Texels are functions
  of the part, so identical parts are free — this is what makes megatexture
  affordable in an instanced procedural engine. Per-instance uniqueness
  (keyed by `stable_id`) is deferred future work, opt-in for hero objects.
- **Runtime cost decouples from authored complexity.** Classifier evaluation,
  N-material height blending, triplanar projection, and macro breakup run in
  the compositor once per page. Frame cost is one indirection fetch plus one
  or two array samples, identical for the raster G-buffer and RT hit shaders.

The Wang `.gtex` tilesets are not replaced: they remain the *source* of
surface detail, sampled by the compositor at page-bake time and live in the
near band (≲ POM distance) where page density cannot carry full detail.

## Background

Current state, and why it cannot stretch to the goal:

- One detail tileset, planar world-XZ projection, hardwired to material 16 at
  `local_provider.cpp:834`; 4 slots total; StreamMountain collapses all four
  terrain buckets to DIRT (`WorldSector.js:119-124`), so a 450–650 m alpine
  range shades with a 2 m forest-floor litter atlas — ~1280 repeats per axis
  across the 2560 m disc with no macro breakup (macro slot is plumbed but has
  zero shader call sites).
- The XZ projection is degenerate on steep faces (`tileset_rotate_normal`
  bails near ±X); the mountain world is mostly steep faces. The planned
  world-XZ macro clipmap inherits the same weakness and additionally gives a
  300 m near-vertical face almost no unique texels (its XZ footprint ≈ 0).
- RT texture LOD is a fixed fudge (`RT_TILESET_CONE_SPREAD = 0.01`,
  `rt_surface_common.glsl:106`); atlases upload uncompressed (~100 MB VRAM
  per 4096² slot); the `.gtex` cache key omits child module sources
  (`local_provider.cpp:887-898`), so appearance-only child edits can serve a
  stale atlas.
- Native `terrain_field.cpp:458` already classifies grass/dirt/rock/snow by
  slope/altitude and the result is deliberately discarded; there is no
  authored path to it.

Readiness worth naming, because it shapes the phases:

- `mesh_charting` (libs/MeshChartingLib) already provides exact-weld
  adjacency, normal-cone chart segmentation, per-chart plane bases, and
  shelf packing — GL-free and unit-tested. It needs 32-bit indices and
  page-aligned packing, not invention.
- `TriEx` carries unused `uv0/1/2` (`tri.h:26-31`); the 72-byte render vertex
  reserves `surface.xy` for UVs (`vk_scene_renderer.h:129-136`); the RT hit
  path already does manual vertex fetch + barycentric interpolation
  (`rt_surface_common.glsl:190-235`). Chart UVs need no vertex-format change
  and no new RT plumbing shape.
- The `.gtex` bake is a deterministic GPU ray tracer pointed at geometry;
  tier-2 enrichment is the same machinery re-aimed (per-texel hemisphere
  instead of ortho-down), and the deferred-bake pipeline that schedules
  tileset bakes after `BakeFinished` is the natural host for background page
  work.

## Goals

- Every rendered part variant owns a charted virtual atlas; raster and RT
  sample identical page data at every distance and bounce.
- Steep and overhung surfaces are first-class: no projection degeneracy
  anywhere, by construction.
- Authoring contract: authors write build algorithms, `defineMaterial`
  declarations, and a `surfaces()` tape. Charts, pages, slots, density,
  residency, and compression are engine lowering, invisible unless a
  diagnostic fires.
- Memory: BC-compressed pages, bounded physical pool (budgeted, configurable),
  small always-resident mip tails. Target: full StreamMountain disc textured
  within ~1 GB of VT VRAM, versus the current single-slot 100 MB that
  textures one material.
- Deterministic and cached: pages are pure functions of
  `(variant content, chart, mip, compositor inputs, bake version)`; a page is
  reproducible bit-exact and cacheable, in the spirit of the existing
  part/settle/gtex caches.
- Fail-closed: any VT failure degrades to the legacy tileset path, never a
  crash, never a black surface.

## Non-Goals

- Per-instance unique texels (id-style). Deferred; design keeps the page key
  extensible to `stable_id` for opt-in hero objects.
- A world-XZ clipmap far-field layer. Demoted to future work; per-variant
  mip tails are expected to carry the far field. Revisit only if tail memory
  across ~5,000 sectors measures heavy.
- Disk-persistent page cache. Pages regenerate from cached inputs; a disk
  cache is an optimization to add behind the same key if bake latency ever
  warrants it.
- Rewiring the mesher's material band table (`terrainVolume`) to the
  `surfaces()` tape. The tape's v1 consumer is the page compositor only;
  geometry material buckets keep the existing mechanism.
- The advanced-PBR workstream (rough/frosted transmission, secondary-ray
  transmissive tint, snow glints, subsurface tuning). Companion spec; it
  composes with this one (pages carry masks/weights, view-dependent lobes
  stay live) but shares no code.
- Any GL-path backport (frozen, as established 2026-07-21).
- Chart quality beyond normal-cone + planar projection (no LSCM/ABF
  parameterization). Accepted distortion is bounded by the cone angle;
  revisit only if metrics or images demand it.

## Architecture overview

```
part bake (Baker)                          runtime
─────────────────                          ───────
mesh per LOD rung                          draw / ray hit
  └─ chart segmentation (normal cone)        └─ vertex UV (surface.xy)
  └─ per-chart plane projection → UVs             └─ indirection fetch (variant,mip)
  └─ page-aligned shelf pack → virtual atlas           ├─ hit: physical page sample
  └─ UVs into TriEx / vertex stream                    └─ miss: resident tail sample
  └─ .part sidecar: chart table, atlas dims,                + feedback request
     resident tail texels
                                           page fault → compositor (tier 1, compute):
world surfaces() tape ────────────────────►  evaluate tape per texel (part-local frame,
defineMaterial table ─────────────────────►  world frame iff world-anchored variant),
.gtex detail tilesets ────────────────────►  triplanar-sample detail tilesets, height-
                                             blend materials, write BC pages to pool
                                           tier 2 (async, deferred-bake queue):
                                             hemisphere AO / horizon per texel via
                                             HW-RT rays, refine pages in place
```

**Coordinate frames — the sharing rule.** Pages are shared across instances,
so the compositor evaluates in **part-local space** by default. World-frame
inputs (altitude, world moisture, world position) are valid only for
**world-anchored variants** — variants referenced by exactly one instance.
Terrain sectors qualify automatically (each sector's params encode its world
placement; sector variants are transient and unique by design). A `surfaces()`
tape that reads world inputs for a multiply-instanced variant is a diagnosed
authoring error (warn once, world inputs evaluate to the instance-independent
fallback). World-dependent variation on shared props (snow dusting on every
rock above the snow line) is a *runtime overlay* concern, out of scope here
and native to the companion PBR spec.

## Phase 1 — Charting in the part bake

Runs inside `Baker::bake` after meshing/simplification, per LOD rung (rung
meshes differ, so charts and UVs are per-rung; this extends the existing
per-rung attribute story in `bake_terrain_lods`).

- **MeshChartingLib upgrades:** 32-bit index overloads (sector meshes exceed
  64k vertices); chart growth unchanged (normal-cone, default 45°, per-call
  override); packing becomes **page-aligned**: each chart's rect is padded to
  the page grid at the finest mip so no fine-mip page spans two charts; a
  4-texel gutter per chart edge (dilated content, filled at page bake) keeps
  bilinear + BC blocks inside owned texels. Coarse mips where many charts
  share a page rely on gutter dilation only (standard VT mip-tail behavior).
- **UV emission:** per-chart plane projection (`plane_basis`) at the variant's
  chosen texel density; packed atlas UVs written to `TriEx.uv*` and through
  to the render vertex `surface.xy`. Vertices shared between charts are
  split (small vertex-count increase; measured and reported by the metrics
  below). `ao_valid` semantics in `surface.zw` are untouched.
- **Texel density:** per-variant `texels_per_meter`, default from part bounds
  (props ~16 t/m, terrain sectors 8–16 t/m at rung 0, halving per rung),
  clamped so no variant's virtual atlas exceeds 8192². Density is a
  lowering decision — authors can override via `defineMaterial`/part params
  but never must.
- **Sidecar:** the `.part` gains a chart table (chart → plane basis, rect,
  density), virtual atlas dimensions per rung, and the **resident tail**: the
  atlas mip chain from ≤ 64² downward, compositor-filled at bake time and
  stored compressed. Parts baked before this schema load with `charts = 0`
  and take the legacy path (fail-closed, no version break).
- **Headless metrics gate** (no renderer needed): chart coverage == 100% of
  triangles, zero chart overlap in the atlas, per-chart projection distortion
  (max singular-value ratio) under a threshold, gutter validity, vertex-split
  inflation < 15%, pack efficiency reported.

*The `.gtex` tileset bake is untouched by this phase.*

## Phase 2 — VT runtime core + tier-1 compositor

### Page addressing

- **Page:** 128² payload texels + 4-texel border (stored 136² pre-compression,
  compressed as 128²-payload BC blocks with border baked into neighboring
  fetch — borders exist so a page samples bilinearly without touching
  neighbors). Physical pool: one `VkImage` array per channel, page-slot
  granularity, budget default 8192 pages (~capped below).
- **Channels per page:**

  | channel | format | bytes/page |
  |---|---|---|
  | albedo | BC7 | 16 KB |
  | normal (RG) | BC5 | 16 KB |
  | ORM | BC7 | 16 KB |
  | aux: dominant material id + secondary id + blend | `R8G8B8A8_UNORM` (uncompressed v1) | 64 KB |

  ~112 KB/page → 8192 pages ≈ **0.9 GB**. Aux exists for the near-band detail
  handoff (below) and debug views; compressing it is a follow-up.
- **Indirection:** one small indirection texture per loaded variant per rung
  (virtual pages are ≤ 64² entries per mip at the 8192² cap — KBs), entries
  `(physical slot, mapped mip)`. Sample path: UV → indirection at desired
  mip → if unmapped, walk toward coarser entries → below the mapped range,
  sample the resident tail. **Every loaded variant always has valid texels**;
  pages only ever sharpen the image.
- **BC transcode:** compute-shader fast BC7/BC5 encode at page fill (quality
  tier "fast"; deterministic given deterministic input).

### Residency

- **Resident tails** upload with the part in PartStore (`LoadedPart` gains
  tail images + indirection handles per rung). Tail budget: ≤ 64² × 4
  channels ≈ 20 KB compressed per variant rung → ~100 MB across 5,000 sector
  variants. This *is* the far field.
- **Feedback:** a ⅛-resolution feedback pass (reusing the G-buffer's
  visibility; writes `(variant, page, mip)` requests) with async readback,
  double-buffered; dedup + priority (mip distance from mapped) on CPU;
  page-fill queue drains a bounded count per frame. Eviction: LRU by
  last-requested frame. No RT-side requests in this phase (Phase 5 note).
- **Invalidation:** page content key =
  `(resolved_hash, rung, chart-table hash, compositor input hash, kEngineBakeVersion)`
  where compositor inputs = surfaces-tape hash + referenced material/tileset
  content hashes. Any key change drops the variant's mapped pages; tails
  rebuild at next part bake (they carry the same key).

### Tier-1 compositor

Compute pass, one dispatch per requested page: rasterize the page's chart
region analytically (chart table gives plane basis + rect → per-texel
part-local position and interpolated normal from the mesh; a small per-chart
triangle list in the sidecar makes this a bounded loop, not a search), then
per texel:

1. Evaluate material weights. **Phase 2 stub:** single material — whatever
   the part's triangles carry (`materialId` per `TriEx`) — with today's
   ground-tileset binding. The tape lands in Phase 4.
2. Sample each weighted material's Wang detail tileset **triplanar**: three
   planar samples along the part-local axes of the texel's normal, weighted
   `|n|^k` — evaluated here, at bake time, never at runtime. Wang cell
   resolution uses the same hash machinery as `tileset_common.glsl` (shared
   include).
3. Height-blend materials by the tilesets' height channels (single material
   in Phase 2 → passthrough).
4. Write albedo/normal (re-projected into chart tangent space)/ORM/aux;
   BC-encode; copy into the physical slot; update indirection.

### G-buffer sampling + near-band handoff

`gbuffer.frag` branches on a per-part "has VT" flag (instance/part record
bit): sample VT via `surface.xy`. Near band (inside the existing POM
fade distance): the detail Wang tileset still samples live and **modulates**
the VT base with the mean-preserving ratio form from the 2026-07-21 macro
design (`detail / mean_albedo` ratio; `mean_rgb` in the slicer finally gets
its consumer) — VT plays the "macro" role in that formula, detail + POM ride
on top, aux tells the near band *which* detail tileset(s) to blend. Beyond
the fade: pure VT. POM is unchanged and detail-height-driven.

**Exit criteria:** StreamMountain renders through VT with mid/far-field
parity against current captures (same single material), **cliff stretching
visibly gone** (the per-chart projection is the fix), frame cost neutral or
better at the far field, pool residency stable under the flight path.

## Phase 3 — `defineMaterial` + automated tileset bakes

- **JS surface:** `defineMaterial(name, { albedo, roughness, metallic, ior,
  transmission, …, detail: 'AlpineRockDetail', detailDensity, macro: 'auto' })`
  available in world scripts and shared-lib. Returns a material handle usable
  anywhere `MAT.*` is. Builds a **per-world registry extension** appended
  after the 30 builtin entries; packed to the GPU by the existing
  `MaterialRegistryPackRtForGPU` path (schema unchanged — this is table
  content, not layout).
- **Automated detail bakes:** declaring `detail:` schedules the `.gtex` bake
  for that Tileset module through the existing deferred pipeline
  (`run_tileset_deferred` generalized: the hardcoded material 16 and the
  root-flag plumbing are replaced by "every declared material with a detail
  scene"). `tileset: true` world roots remain as a deprecated alias binding
  to material 16.
- **Slot allocation:** slots become an LRU pool keyed by `.gtex` content
  hash; count raised from 4 to 8 (descriptor array 24 → 48; all bounds
  checks via one constant). Affordable because:
- **BC compression of tileset slices:** `tileset_slicer` output transcodes to
  BC7/BC5/BC4 (albedo/normal/height) before upload — ~100 MB → ~17 MB per
  4096² slot. Mip generation order: mip then compress, per layer, preserving
  the edge-strip byte-equality invariant at mip 0 (test below).
- **Cache-key fix:** fold the sorted child `resolved_hash` list (already
  computed for the settle key) into `gtex_content_hash`, closing the
  stale-albedo gap for appearance-only child edits.

## Phase 4 — `surfaces()` tape

- **Authoring:** a world instance method in the `field()` idiom — helpers
  record an op tape (`__world_ops` sibling), host compiles it native. Inputs:
  part-local position + normal + curvature always; `height/slope/moisture/
  biome` world queries valid on world-anchored variants (see sharing rule;
  misuse diagnosed). Output: weights over declared materials (top-2 retained
  per texel, quantized into aux).
- **Compositor integration:** tier-1 step 1 evaluates the tape; steps 2–3
  blend the top-2 materials' tilesets by weight *and* height channel
  (height-based blending: grass fills between rocks, snow pools in
  concavities). Hard cap of 2 materials per texel keeps page bakes bounded;
  transitions get their softness from the weight field plus height blending,
  not from wide mixing.
- **`__terrain` retirement path:** `WorldSector.js` keeps its band-table
  mechanism for geometry buckets (non-goal), but the *appearance* now comes
  from the tape. The all-dirt collapse simply stops mattering for texturing.
- Tape hash joins the page/tail content key (invalidation for free).

**Exit criteria:** a test world with a 3-material tape (grass/rock/snow by
slope/altitude) shows correct, height-blended transitions in VT pages at all
distances; page bake time per 128² page measured and budgeted (< 0.5 ms
tier-1 target on the 4090).

## Phase 5 — RT sampling of VT

- Hit shaders interpolate chart UV from the fetched vertices (`surface.xy` is
  inside the existing 72-byte stride — the `vertex_stride != 72` guard
  holds). `GpuRtPartRecord` gains the variant's indirection + tail handles.
- Sample = same indirection walk as raster; below mapped range → tail.
  **Rays never fault and never wait.**
- **Ray cones replace the fudge:** a cone angle propagated in the payload
  (spawn from pixel footprint; widen by roughness at bounces) selects the
  mip; `RT_TILESET_CONE_SPREAD` is deleted. Most secondary hits land in
  tails by footprint — correct and cheap.
- Optional (measure first): hit shaders append page requests to a small
  buffer merged into the feedback queue, so persistent mirror-like
  reflections sharpen. Off by default.
- The RT tileset re-sampling path (`rt_tileset_sample`) remains only for the
  near-band detail term; GI/reflection consistency with the G-buffer becomes
  structural (same pages) instead of re-derived.

**Exit criteria:** G-buffer vs. secondary-hit shading of the same surface
agrees within filtering epsilon (new GPU test); reflection/GI noise on
distant terrain measurably drops (correct mips versus the old constant
spread); no RT frame-time regression.

## Phase 6 — Tier-2 enrichment

Chart-space analog of the `.gtex` AO/horizon channels, baked against real
geometry with the existing HW-RT machinery, scheduled on the deferred-bake
queue at low priority:

- Per-texel origin/normal from the chart table → 32–64 cosine-hemisphere
  occlusion rays against the **variant's own BLAS** (part-local, shareable
  across instances; world-context occlusion is the live RT lighting's job and
  explicitly not baked). Distance-capped like the gtex bake's strip cap, but
  at chart scale (cap ≈ 4× texel size at that mip) so results stay
  arrangement-independent.
- Optional horizon term (8 azimuths) for relief self-shadowing on
  high-relief parts; gated per material.
- Results multiply into the ORM occlusion channel of affected pages
  **in place**; indirection untouched; pages appear tier-1 fast and darken
  into their crevices asynchronously. Tails get tier-2 at part-bake time
  (they're small).
- Enrichment state rides the page key (a page is tier-1 or tier-2; eviction
  forgets it; re-fill re-runs tier-1 and re-queues tier-2).

**Exit criteria:** tunnel interiors / under-overhang texels show baked
contact occlusion absent in tier-1 captures; no visible pop (occlusion fades
in over ~250 ms); background bake keeps < 10% GPU on the flight path.

## Error handling

Fail-closed throughout, matching existing conventions:

- Charting failure (degenerate mesh, pack overflow at min density) → part
  bakes with `charts = 0`, legacy path, warn once per variant.
- Page-fill failure / pool exhaustion → indirection stays coarse (tail);
  never a hole. Pool pressure is a stat, not an error.
- Old `.part` without chart sidecar → legacy path (no version break; charts
  appear on next re-bake via the normal cache keys).
- `defineMaterial` referencing a missing detail module → material renders
  untextured scalar, warn once (mirrors current gtex-miss contract).
- `surfaces()` world-input misuse on shared variants → diagnosed, world
  inputs return fallback constants (deterministic, never instance-dependent).
- Device without BC7/BC5 (not a real target) → uncompressed pages, quarter
  pool page count, loud warning.

## Testing

- **Charting (headless, Phase 1):** metrics gate as specified; determinism
  (same mesh → byte-identical chart table/UVs); 32-bit index correctness on
  a > 64k-vertex sector mesh.
- **Addressing (unit):** virtual→physical resolution across mapped/unmapped/
  tail states; border correctness (bilinear at page edge touches no foreign
  texel); indirection update atomicity across a fill.
- **Compositor (golden):** synthetic tape + synthetic tilesets → byte-stable
  page output (bit-exact given fixed inputs — the determinism the caches
  rely on); triplanar weight correctness on axis-aligned and 45° fixtures;
  height-blend identities (weight 1/0 passthrough).
- **BC invariants (Phase 3):** tileset slice edge-strip byte-equality holds
  post-compression at mip 0 across color-matched layers (BC block alignment
  with the 4-texel gutter makes this exact, assert it).
- **RT consistency (Phase 5):** the G-buffer/secondary-hit agreement test;
  ray-cone mip monotonicity (farther hit → coarser mip).
- **Integration:** StreamMountain flight path — parity shots at Phase 2,
  cliff-fix shots (the before/after that motivates the design), 3-material
  tape shots at Phase 4, reflection shots at Phase 5, overhang/tunnel
  fixture at Phase 6. Residency/pool/bake-time stats logged via the FIFO
  stats channel; memory ceiling asserted (< 1.2 GB VT total on the flight
  path). Windows binary rebuilt each phase (`make windows`).

## Implementation phases

1. **Charting in the part bake:** MeshChartingLib 32-bit + page-aligned
   packing, UV emission into TriEx/vertex stream per rung, sidecar + tails,
   headless metrics gate. *Ship: every part variant carries charts + UVs;
   nothing renders differently yet.*
2. **VT runtime + tier-1:** pool/indirection/tails/feedback, BC transcode,
   single-material compositor with bake-time triplanar, gbuffer sampling +
   near-band handoff. *Ship: StreamMountain through VT — parity mid/far,
   cliffs fixed, memory bounded.*
3. **Material authoring:** `defineMaterial`, per-world registry extension,
   automated detail bakes, 8-slot LRU allocator, BC-compressed slices,
   gtex child-hash fix. *Ship: worlds declare materials with detail scenes;
   slots and bakes are automatic.*
4. **`surfaces()` tape:** recorded tape → native compile → compositor
   weights + height blending, aux channel live, misuse diagnostics.
   *Ship: multi-material mountains from one authored classifier.*
5. **RT on VT:** chart UV at hit, indirection/tail sampling, ray cones,
   consistency test. *Ship: bounces and reflections see the same mountain
   the camera does, at the right mip.*
6. **Tier-2 enrichment:** hemisphere AO (+ optional horizon) per page,
   progressive refinement on the deferred queue. *Ship: baked contact
   occlusion in tunnels and under overhangs, for free at runtime.*

## Future work (out of scope)

- Per-instance page keys (`stable_id`) for hero-object weathering; the page
  key is designed to extend.
- Runtime world-dependent overlays on shared variants (snow dusting by world
  altitude/normal) — companion PBR spec territory.
- Disk-persistent page cache behind the existing content key.
- World-XZ far-field clipmap, only if sector-tail memory measures heavy.
- Aux-channel compression; >2 materials per texel if transitions demand it.
- LSCM-quality parameterization; chart merging across LOD rungs.
- Macro-height → mesher displacement (carried over from 2026-07-21).
- The advanced-PBR companion spec: frosted/rough transmission, transmissive
  tint on secondary rays, stochastic snow glints, subsurface tuning.
