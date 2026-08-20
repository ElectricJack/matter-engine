# Findings from the repo-wide docstring pass, 2026-08-19

The documentation pass over all 482 non-test sources changed **only comments**
(proved by stripping every comment and diffing against HEAD). But reading all
167,906 lines closely surfaced a pile of real defects and stale claims. Nothing
was fixed in the documentation commit itself; this file is the backlog, and
items fixed since are marked inline.

Ordered by how much they can hurt. Line numbers are as of `e7c19aae`.

> **Status, 2026-08-19 (later the same day).** A first fix pass landed the items
> marked **[FIXED]** below. One item originally listed here was withdrawn on
> inspection — see "Not a bug after all". Everything unmarked is still open.

## How this file is maintained

This is a **living backlog**, not a report. Entries are annotated in place as
they are resolved; nothing is deleted, so the record of what was believed and
what turned out to be true stays readable.

Mark an entry with one of:

- **[FIXED]** — confirmed and corrected. Say what landed if it is not obvious
  from the entry.
- **[WITHDRAWN]** — the finding was wrong. Do **not** silently delete it: move
  the reasoning into §0 "Not a bug after all" and leave a one-line
  `[WITHDRAWN -- see §0]` stub where the entry was, so a later reader does not
  rediscover it and "fix" working code. §0 exists precisely because that
  already happened once.
- **[DEFERRED]** — real, but correcting it means changing a documented contract
  or an API shape. Record the recommendation; do not act unilaterally.

An unmarked entry is still open. Confirm a finding against the code (and
against any test that pins the current behaviour) before acting on it — these
were written by reading, not by running.

---

## 0. Not a bug after all

**One bad skin binding kills the whole frame's skinning** — withdrawn.

`AnimationSkinBridge::expand()` returning `false` when no LOD carries
`input.part_hash` is deliberate, and
`MatterEngine3/tests/animation_skin_bridge_tests.cpp` pins it:
`test_stale_and_mismatched_bindings_fail_without_torn_work`, case *"part
replacement cannot reuse an old mapping"*. It exists to catch an entity whose
`PartInstance` was swapped while its skin binding still points at the old
asset. The caller's all-or-nothing behaviour is equally deliberate and
documented on `collect_animation_skinning`: `out` is left untouched on failure
so the renderer can never receive a torn subset of a scene generation.

Both halves are intentional, so the wide blast radius is a design tension
(a per-entity authoring error fails the whole frame's skin queue), not a
defect. Changing it means giving `expand()` a three-way result and reporting
per-entity misconfiguration on the bridge error hub — a design change, not a
fix. The `valid_animation_skinned_asset()` cost noted in §2 is still real and
still open.

---

## 1. Wrong behaviour a user could hit

### Authored `ConvexHullCollider` data is silently discarded  **[FIXED]**
`MatterEngine3/src/ecs/scene_registry.cpp:819`

```cpp
case ComponentKind::ConvexHullCollider:
    e.set<physics::ConvexHullCollider>({});
    break;
```

Every sibling collider parses its JSON; this one sets a default-constructed
value and never reads the recipe. `point_count` stays 0, so physics rejects the
entity with `InvalidCollider`. Authoring a hull collider in a scene appears to
work and then does nothing.

Related, same function: `BoxCollider` parses `center` and `halfExtents` but
never `rotation` (`:807`), though the field exists in both the schema table and
the struct. And `ConvexHullCollider` is missing from the `EntitySnapshot`
whitelist in `simulation_control.h:20`, so a hull collider does not survive
Play → Stop even though the other three collider types do.

### One bad skin binding kills the whole frame's skinning  **[WITHDRAWN -- see §0]**
`MatterEngine3/src/render/animation_skin_bridge.cpp:140`

`expand()` returns `emitted`, so a valid asset with no `AnimationSkinnedLod`
matching `input.part_hash` returns `false`. The caller
(`ecs/dynamic_scene_bridge.cpp`) turns that into `accepted = false` and rejects
the entire frame's skin collection. A part-hash mismatch on one entity is
indistinguishable from a stale pose and takes every other skinned entity down
with it.

### Gizmo ignores the parent transform
`MatterEditor/src/gizmo.cpp:184`

`draw_gizmo` composes the object matrix from `LocalTransform` alone, with no
parent chain, then feeds it to a world-space view/projection. For any entity
with a non-identity parent the handle draws in the wrong place and writes the
wrong values. The multi-select fan-out at `:244` calls its delta "world-space";
it is a local-space delta added to each entity's own `LocalTransform`.

Also `:227` — any manipulation writes back translation, rotation *and* scale
(the result arrives as one matrix and is decomposed), so a pure translate drag
rewrites the rotation quaternion and scale with round-tripped values.

### `TLAS(blas, 0)` dereferences null  **[FIXED]**
`libs/SpatialQueryLib/src/bvh.cpp:536`

`MALLOC64(0)` returns 0, so `tlasNode` is null, and the constructor's own
`Build()` immediately writes `tlasNode[0]` in the `blasCount == 0` branch.
Masked today only because `tlas_manager.cpp` guards with
`if (!instance_ptrs.empty())`.

### `mem::Arena::stats()` / `mem::Pool::stats()` return uninitialized memory  **[FIXED]**
`libs/MemoryLib/include/memory.hpp:42` and `:69`

Both do `MemStats s; mem_*_get_stats(handle, &s); return s;`, and both C getters
early-return *without touching* `out` when the handle is null (moved-from, or a
failed `create`). One-character fix: `MemStats s{};`.

### `divisionPow == 0` divides by zero  **[FIXED]**
`libs/MatterSurfaceLib/src/surface.c:569`

`gridSize = 1 << volume.divisionPow;` then `cellSize = volume.size / (gridSize - 1)`.
Nothing in the file validates `divisionPow`.

---

## 2. Performance work that isn't happening

### The GPU-pick reverse map rebuilds every frame despite its "gate"  **[FIXED]**
`MatterEngine3/src/matter_engine.cpp:10708`

```cpp
// Gated on the expansion counter so it only rebuilds when instances change.
if (impl_->vk_temporal_instances_expansion == expansion) {
```

The mirror block immediately above (`:10677`) assigns
`vk_temporal_instances_expansion = expansion` on the only path where they
differ. So by the time this test runs the two are *always* equal, and the
O(instances) hash rebuild runs unconditionally. This is the same shape as the
already-known `instance_generation_` gate trap — a change-gate whose predicate
is made true by the code just above it.

### Every TLAS is built twice  **[FIXED]**
`libs/MatterSurfaceLib/src/tlas_manager.cpp:237`

`make_unique<TLAS>(...)` runs `Build()` in the constructor, then `tlas_->Build()`
runs it again. Same shape in `blas_manager.cpp:197` for the
`force_subdiv_one_prim` path.

### `resolve_hash` bypasses the fold cache
`MatterEngine3/src/script_host.cpp:1220`

It calls `module_resolver::fold_sources` directly while every other path
(`bake_source`, `eval_requires`, `eval_lods`, `eval_world`, `eval_tileset`,
`merge_params_canonical`) goes through `fold_sources_cached`. Re-folds the whole
shared-lib set on every resolve.

### `valid_animation_skinned_asset()` is O(all skinned vertices) per entity per frame
`MatterEngine3/src/render/animation_skin_bridge.cpp`

It calls `valid_lod()` per LOD, and each scans every vertex in that LOD's
influence window summing weights. Called once per skinned entity per frame.

### `bounds_for_object` scans all entities per selected object per frame
`MatterEditor/src/selection_bounds.cpp:27`

flecs `each` cannot break, so the `found` flag only short-circuits the body.
Called from both the outline overlay and the pick raycast.

### Dead but paid-for work
- `world_tracer.cpp:187` — `hash_to_first_` is populated per instance per build
  and never read. Its comment claims "for O(1) lookup by hash".
- `warp_field.cpp:1442` — `SolveMesh view` copies `out.positions` and
  `out.indices` per sector solve just so `compute_stats` can see the shape.
- `local_provider.cpp` — `compose_world()` linear-scans `install_to_orig_`
  inside the root loop, O(roots²).

---

## 3. Silent truncation and swallowed failures

| Where | What |
|---|---|
| `libs/MatterSurfaceLib/src/tlas_manager.cpp:36` **[FIXED]** | `push_matrix` skips at depth ≥ 32 but the matching `pop_matrix` still pops — overflowing the cap discards a *caller's* outer transform |
| `libs/MatterSurfaceLib/src/cluster.cpp` | `rebuild_dirty_cells` caps `sh_query_box` at 4096/cell, `get_cells_in_region` caps `sh_query_radius` at 1000; surplus dropped with no diagnostic |
| `animation_evaluator.cpp` | `forward_clip_root_delta` truncates at `kMaxSegments = 4096`; a partial root delta is indistinguishable from a complete one |
| `world_tracer.cpp:319` | `expand_instance` drops subtrees past depth 8, nothing in `err` |
| `vt_enrich.cpp` | every failure path in `get_or_build_variant` returns `nullptr` and discards the populated `err` — AS-build failures are undiagnosable |
| `libs/AssetStoreLib/src/store_os.cpp:388` | POSIX `stamp_of` uses whole-second `st_mtime` XOR size, so two commits in one second with equal index size are indistinguishable and `reload_index` skips the reload |
| `MatterEditor/src/part_workbench.cpp:57` | `write_file` returns `true` without checking the stream |
| `MatterEngine3/src/props/props_file.cpp:52` | both early `return false` paths leak `path + ".tmp"` |

Also: `MatterEditor/src/main.cpp:5022` and `:5043` close the FIFO fd **twice**
on POSIX. **[FIXED]**

---

## 4. Comments that are now false

These matter because they actively mislead. Per the pass's rules none were
rewritten; corrections were added alongside.

- **`matter/engine_context.h:26`** — `EngineContext::create` "Requires a live GL
  context current on this thread" and fails on GL < 4.6. There is no GL check
  at all any more (`matter_engine.cpp:8864`). The GL path was deleted.
- **`matter/world_session.h:362`** — the 4-arg `render()` documents
  "Resolve → cull → clear → draw … Requires a live GL context". Its definition
  (`matter_engine.cpp:11256`) is an empty stub that draws nothing.
- **`libs/MatterSurfaceLib/src/material_registry.c:22`,
  `include/material_registry.h:108`, `MatterEngine3/src/render/vk_gi_contract.h:122`** —
  all three cite `MaterialRegistrySetGroundMacroSlot()` as the runtime override.
  That function was deleted by `e7c19aae` *because it had no callers*, so the
  macro-slot override has no way in. (The feature was already unreachable; the
  commit removed the corpse, it did not cause the problem.)
- **`libs/ProfileLib/include/profile.h`** — `dump_chrome_trace` says lanes are
  split "(by zone-name prefix)". That name-prefix heuristic is exactly what the
  real per-thread lane model replaced.
- **`vt_compositor.h:145`** — `invalidate_part` says "Device must be idle w.r.t.
  fills." The implementation was deliberately changed to park entries in the
  retire ring *so it can run on the render thread with frames in flight* — that
  change is what fixed `VUID-vkFreeDescriptorSets-pDescriptorSets-00309` /
  `VK_ERROR_DEVICE_LOST`.
- **`refine_controller.cpp:72`** — "a map keyed by (tx*65536+tz) so pairs stay
  insertion-ordered". The key is `((uint64_t)tx << 32) | tz`, and `std::map`
  iterates in key order. Both claims wrong.
- **`mesh_indexed.hpp`** — `WeldOptions`' `1e-4` default "matches
  mesh_simplifier's existing internal weld". That weld is on a `1e-5` grid.
- **`oriented_cube_algorithm.h`** — documents `MSL_CUBE_SIZE_SCALE` default as
  1.0; the implementation uses `0.6f`.
- **`mesh_simplifier.hpp`** — "Internally converts to raylib::Mesh…". That
  round-trip was removed to escape the 65535-vertex cap.
- **`tileset_bake.cpp` ~600** — documents `magic = 'CSTL'`, `version = 1`; the
  constants below are `'STLC'` and `2`.
- **`terrain_field.cpp:261`** — error text says "too many ops (max 64)"; the cap
  is 96.
- **`vk_scene_renderer.h:202`** — "There is no impostor system on this base".
  There is (M2.5), declared in the same header.
- **`sector_streamer.h`** — "the packed variant carries the four-bit
  coarser-neighbor edge mask", contradicted by the same file's "THE EDGE MASK IS
  GONE FROM THIS ENCODING".
- **`CLAUDE.md` item 8** — "libs/MeshChartingLib … No consumers today".
  `MatterEngine3/Makefile:213` compiles it and `lod_bake.cpp:361`
  (`build_chart_rung`) drives it. **[FIXED]** — item 8 now names the real
  consumers and the `build_chart_rung` call chain.
- **`libs/MemoryLib/README.md`** — the Consumers section names three projects
  that do not exist in the tree. **[ALREADY FIXED]** — the Consumers section in
  the tree today lists the real `mem_pool.c` / `mem_arena.c` consumers and no
  longer claims MatterSurfaceLib keeps a vendored copy.
- **`libs/MatterSurfaceLib/README.md` was a verbatim copy of
  `Prototypes/GPURayTraceExample`'s** — titled "GPU Ray Tracing Example",
  documenting `build.bat` / `run.ps1` / `platform-status.sh` for an app
  (`main.cpp` + `bvh_visualizer`) Phase 5a deleted, and listing an
  "ObjectAllocator (copied from ObjectAllocatorLib)" dependency that
  contradicts CLAUDE.md's no-copies rule. **[FIXED]** — rewritten to describe
  the library, the `shaders`/`regen-shaders` targets that are the Makefile's
  only live purpose, and the per-suite test targets.
- **`libs/MeshChartingLib/README.md`** predated the 2026-07-29 WP-A additions
  (no `pack_charts_paged`, `chart_average_normals`, `projection_distortion` or
  the 32-bit index overloads). **[FIXED]**
- **`MatterEditor/README.md`** — "Reuses, unmodified: the same `APP_SRC` /
  `WIN_ME3_CPP` / `WIN_MSL_CPP` / `WIN_PIPELINE_C` … source lists". The three
  `WIN_*` engine lists were deleted; both targets link an archive
  MatterEngine3's `viewer-lib` target builds. The build command also still
  carried the `TMP=`/`TEMP=` prefix `platform.mk` made unnecessary. **[FIXED]**
- **`CLAUDE.md` item 7** — "Dependencies: MatterEngine3 (libmatter_engine3.a)"
  and "`make -C MatterEditor` → `build/linux/editor`". `editor.exe` links
  `libmatter_engine3_viewer.a`, and `.DEFAULT_GOAL := windows`, so a bare
  `make -C MatterEditor` builds the Windows exe. **[FIXED]**

Misplaced (not wrong, just attached to the wrong declaration):
`dsl_bindings.cpp:1176` (`__habitatAt` docs sit above `j_hasHabitat`),
`part_store.h` (two blocks describing `LoadedPart` and `PartStore` sit above
`SurfaceClassCache` and `WarpAnchor`), `seam_weld.h`, `lod_bake.cpp:112`,
`material_registry.h:10`, `issue_reporter.h:76` (relocated during this pass —
the one comment move made).

---

## 5. Dead code

Declared-but-never-defined (link error if called), all SpatialQueryLib:
`BVH::Refit`, `TLAS::FindBestMatch`, `BvhMesh::BvhMesh(const char*, const char*, float)`,
`BVHAnalyzer::GeneratePerformanceReport`, `BVHAnalyzer::CompareBVHTrees`,
`BVHAnalyzer::GenerateOptimizationRecommendations`,
`BVHReportManager::GenerateSummaryReport`.

Never read: `BVH::buildStack` (~1 KB per BVH), `RefTable::Impl::dirty`,
`TLASManager::instance_array_`, `vk_volumetrics` `ping_index_`,
`Selection::world_generation`, `ProdGraphResolver::schemas_dir_`,
`part_graph.h:124 struct ResolvedNode`, `tlas_manager.hpp LegacyBVHInstance`,
`surface.c:224 IsosurfaceVertex`, `resolve_cache.cpp:239 fold_u32`,
`selection_outline.cpp:87 draw_obb_wireframe`, `local_provider.cpp` `Rng64` and
`identity_transform()`, `resolvers.cpp to_resolved()`.

Never produced: `Status::Locked` (AssetStoreLib), `LoadFailure::Open`
(impostor_bake), `SectorStreamingState::Detaching`.

Always zero: `TLASAnalysis::avg_instance_triangles` (`total_blas_triangles`
never incremented) **[FIXED]**, `physics_transform_marker_allocations_for_test`,
`SceneSnapshot::generation`, `world_flatten.h FlatInstance::stable_id`.

Inert: `TLASManager::print_stats()` (entire body commented out),
`BVHAnalyzer::GenerateReport` emits literal `\n` so reports are one line,
`voxel_imposter` `coverThresh` (unused but still in the cache hash, so changing
it invalidates caches without changing output), `MeshGenerationConfig` /
`GetDefaultMeshConfig()` (no function in that header takes one).

---

## 6. Sharp edges worth knowing (documented in place, not defects)

- **`mc_tables.h` defines non-static globals in a header.** Works only because
  exactly one TU includes it; a second includer is a duplicate-symbol link error
  the include guard cannot prevent.
- **`animation_ir.cpp` `encode()` builds `|`-delimited records from unescaped
  authored names.** A joint/socket name containing `|` or `\n` produces an
  encoding ambiguous with a different rig — a determinism-hash collision.
- **`animation_runtime_asset.cpp` `compile_controller` assigns left vs. right
  foot purely by the order targets appear.** Reordering authored targets
  silently swaps the feet.
- **`evaluate_dlss` failure is sticky and session-wide** — one mismatch clears
  `dlss_available_` permanently, dropping the whole session to native.
- **`physics_ray_cast`'s third parameter is named `translation`** in the
  definition but reads as a direction at the unnamed declaration. A caller
  passing a normalized direction casts exactly one metre.
- **`OzzSkeleton::subtree()` returns the "whole skeleton" sentinel for an
  out-of-range joint** — piping that into `local_to_model()` silently widens the
  operation instead of failing.
- **`vk_lighting_controls.cpp`** calls `sanitize_vulkan_lighting_overrides({1,1,1,exposure_ev})`,
  relying on `exposure_ev` being the 4th declared member. Reordering the struct
  puts the EV into `emission_multiplier`.
- **`SpatialQueryLib` capacity ceilings**: a hit packs instance and primitive
  into one 32-bit word (12 + 20 bits) — 2^20 triangles/mesh, 2^12
  instances/TLAS, wrapping silently past either.
- **UI**: `ui.cpp` ~893 prints `vt_pool_bytes` twice as "used / capacity", so
  capacity always equals used. `properties_panel.cpp:404` truncates the 64-bit
  part hash through a `uint32_t` accessor. Several `properties_panel` buttons
  are drawn live but their callbacks are stubs, so clicks are discarded.

---

## 7. Convention drift

`printf`/`fprintf` instead of `MATTER_LOG*` (the repo's single logging
facility): all of `libs/MatterSurfaceLib` (`blas_manager`, `cluster`, `cell`,
`mesh_build_utils`, `mesh_worker_pool`), `part_store.cpp`,
`retopo_blacklist.cpp`, `vk_volumetrics.cpp`, `scene_model_adapter.cpp`. For
MatterSurfaceLib this is arguably deliberate (it does not depend on
`matter/log.h`); for the `MatterEngine3` and `MatterEditor` files it is not, and
those messages never reach the editor Console.

Headers relying on transitive includes — would break on a header reshuffle:
`mesh_indexed.cpp` (`std::llround`, no `<cmath>`), `tileset_phase.cpp`
(`std::sort`, no `<algorithm>`), `profiler.hpp` (`printf`, `std::sort`),
`vk_resources.cpp` Linux branch (`fopen`/`fscanf`, no `<cstdio>`).

Row-major vs column-major matrices meeting in one codebase:
`selection_bounds.h`'s `world_matrix` is row-major while the private `Mat4` in
`selection_outline.cpp` is column-major; same split between
`animation_debug_overlay.cpp`'s local `Mat4` and `matter::Mat4f`. Neither pair
meets today. Both are now labelled.

---

## 8. Test-suite triage, 2026-08-19

A sweep of `MatterEngine3/tests/`, `MatterEditor/tests/`, `libs/*/tests/` and
`libs/*/main.c` for suites that no longer apply.

### Build-system defects (fixed)

- **`run-physicsevents` did not link.** `PHYSICSEVENTS_CPP` listed
  `../src/ecs/ecs_runtime.cpp` without the animation closure that TU
  constructs, so the target died on `AnimationSystems::set_presentation_delta_seconds`,
  `AnimationEvaluator`'s ctor/dtor, `PoseLodScheduler`'s ctor and the vtable for
  `Box3DAnimationWorldQueries`. Every sibling that links `ecs_runtime.cpp`
  (`ECS_CPP`, `PHYSICS_CPP`, `SCENE_REGISTRY_CPP`, `PROPERTIES_REGISTRY_CPP`,
  `SIMULATION_CONTROL_CPP`, `ECS_ENTITY_BRIDGE_CPP`, and `DYNAMIC_BRIDGE_CPP`
  by listing the same TUs by hand) already carries
  `$(ANIMATION_ECS_RUNTIME_REQUIRED)`; this one did not. **[FIXED]** — added
  that list plus `$(OZZ_OFFLINE_LIBS)` to the link line and prerequisites.
  No other target has the same gap.
- **A literal `\n` inside `def_CPP_SRCS`.** The `$(SUNANGLES_CPP)`/
  `$(PROPS_CPP)`/`$(CLOUDLAYER_CPP)` line carried the two characters `\` `n`
  where a line continuation belonged, so the word `\n` entered the source
  union and had a compile rule generated for it. Harmless only because
  `def_CPP_OBJS` feeds nothing but the unused `ALL_OBJS`. **[FIXED]** — exactly
  the `sed`-over-heredoc hazard the fix contract warns about.
- **Stale `stressforest` mentions.** `stress_forest_tests.cpp` is gone from the
  tree and has no target; two comment blocks in `MatterEngine3/tests/Makefile`
  still listed it in the sh-flavor family. **[FIXED]** — comments corrected.
  Nothing else references the file.

### A third pre-existing red suite (owner: atmosphere/volumetrics)

`run-cloud-layers` fails 1 check, and has since 2026-08-10. The contract's
Rule 6 lists two known-red suites; this is a third.

`cloud_layer_tests.cpp`'s `test_task9_shared_density_and_optional_r16f_contract`
ends by reading `../tools/atmosphere_cloud_shots.sh` and asserting on two
strings inside it. Commit `ac1c04dc` ("Remove helper script that's not needed
any longer") deleted that script, so the `ifstream` yields an empty string and
`"Task 9 capture gates same-process static repeat while retaining Task7 as a
diagnostic"` can never pass. The other two CHECKs in the same function read
files that still exist and pass.

The assertion is obsolete: it pins the contents of a deleted harness, not the
behaviour of any shipped code. Deleting that third `CHECK` (and the
`harness_file`/`harness` locals feeding only it) turns the suite green without
losing coverage of anything that exists. Left for the owning area — it is a
test-source change, not a Makefile one.

### Orphans — deliberate, leave them

- `MatterEngine3/tests/channels_at_bench.cpp` and `scatter_grid_bench.cpp` have
  no Makefile target **on purpose**: both are one-off measurement harnesses
  whose file headers carry their own `g++` command line, and
  `MatterEngine3/src/scatter_grid_native.h` cites the second as the source of
  its documented cost shape. Not dead code.
- `libs/MatterSurfaceLib/tests/simp_perf_probe.cpp` is the same shape — a
  hand-run benchmark, not a suite.

### Orphans — genuinely unwired (owner: MatterSurfaceLib)

- `libs/MatterSurfaceLib/tests/cell_tests.cpp` and `simple_cell_tests.cpp` are
  referenced by no target in `libs/MatterSurfaceLib/tests/Makefile`. They are
  older, larger siblings of `cell_bounds_tests.cpp` (`run-cell`) and of the
  `minimal_cell_test.cpp` the default `run` target builds, and they still
  `#include "raylib.h"` directly. Either wire one of them up or delete both;
  not touched here because that Makefile and those sources belong to another
  area.

### Not found (checked, all clean)

- **No dangling targets.** Every source path named in
  `MatterEngine3/tests/Makefile`, `MatterEditor/Makefile`'s test rules and each
  `libs/*/tests/Makefile` exists. `gpu_cull_tests.cpp` and
  `release_part_tests.cpp` survive only inside comments recording their
  retirement.
- **No stale expected-test-counts.** Every suite prints a runtime-computed
  `passed/total`; none hardcodes a total.
- **No live raylib/GL or HZB test paths.** `api_tests.cpp:110` and
  `world_stream_tests.cpp:226` still call `BeginDrawing()`, but both assertions
  on render output are already `#ifndef MATTER_VULKAN_ONLY`-guarded with a
  comment explaining that `WorldSession::render()` is the no-op stub. The
  remaining `GpuCuller`/`RasterComposer` mentions in `viewer_logic_tests.cpp`,
  `refine_loop_tests.cpp` and `shader_source_tests.cpp` are comments recording
  deletions. Nothing in any suite references the removed HZB.
- **`MatterEditor/tests/` has no Makefile**; its eight `test_*.cpp` are wired
  from `MatterEditor/Makefile` and all eight are present and reachable.

### Missing

- **`MatterEngine3/README.md` does not exist**, though CLAUDE.md's "Project
  Structure" section says each sub-project has one and `MatterEditor/README.md`
  does. Not authored here.
