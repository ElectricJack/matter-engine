# Chart-Space Virtual Texturing — Implementation Plan

**Status:** Active (2026-07-29)
**Spec:** `docs/superpowers/specs/2026-07-29-chart-virtual-texturing-design.md`

This plan sequences the spec's six phases into work packages (WPs) sized for
parallel implementation. Interface contracts that cross WP boundaries are
**pinned here** so concurrent WPs never negotiate them ad hoc. Each WP is
independently buildable/testable and leaves the tree green; WPs do not
commit — integration commits happen at wave boundaries after verification.

## Deviations from the spec (decided here)

1. **Resident tails are generated at part load, not part bake.** The spec
   stored compositor-filled tails in the `.part` sidecar; the compositor is a
   GPU compute pass and the Baker runs headless/CPU (worker threads, tests).
   Instead the sidecar stores charts + atlas layout only, and the VT runtime
   dispatches the compositor over the tail mips when a part loads (bounded:
   ≤ 64² × channels per variant rung). Consequence: headless builds never
   need tails (nothing samples VT headlessly), and the Baker stays CPU-pure.
2. **WP-E lands with a stub page filler** (flat material albedo / geometric
   normal / default ORM) so the entire residency runtime is testable before
   the real compositor (WP-D) swaps in behind the same seam.

## Dependency graph

```
WAVE 1 (parallel)
  WP-A  Charting in the part bake            (spec Phase 1)   [fable]
  WP-B  BC foundation + slice compression
        + gtex child-hash fix + 8 slots      (spec Phase 3b)  [opus]
  WP-C  defineMaterial + automated bakes
        + LRU slot allocator                 (spec Phase 3a)  [opus]
        │
WAVE 2 (parallel, after WP-A; WP-D also needs WP-B's encoder)
  WP-E  VT residency runtime + stub filler
        + gbuffer sampling + near-band       (spec Phase 2a)  [opus]
  WP-D  Tier-1 compositor + GPU BC encode    (spec Phase 2b)  [fable]
        │  (WP-D replaces WP-E's stub at the VtPageFiller seam)
WAVE 3 (parallel, after WP-D+WP-E integrate; WP-F also needs WP-C)
  WP-F  surfaces() tape → compositor weights (spec Phase 4)   [fable]
  WP-G  RT sampling of VT + ray cones        (spec Phase 5)   [opus]
        │
WAVE 4
  WP-H  Tier-2 hemisphere enrichment         (spec Phase 6)   [opus]
```

## Pinned contracts

### C1 — Chart sidecar (producer WP-A; consumers WP-D, WP-E, WP-G)

New header `MatterEngine3/src/render/chart_atlas.h` (engine-wide, no GPU
types). Logical schema, serialized as a new versioned section of the `.part`
artifact (WP-A picks the concrete encoding to match existing artifact
conventions; absence of the section ⇒ `charts = 0` ⇒ legacy path):

```c++
struct ChartEntry {
  float origin[3];        // part-local plane origin
  float tangent[3];       // T — atlas U direction, unit
  float bitangent[3];     // B — atlas V direction, unit
  uint32_t rect_x, rect_y, rect_w, rect_h;   // texels in the virtual atlas (finest mip)
  float texels_per_meter;
  uint32_t first_tri, tri_count;             // into the rung's chart-grouped triangle order
};
struct ChartAtlasRung {
  uint32_t atlas_w, atlas_h;                 // finest-mip virtual dims, ≤ 8192
  std::vector<ChartEntry> charts;
  std::vector<uint32_t>   tri_order;         // triangle indices grouped by chart
};
```

- Chart UVs are written into `TriEx.uv0/1/2` and flow to the render vertex
  `surface.xy` **normalized to [0,1] over the atlas** (not texels).
- Constants (same header): `kVtPagePayload = 128`, `kVtPageBorder = 4`,
  `kVtMaxAtlasDim = 8192`, `kVtTailDim = 64`, `kChartGutterTexels = 4`,
  `kChartNormalConeDeg = 45.0f` (default), `kChartAtlasVersion = 1`.
- Page alignment: every chart rect is padded so no finest-mip page spans two
  charts (rects start on `kVtPagePayload` multiples).

### C2 — VT runtime seam (owner WP-E; WP-D plugs in)

New files `MatterEngine3/src/render/vt_residency.{h,cpp}` (WP-E) and
`MatterEngine3/src/render/vt_compositor.{h,cpp}` (WP-D).

```c++
// vt_types.h  (WP-E creates, both include)
struct VtFillRequest {
  uint64_t variant_hash; uint16_t rung; uint16_t mip;
  uint16_t page_x, page_y;          // page coords at `mip`
  // resolved by the residency layer before the filler runs:
  uint32_t physical_slot;           // destination page slot in the pool
  const ChartAtlasRung* atlas;      // borrowed
  /* + whatever mesh/material handles the filler needs; extend here, both sides recompile */
};
class VtPageFiller {                 // seam: stub (WP-E) vs real compositor (WP-D)
 public:
  virtual void fill(VkCommandBuffer cmd, std::span<const VtFillRequest> batch) = 0;
};
```

- Physical pool: one image array per channel — albedo BC7, normal BC5,
  ORM BC7, aux `R8G8B8A8_UNORM`; page slot = 136² texel region (payload +
  border). Pool page budget: `MATTER_VT_POOL_PAGES` env / config, default 8192.
- Indirection: per (variant, rung), `R16G16_UINT` mip-chained image;
  entry = (physical slot, mapped mip). Sampling helper lives in a new
  `shaders_vk/vt_common.glsl` (WP-E authors; WP-G reuses verbatim).
- Feedback: ⅛-res target written by the G-buffer pass
  (variant-slot id, page, mip packed to RGBA16_UINT), async readback,
  double-buffered.

### C3 — Material authoring (owner WP-C; consumers WP-D/WP-F)

- JS: `defineMaterial(name, spec) → handle` (world scripts + shared-lib),
  where `spec` fields mirror `MaterialDef` JSON-style, plus
  `detail: 'ModuleName'`, `detailDensity`.
- C: `MaterialRegistryDefineDynamic(const MaterialDef*, const char* name)`
  → index ≥ 30; `MaterialRegistryResetDynamic()` on world (re)connect.
  Existing pack path (`MaterialRegistryPackRtForGPU`) unchanged — dynamic
  entries ride the same table.
- Slot allocator: LRU over `kMaxTilesetSlots = 8` keyed by `.gtex`
  content hash; `run_tileset_deferred` generalized to iterate declared
  materials-with-detail; `tileset: true` root remains an alias for
  material 16.

### C4 — surfaces() tape (owner WP-F)

- World instance method `surfaces(s)`; helpers record into
  `globalThis.__surface_ops` (op-line tape, same discipline as
  `__world_ops`); host compiles alongside the terrain field.
- Native eval signature (consumed by the compositor via a small LUT bake or
  per-texel CPU precompute — WP-F decides, documented in code):
  `surface_weights(pos_local, normal, world_ctx_or_null) → top2 {mat, w}`.
- World-input misuse on multi-instance variants: diagnosed once, world
  inputs evaluate to constants (deterministic).

## Work packages

### WP-A — Charting in the part bake  [fable]

**Files:** `libs/MeshChartingLib/*`, `MatterEngine3/src/render/chart_atlas.h`
(new), the Baker/artifact path (`part_graph`, part serialization,
`lod_bake.cpp` per-rung), `tri.h` consumers that copy UVs to the vertex
stream, `MatterEngine3/tests/` (new `chart_atlas_tests.cpp`).

1. MeshChartingLib: 32-bit index overloads (keep the 16-bit API); page-aligned
   shelf packing (`kVtPagePayload` grid) with `kChartGutterTexels` gutters;
   distortion metric (max/min singular value of the projection per chart).
2. Chart build step in the part bake, per LOD rung, after
   meshing/simplification: segment (normal cone) → per-chart `plane_basis`
   projection at the density policy (props 16 t/m; terrain sectors 16 t/m
   rung 0 halving per rung; clamp so atlas ≤ `kVtMaxAtlasDim`) → pack →
   write `ChartAtlasRung` + UVs into `TriEx` (splitting shared vertices
   across charts) → serialize sidecar section.
3. Vertex stream: chart UV lands in `surface.xy` where `TriEx.uv` flows
   today for non-terrain; verify terrain triangles (currently zero UVs) now
   carry chart UVs. `ao_valid` semantics untouched.
4. Headless metrics gate (`run-chart-atlas` test target): coverage 100%, no
   rect overlap, distortion < 2.5, gutters valid, vertex-split inflation
   < 15%, determinism (same mesh ⇒ byte-identical table), >64k-vertex mesh
   correctness.

**Done when** the metrics gate passes on a StreamMountain sector mesh and a
prop mesh (Rock); a full engine build + existing headless suites stay green;
nothing renders differently.

### WP-B — BC foundation, slice compression, hash fix, 8 slots  [opus]

**Files:** `third_party/` (vendor `bc7enc`/`rgbcx`-class single-file
encoder), `MatterEngine3/src/render/tileset_slicer.h/.cpp`,
`vk_scene_renderer.cpp` gtex load path (`:3203-3520` region),
`local_provider.cpp` (`:887-902` hash), tests.

1. Vendor a CPU BC7/BC5/BC4 encoder under `third_party/` (README provenance
   note, license file). Wrap in `MatterEngine3/src/render/bc_encode.{h,cpp}`
   (`encode_bc7/bc5/bc4(src, w, h) → blocks`) — WP-D's GPU path is separate;
   this CPU path serves slices and tails-fallback.
2. `tileset_slicer`: mip then compress per layer; upload formats become
   `BC7_UNORM` (albedo, ORM), `BC5_UNORM` (normal), `BC4_UNORM` (height);
   horizon channels stay uncompressed (quarter-res, cheap). Preserve the
   mip-0 edge-strip byte-equality invariant (4-texel-aligned strips + BC
   block alignment ⇒ assert exact, new test alongside
   `tileset_slicer_tests.cpp`).
3. Slot count: descriptor array 24 → 48 (`slot*6+channel`), all bounds
   checks via one constant (`kMaxTilesetSlots = 8` — placed in
   `tileset_gtex.h`, consumed by renderer + registry).
4. `.gtex` cache key: fold the sorted child `resolved_hash` list (already
   computed for the settle key) into `gtex_content_hash`.

**Done when** Vulkan smoke modes are green, a gtex loads compressed
(validation-silent), slice tests + the new BC edge test pass, VRAM per slot
drops ~6× (log line), and a child-module edit re-bakes the atlas (manual
check via hash change in a unit test).

### WP-C — defineMaterial + automated bakes + slot allocator  [opus]

**Files:** `script_host.cpp`, `part_base.js.h`/`world_base.js.h`,
`world_definition_loader.cpp`, `local_provider.cpp` (`run_tileset_deferred`),
`material_registry.{h,c}`, tests (`run-world-definition` suite extension).

Implements contract C3. Order of work: registry dynamic entries → JS binding
→ loader plumbing (materials declared in world scripts execute before roots
resolve) → generalized deferred bake loop → LRU allocator (evict = unbind
slot, material falls back to scalar albedo; warn). Headless tests: dynamic
define/reset round-trip, handle stability, allocator eviction order,
`tileset: true` alias equivalence. GPU-path smoke: existing worlds
(no `defineMaterial`) byte-identical.

**Done when** a test world declares two materials with detail scenes and both
bake + bind automatically; existing worlds unchanged; headless suites green.

### WP-E — VT residency runtime + stub filler + sampling  [opus]

**Files:** new `vt_types.h`, `vt_residency.{h,cpp}`, `vt_stub_filler.cpp`,
`shaders_vk/vt_common.glsl`, `vt_feedback` additions to the G-buffer pass,
`gbuffer.frag`, `vk_scene_renderer.{h,cpp}` integration, `part_store`
(tail/indirection lifetime), Makefile SPV embed lists (**both**
`MatterEngine3/Makefile` and `MatterViewer/Makefile` — see appendix).

1. Pool + indirection + tails (tail = filler run over mips ≤ `kVtTailDim`
   at load), per contract C2.
2. Feedback pass + readback + dedup/priority queue + bounded fills/frame +
   LRU eviction.
3. `gbuffer.frag`: per-part "has VT" flag → sample via `vt_common.glsl`
   (indirection walk → page or tail); near-band handoff: inside the POM fade
   distance, detail Wang tileset modulates the VT base via the
   mean-preserving ratio form (spec Phase 2); beyond it, pure VT. POM
   unchanged.
4. Unit tests: addressing resolution (mapped/unmapped/tail), border
   correctness, eviction; GPU test: a synthetic 2-chart part renders with
   stub-filled pages, no seam at the chart boundary beyond filtering epsilon.

**Done when** StreamMountain renders end-to-end through VT with the stub
filler (flat colors — parity NOT expected yet), residency stable on a flight
path, pool stats on the FIFO `stats` channel, legacy path intact for
chartless parts.

### WP-D — Tier-1 compositor + GPU BC encode  [fable]

**Files:** new `vt_compositor.{h,cpp}`, new compute shaders
`shaders_vk/vt_composite.comp`, `shaders_vk/vt_bc_encode.comp` (BC7/BC5 fast
GPU encode), shared Wang includes (`tileset_common.glsl` refactored so cell
resolution is includable from compute), Makefile embed lists.

1. Analytic chart rasterization: per page, iterate the chart's triangle
   range (`tri_order`), compute per-texel part-local position + interpolated
   normal (barycentric in plane space); texels outside all triangles take
   nearest-triangle dilation (gutter fill).
2. Material weights: Phase-2 stub = per-triangle `materialId` (single
   material per texel); the C4 seam is a function pointer/LUT so WP-F slots
   in without touching this file's structure.
3. Triplanar Wang sampling along part-local axes weighted `|n|^k` (k = 4),
   height-blend across the (future) top-2 materials — single-material
   passthrough now, but the blend code path exists and is tested with a
   synthetic 2-material LUT.
4. Normal output re-projected into chart tangent space (T, B, N).
5. GPU BC encode (fast-tier BC7 mode-6-dominant + BC5) → copy into pool
   slots. Deterministic given identical inputs (no time/random).
6. Replace WP-E's stub at the `VtPageFiller` seam.
7. Golden tests (GPU): synthetic tape + synthetic tilesets ⇒ byte-stable
   pages; triplanar weight correctness on axis-aligned + 45° fixtures;
   height-blend identities (w=0/1 passthrough).

**Done when** StreamMountain through VT reaches **mid/far-field parity** with
pre-VT captures (single dirt material) and **cliff stretching is visibly
fixed** (the Phase-2 exit criteria); goldens pass; page fill < 0.5 ms/page
tier-1 on the 4090 (logged).

### WP-F — surfaces() tape  [fable]

**Files:** `world_base.js.h`, `script_host.cpp` (tape readback),
`terrain_field.*` (op compile reuse), `vt_compositor` weight seam,
`WorldSector.js` untouched (band table stays), tests.

Implements C4: record → compile → evaluate; world-anchored detection
(variant instance count == 1); misuse diagnostic; aux channel gets top-2
(mat ids + blend); compositor height-blends per spec. Test world
(`worlds/ChartVtProof.js` or similar) with a 3-material slope/altitude tape;
goldens for the weight LUT; tape hash folded into the page/tail content key.

**Done when** the 3-material test world shows height-blended transitions in
VT pages at all distances; page-key invalidation on tape edit verified.

### WP-G — RT sampling of VT + ray cones  [opus]

**Files:** `rt_surface_common.glsl`, `rt_lighting.rgen`/`rt_shadow.rgen`
payload, `vt_common.glsl` reuse, `vk_scene_renderer.cpp`
(`GpuRtPartRecord` extension: indirection + tail handles), tests.

Chart UV interpolation at hit (vertex fetch exists; `surface.xy` is in the
72-byte stride — keep the stride guard); indirection walk with tail
fallback (never fault, never wait); cone angle in the payload (pixel-footprint
spawn, widen by roughness at bounces) → mip; delete
`RT_TILESET_CONE_SPREAD`. RT-side page requests: **not in this WP** (spec
optional, off). New GPU consistency test: G-buffer vs secondary-hit shading
of the same surface within filtering epsilon; cone-mip monotonicity test.

**Done when** consistency + monotonicity tests pass; reflection/GI of distant
terrain visibly stops shimmering at wrong mips; no RT frame-time regression
(stats compare on the flight path).

### WP-H — Tier-2 hemisphere enrichment  [opus]

**Files:** new `shaders_vk/vt_enrich_ao.comp` (rayQuery, pattern-match
`tileset_bake_ao.comp`), `vt_residency` queue extension (enrichment state per
page), deferred-bake scheduling in the provider tail.

32–64 cosine-hemisphere rays per texel against the variant's own BLAS,
distance cap 4× texel size at that mip; multiply into ORM occlusion in
place; tails enriched at load (small); page key carries tier state; fade-in
over ~250 ms at apply. Optional horizon term deferred unless trivial.

**Done when** an overhang/tunnel fixture shows baked contact occlusion vs
tier-1 captures; background enrichment < 10% GPU on the flight path; pages
re-enrich after eviction/refill.

## Appendix — build & environment (for every WP)

- MSYS2 UCRT64 GCC; always pass
  `TMP="C:/Users/webde/AppData/Local/Temp" TEMP="C:/Users/webde/AppData/Local/Temp"`
  to make; `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`.
  Bash tool calls that compile need `dangerouslyDisableSandbox: true`.
- Headless tests: `make -C MatterEngine3/tests run-<suite> GRAPHICS=GRAPHICS_API_OPENGL_43`
  (+ TEMP vars). Some GL-linking test targets fail at link on Windows —
  compilation is the gate there.
- **New Vulkan shaders must be added to BOTH SPV embed lists**
  (`MatterEngine3/Makefile` and `MatterViewer/Makefile`) or features die
  silently (embedded_spirv.h regeneration).
- Worktree junctions: run `bash setup-worktree.sh` from repo root if
  `MatterEngine3/shaders` is a plain file.
- Do not commit; the orchestrator commits at wave boundaries.
- GL raster path (`libs/MatterSurfaceLib/shaders/*`) is frozen — no
  backports, no edits.
