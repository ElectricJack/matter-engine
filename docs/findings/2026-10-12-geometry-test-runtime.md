# Geometry/surface test runtime — clear-stone-99

Measured on 2026-10-12 from base `1f4ff089a9a81f8f54528df6a131a70757648853`.
This is the portable geometry/surface slice, not the aggregate project suite.
The shared fixture/API migration remains pending; see the handoff below.

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

No production code or repository CMake/CI files changed in this checkpoint.

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

Standalone GeometryCompute's existing Linux build and both CTest checks passed
in 0.76s (`matter_geometry_compute`: 0.01s; dependency check: 0.75s), with the
existing optional compute backend disabled. No GPU-enabled execution is claimed.
Its actual GeometryCompute, ComputeDevice and MatterContracts libraries and
solid test also compiled with native MSVC; `matter_geometry_compute` passed in
0.11s (0.19s CTest wall time).
The dependency check currently reads source/build files, so it is outside the
25-test no-fixture-read result above.

## Shared handoff still pending

Supervisor coordination is on `clear-stone-99-memory-fixtures`, with detailed
needs recorded in message `msg-13661d167d824874861943e05cdf45ec`:

- `geometry_hierarchy_tests.cpp` combines cheap compiler/cut/page-codec guards
  with disk-backed BlobStore, RefTable, PageCache, RootCache, prepared-cache and
  PartStore assertions. Preserve failed-commit, admission and race guards in
  the storage owner's in-memory migration rather than silently dropping them.
- `solid_source_evaluation_tests.cpp` needs the script owner's in-memory module
  API to retain dependency-invalidation and seeded-operation guards without
  reading authored JS or temporary module files. Other script-backed surface
  fixtures require the same ownership coordination.
- GeometryCompute's source/build dependency contract should move to configure
  time under the CMake owner; the runtime dependency check currently reads disk.
- The shared Linux CTest graph and any resulting full native build must be
  coordinated with the shared CMake owner. CI files belong to keen-current-19.

This checkpoint does not close the task or claim those pending migrations pass.
