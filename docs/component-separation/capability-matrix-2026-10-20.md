# Optional module capability matrix, 2026-10-20

This is the report for task `clear-crest-48.9.4`. It covers the source tree at
`528d32f1` (which integrates `clear-crest-48.9.3`) plus this task's change.

The change is a move, following the `.9.3` pattern. The Box3d settle core
`tileset_settle.{h,cpp}` moves with `git mv`, byte-identical, from
`MatterEngine3/src` into `libs/AssetPipeline/physics/src`. The old header
location becomes a 4-line forwarder. The engine archive, the engine-core CMake
manifest and FILTER, the engine and test Makefiles, and `matter_settle_native`
all compile the source from where it now lives.

`libs/AssetPipeline/CMakeLists.txt` registers the animation and physics modules
behind `MATTER_ASSET_ANIMATION` and `MATTER_ASSET_PHYSICS`. Both default to
OFF, and neither module is linked into `matter_asset_pipeline`.
`libs/AssetPipeline/tests/independence.py` now fails any basic configuration
that contains an animation, physics or compute target.

The animation and compute modules stay where they already were. Each was already
the single authority for its code: animation's native adapter compiles
`MatterEngine3/src/animation/*` in place, and `GeometryCompute` is already
registered centrally through `cmake/MatterGeometrySupplement.cmake`. No
algorithm was rewritten, and no copy of any source was kept.

Results come from two toolchains on one machine:

- the WSL2 Linux side (1 CPU, GCC 13.3, CMake and Ninja), for the standalone
  module builds and the Make suites;
- the canonical Windows MSVC build. It ran through
  `tools/build-windows-from-wsl.sh` and WSL interop (VS 2022 17.14.7, Vulkan
  SDK, RTX 4090, driver 610.88).

Where a check could not run, the report says so rather than counting it as a
pass.

## Module matrix

| Module | Option (default) | Capabilities | Basic config | Evidence |
| --- | --- | --- | --- | --- |
| Animation products | `MATTER_ASSET_ANIMATION` (OFF) | `matter.asset.animation` | excluded | `ap-opt` ctest: `matter_animation_asset` and 4 × `matter_extracted_animation_*`, all passed |
| Animation native/ozz adapter | `MATTER_ANIMATION_NATIVE` (OFF) | `matter.asset.animation` (authored/ozz) | excluded | `ap-opt` ctest `matter_animation_native` passed; `make run-animation-rigid` ALL PASS |
| Collision products | `MATTER_ASSET_PHYSICS` (OFF) | `matter.collision.product`, `matter.asset.collision` | excluded | physics ctest `physics.products` passed (Box3d OFF build: 3/3) |
| Box3d settling | `MATTER_PHYSICS_BOX3D` (OFF) | `matter.physics.settle.box3d` | excluded | physics ctest 4/4 with Box3d ON; `make run-tilesetphysics` PASSED, `run-tilesetbake` passed, `run-tilesetplacement` PASSED, `run-baketrace` ALL PASS |
| Terrain collision adapter | `MATTER_PHYSICS_TERRAIN_ADAPTER` (ON within physics) | terrain collision candidates | excluded (physics is OFF) | ctest `physics.terrain` passed |
| Solid compute | `MATTER_GEOMETRY_COMPUTE` (OFF) | `matter.geometry.solid.compute` | excluded | GeometryCompute ctest 1/1 with the option OFF (Linux). MSVC ctest `matter_geometry_compute_off` and `matter_geometry_compute_on` (Vulkan backend, GPU) both passed |
| LOD | `MATTER_ASSET_LOD` (ON) | `matter.process.lod.simplify`, `matter.asset.lod`, `matter.asset.lod.chain` | part of the asset pipeline | `matter_asset_lod` and `matter_asset_lod_failures` passed |
| Surface | `MATTER_ASSET_SURFACE` / `_NATIVE` (ON) | `matter.processing.surface.bake`, `matter.processing.surface.local-tape`, `matter.material.surface.samples` | part of the asset pipeline | `matter_asset_surface` and `matter_asset_surface_legacy` passed |
| glTF | `MATTER_ASSET_GLTF` (ON) | `matter.import.gltf.static`, `matter.export.gltf.static` | part of the asset pipeline | `matter_asset_gltf`, `matter_asset_external` and `matter_asset_malformed` passed |

How an undeclared capability is handled:

- The contract validator (`libs/MatterContracts/src/validation.cpp:819-824`)
  requires a product to declare its own capability.
- With an optional module OFF, its producer returns an explicit unsupported
  result instead of a stub. The Box3d-OFF physics tests and the compute-OFF test
  cover this.
- The browser viewer shows a diagnosed fallback for `matter.asset.animation`
  and `matter.asset.collision`, as in
  [qualification-2026-10-20.md](qualification-2026-10-20.md). It does not claim
  the advanced native features.

## Builds and tests run

`S` stands for a scratch directory outside the repository.

```sh
# AssetPipeline, basic (default options): 13/13 passed; build.ninja has no
# settle/collision/animation/box3d/ozz reference.
cmake -S libs/AssetPipeline -B $S/ap-basic -G Ninja && cmake --build $S/ap-basic -j1 && ctest --test-dir $S/ap-basic
# AssetPipeline with every optional module: 183/183 built, 23/23 passed.
cmake -S libs/AssetPipeline -B $S/ap-opt -G Ninja -DMATTER_ASSET_ANIMATION=ON -DMATTER_ASSET_PHYSICS=ON \
      -DMATTER_PHYSICS_BOX3D=ON -DMATTER_ANIMATION_NATIVE=ON
cmake --build $S/ap-opt -j1 && ctest --test-dir $S/ap-opt --output-on-failure
# Physics standalone: Box3d ON 4/4, default OFF 3/3.
cmake -S libs/AssetPipeline/physics -B $S/phys -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMATTER_PHYSICS_BOX3D=ON
cmake -S libs/AssetPipeline/physics -B $S/physoff -G Ninja
# GeometryCompute, default OFF: 1/1.
cmake -S libs/GeometryCompute -B $S/gc-off -G Ninja -DCMAKE_BUILD_TYPE=Release
# Configuration independence (geometry, graphics, archive-only, lod-only, surface-*).
python3 libs/AssetPipeline/tests/independence.py --build-root $S/indep
# Engine archive and engine Make suites. glslc is not installed on this host.
# The embedded SPIR-V header was current but older than the shader sources'
# checkout mtimes, so make is told not to remake it.
make -C MatterEngine3 -o shaders_gen/embedded_spirv.h
make -C MatterEngine3/tests -o ../shaders_gen/embedded_spirv.h <target>
# Dependency and manifest rules.
python3 tools/component-separation/check_dependencies.py --checks static,duplicates
python3 tools/check-source-manifests.py --root .
python3 -m pytest -q tests/component-separation/test_dependency_checks.py
```

| Check | Result |
| --- | --- |
| `make -C MatterEngine3` | rc 0; `tileset_settle.o` is compiled from `libs/AssetPipeline/physics/src` |
| `run-tilesetphysics`, `run-tilesetbake`, `run-tilesetplacement`, `run-baketrace`, `run-animation-rigid` | pass |
| `run-physics`, `run-terrain-collision-physics` | **fail to build, pre-existing.** On Linux, vendored `flecs.c` stops at `CLOCK_MONOTONIC` / `NI_NUMERICHOST` undeclared. The failure is in the `physicsc` object rule, which this change does not touch |
| `run-viewer-logic`, `run-chart-atlas` | **fail to link, pre-existing.** The link lines lack `asset_store::*` and `geometry::RootCache` symbols. No `tileset_settle` symbol is involved |
| `check_dependencies.py` | 21 passed, 6 failed, 6 untested, the same as base. There are 33 violations against 32 at base; the extra one is listed below |
| `check-source-manifests.py` | the same 6 pre-existing errors as base |
| pytest `test_dependency_checks.py` | 58 passed |

The extra dependency-check violation is
`[ownership.scaffold-has-sources] libs/AssetPipeline/physics/src/tileset_settle.cpp`.
It comes from the same rule that already reports every other AssetPipeline source:
`rules.json` still marks `asset_pipeline` as an interface-only scaffold.
Marking that component implemented, so that it is audited, is separate work.

## Canonical MSVC build and CTest

The wrapper needs the checkout on a Windows mount. Run from the Linux worktree,
it failed before configuring (rc 1):

```
UNC paths are not supported.  Defaulting to Windows directory.
CMake Error: Could not read presets from C:/Windows/Microsoft.PowerShell.Core/FileSystem::/wsl.localhost/Ubuntu/home/jkern/dev/matter-engine-cpp/.aq/worktrees/slot-0:
File not found: .../CMakePresets.json
```

Every tracked and untracked file of the worktree was therefore copied to
`C:\tmp\clear-crest-48-9-4-8rwJto`, and `cmp` confirmed the copy was
identical. The checks then ran there:

```sh
./tools/build-windows-from-wsl.sh RelWithDebInfo <target>     # each target below
ctest.exe --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo --output-on-failure -j 1 -R '<regex>'
# RENDER-NORMAL's heavy mode is registered only with
# -DMATTER_ENABLE_HEAVY_VULKAN_SMOKE_TESTS=ON, so it ran directly, from the build directory:
WSLENV=MATTER_VK_SMOKE_MODE MATTER_VK_SMOKE_MODE=vt-normal-frame ./vulkan_smoke_tests.exe
```

| Check | Result |
| --- | --- |
| Wrapper builds: `matter_editor`, `obj_export_golden_tests`, `script_host_tests`, `dsl_determinism_tests`, `part_graph_tests`, `part_flatten_tests`, `solid_source_evaluation_tests`, `async_bake_tests`, `terrain_mesher_tests`, `terrain_field_tests`, `seam_weld_tests`, `contour_seam_tests`, `sector_streaming_coordinator_tests`, `bake_trace_tests`, `tileset_bake_tests`, `tileset_physics_tests`, `physics_tests`, `terrain_collision_physics_tests`, `terrain_collision_artifact_tests`, `terrain_collision_session_tests`, `animation_rigid_bridge_tests`, `vulkan_smoke_tests`, `matter_geometry_compute_off`, `matter_geometry_compute_on` | all rc 0. The build logs compile `libs\AssetPipeline\physics\src\tileset_settle.cpp` into both `matter_engine_core` and `matter_engine_viewer_objects` |
| CTest over those 21 test targets | 20 passed, 1 failed (rc 8) |
| `terrain_collision_session_tests` | **fails, pre-existing.** The failing check is `session canonicalizes the exact field, overlay, bake, mesher, sector, rung, and cache identity`. The same staging copy, reset to base `528d32f1` (the old `MatterEngine3/src/tileset_settle.cpp` compiled), fails the same check with the same single failure. This is the `.9.3` terrain/collision identity gap, outside this task |
| CTest `matter_geometry_compute_off`, `matter_geometry_compute_on` | 2/2 passed (ON: 2.23 s on the GPU) |
| `vulkan_smoke_tests` default mode (CTest) | passed |
| `vt-normal-frame` (RENDER-NORMAL) | rc 0, `validation errors: 0`, `ALL PASS`. At 0° yaw: rasterCos 0.999811, rayCos 0.999998, parity 0.999814, after 5 stable frames |
| `animation-skin` (GPU skinning compute readback, run from `MatterEditor/`) | rc 0, `validation errors: 0`, `ALL PASS`. These are the pass criteria `smoke_vulkan_faults.ps1` applies to this mode |
| One-shot editor capture of StreamMountain (`MATTER_SCREENSHOT`, settle 90) | **no capture.** The run timed out at 600 s, still cold-baking in a copy with no cache. The process was stopped. RENDER-MOUNTAIN lists this recipe as historical, not as a task check |

## C0 inventory coverage

These rows are from [inventory.md](inventory.md#native-compatibility-and-target-matrix)
and the fixtures are from [fixtures.json](fixtures.json).

The Make analogs ran on Linux with
`make -C MatterEngine3/tests -o ../shaders_gen/embedded_spirv.h <target>`. Each
of these returned rc 0 and printed ALL PASS: `run-obj-export-golden`,
`run-script`, `run-flatten`, `run-asyncbake`, `run-contourseam`,
`run-terrainfield`, `run-terrainmesh`, `run-seamweld`, `run-sectorbake`.

The Node commands ran from the repository root on Node 24.15.0, and each
returned rc 0:

- `node tests/vt_normal_frame_reference_tests.mjs`: 480 transformed directional
  probes passed.
- `node projects/world_demo/tests/mountain_geometry_site_tests.mjs`: 3
  placements / 3 dense assets by default, a stress profile of 1283 / 19, and
  stable nested ownership.
- `node projects/world_demo/tests/mountain_terrain_only_tests.mjs`: forest only
  without `terrainOnly`, and the geometry-rock site in both modes.

| Inventory row | C0 fixtures | Result |
| --- | --- | --- |
| Primitives, CSG, transforms, seeds | `PROP-*`, `GEOM-*` | MSVC: `obj_export_golden`, `solid_source_evaluation`, `part_graph`, `part_flatten`, `dsl_determinism`, `script_host` pass. Make analogs pass |
| Shared assembly, castle, cancellation | `BUILD-*`, `EVAL-BOUNDED` | MSVC: `async_bake`, `sector_streaming_coordinator`, `bake_trace` pass. Make analogs pass |
| Simplify/smooth, LOD, charts, export | — | AssetPipeline LOD/surface/glTF ctests pass. The Linux `run-chart-atlas` link is broken (pre-existing) |
| Terrain fields, chunks, seams | `TERRAIN-*` | owned by `.9.3` and unchanged here. MSVC: `terrain_field`, `terrain_mesher`, `seam_weld`, `contour_seam` pass |
| Raster, RT/GI, VT, water, foliage (advanced renderer inputs) | `RENDER-NORMAL`, `RENDER-MOUNTAIN` | RENDER-NORMAL: the `vt-normal-frame` GPU mode and its Node probe pass. RENDER-MOUNTAIN: both Node commands pass. It has no native target, and the cold editor capture timed out (above) |
| Animation/rig/socket/skin/clip | — | MSVC `animation_rigid_bridge` passes. AssetPipeline animation ctests and `run-animation-rigid` pass. The `animation-skin` GPU smoke mode passes |
| Collision, physics placement, hydrology, water | — | MSVC: `tileset_physics`, `tileset_bake`, `physics`, `terrain_collision_physics`, `terrain_collision_artifact` pass. `terrain_collision_session` fails, the same as at base. The Linux flecs-based Make suites do not build (pre-existing) |
| GPU solid compute | — | MSVC `matter_geometry_compute_off` and `_on` pass |

## Open items

- **`terrain_collision_session_tests` fails one check at base `528d32f1` and
  with this change alike.** It belongs to the `.9.3` terrain/collision identity
  work and was not repaired here.
- **Not run:**
  - the rest of the 12-mode `smoke_vulkan_faults.ps1` gate (`animation-skin`
    and `vt-normal-frame` ran individually);
  - the other `-L vulkan` ctests;
  - a warm-cache StreamMountain capture.

  They test renderer and animation code that this change does not edit. The
  engine objects they link, including the moved settle core, are the same
  objects built and tested above.
- **There is no root-level CMake option for the optional modules.** The root
  `cmake/MatterEngine.cmake` graph keeps compiling the engine's in-place
  sources, which are now the module sources themselves, so the editor keeps
  every native feature. A root `MATTER_ASSET_*` switch that drops them from the
  editor would be a product change, not part of this move.
