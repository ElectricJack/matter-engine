# Repo-Wide Code Quality & Performance Review — 2026-07-07

Scope: all first-party sub-projects (~74k lines). Vendored code excluded
(Libraries/, quickjs, imgui, stb, autoremesher_core upstream sources).
Five parallel review passes: MatterEngine3 core, MatterEngine3
viewer/tools/tests, MatterSurfaceLib, particle/spatial libs, rendering
examples.

---

## Cross-cutting themes (fix these classes, not just instances)

### T1. Copy-paste vendoring has diverged and replicates real bugs — HIGH
- `object_allocator.c/.h`: byte-identical copies in **7 projects**, not
  symlinks. A fix in one propagates nowhere.
- `spatial_hash.c`: copies in 4+ projects. `ParticleDynamicsExample/Makefile:82`
  copies it from SpatialQueryLib at build time, but the copy is *also
  committed* — it goes stale silently. MatterSurfaceLib's copy has already
  diverged (different hash function).
- `surface.c`: `OpenParticleSurfaceLib` (930 lines) vs `SurfaceLib`
  (1014 lines) — two different marching-cubes implementations under one
  filename.
- `open_particle_surface.c` exists in both OpenParticleSurfaceLib and
  MatterSurfaceLib, carrying the **same critical realloc-dangling-pointer
  bug** (see B1) in both.
- `matrix_inverse` placeholder-that-returns-input exists in three places
  (see B5).
- `mc_tables.h` duplicated within OpenParticleSurfaceLib (include/ + src/)
  and differs between MatterSurfaceLib's src/ and include/ copies (ODR
  hazard: non-static `int edgeTable[256]` in a header).

**Fix:** adopt the CLAUDE.md `-I../Lib/include` convention (or symlinks)
everywhere; delete committed copies; kill the build-time `cp` rule.

### T2. No header dependency tracking in most Makefiles — HIGH
GPURayTraceExample, SurfaceLib, BasicWindowApp (and others) compile
`%.o: %.c` with no `-MMD -MP`. Header edits don't rebuild dependents —
this is the root cause of the known "wandering silent crashes after
struct/header changes" bug class already noted for Windows builds.
**Fix:** add `-MMD -MP` + `-include $(OBJ:.o=.d)` to every Makefile.

### T3. Fixed-size spatial hash with collision bugs — HIGH
SpatialQueryLib's `spatial_hash.c` (and its copies): fixed 1024 buckets
regardless of capacity; `sh_query_radius`/`sh_query_first` lack the
visited-bucket dedup that `sh_query_radius_nearest` has, so hash-colliding
cells return **duplicate results** — double-counted forces/heat in every
consumer. Also signed-overflow UB in the hash mix
(`coord.x * 73856093` on int). Fix once in SpatialQueryLib, then
re-propagate per T1.

### T4. 16-bit mesh indices silently truncate — HIGH
`SurfaceLib/src/surface.c:649` and `MatterSurfaceLib/src/surface.c:952`
(also `mesh_simplifier.cpp:197`): `unsigned short` indices with
`maxVertices = totalCells*3` (98k–786k possible). Past 65535 vertices,
indices wrap and the mesh silently corrupts. Fail loudly, split, or use
32-bit indices. Related: TLAS/BLAS 16-bit node packing caps at ~65k nodes
/ 1M tris with no guard — add asserts.

---

## Critical bugs (memory safety / silent corruption)

| # | Where | Bug |
|---|-------|-----|
| B1 | `open_particle_surface.c` (~:150/:194) — **both** MatterSurfaceLib and OpenParticleSurfaceLib | `GetCellIndex` stores raw `&spatialHashCells[i]` pointers in the spatial hash, then `realloc`s the array → every stored pointer dangles; `existingCell - spatialHashCells` yields garbage indices. Store integer indices instead. |
| B2 | `ParticleDynamicsExample/src/particle_system.cpp:513-521` | Same pattern: pointers into `particle_refs_` inserted while `emplace_back` can reallocate past the 10k reserve → dangling. |
| B3 | `open_particle_surface.c` (both copies, ~:439/:520 etc.) | Every cell gets `malloc(10000*sizeof(int))` and `indices[particleCount++]` with **no bounds check** → heap overflow past 10k particles/cell. |
| B4 | `MatterSurfaceLib/src/tlas_manager.cpp:304-412` and `GPURayTraceExample/src/tlas_manager.cpp:384-396` | `build()` skips draw records with missing BLAS but `generate_instance_texture_data` indexes `draw_records_[i]` parallel to `tlas_->blas[i]` → after one skip, every later instance gets wrong material/flags/offsets. Compact the surviving records alongside the BLAS list. |
| B5 | `matrix_inverse` stubs: `SpatialQueryLib/src/bvh.c:37-60` (returns identity), `MatterSurfaceLib/src/tlas_manager.cpp:49-52` and `GPURayTraceExample/src/tlas_manager.cpp:42-45` (return input) | Silent-wrong-answer landmines; SpatialQueryLib's is *live* — `bvh_instance_set_transform` produces wrong inverse transforms for any non-identity instance. Implement the real 4×4 inverse once. |
| B6 | `MatterSurfaceLib/src/surface.c:808-849` | Edge-dedup hash: after 100 failed probes it overwrites a foreign occupied slot (corrupts another edge's mapping); overflow paths leave `-1` edge indices later emitted as index 65535 / `vertices[-1]` read. |
| B7 | `MatterEngine3/src/world_tracer.cpp:502-542` | On invalid BVH `tri_idx`, `best_t` is still committed while normal/material keep stale values → probe bakes can shade with mismatched normal/material. |
| B8 | Unchecked `realloc` into the only pointer: `SurfaceLib/surface.c:104-137`, `MatterSurfaceLib/surface.c:108-149`, `open_particle_surface.c` ×6 sites | OOM → NULL-deref + leak of original block. `realloc` into a temp and check. |
| B9 | `ObjectAllocatorLib/src/object_allocator.c:24-28,70` | Stride is `max(objectSize, sizeof(void*))` with no alignment rounding → misaligned objects (UB) for sizes not a multiple of 8. Affects all 7 copies (T1). |
| B10 | `GPURayTraceExample/src/bvh.cpp:62-80` | `BvhMesh`/`BVH` allocate with MALLOC64/`new[]`, no destructors; `FREE64` never called → `clear()/reset_stats()` leaks all geometry. |
| B11 | `MatterSurfaceLib/src/bvh_analyzer.cpp:408` + `cell.cpp:395-401` | `RegisterBVH` keeps raw pointers in a never-pruned registry keyed by per-rebuild names → unbounded growth + dangling after `release_blas`. |
| B12 | `MatterEngine3/src/tileset_gtex.cpp:251,273` | `offset + size` bounds check computed in uint32 — wraps past 2^32 and bypasses the check (hardening). |

---

## Top performance issues

### MatterEngine3 core
1. `script_host.cpp:891-899` — per-cell particle assignment is
   O(cells × particles) though `touch_cells` already computed the overlap
   per particle; invert the loop. Dominant bake cost after meshing.
   Same pattern for carve lists at :914-917.
2. `script_host.cpp` `bake_source` — creates **two** JSRuntimes and
   evaluates/folds the script twice per bake (`merge_params_canonical`
   then `bake_source`). Fold once; ~halves script-heavy bake latency.
3. `part_flatten.cpp:517-523`, `lod_bake.cpp:211` — full-mesh copies per
   LOD level (~10 MB avoidable per level on 100k-tri parts); const-ref the
   unmodified root, copy only when decimating.
4. `script_host.cpp:217-230` — `std::regex` constructed per call, per bake
   and per live-edit tick; make it `static const` or a substring scan.
5. Small-n quadratics to watch: Poisson darts (`tileset_placement.cpp:53`),
   ear clipping re-running `reflex_set` per ear
   (`polygon_triangulate.cpp:104`), `tileset_bake.cpp:529-550` layer scan,
   linear BLAS handle scans (`part_flatten.cpp:749`, `lod_bake.cpp:224`).

### MatterEngine3 viewer
1. `gpu_culler.cpp:756-769` — `glGetBufferSubData` on two SSBOs right
   after cull dispatch **every frame** = full CPU↔GPU sync just for HUD
   counters. Read back one frame late via fence/persistent-mapped ring,
   or gate behind HUD.
2. `gpu_culler.cpp:430-571` — full instance expansion (`mul16` +
   transpose), `inst_recs` rebuild, and complete instance-SSBO re-upload
   every frame even for a static world; fresh heap vectors per frame.
   Dirty-flag on `WorldState::version()` (pattern already exists in
   `world_composer.cpp:64-76`) and hoist the vectors.
3. `gpu_culler.cpp:221-237` — full command template re-uploaded per frame
   just to zero `instance_count`; use `glCopyBufferSubData` from a
   pristine GPU copy or zero in-shader.
4. `raster_composer.cpp:134-176` — 64-entry material table + probe/light
   uniforms repacked & uploaded per frame though they change only on
   reload. `tileset_provider.cpp:158-193` — ~6 `snprintf` +
   `glGetUniformLocation` per slot per frame; cache locations.
5. `gpu_culler.cpp:963,1022` — HiZ `"src"` uniform looked up per call
   while siblings are cached.

### MatterSurfaceLib
1. `cluster.cpp:283-288` — `rebuild_dirty_cells` tests every particle
   against every dirty cell (O(dirty × total)); use the existing spatial
   hash with a radius query per cell. (Aligns with the "reduce input, not
   the mesher" direction.)
2. `surface.c` `ApplySubtractField/ApplyClipField` — every voxel scans all
   carve/clip particles (O(gridCells × carveCount)); bin carve particles
   into the spatial hash.
3. `blas_manager.cpp` — linear `find_if` per handle lookup; TLAS build
   calls `get_offsets` per instance → O(instances × entries). Add a
   handle→index map. `ensure_gpu_textures_ready` regenerates/uploads ALL
   triangle textures on any single change.
4. `bvh.cpp:528` — TLAS top-level split is count/2 in insertion order (no
   spatial sort) → poor tree quality; sort along longest centroid axis.
5. `open_particle_surface.c` — O(n²) bulk particle creation (linear
   free-slot scan; the `particleAllocator` created at :337 is never used),
   O(E²) edge dedup in `SimplifyMesh`, per-frame `printf` in hot paths,
   and cell rebuild `UpdateAnalysis` BVH-quality pass running on every
   `commit_group_mesh` (belongs behind a debug flag).

### ParticleDynamicsExample / SpatialQueryLib
1. `particle_system.h:257-259` — `gravity_radius = 5 + mass*100` with
   1.0 cell size makes `sh_query_radius` probe `(2r+1)³` cells:
   **~9.4M bucket probes per particle per frame** at mass 1 (~4B at
   mass 8). The grid is currently far slower than brute force. Cap the
   radius or size cells to the query radius. Related: `MAX_NEIGHBORS = 64`
   silently truncates — forces come from an arbitrary 64-particle subset.
2. `particle_system.cpp:391-492` — collision pass collects *all* pairs
   into a fresh vector, processes one, `break`s; rest is wasted per frame.
3. `particle_system.cpp:846-849` — `std::random_device` + `mt19937`
   constructed every frame (can hit /dev/urandom); make it a member.
4. `particle_system.cpp:653-828` — `sqrtf(powf(...))` in thermal/
   electrical hot loops; compare squared distances.
5. `particle_system.cpp:186` — `printf` on every `add_particle`.

### GPURayTraceExample
1. `tlas_manager.cpp:466-520` — animated path destroys + recreates TLAS
   node/instance textures **every frame** (`UnloadTexture` +
   `LoadTextureFromImage`); allocate once at max size, `UpdateTexture`.
2. `tlas_manager.cpp:291-325` — per-frame `make_unique<BVHInstance>` per
   instance + full copy + new TLAS; reuse storage across frames.
3. `tlas_manager.cpp:529-532`, `blas_manager.cpp:592-596` —
   `GetShaderLocation` string lookups ~9 uniforms per frame (main.cpp
   already caches camera uniforms — inconsistent).

### OpenParticleSurfaceLib
1. `open_particle_surface.c:1359-1413` — solid draw re-submits every
   triangle via immediate-mode `rlVertex3f` per frame despite the mesh
   being `UploadMesh`-ed; use `DrawMesh` + a lighting shader.

---

## Code smells / design debt

### MatterEngine3
- God functions: `bake_source` (~490 lines) and `eval_tileset`
  (~470 lines) in `script_host.cpp` (goto-cleanup style);
  `dsl_bindings.cpp:225-605` `j_ts_layer` has two near-identical
  copy-pasted placement blocks (:427-503 vs :525-596) — extract
  `place_one_instance(...)`.
- Math helpers duplicated across ≥4 files (`mul16`/`NormalMat`/
  `mat_invert`/quaternion extraction; `world_tracer.cpp` literally says
  "copy of NormalMat from part_flatten.cpp"). One internal `mat_math.h`.
- `tileset_settle.cpp:76,121` — raw `new`/`delete` for
  `SettleWorld::Impl`; use `unique_ptr`.
- `script_host.cpp:795` — whole build buffer copied per bake for
  introspection; `std::move` it.
- Part-expansion walk triplicated: `viewer/main.cpp:180-191`,
  `part_store.cpp:21-38`, `world_composer.cpp:30-56` — kept in sync by
  comments only; one shared traversal with a visitor.
- Viewer screenshot paths diverged: `main.cpp:469-477` still uses the
  `TakeScreenshot` API the FIFO path's own comment documents as broken;
  route both through `LoadImageFromScreen` + `ExportImage`.
- `gpu_culler.cpp:663-736` `readback_batches` (~75 lines) dead in
  production; move to tests or delete.
- `viewer/Makefile:189-190` — shader symlink rule skips when a real
  `shaders/` dir exists; current checkout has a partial `shaders/` dir,
  so a fresh build would run with missing/stale shaders. Validate files,
  not the directory.

### Tests (MatterEngine3)
- `CHECK` macro + failure counter copy-pasted into ~25 files with three
  counter names and inconsistent output; `viewer_logic_tests.cpp` mixes
  aborting `assert()` with accumulating `CHECK`. Shared `tests/check.h`.
- `tests/Makefile` (666 lines): ~20 near-identical blocks re-listing the
  MatterSurfaceLib source set; use a variable + pattern rule.
- `gpu_cull_tests.cpp` — fixed `/tmp/gpu_test_*` paths collide across
  concurrent runs; `mkdtemp`.
- Tools scripts: camera pose set copy-pasted across 3 shot scripts;
  brittle fixed sleeps (25–90 s) where `stress_sweep.sh` already polls
  the log — reuse that.

### MatterSurfaceLib
- `surface.c:519` `GenerateMeshInternal` ~490-line god function;
  `open_particle_surface.c` is an all-static-globals singleton.
- `blas_manager.cpp:180-185` — `if (triangle_count == 3)` "likely our
  unit test" heuristic changes production behavior for real 3-tri meshes
  and double-builds; gate on an explicit flag.
- `surface.c:214` — `enableEdgeDeduplication = false` under a comment
  saying "Default: enabled" (SurfaceLib has the same mismatch at :193,
  where dedup-off multiplies vertex count 4-6× and feeds the 16-bit
  overflow).
- Hardcoded relative `.shader_cache/` path in `main.cpp`.

### Examples
- `GPURayTraceExample/main.cpp` — 1130-line god file, 9 hardcoded scenes
  in one switch; ~250 lines of commented-out legacy code in
  `blas_manager.cpp` + dead teardown blocks in `tlas_manager.cpp`.
- `GPURayTraceExample/main.cpp:650` — ESC bound to cursor toggle but
  raylib's default exit key is also ESC (no `SetExitKey(KEY_NULL)`) —
  toggle unreachable; UI says SPACE but the key is LEFT_SHIFT (:976 vs
  :662).
- `BasicWindowApp/main.cpp:118-121` — the rotate matrix push wraps
  nothing; the "rotating cube" never rotates and the speed slider is
  inert.
- `SurfaceLib/main.c` — `meshMin/Max/Center` never computed (bounds box
  + 3 UI lines show zeros); second `LoadMaterialDefault()` leaked;
  silent geometry-skip on pool exhaustion (`surface.c:519,541,559`)
  produces holes instead of growing.
- `ParticleDynamicsExample/src/cluster_manager.cpp:44-92` — transfer
  functions are placeholders with dummy data that return `true`.

---

## Clean bills of health

- MatterEngine3: `probe_bake.cpp` (well-threaded), `part_asset_v2.cpp`
  (exemplary bounds-checked reader), tileset GPU bake paths (consistent
  GL cleanup), `dsl_state`/`dsl_triangle`, `module_resolver`.
- Viewer: sector binning + TLAS rebuild already properly
  version/fingerprint-gated; no GL resource leaks found.
- MatterSurfaceLib: QEM simplifier and mesh worker pool are clean.
- MeshChartingLib: clean. ObjectAllocatorLib: clean apart from B9.
- SurfaceLib: scalar field correctly computed once per grid point (no
  redundant corner sampling); memory pool avoids per-frame allocs.

---

## Suggested fix order

1. **Memory-safety bugs B1–B6** (dangling realloc pointers, heap
   overflow, TLAS desync, matrix_inverse stubs, edge-hash corruption) —
   silent corruption, several replicated across projects.
   MatterSurfaceLib items meet its "genuine bug" bar.
2. **T4 16-bit index truncation** — real mesh corruption at realistic
   sizes.
3. **T2 header dep tracking** in all Makefiles — retires a known crash
   class cheaply.
4. **T3 spatial-hash dedup/overflow fix** in SpatialQueryLib, then
   **T1 de-duplication** so the fix actually propagates.
5. **Viewer per-frame GPU sync + re-upload** (gpu_culler readback,
   instance re-upload, uniform re-upload) — biggest interactive-frame
   wins.
6. **Bake-path quadratics** (script_host cell assignment, double
   JSRuntime, LOD mesh copies) — biggest bake-latency wins.
7. ParticleDynamicsExample query-radius tuning (grid currently slower
   than brute force).
8. Smell cleanup (god functions, dead code, test harness unification)
   opportunistically alongside the above.
