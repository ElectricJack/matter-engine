# Geometry C / WASM build scaffolding

This directory owns the portable C binding's local build. `matter_geometry_c`
and its `matter_geometry` dependency remain interface scaffolds at this
milestone. The WASM smoke compiles the delivered contract implementation and
fake service/ABI fixtures; it does not implement a geometry evaluator or export
a production C ABI. Those belong to the geometry track and `clear-crest-48.6.2`.
The [frozen contract gate](../../libs/MatterContracts/tests/gate-manifest-v1.json)
and [ownership map](../../docs/component-separation/ownership.md) are prerequisites.

## Supported toolchain and clean reproduction

[`wasm/toolchain.json`](wasm/toolchain.json) pins the supported baseline:
Emscripten **5.0.2**, compiler revision
`dc80f645ee70178c11666de0c3860d9e064d50e4`, SDK Node **22.16.0**,
CMake **3.28.3**, Ninja **1.11.1**, Python **3.12.3**. This is a tested
configuration, not a request to follow `latest`. The compiler revision matches
the [upstream 5.0.2 release](https://github.com/emscripten-core/emscripten/releases/tag/5.0.2).
Updates require a new recorded smoke run and explicit changes to the lock.

Install/activate the named SDK outside the repository using
[emsdk's specific-version workflow](https://emscripten.org/docs/tools_reference/emsdk.html):

```bash
./emsdk install 5.0.2
./emsdk activate 5.0.2
```

From the repository root on Linux/WSL, with the pinned host tools on PATH:

```bash
python3 bindings/geometry-c/wasm/build-smoke.py \
  --emsdk /path/to/emsdk --build-dir /tmp/matter-wasm-contract-smoke
```

The build directory must be absent or empty. The wrapper validates every tool
version and the compiler revision, selects SDK Node, sets a private `EM_CACHE`,
configures the local CMake graph, builds serially, and runs the focused CTest
fixtures serially. It never installs software or deletes an existing directory.
`smoke-evidence.json` records the environment, exact commands, exit codes and
WASM byte counts/SHA-256 hashes, including failed build/test exits. Full compiler
and CTest output can be redirected to a log by the caller.

The official Emscripten CMake toolchain is selected directly. No central native
manifest/preset, Makefile, Windows wrapper, shader build or editor dependency
is involved. A native standalone `cmake -S bindings/geometry-c -B <dir>` still
registers only its component scaffolds and contracts. The supported Windows
product wrappers remain authoritative for actual Matter acceptance; this
milestone's WASM configuration is validated on Linux/WSL.

## Baseline profile: `matter-wasm32-worker-v1`

The contract runs synchronously inside one host-created ordinary worker, with
one private wasm32 memory. Pthreads, shared memory, Emscripten Wasm Workers,
SIMD/relaxed SIMD, memory64, filesystem support, fast math and FP contraction
are disabled. Compile guards reject incompatible inherited thread/SIMD/math
flags. The Node smoke runs in a `worker_threads` worker and verifies that its
heap buffer is an `ArrayBuffer`; browser worker integration remains a separate
consumer task. No COOP/COEP requirement is introduced by this profile.
Emscripten's [pthreads documentation](https://emscripten.org/docs/porting/pthreads.html)
explains why a threaded build would be a separate profile using shared memory.

Internal C++ exceptions use Emscripten's JavaScript exception implementation:
`-fexceptions` at compilation and `DISABLE_EXCEPTION_CATCHING=0` at linking.
The contract codec/validator intentionally throws and catches errors and
`std::bad_alloc`; disabling exceptions would break malformed-input handling.
This profile does not require the browser's native WASM exception proposal.
See [Emscripten exception support](https://emscripten.org/docs/porting/exceptions.html).
The future production C entrypoints must catch every exception and translate
it to the frozen status codes; exceptions may never cross that ABI.

Memory starts at 32 MiB, grows up to 256 MiB, and reserves a 1 MiB stack.
`ABORTING_MALLOC=0` permits libc allocation failure instead of unconditional
abort. These are finite smoke/runtime ceilings, not performance or maximum
geometry-size acceptance budgets. Geometry/coordinator checks must bound
working/output storage before allocation. The current contract's default
64 MiB document limit does not imply that all 64 MiB inputs fit this heap:
validation and canonical decimal encoding can allocate additional storage.
Heap-growth failure, stack overflow or worker termination can still invalidate
the instance. Worker disposal invalidates every handle for that instance.

Future wrappers copy/transfer outputs into JS-owned typed arrays. A borrowed
heap view cannot survive another allocation, memory growth, result release or
worker termination. Do not promise handle sharing between library instances,
in-call message-driven cancellation without shared memory, or identical native
and WASM geometry bytes. Cooperative cancellation within synchronous code and
worker termination are separate host capabilities.

## Portability audit and owning fixes

Audited source paths below are the existing authorities. Only contracts and
MathLib are linked by the contract smoke; the remaining rows are source audits,
not compiled algorithm probes. A successful contract compile cannot qualify a legacy
engine archive, full mesher or optional backend.

| Dependency / source | Filesystem and OS | Threads / SIMD / exceptions | Baseline disposition and owner |
| --- | --- | --- | --- |
| `libs/MatterContracts/src/{value,codec,validation,digest,services}.cpp`; public headers | No file/window/world dependency; JSON/resources arrive in owned buffers | C++17 containers, binary64 `from_chars`, internal exceptions; no thread creation or SIMD | Admitted and exercised. Binding owner `.6.2` must translate boundary exceptions and implement instance-local handles/releases. Contracts changes belong to the contract owner. |
| `libs/MathLib/include/matter_math*.h` | Header-only neutral math, no I/O | Scalar float math; ordinary native/WASM float differences require fixture tolerances | Admitted through the contract target. Geometry owns semantic transform adaptation; retain row-major/column-vector and mirrored-normal rules. |
| `MatterEngine3/src/polygon_triangulate.{cpp,hpp}` | Standard containers/math only | Scalar, allocation through STL | Candidate for geometry extrusion. Geometry owner must validate non-simple inputs, detect incomplete triangulation, bound vertices/work and return diagnostics: this legacy routine returns partial output silently. Compile from source; do not copy it. |
| `libs/MatterSurfaceLib/src/fat_primitive.c` and `include/fat_primitive.h` | C math plus neutral MathLib C values | Scalar, allocation-free | Candidate primitive SDF kernel. Geometry owner must validate shape discriminator/parameters: unknown `kind` currently falls back to a sphere. Reuse the kernel only behind checked portable inputs. |
| `libs/MemoryLib/src/mem_pool.c`, `libs/SpatialQueryLib/src/spatial_hash.c` | libc allocation and math; no file/OS calls | Thread-confined; pointers and `size_t` shrink on wasm32 | Candidate CPU scratch dependencies. Geometry owner must use checked size arithmetic, bound growth, propagate NULL/false, and destroy scratch on all exits. Spatial hash query truncation is not an acceptable silent loss of required geometry. |
| `libs/SpatialQueryLib/include/{tri,bvh,precomp}.h`, `src/bvh.cpp` | `precomp.h` includes native-oriented `<fstream>/<thread>`; aligned storage on POSIX/Windows | `bvh.h` unconditionally defines `USE_SSE`; public structures contain `__m128`, and the ray test calls `_mm_*` | Excluded from scalar baseline until geometry/query owner supplies a genuine scalar representation and selects existing scalar traversal explicitly. `-UUSE_SSE` alone cannot fix a header that redefines it. Do not enable WASM SIMD or fabricate x86 vector types to hide this dependency. Preserve size/alignment checks in the optional SIMD profile if added. |
| `libs/MatterSurfaceLib/src/surface.c`, `include/surface.h`, `src/mesh_worker_pool.cpp` | Raylib `Mesh`/allocation boundary; optional Win32/POSIX timing | Legacy global scratch; worker pool always spawns a thread even for count 1; 16-bit mesh indices and hard truncation caps | Full legacy mesher excluded. Geometry owner extracts a CPU call over owned portable buffers, uses explicit scratch without `MeshWorkerPool`, converts/checks uint32 output and returns budget errors. Keep timer services injected or disabled; no window/GPU shims. Terrain owns terrain-specific seam/field policies. |
| `MatterEngine3/src/csg_lowering.*`, `dsl_state.*`, `script_host.*` | Host/world/renderer types enter through authoring state | CSG semantics coupled to legacy state/backend | Excluded. Geometry owns checked ordered recipes/CPU algorithms; integration `.9.1` owns legacy adapters and shared entrypoints. No QuickJS or renderer port is needed to compile geometry. |
| `libs/MatterSurfaceLib/src/mesh_{indexed,simplifier,smooth,retopo}.cpp`, `third_party/autoremesher_core/` | Indexed mesh pulls SIMD `tri.h`; geogram has native facilities | TBB shim uses `<thread>`; Eigen may vectorize; pipeline uses exceptions | Optional processing excluded. Asset-pipeline owner must independently audit a serial/scalar subset and its transitive graph before declaring browser support. `RETOPO=0` or a warning/continue path must not silently satisfy a required operation. |
| `libs/MeshChartingLib`, `libs/ProfileLib`, `libs/AssetStoreLib` | Charting is an optional CPU processor; profiler owns capture/trace policy; storage owns persistent files/mappings | Separate lifetimes/resource policies | No baseline link. Asset-pipeline owns charting; coordinator/storage adapters own persistence; host owns profiling. Supply resolved resources through contracts rather than emulated filesystem caches. |
| Vulkan/raylib rendering, QuickJS, Flecs, Box3d, ozz, hydrology/GPU compute/GI | Native runtime/platform and domain dependencies | Independently audited optional capabilities | Excluded. Graphics, native host, simulation/animation and optional-processing owners retain native features. Unsupported required browser capabilities fail explicitly through the contract instead of receiving blanket platform stubs. |

No platform shim is currently required for the admitted contract subset.
Necessary future adaptations above are narrow algorithm/binding changes, not
implementations in this tooling task. The binding may reuse `matter_wasm_profile`
after choosing its production target/export list; the smoke's `callMain` export
is solely a fixture entrypoint.

## License inventory

This is a source/dependency inventory, not public-package authorization. The
checkout has no top-level LICENSE or local license files for the first-party
contracts/math/geometry libraries. Preserve existing attribution and have the
project owner settle distribution terms before packaging those sources as a
public SDK; no licence is inferred from a successful build. SpatialQueryLib's
`precomp.h` attributes the IGAD/NHTV/UU Jacco Bikker template but does not supply
a license grant in that header. SDF kernels reference Inigo Quilez formulas;
retain that provenance rather than inventing a license for them.

The excluded retopology dependency's local
[`UPSTREAM.md`](../../third_party/autoremesher_core/UPSTREAM.md) pins its sources:
autoremesher/isotropicremesher MIT, geogram BSD-3 core and Eigen MPL-2 core.
It specifically excludes GPL/LGPL Eigen integrations and GPL-tagged geogram
modules; preserve that selection and the individual vendored license notices
if a later optional build admits them. Its local TBB shim still has a thread
portability issue even though it removes the old shared-library dependency.
QuickJS and the graphics stack are not redistributed by this milestone.
Emscripten is dual-licensed MIT/NCSA; its LLVM/sysroot/toolchain components have their
own notices in the SDK. Installing the SDK does not add it to the module's
public runtime dependency graph.

## Smoke fixture map and evidence

| CTest target | Frozen gate fixtures / actual checks |
| --- | --- |
| `wasm_contract_values` | C1 primitive/extrusion/ordered CSG recipe validation, assemblies, materials, mirrored/nonuniform transforms, malformed/version/capability rejection, exhaustion, canonical round trips and SHA/RNG |
| `wasm_contract_services` | C2 resolved lengths/indices, immutable ownership, cancellation, budgets, stale admission, unsupported capabilities, terrain/graphics service contracts using fake providers |
| `wasm_contract_c_ownership` | `GATE-C-ABI`: C11 version/size structures, stable statuses, cross-instance/stale handles, release/double release, failure/cancellation, query/process and disposal using the delivered fake provider |
| `wasm_contract_vectors` | Exact WASM/JS equality for delivered numeric/SHA known answers, four frozen RNG seeds, canonical recipe/shared-asset bytes and digests |

Seven public headers compile alone in C++17; `abi.h` also compiles as C11 in
the ownership target. Fixture sources are compiled from `libs/MatterContracts/tests/`
without copies or edits. Node supplies the binary bytes and captures output;
the compiled library has filesystem support disabled. The fake providers
exercise contracts, not extracted geometry, rendering, production handle leak
acceptance, browser rendering or performance.

The [2026-10-10 smoke record](wasm/evidence/2026-10-10-smoke.json) preserves the
clean private-cache run, exact commands, compiler revision, fixture outputs and
artifact hashes. All four CTest cases passed: 208 contract checks, 155 service
checks, 7,011 fake-provider C ABI checks, and 267 exact numeric/SHA vectors plus
frozen RNG and recipe/asset digests. Seven C++ headers compiled independently.
All four negative compile probes rejected pthreads, SIMD, fast math and disabled
exceptions. The native standalone scaffold also configured and built.

Additional implementation runs and outcomes are attached in task
`clear-crest-48.6.1` close evidence. Reproduction generates fresh local evidence
with exact artifact hashes; later binding/browser tasks must record their own
native/WASM geometry tolerances, instance disposal/leak checks, browser fixture
acceptance and named-environment performance budgets.
