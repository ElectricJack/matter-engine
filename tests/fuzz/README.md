# Binding and portable-host fuzz properties

The default component builds register three new property gates:

| CTest | Inputs and independent properties |
| --- | --- |
| `matter_geometry_c_properties` | 72 fixed-seed cases: raw and mutated evaluate/query/process wire, generated positive box dimensions versus mesh extents, repeat output equality, full uint64 job echo, independent asset validation, ABI headers, budgets, cancellation, handle state machines, failure diagnostics and recovery |
| `matter_host_quickjs_properties` | 30 fixed-seed cases: raw/mutated modules, unsupported return values, generated programs, repeat canonical output, uint64 seeds, missing/cyclic dependencies, source digest mismatch, bounded loops, memory exhaustion, pre/progress cancellation and recovery |
| `matter_geometry_wasm_properties` | 24 generated boxes plus 32 raw transport cases: copied geometry extents, exact job epochs, repeat equality, foreign/released results, malformed JSON/Unicode, budget values, cancellation, live-result exhaustion/recovery and request views beyond linear memory |

Both native generators additionally exercise the empty input. The C ABI gate
also runs as `matter_geometry_c_properties` in Emscripten.
Each new CTest has a five-second timeout and the `property` label. Tests use
in-memory inputs and build-time embedded fixtures; no runtime fixture file reads,
network, engine, disk assets or graphics are needed. Failures print their seed
and zero-based iteration before execution. The generators use fixed unsigned
xorshift32 steps rather than implementation-dependent standard distributions.
The existing component example/parity/allocation tests remain separate.

## Linux sanitizer gate

Configure and build each component once, then select its property label:

```sh
cmake -S bindings/geometry-c -B /tmp/matter-binding-properties -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build /tmp/matter-binding-properties --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/matter-binding-properties -L property --output-on-failure -j 1
```

Use `-S MatterEngine3/src/script/portable_host` and a different build directory
for the host. Both libraries and tests receive sanitizer instrumentation.
For WASM, activate the pinned SDK in `bindings/geometry-c/wasm/toolchain.json`,
configure with `emcmake cmake -S bindings/geometry-c -B /tmp/matter-wasm-properties
-G Ninja -DCMAKE_BUILD_TYPE=Release`, build, then run the same CTest label.
No sanitizer claim is made for WASM.

## Longer opt-in runs

`-DMATTER_BUILD_FUZZERS=ON` adds `matter_geometry_c_fuzz` or
`matter_host_quickjs_fuzz` to its component build. These native executables
are deliberately absent from CTest. They default to 10,000 cases; override
`--iterations N --seed N` to run/reproduce a longer deterministic sequence:

```sh
/tmp/matter-binding-properties/matter_geometry_c_fuzz --seed 0x43414249 --iterations 10000
```

Both use exactly the fast gate's `LLVMFuzzerTestOneInput` implementation. A
structured input mode keeps valid geometry/program/control paths reachable;
other modes mutate raw bytes or known-valid requests/modules. Native lengths
always describe owned accessible storage; tests do not pass arbitrary native
pointers. Input, module count, result count, work, memory and deadlines are
bounded. Deadlines are cooperative; use an outer process limit for extended
experiments with untrusted source, as with the portable host itself.

For coverage-guided libFuzzer, use Clang, add
`-DMATTER_BUILD_FUZZERS=ON -DMATTER_FUZZ_LIBFUZZER=ON`, and set **both** global
C/C++ flags to `-fsanitize=fuzzer-no-link,address,undefined
-fno-omit-frame-pointer`. This instruments the libraries and VM as well as the
drivers; the driver adds the libFuzzer main only at link time. Example:

```sh
cmake -S bindings/geometry-c -B /tmp/matter-binding-libfuzzer -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DMATTER_BUILD_FUZZERS=ON -DMATTER_FUZZ_LIBFUZZER=ON -DMATTER_GEOMETRY_C_TESTS=OFF \
  -DCMAKE_C_FLAGS='-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer'
cmake --build /tmp/matter-binding-libfuzzer --target matter_geometry_c_fuzz --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/matter-binding-libfuzzer/matter_geometry_c_fuzz -max_len=2048 -max_total_time=300 -timeout=5
```

Use the host source path, `-DBUILD_TESTING=OFF` and host fuzz target for its
independent libFuzzer build. A failure artifact can be replayed by passing its
filename to libFuzzer. Optional corpus/artifact files belong in a temporary
directory, outside the repository. No engine implementation is copied into a
harness and there is no extra QuickJS build variant.
