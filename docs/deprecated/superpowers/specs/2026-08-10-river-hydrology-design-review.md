# Adversarial Review: River Hydrology, Flow, and Water Rendering Design

**Date:** 2026-08-10
**Reviews:** `2026-08-10-river-hydrology-design.md`
**Method:** Every "existing code" claim was verified against the repo (MatterSurfaceLib
scaffold, MatterEngine3 DSL/terrain field, Vulkan renderer). Findings are ordered by
severity. File references are repo-relative.

**Verdict: the high-level architecture is sound, but the design is not implementable
as written in two places (density-field composition, renderer claims), and the solver
section is silent on the numerics that decide whether the whole approach is feasible.**

**Recommended disposition:** revise before approval. Must-fix items are findings 1, 3,
4, and 5. Findings 2 and 12 mainly re-cost the schedule rather than the architecture.

---

## Blocking findings

### 1. Composing the carve into the density field de-optimizes the entire world — and can't be expressed in the field DSL at all

The design says "Compile a `RiverCarveRuntime` and compose it with the existing
analytic 3D terrain density field" as if composition were free and local. It is
neither:

- There is exactly **one `FieldProgram` per world** (`MatterEngine3/src/matter_engine.cpp:3242`,
  handed to every sector bake), and the heightfield recognizer is **structural, not a
  y-dependence test**: `is_heightfield` requires the density op to be exactly
  `Sub(height_reg, input wy)` (`MatterEngine3/src/terrain_field.cpp:544-552`). Any
  `min(density, carve)` composition — even a y-independent carve — flips the whole
  world to the volumetric path: full-depth Y slabs from `yMin` instead of ±2 voxels
  around the surface, numeric gradients (two extra full field evals per vertex), and
  loss of the `density_at` short-circuit, **paid on every column of every sector**,
  kilometers from any river.
- The field op vocabulary has **no spatial gating, no branches, and a hard
  `kMaxSurfaceOps = 96` ceiling** (`MatterEngine3/src/terrain_field.h:23`);
  `min`/`max`/`blend` evaluate both operands unconditionally. 3D Bezier corridor SDFs
  are not expressible inside 96 ops. A carve therefore requires a **new native
  `Op::Kind`** (plus eval, y-dependence classification, and hash treatment) or
  composition at the `FieldRuntime` level in C++ — the design specifies neither.
- `FieldRuntime::hash()` is folded into **every sector's bake params** as `fieldHash`
  (`MatterEngine3/src/matter_engine.cpp:4680-4687`). As designed, moving one river
  node re-bakes every terrain sector in the world. This directly contradicts the
  caching section's promise that "unrelated sector content" is unaffected — here it's
  the reverse: the river invalidates everything unrelated.

**What the design must add:** a native carve op/runtime with an AABB/BVH corridor
prefilter so columns outside carve bounds keep the heightfield short-circuit (the
ColumnCache split survives — only the carve should land in the per-voxel y-dependent
tail); and an explicit invalidation-granularity story for `fieldHash` (e.g. a
spatially-bucketed carve hash folded per-sector, not one global hash). Without this,
the "identical terrain density in sectors and hydraulic voxelization" test passes
while the world's bake cost quietly triples.

### 2. The "existing scaffold" the design reuses mostly doesn't exist

The foundation section reads as "generalize proven substrate"; the code says
otherwise:

- `Occupancy` is a per-slot `std::unordered_map<uint64_t, {materialId}>` with
  `set`/`occupied`/`for_each` and nothing else — no erase, no bounds, no
  serialization, no bricks (`libs/MatterSurfaceLib/include/occupancy.h:22-27`).
  Signed packing exists (±2^20 per axis) but is per-slot, in occupancy.cpp, not
  lattice.h. **Nothing named brick, ghost, or halo exists anywhere in the repo** (the
  "halo" hits are an unrelated sphere-influence radius in cluster/cell).
- The lattice→occupancy→culling path has **zero engine callers** — its only consumers
  are `vertex_ao`, `cluster`'s no-mesh set, and tests. ParticleFlowLib never touches
  it. So "Existing `Occupancy` callers must retain their behavior" is a constraint on
  test code, and it should not be allowed to shape `SparseLattice3D`'s design.
- The warning that "the existing particle-culling rule must not be applied to moving
  water" guards against a hazard that can't currently occur: no material-exemption
  mechanism exists in `cull_interior`, and no water ever flows through it.

None of this invalidates the architecture — but `SparseLattice3D` is a **from-scratch
build** (brick directory, dense in-brick masks, halos, deterministic serialization,
GPU indexing), not "a short foundation task," and the parallel-delivery schedule that
freezes its contract first is resting on that mischaracterization. The honest claims
are `SlotCoord` / Chebyshev `slot_depth` / Surface-Skin-Core semantics (which do
exist and are worth inheriting) and the backlog's draw-into-lattice direction
(confirmed, `docs/superpowers/backlog.md:6-34`).

### 3. The solver section omits the numbers that determine feasibility

Free-surface D3Q19 at these scales has three well-known constraints, and the doc
addresses none:

- **Viscosity.** Water's ν = 1e-6 m²/s at dx = 1 m gives lattice viscosity
  ~1e-7–1e-8 → τ ≈ 0.5000001. Plain BGK is unconditionally unstable there. Every
  real solver at this scale (including FluidX3D, the cited reference) uses
  Smagorinsky LES and/or MRT/cumulant collision. The design must name its collision
  operator and turbulence treatment — this is not an implementation detail; it
  changes GPU memory layout, the artifact's velocity semantics, and whether
  "meter-scale velocity" is believable.
- **Velocity ceiling.** LBM caps u at ~0.1–0.3 lattice units, so
  u_phys ≤ ~0.1·dx/dt. A 20 m waterfall reaches ~20 m/s, forcing dt ≈ 5 ms at
  dx = 1 m for the entire section containing it — a ~10× cell-timestep multiplier
  concentrated exactly at the feature the design showcases. Waterfalls may need to be
  classified/excised regions with prescribed transfer rather than resolved flow; the
  doc should decide.
- **Pool filling runs in physical time.** A time-marching solver fills a pool in
  V/Q simulated seconds regardless of resolution; ramping inflow only makes it
  slower. A 50,000 m³ lake at 2 m³/s is ~25,000 s of simulated time in pass 1. The
  obvious accelerator is absent: **geometric pre-fill** — priority-flood over the
  solid classifier computes pool and spill elevations in milliseconds, and pass 1
  could initialize hydrostatically from it. In fact this begs the harder question:
  most of pass 1's stated outputs (connected wet regions, pool/spill elevations, wet
  envelope) are computable geometrically without any LBM. The design should justify
  why reconnaissance is a fluid solve at all, or restate pass 1 as geometric flood
  analysis + a short LBM refinement for backpressure/velocity.

Relatedly, the doc sets budgets and tolerances but gives **no expected magnitudes**
anywhere: no dt, steps-to-steady, cell-update throughput target, checkpoint size, or
peak-memory estimate. A design that hinges on "bounded cost" needs at least
order-of-magnitude arithmetic for the reference canyon.

### 4. The coarse pass is blind to sub-4m passages — worst on exactly this engine's flagship content

StreamCaverns-style narrow tunnels (this repo's own stress world has tunnel throats
descending to −1000 m) are invisible at 4 m cells. If a pool actually drains through
a 2 m throat, coarse reports it spilling over a sill instead; the fine envelope
(coarse wet region + collar + corridors + *registered* caves) omits the true
downstream path. The fine solve then discovers the drainage, hits the envelope
boundary, and triggers expand-retry cycles descending the tunnel one collar at a time
until the expansion budget aborts → PreviewOnly. "Water is never silently clipped" is
honored, but the flagship case degrades to a budget failure.

Fix is cheap: during envelope construction, run a **final-resolution
air-connectivity flood** (geometric, no fluid) from wet regions through the solid
classifier below max head + margin, and include reachable passages in the envelope up
front. Also note the envelope's "spill routes below the coarse maximum head" needs a
**head margin**, not just a spatial collar — fine resolution finds narrower channels
→ more resistance → pools can rise *above* the coarse head and top sills coarse said
stay dry.

### 5. "The existing Vulkan pipeline already supplies water IOR, transmission, Beer-Lambert absorption, rough refraction" — misleading as stated

What exists is a **generic glass lane in the RT raygen only**
(`MatterEngine3/shaders_vk/rt_lighting.rgen:729-880`: Fresnel, refraction, bounded
TIR walk, Beer-Lambert, VNDF rough refraction). Everything else the sentence implies
is absent:

- **The raster path has no scene refraction at all** — only a sky-only glass fallback
  in `composite.frag` with a flat tint. Since the design's near-camera water is
  raster geometry, "raster and RT water paths share material and normal logic" (a
  listed test) requires building the raster water shading path from nothing.
- **No tessellation or mesh shaders exist** — the features aren't even enabled on the
  device (`MatterEngine3/src/render/vk_context.cpp`). "Subdivided or tessellated and
  displaced in the vertex/mesh shader" must become "CPU-subdivided near mesh + pure
  vertex-shader displacement." The good news, verified: raster-vs-BLAS divergence is
  already shipped practice (impostor billboarding positions cards in `raster.vert`
  with no traced counterpart; compute skinning draws deformed vertices while the BLAS
  keeps bind pose), and RT reads separate per-part buffers from raster's mega-buffer,
  so the static-proxy scheme needs no new plumbing.
- **Two silent interactions need explicit decisions:** (a) transmission > 0 routes
  water into the non-opaque TLAS layer (`vk_scene_renderer.cpp:153-164`) → **any-hit
  traversal cost on every ray** crossing large water surfaces; (b) the volumetric
  sun-shadow ray uses `gl_RayFlagsOpaqueEXT`
  (`MatterEngine3/shaders_vk/vol_scatter.comp:203-215`), so non-opaque water **will
  not shadow fog or clouds** — lakes won't darken mist above them. Also the
  registered builtin water material (index 7, ior 1.33,
  `libs/MatterSurfaceLib/src/material_registry.c:43`) carries
  `MATERIAL_VOLUME_BOUNDARY`, which **nothing reads**.
- **Foam/spray has no substrate.** There is no particle system; the one bake-authored
  emitter path (`emitVolume` → froxel density) is **unwired in the Vulkan renderer**
  — its gatherer (`vk_emitter_gather.h`) has no production caller outside tests.
  "Procedural spray at waterfall classifications" implicitly includes wiring that
  path or building a new one; neither appears in any track.

## Significant findings

### 6. Two-pass decision is temporally incoherent

`boundedFinalCost` depends on the wet envelope, which only the coarse pass produces —
so the savings comparison cannot run before recon, and after recon the coarse cost is
sunk and should be excluded (compare bounded-final vs direct-from-warm-start). The
doc's single formula conflates the a-priori and a-posteriori decisions; specify when
the decision happens and what's in each side.

### 7. Determinism is under-specified relative to this project's own bar

The doc only addresses cross-device tolerance. The established bake gate here is
*same-device double-bake byte equality*. Sparse LBM frontier/compaction done with
atomics is nondeterministic run-to-run, which would churn the payload digest and
break that gate. Require: same device + driver ⇒ bit-identical artifact (atomic-free
compaction via prefix sums), and add a double-bake test to the acceptance list.

### 8. Backpressure wake-up will thrash without hysteresis

A slowly filling downstream lake raises head continuously; as written, every
increment re-dirties upstream sections. The Dirty transition needs a head-change
threshold/hysteresis band, and the doc should say so — it's a
correctness-of-termination issue, not tuning.

### 9. Sector-invalidation and storage edges are unstated but have exact existing templates

The sector bake must fold the hydrology artifact digest into its params (the
established channel: alongside `fieldHash` at
`MatterEngine3/src/matter_engine.cpp:4680`, same as the habitat tape); the natural
query integration point is a third pointer on `WorldBinding`
(`MatterEngine3/src/dsl_state.h:23-35`) next to `field` and `habitat`, with meshing
verbs that fail loudly when unbound; and artifact storage should mirror the
settle-cache pattern (`<cache_root>/hydrology/<key>`, version digest folded inside
the key function per the gtex precedent, `tileset_gtex.h:112-144`). Naming these in
the design costs three sentences and removes real ambiguity. Note also streamed
sector parts are deliberately *transient* (per-process scratch dir) — the water
artifact must not be.

### 10. Water has no LOD story

Near-camera displacement fade is covered, but nothing says what a lake looks like at
2 km: full 1 m mesh to the fog wall? A rung ladder? The repo's one-rule constraint
(`MatterEngine3/src/render/lod_distance.h`, "do not add a second projected-size
comparison") and the known cross-rung seam trap from nested-sector LOD (mesher
ownership is direction-asymmetric) both apply to water border stitching and are
unaddressed. This also interacts with the same-day 3D-sector design — the two specs
should cross-reference.

### 11. DSL surface gaps

- The turtle/builder API shows channel *reading* (`paths.channel("width")`) but no
  channel *writing* — every generator needs to author width/inflow along the path,
  and that API shape (per-node? per-sample? interpolated setter?) is exactly where
  determinism and hashing live.
- There are **two independent native readers** of world statics — the definition
  loader (`world_definition_loader.cpp`, regex class find, constructor bypassed) and
  `script_host.cpp::eval_world` (constructor called, hooks duck-typed). The design
  must say which runtime owns `hydrology(h)`; the `habitat(h)` recorder pattern in
  `eval_world` is the right template, but if the hook relies on constructor state it
  is invisible to the loader.
- Consumed-turtle misuse should throw at DSL time, not surface later in native
  validation.

### 12. Contract freeze before feasibility spike

The parallel model freezes all five contracts first, but `HydrologyArtifact`'s hint
set (vorticity, foam, spray) and `HydraulicScaffold`'s GPU indexing both depend on
what the solver can actually produce and how it wants memory laid out. Recommend a
one-fixture Track-B spike (open channel + small waterfall at physical scale; measure
cell-updates/s and steps-to-steady) *before* freezing `FluidSolver3D` and the
artifact schema. Finding 3's questions get answered empirically by the same spike.

### 13. Minor

- Convergence config units are unspecified (`velocityTolerance: 0.01` — m/s?
  relative?).
- Runtime residency for `sample_water` (whole-level artifact resident vs streamed) is
  unaddressed.
- The doc credits `lattice.h` with coordinate packing that actually lives in
  occupancy.cpp.
- "Approximately meter-scale velocity" for buoyancy is fine given trilinear fill
  interpolation, but say so.

## What the design gets right

Worth stating, since the review above is deliberately hostile: spline-as-intent with
solver authority is the correct call and cleanly stated; the
anonymous-node/consumed-turtle DSL rules are sound; graduated artifact status instead
of hard failure matches how this project actually ships; tolerance-based cross-GPU
validation matches the established replay stance; the static-BLAS + vertex-displaced
render surface scheme is not speculative — it matches two shipped precedents
(impostors, skinning) almost exactly; and the test matrix is unusually complete for a
design at this stage.

**Additions to the test matrix:** same-device double-bake determinism; a solver perf
regression fixture; and a regression proving a *non*-hydrology world keeps the
heightfield fast path.
