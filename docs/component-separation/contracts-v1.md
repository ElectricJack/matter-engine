# Delivered shared contract gate v1

Task `clear-crest-48.1.4` freezes the native data/service boundary for parallel
extraction. The implementation specification remains
[contracts v1](2026-10-10-matter-component-contracts-v1.md); the architecture is
[component separation](2026-10-10-matter-engine-component-separation-design.md).
This is an executable engineering gate. AQ's integration owner delivers its
published source commit before dependent extraction tasks become ready.
Passing local tests or publishing the worker branch alone does not prove delivery
to the project integration base.

The gate builds independent native callers/callees, validates owned data and
resolved bytes, and checks job admission with fake services. It does not supply
production geometry evaluation, rendering, terrain meshing, a script host or a
C/WASM geometry implementation. No performance or visual acceptance follows from
these fixtures. Native/browser geometry equivalence, rendered material parity,
terrain continuity and complete legacy feature preservation remain extraction
and integration deliverables.

## Versions and immutable input manifest

[gate-manifest-v1.json](../../libs/MatterContracts/tests/gate-manifest-v1.json)
records full prerequisite commits, the fixed scaffold dependency map, all new
fixture IDs and SHA-256 for 17 inputs: seven public contract headers, both MathLib
headers, the wire schema, RNG vectors, canonical JS helper, two wire examples,
legacy native RNG/raymath headers and component ownership manifest. The executable
`gate_manifest.py` checks these identities and rejects unrecorded public headers.
Changing frozen inputs requires an explicit compatibility/version decision and
an updated gate manifest; passing a stale manifest is impossible.

| Public header in `libs/MatterContracts/include/matter/contracts/` | Version/profile | Frozen boundary |
| --- | --- | --- |
| `contracts.h` | Schema 1.0; `matter-exact-json-v1`; `matter.splitmix64.v1` | Recipes, assets, diagnostics, materials, optional typed products, transform conventions |
| `jobs.h` | Service v1 | Immutable job results, request/revision/epoch admission, cancellation, budgets, progress |
| `generation.h` | Service v1 | Resolved inputs and owned payload products; geometry evaluate/query/process declarations |
| `hosts.h` | Service v1 | Frozen sources, resolver/authoring/cache/build declarations with injected services |
| `graphics.h` | Service v1 | Uploads, tickets, instance updates, camera/light state, observations and release declarations |
| `terrain.h` | Service v1 | Field/region/detail envelopes, boundaries and validated product admission |
| `abi.h` | ABI major 1, minor 0 | Pure C, size/version tags, fixed-width statuses/handles, function table and release |

C1 source commit: `73d1432e37d13e7d4863ec5ef5bab55a3429c3f1`.
C2 source commit: `9425ecbf9c53b921f9d583ef60188affee1e089c`.
Gate integration base: `6ef108e22f7ed00ed1b7eb8d136f70b22ba8ab5e`.
The gate's own exact delivered commit is obtained from the integration receipt;
the source commit can also be found with
`git log -1 --format=%H -- docs/component-separation/contracts-v1.md`.
Embedding its own commit SHA in this file would change that SHA.

## Fixed file ownership

The baseline [ownership map](ownership.md) remains authoritative for central
writers. Its source provenance is unchanged. This table freezes the parallel
destinations and allowed direct dependencies; every module owns its local build
file and tests. All non-contract native targets are currently interface-only
scaffolds, even when a matrix build succeeds.

| Owner/module | Owned paths | Allowed dependencies |
| --- | --- | --- |
| Shared contracts | `libs/MatterContracts/`; this gate document | Standard library and neutral `libs/MathLib/include/` |
| Geometry | `libs/ProceduralGeometry/` | Contracts, audited CPU algorithms |
| Authoring SDK | `packages/matter-authoring/` | JS language, injected host interfaces |
| Native host adapter | `MatterEngine3/src/script/portable_host/` | Contracts, SDK adapter, QuickJS |
| Build coordinator | `libs/ProceduralBuild/` | Contracts; injected resolver/cache/authoring/geometry |
| Graphics | `libs/MatterGraphics/` | Contracts, graphics platform/backends/shaders |
| Terrain | `libs/TerrainGeneration/` | Contracts, geometry, audited field algorithms |
| Processing | `libs/AssetPipeline/` | Contracts, geometry, explicitly selected processing modules |
| C/WASM binding | `bindings/geometry-c/` | Contracts, geometry |
| Browser integration | `integrations/web-procedural/` | SDK, C/WASM wrapper, independent renderer adapter |
| Matter coordination | `MatterEngine3/src/component_adapters/` | Public interfaces of connected modules |

Central manifests, ScriptHost/DslState and live renderer/world wiring stay with
ordered integration tasks `.9.1`, `.9.3`, `.9.4`; final build/toolchain enforcement
belongs to `.9.5`, retirement to `.9.7`. Full IDs and file assignments are in
[ownership.md](ownership.md). This gate edits no central entrypoint or manifest.

## Compatibility and capability table

| Boundary | Established by this gate | Limitation/required consumer behavior |
| --- | --- | --- |
| Math/legacy placement | Row-major float32; column vectors; translation 3/7/11; `parent * local`; exact noncommuting raymath comparison | Legacy `MatrixMultiply(a,b)` converts to `mm::multiply(b,a)`. Mirrored placement reverses winding/tangent w; normals use inverse transpose. Normal-ratio check uses absolute `1e-6` (dimensionless). |
| Recipe/identity | Exact ordered CSG and canonical round trips; optional metadata excluded from semantic digest | Wire schema 1.0 only; order is semantic. Geometry algorithms are not exercised. Legacy cache hashes retain explicit namespaces. |
| RNG | 4 pinned seeds, 8 word/unit known answers each across C++/JS; 128 draws per seed against actual native `dsl::Rng` | Native zero seed maps to `0x9e3779b97f4a7c15`; xoshiro shared-lib RNG is a different algorithm. |
| Material/instances | Opaque PBR references, exact factors, shared prototypes and separate instance IDs survive serialization | Color spaces, channels and texture sampling follow the specification. No GPU upload or rendered appearance is measured. |
| Required capabilities | Core recipe, asset and PBR version 1; unknown names/versions fail explicitly | Optional attachments/LOD/collision/animation/terrain require document declaration and consumer opt-in. Data support does not advertise runtime implementation. |
| Resolved mesh payloads | Length/type/digest/finiteness and actual little-endian uint32 indices checked before evaluation | Out-of-range indices now fail `invalid_input` with node/source diagnostics. Description-only validation cannot verify absent bytes. |
| Native ownership/jobs | Separate caller/callee, immutable owner lifetime, cancellation/budgets, full envelope admission | Fake completion returns hand-built geometry. Cooperative checks do not establish scheduler or hard timeout behavior. |
| C ABI | C11 producer to C++17 caller and C caller; size/version/status/handle and explicit release checks | Same-toolchain native builds tested; no stable cross-compiler C++ binary ABI promised. C result wire encoding/export implementation remains binding work. |
| Terrain | Serialized extension, high precision origin, boundary/detail envelope and stale completion admission | Adjacent chunk/resolution metadata tested with fake products; seam continuity and transitions require terrain algorithm fixtures. |
| Graphics independence | Public graphics consumer compiles/runs with all extraction targets disabled; imported synthetic asset input | Shares neutral resolved-input types from `generation.h`; links no geometry/terrain/host implementation and calls no generator. No actual renderer is supplied. |
| Browser/WASM/native feature matrix | C++/JS canonical metadata and RNG exactness only | WASM geometry, browser disposal/heap-growth tests, rendered views, optional runtime modules and performance remain unverified. |

Cross-target geometry must establish topology rules and absolute/relative
tolerances in metres before accepting evaluated outputs. This gate compares
synthetic data and exact metadata, and establishes no evaluated-geometry tolerance.

## Executable fixture IDs

| IDs | Test and assertion |
| --- | --- |
| `GATE-C1-PRIMITIVE`, `GATE-C1-EXTRUDE`, `GATE-C1-CSG` | `gate_native`: public recipe construction/validation; ordered subtraction operands and content identity |
| `GATE-C1-CANONICAL`, `GATE-C1-ASSEMBLY`, `GATE-C1-MATERIAL`, `GATE-C1-AFFINE` | Exact decimal profile, nonsemantic metadata, prototype sharing, material assignment, mirrored/nonuniform point and normal transforms |
| `GATE-C1-MALFORMED`, `GATE-C1-EXHAUSTION`, `GATE-C1-TERRAIN-EXTENSION` | Missing references, bounded nodes, explicit terrain opt-in and serialized extension |
| `GATE-C2-RESOLVED-INDEX`, `GATE-C2-RESOLVED-LENGTH` | Actual out-of-range uint32 indices (including a high byte) and truncated verified payload rejection |
| `GATE-C2-OWNER`, `GATE-C2-CANCEL`, `GATE-C2-BUDGET` | Separate callee boundary; aliased immutable ownership; callback cancellation and vertex budget publish no product |
| `GATE-C2-TERRAIN`, `GATE-C2-LATE`, `GATE-C2-CAPS` | Product shape, origin/detail and complete envelope; invalidation preserves prior owner; exact boundary policy version |
| `GATE-C2-GRAPHICS` | `gate_graphics`: standalone public consumer, synthetic/imported mesh data and shared instance identity |
| `GATE-C-ABI` | `gate_c_abi`: real C11 callee/C++17 caller linkage, versioned function table, caller-buffer sizing, read/release |
| `GATE-LEGACY-MATH`, `GATE-LEGACY-RNG` | `matter_contracts_legacy_checks`: legacy operand reversal and frozen native RNG sequence |
| `GATE-MANIFEST`, `GATE-INDEPENDENT-GRAPH` | Python standard-library audit: hashes, public includes, exact native CMake File API targets/edges/include paths/sources; extraction scaffolds absent |

Retained C1 fixtures include `ordered-csg-v1`, `extruded-prop-v1`,
`repeated-assembly-v1`, `typed-products-v1`, `explicit-mesh-v1`,
`assembly-recipe-v1`, `texture-material-v1`, `malformed-v1`, `exhaustion-v1`.
Retained C2 fixture IDs and consumer obligations are listed in
[SERVICES.md](../../libs/MatterContracts/SERVICES.md), including fake adjacent/detail
terrain products, stale admission, shared upload lifetimes, disposal and 1,000
C allocate/copy/release cycles. All remain mandatory; no test is waived.

## Exact repeatable commands and evidence

Supported native PowerShell (test dependency isolated under ignored `build/`):

```powershell
libs/MatterContracts/tests/prepare-test-python.ps1
libs/MatterContracts/build-windows.ps1 -Config RelWithDebInfo -Test -Matrix `
  -TestPython "$PWD/libs/MatterContracts/build/test-python/Scripts/python.exe"
```

Equivalent tested WSL invocation:

```bash
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe \
  -NoProfile -ExecutionPolicy Bypass \
  -File "$(wslpath -w "$PWD/libs/MatterContracts/build-windows.ps1")" \
  -Config RelWithDebInfo -Test -Matrix \
  -TestPython "$(wslpath -w "$PWD/libs/MatterContracts/build/test-python/Scripts/python.exe")"
```

Independent GNU/Linux checks:

```bash
cmake -S libs/MatterContracts -B /tmp/matter-contract-gate -G Ninja \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/matter-contract-gate -j 2
ctest --test-dir /tmp/matter-contract-gate --output-on-failure -j 1
python3 libs/MatterContracts/tests/build_matrix.py --cmake /usr/bin/cmake --generator Ninja
```

The standalone executable-fixture root also builds directly:

```bash
cmake -S libs/MatterContracts/tests/independent -B /tmp/matter-contract-gate-repro \
  -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/matter-contract-gate-repro -j 2
ctest --test-dir /tmp/matter-contract-gate-repro --output-on-failure -j 1
```

The full contract CTest set has 10 tests. The matrix has 17 configurations: the
original 16 scaffold/probe configurations plus the independent root's compiled
caller/callee/graphics/C ABI fixtures and dependency audit. Node and Python are
required for the full gate; `jsonschema==4.26.0` is a test-only dependency.
Missing tools/packages fail explicitly. Plain `BUILD_TESTING=OFF` consumption
needs neither Node nor Python.

Gate run results: MSVC 19.44.35211.0 with native CMake 3.31.6-msvc6 passed
10/10 CTests and 17/17 matrix configurations. GNU 13.3.0 with CMake 3.28.3
passed the same 10/10 CTests and 17/17 configurations. The resolved-index fixture
failed against the prerequisite code and passed after the validation fix.

Evidence is recorded in the task comments/close receipt with exact source commit,
commands and results. Logs for this gate run: `/tmp/matter-contract-gate-repro.log`
(failure before fix, success after), `/tmp/matter-contract-gate-native.log`,
`/tmp/matter-contract-gate-matrix-native.log`,
`/tmp/matter-contract-gate-msvc-final.log`. The initial MSVC schema failure due
to missing native `jsonschema` is retained in `/tmp/matter-contract-gate-msvc.log`;
the isolated test venv resolves that prerequisite without changing global Python.
Final post-edit CTest results are also retained in
`/tmp/matter-contract-gate-native-recheck.log` and
`/tmp/matter-contract-gate-msvc-recheck.log` (both 10/10 passed).
