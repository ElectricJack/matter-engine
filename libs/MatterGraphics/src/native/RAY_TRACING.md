# Production ray tracing, lighting and GI relocation

Task `clear-crest-48.4.7` moves the existing implementation from reference commit
`6490dbab806876631465259cdf5b397f258c5837`. The governing directive is a pure
separation refactor: preserve algorithms, shader ABI, specialization, synchronization,
fault handling and native behavior. The earlier raster relocation establishes
the private-fragment pattern in [README.md](README.md).

## Ownership and inputs

The files below retain their namespaces and existing typed inputs: native mesh
and material records, instance transforms, `WorldLights` value records, camera
and environment data, `VulkanRayTracingSettings`, `VulkanGiSettings`, and sparse scene data.
`world_lights.*` contains value types and the CPU light index, not a world session.
None of the moved routines calls a generator or accesses `ScriptHost`/`WorldSession`.

The `.inl` files are private fragments included exactly once at their original
lexical positions. They retain the existing renderer's device, resources, pipeline
and frame state, helpers, macros and fault injection. The renderer still owns
shared initialization, allocation, descriptors, reset/release and destruction.
BLAS/TLAS creation, update/retirement decisions, dispatch and submission commit
remain the original implementation, using that retained host state. Sparse
ray-query acceleration also continues to serve surface-query and raster consumers.
Do not compile fragments separately or treat this as a standalone producer API.
Facade capability negotiation is unchanged; proposed producer names in
[RENDERING.md](../../RENDERING.md) remain unadvertised and rejected there.

## Moved sources

Full files retain legacy forwarding locations, except shader entrypoints, whose
physical sources are mapped to the same logical embedded SPIR-V names in CMake
and Make. No parallel implementation remains at the old location.

| Original location | Component location | Preserved content |
| --- | --- | --- |
| `MatterEngine3/src/render/` | This directory | `blas_disk_cache.h`, `vk_blas_cache.h`, `vk_gi_contract.h`, `vk_gi_math.*`, `vk_lighting_controls.*`, `primary_light_culling.h` |
| `MatterEngine3/src/` | This directory | `world_lights.*`: typed lights, sparse CPU grid and oversized-light list |
| `MatterEngine3/shaders_vk/` | `../../shaders/` | All `rt_*` stages/helpers; `gi_common.glsl`, `gi_temporal.comp`, `gi_atrous.comp`, `local_light_types.glsl`, `local_lighting.glsl`, `primary_light_cull.comp`, `sparse_voxel_shadow.comp`, `sparse_voxel_optics.glsl` |
| `MatterEngine3/tests/` | `../../tests/native/` | `local_light_index_tests.cpp`, `primary_light_culling_tests.cpp`, `blas_disk_cache_tests.cpp`, `blas_cache_queue_tests.cpp` |

Fragment ranges below are inclusive line numbers in the reference commit's
`MatterEngine3/src/render/vk_scene_renderer.cpp`, except the three marked sparse
fragments, which came from `vk_sparse_voxel.cpp`. These ranges permit exact
comparison without relying on Git's similarity heuristics for large moves.

| Fragment | Original lines | Retained routine boundary |
| --- | --- | --- |
| `rt_contract.inl` | 338–405 | Counter/filter/history constants and opaque-material classification |
| `rt_selection.inl` | 1496–1567 | Native mesh/billboard and LOD geometry selection |
| `sparse_shadow_scene.inl` | 1837–1861 | Typed sparse/shared shadow input and timing readback |
| `rt_settings.inl` | 2525–2562 | RT and GI settings validation/invalidation |
| `gi_pipelines.inl` | 3215–3338 | Temporal and atrous pipelines |
| `rt_pipeline.inl` | 3340–3885 | RT layouts, variants, pipelines and SBT |
| `local_lights.inl` | 8525–9027 | Light/grid upload, descriptors, culling, direct-light owner and audit |
| `rt_test_fixtures.inl` | 10675–10969 | Existing surface/BLAS/counter/scratch fault fixtures |
| `gi_test_fixtures.inl` | 10971–11622 | Existing temporal/atrous GPU fixtures |
| `gi_filters.inl` | 11867–12293 | Temporal and atrous recording for existing signal lanes |
| `rt_frame_commit.inl` | 12295–12412 | BLAS statistics, successful/failed submission commit, history promotion |
| `rt_dispatch.inl` | 15403–17119 | Geometry/BLAS builds, instances/TLAS, shadows, ray dispatch and diagnostics |
| `sparse_shadow_create.inl` | 146–181 (sparse) | Sparse/shared shadow-caster construction |
| `sparse_acceleration.inl` | 214–519 (sparse) | Shared root buffers, AABB BLAS/TLAS, query/shadow pipelines |
| `sparse_shadow_dispatch.inl` | 1301–1355 (sparse) | Shadow dispatch and timestamps |

Expanding these 15 new includes reconstructs both reference host sources byte
for byte. All 42 whole-file moves also match their reference bytes after reversing
five include-path changes (the culling header and four relocated tests). This
accounts for **11,884 moved lines**: 6,902 in whole files and 4,982 in fragments.
New implementation consists only of forwarding includes and build/path adapters;
there are no new renderer algorithms or shader changes. Large new implementation
files are the moved code identified above, not newly written implementations.
The adapters total 142 added/modified lines, including comments/blank lines:
49 whole-file forwards, 15 fragment includes, 64 module build lines, six root
build lines, three dependency-audit lines and five adjusted includes within the
moved files. Documentation is additional; this file records provenance and
qualification, not a new implementation.

## Qualification

Use the supported native Windows/MSVC wrappers. The root build retains the old
CPU test target names through forwards, and the native smoke executable compiles
the relocated fragments through the production host. The standalone component's
dependency audit tracks the moved local-light GLSL includes; its portable facade
still has its own independent build and tests.

Native qualification uses NVIDIA GeForce RTX 4090, NVIDIA 610.74, Vulkan API
1.4.341, MSVC 14.44.35207 and Vulkan SDK 1.4.357.0, RelWithDebInfo. Commands:

```sh
./tools/build-windows-from-wsl.sh RelWithDebInfo preflight
./tools/build-windows-from-wsl.sh RelWithDebInfo vulkan_smoke_tests
for target in local_light_index_tests primary_light_culling_tests blas_disk_cache_tests blas_cache_queue_tests; do
    ./tools/build-windows-from-wsl.sh RelWithDebInfo "$target"
done
```

All builds passed, including all 21 relocated shader stages. Native CTest with
`-j 1 -R '^(local_light_index_tests|primary_light_culling_tests|blas_disk_cache_tests|blas_cache_queue_tests)$'`
passed all four tests. `ctest.exe` is resolved from the same native toolchain;
the root test directory is `MatterEditor/build/cmake/windows-msvc/relwithdebinfo`.
The Make rollback graph was checked with `make -n` for RT lighting, temporal GI,
primary-light culling and sparse shadows; it selects the new physical sources.

The native GPU fixture selector is `MATTER_VK_SMOKE_MODE`. Run
`MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe`
with working directory `MatterEditor`, one process at a time. The bounded runner
uses the process/validation checks in `MatterEditor/tools/smoke_vulkan_faults.ps1`.
Each mode must report `ALL PASS`, exit zero and `validation errors: 0`; the
known failing cases below are recorded as failures, not converted into passes.

| Mode | Result | Scope |
| --- | --- | --- |
| `rt` | PASS, 128.5 s | Surface/material queries, LOD/transform mapping, BLAS pinning/build/update/retirement, failed-frame rollback, GI histories, shadows, diffuse/specular filtering and reflection |
| `rt-transmission` | PASS, 25.2 s | Roughness blur, energy, clear fallback and alpha walk; all four energy rungs 0.95951 |
| `rt-local-ris`, sampling 1 | PASS, 141.1 s | 256 seeded samples against exhaustive secondary lighting, all three RGB means within measured uncertainty |
| `gi-firefly` | PASS, 15.3 s | Temporal/atrous rejection, signal filtering and local-direct history |
| `rt-empty-tlas` | PASS, 14.2 s | Empty-scene RT and volumetric interaction |
| `sparse-voxel` | PASS, 49.3 s | Existing sparse raster, query, temporal and shadow suite |
| `rt-disabled` | PASS, 16.4 s | Raster/disabled-RT compatibility |
| `rt-unavailable` | PASS, 8.6 s | Forced unsupported device, explicit reason, zero RT dispatches, mode/extent changes and cleanup |
| `primary-light-cull` | 6 known failed assertions | Exact recorded baseline match, validation errors 0 |
| `vt-rt` | 12 known failed assertions | Exact preceding raster-relocation baseline match, validation errors 0 |

Every passing GPU mode above reported zero validation errors, including the
existing teardown checks. Loader diagnostics about missing/duplicate Epic overlay
JSON files were separate from the Vulkan validation count.

The sampling fixture explicitly sets `MATTER_RT_SECONDARY_LIGHT_SAMPLING=1`.
An initial run with inherited/default sampling was terminated by a 120 s harness
timeout and is not counted as a pass. The completed sampling-1 run used a 300 s
bound with unchanged fixture assertions; its RGB means were
`(0.03812000, 0.06759033, 0.08596975)` against exhaustive means
`(0.03951406, 0.06685448, 0.09147406)`.

`primary-light-cull` reports the same six assertions as the recorded baseline:
five `GPU tile mask covers partial tiles and more than64 lights` assertions and
one `moving an ordinary light removes its ID without reordering oversized lights`.
It reports zero validation errors. See
[`input-idle-rt-failure-comparison.json`](../../../../docs/agent/evidence/2026-09-15-vt-feedback/input-idle-rt-failure-comparison.json).
Neither this fixture nor its implementation was changed to hide those failures.

`vt-rt` reproduces all 12 assertions in
[`2026-10-11-graphics-producer-move/vt-rt.txt`](../../../../docs/agent/evidence/2026-10-11-graphics-producer-move/vt-rt.txt),
covering transported VT slots, page resolution, UV density and cone-mip selection.
Both known failure lists were compared programmatically, with no additional
assertions. No fresh baseline build or test weakening was used.

The standalone build used `libs/MatterGraphics/build-windows.ps1 -Config RelWithDebInfo`,
then native `ctest.exe --test-dir libs/MatterGraphics/build/windows-msvc/relwithdebinfo --output-on-failure -j 1`.
All five tests passed: lifetime/capability rejection, scene, composition, Vulkan
and independence (including the exact relocated shader dependency closure).
The standalone Vulkan fixture reported the RTX 4090 with `validation=1`.
`git diff --cached --check` and the source reconstruction checks also passed.
