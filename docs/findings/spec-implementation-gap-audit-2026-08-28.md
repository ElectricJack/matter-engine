# MatterEngine design and implementation gap audit

**Date:** 2026-08-28
**Audited baseline:** `codex/dualsphysics-fluid-spike` at `7d49a548`, including the existing uncommitted working-tree changes
**Scope:** 242 finalized, project-authored design specifications, implementation plans, roadmaps, and backlog documents in the current tree and on reachable project branches
**Purpose:** identify material requirements that are not implemented in the current working tree, distinguish them from deliberately superseded work, and turn the result into one understandable gap register

## Executive summary

The repository is much more complete than the raw archive makes it appear. Of 242 audited documents:

| Classification | Documents | Meaning |
|---|---:|---|
| Implemented | 127 | Material behavior exists in the current tree with source/test evidence |
| Partial | 20 | A coherent subset exists, but material requirements remain |
| Missing | 4 | A still-relevant specification has no implementation in this branch |
| Superseded/obsolete | 88 | A later architecture deliberately replaced the document |
| Backlog | 3 | The document intentionally records unscheduled work |
| Unverifiable | 0 | No document required this classification after static evidence review |
| **Total** | **242** | |

The large superseded count is healthy history, not 88 lost features. It is dominated by the OpenGL-to-Vulkan transition, several generations of LOD/impostor design, the retired ExplorerDemo, deleted probe/CUDA-OptiX paths, and the failed bespoke LBM river solver that was replaced by the accepted in-process PhysX PBD pipeline.

The main current problem is not the fluid bake itself. The current river foundation is real: imperative river DSL, carved terrain, PhysX section bakes, sequential spillway handoff, static collision/gameplay fields, floating bodies, raster water, foam/wave shading, and 30-frame baked mesh animation are present. The urgent gap is that the water presentation architecture drifted:

- the implementation plan requires raster-only animated geometry and a static RT proxy;
- the later-edited design and dirty working tree added per-frame compute decode and cached animated BLAS geometry;
- the live run allocates about 63 MiB per animation-frame BLAS, reaching about 1.89 GiB for 30 frames;
- the approved 2026-08-28 direction now pins animated water to raster-only rendering and requires a general part/instance ray-tracing eligibility flag.

The decision is captured in `docs/superpowers/specs/2026-08-28-render-eligibility-and-document-lifecycle-design.md`. The remaining gap is implementation: remove animated-water RT geometry and bring the raster water material up to the surrounding RT scene's visual standard.

## What is ready versus not ready

### Ready and supported by current evidence

- Canonical MSVC/CMake/Ninja Windows builds and in-process PhysX integration.
- GPU visual water meshing with CPU oracle/fallback.
- Two sequential river sections, waterfall, pool/spillway handoff, and DSL-owned placement.
- Engine-wide bounded terrain collision and river float bodies.
- Immutable gameplay/presentation river fields.
- Dedicated raster water material with flow animation, depth response, foam, and baked 30-frame mesh playback.
- Current core engine/editor foundations: part graph, JS authoring, async bake, streaming, Vulkan renderer, native RT, DLSS bridge, ECS, Box3D, chart VT, atmosphere/clouds, profiler, event system, BakeTrace/Part Workbench, and Vulkan-only `.gtex` bake.

### Not ready or not complete

- The raster-only water architecture is approved, but the current dirty tree still builds/caches animated-water BLASes.
- Raster refraction, shallow-visible depth fog, turbulence foam, reflections, full evidence automation, and final performance gates remain incomplete. The retired High/Ultra RT-water requirements should not be implemented.
- A clean, reproducible acceptance run does not cover the current dirty water/shadow tree.
- The stable-slot/O(changed) RT TLAS CPU mirror redesign is missing.
- The current branch lacks the completed character-controller work that exists on `main`/`feature/character-controller`.
- The final LOD/VT proxy-world, visibility, unified-budget, and acceptance endpoint is incomplete.
- Streaming publication still performs a Vulkan registration tail on the app lane.
- Several lower-priority authoring/rendering designs remain partial or absent, listed below.

## Priority gap register

### P0 — Implement the approved raster-only animated-water policy

**Affected documents**

- `docs/completed/superpowers/plans/2026-08-26-baked-water-mesh-animation.md`
- `docs/completed/superpowers/specs/2026-08-26-baked-water-mesh-animation-design.md`
- `docs/deprecated/superpowers/plans/2026-08-24-real-time-river-presentation-floating-bodies.md`
- `docs/deprecated/superpowers/specs/2026-08-24-real-time-river-presentation-floating-bodies-design.md`

**Conflict**

The animation plan says the moving geometry is raster-only, the accepted static mesh remains the RT proxy, and animated vertices never enter BLAS build/update paths (`2026-08-26-baked-water-mesh-animation.md:5-7,24,395-424`). The amended design instead suppresses the static proxy in RT, compute-decodes every selected frame for RT, and caches one BLAS per loop frame (`2026-08-26-baked-water-mesh-animation-design.md:275-295`).

The dirty working tree follows the amended design in `MatterEngine3/src/render/vk_scene_renderer.cpp:13130-13448` and `MatterEngine3/shaders_vk/water_animation_rt_decode.comp`. Existing live evidence records 30 roughly 63 MiB BLAS entries and a final cache near 1,891 MiB in `.codex-tmp/live-editor/river.stderr.log:50-222`. A 10-shadow-sample run measured 14.05 ms median frame time, with 11.43 ms in RT GI/transmission, while the water animation raster work itself was about 0.06 ms (`.codex-tmp/render-perf/current-rt-s10.jsonl:1`). This is not a causal no-water-BLAS comparison: the warmed run reports `gpu_blas_ms=0`, so it proves the cache-memory cost and the overall RT cost, not that animated-water BLAS caused the 11.43 ms. A matched A/B remains required.

**Gap**

The authoritative design now requires animated water to remain raster-only, but the implementation still contains animated RT compute decode, BLAS caching, and TLAS insertion.

**Closure**

1. Add the general part/instance `rayTraced` boolean and mark water false.
2. Delete or disable animated RT compute decode, per-frame BLAS construction/cache, TLAS insertion, and their 2 GiB budget.
3. Preserve the recent correct shadow behavior for raster water and static scene receivers.
4. Add tests proving animated water never enters BLAS/TLAS preparation.
5. Re-measure the same RiverFloatLab cameras at 1, 10, and 16 shadow samples.

**Impact:** highest. This removes the proven animated-BLAS memory cost, creates the matched measurement needed to quantify the performance change, and aligns the renderer with the gameplay requirement: realistic moving water without runtime fluid interaction.

### P0 — Re-scope and finish river presentation acceptance

**Affected documents:** the real-time river presentation design/plan and baked-animation design/plan.

The implemented foundation includes immutable fields, float forces, dedicated water identity, flow-following waves, automatic foam, raster playback, collision, and section handoff. Remaining original requirements include `VulkanWaterQuality` Low/High/Ultra policy, RT-derived/fallback thickness diagnostics, High/Ultra refraction bounds, Ultra shallow caustics, per-feature budgets, scripted traversal, screenshots, CSV/perf summaries, and live two-section float-body acceptance.

The original RT-water requirements are retired rather than incomplete commitments. The raster-water work now covers:

- lit-scene refraction or distortion;
- depth-based absorption/fog with shallow-bottom visibility;
- SSR/environment fallback;
- flow-driven normals and waves;
- foam/whitewater response;
- temporal/reactivity behavior;
- static-scene shadow reception without duplicate shadows;
- performance and memory budgets with zero animated-water BLAS work.

After that revision, run a clean MSVC/PhysX bake and cache-hit playthrough with the five existing river cameras and floating objects.

### P1 — RT world scaling: stable-slot TLAS CPU mirror is missing

**Document:** `docs/rt-tlas-cpu-mirror-redesign-2026-08-07.md`
**Classification:** missing

The current renderer still scans all resident `rt_instances_` during `build_ray_geometry` every frame (`MatterEngine3/src/render/vk_scene_renderer.cpp:12640-12695`). The specified stable `(instance, cluster)` slots, free-list reuse, zero-on-free, O(changed) patching, dense-active GPU compaction, validator, and perf/memory gate do not exist.

**Impact:** CPU RT preparation remains O(resident RT world), a large-world scaling risk even after animated water leaves RT.

### P1 — Character controller is implemented elsewhere but absent here

**Document:** `docs/superpowers/specs/2026-08-15-character-controller-design.md` on `feature/character-controller` and `main`
**Classification:** missing in the audited branch

M0-M4 commits exist on `feature/character-controller`/`main`, but they are not ancestors of the current fluid branch. Current HEAD has none of the defining `MoveIntent`, `CharacterController`, fixed-step movement, collide-and-slide, step/slope, walk/fly, jump/gravity, or JS authoring surfaces.

**Impact:** not required to keep floating test rafts moving, but required before a conventional controllable-character playtest. This is an integration gap, not lost implementation work.

### P1 — LOD/VT migration has not reached its final endpoint

**Documents**

- `docs/lod-vt-redesign-2026-08-04.md`
- `docs/superpowers/plans/2026-08-04-lod-vt-migration.md`

**Classification:** partial

M0-M6 and substantial instrumentation landed. Still open are the complete visibility-gated proxy world, unified budget governor, remaining cross-product/residency gates, and final visual/RT measurements. The implementation plan itself records decisive acceptance as unrun and retains M7-M9 work.

**Impact:** large worlds cannot yet claim the intended visibility-driven work suppression or whole-system frame/memory budgets.

### P1 — Streaming publication retains an app-lane Vulkan tail

**Document:** `docs/superpowers/specs/2026-08-07-bake-publish-offthread-design.md`
**Classification:** partial

Child-catalog prewarm and CPU Vulkan-part construction moved off-thread, but publication still calls renderer registration/`ensure_part` on the app lane (`MatterEngine3/src/matter_engine.cpp:9159-9268,11615-11631`). The specified `PreparedPart`/adopt seam and sub-2-ms activation endpoint are not complete.

**Impact:** a large newly published part can still hitch streaming.

### P1 — Dynamic command-layout relayout remains O(resident world)

**Current authority:** `ROADMAP.md` under "Scale before expansion"
**Historical detail:** `docs/deprecated/superpowers/backlog.md`

The planned static-layout generation and memoized merged counts/staging size have not landed. One dynamic entity can still trigger resident-world relayout work around `MatterEngine3/src/render/vk_scene_renderer.cpp:11356,12371`.

**Impact:** another large-world frame-time scaling risk, complementary to the TLAS mirror gap.

### P2 — Authoring and editor gaps

#### Native Windows live-edit watcher

`docs/superpowers/specs/2026-06-24-dev-live-edit-design.md` is partial. Linux watching, debounce, scoped rebuilds, budgets, and last-good behavior exist, but `MatterEngine3/src/win_watcher.h:6-13` is still a throwing `ReadDirectoryChangesW` stub.

#### First-class lattice authoring

`docs/superpowers/specs/2026-06-24-dsl-procedural-geometry-design.md` and the broader procedural-part authoring design are partial. The DSL has voxel and triangle sessions, but not `beginLattice`/`endLattice`, mutable/queryable lattice operations such as deposit/erode/dilate, lattice scatter, or direct mesh `pointsOnSurface`. The procedural-part north-star also names unscheduled SP-8 through SP-10 networking/server/collaboration work.

#### True ROUND extrusion joins

The primitive expansion design and combined geometry-primitives plan are partial. `ROUND` currently aliases `BEVEL` in `MatterEngine3/src/triangle_emit.cpp:503`, and tests do not distinguish them.

#### Interactive Settle Lab

`MatterEngine3/docs/settle-tick-optimizer.md` is partial. The step API, pose metric, plan builder, and headless benchmark exist; the editor panel with stepping, convergence curves, collider/blame visualization, parameters, and ghost A/B view is deliberately parked.

#### Decal-ground compositing beyond the foundation

The branch-only `2026-08-01-decal-ground-compositing-design.md` is partial. Production warp, charting, and near-band modulation exist. Missing phases are:

- VT height lane and format bump;
- decal atlas, masks, hashes, and authoring;
- bounded compositor gather and conforming placement;
- live micro-elements and grain;
- far-field height relief;
- RT evaluation/parity and anti-repetition acceptance.

### P2 — Rendering and material gaps

#### Baked part-local AO

The 2026-07-15 Vulkan bake/part-AO design and plan are partial. Probe/cache/flatten fixes landed, but the planned part-local AO product, authoring quality, schema/cache participation, tests, and acceptance were reverted; `part_ao_bake.*` is absent.

#### Ground macro frequency split

The 2026-07-21 tileset parallax/macro design and plan are partial. Close POM/detail exists; `ForestFloorMacro`, `groundMacroSlot` content, macro-deviation composition in raster and RT, and the visual gate remain backlogged.

#### Procedural animation E/F/J endpoint

The procedural-animation system design and D/E/F/J plan are partial; the remaining-work plan is backlog. Missing material endpoints include gameplay JS/event/tick binding, general long-chain FABRIK and authored constraints, deforming/skinned RT geometry/BLAS policy, and several production/polish work packages.

#### Ray-traced ice/snow

`docs/superpowers/specs/2026-07-29-rt-pbr-ice-snow-design.md` is missing. Generic transmission, IOR, Beer-Lambert, clearcoat, and snow content exist, but the specified ice/snow glint, dusting, rough-transmission behavior, authoring, tests, and acceptance do not.

#### Emissive meshes lighting volumetric fog

`MatterEngine3/docs/volumetric-emission-sampling.md` is missing. No froxel TLAS emission-ray loop, RT part/material descriptors, mesh emission fetch, settings/UI, fallbacks, tests, or performance gate exists. The existing CPU-gathered volumetric emitters are a different feature.

#### Complete raylib dependency removal

The 2026-07-25 removal plan is partial. Rendering is Vulkan-only and GL/raylib rendering is gone, but raylib headers/vendor remain as an include-only POD dependency. Product impact is low; build/dependency maintenance is the remaining cost.

### P3 — Deferred inventory retained for product decisions

These ideas were recorded by the audit and are preserved for explicit product
decisions; they are not scheduled commitments in the current roadmap:

- multi-resolution draw-into-lattice authoring;
- particle shapes beyond spheres;
- ground macro tileset and associated artist passes;
- rock/plane datum alignment;
- GI shimmer tuning;
- current-renderer voxel-box impostors;
- volumetric emitter wiring audit;
- full Meadow-scale restoration;
- dynamic command-layout memoization;
- rollback-only seam overlap-band filtering;
- broad procedural-animation remaining work; and
- general particle-material/thermal/reaction/electrical/bonding sandbox concepts in the archived legacy roadmap.

The archived `docs/deprecated/roadmaps/ROADMAP-legacy-2026-08-28.md` also
contains two unresolved measurements: the autoremesher's full-Meadow
safety/load proof and the old 500,000-instance compact-cull-transform target.
They are historical candidates, not current roadmap commitments. The
shared-object test build, three-part OOM hardening, and core autoremesher
integration are implemented; the old HiZ, ODE, SurfaceLib,
OpenParticleSurfaceLib, and standalone GPU-ray entries are superseded.

Additional bookkeeping from the source audits should remain visible: the
archived backlog's stale-binary/target-link test-hygiene assumptions need a fresh MSVC
re-audit; the MatterSurfaceLib dynamic-matter/data-store aspirations remain
future work and `AssetStore` still has no production consumer; and the legacy
`visual-river-backlog.md` is mostly implemented or architecture-superseded,
with only its old noncanonical AnimationGallery/autoremesher-disabled
configuration left unverified.

## Historical non-complete matrix

This table accounts for every document classified partial, missing, or backlog
when the parallel audit was run. It preserves the point-in-time inventory;
rows marked "retired after audit" were subsequently archived by the approved
documentation cleanup. Companion design/plan rows often describe one physical
defect; document counts are not unique-defect counts.

| Document | Class | Gap |
|---|---|---|
| `specs/2026-06-24-dev-live-edit-design.md` | Partial | Native Windows watcher |
| `specs/2026-06-24-dsl-procedural-geometry-design.md` | Partial | Lattice session/query/mutation/scatter |
| `specs/2026-06-24-procedural-part-authoring-design.md` | Partial | Lattice plus SP-8–SP-10 north-star |
| `specs/2026-06-27-geometry-primitives-implementation-plan.md` | Partial | ROUND aliases BEVEL |
| `specs/2026-06-27-primitive-library-expansion-design.md` | Partial | ROUND arc fillet absent |
| `claude/decal-ground-compositing-spec:docs/superpowers/specs/2026-08-01-decal-ground-compositing-design.md` | Partial | Phases 3–8 absent |
| `plans/2026-07-15-vulkan-bake-fixes-and-part-ao.md` | Partial | Baked part AO absent |
| `specs/2026-07-15-vulkan-bake-fixes-and-part-ao-design.md` | Partial | Baked part AO absent |
| `plans/2026-07-21-tileset-vulkan-parallax-macro.md` | Partial | Macro frequency split/content absent |
| `specs/2026-07-21-tileset-vulkan-parallax-macro-design.md` | Partial | Macro frequency split/content absent |
| `plans/2026-07-25-mathlib-and-raylib-removal.md` | Partial | Include-only raylib dependency remains |
| `plans/2026-07-26-procedural-animation-phases-d-e-f-j.md` | Partial | Gameplay, long-chain IK, deforming RT |
| `specs/2026-07-22-procedural-animation-system-design.md` | Partial | Gameplay, long-chain IK, deforming RT |
| `plans/2026-08-04-lod-vt-migration.md` | Partial | Proxy/visibility/budget/final gates |
| `docs/lod-vt-redesign-2026-08-04.md` | Partial | Proxy/visibility/budget/final gates |
| `specs/2026-08-07-bake-publish-offthread-design.md` | Partial | App-lane Vulkan registration tail |
| `plans/2026-08-24-real-time-river-presentation-floating-bodies.md` | Retired after audit | RT-water quality ladder replaced by the approved raster-only design |
| `specs/2026-08-24-real-time-river-presentation-floating-bodies-design.md` | Retired after audit | RT-water quality ladder replaced by the approved raster-only design |
| `MatterEngine3/docs/settle-tick-optimizer.md` | Partial | Interactive Settle Lab |
| `docs/deprecated/roadmaps/ROADMAP-legacy-2026-08-28.md` | Retired after audit | Mixed legacy roadmap replaced by the concise root roadmap |
| `docs/rt-tlas-cpu-mirror-redesign-2026-08-07.md` | Missing | Stable-slot O(changed) TLAS CPU mirror |
| `specs/2026-07-29-rt-pbr-ice-snow-design.md` | Missing | Ice/snow material additions |
| `MatterEngine3/docs/volumetric-emission-sampling.md` | Missing | Mesh emission in fog |
| `feature/character-controller:docs/superpowers/specs/2026-08-15-character-controller-design.md` | Missing in branch | Completed implementation not integrated |
| `plans/2026-07-26-procedural-animation-remaining-work.md` | Retired after audit | Broad queue archived; retained animation choices are deferred in `ROADMAP.md` |
| `docs/superpowers/backlog.md` | Retired after audit | Entry queue archived; selected candidates are grouped in `ROADMAP.md` |
| `codex/river-hydrology:specs/visual-river-backlog.md` | Backlog | Conditional legacy river hardening/UX |

## Superseded work that should not return to the roadmap

The 88 superseded/obsolete documents fall into a small number of deliberate replacement families:

| Historical family | Current successor | What remains useful |
|---|---|---|
| Occupancy and cell-granular interior culling | Per-cell skip/core classification | Culling invariants and tests |
| Cage, fitted, chart, voxel-box, macrocell impostor proposals | Vulkan card bake plus unified representation rungs | Deterministic terminal representation goals |
| Part serialization v1 | Part artifact v2 | Compatibility reader |
| Legacy OpenGL viewers/raster switch/Meadow rendering | Vulkan-only MatterEditor | Authored content and performance intent |
| ExplorerDemo/editor split | MatterEditor plus Phase-C streaming | Time-to-visible, progress, regeneration concepts |
| Fixed frame-time LOD and several branch selectors | 2026-08-04 unified representation model | Stable transitions and benefit-based selection |
| Probe bricks/SH-L1 thread | Native RT/GI/temporal pipeline | Scatter-scale work only |
| CUDA/OptiX interop | Native Vulkan RT plus Streamline | Matrix/motion/jitter discipline |
| 2-D nested sectors and runtime seam welder default | Volumetric sectors plus shared-contour seams | 2:1 balance and failure history |
| Bespoke D3Q19/MRT-LBM river and rescue experiments | In-process PhysX PBD section bakes | Determinism, artifacts, terrain coupling, acceptance discipline |
| Fixed-domain visual/staged river milestones | Sequential PhysX river sections and animation | River DSL, fill sensors, static-product model |
| Original Bake Lab Part Lab/variants | Part Workbench; BakeTrace/timeline retained | Trace and manual optimization tools |
| ODE ParticleDynamicsLib proposal | Box3D runtime and PhysX PBD | General simulation aspirations only |

No implementation effort should be scheduled from a superseded document without first writing a new current design.

## Recommended execution order

1. **Add general RT eligibility and remove animated-water RT geometry.** Delete compute decode, BLAS cache/build, TLAS records, and the 2 GiB cache budget; add negative BLAS/TLAS tests.
2. **Improve and accept raster water.** Depth visibility/fog, refraction/distortion, SSR/environment fallback, foam/waves/reactivity, correct shadow reception, and matched performance captures.
3. **Create one clean playtest baseline.** MSVC/PhysX cold bake, cache-hit run, five river cameras, floating crates/rafts with terrain collision, memory/perf CSV, and a live editor acceptance.
4. **Integrate only the gameplay prerequisites needed next.** Bring the character controller from `main` if the next test requires a walking player; otherwise design raft input/control first.
5. **Address scaling before expanding world length.** Stable-slot TLAS mirror, app-lane publish tail, dynamic-layout memoization, and remaining LOD/VT visibility/budget work.
6. **Schedule lower-priority rendering/authoring gaps explicitly.** Do not let old partial specs silently compete with the river/gameplay roadmap.

## Audit methodology and limits

Three parallel subagents read non-overlapping cohorts and compared every document against the current working tree, tests, build manifests, architecture docs, and useful Git history. Unchecked checkboxes and stale status lines were not accepted as proof. Branch-only documents were read with `git show` and judged against the current branch.

Inventory composition:

- 95 current-tree files under `docs/superpowers/specs`;
- 88 current-tree files under `docs/superpowers/plans`;
- 9 standalone design notes under `docs`;
- 7 engine-internal specs/plans under `MatterEngine3/docs`;
- `ROADMAP.md` and `docs/superpowers/backlog.md`;
- 15 finalized branch-only specifications and 26 branch-only plans from local/origin refs.

Excluded from the document count but used as evidence:

- `*-results.md`, acceptance findings, review logs, QA docs, architecture/current-state references;
- temporary `.superpowers` briefs/reports and brainstorm drafts;
- duplicate `.codex-tmp/parent-proof` copies;
- third-party/vendor designs and roadmaps.

This was a static implementation-coverage audit. It did not run the expensive full build, PhysX bake, GPU visual acceptance, or performance suite. Existing uncommitted source was counted as implementation but not as proof of a clean, reproducible checkout. The raster-only water decision was approved immediately after the inventory was completed, so the raw classification totals remain a point-in-time audit while the priority text above reflects the accepted direction.

## Complete inventory

The following ledger is the exact 242-document audit population. It is an
immutable point-in-time inventory and therefore retains original paths and
classifications. Current locations are recorded in
`docs/completed/MANIFEST.md` and `docs/deprecated/MANIFEST.md`; current product
priority is defined only by `ROADMAP.md`.

### Current-tree design/spec archive (95)

1. `docs/superpowers/specs\2026-06-13-mesh-simplification-design.md`
2. `docs/superpowers/specs\2026-06-14-cell-granular-interior-culling-design.md`
3. `docs/superpowers/specs\2026-06-14-cell-skip-meshing-interior-design.md`
4. `docs/superpowers/specs\2026-06-14-lattice-particle-stress-scene-design.md`
5. `docs/superpowers/specs\2026-06-14-material-aware-surfacing-design.md`
6. `docs/superpowers/specs\2026-06-14-occupancy-interior-culling-design.md`
7. `docs/superpowers/specs\2026-06-14-tiered-surface-lattice-design.md`
8. `docs/superpowers/specs\2026-06-15-organic-surface-carving-design.md`
9. `docs/superpowers/specs\2026-06-15-parallel-cell-meshing-design.md`
10. `docs/superpowers/specs\2026-06-15-surface-scratch-context-design.md`
11. `docs/superpowers/specs\2026-06-19-meshing-algorithm-interface-design.md`
12. `docs/superpowers/specs\2026-06-20-baked-vertex-ao-design.md`
13. `docs/superpowers/specs\2026-06-20-imposter-generation-design.md`
14. `docs/superpowers/specs\2026-06-20-part-serialization-design.md`
15. `docs/superpowers/specs\2026-06-21-fitted-cage-imposter-design.md`
16. `docs/superpowers/specs\2026-06-22-chart-based-cage-uv-design.md`
17. `docs/superpowers/specs\2026-06-22-voxel-box-imposter-design.md`
18. `docs/superpowers/specs\2026-06-24-composition-to-world-design.md`
19. `docs/superpowers/specs\2026-06-24-dev-live-edit-design.md`
20. `docs/superpowers/specs\2026-06-24-dsl-procedural-geometry-design.md`
21. `docs/superpowers/specs\2026-06-24-part-artifact-v2-design.md`
22. `docs/superpowers/specs\2026-06-24-part-graph-install-design.md`
23. `docs/superpowers/specs\2026-06-24-procedural-part-authoring-design.md`
24. `docs/superpowers/specs\2026-06-24-script-host-design.md`
25. `docs/superpowers/specs\2026-06-24-sector-lod-instanced-world-design.md`
26. `docs/superpowers/specs\2026-06-24-shared-script-library-design.md`
27. `docs/superpowers/specs\2026-06-24-temporal-rendering-foundation-design.md`
28. `docs/superpowers/specs\2026-06-24-triangle-path-variations-design.md`
29. `docs/superpowers/specs\2026-06-25-world-viewer-design.md`
30. `docs/superpowers/specs\2026-06-26-materials-and-tree-design.md`
31. `docs/superpowers/specs\2026-06-27-geometry-primitives-implementation-plan.md`
32. `docs/superpowers/specs\2026-06-27-primitive-library-expansion-design.md`
33. `docs/superpowers/specs\2026-06-27-stateful-dsl-completeness-design.md`
34. `docs/superpowers/specs\2026-06-27-typed-iso-primitives-design.md`
35. `docs/superpowers/specs\2026-07-02-meadow-density-demo-design.md`
36. `docs/superpowers/specs\2026-07-02-raster-switch-design.md`
37. `docs/superpowers/specs\2026-07-03-frame-time-lod-design.md`
38. `docs/superpowers/specs\2026-07-03-gpu-instancing-culling-design.md`
39. `docs/superpowers/specs\2026-07-03-world-picker-panel-design.md`
40. `docs/superpowers/specs\2026-07-05-ground-tileset-bake-design.md`
41. `docs/superpowers/specs\2026-07-05-stress-forest-flatten-policy-design.md`
42. `docs/superpowers/specs\2026-07-07-autoremesher-integration-design.md`
43. `docs/superpowers/specs\2026-07-07-engine-editor-roadmap.md`
44. `docs/superpowers/specs\2026-07-07-phase-a-kernel-extraction-design.md`
45. `docs/superpowers/specs\2026-07-08-memory-lib-design.md`
46. `docs/superpowers/specs\2026-07-08-modifier-regions-design.md`
47. `docs/superpowers/specs\2026-07-08-phase-b-async-bake-design.md`
48. `docs/superpowers/specs\2026-07-09-particle-flow-tree-design.md`
49. `docs/superpowers/specs\2026-07-09-rock-realism-design.md`
50. `docs/superpowers/specs\2026-07-10-lod-aware-instanced-children-design.md`
51. `docs/superpowers/specs\2026-07-10-phase-c-infinite-world-design.md`
52. `docs/superpowers/specs\2026-07-11-scatter-scales-probe-bricks-design.md`
53. `docs/superpowers/specs\2026-07-13-vulkan-temporal-foundation-design.md`
54. `docs/superpowers/specs\2026-07-14-vulkan-fix3-review-followup-design.md`
55. `docs/superpowers/specs\2026-07-14-vulkan-gpu-instancing-parity-design.md`
56. `docs/superpowers/specs\2026-07-14-vulkan-hybrid-gi-materials-design.md`
57. `docs/superpowers/specs\2026-07-14-vulkan-rt-dlss-super-resolution-design.md`
58. `docs/superpowers/specs\2026-07-14-vulkan-smoke-ui-isolation-design.md`
59. `docs/superpowers/specs\2026-07-15-lighting-sculpture-garden-design.md`
60. `docs/superpowers/specs\2026-07-15-rt-transport-design.md`
61. `docs/superpowers/specs\2026-07-15-vulkan-bake-fixes-and-part-ao-design.md`
62. `docs/superpowers/specs\2026-07-15-vulkan-lighting-exposure-controls-design.md`
63. `docs/superpowers/specs\2026-07-16-froxel-volumetrics-design.md`
64. `docs/superpowers/specs\2026-07-17-flecs-ecs-foundation-design.md`
65. `docs/superpowers/specs\2026-07-17-world-as-js-authoring-design.md`
66. `docs/superpowers/specs\2026-07-18-box3d-runtime-physics-design.md`
67. `docs/superpowers/specs\2026-07-18-sector-streaming-manual-acceptance.md`
68. `docs/superpowers/specs\2026-07-19-phase4-runtime-scene-editor-bridge-design.md`
69. `docs/superpowers/specs\2026-07-19-phase5-world-as-js-authoring-integration.md`
70. `docs/superpowers/specs\2026-07-19-phase5.5-manual-acceptance.md`
71. `docs/superpowers/specs\2026-07-19-phase5.5-viewer-editor-panels.md`
72. `docs/superpowers/specs\2026-07-21-tileset-vulkan-parallax-macro-design.md`
73. `docs/superpowers/specs\2026-07-22-procedural-animation-system-design.md`
74. `docs/superpowers/specs\2026-07-28-alpine-streaming-terrain-design.md`
75. `docs/superpowers/specs\2026-07-29-alpine-vegetation-gallery-design.md`
76. `docs/superpowers/specs\2026-07-29-chart-virtual-texturing-design.md`
77. `docs/superpowers/specs\2026-07-29-rt-pbr-ice-snow-design.md`
78. `docs/superpowers/specs\2026-07-30-alpine-streaming-vegetation-design.md`
79. `docs/superpowers/specs\2026-07-30-texel-tape-design.md`
80. `docs/superpowers/specs\2026-07-31-property-system-design.md`
81. `docs/superpowers/specs\2026-08-07-bake-publish-offthread-design.md`
82. `docs/superpowers/specs\2026-08-07-engine-profiler-design.md`
83. `docs/superpowers/specs\2026-08-08-physical-atmosphere-volumetric-clouds-design.md`
84. `docs/superpowers/specs\2026-08-09-atmosphere-presentation-lighting-adjustments-design.md`
85. `docs/superpowers/specs\2026-08-10-cloud-terrain-occlusion-design.md`
86. `docs/superpowers/specs\2026-08-10-retain-volumetric-history-across-atmosphere-updates-design.md`
87. `docs/superpowers/specs\2026-08-10-river-hydrology-design-review.md`
88. `docs/superpowers/specs\2026-08-10-river-hydrology-review-2026-08-12.md`
89. `docs/superpowers/specs\2026-08-22-gpu-visual-meshing-foundation-design.md`
90. `docs/superpowers/specs\2026-08-22-physx-fluid-bake-integration-design.md`
91. `docs/superpowers/specs\2026-08-22-windows-msvc-build-migration-design.md`
92. `docs/superpowers/specs\2026-08-24-engine-wide-terrain-collision-design.md`
93. `docs/superpowers/specs\2026-08-24-real-time-river-presentation-floating-bodies-design.md`
94. `docs/superpowers/specs\2026-08-24-sequential-river-sections-waterfall-design.md`
95. `docs/superpowers/specs\2026-08-26-baked-water-mesh-animation-design.md`

### Current-tree implementation-plan archive (88)

1. `docs/superpowers/plans\2026-06-11-verified-bug-fixes.md`
2. `docs/superpowers/plans\2026-06-13-mesh-simplification.md`
3. `docs/superpowers/plans\2026-06-14-cell-granular-interior-culling.md`
4. `docs/superpowers/plans\2026-06-14-cell-skip-meshing-interior.md`
5. `docs/superpowers/plans\2026-06-14-lattice-particle-stress-scene.md`
6. `docs/superpowers/plans\2026-06-14-material-aware-surfacing.md`
7. `docs/superpowers/plans\2026-06-14-occupancy-interior-culling.md`
8. `docs/superpowers/plans\2026-06-14-tiered-surface-lattice.md`
9. `docs/superpowers/plans\2026-06-15-organic-surface-carving.md`
10. `docs/superpowers/plans\2026-06-15-parallel-cell-meshing.md`
11. `docs/superpowers/plans\2026-06-15-surface-scratch-context.md`
12. `docs/superpowers/plans\2026-06-19-meshing-algorithm-interface.md`
13. `docs/superpowers/plans\2026-06-20-baked-vertex-ao.md`
14. `docs/superpowers/plans\2026-06-20-imposter-generation.md`
15. `docs/superpowers/plans\2026-06-20-part-serialization.md`
16. `docs/superpowers/plans\2026-06-21-fitted-cage-imposter.md`
17. `docs/superpowers/plans\2026-06-22-chart-based-cage-uv.md`
18. `docs/superpowers/plans\2026-06-22-voxel-box-imposter.md`
19. `docs/superpowers/plans\2026-06-24-composition-to-world-plan.md`
20. `docs/superpowers/plans\2026-06-24-dev-live-edit-plan.md`
21. `docs/superpowers/plans\2026-06-24-part-artifact-v2-plan.md`
22. `docs/superpowers/plans\2026-06-24-part-graph-install-plan.md`
23. `docs/superpowers/plans\2026-06-24-procedural-part-system-master-plan.md`
24. `docs/superpowers/plans\2026-06-24-script-host-plan.md`
25. `docs/superpowers/plans\2026-06-24-shared-script-library-plan.md`
26. `docs/superpowers/plans\2026-06-24-triangle-path-variations-plan.md`
27. `docs/superpowers/plans\2026-06-25-world-viewer.md`
28. `docs/superpowers/plans\2026-06-26-materials-and-tree.md`
29. `docs/superpowers/plans\2026-07-02-meadow-density-demo.md`
30. `docs/superpowers/plans\2026-07-02-raster-lighting-clusters.md`
31. `docs/superpowers/plans\2026-07-02-raster-mvp.md`
32. `docs/superpowers/plans\2026-07-03-frame-time-lod.md`
33. `docs/superpowers/plans\2026-07-03-gpu-instancing-culling.md`
34. `docs/superpowers/plans\2026-07-03-world-picker-panel.md`
35. `docs/superpowers/plans\2026-07-05-stress-forest-flatten-policy.md`
36. `docs/superpowers/plans\2026-07-05-tileset-physics-core.md`
37. `docs/superpowers/plans\2026-07-06-tileset-dsl-placement.md`
38. `docs/superpowers/plans\2026-07-06-tileset-gpu-bake-gtex.md`
39. `docs/superpowers/plans\2026-07-06-tileset-viewer-consumption.md`
40. `docs/superpowers/plans\2026-07-07-autoremesher-integration.md`
41. `docs/superpowers/plans\2026-07-07-code-review-fixes.md`
42. `docs/superpowers/plans\2026-07-07-phase-a-kernel-extraction.md`
43. `docs/superpowers/plans\2026-07-08-memory-lib.md`
44. `docs/superpowers/plans\2026-07-08-modifier-regions.md`
45. `docs/superpowers/plans\2026-07-08-phase-b-async-bake.md`
46. `docs/superpowers/plans\2026-07-09-particle-flow-tree.md`
47. `docs/superpowers/plans\2026-07-09-rock-realism.md`
48. `docs/superpowers/plans\2026-07-10-lod-aware-instanced-children.md`
49. `docs/superpowers/plans\2026-07-10-phase-c-infinite-world.md`
50. `docs/superpowers/plans\2026-07-11-scatter-scales-probe-bricks.md`
51. `docs/superpowers/plans\2026-07-13-vulkan-matrix-phase1.md`
52. `docs/superpowers/plans\2026-07-14-vulkan-fix3-review-followup.md`
53. `docs/superpowers/plans\2026-07-14-vulkan-gpu-instancing-parity.md`
54. `docs/superpowers/plans\2026-07-14-vulkan-hybrid-gi-materials.md`
55. `docs/superpowers/plans\2026-07-14-vulkan-rt-dlss-super-resolution.md`
56. `docs/superpowers/plans\2026-07-15-lighting-sculpture-garden.md`
57. `docs/superpowers/plans\2026-07-15-rt-transport.md`
58. `docs/superpowers/plans\2026-07-15-vulkan-bake-fixes-and-part-ao.md`
59. `docs/superpowers/plans\2026-07-15-vulkan-lighting-exposure-controls.md`
60. `docs/superpowers/plans\2026-07-16-froxel-volumetrics.md`
61. `docs/superpowers/plans\2026-07-16-indexed-mesh-format-stage1.md`
62. `docs/superpowers/plans\2026-07-17-flecs-ecs-foundation.md`
63. `docs/superpowers/plans\2026-07-18-box3d-runtime-physics.md`
64. `docs/superpowers/plans\2026-07-18-ecs-sector-streaming-consolidation.md`
65. `docs/superpowers/plans\2026-07-19-phase4-runtime-scene-editor-bridge.md`
66. `docs/superpowers/plans\2026-07-21-tileset-vulkan-parallax-macro.md`
67. `docs/superpowers/plans\2026-07-22-procedural-animation-phase-abc.md`
68. `docs/superpowers/plans\2026-07-25-mathlib-and-raylib-removal.md`
69. `docs/superpowers/plans\2026-07-25-phase-b-integration-repair.md`
70. `docs/superpowers/plans\2026-07-25-repo-layout-and-cache-consolidation.md`
71. `docs/superpowers/plans\2026-07-26-procedural-animation-phases-d-e-f-j.md`
72. `docs/superpowers/plans\2026-07-26-procedural-animation-remaining-work.md`
73. `docs/superpowers/plans\2026-07-29-alpine-vegetation-gallery.md`
74. `docs/superpowers/plans\2026-07-29-chart-virtual-texturing-plan.md`
75. `docs/superpowers/plans\2026-07-30-alpine-streaming-vegetation.md`
76. `docs/superpowers/plans\2026-08-04-lod-vt-migration.md`
77. `docs/superpowers/plans\2026-08-08-nested-sector-lod-migration.md`
78. `docs/superpowers/plans\2026-08-09-atmosphere-presentation-lighting-adjustments.md`
79. `docs/superpowers/plans\2026-08-09-physical-atmosphere-volumetric-clouds.md`
80. `docs/superpowers/plans\2026-08-10-cloud-terrain-occlusion.md`
81. `docs/superpowers/plans\2026-08-10-retain-volumetric-history-across-atmosphere-updates.md`
82. `docs/superpowers/plans\2026-08-22-windows-msvc-build-migration.md`
83. `docs/superpowers/plans\2026-08-23-gpu-visual-meshing-foundation.md`
84. `docs/superpowers/plans\2026-08-23-physx-fluid-bake-integration.md`
85. `docs/superpowers/plans\2026-08-24-real-time-river-presentation-floating-bodies.md`
86. `docs/superpowers/plans\2026-08-24-sequential-river-sections-waterfall.md`
87. `docs/superpowers/plans\2026-08-25-engine-wide-terrain-collision.md`
88. `docs/superpowers/plans\2026-08-26-baked-water-mesh-animation.md`

### Standalone/current design, implementation, roadmap, and backlog documents (18)

1. `docs/contour-seam-design-2026-08-13.md`
2. `docs/habitat-tape-sketch-2026-08-08.md`
3. `docs/lod-vt-redesign-2026-08-04.md`
4. `docs/rt-tlas-cpu-mirror-redesign-2026-08-07.md`
5. `docs/streammountain-refactor-implementation-2026-08-09.md`
6. `docs/streammountain-refactor-plan-2026-08-09.md`
7. `docs/superpowers/backlog.md`
8. `docs/terrain-nested-sector-lod-2026-08-08.md`
9. `docs/volumetric-sectors-design-2026-08-10.md`
10. `docs/volumetric-sectors-m0-resolutions.md`
11. `MatterEngine3/docs/bake-lab-plan.md`
12. `MatterEngine3/docs/bake-lab.md`
13. `MatterEngine3/docs/event-system.md`
14. `MatterEngine3/docs/part-workbench.md`
15. `MatterEngine3/docs/settle-tick-optimizer.md`
16. `MatterEngine3/docs/volumetric-emission-sampling.md`
17. `MatterEngine3/docs/vulkan-rt-gtex-bake.md`
18. `ROADMAP.md`

### Finalized branch-only documents (41)

1. `origin/claude/sharp-stonebraker-2f20e7:docs/superpowers/plans/2026-07-09-phase-c-explorer-demo.md`
2. `codex/far-field-impostors:docs/superpowers/plans/2026-07-30-dense-asset-lod-ladders.md`
3. `codex/far-field-impostors:docs/superpowers/plans/2026-07-30-far-field-asset-impostors.md`
4. `codex/far-field-impostors:docs/superpowers/plans/2026-07-31-adaptive-voxel-lods-prefab-issues.md`
5. `codex/far-field-impostors:docs/superpowers/plans/2026-08-01-object-instanced-terminal-impostors.md`
6. `codex/far-field-impostors:docs/superpowers/plans/2026-08-02-editor-lod-wireframe-debug.md`
7. `codex/far-field-impostors:docs/superpowers/plans/2026-08-02-one-sided-adaptive-lod-ratio.md`
8. `codex/far-field-impostors:docs/superpowers/plans/2026-08-03-lod-representation-repair.md`
9. `codex/far-field-impostors:docs/superpowers/plans/2026-08-03-object-impostor-lod-selection.md`
10. `codex/river-hydrology:docs/superpowers/plans/2026-08-12-river-hydrology-implementation-master.md`
11. `codex/river-hydrology:docs/superpowers/plans/2026-08-12-river-hydrology-integration.md`
12. `codex/river-hydrology:docs/superpowers/plans/2026-08-12-river-hydrology-path-dsl.md`
13. `codex/river-hydrology:docs/superpowers/plans/2026-08-12-river-hydrology-phase-0.md`
14. `codex/river-hydrology:docs/superpowers/plans/2026-08-12-river-hydrology-rendering.md`
15. `codex/river-hydrology:docs/superpowers/plans/2026-08-12-river-hydrology-runtime-streaming.md`
16. `codex/river-hydrology:docs/superpowers/plans/2026-08-12-river-hydrology-solver-artifact.md`
17. `codex/river-hydrology:docs/superpowers/plans/2026-08-12-river-hydrology-terrain-domain.md`
18. `codex/river-hydrology:docs/superpowers/plans/2026-08-13-free-surface-topology-diagnostics-plan.md`
19. `codex/river-hydrology:docs/superpowers/plans/2026-08-13-gameplay-lbm-damping.md`
20. `codex/river-hydrology:docs/superpowers/plans/2026-08-13-hydrostatic-pressure-residual-plan.md`
21. `codex/river-hydrology:docs/superpowers/plans/2026-08-13-hydrostatic-solver-diagnostics.md`
22. `codex/river-hydrology:docs/superpowers/plans/2026-08-17-gameplay-lbm-stabilization.md`
23. `codex/river-hydrology:docs/superpowers/plans/2026-08-18-gameplay-driven-flow.md`
24. `codex/river-hydrology:docs/superpowers/plans/2026-08-20-gameplay-first-river-drive.md`
25. `codex/river-hydrology:docs/superpowers/plans/2026-08-20-visual-river-test-world.md`
26. `codex/river-hydrology:docs/superpowers/plans/2026-08-21-staged-river-upstream-section.md`
27. `origin/claude/sharp-stonebraker-2f20e7:docs/superpowers/specs/2026-07-08-phase-c-explorer-demo-design.md`
28. `origin/claude/sharp-stonebraker-2f20e7:docs/superpowers/specs/2026-07-18-sector-streaming-explorer-consolidation-design.md`
29. `claude/decal-ground-compositing-spec:docs/superpowers/specs/2026-08-01-decal-ground-compositing-design.md`
30. `codex/river-hydrology:docs/superpowers/specs/2026-08-10-river-hydrology-design.md`
31. `codex/river-hydrology:docs/superpowers/specs/2026-08-13-free-surface-topology-diagnostics-design.md`
32. `codex/river-hydrology:docs/superpowers/specs/2026-08-13-gameplay-lbm-damping-design.md`
33. `codex/river-hydrology:docs/superpowers/specs/2026-08-13-hydrostatic-pressure-residual-design.md`
34. `codex/river-hydrology:docs/superpowers/specs/2026-08-13-hydrostatic-solver-diagnostics-design.md`
35. `feature/character-controller:docs/superpowers/specs/2026-08-15-character-controller-design.md`
36. `codex/river-hydrology:docs/superpowers/specs/2026-08-17-gameplay-lbm-stabilization-design.md`
37. `codex/river-hydrology:docs/superpowers/specs/2026-08-18-gameplay-driven-flow-design.md`
38. `codex/river-hydrology:docs/superpowers/specs/2026-08-20-gameplay-first-river-drive-design.md`
39. `codex/river-hydrology:docs/superpowers/specs/2026-08-20-visual-river-test-world-design.md`
40. `codex/river-hydrology:docs/superpowers/specs/2026-08-21-staged-river-upstream-section-design.md`
41. `codex/river-hydrology:docs/superpowers/specs/visual-river-backlog.md`
