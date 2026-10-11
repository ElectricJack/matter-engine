# Native/browser parity and result-lifetime harness

`run_parity.py` checks that the production geometry C ABI, its WASM build
and the browser worker runtime produce the same results from the frozen
component-separation corpus (`tests/component-separation/fixtures`). It also
checks that the three authoring hosts produce byte-identical canonical
recipes and seeded vectors, and it stresses cancellation, restart, stale
results and result ownership. The harness never writes inside the corpus.

| Backend / host | What runs |
| --- | --- |
| `native` | `matter_parity_native` (`native_runner.cpp`): the C ABI compiled from `bindings/geometry-c` |
| `node` | the production WASM adapter under Node (`node_wasm.mjs` → `wasm_steps.mjs`) |
| `chromium` | the same adapter, the portable authoring SDK and `integrations/web-procedural/runtime` in headless Chromium (`browser/`) |
| native authoring | the portable QuickJS host `matter_host_quickjs_tests --stdin` |

Cases live in `cases.json`. Each case names its fixture IDs, its mandatory
case from `manifest.json`, and its `comparison-policy.json` profile. The
report prints that profile's per-class tolerances (position, normal, volume
and uv) beside every case.

## What is compared

- **Authoring** (`A-*`): the canonical recipe bytes and seeded vectors must
  be identical across native QuickJS, Node and Chromium, and must match the
  frozen canonical fixtures and `rng-splitmix64.json`.
- **Geometry** (`G-*`): native and WASM step outcomes must agree exactly.
  The only exceptions are numbers the case's profile governs (positions,
  bounds, normals, tangents, uv), which must fall within its tolerance.
  Node and Chromium must agree line for line. Each outcome is also judged
  against the fixture's expected status code, or against its analytic oracle
  through `verify.compare_geometry`.
- **Runtime** (`R-*`) observations:
  - the corpus through the worker, under the native envelope, byte for byte;
  - a staged query;
  - abort before and during a job;
  - a deadline and a restart;
  - stale envelopes;
  - 50 repeated results with a constant WASM heap, transfer detachment, and
    a kept result that survives `dispose()`;
  - author errors.
- **Baseline**: every observation is compared with the pinned `baseline.json`.

## Verdicts and exit codes

| Verdict | Meaning |
| --- | --- |
| `PASS` | every check held |
| `FAIL` | any mismatch, including a declared divergence that no longer reproduces |
| `DIVERGENT` | the only mismatches are declared `oracle_divergence` entries: the exact first mismatch a frozen oracle produces against production, plus its reason |
| `UNSUPPORTED` | the case needs a profile no backend exposes; it is listed with the reason and never counted as a pass |

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | PASS (DIVERGENT and UNSUPPORTED cases are listed separately) |
| 1 | FAIL, including a success that requires a capability no backend advertises |
| 2 | configuration error in `cases.json` or the corpus, such as an unknown check, key or status |
| 3 | a required tool is unavailable |

### Current declared divergences

The frozen oracles disagree with production in two places. Both backends
agree with each other byte for byte in both.

- `G-EXTRUDE`, step `frozen`: production `profile.extrude` maps profile
  `(u, v)` to `(-u, -v)` (`libs/ProceduralGeometry/src/geometry.cpp`), so its
  bounds are `[-1,-1,0]..[0,0,2]`. The oracle's are `[0,0,0]..[1,1,2]`.
- `G-ORDERED-CSG` (step `ordered`) and `G-CANCELLATION` (step `after`):
  production CSG samples a 0.1 m field (`csg.cpp`) and gives 6.8853 m³, where
  the exact boolean gives 7 m³. The bounds and closed-manifold checks that run
  before the volume check still pass.

### Unsupported profiles

- `U-TERRAIN-ADJACENT-CHUNKS` and `U-TERRAIN-RESOLUTION-CHANGES`: neither
  side exposes a terrain producer.
- `U-ASSET-INPUT`: the binding evaluates recipes only, so asset-kind
  fixtures are refused as an "unknown binding field".

## Building the inputs

Run everything from the repo root. Run builds and the harness one at a
time; the suites are memory-heavy (see CLAUDE.md). `$OUT` is any directory
outside the repo.

```bash
# Native step runner. Release is required and is the default: the frozen job
# budget allows 5 s, and an unoptimised build can exceed it on ordered CSG.
cmake -S tests/component-separation/parity -B $OUT/parity-rel -G Ninja
cmake --build $OUT/parity-rel

# Portable QuickJS authoring host
cmake -S MatterEngine3/src/script/portable_host -B $OUT/portable-host -G Ninja -DBUILD_TESTING=ON
cmake --build $OUT/portable-host --target matter_host_quickjs_tests

# WASM module (matter_geometry_wasm.mjs + .wasm), via the binding's own smoke build
cmake -S bindings/geometry-c -B $OUT/geometry-c -G Ninja && cmake --build $OUT/geometry-c
/usr/bin/python3.12 bindings/geometry-c/wasm/build-smoke.py --emsdk ~/emsdk \
    --build-dir $OUT/wasm --native-consumer $OUT/geometry-c/matter_geometry_c_tests

# Browser runtime
tsc -p integrations/web-procedural/runtime/tsconfig.json --outDir $OUT/runtime-dist
```

You also need Node 22.7 or newer, Chromium at `/opt/google/chrome/chrome`
(or pass `--browser`), and Playwright for `/usr/bin/python3.12`.

## Running

```bash
/usr/bin/python3.12 tests/component-separation/parity/run_parity.py \
    --native-runner $OUT/parity-rel/matter_parity_native \
    --native-host $OUT/portable-host/matter_host_quickjs_tests \
    --wasm-module $OUT/wasm/matter_geometry_wasm.mjs \
    --runtime-dir $OUT/runtime-dist \
    --out $OUT/parity-out
```

`--out` receives:

- `report.json`, with per-case verdicts, problems, tolerances, maximum
  deviations, native/WASM wire counts (identical bytes vs within tolerance),
  geometry reports, divergences and tool fingerprints;
- `plan.json` and `raw/`;
- `candidate-baseline.json`.

The binding's default execution budget is 5 s of wall-clock time, so on a
loaded host an equivalence step could turn into `resource_limit`. To keep
outcomes independent of host load, every geometry call and runtime job carries
an explicit 120 s `EXECUTION_CEILING`. No fixture targets execution time, and
the budget fixtures still set their own limits.

## Adopting a new baseline

There is no update mode, so a regression can't be accepted by rerunning the
harness.

`baseline.json` stores:

- the digest of every input it was pinned against;
- every observation;
- each geometry wire as its exact text, keyed by its sha256.

The whole file is pinned by `BASELINE_SHA256` in `run_parity.py`. The run
fails if any of these happen:

- the file no longer hashes to the pin;
- a stored wire no longer hashes to its key;
- an input fixture changed;
- any exact observation changed.

Wire numbers governed by the profile may move only within tolerance, and that
movement is reported as deviation. Everything else in a wire is compared
exactly, through its metadata digest.

To adopt an intended change:

1. Review `$OUT/parity-out/candidate-baseline.json` against `baseline.json`.
2. Copy it over `baseline.json`.
3. Set `BASELINE_SHA256` to its new sha256 in the same reviewed change.

`baseline_selftest.py $OUT/parity-out` proves these guards hold. It judges
tampered copies of the baseline and candidate in a temporary directory:

- **Accepted:** the pinned pair, and a 4e-7 m drift inside tolerance.
- **Rejected:** an overwritten baseline without a re-pin, a removed pin, a
  1e-3 m drift, a renamed material, a dropped triangle, a malformed recipe
  that now succeeds, a flipped ownership observation, a changed input, and an
  edited stored wire whose file was re-pinned.
