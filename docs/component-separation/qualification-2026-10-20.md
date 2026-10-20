# Native and browser vertical-slice qualification, 2026-10-20

This is the report for task `clear-crest-48.9.2`. It covers the source tree at
`39ac31942fae0df433387b6a800df8d6c51aa94a`, which integrates
`clear-crest-48.9.1` (`94988b9abe095761a402183de7d1c87e587a7bc5`), plus this
task's harness fix and documentation changes.

It records what was run on one Linux host, against the frozen corpus in
`tests/component-separation/fixtures`, and what was measured. No numeric
threshold for the provisional profiles has been agreed
([validation.md](validation.md)). Every timing, size and memory figure below
is therefore an observation, not a pass/fail result. Where a check was not
possible on this host, the report says so instead of counting it as a pass.

Raw evidence, the measuring scripts and their outputs are in
[qualification-2026-10-20/](qualification-2026-10-20/). In those files,
`$OUT` stands for a build directory outside the repository.

## Verdict for the declared initial profile

**Passed**

- **Recipe parity.** The `A-*` cases passed: canonical recipe bytes and seeded
  vectors were identical across native QuickJS, Node and Chromium.
- **Geometry parity.** The `G-*` cases passed. Every compared native/WASM wire
  was byte-identical, with zero comparisons needing tolerance. Three declared
  oracle divergences are reported as such.
- **Runtime ownership, cancellation and disposal in the worker runtime.** The
  `R-*` cases passed, as did the `WEB-*` runtime fixtures and the viewer's
  `E2E-*` fixtures.
- **Native matched view against the legacy renderer.** This passed for the
  three unmirrored crate instances.

**Not proven**

- **Native vs browser pixels.** There is no pixel comparison between the native
  renderer and the browser viewer (see Gaps).
- **Mirrored crate.** In the native matched view, the mirrored crate differs
  from the legacy renderer. This is attributed to a legacy winding defect, not
  to a passing match.
- **Profiles not run here.** `terrain-boundaries` and `render-mountain` were
  not measured.

## Environment

| Item | Value |
| --- | --- |
| Host | WSL2, Linux 6.18.33.2-microsoft-standard-WSL2 x86_64, glibc 2.39, 1 CPU (`nproc` 1) |
| Native compiler | c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, Ninja, CMake Release |
| WASM | emcc 5.0.2 (`dc80f645ee70178c11666de0c3860d9e064d50e4`), profile `matter-wasm32-worker-v1` |
| Node / browser | Node v24.15.0; Chromium 150.0.7871.46 (`/opt/google/chrome/chrome`) via Playwright on `/usr/bin/python3.12` (3.12.3) |
| Vulkan (matched view) | llvmpipe (LLVM 20.1.2), Mesa 25.2.8, `MatterEditor/build/linux/editor` built 2026-10-20 05:46 from the same source |

The canonical Windows/MSVC wrappers were not rerun for this task. Every
native figure here comes from Linux GCC builds.

## Builds

```sh
cmake -S tests/component-separation/parity -B $OUT/parity-rel -G Ninja && cmake --build $OUT/parity-rel
cmake -S MatterEngine3/src/script/portable_host -B $OUT/portable-host -G Ninja -DBUILD_TESTING=ON
cmake --build $OUT/portable-host --target matter_host_quickjs_tests
# Library-only WASM: separate .mjs (42,633 B) + .wasm (918,492 B)
cmake -S bindings/geometry-c -B $OUT/wasm-lib -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake \
  -DBUILD_TESTING=OFF -DMATTER_GEOMETRY_C_TESTS=OFF
cmake --build $OUT/wasm-lib --target matter_geometry_wasm
# Testing WASM (SINGLE_FILE .mjs, 1,073,221 B) for the runtime CTest
python3.12 bindings/geometry-c/wasm/build-smoke.py --emsdk ~/emsdk --build-dir $OUT/wasm \
  --native-consumer $OUT/geometry-c/matter_geometry_c_tests
tsc -p integrations/web-procedural/runtime/tsconfig.json --outDir $OUT/runtime-dist
cmake -S integrations/web-procedural/runtime -B $OUT/web-runtime -G Ninja \
  -DMATTER_WEB_WASM_MODULE=$OUT/wasm/matter_geometry_wasm.mjs -DMATTER_WEB_TEST_PYTHON=/usr/bin/python3.12
cmake -S integrations/web-procedural/viewer -B $OUT/web-viewer -G Ninja \
  -DMATTER_WEB_WASM_MODULE=$OUT/wasm-lib/matter_geometry_wasm.mjs -DMATTER_WEB_TEST_PYTHON=/usr/bin/python3.12
cmake -S MatterEngine3/src/component_adapters/authoring -B $OUT/authoring -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
```

**Defects fixed in this task.** Both were defects in the harness and its
documentation; neither tolerance nor baseline was changed.

1. **WASM build recipe.** `wasm/Module.cmake` links the module SINGLE_FILE
   whenever `BUILD_TESTING` or `MATTER_GEOMETRY_C_TESTS` is on, and that
   includes `build-smoke.py`. The parity harness and the viewer's demo bundle
   both need the separate `.wasm`, so the documented recipes stopped:
   - `run_parity.py` exited with code 3;
   - the viewer configure failed with `Demo bundle input missing: …/matter_geometry_wasm.wasm`.

   [parity/README.md](../../tests/component-separation/parity/README.md) and
   [viewer/README.md](../../integrations/web-procedural/viewer/README.md) now
   give the library-only build.
2. **Error-line parsing for `A-UNKNOWN-CAPABILITY`.** `host_tests.cpp` prints
   `FAIL <code>: <message>`, but `run_parity.py`'s `native_author()` parsed the
   last stderr line without the `FAIL ` prefix. It therefore reported
   `host_failure` instead of the diagnostic. The parser now strips the prefix
   (3 changed lines).

## Recipe and geometry parity

Command, exit 0:

```sh
/usr/bin/python3.12 tests/component-separation/parity/run_parity.py \
  --native-runner $OUT/parity-rel/matter_parity_native \
  --native-host $OUT/portable-host/matter_host_quickjs_tests \
  --wasm-module $OUT/wasm-lib/matter_geometry_wasm.mjs \
  --runtime-dir $OUT/runtime-dist --out $OUT/parity-out
```

The run printed `RESULT PASS: 36 pass, 3 divergent, 0 fail, 3 unsupported`.

- **Baseline.** The pinned baseline matched its SHA-256
  `fda18136…58060ac2`.
- **Self-test.** `baseline_selftest.py $OUT/parity-out` passed all 11 of its
  tamper checks.
- **Tolerance profile.** `analytic-mesh-v1`, with position tolerance 1e-6 m.
- **Full report.** [parity-report.json](qualification-2026-10-20/parity-report.json).

| Case | Verdict | Fixture IDs | Native/WASM wires identical / within tolerance |
| --- | --- | --- | --- |
| G-PRIMITIVE | PASS | CORPUS-PRIMITIVE, PROP-STONE | 2 / 0 |
| G-EXTRUDE | DIVERGENT (declared) | CORPUS-EXTRUDE, GEOM-EXTRUDE | 2 / 0 |
| G-ORDERED-CSG | DIVERGENT (declared) | CORPUS-ORDERED_CSG, CORPUS-CSG-REVERSED, GEOM-CSG | 4 / 0 |
| G-MATERIAL | PASS | CORPUS-MATERIAL-VIEWS, PROP-SLAB | 1 / 0 |
| G-MIRRORED-PLACEMENT | PASS | CORPUS-AFFINE, GEOM-TRANSFORM | 1 / 0 |
| G-SHARED-INSTANCES | PASS | BUILD-SHARED, CORPUS-REPEATED-ASSEMBLY | 2 / 0 |
| G-SEEDED-VARIATION | PASS | CORPUS-SEEDED-VARIATION, GEOM-SEED, PROP-FLOWER | 3 / 0 |
| G-MALFORMED | PASS | ERROR-MISSING-REFERENCE, -CYCLE, -UNKNOWN-REQUIRED, -UNSUPPORTED-VERSION, -NEGATIVE-SIZE, -CSG-NONUNIFORM, -DUPLICATE-KEY, -NONFINITE | status codes only |
| G-RESOURCE-EXHAUSTION | PASS | JOB-BUDGET-* (8), EVAL-BOUNDED | 1 / 0 |
| G-CANCELLATION | DIVERGENT (declared) | JOB-CANCEL-PRE, JOB-CANCEL-PROGRESS, BUILD-CANCEL, EVAL-BOUNDED | 3 / 0 |
| G-STAGED-QUERY, G-RESULT-LIFETIME, G-REPEAT-ALLOCATION | PASS | CORPUS-PRIMITIVE | 2 / 3 / 2 identical |
| G-LATE-INVALIDATION | PASS | JOB-LATE-REQUEST, -REVISION, -EPOCH, TERRAIN-LATE | 1 / 0 |
| U-TERRAIN-ADJACENT-CHUNKS | UNSUPPORTED | TERRAIN-ADJACENT, CORPUS-TERRAIN-BOUNDARY, RENDER-MOUNTAIN | — |
| U-TERRAIN-RESOLUTION-CHANGES | UNSUPPORTED | TERRAIN-FLAT, TERRAIN-CONTOUR, CORPUS-TERRAIN-BOUNDARY | — |
| U-ASSET-INPUT | UNSUPPORTED | ERROR-INVALID-INDEX, ERROR-SINGULAR-TRANSFORM, CORPUS-REPEATED-ASSEMBLY | — |

**Authoring** (all PASS): A-PRIMITIVE, A-EXTRUDE, A-ORDERED-CSG,
A-REVERSED-CSG, A-MATERIAL, A-MIRROR, A-SHARED, A-ASSEMBLY, and
A-UNKNOWN-CAPABILITY. The seeded cases also passed for seeds `0000000000000000`,
`0000000000000001`, `0123456789abcdef` and `ffffffffffffffff`.

**Runtime in Chromium** (all PASS): R-CORPUS, R-STAGED, R-CANCEL-BEFORE,
R-CANCEL-RUNNING, R-DEADLINE, R-RESTART, R-STALE, R-OWNERSHIP, R-DISPOSE and
R-AUTHOR-ERRORS.

All eight mandatory cases pass: primitive, repeated_child_assembly,
material_assignment, mirrored_nonuniform_placement,
deterministic_seeded_variation, malformed_recipe, resource_exhaustion and
terrain_late_invalidation.

Divergences are against the analytic oracles. The native and WASM results are
byte-identical to each other in every case.

- **Extruded prop.** The produced (u, v) is the oracle's (−u, −v), and the
  bounds are [−1,−1,0]..[0,0,2].
- **Ordered CSG and cancellation.** The volume is 6.8853 m³ against the
  analytic 7 m³. This comes from the 0.1 m sampled CSG in `csg.cpp:37`.

Unsupported means the browser has no terrain producer and no imported-asset
input yet. These cases are reported as unsupported, not as passes.

## Browser runtime and viewer

`ctest --test-dir $OUT/web-runtime` passed `matter_web_browser` in 2.60 s.
Every runtime fixture passed:

- WEB-PROP, WEB-ASSEMBLY, WEB-STAGED, WEB-REPEAT-OWNERSHIP;
- WEB-CANCEL-RESTART, WEB-RESTART, WEB-DEADLINE, WEB-BOUNDS-QUEUED-CANCEL;
- WEB-REVISION-EPOCH, WEB-ERROR-RELEASE-CAPS, WEB-RESPONSIVENESS, WEB-DISPOSE;
- WEB-REALM-RECYCLE, WEB-LATE-MESSAGE-ADMISSION, WEB-INPUT-SNAPSHOT-LIMITS.

The run used 2 workers. Its full evidence is
[web-runtime-evidence.json](qualification-2026-10-20/web-runtime-evidence.json).

| Fixture | Worker job time | Bytes transferred to page | WASM heap |
| --- | --- | --- | --- |
| WEB-PROP | 80.5 ms | 3,780 | 33,554,432 B |
| WEB-ASSEMBLY | 25.7 ms | 5,661 | 33,554,432 B |
| WEB-STAGED (3 stages) | 28.1 ms | 4,054 | 33,554,432 B |
| WEB-REPEAT (24 iterations) | — | — | 33,554,432 B first and last (constant) |

**UI responsiveness (WEB-RESPONSIVENESS).** A warmed 350 ms worker CPU probe
ran for 359.6 ms. Over that time, a 5 ms page timer ticked 71 times, and its
largest gap was 5.2 ms. Page JS heap was 2,422,619 B, which excludes worker
WASM heaps. Heap figures are capacity, not allocator peak.

**Download.** The runtime downloads 1,158,475 B raw, or 355,519 B with per-file
gzip (hypothetical). This counts the production modules plus the WASM actually
served, with the SINGLE_FILE module accounting for 1,073,221 / 328,251 B.
The library-only split build is a 42,633 B `.mjs` plus a 918,492 B `.wasm`.

**Copies.** These are counted from code, not instrumented. Geometry produces
one result wire in the WASM heap. `geometry.mjs` copies it out once
(`HEAPU8.slice`). It then decodes the typed mesh arrays once from that wire's
JSON. `worker.mts` transfers the wire and mesh buffers to the page as
transferables, which moves them without copying, and releases every native
handle first. The viewer (`adapter.mts:484-496`) wraps those transferred
arrays directly in `BufferAttribute`s. The exception is mirrored tangents,
which are flipped into a new array (`adapter.mts:516`). three.js then uploads
them to WebGL.

**Viewer.** `ctest --test-dir $OUT/web-viewer` passed 4 of 4:

| Test | Time | Fixtures / evidence |
| --- | --- | --- |
| `matter_web_viewer_types` | 1.28 s | type checks |
| `matter_web_viewer_browser` | 34.20 s | COLOR-SRGB, TEX-ORIENT, UVSET1, WINDING, COORDS, SHEAR, TANGENT-MIRROR, DISPOSE (20 iterations), ERRORS, E2E-PROP, E2E-CSG, E2E-ASSEMBLY, E2E-REPEAT, E2E-SUPERSEDE, E2E-ERROR-KEEPS, E2E-ERROR-FIXTURES, E2E-CONTRACT, E2E-DISPOSED ([viewer-evidence.json](qualification-2026-10-20/viewer-evidence.json)) |
| `matter_web_demo_smoke` | 1.96 s | first useful frame: WEB-VIEW-PROP, 44 triangles, coverage 0.1746, 1 worker, 1.766 s from page start, no errors ([demo-smoke.json](qualification-2026-10-20/demo-smoke.json)) |
| `matter_web_demo_bundle` | 15.89 s | split `.wasm` served; [demo-bundle-evidence.json](qualification-2026-10-20/demo-bundle-evidence.json) |

| Demo-bundle fixture | Generation (worker runtime) | Triangles drawn / uploaded | Notes |
| --- | --- | --- | --- |
| WEB-VIEW-PROP | 62.4 ms (60.7 ms) | 44 / 44 | |
| WEB-VIEW-CSG | 1077.5 ms (1070.5 ms) | 2712 / 2712 | |
| WEB-VIEW-ASSEMBLY | 301.4 ms (298.0 ms) | 2064 / 492 | 16 instances in 6 draw calls (2 instanced, 1 mirrored, 3 individual) |
| WEB-VIEW-CONTRACT | 8.3 ms | — | fallback diagnostics for collision and LOD; optional extension ignored; attachment anchor |

## Native matched view

This compares Matter's portable scene route
(`MatterEngine3/src/component_adapters/graphics/portable_scene.cpp`) against
the legacy renderer's debug views. Both run in the same editor process, on the
same world, camera and 480x270 size: `PortableCrateFixture` on lavapipe, with RT
forced unavailable and volumetrics off.

```sh
docs/component-separation/qualification-2026-10-20/tools/run_matched_view.sh $OUT/mv1
/usr/bin/python3.12 docs/component-separation/qualification-2026-10-20/tools/matched_compare.py $OUT/mv1
```

- **Run.** Bake publish took 90,958 ms and logged `bake finished (0 errors)`.
  The route produced 4 portable draws at revision 1.
- **Coverage.** Both renderers covered 8,645 pixels, with 0 portable-only and
  0 legacy-only pixels.
- **Portable albedo** over the crates was exactly the legacy palette
  (209, 199, 184).
- **Tolerances.** Channel tolerance 2, at most 0.5% of pixels over it. The
  output is in [matched-view.txt](qualification-2026-10-20/matched-view.txt) and
  [matched-view-portable.json](qualification-2026-10-20/matched-view-portable.json).

| Instance | Pixels | Normal max | Depth max | Albedo max | Result |
| --- | --- | --- | --- | --- | --- |
| 39082e5e2f24ae48/part.1 | 1599 | 1 | 1 | 0 | PASS |
| 44366aa842c611c9/part.1 | 2039 | 1 | 1 | 0 | PASS |
| 970d0641c9a8b10e/part.1 | 2615 | 1 | 1 | 0 | PASS |
| 8db58308f1cbfb47/part.1 (mirrored) | 2392 | 165 | 115 | 0 | **not matched**: legacy inside-out |

**The mirrored crate.** Coverage and albedo match, but normals and depth do
not. Legacy depth is farther than portable depth at 1,971 of 2,392 pixels,
which means legacy is drawing back faces. The legacy rasterizer uses a fixed
`frontFace` (`vk_scene_renderer.cpp:2769`) and has no per-instance winding
parity. The portable route and the browser viewer's WINDING and
TANGENT-MIRROR fixtures handle det<0.

So this row is evidence of a legacy defect, not a passing match. The
comparator's overall `RESULT PASS` excludes it by that explicit rule. The run
reproduces `.9.1`'s `runH3`/`runH4`.

## Benchmark profiles

The measuring scripts are
[tools/profile_bench.py](qualification-2026-10-20/tools/profile_bench.py) and
[tools/measure_native.py](qualification-2026-10-20/tools/measure_native.py).
Their outputs are
[profile-bench.json](qualification-2026-10-20/profile-bench.json) and
[process-rss.json](qualification-2026-10-20/process-rss.json).

**Protocol.** Each case and backend gets one fresh process. It runs create,
then 1 cold evaluate, then 5 warmup and 30 measured evaluate/release pairs.
Both builds are Release. The native backend is `matter_parity_native`, the
C ABI. The node-wasm backend is the production adapter in Node 24 through
`tools/node_wasm_stream.mjs`.

The time measured is the stdin→stdout round trip of the evaluate step, which
includes reading the result wire. The step protocol cannot separate
authoring, validation, geometry and processing, as validation.md asks. It
does not count allocations or cache hits either.

| Profile | Fixture (case) | Wire bytes | Native: create / cold / median / p95 / max, ms | Node WASM: create / cold / median / p95 / max, ms |
| --- | --- | --- | --- | --- |
| props-small | PROP-SLAB (G-MATERIAL) | 4,367 | 1.78 / 9.73 / 9.15 / 9.57 / 9.66 | 37.89 / 32.60 / 14.81 / 16.66 / 18.50 |
| props-small | PROP-FLOWER (G-SEEDED-VARIATION) | 3,505 | 1.53 / 5.26 / 5.07 / 5.27 / 5.43 | 40.23 / 27.76 / 8.67 / 9.56 / 9.61 |
| props-small | GEOM-EXTRUDE (G-EXTRUDE) | 2,298 | 1.74 / 3.88 / 3.61 / 3.81 / 3.88 | 39.54 / 25.14 / 6.55 / 7.50 / 8.47 |
| solid-source-csg | GEOM-CSG (G-ORDERED-CSG) | 1,147,021 | 1.48 / 1590.1 / 1552.6 / 1596.9 / 1603.2 | 34.9 / 2347.6 / 2312.7 / 2427.1 / 2507.7 |
| assembly-shared | BUILD-SHARED (G-SHARED-INSTANCES) | 3,094 | 1.50 / 5.65 / 5.49 / 5.75 / 5.77 | 34.72 / 31.40 / 9.32 / 10.45 / 11.10 |
| (reference) | CORPUS-PRIMITIVE (G-PRIMITIVE) | 2,324 | 1.54 / 4.91 / 4.72 / 4.87 / 4.97 | 45.42 / 27.24 / 7.60 / 8.00 / 8.14 |

**Whole case files.** The next table gives process wall time over 3 runs,
including startup and every step in the case file, with peak RSS from
`/usr/bin/time -f %M`. Node RSS includes the Node runtime. Output bytes differ
slightly because the native and WASM step lists differ.

| Case | Native: cold / warm median ms / peak RSS MiB | Node WASM: cold / warm median ms / peak RSS MiB | Output bytes native / WASM |
| --- | --- | --- | --- |
| G-CANCELLATION | 1546.5 / 1584.5 / 22.8 | 2395.6 / 2431.1 / 98.1 | 1150391 / 1150195 |
| G-EXTRUDE | 9.0 / 9.2 / 4.9 | 82.7 / 77.9 / 65.4 | 4783 / 4761 |
| G-LATE-INVALIDATION | 13.8 / 13.3 / 4.9 | 88.6 / 86.6 / 64.9 | 2976 / 2513 |
| G-MALFORMED | 11.7 / 10.6 / 5.4 | 86.1 / 85.3 / 68.1 | 2789 / 2823 |
| G-MATERIAL | 10.5 / 10.3 / 5.1 | 69.2 / 79.1 / 64.9 | 4467 / 4456 |
| G-MIRRORED-PLACEMENT | 7.2 / 6.5 / 4.9 | 68.1 / 65.2 / 64.8 | 2693 / 2682 |
| G-ORDERED-CSG | 3002.7 / 3113.1 / 24.6 | 4590.4 / 4693.4 / 104.4 | 2296503 / 2296459 |
| G-PRIMITIVE | 11.3 / 11.4 / 4.9 | 77.4 / 71.1 / 64.8 | 4835 / 4813 |
| G-REPEAT-ALLOCATION | 1564.8 / 1538.5 / 4.9 | 2546.3 / 2607.6 / 89.8 | 5155 / 5207 |
| G-RESOURCE-EXHAUSTION | 1552.1 / 1583.3 / 22.3 | 2419.5 / 2382.6 / 93.1 | 3597 / 3491 |
| G-RESULT-LIFETIME | 16.0 / 16.6 / 5.1 | 94.7 / 94.3 / 65.3 | 7636 / 7840 |
| G-SEEDED-VARIATION | 16.8 / 16.8 / 4.9 | 86.5 / 80.6 / 65.1 | 10789 / 10756 |
| G-SHARED-INSTANCES | 13.3 / 13.4 / 5.1 | 86.4 / 90.4 / 67.3 | 6556 / 6537 |
| G-STAGED-QUERY | 15.8 / 15.5 / 5.3 | 112.9 / 112.8 / 67.9 | 1040 / 1010 |

**Profile parts not measured**

- `solid-source-csg`:
  - PROP-STONE at 0.006 m detail;
  - seeds 0–7 of the 23-operation source;
  - a separate cancellation-latency measurement. Cancellation behaviour itself
    is covered by R-CANCEL-RUNNING and G-CANCELLATION.
- `assembly-shared`:
  - BUILD-CASTLE;
  - warm or edited DAG invalidation scope.
- `terrain-boundaries`: no portable terrain producer exists (the U-TERRAIN-*
  cases).
- `render-mountain`: this needs the MSVC/GPU recipe and a portable terrain
  route.

## Supported capabilities and native-only cases

The browser viewer's `CAPABILITY_MATRIX` is defined in
`integrations/web-procedural/viewer/src/capabilities.mts`. An unknown required
capability fails explicitly, as A-UNKNOWN-CAPABILITY, ERROR-UNKNOWN-REQUIRED and
E2E-CONTRACT show.

| Status | Capabilities |
| --- | --- |
| Supported | `matter.asset.core`, `matter.material.pbr`, `matter.recipe.core`, `matter.asset.attachments` |
| Fallback (diagnosed) | `matter.asset.lod` (base prototypes only), `matter.asset.collision` (not rendered), `matter.asset.animation` (static bind pose) |
| Unsupported | `matter.asset.terrain` |

The following remain native-only, per `NATIVE_ONLY_FEATURES` in the same file:

- RT lighting and shadows;
- VT/chart atlases;
- POM;
- LOD rungs and imposters;
- the Vulkan compute cull;
- terrain sector streaming;
- retopology and other native processing;
- transparency and advanced material extensions.

## Gaps and open defects

- **No native-vs-browser pixel comparison.** The browser viewer is checked
  against analytic probes: colour, orientation, UV set, winding, coordinates,
  shear and tangent mirroring. The native route is checked against the legacy
  renderer. The two use different lighting models, and no shared matched view
  exists. Recipe and geometry parity between them is byte-exact; pixel parity
  is not shown.
- **The native graphics authoring fixture is not run.** MatterGraphics cannot
  be configured on this Linux host because there are no Vulkan headers or
  `glslc`, so `authoring_e2e`'s graphics case is not registered. The other 8
  authoring e2e tests passed:

  | Test | Time |
  | --- | --- |
  | authored | 5.40 s |
  | geometry | 3.02 s |
  | material | 0.03 s |
  | mirror | 0.02 s |
  | shared | 0.02 s |
  | seeded | 0.07 s |
  | errors | 0.02 s |
  | dedup | 0.02 s |

  The editor matched view above used an existing Linux editor build instead.
- **No MSVC rerun.** The canonical Windows editor and tests were not rebuilt
  for this report.
- **`.9.1` defects, found by reading the code and not exercised here.**
  - *Warm-cache parts are dropped.* `portable_scene.cpp:145-146` skips parts
    with no product (`// Legacy content.`). Parts whose warm world-resolve
    cache bypassed `LegacyPartRoute::resolve` are therefore omitted silently.
  - *Late products are never retried.* `update()` republishes only on
    `instances_changed || pending`.
  - *Unsupported material ids render white.* `legacy_part_route.cpp:101`
    falls back to `PALETTE[+k] ?? {}`, so unsupported or dynamic material ids
    render factor-1 white instead of failing.

  The crate fixture passes cold, but none of these paths is covered.
- **Mirrored legacy instances render inside-out**, as described in the
  matched-view section.
