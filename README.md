# matter-engine-cpp

A prototype C/C++ engine for **procedural generation and ray-traced rendering of voxel/particle "matter"** — the long-term target is real-time rendering of billions of meshed static particles with LOD, plus a dynamic particle-physics layer for material interactions (thermal, electrical, chemical, bonding).

This repository is a **monorepo of independently-buildable sub-projects**, each one a focused experiment that builds on the last. Together they form the technology stack the eventual engine sits on.

---

## Sub-projects

Ordered roughly from foundational → integration.

### `MemoryLib/` — memory managers: pool, arena, growable array (C)

Test-driven C allocator that grows in pages of fixed-size objects. No graphics. **6/6 tests pass.**

### `SpatialQueryLib/` — geometry types + spatial acceleration (C/C++)

Source of truth for everything below the meshing layer: `precomp.h` (float3/float4/SIMD), `tri.h` (`Tri`/`TriEx`/`mat4` — the engine's universal triangle interchange types), the BVH/TLAS structures and analyzer, and a generic spatial hash for radius/box queries. GL-free. Compiled directly into `libmatter_engine3.a`. **14/14 tests pass.**

*The name predates the contents — it now owns the core geometry types, not just queries.*

### `ParticleFlowLib/` — particle flow simulation (C++)

Deterministic particle simulation with force fields and append-only path recording, used by the tree/foliage generators. Compiled into the engine and viewer.

### `MatterSurfaceLib/` — the convergence project

![MatterSurfaceLib screenshot](docs/screenshots/matter_surface_lib.png)

**Pulls everything together.** Implements the `Cluster` / `Cell` architecture from the roadmap: a cluster owns particles in its local space, sub-divides into power-of-two integer cells, generates per-cell marching-cubes meshes (~0.5 ms each), registers each mesh as a BLAS, and ray-traces the resulting TLAS in a fragment shader. The screenshot shows the BVH-visualization debug mode with the analyzer panel listing every BLAS in the scene.

See [`ROADMAP.md`](./ROADMAP.md) for the design intent behind each project and what's still ahead (ODE-backed `ParticleDynamicsLib`, streaming data layer, the asteroid-mining game prototype).

## Architecture

- **One repo, many independent projects.** Each sub-project has its own `Makefile` and produces its own binary. There's no umbrella build target — `build-all.sh` just walks the list.
- **Code sharing** between sub-projects is via `-I../OtherProject/include` in Makefiles (and occasionally filesystem symlinks). No package manager, no submodules.
- **Vendored third-party deps** live under `Libraries/` (raylib, ImGui, ODE). Building from a fresh clone needs no system packages other than a C/C++ toolchain and OpenGL/X11 dev headers.
- **Cross-platform** Makefiles target Linux, macOS, and Windows (native + MinGW cross-compile from WSL). `build-all.sh` defaults to the host platform.

## Building & running

```bash
# Build every project for the host platform
./build-all.sh

# Clean and rebuild from scratch
./build-all.sh clean

# Build, then run the headless test suites (MemoryLib + SpatialQueryLib)
./build-all.sh test
```

Per-project builds:

```bash
cd MatterSurfaceLib
make WSL_LINUX=1    # or just `make` on native Linux/macOS
./matter_surface_lib
```

### Prerequisites (Linux/WSL)

- `gcc`, `g++`, `make`, `pkg-config`
- OpenGL + X11 dev headers: `libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`
- For graphical apps: a working display (WSLg, XQuartz, or a desktop session)

### Per-project quick reference

| Project | Build command | Binary |
|---|---|---|
| `MemoryLib` | `make` | `./memorylib` (test runner) |
| `SpatialQueryLib` | `make` | `./spatialquerylib` (test runner) |
| `ParticleFlowLib` | `make` | `libparticleflow.a` |
| `MatterEngine3` | `make` | `libmatter_engine3.a` |
| `MatterSurfaceLib` | `make WSL_LINUX=1` | `./matter_surface_lib` |
| `MatterViewer` | `make` | `./viewer` |

Retired experiments live under `Prototypes/` and are excluded from `build-all.sh`.

## Status

Working as of the latest commit, verified by `./build-all.sh test` on Linux/WSL with an RTX 4090 via WSLg:

- All projects build cleanly
- Headless tests pass: 6 in `MemoryLib`, 14 in `SpatialQueryLib`, plus the
  `MatterEngine3` suites (`run-script`, `run-evalworld`, `run-world-definition`, `run-iso`)
- All raylib apps initialize a window and reach the render loop
- `MatterSurfaceLib` runs the full pipeline: 1 cluster → 80 cells → marching-cubes mesh generation → BLAS registration → TLAS-based GPU ray tracing

See [`ROADMAP.md`](./ROADMAP.md) for what's done and what's planned.

## Repository history

This repo was consolidated from seven previously-independent local-only sub-repos into a single monorepo. Per-project history (86 commits across the original repos) was preserved via `git subtree` import — those commits remain reachable via `git log --all`. The first project-level commit you'll see on `main` is `7a94621` (initial monorepo); everything older lives in the imported histories.

## License

Not yet specified. The vendored libraries under `Libraries/` retain their original upstream licenses (raylib: zlib, ImGui: MIT, ODE: BSD/LGPL dual).
