# MatterSurfaceLib

The engine's meshing / surfacing backend and its GPU acceleration-structure
managers. It is a **library**, not an application: consumers add
`-I../MatterSurfaceLib/include` and compile the `.c`/`.cpp` they need straight
out of `src/` (CLAUDE.md, "Code Sharing Between Projects"). Nothing here is
copied or symlinked into a consumer.

> This README used to be a verbatim copy of `Prototypes/GPURayTraceExample`'s,
> describing a standalone raylib/GL ray-tracing app with `build.bat`,
> `run.ps1`, `platform-status.sh` and an "ObjectAllocator (copied from
> ObjectAllocatorLib)" dependency. That app (`main.cpp` +
> `bvh_visualizer.{cpp,hpp}`) was deleted outright in Phase 5a along with the
> whole GL renderer path, and nothing was ever copied from MemoryLib. The
> `build.sh` / `run.sh` / `run*.ps1` / `platform-status.sh` scripts still
> sitting in this directory are leftovers from that app and drive nothing.

## Dependencies

SpatialQueryLib (`precomp.h`, `tri.h`, the BVH), MemoryLib (`mem_pool`),
MathLib, and raylib **headers** (POD `Mesh`/`Texture2D`/`Shader` types). It is
below MatterEngine3 in the one-way chain
MatterEditor → MatterEngine3 → MatterSurfaceLib → SpatialQueryLib → MemoryLib.

## What it provides

Surfacing and meshing

- `surface.c` / `surface.h` — marching-cubes + CSG isosurface extraction
- `marching_cubes_algorithm`, `oriented_cube_algorithm`, `meshing_algorithm` —
  the pluggable mesher backends (`mc_tables.h` is a single-TU table header;
  see the note in it before including it a second time)
- `cluster` / `cell` / `cell_visitor` / `lattice` / `occupancy` — the spatial
  cell/cluster meshing layer and its dirty-cell rebuild
- `mesh_simplifier`, `mesh_indexed`, `mesh_transform`, `mesh_smooth`,
  `mesh_build_utils`, `mesh_worker_pool` — post-mesh processing
- `mesh_retopo` — autoremesher-backed retopology, compiled only under
  `MATTER_HAVE_AUTOREMESHER`
- `particle_culling`, `vertex_ao`, `fat_primitive`, `voxel_imposter`
- `part_asset`, `material_registry`

GPU acceleration structures

- `blas_manager`, `tlas_manager` — these own `Texture2D`/`Shader` and are the
  GL upload path, distinct from the pure structures in SpatialQueryLib
  (`bvh.cpp`, `bvh_analyzer.cpp`), which is where the BVH build itself lives

## Shaders (the reason this Makefile still exists)

`make` here does **not** build a binary. The default goal is `all: shaders`,
which runs `src/shader_preprocessor.cpp` to expand the `#include`s in
`shaders/raytrace_tlas_blas.fs` into the **committed**
`shaders/raytrace_tlas_blas_processed.fs`. MatterEngine3's embedded-shaders
step reads that file directly out of this directory
(`MSL_SHADER_DIR` in `MatterEngine3/Makefile`).

Because the processed shader is committed, `clean` deliberately does not
delete it — removing it breaks every downstream build until a toolchain that
can rebuild the preprocessor is available. Regenerate deliberately with:

```bash
make -C libs/MatterSurfaceLib regen-shaders   # review the diff before committing
make -C libs/MatterSurfaceLib platform        # print the platform-detection state
```

Everything else in the Makefile (LDFLAGS/LDLIBS, the raylib build plumbing,
the `TARGET=windows-native` / `NO_MINGW` / `WSL_LINUX` switches) is inert
leftover from the deleted app; only `$(PREPROCESSOR)`'s `CXXFLAGS`/`BUILD_DIR`
are still live.

## Tests

`tests/Makefile` has one target per suite; run them individually:

```bash
make -C libs/MatterSurfaceLib/tests run-simp     # mesh_simplifier
make -C libs/MatterSurfaceLib/tests run-blas     # blas_manager refcounting
make -C libs/MatterSurfaceLib/tests run-cell     # cell bounds
make -C libs/MatterSurfaceLib/tests run-cont     # mesh continuity
make -C libs/MatterSurfaceLib/tests run-reg      # material_registry
make -C libs/MatterSurfaceLib/tests run-tint     # blas tinting
make -C libs/MatterSurfaceLib/tests run-cull     # particle_culling
make -C libs/MatterSurfaceLib/tests run-ao       # vertex_ao
make -C libs/MatterSurfaceLib/tests run-par      # parallel meshing
make -C libs/MatterSurfaceLib/tests run-cube     # oriented_cube_algorithm
make -C libs/MatterSurfaceLib/tests run-part     # part_asset round-trip
make -C libs/MatterSurfaceLib/tests run-vox      # voxel_imposter
make -C libs/MatterSurfaceLib/tests run-midx     # mesh_indexed
make -C libs/MatterSurfaceLib/tests run-mtx      # mesh_transform
make -C libs/MatterSurfaceLib/tests run-retopo   # mesh_retopo (needs autoremesher)
make -C libs/MatterSurfaceLib/tests run-smooth   # mesh_smooth
make -C libs/MatterSurfaceLib/tests run          # minimal_cell_test
```

`tests/cell_tests.cpp` and `tests/simple_cell_tests.cpp` are not wired to any
target; `tests/simp_perf_probe.cpp` is a hand-run benchmark, not a suite.

`build-all.sh` builds this project with `WSL_LINUX=1`.
