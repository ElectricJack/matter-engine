# Tech Debt

Known debt with evidence, so nobody has to re-derive it. Each item records what
was **ruled out** as well as what's outstanding — several of these look worse
than they are, and one looks smaller than it is.

Nothing here is scheduled. Last audited 2026-07-25.

---

## 1. `Matrix4x4` duplicates `mat4` — with conversion shims between them

**The one real duplicate in the math layer.**

`libs/MatterSurfaceLib/src/tlas_manager.cpp` carries a full 4×4 matrix suite on its
own `Matrix4x4` type, sitting beside `mat4` (from `libs/SpatialQueryLib/include/tri.h`)
in the same file, with explicit converters between the two:

```cpp
mat4      TLASManager::convert_matrix(const Matrix4x4& legacy_matrix);  // :15
Matrix4x4 TLASManager::convert_matrix_back(const mat4& new_matrix);     // :23
```

Both are row-major `float[16]`. Same layout, same convention, same layer,
adjacent classes. The parameter name `legacy_matrix` suggests this was known.

**Where it came from.** The nine functions are a C++ port of the matrix API that
lived in SpatialQueryLib's C BVH — the implementation deleted in `c3f0577a` as
having no consumers. It had no consumers; its *matrix half* had already been
copied here and is load-bearing.

| Deleted `libs/SpatialQueryLib/include/bvh.h` (see `c3f0577a^`) | Live in `tlas_manager.cpp` |
|---|---|
| `matrix_identity`, `matrix_multiply`, `matrix_inverse`, `matrix_translation`, `matrix_scale`, `matrix_rotation_x/y/z/axis` | all nine, identical names and `const Matrix4x4*` signatures |
| `matrix_transform_point`, `matrix_transform_vector` | not carried over |

**Still load-bearing** — not vestigial:
- `libs/MatterSurfaceLib/include/tlas_manager.hpp:21` (definition; 14 uses in that header)
- `MatterEngine3/src/tileset_torus_bvh.cpp:162,169,299` — builds transforms from poses
- `MatterEngine3/src/render/tileset_bake_vk.cpp:420` — row-major → `VkTransformMatrixKHR`
- `MatterEngine3/tests/part_asset_v2_tests.cpp:96-99`

**Partially fixed in `a48a71f0` (Phase 2). Do not close this item.** The nine
functions and both *named* shims are gone, and `Matrix4x4` no longer exists —
every use moved to `mm::Mat4` (`libs/MathLib`). The dead one, `matrix_inverse`,
had zero callers tree-wide and was deleted outright.

But the fix as written above said "merge onto `mat4`", and that is not what
happened: the merge went onto `mm::Mat4`, leaving `mat4` (SpatialQueryLib
`tri.h`) in place. **The duplicate this item describes therefore survives
verbatim** — two identical row-major `float[16]` types in the same layer — and
`convert_matrix` was not eliminated so much as *inlined*, as the element-copy
loop at `libs/MatterSurfaceLib/src/tlas_manager.cpp:228-231`.

Restated: **`mm::Mat4` duplicates `mat4`, with an open-coded converter between
them.** Closing it means deciding whether `mat4` becomes an alias for `mm::Mat4`
or SpatialQueryLib keeps its own. That decision is downstream of item 4 and was
deliberately not taken in Phase 2.

Related dead weight found while reviewing: `DrawRecord::inv_transform`
(`tlas_manager.hpp`) is never read — the GPU upload path uses
`BVHInstance::GetInvTransform()` instead.

---

## 2. Six matrix inverses, four different failure modes

This is a **correctness** item, not tidiness. The duplication is secondary; the
divergent handling of singular input is the actual hazard.

| Implementation | Type | On singular input |
|---|---|---|
| `MatterEngine3/src/render/matrix_math.cpp:120` `mat4_inverse` | `Mat4f` | returns `false` — caller decides |
| `libs/SpatialQueryLib/include/tri.h:57` `mat4::Inverted` | `mat4` | returns **identity** |
| `MatterEngine3/src/csg_lowering.cpp:24` `mat_invert` | raylib `Matrix` | returns **zero matrix** (guard at `:37`) |
| `libs/MatterSurfaceLib/src/tlas_manager.cpp:49` `matrix_inverse` | `Matrix4x4` | — |
| `MatterEngine3/src/world_tracer.cpp:34` `invert4x4` | raw `float*` | cofactor expansion |
| `MatterEngine3/src/matter_engine.cpp:197` `invert4x4` | raw `float*` | adjugate form — **dead code** |
| `third_party/raylib/src/raymath.h:1538` `MatrixInvert` | raylib `Matrix` | **no guard** — computes `1.0f/det`, yields inf/NaN |

A degenerate transform silently becomes identity on one path, a zero matrix on
another, and NaN on a third.

The last two share a **name and signature** — `static bool invert4x4(const
float*, float*)` — in two translation units of the same project, with different
bodies (`world_tracer` uses cofactor expansion, `matter_engine` the classic
adjugate/`d[16]` form). Both are `static`, so no ODR clash; it is parallel
reinvention. `matter_engine.cpp`'s copy is unused — the compiler reports
`defined but not used`, which is how it was found.

**Ruled out:** `csg_lowering::mat_invert` looks like a gratuitous reimplementation
of raymath's `MatrixInvert` for the same type — its own comment even says it
"matches raymath's MatrixInvert semantics". It is **not** gratuitous: raymath has
no zero-determinant guard, and `mat_invert` adds one. The reimplementation exists
*for* the guard. Don't delete it without replacing the guard.

**Fix.** Pick one singular-matrix policy and apply it everywhere; delete the dead
`matter_engine.cpp` copy outright.

---

## 3. Three matrix multiplies, because there are three matrix types

All inside MatterEngine3:

- `MatrixMultiply` (raymath) — `src/dsl_state.cpp:31-40,76`, the DSL transform stack
- `mat4_mul` (on `Mat4f`) — `src/matter_engine.cpp:4513`, `src/provider/resolvers.cpp:107`
- `mul16` (raw `float[16]`) — `src/mat_math.h:27`, used by `src/part_flatten.cpp:336,421`

Not duplicates of each other in the strict sense — different types — but they
exist *because* of item 4. Folding them is downstream of that decision.

---

## 4. Three vector/matrix type families — **deliberate; do not unify**

Recorded so this isn't re-audited as if it were debt.

| Family | Home | Reach | Role |
|---|---|---|---|
| `float3`/`float4`/`mat4` | `libs/SpatialQueryLib/include/{precomp,tri}.h` | **61 files** | SIMD-aligned, shaped for BVH build |
| `matter::Float3`/`Float4`/`Quaternion`/`Mat4f` | `MatterEngine3/include/matter/math_types.h` | **24 files** | the exported engine API boundary |
| raylib `Vector3`/`Matrix`/`Quaternion` | vendored | DSL/CSG layer | where the code is raylib-facing |

**The reason not to unify:** there is no conversion tax to recover. A search for
cross-family converters found exactly one — `row_major_to_matrix`
(`MatterEngine3/src/render/raster_mesh.cpp:158`, declared `.h:40`). The families
are cleanly separated by layer, not tangled. Unifying would touch 85+ files to
remove a cost nobody is paying.

Revisit only if conversions start appearing at layer boundaries.

---

## 5. Verified **not** debt — don't re-audit these

| Thing | Why it's legitimately separate |
|---|---|
| `libs/MatterSurfaceLib/src/mesh_simplifier.cpp:80` `V3` | **double** precision — Garland-Heckbert quadrics need it. Comment: "avoids raymath coupling" |
| `libs/ParticleFlowLib/include/particle_flow.h:14` `pf::V3` | deterministic sim, header-only public API |
| `MatterEngine3/shared-lib/vecmath.js` | QuickJS scripting runtime — different language |
| `MatterEngine3/src/render/vk_gi_math.*` | BRDF/GGX sampling, builds on `matter::Float3` |
| `MatterEngine3/src/render/gpu_matrix_pack.h:7` `GpuMat4` | GPU packing layout |
| `MatterEngine3/src/mat_math.h:46` `NormalMat` | 3×3 inverse-transpose for normals |
| `terrain_mesher.cpp:12`, `mesh_charting.cpp:10`, `selection_outline.cpp:11` | POD structs, **zero** operations defined |

Minor and real: `MatterEditor/viewport_pick.cpp:17` defines `Vec3` plus
`add`/`cross`/`normalize`/`scale`/`sub` with no precision or determinism reason.

---

## 6. GL BLAS/TLAS upload machinery — delete with the GL path

The GL ray-tracing path is being replaced by Vulkan. When it goes, delete rather
than refactor:

- `textures_dirty_`, `shader_values_dirty_`, per-entry `gpu_dirty`, the two
  `Texture2D` members, `ensure_gpu_textures_ready()`, `bind_to_shader()`
  (BLAS and TLAS managers)
- `libs/MatterSurfaceLib/main.cpp:1150,1151,1496` also calls `bind_to_shader`

`content_revision()` (landed `b11d36fc`) is the backend-agnostic replacement: a
Vulkan uploader holds its own `seen_content_revision_` and skips when equal.
O(1), and per-consumer — a shared dirty *flag* is cleared by whoever services it
first, silently starving a second uploader.

**Two GL-specific facts to carry forward**, currently recorded only as comments
in code slated for deletion:
1. `bind_to_shader` stages textures **every frame regardless of dirty state** —
   raylib's batch resets `activeTextureId` after each draw.
2. `TLASManager::mark_dirty` resets `cached_shader_id_ = 0` — GL reuses program
   ids after a shader is deleted, so a cached uniform location can silently
   belong to a stale program.

---

## 7. Smaller open items

- **`Prototypes/GPURayTraceExample` duplicates.** 11 file pairs against
  MatterSurfaceLib (`blas_manager`, `tlas_manager`, `bvh_visualizer`, …), plus
  `precomp.h` and `bvh.{cpp,h}` against SpatialQueryLib. `precomp.h` is still
  **byte-identical** to SpatialQueryLib's. No maintenance cost while parked and
  out of `build-all.sh`; the whole class disappears if that prototype is ever
  deleted. It is the last duplicate cluster left in the tree.

- ~~**`libs/MatterSurfaceLib/Makefile` hardcodes `x86_64-w64-mingw32-g++-posix`**~~
  — **fixed** in the Phase 6 sweep (`f674f773`): native-Windows branch now uses
  the plain `x86_64-w64-mingw32-g++` that MSYS2 UCRT64 actually installs. The
  Linux/WSL cross-compile branches, which use a real `-posix` package, were
  left untouched.

- **ASan is unavailable** in MSYS2 UCRT64 (`cannot find -lasan`).
  `SpatialQueryLib` and `MemoryLib` `test` targets are sanitizer-based, so on
  Windows they are quietly weaker than the Makefile implies.

- **`SpatialQueryLib`'s name under-describes it.** It owns `float3`/`float4`/
  `Tri`/`mat4` and the BVH — a geometry + acceleration foundation, not a query
  library. Renaming is churn across ~20 include paths; deliberately deferred.

- **`origin/main` is behind local `main`** by the full consolidation series plus
  ~65 earlier commits. Independent of the work above.

---

## 8. Broken tests — all pre-existing, all found while moving paths

None of these were introduced by the layout work; each was verified present
before it. They are grouped here because they surfaced together. The first
five items below were **fixed by the Phase 6 sweep (`f674f773`)**; kept for
the paper trail. The last item is new and still open.

- ~~**Six test files reference `projects/world_demo/schemas/`**~~ — **fixed**.
  `WorldSector.js` moved to `objects/` in `83f171c9`; `sector_bake_tests.cpp`,
  `rock_bake_profile.cpp`, `tree_bake_check.cpp`, `grass_lod_tests.cpp`,
  `stress_forest_tests.cpp` and `part_graph_integration_tests.cpp` were
  repointed. `sector_bake_tests` no longer fails on `WorldSector.js readable`.
- ~~**`realpath` is not declared under UCRT64**~~ — **fixed**. The six
  duplicated `abspath()`/`realpath()` helpers (`gallery_bake_tests`,
  `example_world`, `grass_lod_tests`, `stress_forest_tests`, and two more —
  the original count of four undercounted this) were replaced by one shared
  `MatterEngine3/tests/portable_realpath.h` that branches to `_fullpath` on
  `_WIN32`.
- ~~**`lighting_garden_tests.cpp:272` uses an undeclared `schemas`**~~ — **fixed**,
  same repoint as the six-file item above.
- ~~**`TILESETMEADOWMANIFEST_CPP` omits `world_definition_loader.cpp`**~~ —
  **fixed**; `tileset_meadow_manifest_tests` links and passes (5 run, 0 failed).
  Doing this exposed two further build-graph bugs, both since fixed: the same
  source got wired into `EXAMPLE_CPP` for the identical reason and ended up
  double-listed in `VIEWER_LOGIC_CPP` (duplicate-object link failure on
  `run-viewer-logic` — §10), and it lingered in `def_CPP_SRCS` without
  `QJS_INC` (dormant `quickjs.h: No such file or directory` the moment any
  `def`-flavor target reuses those sources — also §10).
- ~~**`material_registry_tests.cpp` has a wrong-depth include**~~ — **fixed**;
  `../../MatterEngine3/...` corrected to account for tests sitting three
  levels below the repo root.

- **The schema-path repoint above is correct, but the affected suites still
  don't pass on Windows** — for an unrelated, older reason: they build their
  sandbox directories with `system("rm -rf ... && mkdir -p ...")`, a POSIX-shell
  assumption. Under `cmd.exe` this prints `The syntax of the command is
  incorrect.` and the sandbox never gets created, so every test that depends on
  it fails at `chdir`. Verified by actually running each target
  (UCRT64/Windows, this audit — do not trust smaller numbers quoted elsewhere
  without re-running):
  - `run-sectorbake`: **7 failures**, exit 2 (`FAIL: save_v2 failed`, `FAIL:
    deterministic hash`, ... — down from an earlier 11, but still red)
  - `run-example`, `run-gallery`, `run-treebake`, `run-rock-profile`: **1
    failure each** — `FAIL: chdir sandbox`, exit 2
  - `run-grasslod`: **11 failures**, exit 2
  - `run-stressforest`: **3 failures** (`FAIL: chdir(...)` x2 +
    `FAIL: both sandboxes completed bake + flatten`), exit 2
  - `run-graph-integration`: **20 failures**, exit 2 — a previously-quoted
    figure of 11 for this target is wrong; the binary prints 20 distinct
    `FAIL:` lines (no `N FAILURE(S)` summary line is printed for this suite)
  None of this is new and none of it was introduced by the schema repoint —
  the `system()` calls predate it. Fix is mechanical: replace each
  `system("rm -rf X && mkdir -p X")` with `std::filesystem::remove_all` +
  `create_directories`, the same pattern `viewer_logic_tests.cpp`'s
  `reset_test_dir()`/`ensure_parts_dir()` already use.

## 9. Build and packaging

- **`STREAMLINE_DLL_DIR` defaults disagree.** `MatterEditor/Makefile:78` uses
  `$(STREAMLINE_PATH)/bin/x64` (16 release DLLs); `tools/check_vulkan_toolchain.sh:10`
  and every historical build use `bin/x64/development` (18, adds `sl.imgui` and
  `sl.nvperf`). Relying on the Makefile default silently ships a different DLL
  set than the project has ever shipped. `build-dlss.sh` passes it explicitly to
  sidestep this; the defaults should be reconciled.
- **`build_features.txt` records `VULKAN`/`OPENGL` but not `STREAMLINE`**, so a
  built binary carries no record of whether DLSS is compiled in. `strings
  editor.exe | grep active_dlss_mode` is the current workaround.
- **`raytrace_tlas_blas_processed.fs` is generated but committed.** Produced by
  MatterSurfaceLib's `shader_preprocessor`, consumed by MatterEngine3's
  embedded-shaders step through the `MatterEngine3/shaders` junction. `clean`
  used to delete it, which broke every downstream build until a `git
  checkout`; that was fixed and a deliberate `make regen-shaders` added, but
  the underlying generated-yet-committed status remains, so `all: shaders
  ...` still regenerates it on every build. Two prior blockers to regenerating
  it under stock MSYS2 UCRT64 are now both fixed: the `g++-posix` compiler gap
  (§7, fixed in `f674f773`), and `shader_preprocessor.cpp` opening its output
  `ofstream` in text mode, which on Windows silently rewrote every `\n` to
  `\r\n` — a 69,161-byte LF file regenerated as 70,859 bytes of CRLF, a
  whole-file diff on every touch. Fixed by opening the output stream with
  `std::ios::binary` (`libs/MatterSurfaceLib/src/shader_preprocessor.cpp`,
  `process_file()`). Verified: `make -C libs/MatterSurfaceLib shaders`
  reproduces the committed file byte-for-byte (`git status` clean afterward).
  The generated-yet-committed status itself is unchanged; regenerating is
  just no longer a trap.
- **Tracked symlinks vs NTFS junctions.** `MatterEngine3/shaders`,
  `MatterEditor/shaders` and `MatterEditor/shaders_gpu` are tracked as mode
  120000 but exist here as junctions (`core.symlinks=false`). `git add <parent>`
  descends into them and replaces each symlink with a directory of duplicated
  blobs; `skip-worktree` does NOT prevent it, because the damage creates new
  paths rather than modifying the flagged one. Now guarded by trailing-slash
  `.gitignore` rules — do not remove them.

## 10. This audit's fixes (build-graph bugs, both resolved)

- **`run-viewer-logic` failed to link: duplicate object in `VIEWER_LOGIC_OBJS`.**
  `MatterEngine3/tests/Makefile`'s `EXAMPLE_CPP` gained
  `../src/script/world_definition_loader.cpp` (needed — `example_world` and
  `lighting_garden_tests` require it). `VIEWER_LOGIC_CPP` (:746) already
  listed that same file explicitly *and* appends `VIEWER_PIPELINE_CPP` (:745,
  `$(filter-out example_world.cpp,$(EXAMPLE_CPP))`), so the file landed in the
  link line twice. `obj_list` (:261) does not `$(sort)`, so `ld` saw
  `up__src__script__world_definition_loader.cpp.o` twice and failed with
  `multiple definition of matter::load_world_definition`. Fixed by deleting
  the now-redundant explicit line — the file already arrives transitively via
  `VIEWER_PIPELINE_CPP` — rather than touching the shared `obj_list` macro
  (used by ~65 other `*_OBJS` variables) or `EXAMPLE_CPP` itself (shared by
  `GALLERY_CPP`/`TREEBAKE_CPP`/`ROCKPROF_CPP`/`GRASSLOD_CPP`/
  `STRESSFOREST_CPP`/`LIGHTING_GARDEN_CPP`). Audited every other `*_OBJS`
  variable in the file for the same hazard (mechanically, by expanding each
  one and diffing total vs. unique object-file count) — this was the only one
  affected. `run-viewer-logic` now links and runs. On a byte-fresh
  `%TEMP%/me3_viewer_cache_test` it gets to exactly one remaining failure —
  `FAIL: passthrough composes every instance plus its children` — traced to
  `WorldComposer::compose()`'s instance count diverging from the test's own
  count for the 2 of 3 demo-manifest parts whose `flatten()` legitimately
  produces no mesh (`flatten: merged mesh is empty`, by design, not a bug in
  anything this pass touched). That failure, `viewer_logic_tests.cpp`,
  `world_composer.cpp`, `local_provider.cpp`, `part_store.cpp` and the
  `world_demo` project content are all byte-identical between `f674f773` and
  current `HEAD`, so it predates and is independent of this fix — left open,
  not fixed here. (A *stale* local cache — leftover `.flat.part` files from
  an older on-disk format — makes it look far worse: 4 failures including
  `load_flat_v3 failed`, reproduced once during this audit. Wipe
  `%TEMP%/me3_viewer_cache_test` before trusting a run's failure count.)
- **Dormant `def`-flavor breakage in `TILESETMEADOWMANIFEST_CPP`.** The same
  commit moved `TILESETMEADOWMANIFEST_OBJS` to the `qjs` flavor (needs
  `QJS_INC` to find `quickjs.h`) but left `$(TILESETMEADOWMANIFEST_CPP)` — now
  including `world_definition_loader.cpp` + `module_resolver.cpp` — in
  `def_CPP_SRCS` too. `FLAVOR_def_FLAGS` (:223) has no `$(QJS_INC)`, so any
  future `def`-flavor target reusing those sources would fail with `quickjs.h:
  No such file or directory`. Nothing requests the `def`-flavor object today,
  so it was silent. Fixed by removing `$(TILESETMEADOWMANIFEST_CPP)` from
  `def_CPP_SRCS` (:886) — it only needs to be listed once, under `qjs_CPP_SRCS`
  (:918), which is where the real target (`TILESETMEADOWMANIFEST_OBJS`) draws
  from. `tileset_meadow_manifest_tests`: 5 run, 0 failed.
