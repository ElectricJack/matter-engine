# Baseline commands and qualification profiles

Run from the repository root. `fixtures.json` maps IDs to inputs, native
targets, assertions and gate gaps. `baseline.json` distinguishes checks run by
this task from historical native evidence. No extracted component or WASM
benchmark was run here.

## Existing supported commands

```sh
python3 docs/component-separation/verify.py
aq test tools/tests/test_source_manifests.py -q
python3 tools/check-source-manifests.py --root .
./tools/build-windows-from-wsl.sh RelWithDebInfo preflight
node projects/world_demo/tests/castle_plan_tests.mjs
node projects/world_demo/tests/mountain_geometry_site_tests.mjs
node projects/world_demo/tests/mountain_terrain_only_tests.mjs
node tests/vt_normal_frame_reference_tests.mjs
python3 tools/ci-component-matrix.py --jobs 2 --emsdk ~/emsdk --require wasm
```

`tools/ci-component-matrix.py` (task `.9.5`) is the component build matrix.
Each extracted configuration configures, builds and tests from its own
directory: neutral contracts, headless geometry, graphics-only, terrain-only,
build, asset pipeline, the geometry C binding, the portable QuickJS host and
the two engine adapters. It then runs the source-manifest, dependency/ownership
and fixture gates and the WASM geometry smoke, which uses the native C consumer
as its oracle. The command and its NOT RUN rules are documented in
`tools/component-separation/README.md` ("CI"). The hosted `component-matrix`
job in `.github/workflows/tests.yml` runs it without an Emscripten or Vulkan
SDK, so the WASM gate and the `graphics/vulkan` probe are NOT RUN there.

Two checker results were red at `.9.5` until their moves landed; neither was
waived:

- `repository/-/compiled-twice`: `terrain_field.cpp` was compiled by both
  `asset_pipeline` (`matter_asset_surface_runtime`) and `terrain`. Task `.9.11`
  moved the field kernel into geometry's `matter_geometry_field`, which both
  link, so the asset pipeline no longer lists the terrain source or headers.
- `graphics/vulkan/build`, seen only where the Vulkan SDK is available (lavapipe
  with `--host-cache`): `matter_graphics_environment` referenced resource
  helpers that only the host defined. Task `.9.12` moved `vk_resources.cpp` and
  the device-lifetime registry into `libs/MatterGraphics/src/native`, so the
  whole-archive probe links without the engine. The engine headers the moved
  sources still read are audited one by one in `rules.json`, with the symbol
  evidence for each. Graphics' own sources include their sibling headers, not
  the engine's compatibility forwarders.

The source-manifest unit tests pass. At the baseline's pinned commit, the
repository manifest gate failed because tracked `libs/MemoryLib/src/mem_bank.c`
was unclassified. Task `.9.5` classifies it, together with the other MemoryLib
sources that only a Make build compiles, so `tools/check-source-manifests.py`
now passes.
The known historical shader-source failure is also retained; neither issue is
fixed or suppressed by this documentation task.

Windows native CMake/MSVC is the canonical acceptance toolchain. The WSL
wrapper invokes native PowerShell and the pinned Windows toolchain resolver;
it is not a Linux compiler path. Preflight verifies VS/MSVC, Windows SDK,
native Python and Vulkan tools. Existing root headless builds still require
the root toolchain/shader setup and link native host/optional-domain libraries.
They do not establish component independence. The legacy per-project Make
path remains documented in `CLAUDE.md`; use the supported Windows wrappers
for the native acceptance below.

Build only selected targets; then run CTest serially. Example for the small
shared-assembly fixture:

```sh
./tools/build-windows-from-wsl.sh RelWithDebInfo part_graph_tests
```

Run native PowerShell from the repository root (or pass this script through
native `powershell.exe` using a script file):

```powershell
Import-Module ./tools/windows/MatterWindowsToolchain.psm1 -Force
$componentToolchain = Resolve-MatterWindowsToolchain -RepositoryRoot (Get-Location).Path
$componentCTest = Join-Path (Split-Path $componentToolchain.CMake) 'ctest.exe'
& $componentCTest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo `
    --output-on-failure -j 1 -R '^part_graph_tests$'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

For another fixture replace the build target and exact CTest name with its
manifest entry. For `RENDER-NORMAL`, build `vulkan_smoke_tests` and select
`^smoke_vt_normal_frame$`. CTest supplies that mode's runtime environment and
120-second timeout. GPU/scene captures require the normal admission and
serial recipes in `docs/agent/qa-cookbook.md`; do not launch multiple editors
or GPU suites concurrently. OBJ goldens bake all three selected parts; keep
`MATTER_EXPORT_GOLDEN_UPDATE` unset so a failure cannot silently update its
oracle. Current CTest registration supplies the required working directories.

Native focused targets selected by this map:

| Area | Build targets / CTest names |
| --- | --- |
| Props / export | `obj_export_golden_tests`, `obj_export_tests`, `script_host_tests` |
| CSG, transforms, seed | `script_host_tests`, `dsl_determinism_tests` |
| Assembly | `part_graph_tests`, `part_flatten_tests` |
| Bounded evaluation and jobs | `solid_source_evaluation_tests`, `async_bake_tests` |
| Terrain | `terrain_field_tests`, `terrain_mesher_tests`, `contour_seam_tests`, `sector_streaming_coordinator_tests` |
| Graphics | Build `vulkan_smoke_tests`; CTest `smoke_vt_normal_frame`, `vt_surface_material_tests`, `geometry_pages_gpu_tests` |
| Optional preservation | Inventory-specific targets in `inventory.md`; select only the capability being integrated. |

Full editor/package gates remain `matter_editor` and `matter_dist` through
the same wrapper. The package is `MatterEditor/build/dist/world_demo/` and
must retain `build_features.json`, assets and required DLLs. Existing
`tools/ci-windows.ps1` owns native CI/package verification. At the pinned
commit `.github/workflows/native-windows.yml` triggers on `main` and `aq/**`,
not `dev`; this is observed promotion setup, not proof of a completed dev
promotion pipeline. Task `.9.5` coordinates with the separate promotion CI
owner before changing this surface.

## Reserved future targets and commands

These are **provisional acceptance names**, not runnable baseline targets.
Module owners may settle local test names at the delivered contract gate and
record a mapping here. Do not add central registrations ahead of their owner.

| Configuration / reserved test target | Required proof |
| --- | --- |
| `matter_contracts` / `matter_contracts_tests`, `matter_contracts_consumer_tests` | Public headers compile alone; immutable validation, capability/version errors, budget/cancel and release ownership; fake services; no renderer/JS/ECS/filesystem/window includes or links. |
| `matter_geometry` / `matter_geometry_tests` | Same fixture recipes in native and WASM, metadata identity and declared geometry tolerances; no graphics, host, terrain or Matter archive linked. |
| `matter_build` / `matter_build_tests` | Shared-child DAG, deterministic identity/invalidation, cycles, bounded jobs and stale admission using injected services. |
| `matter_graphics` / `matter_graphics_consumer_tests` | Upload hand-built/serialized assets, render with no geometry/terrain generator linked, resource release and supported representation diagnostics. |
| `matter_terrain` / `matter_terrain_tests` | Generate chunks headlessly; adjacent/resolution-transition boundary tests; no renderer or streamer links. |
| `matter_geometry_c` / `matter_geometry_c_tests` | Native C consumer with opaque handles, versioned buffers, ownership/release and explicit failures. |
| Browser integration / conformance corpus | SDK + C/WASM wrapper and independent renderer; transfer/copy lifetime, heap growth, required capability diagnostics and fixture parity. Toolchain/browser/device versions recorded by its owner. |
| Full native, each optional module disabled/enabled, package | Existing native feature matrix and package remain usable; disabled dependencies absent from configure/build/link evidence. |

Each gate must publish a real configure/build/test command for its local
directory and supported native/Emscripten configuration. No `-S` path or
component-off flag is invented here for a target that does not yet exist.
Library names alone and a stub's pass are insufficient proof. Native browser
equivalence tolerances, Emscripten version and device budgets are unmeasured
and remain explicit gate deliverables.

## Provisional benchmark profiles

Correctness is a prerequisite. Do not compare stubs or change geometry,
LOD/material policy, scene coverage, texture density or work budgets between
the monolith and extracted implementation. No numerical latency/memory
acceptance threshold is agreed by this baseline.

| Profile | Corpus and provisional run protocol | Report |
| --- | --- | --- |
| `props-small` | `PROP-SLAB`, `PROP-FLOWER`, `GEOM-EXTRUDE`; serial native release-equivalent builds. One cold process per fixture, then 5 warmups and 30 measured evaluations with the same owned result/release policy. | Authoring, validation, geometry and processing separately; median/p95/max wall time, output counts/bytes, peak process memory and measured allocation count if instrumented. |
| `solid-source-csg` | `GEOM-CSG`, `PROP-STONE`, `EVAL-BOUNDED`; separate authoring-only owned recipe evaluation from CSG/meshing/export. Stone uses current 0.006 m detail, not old clamped geometry. Seeds 0–7 for the 23-operation source fixture. | Evaluation versus mesh time; cancellation latency; recipe/field identity and output counts; peak memory. No renderer timing attributed to geometry. |
| `assembly-shared` | `BUILD-SHARED`, `BUILD-CASTLE`; cold cache then warm unchanged DAG, then one child-parameter and one transitive shared-source edit. 5 warmups, 30 measured builds per case. | Resolver/evaluation/geometry work counts, unique products, cache hits/misses and invalidation scope; time/memory and release correctness. |
| `terrain-boundaries` | `TERRAIN-FLAT`, `TERRAIN-ADJACENT`, `TERRAIN-CONTOUR`; fixed analytic/noise sources, seeds, sizes/rungs/slabs from the tests. 5 warmups and 30 serial chunk batches. | Field/mesh/boundary time, triangles/bytes, peak memory, seam error with the existing oracle, late-result rejection in a separate coordinator test. |
| `render-mountain` | `RENDER-MOUNTAIN` at 1920×1080, seed 20260722 and the pinned camera; static default/POM off, VG compared only by explicit opt-in. Preserve three 128-resolution focal rocks, natural rocks/forest, terrain 128 texels/m and finest 0.25 m sampling. Serial cold and warm runs. Reuse the 45 s/300 s capture recipes and samples near 20 s/180 s in existing evidence. | CPU/GPU median/p95/p99/max by zone, startup/first-frame and cold/warm cache status, geometry/VT residency and readiness, actual rendered coverage plus identity/depth/normal screenshots. Numeric speedup is invalid when coverage differs. |

Record commit, compiler/options, CPU/GPU/driver, OS, browser/runtime versions,
power mode, concurrency, cache location/reset method, seed/params, input
digests, resolution, output counts, memory metric and raw samples for every
campaign. Hardware/run counts above are provisional; qualify them before
using the profiles as acceptance. Historical captures do not establish visual
parity or the 16.7 ms frame goal. Retain failures and unavailable measurements
explicitly; correctness pass and performance acceptance are separate results.
