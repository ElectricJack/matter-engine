# Scripting test qualification

This separate CMake graph runs the portable QuickJS host, SDK authoring, native
C ABI allocation checks, embedded WASM consumers and browser worker. It also
registers each legacy engine scripting case when its existing consumer binary
is supplied. The product root CMake graph remains Windows only.

Build `MatterEngine3/tests/build/script_host_tests` through its existing Makefile
on Linux, or the product `script_host_tests` target on native Windows. Supply
platform-native raylib and Ozz archives to the Linux Makefile as usual. Build the
geometry-c Emscripten test graph with smoke tests enabled before configuring the
complete graph:

```sh
cmake -S tests/scripting -B build/scripting -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMATTER_SCRIPT_HOST_TEST_BINARY=/absolute/path/to/script_host_tests \
  -DMATTER_SCRIPTING_WASM_BUILD=/absolute/path/to/geometry-wasm-build \
  -DMATTER_WEB_WASM_MODULE=/absolute/path/to/geometry-wasm-build/matter_geometry_wasm.mjs \
  -DMATTER_WEB_TEST_PYTHON=/absolute/path/to/python-with-playwright
cmake --build build/scripting --parallel 2
ctest --test-dir build/scripting --output-on-failure -j 2 --output-junit build/scripting/results.xml
```

Node, TypeScript and Chrome must be available. For isolated installations, set
`MATTER_AUTHORING_TSC` and `MATTER_WEB_TSC`; `MATTER_WEB_BROWSER` selects the worker
browser. All optional graphs above were enabled for the 83-case qualification.
A graph without those options intentionally covers only its native components.

## Fixture and coverage contract

Script imports and the six actual SDK modules are embedded at build time.
The same owned imported bytes feed canonical source hashing and QuickJS module
evaluation; replacement invalidates cached folds, and missing snapshot imports
fail closed. Expected recipes/RNG words and the seven binding requests remain
independent literal data. QuickJS request batching creates a fresh host/context
for every request. Test WASM modules embed their executable bytes with
`SINGLE_FILE`; library builds with testing disabled retain their usual packaging.

Each legacy scripting case binds a private `MemoryCacheByteStorage` through the
shared bundle byte boundary. Production serializers, validators and reloads
still run: geometry, deterministic bytes, child placement, render policy,
flatten hints and volume-emitter trailers are asserted after loading. Cases
share neither artifact generations nor stderr capture files. Linux tracing of
all 66 scripting cases found only OS loader/shared-library file operations, with
no source, asset, cache or fixture reads. The manual component separation audit
retains its frozen-baseline/source-integrity checks; it is not registered here.

All 65 original scripting cases remain, plus the owned module snapshot case.
Only these three fixtures reduce work and coverage:

| Case | Particles before → after | Max age before → after | Ticks before → after |
| --- | --- | --- | --- |
| `test_pf_stamp_paths_positive` | 16 → 4 | 60 → 8 | 80 → 12 |
| `test_pf_stamp_paths_outside_session` | 16 → 4 | 60 → 8 | 80 → 12 |
| `test_pf_determinism_double_bake` | 32 → 4 | 60 → 8 | 120 → 12 |

These lose long-history and dense-overlap simulation coverage. They retain
actual stamped geometry, age expiry/reuse, rejection outside a voxel session,
and independently serialized double-bake byte equality. The separate 300-tick
pure-JS simulation, 200-point cloud, all QuickJS OOM cases and all 6,739 native
allocation failure positions are unchanged. Browser worker busy/cancel/repeat,
WASM/native geometry parity, heap ownership and uint64 guards are retained.

## Measurements

Measured on the same workstation, 2026-10-12. CTest used `-j 2` for each complete
graph. The original product scripting test took **118.15 s**; the final native
Windows product scripting test took **3.56 s** (**3.58 s** total CTest wall).
The complete 83-case graph passed in **17.17 s on Windows** and **15.56 s on
Linux**, with no skipped or disabled cases. Linux legacy compilation used the
canonical Makefile and its real CPU solid/face closure, without added link
objects or a supplemental Makefile.

No complete 83-case pre-change graph or successful original WASM timing was
captured. The pre-bounding instrumented legacy run (including the new snapshot
case) summed to 99.393218 s; that per-case sum is distinct from CTest wall time.
Its slowest 20 cases are listed separately from the final complete graphs.

The final full native product build is coordinated separately with core at
parallelism 1; these timings establish consumer compile/test qualification.

## Slowest 20 before bounding the legacy fixtures

| Test | Seconds |
| --- | ---: |
| `test_pf_determinism_double_bake` | 80.097296 |
| `test_pf_stamp_paths_positive` | 12.757754 |
| `test_p5_round_mesh_dispatch` | 0.849518 |
| `test_absolute_voxel_sampling` | 0.847159 |
| `test_g1_voxel_line_and_round_dispatch` | 0.680814 |
| `test_fresh_context_no_residue` | 0.619618 |
| `test_seeded_rng_and_no_ambient` | 0.481891 |
| `test_determinism_identical_bytes` | 0.341828 |
| `test_bake_result_child_modules_placed` | 0.285053 |
| `test_bindings_record_ops_and_misuse` | 0.279543 |
| `test_place_child_roundtrip` | 0.214151 |
| `test_g6_place_child_params` | 0.188787 |
| `test_bake_writes_part` | 0.162072 |
| `test_bake_writes_flatten_hints` | 0.147618 |
| `test_ray_tracing_policy_authoring_roundtrip` | 0.145244 |
| `test_emit_volume_hash_divergence` | 0.121556 |
| `test_modifier_region_bake_rules` | 0.119920 |
| `test_pf_bindings_smoke` | 0.087962 |
| `test_bake_seed_invariance_per_lod_params` | 0.085901 |
| `test_resolve_hash_matches_and_skips_build` | 0.081696 |

## Slowest 20 after — native Windows

| Test | Seconds |
| --- | ---: |
| `matter_geometry_c_allocation` | 15.248800 |
| `matter_web_browser` | 3.768600 |
| `matter_geometry_wasm_wrapper` | 2.501740 |
| `matter_geometry_c_consumer_wasm` | 1.271770 |
| `matter_authoring_browser` | 1.052870 |
| `script_host_test_pf_determinism_double_bake` | 0.971498 |
| `matter_authoring_types` | 0.899786 |
| `matter_host_quickjs_controls` | 0.837895 |
| `matter_geometry_c_consumer` | 0.545535 |
| `script_host_test_owned_module_source_snapshot` | 0.453960 |
| `script_host_test_pf_stamp_paths_positive` | 0.449808 |
| `wasm_contract_values` | 0.442776 |
| `script_host_test_fresh_context_no_residue` | 0.402866 |
| `matter_authoring_sdk` | 0.395031 |
| `matter_host_quickjs_compiler_oom` | 0.380674 |
| `script_host_test_absolute_voxel_sampling` | 0.379715 |
| `wasm_contract_services` | 0.369708 |
| `matter_host_quickjs_parity` | 0.367159 |
| `wasm_contract_vectors` | 0.345544 |
| `script_host_test_g1_voxel_line_and_round_dispatch` | 0.337233 |

## Slowest 20 after — Linux

| Test | Seconds |
| --- | ---: |
| `matter_geometry_c_allocation` | 12.756000 |
| `matter_web_browser` | 4.576560 |
| `matter_geometry_wasm_wrapper` | 2.310220 |
| `matter_geometry_c_consumer_wasm` | 1.663210 |
| `matter_authoring_browser` | 0.854642 |
| `matter_authoring_types` | 0.635780 |
| `matter_host_quickjs_controls` | 0.587709 |
| `script_host_test_pf_determinism_double_bake` | 0.559489 |
| `script_host_test_fresh_context_no_residue` | 0.533109 |
| `matter_geometry_c_consumer` | 0.464022 |
| `script_host_test_g1_voxel_line_and_round_dispatch` | 0.426151 |
| `wasm_contract_vectors` | 0.392679 |
| `matter_host_quickjs_parity` | 0.386264 |
| `script_host_test_absolute_voxel_sampling` | 0.360673 |
| `wasm_contract_values` | 0.340805 |
| `matter_authoring_sdk` | 0.328531 |
| `script_host_test_determinism_identical_bytes` | 0.298013 |
| `wasm_contract_services` | 0.293958 |
| `wasm_contract_c_ownership` | 0.268898 |
| `matter_host_quickjs_compiler_oom` | 0.252140 |

