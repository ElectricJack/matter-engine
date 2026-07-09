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

- `Libraries/` - Vendored third-party dependencies (raylib, imgui, ode)
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