# matter-engine-cpp

A prototype C/C++ engine for **procedural generation and ray-traced rendering of voxel/particle "matter"** — the long-term target is real-time rendering of billions of meshed static particles with LOD, plus a dynamic particle-physics layer for material interactions (thermal, electrical, chemical, bonding).

This repository is a **monorepo of independently-buildable sub-projects**, each one a focused experiment that builds on the last. Together they form the technology stack the eventual engine sits on.

---

## Sub-projects

Ordered roughly from foundational → integration → application.

### `libs/MemoryLib/` — memory managers: pool, arena, growable array (C)

Test-driven C allocator that grows in pages of fixed-size objects. No graphics, no dependencies.

### `libs/SpatialQueryLib/` — geometry types + spatial acceleration (C/C++)

Source of truth for everything below the meshing layer: `precomp.h` (float3/float4/SIMD), `tri.h` (`Tri`/`TriEx`/`mat4` — the engine's universal triangle interchange types), the BVH/TLAS structures and analyzer, and a generic spatial hash for radius/box queries. GL-free.

*The name predates the contents — it now owns the core geometry types, not just queries.*

### `libs/MathLib/` — canonical Vec2/Vec3/Vec4/Mat4/Quat math (C++)

One documented math type, replacing the raylib math types the rest of the engine used to depend on.

### `libs/ParticleFlowLib/` — particle flow simulation (C++)

Deterministic particle simulation with force fields and append-only path recording, used by the tree/foliage generators.

### `libs/MatterSurfaceLib/` — the convergence project

![MatterSurfaceLib screenshot](docs/screenshots/matter_surface_lib.png)

**Pulls everything together.** Implements the `Cluster` / `Cell` architecture from the roadmap: a cluster owns particles in its local space, sub-divides into power-of-two integer cells, generates per-cell marching-cubes meshes (~0.5 ms each), registers each mesh as a BLAS, and feeds the resulting TLAS to the ray tracer. The screenshot is from the retired GL-era standalone viewer; the same meshing/BLAS pipeline now feeds MatterEngine3's Vulkan renderer.

### `libs/MeshChartingLib/` — UV chart segmentation + atlas packing (GL-free)

No consumers today; kept for the voxel-box-imposter work.

### `libs/AssetStoreLib/` — MatterStore: content-addressed blob storage (C)

Packs + an atomically-swapped index + per-blob CRC, an LRU ref table, and coalesced batch reads. No consumers yet — adopting it as the engine's cache is future work.

### `libs/ProfileLib/` — always-on lightweight profiler (C++)

Frame-record capture, Chrome-trace export, and the in-editor Performance/Memory panels.

### `MatterEngine3/` — the kernel library

Script host (QuickJS-ng DSL), the bake pipeline (world flatten/LOD/sector grid), and the Vulkan render subsystem (compute-culled, GPU-driven). Builds to `libmatter_engine3.a`; no application `main` of its own.

### `MatterEditor/` — the interactive editor

Links the kernel library plus Dear ImGui, QuickJS-ng, Box3d, and (optionally) an autoremesher-based retopology backend. **Vulkan-only** — the original raylib/OpenGL rendering and windowing path was deleted outright; the Windows build asserts no OpenGL import survives in the linked binary.

See [`ROADMAP.md`](./ROADMAP.md) for the design intent behind each project and what's still ahead.

## Architecture

- **One repo, many independently testable projects.** The shipping Windows
  editor and its complete dependency graph use the root CMake target graph;
  the per-project Makefiles remain useful on Unix and as a temporary Windows
  rollback path.
- **Code sharing** between sub-projects is via `-I../OtherProject/include` in Makefiles, compiling the sibling's sources directly from where they live — never copied, never symlinked (symlinks were tried and abandoned; see `CLAUDE.md`).
- **Vendored third-party deps** live under `third_party/`: raylib, Dear ImGui, box3d (physics), quickjs-ng (the JS DSL host), autoremesher_core (retopology), ozz-animation, flecs, and Vulkan-Headers.
- **Windows x64 with Visual Studio 2022 MSVC is the verified editor platform.**
  Native PowerShell and Ubuntu/WSL agents both drive the same Windows
  CMake/Ninja/MSVC graph through repository wrappers. The lower-level `libs/`
  projects remain plain portable C/C++ and build independently on
  Linux/macOS/Windows.

## Building & running

```bash
# Build every project for the host platform
./build-all.sh

# Clean and rebuild from scratch
./build-all.sh clean

# Build, then run the headless test suites
./build-all.sh test
```

`./build-all.sh test` runs far more than the two oldest libraries: MemoryLib,
SpatialQueryLib, ParticleFlowLib, MatterSurfaceLib (a dozen-plus suites),
MeshChartingLib, MathLib, AssetStoreLib, and MatterEngine3's `run-*` targets
(script host, bake pipeline, tileset pipeline, event system, and more), plus
GPU suites when a capable driver is detected.

Canonical Windows builds (see `CLAUDE.md` for dependency and worktree details):

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_dist
tools/check-windows-msvc-package.ps1 -DistPath MatterEditor/build/dist/world_demo
```

From WSL, use
`./tools/build-windows-from-wsl.sh RelWithDebInfo matter_dist`; it invokes the
same native Windows compiler rather than cross-compiling with Linux GCC. The
old `make -C MatterEditor windows` MSYS2/UCRT64 path is rollback-only pending a
separate removal commit.

### Prerequisites (Linux/WSL)

- `gcc`, `g++`, `make`, `pkg-config`
- The `libs/` projects (MemoryLib, SpatialQueryLib, ParticleFlowLib, MatterSurfaceLib, MeshChartingLib, MathLib, AssetStoreLib, ProfileLib) are plain C/C++ and need only a toolchain
- MatterEngine3/MatterEditor additionally need a Vulkan SDK/loader; the Windows
  build is the one actually exercised regularly (see `CLAUDE.md`)

Retired experiments live under `Prototypes/` (`BasicWindowApp`,
`GPURayTraceExample`) and are excluded from `build-all.sh` — frozen snapshots,
not a reference for current engine code.

## Status

The canonical MSVC CTest graph exercises the required Windows CPU suites plus
Vulkan compatibility, smoke, compositor, shader-rebuild, and editor contract
gates. `./build-all.sh test` remains the portable/legacy aggregate for the
independent Make projects. The editor is Vulkan-only — there is no OpenGL
renderer or raylib window anywhere in its production build.

See [`ROADMAP.md`](./ROADMAP.md) for what's done and what's planned.

## Repository history

This repo was consolidated from seven previously-independent local-only sub-repos into a single monorepo. Per-project history (86 commits across the original repos) was preserved via `git subtree` import — those commits remain reachable via `git log --all`. The first project-level commit you'll see on `main` is `7a94621` (initial monorepo); everything older lives in the imported histories.

## License

Not yet specified. The vendored libraries under `third_party/` retain their original upstream licenses (raylib: zlib, Dear ImGui: MIT, box3d: MIT).
