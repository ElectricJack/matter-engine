# Findings from the repo-wide docstring pass, 2026-08-19

The documentation pass over all 482 non-test sources changed **only comments**
(proved by stripping every comment and diffing against HEAD). But reading all
167,906 lines closely surfaced a pile of real defects and stale claims. None of
them were fixed — this file is the backlog.

Ordered by how much they can hurt. Line numbers are as of `e7c19aae`.

---

## 1. Wrong behaviour a user could hit

### Authored `ConvexHullCollider` data is silently discarded
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

### One bad skin binding kills the whole frame's skinning
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

### `TLAS(blas, 0)` dereferences null
`libs/SpatialQueryLib/src/bvh.cpp:536`

`MALLOC64(0)` returns 0, so `tlasNode` is null, and the constructor's own
`Build()` immediately writes `tlasNode[0]` in the `blasCount == 0` branch.
Masked today only because `tlas_manager.cpp` guards with
`if (!instance_ptrs.empty())`.

### `mem::Arena::stats()` / `mem::Pool::stats()` return uninitialized memory
`libs/MemoryLib/include/memory.hpp:42` and `:69`

Both do `MemStats s; mem_*_get_stats(handle, &s); return s;`, and both C getters
early-return *without touching* `out` when the handle is null (moved-from, or a
failed `create`). One-character fix: `MemStats s{};`.

### `divisionPow == 0` divides by zero
`libs/MatterSurfaceLib/src/surface.c:569`

`gridSize = 1 << volume.divisionPow;` then `cellSize = volume.size / (gridSize - 1)`.
Nothing in the file validates `divisionPow`.

---

## 2. Performance work that isn't happening

### The GPU-pick reverse map rebuilds every frame despite its "gate"
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

### Every TLAS is built twice
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
| `libs/MatterSurfaceLib/src/tlas_manager.cpp:36` | `push_matrix` skips at depth ≥ 32 but the matching `pop_matrix` still pops — overflowing the cap discards a *caller's* outer transform |
| `libs/MatterSurfaceLib/src/cluster.cpp` | `rebuild_dirty_cells` caps `sh_query_box` at 4096/cell, `get_cells_in_region` caps `sh_query_radius` at 1000; surplus dropped with no diagnostic |
| `animation_evaluator.cpp` | `forward_clip_root_delta` truncates at `kMaxSegments = 4096`; a partial root delta is indistinguishable from a complete one |
| `world_tracer.cpp:319` | `expand_instance` drops subtrees past depth 8, nothing in `err` |
| `vt_enrich.cpp` | every failure path in `get_or_build_variant` returns `nullptr` and discards the populated `err` — AS-build failures are undiagnosable |
| `libs/AssetStoreLib/src/store_os.cpp:388` | POSIX `stamp_of` uses whole-second `st_mtime` XOR size, so two commits in one second with equal index size are indistinguishable and `reload_index` skips the reload |
| `MatterEditor/src/part_workbench.cpp:57` | `write_file` returns `true` without checking the stream |
| `MatterEngine3/src/props/props_file.cpp:52` | both early `return false` paths leak `path + ".tmp"` |

Also: `MatterEditor/src/main.cpp:5022` and `:5043` close the FIFO fd **twice**
on POSIX.

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
  (`build_chart_rung`) drives it.
- **`libs/MemoryLib/README.md`** — the Consumers section names three projects
  that do not exist in the tree.

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
never incremented), `physics_transform_marker_allocations_for_test`,
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
