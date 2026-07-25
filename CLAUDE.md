# MatterEngine2 Project Structure

This document describes the modular architecture for MatterEngine2: a single git monorepo containing many independently-buildable sub-projects.

## Repository Layout

The entire codebase lives in **one git repo at the root**. Each sub-project is a top-level directory; there are no submodules or nested repos. Per-project history from the original seven sub-repos was preserved via `git subtree` during consolidation, so `git log --all` still surfaces every original commit.

## Project Philosophy

MatterEngine2 follows a modular architecture where:

1. Each project is a standalone application that can be built and run independently from its own subdirectory
2. Projects build on each other by referencing sibling project headers via `-I../OtherProject/include` in their Makefiles (or, where convenient, via filesystem symlinks)
3. Compilation is fast because only the necessary code is compiled for each project
4. Testing is simplified with self-contained examples

## Project Structure

The root directory contains:

- `third_party/` - Vendored third-party dependencies (raylib, imgui, box3d, quickjs-ng, autoremesher_core, Vulkan-Headers)
- `libs/` - Foundation libraries beneath MatterEngine3 in the dependency chain: `MemoryLib`, `SpatialQueryLib`, `ParticleFlowLib`, `MatterSurfaceLib`, `MeshChartingLib`
- `build-all.sh` - Top-level script that builds every project for the current platform; `./build-all.sh test` also runs headless test suites
- `create_project.sh` - Bootstrap a new sub-project skeleton
- Individual sub-project directories (e.g., `MatterEngine3`, `MatterEditor`, `BasicWindowApp`)
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

1. Library projects (`MemoryLib`, `SpatialQueryLib`, `ParticleFlowLib`) organize reusable code in `include/` and `src/` directories
2. Consumer projects add `-I../OtherProject/include` to their CFLAGS **and compile the sibling's `.c`/`.cpp` directly from its source directory**. See `MatterEngine3/Makefile`, which compiles `$(SQL_DIR)/src/spatial_hash.c` and `$(MEMLIB_DIR)/src/mem_pool.c` from their source-of-truth libraries. This is the only mechanism actually in use.

**Do not copy a sibling's sources into your project.** Every duplicate in this
repo's history began as a copy made at project-creation time and then silently
diverged — `surface.c` was copied SurfaceLib → OpenParticleSurfaceLib →
MatterSurfaceLib in 2025-06 and drifted for a year, so the 2026-07 review sweep
had to land near-identical fixes in each copy. If you need a sibling's code,
compile it from where it lives.

Symlinks to sibling sources were tried three times (MatterSurfaceLib and
GPURayTraceExample both symlinked into OpenParticleSurfaceLib / SpatialQueryLib)
and survive nowhere: git worktrees on Windows materialise tracked symlinks as
plain text files, which breaks the build in a confusing way. Prefer the `-I` +
compile-from-source approach above. Directory symlinks that the build genuinely
requires are recreated as NTFS junctions by `setup-worktree.sh`.

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
3. Create symlinks to code from other projects you want to reuse
4. Update the Makefile to include the necessary dependencies
5. Build and test your new project independently

## Building Projects

### Toolchain (Windows / MSYS2 UCRT64)

The project builds with GCC from MSYS2's UCRT64 environment. The compiler lives at
`C:\msys64\ucrt64\bin\g++.exe`. MSYS2's `/usr/bin/make` is used as the build driver.

**Critical: TEMP variable fix.** MSYS2's make clobbers the Windows TEMP env var,
causing GCC to fail with "Cannot create temporary file in C:\WINDOWS\". Always pass
TEMP explicitly:

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

# Build kernel library
make -C MatterEngine3 \
  TMP="C:/Users/webde/AppData/Local/Temp" \
  TEMP="C:/Users/webde/AppData/Local/Temp"

# Build editor (Windows target)
make -C MatterEditor windows \
  TMP="C:/Users/webde/AppData/Local/Temp" \
  TEMP="C:/Users/webde/AppData/Local/Temp"

# Run tests (pass GRAPHICS= on Windows since it's unset)
make -C MatterEngine3/tests run-world-definition \
  TMP="C:/Users/webde/AppData/Local/Temp" \
  TEMP="C:/Users/webde/AppData/Local/Temp" \
  GRAPHICS=GRAPHICS_API_OPENGL_43
```

**Note:** Test link targets that use `-lGL -lX11 -ldl -lrt` (Linux-only libs) will
fail at link time on Windows. Compilation still succeeds — the syntax/semantic check
is the important gate. Tests that don't depend on raylib or GL (like
`run-world-definition`, `run-script`, `run-evalworld`) link and run fully on Windows.

### Worktree setup (symlinks)

Git worktrees on Windows render tracked symlinks as small text files. Run the setup
script from repo root after creating a worktree:

```bash
bash setup-worktree.sh
```

This creates NTFS junctions for the three directory symlinks the build requires:
- `MatterEngine3/shaders` → `libs/MatterSurfaceLib/shaders`
- `MatterEditor/shaders` → `libs/MatterSurfaceLib/shaders`
- `MatterEditor/shaders_gpu` → `MatterEngine3/shaders_gpu`

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

3. **libs/ParticleFlowLib** - Particle-flow simulation, fields, path recording (C++)
   - Compiled directly into MatterEngine3 and MatterEditor

4. **libs/MatterSurfaceLib** - Meshing/surfacing backend + GPU resource management
   - Dependencies: SpatialQueryLib, MemoryLib, raylib
   - Provides: marching-cubes/CSG surfacing (`surface.c`), cluster/cell meshing,
     mesh simplification, and the BLAS/TLAS *GPU* managers (`blas_manager`,
     `tlas_manager`, `bvh_visualizer` — these own `Texture2D`/`Shader` and are
     the GL upload path, distinct from the structures in SpatialQueryLib)

5. **MatterEngine3** - Kernel library (`libmatter_engine3.a`) for the procedural engine
   - Provides: script host (QuickJS-ng DSL), bake pipeline (world_flatten/lod_bake/sector_grid),
     render subsystem (renderer/raster_composer/part_store/world_composer/gpu_culler),
     provider subsystem (local_provider/resolvers), facade (matter_engine.cpp)
   - Build: `make -C MatterEngine3` → `libmatter_engine3.a` + embedded shader header
   - Tests: `make -C MatterEngine3/tests run-*` (headless) and GPU suites with `GALLIUM_DRIVER=d3d12`

6. **MatterEditor** - Interactive editor application linking the kernel library
   - Dependencies: MatterEngine3 (libmatter_engine3.a), MatterSurfaceLib, raylib, Dear ImGui,
     QuickJS-ng, Box3d, optionally autoremesher_core + TBB
   - Build: `make -C MatterEditor` → `editor` binary (runs from MatterEditor/ working directory)
   - Shader symlinks: `MatterEditor/shaders` → libs/MatterSurfaceLib/shaders,
     `MatterEditor/shaders_gpu` → MatterEngine3/shaders_gpu

7. **libs/MeshChartingLib** - UV chart segmentation + atlas packing (GL-free)
   - No consumers today; kept for the voxel-box-imposter work

`Prototypes/` holds retired experiments (`BasicWindowApp`, `GPURayTraceExample`).
They are excluded from `build-all.sh` and their sources are frozen snapshots —
`GPURayTraceExample` in particular still contains its own diverged copies of
`blas_manager`, `tlas_manager`, `bvh_visualizer` and `precomp.h`. Do not treat
those as a reference for current engine code.

Future projects should build on these components via `-I` + compiling from the
source-of-truth directory (see "Code Sharing Between Projects" above), not by
copying or symlinking.