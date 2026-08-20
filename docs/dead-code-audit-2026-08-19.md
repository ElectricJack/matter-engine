# Dead-code audit — 2026-08-19

Whole-program reachability analysis of `MatterEditor/build/windows/editor.exe`
(489 objects: 47 editor TUs, 162 `libmatter_engine3_viewer.a` members, and the
vendored archives). Method and tooling: `.claude/skills/dead-code/`.

## Method, and why the linker could not answer

Every project compiles `-ffunction-sections -fdata-sections` and links
`-Wl,--gc-sections`, so `-Wl,--print-gc-sections` ought to print the dead
functions directly. **It does not work on this toolchain.** Verified with a
planted unreferenced function: mingw-w64 GCC 16.1 / GNU ld collects `.data$`
and `.rdata$` sections but never `.text$` sections, so the dead function
linked into the binary and was never reported. A probe relink of the real
editor produced a file byte-for-byte the same size as the shipping exe, and
of 5113 discarded sections **zero** were first-party `.text`.

> Note: the `--whole-archive` comment block in `MatterEditor/Makefile` states
> that `--gc-sections` "still prunes genuinely unreachable code afterwards".
> On the Windows target it prunes data only, not code. The conclusion drawn
> there (that the exe is the same size) is consistent with pruning nothing.

The analysis was therefore rebuilt from `objdump`: `-t` maps every symbol to
its own `.text$<mangled>` section, `-r` gives each section's outgoing
references, and a mark-and-sweep runs from the real roots (entry point,
`_GLOBAL__sub_I*` static initialisers, `.ctors`/`.CRT$*`, non-empty catch-all
sections). That is precisely what `--gc-sections` does on ELF.

Results are deduped for COMDAT (an inline/template symbol is dead only when
every definition is unreachable) and filtered to code the team authored —
235 of the 807 unreachable symbols are `std::`/`flecs::` template
instantiations or vendored `stb_image*` code textually included into
`tileset_gtex.cpp`, which are compiler output rather than deletable source.

Every run asserts that no unreachable symbol has a reachable caller; that
check passes here. The tool is also validated against a fixture with one
known-dead and one known-live function before being trusted.

## Result

**572 authored functions (186.8 KB of code) are unreachable from the editor's
entry point.**

| shape | referenced elsewhere | functions | bytes |
|---|---|---:|---:|
| `orphan` | tests | 331 | 94772 |
| `dead-cluster` | none | 48 | 32627 |
| `orphan` | none | 121 | 24676 |
| `dead-cluster` | tests | 43 | 23403 |
| `orphan` | prototypes | 19 | 8946 |
| `dead-cluster` | prototypes | 9 | 6839 |
| `orphan` | script | 1 | 70 |

- **`orphan`** — nothing in the entire link references it.
- **`dead-cluster`** — referenced only by other dead code; delete as a group.
- **elsewhere** — whether the identifier also occurs under `tests/`,
  `prototypes/` (retired, excluded from `build-all.sh`), or in `.js` files.

### The three groups, in order of what to do about them

1. **Dead everywhere — 169 functions, 56.0 KB, 55 files.** Not reachable from
   the editor and the identifier appears nowhere else in the repo (tests
   included). These are the deletion candidates.
2. **Product-dead, test-alive — 374 functions, 115.4 KB.** Only the test
   suites exercise them. Not deletable without deleting tests; worth knowing
   how much API surface exists solely to be tested.
3. **Prototype-only — 28 functions, 15.4 KB.** Reachable only from
   `prototypes/`, which is frozen and not built by `build-all.sh`.

## Outcome — what was actually deleted, and an important correction

**41 functions / 585 lines across 40 files were removed.** `editor.exe` went
from 17,504,256 to 17,470,976 bytes. Editor, `vulkan_smoke_tests.exe`, the
three edited libraries and 14 engine test suites all build and pass; the editor
was run headlessly against `FloorDemo` and renders correctly.

That is far fewer than the 169 "dead everywhere" reported below, and the gap is
the most useful result of this audit:

**Unreachable is not the same as uncalled.** At `-O2`, a function whose every
call site was *inlined* still emits an out-of-line copy that nothing
references. That copy is genuinely dead machine code — but the source function
is live. The first deletion pass removed 92 such candidates and the linker
rejected 36 of them outright. So the 572 figure below should be read as
*"unreachable compiled bodies"*, not *"deletable source functions"*.

Three further false-positive classes surfaced, each caught by a build:

- **`assert()` under `NDEBUG`.** The editor compiles `-DNDEBUG`, so a call that
  appears only inside `assert(...)` leaves no relocation edge.
  `skinned_rt_uses_bind_pose_blas()` looked dead for exactly this reason and
  broke `vulkan_smoke_tests.exe`, which builds with asserts enabled.
- **Transitive test use.** The `elsewhere` column is name-based: it only sees
  functions a test names *directly*. If a test calls Y and Y calls X, X is
  exercised by the suite even though its identifier never appears under
  `tests/`. Propagating reachability through the call graph moved 38 of the 169
  out of the deletable set before any code was touched.
- **A tooling bug in this very report.** `bare_name` did not strip the
  `[abi:cxx11]` ABI tag, so every `std::string`-returning function failed the
  identifier check and was silently dropped from the cross-reference — landing
  in the "referenced nowhere" bucket by default.
  `VkSceneRenderer::cloud_shadow_allocation_error()` is called by
  `vulkan_smoke_tests.cpp:6399` and was only caught by the declaration sweep.
  Fixed in `.claude/skills/dead-code/dead_code.py`.

The safe procedure, now written up in the skill, is: propagate test
reachability through the call graph, close the set under callers, apply a
source-level call-site gate, then iterate with the linker as the oracle,
building both the editor and the smoke-test binary.

Special members were left alone: unreachable destructors and `operator=` are
usually implicit or defaulted, so there is nothing to delete and removing a
user-declared one changes the type's semantics.

### Deleted

`ConvertMeshToBVHTriangles`, `FreeBVHTriangles`, `GenerateFullReport`,
`GenerateMeshWithConfig`, `GetRegisteredNames`,
`MaterialRegistrySetGroundMacroSlot`, `MemRealloc`, `SurfaceLibCleanup`,
`UpdateAllAnalyses`, `accept_transformed`, `active_scope_open`,
`blacklist_size`, `clear_all_cells`, `clear_frame`, `clear_particles`,
`count_awake`, `disk_path`, `expanded_instance_by_hash`,
`force_rebuild_all_cells`, `get_diagonal_length`, `get_mesh_worker_count`,
`lights_fingerprint`, `preserve_after_unproven_external_work`,
`remove_particle`, `remove_particle_index`, `root_instance_count`,
`root_instance_info`, `set_ao_baker`,
`set_fail_next_froxel_bundle_creation_for_test`, `set_mesh_worker_count`,
`set_no_mesh_cells`, `set_symbol`, `sh_query_point`, `source_expansion_count`,
`update_emitters`, `update_particle_position`, `viewer_atmosphere_status`,
`viewer_session_status`, `visit_all_cells`, `visit_cells`, `world_bounds`,
`world_to_local`

The bulk is a self-contained island: `Cluster` lost 12 methods and `Cell` 3
(the particle-editing and cell-visiting API nothing calls any more), the
`BVHReportManager` / `BVHAnalyzer` reporting path is gone, and `surface.h`
shed four legacy C entry points.

## Group 1 — dead everywhere

Nothing in the repo references these. Spot-check with `grep -rn -w <name>` (excluding `.worktrees/`) before deleting.

### `libs/SpatialQueryLib/src/bvh_analyzer.cpp` — 15 functions, 7337 B

- `BVHAnalyzer::GenerateQualityAssessment(BVHTreeAnalysis&)` — 2023 B, dead-cluster
- `BVHTreeAnalysis::BVHTreeAnalysis(BVHTreeAnalysis const&)` — 1351 B, dead-cluster
- `BVHAnalyzer::AnalyzeNodeRecursive(BVH const*, unsigned int, unsigned int, BVHTreeAnalysis&, std::vector<unsigned int, std::allocator<unsigned int> >&)` — 1335 B, dead-cluster
- `BVHReportManager::GenerateFullReport[abi:cxx11]()` — 1063 B, orphan
- `BVHReportManager::GetRegisteredNames[abi:cxx11]()` — 542 B, orphan
- `TLASAnalysis::~TLASAnalysis()` — 256 B, dead-cluster
- `BVHAnalyzer::AnalyzeTLASNodeRecursive(TLAS const*, unsigned int, unsigned int, TLASAnalysis&)` — 210 B, dead-cluster
- `BVHReportManager::UpdateAllAnalyses()` — 136 B, dead-cluster
- `BVHAnalyzer::CalculateQualityScore(BVHTreeAnalysis const&)` — 110 B, orphan
- `BVHAnalyzer::CalculateTreeEfficiency(BVHTreeAnalysis const&)` — 65 B, orphan
- `BVHAnalyzer::CalculateNodeSurfaceArea(BVHNode const&)` — 57 B, orphan
- `BVHAnalyzer::CalculateTLASNodeSurfaceArea(TLASNode const&)` — 57 B, orphan
- `BVHAnalyzer::CalculateBalanceFactor(BVHTreeAnalysis const&)` — 53 B, orphan
- `BVHAnalyzer::EstimateTraversalCost(BVHTreeAnalysis const&)` — 48 B, orphan
- `BVHAnalyzer::GetTimeMs()` — 31 B, orphan

### `libs/MatterSurfaceLib/src/cell.cpp` — 6 functions, 5874 B

- `Cell::commit_group_mesh(GroupMeshResult&, BLASManager&)` — 5223 B, dead-cluster
- `Cell::remove_particle_index(unsigned int, unsigned int)` — 422 B, orphan
- `Cell::commit_cell_meshes(CellMeshResult&, BLASManager&)` — 79 B, dead-cluster
- `Cell::clear_particle_indices()` — 76 B, dead-cluster
- `Cell::get_diagonal_length() const` — 55 B, orphan
- `Cell::accept_transformed(CellRenderVisitor&, mm::Mat4 const&) const` — 19 B, dead-cluster

### `MatterEngine3/src/animation/animation_ir.cpp` — 1 functions, 5251 B

- `matter::animation::CanonicalAnimationBuild::encode[abi:cxx11]() const` — 5251 B, orphan

### `MatterEngine3/src/world_tracer.cpp` — 6 functions, 4857 B

- `world_tracer::WorldTracer::Impl::traverse_ibvh(int, std::vector<int, std::allocator<int> > const&, float const*, float const*, float const*, float&, float*, int&, int&) const` — 2661 B, dead-cluster
- `world_tracer::WorldTracer::Impl::intersect_instance((anonymous namespace)::ExpandedInst const&, float const*, float const*, float&, float*, int&) const [clone .isra.0]` — 1514 B, dead-cluster
- `world_tracer::WorldTracer::expanded_instance_by_hash(unsigned long long, unsigned long long&, float*) const` — 281 B, orphan
- `(anonymous namespace)::aabb_hit(float const*, float const*, float const*, float const*, float)` — 212 B, dead-cluster
- `world_tracer::WorldTracer::world_bounds(float*, float*) const` — 146 B, orphan
- `world_tracer::WorldTracer::expanded_instance_count() const` — 43 B, dead-cluster

### `MatterEngine3/src/ecs/physics_context.cpp` — 8 functions, 4085 B

- `matter::physics::detail::PhysicsContext::enqueue_wake(ecs_world_t const*, unsigned long long)` — 1199 B, dead-cluster
- `matter::physics::(anonymous namespace)::resolve_command_target(flecs::entity, matter::physics::(anonymous namespace)::CommandTarget&) [clone .constprop.0]` — 538 B, dead-cluster
- `matter::physics::detail::(anonymous namespace)::can_enqueue_command(std::unordered_map<unsigned long long, std::unique_ptr<matter::physics::detail::(anonymous namespace)::BridgeRecord, std::default_delete<matter::physics::detail::(anonymous namespace)::BridgeRecord> >, std::hash<unsigned long long>, std::equal_to<unsigned long long>, std::allocator<std::pair<unsigned long long const, std::unique_ptr<matter::physics::detail::(anonymous namespace)::BridgeRecord, std::default_delete<matter::physics::detail::(anonymous namespace)::BridgeRecord> > > > > const&, matter::physics::detail::PhysicsContext const*, ecs_world_t const*, unsigned long long)` — 518 B, dead-cluster
- `matter::physics::detail::PhysicsContext::enqueue_force(ecs_world_t const*, unsigned long long, matter::Float3)` — 515 B, dead-cluster
- `matter::physics::detail::PhysicsContext::enqueue_impulse(ecs_world_t const*, unsigned long long, matter::Float3)` — 515 B, dead-cluster
- `matter::physics::detail::PhysicsContext::enqueue_teleport(ecs_world_t const*, unsigned long long, matter::Float3, matter::Quaternion)` — 482 B, dead-cluster
- `matter::physics::detail::(anonymous namespace)::indexed_overlap_callback(int, unsigned long long, void*)` — 292 B, dead-cluster
- `matter::physics::detail::PhysicsContext::world_is_valid() const` — 26 B, orphan

### `libs/MatterSurfaceLib/src/cluster.cpp` — 16 functions, 3964 B

- `Cluster::set_no_mesh_cells(std::vector<mm::Vec3, std::allocator<mm::Vec3> > const&)` — 1094 B, orphan
- `Cluster::find_or_create_cell(mm::Vec3 const&)` — 549 B, dead-cluster
- `Cluster::world_to_local(mm::Vec3 const&) const` — 499 B, orphan
- `Cluster::~Cluster()` — 411 B, orphan
- `Cluster::remove_particle(unsigned int)` — 333 B, orphan
- `Cluster::visit_cells(CellRenderVisitor&) const` — 211 B, orphan
- `Cluster::add_to_tlas() const` — 207 B, dead-cluster
- `Cluster::clear_all_cells()` — 184 B, dead-cluster
- `Cluster::update_particle_position(unsigned int, mm::Vec3 const&)` — 120 B, orphan
- `Cluster::force_rebuild_all_cells()` — 104 B, orphan
- `Cluster::compute_finest_detail() const` — 103 B, orphan
- `Cluster::visit_all_cells(CellVisitor&) const` — 60 B, orphan
- `Cluster::set_ao_baker(Occupancy const*, AoGrid, AoParams)` — 27 B, orphan
- `Cluster::get_mesh_worker_count() const` — 23 B, orphan
- `Cluster::clear_particles()` — 22 B, orphan
- `Cluster::set_mesh_worker_count(int)` — 17 B, orphan

### `MatterEngine3/src/part_cluster.cpp` — 2 functions, 2789 B

- `void part_cluster::(anonymous namespace)::split_recursive_generic<part_cluster::split_clusters(std::vector<Tri, std::allocator<Tri> >&, std::vector<TriEx, std::allocator<TriEx> >&, unsigned int)::{lambda(unsigned int)#1}&>(std::vector<unsigned int, std::allocator<unsigned int> >&, unsigned int, unsigned int, unsigned int, unsigned int, part_cluster::split_clusters(std::vector<Tri, std::allocator<Tri> >&, std::vector<TriEx, std::allocator<TriEx> >&, unsigned int)::{lambda(unsigned int)#1}&, std::vector<part_cluster::Cluster, std::allocator<part_cluster::Cluster> >&)` — 2604 B, dead-cluster
- `void part_cluster::(anonymous namespace)::centroid_aabb<part_cluster::split_clusters(std::vector<Tri, std::allocator<Tri> >&, std::vector<TriEx, std::allocator<TriEx> >&, unsigned int)::{lambda(unsigned int)#1}&>(std::vector<unsigned int, std::allocator<unsigned int> > const&, unsigned int, unsigned int, part_cluster::split_clusters(std::vector<Tri, std::allocator<Tri> >&, std::vector<TriEx, std::allocator<TriEx> >&, unsigned int)::{lambda(unsigned int)#1}&, float*, float*) [clone .isra.0]` — 185 B, dead-cluster

### `libs/MatterSurfaceLib/src/particle_culling.cpp` — 4 functions, 2769 B

- `make_sub_particle(Lattice const&, SlotCoord, int, int, int, int, SlotData const&, CullParams const&)` — 1769 B, dead-cluster
- `lattice_vnoise(float, float, float)` — 780 B, dead-cluster
- `cell_coord_of(Lattice const&, SlotCoord, CullParams const&)` — 153 B, dead-cluster
- `lattice_vhash(int, int, int)` — 67 B, orphan

### `MatterEngine3/src/part_asset_v2.cpp` — 1 functions, 2537 B

- `chart_atlas::parse_chart_rungs(unsigned char const*, unsigned char const*, std::vector<chart_atlas::ChartAtlasRung, std::allocator<chart_atlas::ChartAtlasRung> >&)` — 2537 B, dead-cluster

### `MatterEngine3/src/world_lights.cpp` — 1 functions, 1502 B

- `world_lights::lights_fingerprint(world_lights::WorldLights const&)` — 1502 B, orphan

### `libs/SpatialQueryLib/src/spatial_hash.c` — 3 functions, 1454 B

- `sh_query_box` — 722 B, dead-cluster
- `sh_query_point` — 384 B, orphan
- `sh_remove` — 348 B, orphan

### `MatterEngine3/src/render/part_store.cpp` — 2 functions, 1402 B

- `viewer::PartStore::disk_path[abi:cxx11](unsigned long long) const` — 1369 B, orphan
- `viewer::walk_part_tree(unsigned long long, std::function<viewer::LoadedPart const* (unsigned long long)> const&, std::function<void (viewer::LoadedPart const*, unsigned long long, float const*, int)> const&)` — 33 B, orphan

### `libs/MatterSurfaceLib/src/surface.c` — 5 functions, 1327 B

- `ConvertMeshToBVHTriangles` — 955 B, orphan
- `GenerateMeshWithConfig` — 312 B, orphan
- `SurfaceLibCleanup` — 37 B, orphan
- `FreeBVHTriangles` — 17 B, orphan
- `GetDefaultMeshConfig` — 6 B, orphan

### `MatterEngine3/src/props/draw_overrides.cpp` — 1 functions, 1247 B

- `matter::DrawOverrideResolver::modules[abi:cxx11]() const` — 1247 B, orphan

### `MatterEngine3/src/render/vk_scene_renderer.cpp` — 11 functions, 980 B

- `viewer::vk_scene_detail::select_scene_cluster_lod(viewer::VkSceneCluster const&, matter::Mat4f const&, matter::Float3, float)` — 283 B, dead-cluster
- `viewer::VkSceneRenderer::cloud_shadow_allocation_error[abi:cxx11]() const` — 145 B, orphan
- `viewer::VkSceneRenderer::write_gpu_timestamp(VkCommandBuffer_T*, unsigned int, bool, viewer::VkSceneRenderer::FrameResources&)` — 121 B, orphan
- `viewer::vk_scene_detail::lod_is_billboard(viewer::VkScenePart const&, viewer::VkSceneLod const&)` — 107 B, orphan
- `viewer::VkSceneRenderer::tileset_channel_view(int, int) const` — 104 B, orphan
- `viewer::VkSceneRenderer::settle_free_ranges()` — 83 B, orphan
- `viewer::VkSceneRenderer::skinned_rt_uses_bind_pose_blas() const` — 39 B, orphan
- `viewer::VkSceneRenderer::write_vt_descriptors_for_frame(viewer::VkSceneRenderer::FrameResources&)` — 33 B, orphan
- `viewer::VkSceneRenderer::vt_record_post_pass(VkCommandBuffer_T*)` — 29 B, orphan
- `viewer::VkSceneRenderer::set_fail_next_froxel_bundle_creation_for_test(bool)` — 19 B, orphan
- `viewer::VkSceneRenderer::note_command_layout_rebuild()` — 17 B, orphan

### `libs/MatterSurfaceLib/src/part_asset.cpp` — 1 functions, 858 B

- `part_asset::cache_path[abi:cxx11](unsigned long long)` — 858 B, orphan

### `MatterEngine3/src/render/vk_cloud_shadows.cpp` — 3 functions, 763 B

- `viewer::(anonymous namespace)::record_read_tau(VkCommandBuffer_T*, void*)` — 257 B, dead-cluster
- `viewer::(anonymous namespace)::record_write_tau(VkCommandBuffer_T*, void*)` — 257 B, dead-cluster
- `viewer::VkCloudShadows::failed_candidate_destroyed_for_test() const` — 249 B, dead-cluster

### `MatterEngine3/src/ecs/transform_system.cpp` — 1 functions, 689 B

- `matter::ecs::(anonymous namespace)::hierarchy_command_queue(flecs::entity) [clone .isra.0]` — 689 B, dead-cluster

### `MatterEngine3/src/props/props.cpp` — 8 functions, 664 B

- `matter::props::get_string[abi:cxx11](matter::props::Binding const&, matter::props::Desc const&)` — 184 B, orphan
- `matter::props::DynamicGroup::get_string[abi:cxx11](unsigned int) const` — 135 B, orphan
- `matter::props::DynamicGroup::format[abi:cxx11](unsigned int) const` — 120 B, orphan
- `matter::props::DynamicGroup::copy_values(void*, void const*) const` — 80 B, orphan
- `matter::props::group_copy_assign(matter::props::Group const&, void*, void const*)` — 62 B, orphan
- `matter::props::group_construct(matter::props::Group const&, void*)` — 33 B, orphan
- `matter::props::group_destruct(matter::props::Group const&, void*)` — 33 B, orphan
- `matter::props::Binding::free_instance(void*) const` — 17 B, orphan

### `MatterEngine3/src/render/vk_volumetrics.cpp` — 4 functions, 648 B

- `viewer::VkVolumetrics::grid_rgba16f_volume_count_for_test() const` — 264 B, dead-cluster
- `viewer::VkVolumetrics::update_emitters(matter::VulkanDevice&, std::vector<viewer::GpuVolumeEmitter, std::allocator<viewer::GpuVolumeEmitter> > const&)` — 210 B, orphan
- `viewer::(anonymous namespace)::half_to_float(unsigned short)` — 132 B, dead-cluster
- `viewer::VkVolumetrics::cloud_density_dimensions_for_test() const` — 42 B, dead-cluster

### `libs/MatterSurfaceLib/src/mesh_worker_pool.cpp` — 2 functions, 582 B

- `MeshWorkerPool::worker_loop(int)` — 450 B, dead-cluster
- `MeshWorkerPool::~MeshWorkerPool()` — 132 B, dead-cluster

### `MatterEngine3/src/render/vk_context.cpp` — 5 functions, 513 B

- `matter::detail::DeviceLifetimeControl::~DeviceLifetimeControl()` — 388 B, orphan
- `matter::detail::DeviceAccessToken::unregister_control(matter::detail::DeviceLifetimeControl&)` — 52 B, orphan
- `matter::detail::DeviceAccessToken::destroy_registered_resources()` — 40 B, orphan
- `matter::detail::DeviceAccessToken::register_control(matter::detail::DeviceLifetimeControl&)` — 22 B, orphan
- `matter::VulkanDevice::preserve_after_unproven_external_work()` — 11 B, orphan

### `MatterEngine3/src/render/vt_residency.cpp` — 3 functions, 511 B

- `vt::VtResidency::inject_feedback_for_test(vt::VtFeedbackRequest const*, unsigned long long)` — 352 B, orphan
- `vt::VtResidency::refresh_indirection_stats()` — 102 B, orphan
- `vt::VtResidency::drain_enrich(VkCommandBuffer_T*)` — 57 B, orphan

### `MatterEngine3/src/matter_engine.cpp` — 8 functions, 505 B

- `matter::WorldSession::Impl::reset_publication_completion_locked(matter::WorldSession::Impl::PublicationCompletion&)` — 183 B, orphan
- `matter::WorldSession::root_instance_info(unsigned int, matter::InstanceInfo&) const` — 148 B, orphan
- `matter::WorldSession::root_instance_count() const` — 44 B, orphan
- `matter::WorldSession::Impl::ensure_bake_pool_started()` — 38 B, orphan
- `matter::WorldSession::Impl::bake_pool_outstanding()` — 27 B, orphan
- `matter::WorldSession::Impl::shutdown_bake_pool()` — 25 B, orphan
- `matter::WorldSession::Impl::ensure_worker_started()` — 21 B, orphan
- `matter::WorldSession::Impl::sector_drawn_level_conflict(matter::WorldSession::Impl::SectorKey const&, int*, std::unordered_set<matter::WorldSession::Impl::SectorKey, matter::WorldSession::Impl::SectorKeyHash, std::equal_to<matter::WorldSession::Impl::SectorKey>, std::allocator<matter::WorldSession::Impl::SectorKey> > const*, matter::WorldSession::Impl::SectorKey*) const` — 19 B, orphan

### `MatterEngine3/src/animation/ozz_adapter.cpp` — 3 functions, 402 B

- `matter::animation::OzzSkeleton::operator=(matter::animation::OzzSkeleton&&)` — 157 B, orphan
- `matter::animation::OzzSampleContext::operator=(matter::animation::OzzSampleContext&&)` — 125 B, orphan
- `matter::animation::OzzAnimation::operator=(matter::animation::OzzAnimation&&)` — 120 B, orphan

### `MatterEngine3/src/render/vk_atmosphere.cpp` — 3 functions, 339 B

- `viewer::VkAtmosphere::direct_sun_transmittance(float, matter::Float3 const&) const` — 198 B, orphan
- `viewer::VkAtmosphere::view_change_pending(float, matter::Float3 const&) const` — 118 B, orphan
- `viewer::VkAtmosphere::multiscatter() const` — 23 B, orphan

### `MatterEngine3/src/sector_streamer.cpp` — 4 functions, 326 B

- `matter_stream::SectorStreamer::tile_centre_dist(int, long long, long long) const` — 102 B, orphan
- `matter_stream::SectorStreamer::sector_dist(long long, long long) const` — 79 B, orphan
- `matter_stream::SectorStreamer::desired_lod_for_dist(float) const` — 74 B, orphan
- `matter_stream::SectorStreamer::nested_scatter_tier(float) const` — 71 B, orphan

### `MatterEngine3/src/animation/animation_store.cpp` — 1 functions, 318 B

- `matter::AnimationService::set_symbol(matter::AnimationInputHandle, unsigned int)` — 318 B, orphan

### `MatterEngine3/src/render/vk_pipeline.cpp` — 2 functions, 318 B

- `matter::(anonymous namespace)::record_dispatch(VkCommandBuffer_T*, void*)` — 286 B, dead-cluster
- `matter::VkComputePipelineResource::operator=(matter::VkComputePipelineResource&&)` — 32 B, orphan

### `MatterEngine3/src/event/command.cpp` — 2 functions, 314 B

- `matter::evt::CommandRegistry::is_lane_owner_current_thread(matter::evt::lane) const` — 253 B, orphan
- `matter::evt::CommandRegistry::active_scope_open() const` — 61 B, orphan

### `MatterEngine3/src/event/dispatch_context.cpp` — 1 functions, 293 B

- `matter::evt::detail::is_dispatching_on_this_thread(matter::evt::SubscriptionBlock const*)` — 293 B, dead-cluster

### `MatterEngine3/src/render/vk_resources.cpp` — 1 functions, 235 B

- `matter::(anonymous namespace)::record_transition(VkCommandBuffer_T*, void*)` — 235 B, dead-cluster

### `MatterEngine3/src/animation/animation_controllers.cpp` — 1 functions, 205 B

- `matter::animation::NativeControllerRegistry::register_factory(unsigned long long, std::unique_ptr<matter::animation::NativeController, std::default_delete<matter::animation::NativeController> > (*)(unsigned char const*, unsigned long long, matter::animation::NativeControllerLayout&))` — 205 B, orphan

### `MatterEditor/src/editor_props.cpp` — 6 functions, 197 B

- `viewer::EditorProps::clear_world_dirty()` — 56 B, orphan
- `viewer::EditorProps::release_world_props()` — 45 B, orphan
- `viewer::EditorProps::release_draw_overrides()` — 45 B, orphan
- `viewer::EditorProps::world_props()` — 35 B, orphan
- `viewer::EditorProps::viewer_session_status()` — 8 B, orphan
- `viewer::EditorProps::viewer_atmosphere_status()` — 8 B, orphan

### `MatterEngine3/src/animation/animation_systems.cpp` — 3 functions, 193 B

- `matter::animation::AnimationSystems::run_physics(double)` — 84 B, orphan
- `matter::animation::AnimationSystems::run_post_physics(double)` — 84 B, orphan
- `matter::animation::AnimationSystems::sample_service_bindings()` — 25 B, orphan

### `MatterEditor/src/editor_model.cpp` — 2 functions, 176 B

- `viewer::EditorModel::mark_rows_changed()` — 117 B, orphan
- `viewer::EditorModel::is_selection_valid() const` — 59 B, orphan

### `MatterEditor/src/session_binding.cpp` — 2 functions, 103 B

- `viewer::SessionBinding::open_epoch()` — 90 B, orphan
- `viewer::SessionBinding::quiesce_bridge()` — 13 B, orphan

### `MatterEngine3/src/render/vk_animation_bounds.cpp` — 2 functions, 98 B

- `viewer::VkAnimationBounds::valid_aabb(viewer::VkAnimationBoundsAabb const&)` — 83 B, orphan
- `viewer::VkAnimationBounds::clear_frame()` — 15 B, orphan

### `libs/MatterSurfaceLib/src/lattice.cpp` — 2 functions, 88 B

- `GridLattice::~GridLattice()` — 55 B, dead-cluster
- `GridLattice::~GridLattice()` — 33 B, dead-cluster

### `MatterEngine3/src/animation/animation_targets.cpp` — 1 functions, 84 B

- `matter::animation::target_chains_overlap(matter::animation::CanonicalTarget const&, matter::animation::CanonicalTarget const&)` — 84 B, orphan

### `libs/MatterSurfaceLib/src/occupancy.cpp` — 1 functions, 78 B

- `pack_slot(SlotCoord)` — 78 B, dead-cluster

### `MatterEngine3/src/tileset_settle.cpp` — 1 functions, 64 B

- `tileset::SettleWorld::Impl::count_awake() const` — 64 B, orphan

### `MatterEngine3/src/render/vulkan_only_compat.cpp` — 1 functions, 60 B

- `MemRealloc` — 60 B, orphan

### `MatterEditor/src/camera_controller.cpp` — 1 functions, 60 B

- `viewer::CameraController::apply_raw_motion(GLFWwindow*, bool)` — 60 B, orphan

### `MatterEngine3/src/streaming/sector_streaming_coordinator.cpp` — 2 functions, 54 B

- `matter::streaming::detail::Coordinator::allocate_generation()` — 27 B, orphan
- `matter::streaming::detail::Coordinator::allocate_issuance()` — 27 B, orphan

### `MatterEngine3/src/render/vt_enrich.cpp` — 1 functions, 52 B

- `vt::VtEnricher::VtEnricher(std::unique_ptr<vt::VtEnricher::Impl, std::default_delete<vt::VtEnricher::Impl> >)` — 52 B, orphan

### `MatterEngine3/src/bake_trace.cpp` — 1 functions, 40 B

- `bake_trace::Collector::now_ms() const` — 40 B, orphan

### `libs/MatterSurfaceLib/src/material_registry.c` — 1 functions, 26 B

- `MaterialRegistrySetGroundMacroSlot` — 26 B, orphan

### `MatterEngine3/src/ecs/dynamic_scene_bridge.cpp` — 1 functions, 22 B

- `matter::scene::DynamicSceneBridge::fold_pick_token(unsigned long long)` — 22 B, orphan

### `MatterEngine3/src/render/vk_temporal.cpp` — 1 functions, 17 B

- `viewer::TemporalState::TransformTable::build_map()` — 17 B, orphan

### `MatterEditor/src/bake_lab_timeline.cpp` — 1 functions, 14 B

- `viewer::BakeLabTimeline::fit_view()` — 14 B, orphan

### `MatterEngine3/src/retopo_blacklist.cpp` — 1 functions, 8 B

- `matter_engine3::retopo_blacklist::blacklist_size()` — 8 B, orphan

### `MatterEngine3/src/live_edit.cpp` — 1 functions, 5 B

- `live_edit::FakeWatcher::now_ms()` — 5 B, dead-cluster

### `MatterEngine3/src/render/vk_instance_cache.cpp` — 1 functions, 5 B

- `viewer::VulkanInstanceCache::source_expansion_count() const` — 5 B, orphan

### `MatterEngine3/src/event/event_hub.cpp` — 1 functions, 1 B

- `matter::evt::Hub::check_lane_owner(matter::evt::lane)` — 1 B, orphan

## Group 2 — product-dead, test-alive (by file)

| file | functions | bytes |
|---|---:|---:|
| `libs/MatterSurfaceLib/src/cluster.cpp` | 12 | 9248 |
| `MatterEngine3/src/animation/animation_store.cpp` | 17 | 8349 |
| `libs/MeshChartingLib/src/mesh_charting.cpp` | 4 | 7308 |
| `MatterEngine3/src/render/raster_mesh.cpp` | 1 | 7179 |
| `MatterEngine3/src/ecs/physics_context.cpp` | 33 | 5400 |
| `MatterEngine3/src/ecs/scene_registry.cpp` | 1 | 5260 |
| `libs/MatterSurfaceLib/src/particle_culling.cpp` | 6 | 4594 |
| `MatterEditor/src/streaming_anchor_controller.cpp` | 9 | 4170 |
| `MatterEngine3/src/animation/animation_evaluator.cpp` | 1 | 3892 |
| `MatterEngine3/src/part_cluster.cpp` | 1 | 3796 |
| `MatterEngine3/src/render/vk_scene_renderer.cpp` | 44 | 3364 |
| `MatterEngine3/src/tileset_metrics.cpp` | 4 | 3269 |
| `MatterEngine3/src/render/vk_emitter_gather.cpp` | 2 | 3022 |
| `MatterEngine3/src/props/props.cpp` | 34 | 2804 |
| `MatterEditor/src/selection_set.cpp` | 3 | 2475 |
| `libs/MatterSurfaceLib/src/blas_manager.cpp` | 9 | 2455 |
| `MatterEngine3/src/warp_field.cpp` | 2 | 2073 |
| `libs/MatterSurfaceLib/src/mesh_worker_pool.cpp` | 5 | 2054 |
| `MatterEngine3/src/event/subscription.cpp` | 3 | 2048 |
| `MatterEngine3/src/render/vk_temporal.cpp` | 6 | 1962 |
| `MatterEngine3/src/lod_select.cpp` | 3 | 1771 |
| `MatterEngine3/src/async_bake.cpp` | 1 | 1736 |
| `MatterEngine3/src/matter_engine.cpp` | 13 | 1723 |
| `MatterEngine3/src/render/vk_gi_math.cpp` | 7 | 1691 |
| `libs/MatterSurfaceLib/src/vertex_ao.cpp` | 3 | 1522 |
| `MatterEngine3/src/triangle_emit.cpp` | 3 | 1404 |
| `MatterEngine3/src/animation/animation_binding_bake.cpp` | 5 | 1332 |
| `libs/MatterSurfaceLib/src/cell.cpp` | 6 | 1188 |
| `MatterEngine3/src/live_edit.cpp` | 2 | 1124 |
| `libs/MatterSurfaceLib/src/occupancy.cpp` | 4 | 1104 |
| `MatterEngine3/src/world_tracer.cpp` | 3 | 1076 |
| `MatterEngine3/src/render/vk_animation_skinning.cpp` | 4 | 1075 |
| `libs/MatterSurfaceLib/src/surface.c` | 3 | 1014 |
| `MatterEngine3/src/render/vk_pipeline.cpp` | 2 | 991 |
| `MatterEngine3/src/event/event_hub.cpp` | 1 | 972 |
| `MatterEngine3/src/csg_lowering.cpp` | 1 | 839 |
| `MatterEngine3/src/render/matrix_math.cpp` | 7 | 802 |
| `MatterEditor/src/editor_model.cpp` | 3 | 802 |
| `MatterEngine3/src/part_asset_v2.cpp` | 1 | 755 |
| `libs/SpatialQueryLib/src/spatial_hash.c` | 2 | 753 |
| `MatterEngine3/src/ecs/dynamic_scene_bridge.cpp` | 3 | 752 |
| `libs/SpatialQueryLib/src/bvh_analyzer.cpp` | 1 | 523 |
| `MatterEngine3/src/streaming/sector_streaming_coordinator.cpp` | 9 | 516 |
| `MatterEngine3/src/tileset_settle.cpp` | 4 | 492 |
| `MatterEngine3/src/render/vk_lighting_controls.cpp` | 2 | 490 |
| `MatterEngine3/src/render/vk_animation_bounds.cpp` | 1 | 457 |
| `MatterEngine3/src/animation/animation_systems.cpp` | 6 | 435 |
| `libs/MatterSurfaceLib/src/tlas_manager.cpp` | 7 | 408 |
| `MatterEngine3/src/terrain_field.cpp` | 1 | 318 |
| `MatterEngine3/src/scene/scene_service.cpp` | 1 | 304 |
| `MatterEngine3/src/script_rng_binding.cpp` | 3 | 295 |
| `MatterEngine3/src/sector_grid.cpp` | 1 | 286 |
| `MatterEditor/src/ui_lighting_controls.cpp` | 1 | 282 |
| `MatterEditor/src/animation_panel_model.cpp` | 2 | 274 |
| `MatterEngine3/src/ecs/simulation_control.cpp` | 1 | 273 |
| `MatterEngine3/src/render/vk_cloud_shadows.cpp` | 4 | 265 |
| `MatterEngine3/src/script/world_definition_loader.cpp` | 1 | 254 |
| `MatterEngine3/src/ecs/transform_system.cpp` | 2 | 232 |
| `MatterEngine3/src/event/command.cpp` | 2 | 212 |
| `MatterEngine3/src/animation/anim_bundle.cpp` | 2 | 211 |
| `MatterEngine3/src/dsl_animation.cpp` | 1 | 198 |
| `MatterEngine3/src/animation/animation_budget.cpp` | 4 | 191 |
| `libs/MatterSurfaceLib/src/lattice.cpp` | 3 | 175 |
| `libs/SpatialQueryLib/src/bvh.cpp` | 1 | 167 |
| `libs/ParticleFlowLib/src/pf_path_recorder.cpp` | 1 | 165 |
| `MatterEngine3/src/sector_streamer.cpp` | 2 | 157 |
| `MatterEngine3/src/tileset_layout.cpp` | 2 | 126 |
| `MatterEngine3/src/world_flatten.cpp` | 1 | 122 |
| `MatterEngine3/src/tileset_bake.cpp` | 1 | 111 |
| `MatterEngine3/src/render/vt_compositor.cpp` | 3 | 108 |
| `libs/ProfileLib/src/profile.cpp` | 1 | 106 |
| `MatterEngine3/src/dsl_bindings.cpp` | 3 | 104 |
| `MatterEngine3/src/event/property.cpp` | 2 | 101 |
| `libs/MemoryLib/src/mem_pool.c` | 1 | 87 |
| `MatterEditor/src/properties_registry.cpp` | 1 | 78 |
| `libs/ParticleFlowLib/src/pf_math.cpp` | 1 | 73 |
| `MatterEngine3/src/seam_weld.cpp` | 1 | 72 |
| `MatterEngine3/src/retopo_blacklist.cpp` | 1 | 70 |
| `libs/MatterSurfaceLib/src/part_asset.cpp` | 1 | 56 |
| `MatterEditor/src/console_log.cpp` | 1 | 46 |
| `libs/MatterSurfaceLib/src/material_registry.c` | 2 | 42 |
| `MatterEngine3/src/render/vk_context.cpp` | 3 | 33 |
| `MatterEngine3/src/animation/ozz_adapter.cpp` | 2 | 28 |
| `MatterEngine3/src/render/vk_atmosphere.cpp` | 1 | 23 |
| `MatterEngine3/src/props/draw_overrides.cpp` | 1 | 15 |
| `MatterEngine3/src/ecs/ecs_runtime.cpp` | 3 | 14 |
| `MatterEngine3/src/render/dynamic_instance_slots.cpp` | 2 | 14 |
| `MatterEngine3/src/ecs/physics_shapes.cpp` | 1 | 8 |
| `MatterEngine3/src/scene/scene_change_tracker.cpp` | 1 | 5 |
| `MatterEngine3/src/render/vk_instance_cache.cpp` | 1 | 5 |

## Group 3 — prototype-only

- `libs/MatterSurfaceLib/src/tlas_manager.cpp` — 9 functions, 6501 B
- `libs/MatterSurfaceLib/src/blas_manager.cpp` — 13 functions, 6284 B
- `libs/SpatialQueryLib/src/bvh.cpp` — 5 functions, 2951 B
- `MatterEngine3/src/refine_controller.cpp` — 1 functions, 49 B

## Second axis — whole files never compiled

Sources that no build produces an object for (the reachability pass cannot
see these at all). Test suites compile straight to executables without
leaving `.o` files, so everything under `tests/` is a false positive and is
excluded. What remains:

| file | status |
|---|---|
| `superpowers/sdd/phase3-task-6-imguizmo-api-probe.cpp` | in no Makefile at all — orphan file |
| `libs/MatterSurfaceLib/src/voxel_imposter.cpp` | only named by `libs/MatterSurfaceLib/tests/Makefile` — test-only source |
| `libs/MatterSurfaceLib/src/shader_preprocessor.cpp` | named by three Makefiles but no object built on Windows |
| `MatterEditor/src/glfw_vulkan_only_context_x11.c` | Linux target only — expected, not dead |

## Caveats

- Scoped to one binary. A function dead in `editor.exe` may be live in
  `vulkan_smoke_tests.exe` or a `MatterEngine3/tests` target; the `elsewhere`
  column approximates that. For a definitive answer, run the tool per link
  and intersect.
- Virtual dispatch is handled (vtable relocations are real edges), but code
  reached only by *name* at runtime — QuickJS bindings, FIFO command
  handlers, property-system registrations — can look dead. One symbol landed
  in the `script` bucket for this reason and should be checked by hand.
- Reflects the build state of the objects on disk at the time of the run.
