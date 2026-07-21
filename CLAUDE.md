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

- `Libraries/` - Vendored third-party dependencies (raylib, imgui, box3d, quickjs-ng, autoremesher_core, Vulkan-Headers)
- `Examples/` - Reference material (e.g., `bvh_article`)
- `build-all.sh` - Top-level script that builds every project for the current platform; `./build-all.sh test` also runs headless test suites
- `create_project.sh` - Bootstrap a new sub-project skeleton
- Individual sub-project directories (e.g., `BasicWindowApp`, `SurfaceLib`, `MatterSurfaceLib`)
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

1. Library projects (like `SurfaceLib`, `MemoryLib`, `SpatialQueryLib`) organize reusable code in `include/` and `src/` directories
2. Consumer projects reference siblings via `-I../OtherProject/include` in their Makefile's CFLAGS (the common approach today; see `ParticleDynamicsExample/Makefile` for an example pulling in `SpatialQueryLib`)
3. As an alternative, consumer projects can create symlinks to specific files when finer-grained reuse is needed

Symlink example:
```bash
# From a new project that wants to use SurfaceLib
ln -s ../SurfaceLib/include/surface.h include/surface.h
ln -s ../SurfaceLib/src/surface.c src/surface.c
```

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

# Build viewer (Windows target)
make -C MatterViewer windows \
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
- `MatterEngine3/shaders` → `MatterSurfaceLib/shaders`
- `MatterViewer/shaders` → `MatterSurfaceLib/shaders`
- `MatterViewer/shaders_gpu` → `MatterEngine3/shaders_gpu`

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

Current projects and their relationships:

1. **BasicWindowApp** - Simple raylib application showing a rotating cube
   - Dependencies: raylib

2. **SurfaceLib** - Isosurface geometry library with visualization
   - Dependencies: raylib
   - Provides: Isosurface generation algorithms

3. **MatterEngine3** - Kernel library (`libmatter_engine3.a`) for the procedural engine
   - Provides: script host (QuickJS-ng DSL), bake pipeline (world_flatten/lod_bake/sector_grid),
     render subsystem (renderer/raster_composer/part_store/world_composer/gpu_culler),
     provider subsystem (local_provider/resolvers), facade (matter_engine.cpp)
   - Build: `make -C MatterEngine3` → `libmatter_engine3.a` + embedded shader header
   - Tests: `make -C MatterEngine3/tests run-*` (headless) and GPU suites with `GALLIUM_DRIVER=d3d12`

4. **MatterViewer** - Interactive viewer application linking the kernel library
   - Dependencies: MatterEngine3 (libmatter_engine3.a), MatterSurfaceLib, raylib, Dear ImGui,
     QuickJS-ng, Box3d, optionally autoremesher_core + TBB
   - Build: `make -C MatterViewer` → `viewer` binary (runs from MatterViewer/ working directory)
   - Shader symlinks: `MatterViewer/shaders` → MatterSurfaceLib/shaders,
     `MatterViewer/shaders_gpu` → MatterEngine3/shaders_gpu

Future projects can build on these existing components by creating the appropriate symlinks.