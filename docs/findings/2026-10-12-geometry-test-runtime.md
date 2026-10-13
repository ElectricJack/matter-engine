# Geometry/surface test runtime — clear-stone-99

Measured on 2026-10-12 from base `1f4ff089a9a81f8f54528df6a131a70757648853`.
This is the portable geometry/surface slice, not the aggregate project suite.
The independent component work is qualified below. Delivery of the shared
engine graph and the serialized full native build remains pending.

## Changes and coverage

- The default large-index QEM fixture is an 8x8 connected quad grid: 128 real
  triangles, 65,792 input vertices, and every referenced index above 65,535.
  Unreferenced padding lies outside the grid so index truncation cannot find an
  equivalent surface. The existing nonempty/significant-reduction guard remains,
  with output index, finite-position and grid-bound checks added. All other
  simplifier fixtures, including the six soup-sphere collapse variants, remain.
- **Default coverage loss:** the old 320x260 grid's 166,400 triangles and 83,781
  distinct referenced positions no longer exercise dense QEM scalability in
  regular CTest. Run `mesh_simplifier_tests --dense-qem` for that original
  workload. The default still exercises the real 32-bit indexed simplifier and
  its historical input-size/index-cap regression. Reintroducing the old
  over-65,535 identity return via a temporary link wrapper made the new test
  abort at its reduction assertion, as intended.
- The cave field guard keeps all 1,600 rays and their original endpoints
  (`y=192` and the last old scan sample, `y=-127.75`). Crossing parity telescopes
  to endpoint signs. Replacing 1,278 interior evaluations per ray removes no
  asserted interior oracle; explicit air-above/rock-below checks also reject
  reversed endpoint signs. Mesh, roof and seam checks remain unchanged.
  **Sampling reduction:** this guard no longer executes the 2,044,800 interior
  density evaluations. Their input/operation-execution coverage is removed,
  while the original parity assertion is retained exactly.
- The seam test explicitly captures its tile split in the lookup lambda.
  MSVC rejected the previous implicit use. The fixture and assertions are
  unchanged.

No production implementation or CI files changed. Component CMake now moves
GeometryCompute dependency reads into mandatory build qualification. The
SurfaceLib Makefile now exposes the existing staged-normal regression suite
with a private transient object directory.

## Timings

All 25 portable tests passed in each run. Assertions were enabled.

| Run | Wall seconds |
| --- | ---: |
| Linux serial, original test snapshots | 101.94 |
| Linux serial, changed tests | 14.60 |
| Linux `ctest -j4`, changed tests, with syscall tracing | 5.73 |
| Native Windows MSVC serial, changed tests | 12.55 |
| Native Windows MSVC `ctest -j4`, changed tests | 5.49 |

Linux used GCC Release (`-O3`, C++17, `-UNDEBUG`); native Windows used MSVC
14.44.35207 Release (`/O2`, C++17, `/UNDEBUG`) and SDK 10.0.26100.0. These
are single runs on a shared host; unchanged tests show scheduling variation.
The before run substitutes only the original QEM test snapshot and the original
terrain executable. It links the same actual production implementations.
A separate matched `-O2` simplifier check measured 107.37s before and 0.06s after;
the standalone native simplifier run passed in 0.20s.

Slowest 20 before, with their corresponding after times (seconds):

| Test | Before | After |
| --- | ---: | ---: |
| `mesh_simplifier_tests` | 90.427 | 0.207 |
| `surface_proxy_tests` | 4.561 | 6.595 |
| `terrain_mesher_tests` | 3.784 | 3.472 |
| `sdf_candidate_bounds_tests` | 1.903 | 1.933 |
| `contour_engine_tests` | 0.419 | 0.326 |
| `sector_lod_tests` | 0.247 | 0.498 |
| `staged_normals_tests` | 0.072 | 0.478 |
| `lod_distance_tests` | 0.068 | 0.127 |
| `vertex_cache_order_tests` | 0.061 | 0.070 |
| `surface_field_tests` | 0.056 | 0.099 |
| `modifier_apply_tests` | 0.055 | 0.023 |
| `contour_seam_tests` | 0.043 | 0.067 |
| `geometry_cut_tests` | 0.036 | 0.042 |
| `contour_mesh_tests` | 0.032 | 0.049 |
| `seam_weld_tests` | 0.023 | 0.027 |
| `mesh_indexed_tests` | 0.021 | 0.024 |
| `polygon_triangulate_tests` | 0.017 | 0.039 |
| `mesh_transform_tests` | 0.017 | 0.031 |
| `terrain_field_tests` | 0.016 | 0.045 |
| `mesh_error_tests` | 0.010 | 0.039 |

The other five measured tests were `mesh_smooth_tests`,
`material_registry_tests`, `oriented_cube_tests`, `solid_face_projection_tests`
and `blas_refcount_tests`; none were removed or weakened.

## Verification method

Because the base root CMake graph accepts only MSVC/Windows, an external,
temporary CTest graph compiled the repository sources in place. No production
implementation was copied or replaced with a test implementation. It included
the 20 tests in the timing table and the five additional tests listed above. The source
closure used actual MatterSurfaceLib, SpatialQueryLib, MemoryLib, terrain/river
meshing, seam welding, modifiers, surface proxies, mesh-error verification,
polygon triangulation, geometry-cut traversal and sector/LOD resolution.
The real LOD adapter was compiled from `lod_bake.cpp`; Linux discarded unused
functions with section garbage collection. Native Windows linked the real
BLAS manager, bake trace and MeshChartingLib dependencies too.

The temporary Linux graph is `/tmp/clear-stone-99-slice`; commands were:

```sh
cmake -S /tmp/clear-stone-99-slice -B /tmp/clear-stone-99-slice/build \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DROOT="$PWD"
cmake --build /tmp/clear-stone-99-slice/build -j2
# SLICE_BASELINE=ON selects the two original test snapshots; OFF uses the changes.
ctest --test-dir /tmp/clear-stone-99-slice/build --output-on-failure \
  --output-junit /tmp/clear-stone-99-slice-after.xml
strace -f -e trace=execve,openat,clone,clone3,fork,vfork \
  -o /tmp/clear-stone-99-slice-after-parallel.trace \
  ctest --test-dir /tmp/clear-stone-99-slice/build -j4 --output-on-failure
```

Tracing the test executables and their descendant thread (26 process/thread
IDs) found opens only for `/etc/ld.so.cache` and the system C/C++/math runtime
libraries. No test fixture, source, asset, cache, evidence or build data was
opened. CTest's own configuration/log reads were excluded from the test-process
audit. The tests generate their geometry in memory.

Native validation used a separate temporary Ninja graph under
`C:/Users/webde/AppData/Local/Temp/clear-stone-99-native`, compiling the same
actual repository sources with the project's resolved MSVC toolchain. This
was a geometry slice build, not the full editor/package build.

## Component builds and runtime file access

GeometryCompute uses its actual standalone CMake project, with
`BUILD_TESTING=ON`, `CMAKE_BUILD_TYPE=Release`, and the optional backend
explicitly selected. The original disabled-backend graph passed in 0.76s:
0.01s runtime and 0.75s source/build dependency inspection. The final disabled
runtime CTest passes in 0.01s. The dependency audit is now an `ALL` build target
and a prerequisite of the runtime executable, including explicit test-target
builds. It retains exact direct links, neutral-device source/include restrictions,
and forbidden scene/host/terrain dependencies in the generated executable
closure. It runs every build, so edits/new headers/regenerated graphs cannot
reuse an old success stamp. The measured incremental qualification cost is
0.84s (excluded from runtime CTest); the initial disabled build plus qualification
was 42.47s on the shared host. Direct-link and neutral-source assertions are retained. **Graph scope change:**
the old standalone check scanned all of `build.ninja`; the new guard scans
the generated runtime executable dependency closure. Unrelated nontransitive
host/scene/terrain targets in a larger build are no longer rejected. Forbidden
dependencies actually reaching the compute executable remain rejected.

CMake 3.27+ supplies the actual generated file-api codemodel. The original
CMake 3.25/Ninja combination remains supported through Ninja's generated target
graph. Qualification also passed with Unix Makefiles and an empty build type;
the Ninja fallback passed separately. Negative probes rejected an additional
direct link, an actual generated transitive `script_host.cpp` dependency, and a
new neutral header including `graphics/forbidden.h`. The marker source was
never compiled or executed; all ordinary builds use production implementations.
These receipts are `/tmp/clear-stone-99-dependency-probes/{direct,transitive,neutral}.log`.

The enabled backend retains every existing admission, cancellation, shape,
subtraction, determinism, capacity, stale-result, synchronization-validation,
and CPU-ownership guard. Both real Vulkan implementations and the four embedded
SPIR-V stages compiled on Linux and native MSVC. Initial GPU runtime was 32.47s
on Linux Lavapipe and 2.06s on native Windows. A Linux trace then discovered
validation-layer reads/writes of `shader_validation_cache-1000.bin`. The test
now disables validation-result caching (both old/current setting names) and
Mesa shader caching in its own process, so direct binary invocations also use
these settings. Shader and synchronization validation remain enabled; no GPU
assertion or workload was reduced. Persistent cache-reuse behavior is no
longer exercised; it was not an asserted geometry oracle. Settings are documented by
[Khronos validation-layer migration guidance](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/updating_from_VK_EXT_validation_features.md).
Final cache-disabled CTest passed in 30.85s on Linux with syscall tracing
(test time 30.75s), and 2.08s on native Windows (test time 2.07s).
The final Linux trace contains 52 test/descendant IDs and 68 opened platform
paths, with no test fixture/source/build/cache reads. The exact path inventory is
`/tmp/clear-stone-99-compute-enabled-final-file-read-audit.txt`; the raw trace
is `/tmp/clear-stone-99-compute-enabled-final.trace`.

Linux enabled configuration uses Ubuntu glslc/libshaderc and validation-layer
packages extracted under `/tmp/clear-stone-99-vulkan-packages`, the actual
system Vulkan loader and Lavapipe ICD, and `MATTER_GEOMETRY_COMPUTE=ON`.
There is no system package installation. Native enabled configuration uses the
pinned Vulkan SDK 1.4.357.0, MSVC 14.44.35207 and Windows SDK 10.0.26100.0.
The system loader/ICD/layer manifests, shared libraries and CPU/proc discovery
are platform reads, separately identified in the GPU trace; they are not geometry
fixtures. Source, asset, evidence, generated SPIR-V and build data are not runtime
inputs. The cache-disabled audit passes. Native runtime passes with the same
in-process settings; native syscall tracing is not claimed.

SurfaceLib's actual `tests/Makefile` built eight real `build/*` binaries in place
(serial build to avoid the older shared transient C objects). The new
`build/staged_normals_tests` target gives that existing test a reproducible
Makefile path and uses private C objects. Runtime `ctest -j4` passed all eight
in 0.41s under syscall tracing. Every test/descendant (nine process/thread IDs)
opened only the loader cache and libc/libgcc/libm/libstdc++.
The report is `/tmp/clear-stone-99-msl-make-file-read-audit.txt`.

The exact build command was:

```sh
make -C libs/MatterSurfaceLib/tests -j1 \
  CFLAGS='-O3 -std=c++17 -Wall -Wno-missing-braces -Wno-unused-variable -DPLATFORM_DESKTOP -DMATTER_VULKAN_ONLY -UNDEBUG' \
  LDFLAGS='../../../MatterEngine3/src/render/vulkan_only_compat.cpp -lm -lpthread -ldl -lrt' \
  LDLIBS= build/mesh_simplifier_tests build/mesh_indexed_tests \
  build/mesh_transform_tests build/mesh_smooth_tests \
  build/material_registry_tests build/oriented_cube_tests \
  build/blas_refcount_tests build/staged_normals_tests
```

This links the repository's actual CPU allocation/mesh-release implementation,
`vulkan_only_compat.cpp`, in place; no mock/stub replaces production behavior.
C++ sources use the listed macros and `-O3`; existing C rules use `-O2` and
`PLATFORM_DESKTOP`. Each runtime binary is registered by absolute path into
`/tmp/clear-stone-99-msl-ctest/build`; registration reads happen in CTest itself.

| Make target | Production source closure, plus the shared compatibility TU |
| --- | --- |
| `mesh_simplifier_tests` | `mesh_simplifier.cpp`, `mesh_indexed.cpp` |
| `mesh_indexed_tests` | `mesh_indexed.cpp` |
| `mesh_transform_tests` | `mesh_indexed.cpp`, `mesh_transform.cpp` |
| `mesh_smooth_tests` | `mesh_smooth.cpp` |
| `material_registry_tests` | `material_registry.c` |
| `oriented_cube_tests` | `oriented_cube_algorithm.cpp`, `material_registry.c` |
| `blas_refcount_tests` | `blas_manager.cpp`, SpatialQueryLib `bvh.cpp`, `vertex_ao.cpp`, `occupancy.cpp` |
| `staged_normals_tests` | `surface.c`, `fat_primitive.c`, SpatialQueryLib `spatial_hash.c`, MemoryLib `mem_pool.c` |

## Shared handoff and remaining qualification

Supervisor answer `msg-ace14b00e1a94bcf9995aa15d4e6bdd5` assigns the whole
`geometry_hierarchy_tests.cpp` and `solid_source_evaluation_tests.cpp` TUs,
shared fixture transport, common native CMake and
`MatterEngine3/tools/register_core_ctest.py` to steady-harbor-62. They are not
edited here. The core owner retains mandatory cache admission, failed-commit
and concurrent-reader build qualification outside timed disk-free CTest,
with generated-memory runtime cases. No completed AssetStore memory backend
is assumed. Moving those guards changes the runtime/build boundary and its
cost must be disclosed by the delivering owner.

Published scripting API is `ScriptHost::set_module_sources(optional<vector<ResolvedModule>>)`:
owned module bytes, strict no-disk fallback for explicit empty input, filesystem
roots restored by nullopt, and fold invalidation on replacement. Its delivered
OID is `9b689a8c03609948e454a890d52ec900a8a00328`. Core retains CastleStone
8-seed/23-operation identity, dependency-change, cancellation/runaway and
prior-result guards using its actual shared fixture implementation.

Core must deliver its qualified graph/OID and the next serialized full native
build slot. That final-source full build is required by this task's component
CMake change; the native component/slice receipts above do not substitute for it.
CI files belong to keen-current-19. Coordination is on
`clear-stone-99-native-qualification`. This checkpoint does not close the task
or claim the aggregate project suite meets its one-minute goal.
