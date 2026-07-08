# Task 8 Report: Stage 5a — Pure `git mv` Renames

**Branch:** `feature/phase-a-kernel-extraction`
**Commits:** a58840d, 5e58bb0, 75d8e7f

---

## Shaders Symlink Finding

`git ls-files -s MatterEngine3/viewer/shaders` returned **nothing** — there is no
git-tracked entry for `shaders` at all. On disk, `MatterEngine3/viewer/shaders/` is a
real directory containing:
- `bvh_tlas_common.glsl`, `fullscreen.vs`, `lighting.glsl`, `materials.glsl`,
  `raytrace_tlas_blas.fs`, `raytrace_tlas_blas_processed.fs` (regular files)
- `raster.fs -> ../../../MatterSurfaceLib/shaders/raster.fs` (untracked local symlink)
- `raster.vs -> ../../../MatterSurfaceLib/shaders/raster.vs` (untracked local symlink)
- `tileset_sampling.glsl -> ../../../MatterSurfaceLib/shaders/tileset_sampling.glsl`
  (untracked local symlink)

A previous implementer added these three symlinks locally without committing them.
Since no git-tracked `shaders` entry exists, `git mv viewer/shaders shaders` cannot be
performed. **No `git mv` for shaders was done.** The entire `viewer/shaders/` directory
remains in place untracked; the three MSL symlinks are regenerable via `make shaders`.

---

## Commit 1: Kernel material into the library tree

**Hash:** `a58840d5757964fd45a269f0b3a209902648386b`

72 files changed, all renames (0 insertions, 0 deletions).

### Files moved to `MatterEngine3/src/render/`:
- `viewer/renderer.h`, `viewer/renderer.cpp`
- `viewer/raster_composer.h`, `viewer/raster_composer.cpp`
- `viewer/raster_mesh.h`, `viewer/raster_mesh.cpp`
- `viewer/part_store.h`, `viewer/part_store.cpp`
- `viewer/world_composer.h`, `viewer/world_composer.cpp`
- `viewer/world_state.cpp` (no `world_state.h` existed)
- `viewer/gpu_culler.h`, `viewer/gpu_culler.cpp`
- `viewer/probe_texture.h`, `viewer/probe_texture.cpp`
- `viewer/tileset_provider.h`, `viewer/tileset_provider.cpp`
- `viewer/tileset_gl_ctx.h`, `viewer/tileset_gl_ctx.cpp`
- `viewer/raster_cull.h`, `viewer/gl46.h`, `viewer/gpu_cull_types.h`

### Files moved to `MatterEngine3/src/provider/`:
- `viewer/local_provider.h`, `viewer/local_provider.cpp`
- `viewer/world_source.h`
- `viewer/sector_resolver.h`
- `viewer/resolvers.cpp` (brief named this `sector_resolver.cpp` — see Deviations)

### Facade moved to `MatterEngine3/src/`:
- `viewer/matter_engine.cpp`

### Flat internal headers moved to `MatterEngine3/src/` (39 headers):
All files matching `include/*.h` excluding `include/matter/*` (the public API).
Includes `include/world_tracer.h` (added by Task 7 as noted in task context).

### `shaders_gpu` moved to `MatterEngine3/shaders_gpu/`:
5 tracked files: `cull.comp`, `hiz_downsample.comp`, `raster_gpu_driven.vs`,
`tileset_bake_ao.comp`, `tileset_bake_primary.comp`.

**Verification (`git show --stat`):** All 72 lines are renames (`=>`).

---

## Commit 2: Viewer app to top-level `MatterViewer/`

**Hash:** `5e58bb0bab5d69c58487ef02ef2bd3cc69e71757`

4 files changed, all renames.

- `MatterEngine3/viewer/main.cpp` → `MatterViewer/main.cpp`
- `MatterEngine3/viewer/ui.h` → `MatterViewer/ui.h`
- `MatterEngine3/viewer/ui.cpp` → `MatterViewer/ui.cpp`
- `MatterEngine3/viewer/Makefile` → `MatterViewer/Makefile`

**Runtime cache:** `MatterEngine3/viewer/cache/` is 2.4 GB of untracked bake output.
NOT copied (`cp -r` on 2.4 GB was deferred — too large for a background task; Task 9
Makefile update should point `CACHE_DIR` to the new path or the cache can be relinked).

**Verification (`git show --stat`):** All 4 lines are renames (`=>`).

---

## Commit 3: GPU tests into the kernel test tree

**Hash:** `75d8e7fbbea30024a7aa84afb6ab4c648e6af589`

5 files changed, all renames.

- `MatterEngine3/viewer/gpu_cull_tests.cpp` → `MatterEngine3/tests/gpu_cull_tests.cpp`
- `MatterEngine3/viewer/tileset_gpu_tests.cpp` → `MatterEngine3/tests/tileset_gpu_tests.cpp`
- `MatterEngine3/viewer/tileset_load_tests.cpp` → `MatterEngine3/tests/tileset_load_tests.cpp`
- `MatterEngine3/viewer/tileset_provider_tests.cpp` → `MatterEngine3/tests/tileset_provider_tests.cpp`
- `MatterEngine3/viewer/tileset_seam_tests.cpp` → `MatterEngine3/tests/tileset_seam_tests.cpp`

**Verification (`git show --stat`):** All 5 lines are renames (`=>`).

---

## State of `MatterEngine3/viewer/` after Commit 3

`git ls-files MatterEngine3/viewer/` → **empty** (no tracked files remain).

Untracked leftovers in `MatterEngine3/viewer/`:
```
api_tests          (binary, build output)
build/             (build artifacts)
cache/             (2.4 GB bake cache — untracked)
gpu_tests          (binary)
imgui.ini          (runtime config)
shaders/           (real directory with 6 regular shader files + 3 local MSL symlinks)
tileset_gpu_tests  (binary)
tileset_load_tests (binary)
tileset_provider_tests (binary)
tileset_seam_tests (binary)
viewer             (binary — the compiled viewer)
```

All leftovers are build artifacts, runtime config, or the untracked shaders directory.
Task 9 should clean these up (`.gitignore` or removal).

---

## Deviations from Brief

1. **`viewer/shaders` not moved:** Brief assumed a git-tracked symlink entry; in reality
   it is an untracked real directory. No `git mv` possible. Left in place.

2. **`sector_resolver.cpp` → `resolvers.cpp`:** Brief named this file `sector_resolver.cpp`
   but the actual tracked filename is `resolvers.cpp`. Moved `resolvers.cpp` to
   `src/provider/` as-is (no rename of filename, just directory).

3. **`tileset_bake_ao.cpp` and `tileset_bake_primary.cpp` not moved from viewer/:** Brief
   listed them under Commit 1's render moves. They were already present in
   `MatterEngine3/src/` before this task (placed there by an earlier phase). No such
   files existed in `viewer/`.

4. **`world_state.h` missing:** Brief listed it for the render move; only `world_state.cpp`
   exists. Only the `.cpp` was moved.

5. **`gpu_cull_types.h` added to render/:** This tracked file was in `viewer/` but not
   listed in the brief. It belongs with the render sources and was moved to `src/render/`.

6. **Runtime cache not copied:** `MatterEngine3/viewer/cache/` is 2.4 GB; deferred to
   Task 9 to wire the Makefile to the existing cache location or re-point CACHE_DIR.

---

## Concerns for Task 9

- The `MatterViewer/Makefile` will need `-I../MatterEngine3/src/render`,
  `-I../MatterEngine3/src/provider`, `-I../MatterEngine3/src`, and
  `-I../MatterEngine3/include` path updates (old paths referenced `viewer/` siblings).
- `resolvers.cpp` was renamed to `resolvers.cpp` (no filename change) but previously
  included via `sector_resolver.cpp` in some Makefile — check `MatterViewer/Makefile`
  source list.
- The untracked `viewer/shaders/` directory needs either a `make shaders` target
  redirect pointing to `MatterViewer/shaders/` or a symlink from `MatterViewer/shaders`
  → `../MatterEngine3/viewer/shaders` (or the shader files moved manually).
- `viewer/cache/` (2.4 GB) should be left at its current path and the Makefile
  `CACHE_DIR` or equivalent updated to `../MatterEngine3/viewer/cache` from
  `MatterViewer/`.
