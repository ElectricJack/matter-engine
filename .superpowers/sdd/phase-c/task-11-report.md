# Task 11 Report: ExplorerDemo Windows Build + Zip Packaging

## Status: DONE

## What Was Built

### Step 1: Data-dir relocation in main.cpp

Added `EXPLORER_DATA_DIR` env override with a two-tier fallback in `ExplorerDemo/main.cpp`:

1. `EXPLORER_DATA_DIR=<path>` → `<path>/schemas/`, `<path>/worlds/`, `<path>/shared-lib/`
2. `./WorldData/` exists (packaged layout) → `WorldData/schemas/`, `WorldData/worlds/`, `WorldData/shared-lib/`
3. Dev fallback: original `../MatterEngine3/examples/world_demo/...` paths (unchanged behavior when `./WorldData` absent)

`WorldDesc.world_data_dir` is set to the worlds container (e.g. `WorldData/worlds`); the engine appends `world_name` (`Meadow`) to find `world.manifest`. Added `<sys/stat.h>` + portable `dir_exists()` with `#ifdef _WIN32` guard for `_S_IFDIR`.

### Step 2: Windows target in ExplorerDemo/Makefile

Added a full Windows MinGW cross-compile stanza mirroring `MatterViewer/Makefile`:
- Compiler: `x86_64-w64-mingw32-g++-posix` (C++), `x86_64-w64-mingw32-gcc` (C)
- `WIN_ME3_CPP`: matches ME3 Makefile's `ME3_CPP` list exactly, including:
  - `retopo_blacklist.cpp` (has `#ifndef _WIN32` guard for fsync, compiles clean)
  - `inotify_watcher.cpp` (guarded by `#ifdef __linux__`, empty TU on Windows)
  - All Phase B/C additions: `async_bake.cpp`, `refine_controller.cpp`, `live_edit*.cpp`, `part_graph_snapshot.cpp`
- `WIN_MSL_CPP`: mirrors viewer's list (no `mesh_retopo.cpp` — no autoremesher)
- `WIN_PIPELINE_C`: 6 MSL C sources via `WIN_CC`
- `QJS_C`: 5 QuickJS-ng C sources
- No imgui, no `MATTER_HAVE_AUTOREMESHER` define
- `box3d`: `../Libraries/box3d/build-mingw/libbox3d.a` (tileset_settle needs it)
- Link order: `$(WIN_RAYLIB)` → `-lopengl32 -lgdi32 -lwinmm` → `libbox3d.a` → `-lwinpthread -static`
- `embedded-shaders` order-only prerequisite for C++ objects (shader_source.cpp needs header)
- Per-object build dir: `build/windows/` (separate from `build/linux/`)
- `-MMD -MP` dep tracking on all TUs

Outputs: `ExplorerDemo/explorer.exe` (7.7 MB stripped static PE)

### Step 3: package_explorer.sh

Created `ExplorerDemo/tools/package_explorer.sh`:
- Builds `explorer.exe` via `make windows`
- Stages layout: `explorer.exe`, `WorldData/schemas/`, `WorldData/worlds/Meadow/`, `WorldData/shared-lib/`, `README.txt`
- Writes `README.txt` with controls, sysreq (Windows 10+, GL 4.6, RTX 3060 recommended), cold-bake time (~2 min), and "read the .js files" note
- Zip via `zip` or `python3 -m zipfile` fallback (handles hosts without `zip` installed)
- Outputs `ExplorerDemo/dist/MeadowValley-Explorer-<YYYYMMDD>.zip`

### Gitignore updates

Added to `.gitignore`:
- `ExplorerDemo/dist/` (generated zip output)
- `ExplorerDemo/explorer.exe` (build artifact)
- `ExplorerDemo/explorer_asan` (pre-existing untracked binary)

### README.md update

Updated `ExplorerDemo/README.md` to document:
- `make -C ExplorerDemo windows` command
- Data-dir resolution priority (EXPLORER_DATA_DIR → ./WorldData → dev fallback)
- Packaged layout structure
- `package_explorer.sh` usage

## Build and Test Commands + Results

### Linux clean rebuild
```
cd ExplorerDemo && make clean && make
```
Result: Clean compile (0 errors, 0 warnings). `explorer` binary built.

### Dev-path smoke test (fallback path, ./WorldData absent)
```
cd ExplorerDemo && GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=30" ./explorer
```
Result: `explorer: bake started` → `explorer: ready` → `explorer: smoke done (30.0s, 1800 frames)` → **exit 0**

### Packaged layout smoke test (./WorldData staged to /tmp/explorer_pkg_test/)
```
cd /tmp/explorer_pkg_test && GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=60" ./explorer
```
Result: `explorer: bake started` → `explorer: ready` → `explorer: smoke done (66.1s, 3303 frames)` → **exit 0**

### Windows cross-compile
```
cd ExplorerDemo && make windows
```
Result: Clean link (0 errors; pre-existing warnings from MSL/raylib only). `explorer.exe` produced (7.7 MB).

### Zip packaging
```
bash ExplorerDemo/tools/package_explorer.sh
```
Result: `ExplorerDemo/dist/MeadowValley-Explorer-20260709.zip` — **2.8 MB**

Contents:
```
README.txt
WorldData/schemas/*.js  (14 schema files)
WorldData/shared-lib/*.js  (6 shared lib files)
WorldData/worlds/Meadow/world.manifest
explorer.exe
```

Note: 2.8 MB is the scripts-only zip (no bake cache). The exe stripped binary is 7.7 MB uncompressed; the zip compresses it well. If the exe were included as a separate download or the zip were built with `-0`, it would be ~10-15 MB; `ZIP_DEFLATED` achieves good compression on the PE binary.

## Deviations from Brief

1. **retopo_blacklist.cpp added**: The viewer's `WIN_ME3_CPP` does NOT include `retopo_blacklist.cpp`, but ME3's own Makefile does compile it (into `libmatter_engine3.a`). Since `part_flatten.cpp` includes `retopo_blacklist.h` and calls its functions unconditionally (not gated on `MATTER_HAVE_AUTOREMESHER`), omitting it would cause link errors. ExplorerDemo's `WIN_ME3_CPP` adds it. This is a latent viewer bug (the viewer's `WIN_ME3_CPP` is also missing it), noted here as a concern.

2. **inotify_watcher.cpp included**: Added explicitly since ME3's Makefile includes it. Compiles to an empty TU on Windows due to `#ifdef __linux__` guard.

3. **zip size 2.8 MB**: The brief estimated "tens of MB" for the zip. This is because we compress the PE binary well and include no bake cache. The actual compressed size is ~2.8 MB (scripts: ~40 KB; exe compressed: ~2.7 MB). This is well within the < 100 MB cap.

4. **Step 4 (Windows exe test)**: Skipped as per brief instructions. Jack should double-click `explorer.exe` on Windows and observe: first-run bake (~2 min), then fly-through. The exe is a static PE32+ binary and should run on Windows 10+ with a GL 4.6 GPU.

## Concerns

1. **Viewer WIN_ME3_CPP missing retopo_blacklist.cpp**: The viewer's windows stanza would have undefined-symbol link errors for `retopo_blacklist::init/is_blacklisted/begin_attempt/end_attempt` if it were compiled with the full ME3 source list. It compiles today because I didn't change the viewer's Makefile, but if Jack ever adds retopo calls in another path, the viewer windows build will break. This is pre-existing and out of scope for Task 11.

2. **cache/ directory for Windows**: The packaged zip has no `cache/` subdirectory. On first run, `EngineContext::create` will create it relative to the working directory. Windows users must run `explorer.exe` from the same directory where the exe lives (or the cache will land in `%CD%`). A `cache/` placeholder dir could be added to the zip but is not required by the brief.

3. **EXPLORER_DATA_DIR on Windows**: The env var override uses forward slashes in string concatenation. Windows accepts both `/` and `\` in paths via MinGW/MSVC runtimes, so this should work.
