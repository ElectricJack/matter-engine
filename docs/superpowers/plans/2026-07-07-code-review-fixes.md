# Code Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix all findings from `docs/code-review-2026-07-07.md` (the source spec — read it before any task): memory-safety bugs B1–B12, cross-cutting themes T1–T4, per-project performance issues, and code smells.

**Architecture:** Fix shared copied files at their source-of-truth project first (SpatialQueryLib, ObjectAllocatorLib), convert identical-copy consumers to compile sibling sources per the CLAUDE.md `-I../Lib` convention, then run per-project fix tasks in parallel (disjoint directories), then a single cross-repo Makefile dependency-tracking task, then full verification.

**Tech Stack:** C/C++17, raylib, OpenGL, QuickJS (vendored — do not touch), GNU Make. Linux verification via `bash build-all.sh test` with `GALLIUM_DRIVER=d3d12`.

## Global Constraints

- Worktree: `.claude/worktrees/code-review-fixes`, branch `fix/code-review-2026-07`. Never touch the main checkout.
- `GALLIUM_DRIVER=d3d12` must be exported for any test run that opens GL (viewer/gpu tests) — without it Mesa falls back to llvmpipe GL 4.5 and gates FATAL.
- Do NOT modify vendored code: `Libraries/`, quickjs*/libregexp*/libunicode*/cutils*/xsum* in MatterEngine3, imgui/stb/glad anywhere.
- MatterSurfaceLib is a stabilized project: Task 10 (genuine bugs) is in-policy; Task 11 (perf) is included because the user asked for all findings, but keep changes surgical and behavior-preserving.
- Every task: after edits, do a CLEAN rebuild of the affected project (`make clean && make`) — this repo has no header dep tracking until Task 17 and stale objects cause silent crashes.
- Commit after each task with a conventional message (`fix(project): ...`, `perf(project): ...`, `refactor(project): ...`).
- Findings referenced as B1–B12 / T1–T4 are defined in `docs/code-review-2026-07-07.md`.

---

## Phase 0 — Shared foundations (sequential: Tasks 1–4 in order)

### Task 1: SpatialQueryLib spatial hash fixes (T3)

**Files:**
- Modify: `SpatialQueryLib/src/spatial_hash.c`
- Create: `SpatialQueryLib/tests/spatial_hash_tests.c`
- Modify: `SpatialQueryLib/Makefile` (add `test` target)

**Interfaces:**
- Produces: unchanged public API in `SpatialQueryLib/include/spatial_hash.h`; consumers in Tasks 4–7 rely on `sh_query_radius`/`sh_query_first` no longer returning duplicates.

- [ ] **Step 1: Write failing tests.** Create `SpatialQueryLib/tests/spatial_hash_tests.c`, a standalone main that:
  - inserts two objects into cells known to collide in a 1024-bucket table (e.g. brute-force two `(x,y,z)` int coords whose current hash mod tablesize match — compute in the test), then calls `sh_query_radius` with a radius covering both and asserts each object appears exactly once in results;
  - inserts one object, queries `sh_query_first` with a radius spanning many colliding cells, asserts non-NULL and correct;
  - inserts coords near INT_MAX/4 scale to exercise the hash without UB (run test build with `-fsanitize=undefined`).
- [ ] **Step 2: Add `test:` target to `SpatialQueryLib/Makefile`** compiling `tests/spatial_hash_tests.c src/spatial_hash.c -Iinclude -fsanitize=address,undefined -g`. Run: expect duplicate-result assertion FAILS (and possibly UBSan hash overflow report).
- [ ] **Step 3: Fix `spatial_hash.c`:**
  - Hash mix (~line 42-44): compute in `unsigned int`: `((unsigned)coord.x * 73856093u) ^ ((unsigned)coord.y * 19349663u) ^ ((unsigned)coord.z * 83492791u)`, mod table size.
  - Table sizing: size bucket count from `initialCapacity` (next power of two ≥ capacity/4, min 1024) instead of fixed 1024.
  - Duplicate-visit guard: in `sh_query_radius` (~:181) and `sh_query_first` (~:402 region) add the same visited-bucket dedup already used by `sh_query_radius_nearest` (~:224). Additionally, entries must be verified against the actual queried cell coord if entries store their cell coord; if they don't, add `SpatialCoord coord` to the entry struct at insert time and compare in queries so hash-colliding foreign cells are skipped, and so the same bucket is never re-scanned.
  - Rename inner `float dx` (~:202-204) shadowing loop `int dx` → `fdx/fdy/fdz`.
- [ ] **Step 4: Run tests — expect PASS, ASan/UBSan clean.**
- [ ] **Step 5: Commit** `fix(SpatialQueryLib): dedup spatial hash query results, unsigned hash, capacity-sized table`.

### Task 2: SpatialQueryLib real matrix_inverse

**Files:**
- Modify: `SpatialQueryLib/src/bvh.c:37-60`
- Modify: `SpatialQueryLib/tests/spatial_hash_tests.c` → add or create `SpatialQueryLib/tests/bvh_tests.c` (wire into `test` target)

**Interfaces:**
- Produces: `matrix_inverse` returning a true 4×4 inverse; `bvh_instance_set_transform` (~:554) becomes correct for non-identity transforms.

- [ ] **Step 1: Failing test:** build a translation+rotation+scale matrix, call `matrix_inverse`, multiply M·M⁻¹, assert ≈ identity (epsilon 1e-4). Expect FAIL (stub returns identity).
- [ ] **Step 2: Implement** a standard 4×4 cofactor/adjugate inverse (the raylib `MatrixInvert` implementation is a good reference; write it locally — SpatialQueryLib must stay raylib-free if it currently is). Guard determinant ≈ 0 by returning identity.
- [ ] **Step 3: Test PASS. Also verify an instance-transformed `bvh_intersect` hits where expected** (ray a known transformed triangle).
- [ ] **Step 4: Commit** `fix(SpatialQueryLib): implement real 4x4 matrix_inverse (was identity stub)`.

### Task 3: ObjectAllocatorLib alignment (B9)

**Files:**
- Modify: `ObjectAllocatorLib/src/object_allocator.c:24-28,70`
- Create: `ObjectAllocatorLib/tests/object_allocator_tests.c` + `test` Makefile target

- [ ] **Step 1: Failing test:** create allocator with objectSize 12 (or any non-multiple of 8), allocate several objects, assert `((uintptr_t)p % _Alignof(max_align_t)) == 0` for each. Build with UBSan.
- [ ] **Step 2: Fix:** round stride up: `stride = (stride + al - 1) / al * al;` with `size_t al = _Alignof(max_align_t);` after the existing `max(objectSize, sizeof(void*))`.
- [ ] **Step 3: Test PASS.**
- [ ] **Step 4: Commit** `fix(ObjectAllocatorLib): align object stride to max_align_t`.

### Task 4: De-duplicate shared sources into consumers (T1)

**Files:**
- Modify: `ParticleDynamicsExample/Makefile` (delete the `cp ../SpatialQueryLib/src/spatial_hash.c src/` rule ~:82; compile `../SpatialQueryLib/src/spatial_hash.c` directly; `-I../SpatialQueryLib/include`)
- Delete: committed duplicate copies where byte-identical to source of truth **before Tasks 1–3** (verify with git history/md5 against `main`): `ParticleDynamicsExample/src/spatial_hash.c` + header, `OpenParticleSurfaceLib` and `SurfaceLib` copies of `spatial_hash.c/.h`, `object_allocator.c/.h` copies in SpatialQueryLib, ParticleDynamicsExample, OpenParticleSurfaceLib, SurfaceLib, GPURayTraceExample
- Modify: each affected project Makefile to compile the sibling source directly (per CLAUDE.md convention), e.g. add `../ObjectAllocatorLib/src/object_allocator.c` to sources and `-I../ObjectAllocatorLib/include` to CFLAGS
- Delete: `OpenParticleSurfaceLib/src/mc_tables.h` (identical duplicate of `include/mc_tables.h`; fix any `#include "mc_tables.h"` pathing)
- **Exclusions:** MatterSurfaceLib keeps its diverged copies (handled in Task 10); MeshChartingLib untouched.

- [ ] **Step 1:** For each candidate duplicate, `md5sum` against the pre-Task-1 source-of-truth version (use `git show main:SpatialQueryLib/src/spatial_hash.c | md5sum`). Only convert byte-identical ones; report any unexpected divergence instead of overwriting.
- [ ] **Step 2:** Update Makefiles: remove local copies from SRC lists, add sibling paths + include dirs. Object files for sibling sources go into the project's build dir (add an explicit rule, e.g. `spatial_hash.o: ../SpatialQueryLib/src/spatial_hash.c`).
- [ ] **Step 3:** `git rm` the duplicate files.
- [ ] **Step 4:** Clean-build each affected project: `for p in ParticleDynamicsExample OpenParticleSurfaceLib SurfaceLib GPURayTraceExample; do (cd $p && make clean && make); done` — expect all succeed.
- [ ] **Step 5: Commit** `refactor: compile shared spatial_hash/object_allocator from source-of-truth libs (T1)`.

---

## Phase 1 — Per-project fixes (Tasks 5–16; parallelizable across disjoint projects, but Tasks 12/13 and 14/15 sequential within their directories)

### Task 5: ParticleDynamicsExample bugs + perf

**Files:** Modify `ParticleDynamicsExample/src/particle_system.cpp`, `include/particle_system.h`, `src/cluster_manager.cpp`, `main.cpp`.

Findings (report §ParticleDynamicsExample): B2 dangling refs; gravity radius grid blowup; MAX_NEIGHBORS truncation; wasted collision pass; per-frame RNG; sqrtf(powf); printf; variable-dt damping; placeholder cluster transfers.

- [ ] **Step 1 (B2):** `particle_system.cpp:513-521` — store particle **indices** in the spatial hash (`(void*)(uintptr_t)i`), decode on query; delete the pointer-into-vector pattern. Verify with ASan run of the demo headless if possible, else code review + unit reasoning.
- [ ] **Step 2 (radius):** `particle_system.h:257-259` — clamp query radius: `radius = fminf(radius, MAX_QUERY_RADIUS)` with `MAX_QUERY_RADIUS = 16 * SPATIAL_CELL_SIZE` (documented constant), and increase `SPATIAL_CELL_SIZE` to 4.0 so `(2r+1)³` stays ≤ ~1k cells. Print a one-time warning when clamping.
- [ ] **Step 3 (MAX_NEIGHBORS):** on truncation (query returned == capacity), emit a one-time warning; bump to 128.
- [ ] **Step 4 (collision pass):** `particle_system.cpp:391-492` — process all collected pairs with a "dead" flag check per particle instead of collecting-all-then-break; or early-exit collection at first pair if single-pair semantics are intended (read the code; preserve existing gameplay behavior, remove the wasted work).
- [ ] **Step 5:** RNG → member `std::mt19937 rng_` seeded once (:846-849). Replace `sqrtf(powf(dx,2)+…)` with squared-distance comparisons at :653-655, :754-756, :826-828. Delete/gate the `add_particle` printf (:186). Damping: apply `powf(DAMPING, dt*60.0f)` (frame-rate independent) at :545 region.
- [ ] **Step 6:** `cluster_manager.cpp:44-92` — placeholder transfer functions: make them return `false` and log "not implemented", or delete if unreferenced (check callers first).
- [ ] **Step 7:** `make clean && make`; run the binary briefly headless if it supports it (else compile-only). **Commit** `fix(ParticleDynamicsExample): spatial-hash index storage, query radius cap, hot-loop fixes`.

### Task 6: OpenParticleSurfaceLib bugs + perf

**Files:** Modify `OpenParticleSurfaceLib/src/open_particle_surface.c`.

Findings: B1 (realloc-dangling cell pointers ~:194), B3 (10k malloc no bounds :520/:535/:637/:652), B8 (unchecked reallocs :196,:319,:936,:1002,:1115,:1120; mallocs :340,:366), O(E²) SimplifyMesh edge dedup (:1143-1148,:1224-1228), immediate-mode solid draw (:1359-1413), O(n²) CreateParticle free-slot scan (:475, unused `particleAllocator` :337), linear AddActiveCellIfNeeded (:308-311), per-frame printf/DEBUG_LOG (:745,:804,:816,:882,:1273), dead `newCellIndices` (:750-759).

- [ ] **Step 1 (B1):** store **int cell indices** in the spatial hash instead of `SpatialHashCell*`; replace `existingCell - spatialHashCells` arithmetic with the stored index. Grep every `sh_insert`/`sh_query_first` use in this file.
- [ ] **Step 2 (B3):** replace fixed `malloc(10000*sizeof(int))` with capacity-tracked growth: store `capacity` next to `particleCount`, `realloc` ×2 when full (checked, temp-pointer pattern).
- [ ] **Step 3 (B8):** every `p = realloc(p, …)` → `tmp = realloc(...); if (!tmp) { log + bail without losing p; } p = tmp;`. Check the mallocs.
- [ ] **Step 4 (perf):** edge dedup → hash edges by `((uint64_t)min(v1,v2)<<32)|max(v1,v2)` open-addressed table sized from edge count; free-slot free-list for CreateParticle (singly-linked through the dead slots); `AddActiveCellIfNeeded` → per-cell `isActive` flag checked O(1); solid draw path → `DrawMesh` of the uploaded mesh with raylib default lighting shader instead of per-frame `rlVertex3f` (verify visually only if trivially runnable — otherwise note in commit that draw-path change is compile-verified).
- [ ] **Step 5:** delete dead `newCellIndices`; gate all hot-path printf/DEBUG_LOG behind `#ifdef OPS_DEBUG`.
- [ ] **Step 6:** `make clean && make` (ASan build if a headless run exists). **Commit** `fix(OpenParticleSurfaceLib): spatial-hash indices, bounded cell arrays, checked reallocs, hot-path perf`.

### Task 7: SurfaceLib fixes

**Files:** Modify `SurfaceLib/src/surface.c`, `SurfaceLib/main.c`.

Findings: T4 16-bit indices (:649,:663), dedup default/comment mismatch (:193), unchecked pool reallocs (:104-137), silent geometry skip on pool exhaustion (:519,:541,:559), capped-32 nearest query (:711-713), main.c dead bounds display (:105-108,:228-231,:250-252), leaked second `LoadMaterialDefault()` (:95,:146), global mesh.

- [ ] **Step 1 (T4):** switch `mesh.indices` to `unsigned int`. raylib's `Mesh.indices` is `unsigned short*` — so instead: keep 16-bit but **fail loudly**: if vertexCount would exceed 65535, log an error and stop emitting (no wraparound), AND enable edge dedup by default (:193 → `true`, fixing the comment mismatch) which cuts vertex count 4–6× below the limit for the default 32³ grid. Add a static assert/comment documenting the raylib constraint.
- [ ] **Step 2:** pool reallocs → checked temp-pointer pattern; on exhaustion grow (×2) instead of warn+skip at :519,:541,:559.
- [ ] **Step 3:** nearest-particle query (:711-713): track min-distance inside the loop instead of collecting a capped-32 list.
- [ ] **Step 4 (main.c):** compute `meshMin/Max/Center` from the generated mesh once after generation; delete the duplicate `LoadMaterialDefault()` at :146 (reuse the first).
- [ ] **Step 5:** `make clean && make`, run briefly under WSLg (`GALLIUM_DRIVER=d3d12 ./surface_app` few seconds) to confirm surface renders and bounds display is nonzero. **Commit** `fix(SurfaceLib): loud 16-bit index guard, dedup on by default, pool growth, bounds display`.

### Task 8: BasicWindowApp rotation

**Files:** Modify `BasicWindowApp/main.cpp:118-121`.

- [ ] **Step 1:** move the `DrawCube` call inside the `rlPushMatrix/rlRotatef/rlPopMatrix` block so the rotation applies; wire the ImGui speed slider variable into the accumulated angle.
- [ ] **Step 2:** `make clean && make`; run under WSLg a few seconds — cube must rotate, slider must change speed. **Commit** `fix(BasicWindowApp): cube rotation matrix actually wraps the draw`.

### Task 9: GPURayTraceExample fixes

**Files:** Modify `GPURayTraceExample/src/tlas_manager.cpp`, `src/blas_manager.cpp`, `src/bvh.cpp`, `main.cpp`.

Findings: per-frame texture destroy/create (:466-520), per-frame unique_ptr churn (:291-325), uniform string lookups (:529-532; blas :592-596), B4 draw-record/BLAS desync (:384-396 vs :299-302), B5 matrix_inverse placeholder (:42-45), B10 BVH leaks (bvh.cpp:62-80), ESC exit-key clash (main.cpp:650), SPACE/LEFT_SHIFT UI text (:976/:980 vs :662), ~250 lines dead code (blas_manager :57-115,:202-217,:711-812; tlas dead teardown :122-128,:255-263,:281-289).

- [ ] **Step 1 (B4):** build a compacted `active_records` vector during `build()` skips; use it in `generate_instance_texture_data`.
- [ ] **Step 2 (B5):** replace the placeholder `matrix_inverse` with raylib's `MatrixInvert` (raylib is already linked here) or delete it if unreferenced (grep first).
- [ ] **Step 3 (B10):** add destructors (or explicit `destroy()` called from BLASManager `clear()/reset_stats()`) freeing `tri/triEx/bvhNode` via FREE64 and `delete[] triIdx`.
- [ ] **Step 4 (perf):** allocate TLAS node + instance textures once at max size, `UpdateTexture` per frame; hoist `instance_storage_`/TLAS reuse across frames; cache all shader uniform locations once after shader load (mirror main.cpp's camera-uniform pattern).
- [ ] **Step 5 (UX/dead code):** `SetExitKey(KEY_NULL)` + keep ESC cursor toggle; fix the help text to match actual keys; delete the commented-out legacy blocks and dead teardown code.
- [ ] **Step 6:** `make clean && make`; run scene 1 briefly under WSLg, toggle animation, confirm no per-frame stutter regression and correct render. **Commit** `fix(GPURayTraceExample): TLAS texture reuse, record/BLAS desync, BVH leaks, input fixes`.

### Task 10: MatterSurfaceLib genuine bugs (in-policy for the read-only project)

**Files:** Modify `MatterSurfaceLib/src/open_particle_surface.c`, `src/surface.c`, `src/tlas_manager.cpp`, `src/spatial_hash.c`, `src/blas_manager.cpp`, `src/bvh_analyzer.cpp`, `src/cell.cpp`, `src/mesh_simplifier.cpp`; Delete `MatterSurfaceLib/include/mc_tables.h` OR `src/mc_tables.h` (keep the used one — grep includes).

Findings: B1/B3/B8 (same as Task 6 but in this diverged copy — apply the same fixes: hash stores indices; bounded growth; checked reallocs), B4 TLAS desync (:304-322 vs :364-412 — compacted records fix), B6 edge-hash corruption (surface.c:808-849 — on probe exhaustion fall through to non-dedup vertex emission; skip triangles containing -1 edge vertices), B11 (bvh_analyzer.cpp:408 registry — unregister on `release_blas`, key by stable cell id), T4 16-bit indices (surface.c:952, mesh_simplifier.cpp:197 — loud failure on >65535, same rationale as Task 7), spatial_hash.c dedup guard (:181,:402 — port the visited[] guard from :224), `enableEdgeDeduplication` comment/value mismatch (:214 — enable it, matching Task 7), `matrix_inverse` placeholder (tlas_manager.cpp:49-52 — implement or delete if unreferenced), `triangle_count == 3` unit-test heuristic (blas_manager.cpp:180-185 — replace with explicit `force_subdiv_one_prim` flag defaulting false; update any test that relied on it), 16-bit node-packing guards (add asserts where child indices are `& 0xFFFF` packed).

- [ ] **Step 1:** apply each fix above, surgically; no restructuring, no renames beyond what the fix needs.
- [ ] **Step 2:** `cd MatterSurfaceLib && make clean && make` (and `bash build.sh` if that's the canonical build).
- [ ] **Step 3:** run MatterEngine3 test suite (it compiles MSL sources): `cd ../MatterEngine3 && make test` with `GALLIUM_DRIVER=d3d12` — all suites must pass; fix regressions (esp. anything that depended on the triangle_count==3 heuristic).
- [ ] **Step 4: Commit** `fix(MatterSurfaceLib): realloc dangling pointers, heap overflow, TLAS desync, edge-hash corruption, index guards`.

### Task 11: MatterSurfaceLib performance (surgical)

**Files:** Modify `MatterSurfaceLib/src/cluster.cpp`, `src/surface.c`, `src/blas_manager.cpp`, `src/tlas_manager.cpp`, `src/bvh.cpp`, `src/cell.cpp`, `src/open_particle_surface.c`.

Findings: rebuild_dirty_cells O(dirty×total) (cluster.cpp:283-288 → radius query per cell via the existing spatial hash), ApplySubtractField/ApplyClipField voxel×carve scans (bin carve particles into the spatial hash, query per cell), BLAS linear find_if lookups + per-instance get_offsets (add `std::unordered_map<handle,size_t>` maintained on add/release), `ensure_gpu_textures_ready` full re-upload on any change (dirty-flag per entry), TLAS count/2 split (bvh.cpp:528 → sort along longest centroid axis before split), `add_particle_index` std::find (cell.cpp:113 → skip the find when caller guarantees uniqueness, or use a flag array), `UpdateAnalysis` on every commit_group_mesh (cell.cpp:395-401 → behind env/debug flag), per-frame printf hot spam (open_particle_surface.c — gate behind ifdef), CreateParticle free-list + AddActiveCellIfNeeded flag (same approach as Task 6).

- [ ] **Step 1:** apply fixes; keep every change behavior-preserving (identical outputs, faster).
- [ ] **Step 2:** `make clean && make`; `cd ../MatterEngine3 && GALLIUM_DRIVER=d3d12 make test` — pass required (meshing goldens must not change; the TLAS split-axis change affects BVH layout, not intersections — if a golden encodes BVH node order, prefer reverting the split change over touching goldens and note it).
- [ ] **Step 3: Commit** `perf(MatterSurfaceLib): spatial-hash dirty-cell rebuild, carve binning, BLAS handle map, TLAS split axis`.

### Task 12: MatterEngine3 core performance

**Files:** Modify `MatterEngine3/src/script_host.cpp`, `src/part_flatten.cpp`, `src/lod_bake.cpp`.

Findings: O(cells×particles) cell assignment (script_host.cpp:891-899 — invert: during the existing per-particle `touch_cells` pass, push the particle index into each touched cell; same inversion for carve lists :914-917), double JSRuntime/double eval per bake (`merge_params_canonical` + `bake_source` — fold sources once, evaluate once, pass canonical params through; keep determinism identical), LOD full-mesh copies (part_flatten.cpp:517-523 const-ref the unmodified root level; lod_bake.cpp:211 copy only when decimating), per-call `std::regex` (script_host.cpp:217-230 → `static const std::regex` or substring scan), `last_buffer_` copy (:795 → `std::move`).

- [ ] **Step 1:** apply fixes. The bake path is deterministic-by-design — outputs must be bit-identical; the existing test suite has bake goldens that will catch drift.
- [ ] **Step 2:** `cd MatterEngine3 && make clean && GALLIUM_DRIVER=d3d12 make test` — all pass, goldens unchanged.
- [ ] **Step 3: Commit** `perf(MatterEngine3): invert cell assignment loop, single JSRuntime per bake, avoid LOD mesh copies`.

### Task 13: MatterEngine3 core correctness + smells

**Files:** Modify `MatterEngine3/src/world_tracer.cpp`, `src/tileset_gtex.cpp`, `src/tileset_settle.cpp`, `src/dsl_bindings.cpp`; Create `MatterEngine3/src/mat_math.h`; Modify `src/part_flatten.cpp`, `src/csg_lowering.cpp`, `src/world_flatten.cpp`, `src/tileset_bake.cpp` (to use it).

Findings: B7 stale-shading commit (world_tracer.cpp:502-542 — commit `best_t` only in the branch that validates `tri_idx` and writes normal/material), B12 uint32 bounds wrap (tileset_gtex.cpp:251,273 — cast to `uint64_t`/`size_t` before add; same `p + sizeof(T)` hardening in part_asset_v2.cpp Reader), SettleWorld raw new/delete (tileset_settle.cpp:76,121 → `std::unique_ptr<Impl>`), duplicated math helpers (`mul16`/`NormalMat`/`mat_invert`/`mat_mul`/`mat4_mul`/Shepperd quat extraction across part_flatten/world_tracer/csg_lowering/world_flatten/tileset_bake/tileset_settle → one header-only `src/mat_math.h`, delete the copies), j_ts_layer duplicated placement blocks (dsl_bindings.cpp:427-503 vs :525-596 → extract `place_one_instance(...)` static helper; byte-identical bake outputs required).

**Explicitly deferred (get user sign-off before attempting):** splitting `bake_source`/`eval_tileset` god functions — goto-cleanup refactors of 500-line functions carry regression risk disproportionate to the payoff.

- [ ] **Step 1:** B7 + B12 fixes first; `make test` green.
- [ ] **Step 2:** mat_math.h consolidation + unique_ptr + place_one_instance extraction; `make test` green (bake goldens verify byte-identical outputs).
- [ ] **Step 3: Commit** `fix(MatterEngine3): world_tracer stale-hit commit, 64-bit bounds checks; refactor: shared mat_math.h, j_ts_layer helper`.

### Task 14: MatterEngine3 viewer performance

**Files:** Modify `MatterEngine3/viewer/gpu_culler.cpp`, `viewer/raster_composer.cpp`, `viewer/tileset_provider.cpp`.

Findings: per-frame `glGetBufferSubData` sync (:756-769 → keep the readback but gate it behind the HUD/stats FIFO command being active; read one-frame-late from the previous frame's dispatch), full per-frame instance expansion + SSBO re-upload for static worlds (:430-571 → dirty-check on `WorldState::version()` + resolved-set fingerprint, mirroring `world_composer.cpp:64-76`; hoist `expanded`/`per_slot_count`/`inst_recs` to members), cmd template full re-upload (:221-237 → pristine GPU-side buffer + `glCopyBufferSubData`), static uniforms per frame (raster_composer.cpp:134-176 → upload on (re)connect/dirty only; tileset_provider.cpp:158-193 → cache uniform locations per program, no per-frame snprintf), HiZ `"src"` lookup (:963,:1022 → cache like :897-899).

- [ ] **Step 1:** apply fixes; keep the stats path functional when HUD is enabled.
- [ ] **Step 2:** `cd MatterEngine3/viewer && make clean && make`; run `GALLIUM_DRIVER=d3d12` gpu_cull tests (`make test` covers them at repo level) — pass.
- [ ] **Step 3:** live check via the FIFO workflow (tools/viewer_shots.sh) — screenshots must match a pre-change reference run; viewer must self-terminate.
- [ ] **Step 4: Commit** `perf(viewer): gate GPU readback, dirty-flag instance upload, cached uniforms`.

### Task 15: MatterEngine3 viewer smells

**Files:** Modify `MatterEngine3/viewer/main.cpp`, `viewer/gpu_culler.cpp`, `viewer/world_composer.cpp`, `MatterEngine3/src/part_store.cpp` (+header), `viewer/Makefile`.

Findings: screenshot path divergence (main.cpp:469-477 vs :482-496 — route both through the `LoadImageFromScreen`+`ExportImage` helper), triplicated part-expansion walk (main.cpp:180-191, part_store.cpp:21-38, world_composer.cpp:30-56 → one traversal in part_store with a visitor callback; depth cap 8 preserved), dead `readback_batches` (gpu_culler.cpp:663-736 → move into the test TU that uses it, or delete + delete `RasterBatch` if then unused), local `mul16` dup (world_composer.cpp:9-16 → use `viewer::mul16` from raster_cull.h), Makefile shaders rule (:189-190 → validate required shader files exist, error with a clear message if not, not just `[ -e shaders ]`).

- [ ] **Step 1:** apply; clean build; `make test`; one FIFO screenshot run to verify both screenshot paths write correct absolute-path files.
- [ ] **Step 2: Commit** `refactor(viewer): unify screenshot + part-expansion paths, validate shader dir, drop dead readback`.

### Task 16: MatterEngine3 tests + tools hygiene

**Files:** Create `MatterEngine3/tests/check.h`; Modify all `MatterEngine3/tests/*.cpp` (~25 files), `tests/Makefile`, `viewer/gpu_cull_tests.cpp`, `tools/viewer_shots.sh`, `tools/meadow_sweep.sh`, `tools/meadow_forestfloor_shots.sh` (+ variants).

Findings: CHECK macro copy-pasted ×25 with 3 counter names (→ shared `tests/check.h` with one `CHECK` + `g_failures` + `check_summary()`; mechanical replacement per file), `viewer_logic_tests.cpp` mixing abort-style `assert()` with CHECK (→ all CHECK), tests/Makefile ~20 duplicate blocks (→ `COMMON_MSL_SRC` variable + pattern rule; verify identical link lines via `make -n` diff before/after), `/tmp/gpu_test_*` fixed paths (gpu_cull_tests.cpp:241,396,… → `mkdtemp` per run), camera pose set copy-pasted ×3 in tools scripts (→ `tools/lib_poses.sh` sourced by each), fixed startup sleeps (→ port `stress_sweep.sh`'s log-polling readiness wait).

- [ ] **Step 1:** check.h + mechanical migration; `GALLIUM_DRIVER=d3d12 make test` — same test counts, all green.
- [ ] **Step 2:** Makefile dedup (verify `make -n` outputs equivalent), mkdtemp, tools script dedup (run `tools/viewer_shots.sh` once end-to-end — must self-terminate).
- [ ] **Step 3: Commit** `refactor(MatterEngine3): shared test check.h, deduped test Makefile, robust tools scripts`.

---

## Phase 2 — Cross-repo (sequential, after all Phase 1 tasks merge into the branch)

### Task 17: Header dependency tracking in every Makefile (T2)

**Files:** Modify Makefiles of: BasicWindowApp, SurfaceLib, OpenParticleSurfaceLib, ParticleDynamicsExample, SpatialQueryLib, ObjectAllocatorLib, MeshChartingLib, GPURayTraceExample, MatterSurfaceLib, MatterEngine3 (root, viewer, tests) — skipping any that already do `-MMD`.

- [ ] **Step 1:** For each: add `-MMD -MP` to CFLAGS/CXXFLAGS and `-include $(OBJS:.o=.d)` (adapt to each Makefile's object list variable; for pattern rules ensure `.d` files land next to `.o`). Add `*.d` to clean targets and `.gitignore` if needed.
- [ ] **Step 2:** Verification per project: `make clean && make`, then `touch` a public header, `make` again — dependent objects MUST rebuild (spot-check one header per project with `make -d 2>/dev/null | grep -m1 remake` or just observe recompilation).
- [ ] **Step 3: Commit** `build: header dependency tracking (-MMD -MP) across all projects (T2)`.

### Task 18: Final verification + review

- [ ] **Step 1:** Full clean: `git clean -nX` review, then clean all build dirs via each project's `make clean`; run `GALLIUM_DRIVER=d3d12 bash build-all.sh test` — everything builds, all suites pass.
- [ ] **Step 2:** `git diff main --stat` review: no vendored files touched, no unintended deletions.
- [ ] **Step 3:** Run superpowers:requesting-code-review on the branch diff; fix findings.
- [ ] **Step 4:** Update `docs/code-review-2026-07-07.md` with a short "Status" section listing fixed vs deferred items (god-function splits, GPURayTraceExample main.cpp split).
- [ ] **Step 5: Commit** `docs: mark code-review findings fixed/deferred`.
