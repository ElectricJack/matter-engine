# MatterEngine2 Project Structure

This document describes the modular architecture for MatterEngine2: a single git monorepo containing many independently-buildable sub-projects.

## Repository Layout

The entire codebase lives in **one git repo at the root**. Each sub-project is a top-level directory; there are no submodules or nested repos. Per-project history from the original seven sub-repos was preserved via `git subtree` during consolidation, so `git log --all` still surfaces every original commit.

## Project Philosophy

MatterEngine2 follows a modular architecture where:

1. Each project is a standalone application that can be built and run independently from its own subdirectory
2. Projects build on each other by referencing sibling project headers via `-I../OtherProject/include` in their Makefiles
3. Compilation is fast because only the necessary code is compiled for each project
4. Testing is simplified with self-contained examples

## Project Structure

The root directory contains:

- `third_party/` - Vendored third-party dependencies (raylib, imgui, box3d, quickjs-ng, autoremesher_core, ozz-animation, flecs, Vulkan-Headers)
- `libs/` - Foundation libraries beneath MatterEngine3 in the dependency chain: `MemoryLib`, `SpatialQueryLib`, `MathLib`, `ParticleFlowLib`, `MatterSurfaceLib`, `MeshChartingLib`, `AssetStoreLib`, `ProfileLib`
- `platform.mk` - Shared build config every project's Makefile includes: TMP/TEMP export, GLSLC default, top-level `-j` parallelism, ccache detection (see "Toolchain" below)
- `build-all.sh` - Top-level script that builds every project for the current platform; `./build-all.sh test` also runs the headless test suites
- `create_project.sh` - Bootstrap a new sub-project skeleton
- Individual sub-project directories (e.g., `MatterEngine3`, `MatterEditor`)
- `Prototypes/` - Retired experiments, excluded from `build-all.sh` (see the note at the end of "Project Relationships")
- `docs/` - Design docs, findings, and `docs/agent/` (agent-facing control-surface/QA/issue-system reference — see the "QA quick reference" section below)
- This documentation file and `ROADMAP.md`

Each project follows this general structure:

```
ProjectName/
├── Makefile        # Project-specific build configuration
├── README.md       # Project documentation
├── main.c          # Main application entry point
├── include/        # Public API headers (for library projects)
│   └── *.h         # Header files defining the project's public API
└── src/            # Implementation files (for library projects)
    └── *.c         # Source code implementing the library
```

## Code Sharing Between Projects

To share code between projects while maintaining independence:

1. Library projects (`MemoryLib`, `SpatialQueryLib`, `MathLib`, `ParticleFlowLib`) organize reusable code in `include/` and `src/` directories
2. Consumer projects add `-I../OtherProject/include` to their CFLAGS **and compile the sibling's `.c`/`.cpp` directly from its source directory**. See `MatterEngine3/Makefile`, which compiles `$(SQL_DIR)/src/spatial_hash.c` and `$(MEMLIB_DIR)/src/mem_pool.c` from their source-of-truth libraries. This is the only mechanism actually in use.

**Do not copy a sibling's sources into your project.** Every duplicate in this
repo's history began as a copy made at project-creation time and then silently
diverged — `surface.c` was copied SurfaceLib → OpenParticleSurfaceLib →
MatterSurfaceLib in 2025-06 and drifted for a year, so the 2026-07 review sweep
had to land near-identical fixes in each copy. If you need a sibling's code,
compile it from where it lives.

**Do not symlink sibling sources either.** It was tried three times
(MatterSurfaceLib and GPURayTraceExample both symlinked into
OpenParticleSurfaceLib / SpatialQueryLib) and survives nowhere: git worktrees
on Windows materialise tracked symlinks as plain text files, which breaks the
build in a confusing way. The two directory symlinks the build itself used to
require were removed outright on 2026-08-14 — every Makefile now references
`libs/MatterSurfaceLib/shaders` directly (see "Worktree setup" below). Prefer
`-I` + compile-from-source for any new sharing need.

### Benefits of this approach:

- Clear dependency graph between projects
- Each project can be built and run independently
- Easy to add, remove, or modify dependencies
- Fast incremental builds
- Self-documenting structure

## Adding a New Project

To create a new project that builds on existing ones:

1. Create a new directory with the project name
2. Copy the basic structure (Makefile, main.c, etc.) from a similar project
3. Reference code from other projects via `-I` + compiling from its source directory (see "Code Sharing Between Projects" above) — never copy or symlink
4. Update the Makefile to include the necessary dependencies
5. Build and test your new project independently

## Building Projects

### Toolchain (Windows / MSYS2 UCRT64)

The project builds with GCC from MSYS2's UCRT64 environment. The compiler lives at
`C:\msys64\ucrt64\bin\g++.exe`. MSYS2's `/usr/bin/make` is used as the build driver.

Every project's Makefile (and every `tests/` sub-Makefile) starts with
`include ../platform.mk` (or `../../platform.mk` one level deeper). That file
now handles two things that used to be the caller's job:

- **TMP/TEMP.** MSYS2's make clobbers the Windows `TEMP` env var, which used to
  make GCC fail with "Cannot create temporary file in C:\WINDOWS\" unless you
  passed `TMP=`/`TEMP=` explicitly on every invocation. `platform.mk` detects
  Windows and exports both from `LOCALAPPDATA` automatically, so plain build
  commands work with no env var prefix (see below). **If you ever see that
  error again**, the auto-detection didn't fire for that invocation path (a
  nested shell that drops inherited env vars — see the comment at the top of
  `platform.mk`) — fall back to passing `TMP`/`TEMP` explicitly as before.
- **Parallelism.** The top-level invocation picks `-j$(nproc)` automatically;
  recursive submakes (`MatterEditor` → `MatterEngine3`, `MatterEngine3` →
  `tests`) inherit the jobserver rather than forcing a second `-j`. A caller
  that already passed its own `-jN` is left alone.

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

# Build kernel library
make -C MatterEngine3

# Build editor (Windows target)
make -C MatterEditor windows

# Run tests (pass GRAPHICS= on Windows since it's unset)
make -C MatterEngine3/tests run-world-definition GRAPHICS=GRAPHICS_API_OPENGL_43
```

**Note:** Test link targets that use `-lGL -lX11 -ldl -lrt` (Linux-only libs) will
fail at link time on Windows. Compilation still succeeds — the syntax/semantic check
is the important gate. Tests that don't depend on raylib or GL (like
`run-world-definition`, `run-script`, `run-evalworld`) link and run fully on Windows.

**RETOPO.** `MatterEditor/Makefile` defaults to `RETOPO=1` (autoremesher-backed
retopology). It used to require a real TBB shared library that only built on
Linux, forcing `RETOPO=0` on Windows; that was replaced with a header-only TBB
shim (`third_party/autoremesher_core/thirdparty/tbb_shim/`), so the archive is
self-contained now and the default `make -C MatterEditor windows` build does
**not** currently need `RETOPO=0`. Pass `RETOPO=0` only to skip retopo
deliberately (opting-in schemas then hit the warn-and-continue path in
`MatterEngine3/src/modifier_apply.cpp`) — re-check the Makefile's `RETOPO ?=`
line before trusting this if retopo linking has regressed since.

### Worktree setup

Git worktrees on Windows render tracked symlinks as small text files, which is
why the shader symlinks existed and then were removed (2026-08-14) rather than
kept. `setup-worktree.sh` still exists at repo root but is now a **deprecated
stub** that prints an explanation and exits 0, so the old `bash
setup-worktree.sh` habit after `git worktree add` fails loudly-but-kindly
instead of silently doing nothing.

The one remaining per-worktree step is animation-only: `*.a` files are
gitignored except a couple of named exceptions, and ozz only builds via its
own CMake/PowerShell flow, not this repo's Makefiles. If you're building an
animation target, copy the prebuilt archives from a checkout that already has
them: `third_party/ozz-animation/build/matter/**/*.a` (mirror the same
subpaths into the new worktree).

### Shaders

Vulkan shader sources live in `MatterEngine3/shaders_vk/` and compile to
embedded SPIR-V (`MatterEngine3/shaders_gen/embedded_spirv.h`) as an ordinary
build prerequisite: every object in `libmatter_engine3.a` depends on that
header, which depends on the compiled `.spv` files, which depend on their
`.comp`/`.vert`/`.frag`/`.rgen`/`.glsl` sources via explicit dependency edges.
**`make -C MatterEngine3` (the default target) rebuilds SPIR-V whenever a
shader source changes** — there is no separate `vulkan-spirv` target to
remember, so a green build can't silently keep stale SPIR-V. `MatterEditor/Makefile`
delegates to the same rule (`$(MAKE) -C ../MatterEngine3 vulkan-spirv`) rather
than keeping an independent shader list, so the two builds can never embed
different SPIR-V for the same source tree.

### Sandbox note for Claude Desktop App

When running commands via the Claude Desktop App (not CLI), the Bash tool is sandboxed.
To compile, use `dangerouslyDisableSandbox: true` on Bash tool calls. The sandbox
blocks writes to system temp directories that GCC requires.

### Standard build commands

Each project has its own Makefile and can be built independently:

```bash
cd ProjectName
make
./project_executable  # Run the compiled application
```

The build system ensures that:
- Only the code needed for the specific project is compiled
- Dependencies are correctly handled
- Each project can use different compiler flags if needed

### JS world-script tests (node)

The world scripts under `projects/*/shared-lib/` and `MatterEngine3/shared-lib/`
have Node test files (`projects/world_demo/tests/*.mjs`). There is no Makefile
target and no `package.json` anywhere in the repo, so the `.js` modules those
tests import default to CommonJS and a plain `node file.mjs` dies with
"is a CommonJS module". Run them from the repo root with:

```bash
node --experimental-default-type=module projects/world_demo/tests/alpine_ecology_tests.mjs
```

(`--experimental-detect-module` works too on Node >= 20.10.) Do not "fix" this
by adding a `package.json` — the same `.js` files are loaded by the engine's
QuickJS host, which has its own module resolution and would not see it.

## QA quick reference

Driving the editor headlessly. Full reference: `docs/agent/`.

One-shot screenshot (capture-then-quit, no FIFO needed):

```bash
cd MatterEditor
MATTER_WORLD=StreamMountain MATTER_SCREENSHOT="C:/tmp/shot.png" \
MATTER_SCREENSHOT_SETTLE=90 ./build/windows/editor.exe
```

Multi-step timeline via `MatterEngine3/tools/drive.py` (launches the editor
against a `MATTER_CMD_FIFO` file, verifies every `shot`/`shot_now` it promised
actually landed): `python MatterEngine3/tools/drive.py --world StreamMountain
--timeline shots.txt --out-dir out/ --hide-ui`, where `shots.txt` is one verb
per line:

```
wait_idle 5
set viewer.debug.lod_tint true
shot C:/tmp/out/shot1.png
wait_event bake.finished 30
quit
```

Issue replay + diff: `bash docs/baselines/capture-replay-baseline.sh
issues/<guid> /tmp/before.png` (see `docs/agent/issue-system.md`).

Full docs: `docs/agent/control-surface.md` (env vars, FIFO grammar, events),
`docs/agent/qa-cookbook.md` (copy-paste recipes), `docs/agent/issue-system.md`
(capture/file/replay), `docs/README.md` (everything else).

## Project Relationships

Current projects and their relationships. Dependencies run one way only:
**MatterEditor → MatterEngine3 → MatterSurfaceLib → SpatialQueryLib → MemoryLib.**

1. **libs/MemoryLib** - Pool / arena / growable-array allocators (C)
   - Source of truth for `mem_pool`; no dependencies

2. **libs/SpatialQueryLib** - Geometry + spatial acceleration foundation
   - Dependencies: MemoryLib (`mem_pool`)
   - Provides: `precomp.h` (float3/float4/ALIGN), `tri.h` (Tri/TriEx/mat4),
     the BVH/TLAS structures (`bvh.cpp`, `bvh_analyzer.cpp`), and the spatial hash
   - Note: the name predates its contents — it now owns the engine's core
     geometry types, not just queries

3. **libs/MathLib** - The engine's canonical math library (C++)
   - No dependencies. One documented `Vec2`/`Vec3`/`Vec4`/`Mat4`/`Quat`
     (`mm::` namespace) plus layout-compatible C-ABI mirrors in
     `matter_math_c.h`, replacing the raylib math types the engine used for
     everyday vector/matrix work — distinct from SpatialQueryLib's `precomp.h`
     SIMD types, which remain the BVH/Tri interchange format

4. **libs/ParticleFlowLib** - Particle-flow simulation, fields, path recording (C++)
   - Compiled directly into MatterEngine3 and MatterEditor

5. **libs/MatterSurfaceLib** - Meshing/surfacing backend + GPU resource management
   - Dependencies: SpatialQueryLib, MemoryLib, raylib
   - Provides: marching-cubes/CSG surfacing (`surface.c`), cluster/cell meshing,
     mesh simplification, and the BLAS/TLAS *GPU* managers (`blas_manager`,
     `tlas_manager`, `bvh_visualizer` — these own `Texture2D`/`Shader` and are
     the GL upload path, distinct from the structures in SpatialQueryLib)

6. **MatterEngine3** - Kernel library (`libmatter_engine3.a`) for the procedural engine
   - Provides: script host (QuickJS-ng DSL), bake pipeline (world_flatten/lod_bake/sector_grid),
     render subsystem (part_store/world_state/vk_scene_renderer + the Vulkan compute cull in
     `shaders_vk/cull.comp`), provider subsystem (local_provider/resolvers), facade
     (matter_engine.cpp)
   - LOD selection has ONE rule, `MatterEngine3/src/render/lod_distance.h`: rungs carry
     normalized switch DISTANCES and the cull shader plus both CPU mirrors call it. The
     header carries the equivalence proof; `make -C MatterEngine3/tests run-lod-distance`
     asserts it. Do not add a second projected-size comparison — that duplication is what
     the Representation migration exists to remove (docs/lod-vt-redesign-2026-08-04.md)
   - Build: `make -C MatterEngine3` → `build/libmatter_engine3.a` + embedded shader/SPIR-V headers
   - Tests: `make -C MatterEngine3/tests run-*` (headless) and GPU suites with `GALLIUM_DRIVER=d3d12`

7. **MatterEditor** - Interactive editor application linking the kernel library
   - Dependencies: MatterEngine3 (libmatter_engine3.a), MatterSurfaceLib, raylib (headers
     only — see below), Dear ImGui, QuickJS-ng, Box3d, optionally autoremesher_core
   - **Vulkan-only.** The GL/raylib rendering and windowing path was deleted
     outright (Phase 5a); `windows`/`linux` are Vulkan+GLFW targets and the
     link step asserts no OpenGL import survives in the binary. `raylib`
     headers remain an include-path dependency only (POD BLAS/Tri types)
   - Build: `make -C MatterEditor` → `build/linux/editor` (or `make -C MatterEditor windows` →
     `build/windows/editor.exe`); always launched from the MatterEditor/ working directory
   - Packaging: `make -C MatterEditor dist` (optionally `PROJECT=<name>`, default `world_demo`)
     → `build/dist/<PROJECT>/` — the exe plus `projects/<PROJECT>/` (minus `.cache`/`backup`),
     ready to zip and hand off; shaders are embedded in the exe, not copied

8. **libs/MeshChartingLib** - UV chart segmentation + atlas packing (GL-free)
   - No consumers today; kept for the voxel-box-imposter work

9. **libs/AssetStoreLib** - MatterStore: content-addressed blobs in append-only packs
   - Dependencies: MemoryLib only. No engine headers, no raylib, no Vulkan
   - Provides: `BlobStore` (packs + an atomically-swapped index + per-blob CRC),
     `RefTable` (opaque semantic keys, LRU against a disk budget, compaction),
     `ReadBatch` (physical-order coalesced reads landing in a caller's arena)
   - The committed index is the only authority on what exists, so a crash
     mid-append leaves nothing addressable and nothing to repair
   - Build: `make -C libs/AssetStoreLib` -> `build/libasset_store.a`;
     `make -C libs/AssetStoreLib test`, and `bench` for the pack-vs-small-files
     measurement (docs/asset-store-benchmark-2026-08-05.md)
   - **No consumers yet.** Adopting it as the engine's cache is M5's second half
     (docs/superpowers/plans/2026-08-04-lod-vt-migration.md), deliberately not
     done alongside the library's first appearance

10. **libs/ProfileLib** - Always-on lightweight profiler (C++)
    - No dependencies. Compiled into MatterEngine3 and MatterEditor. Provides
      frame-record capture, Chrome-trace export (`MATTER_PROFILE_TRACE`), and
      the in-editor Performance/Memory panels; `MATTER_PROFILE=0` compiles the
      instrumentation out

`Prototypes/` holds retired experiments (`BasicWindowApp`, `GPURayTraceExample`).
They are excluded from `build-all.sh` and their sources are frozen snapshots —
`GPURayTraceExample` in particular still contains its own diverged copies of
`blas_manager`, `tlas_manager`, `bvh_visualizer` and `precomp.h`. Do not treat
those as a reference for current engine code.

Future projects should build on these components via `-I` + compiling from the
source-of-truth directory (see "Code Sharing Between Projects" above), not by
copying or symlinking.
