# Native and browser vertical-slice qualification, 2026-10-20

This is the report for task `clear-crest-48.9.2`. It covers the source tree at
`39ac31942fae0df433387b6a800df8d6c51aa94a`, which integrates
`clear-crest-48.9.1` (`94988b9abe095761a402183de7d1c87e587a7bc5`), plus this
task's harness fix and documentation changes.

**Update for `clear-crest-48.9.8`.** This revision is based on `.9.2`'s
integrated head `528d32f1`. It fixes the three `.9.1` defects listed under Gaps
and the mirrored-crate mismatch, and adds a native/browser pixel-matched view.
It also runs the named profile cases that `.9.2` left unmeasured. The editor,
graphics e2e and named-case results in this revision come from that tree.
Earlier sections that this task did not rerun still describe `.9.2`'s tree:
parity, the browser runtime and viewer, and the benchmark tables.

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
- **Native matched view against the legacy renderer.** All four crate
  instances passed, the mirrored one included, both from a cold cache and from a
  warm WORLD resolve cache (`.9.8`).
- **Native vs browser pixels.** The four graphics authoring generators passed
  per pixel between native VulkanRaster and the browser viewer's AssetView
  (`.9.8`, [native/browser matched view](#native-browser-matched-view)).
- **Named corpus cases.** Every named `solid-source-csg`, `assembly-shared` and
  `terrain-boundaries` case ran and passed natively. See
  [Named measurements and corpus cases](#named-measurements-and-corpus-cases).

**Not proven**

- **Portable terrain.** The browser has no terrain producer. `terrain-boundaries`
  is measured on the native terrain suites only, and the `U-TERRAIN-*` parity
  cases stay unsupported.
- **`render-mountain` performance.** The Windows MSVC/GPU recipe cannot run on
  this host. A Linux/lavapipe functional run is recorded instead, and its
  numbers are not comparable to the Windows evidence.

## Environment

| Item | Value |
| --- | --- |
| Host | WSL2, Linux 6.18.33.2-microsoft-standard-WSL2 x86_64, glibc 2.39, 1 CPU (`nproc` 1) |
| Native compiler | c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, Ninja, CMake Release |
| WASM | emcc 5.0.2 (`dc80f645ee70178c11666de0c3860d9e064d50e4`), profile `matter-wasm32-worker-v1` |
| Node / browser | Node v24.15.0; Chromium 150.0.7871.46 (`/opt/google/chrome/chrome`) via Playwright on `/usr/bin/python3.12` (3.12.3) |
| Vulkan (matched view) | llvmpipe (LLVM 20.1.2), Mesa 25.2.8, `MatterEditor/build/linux/editor` built 2026-10-20 09:44 from the `.9.8` tree |

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

- **Run.** `.9.2`'s run took 90,958 ms to publish. The cold `.9.8` run logged
  `bake-timing … publish=170109ms` and `resolve census: 2 parts baked, 2 cache
  hits`, then `bake finished (0 errors)`. The route produced 4 portable draws at
  revision 1. Both runs used 1 CPU, so publish times vary with host load.
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
| 8db58308f1cbfb47/part.1 (mirrored) | 2392 | 1 | 1 | 0 | PASS |

The comparator prints `RESULT PASS (channel-tol 2, max-diff-pct 0.5)`. It no
longer has a rule exempting mirrored instances; `.9.2` used one to set the
mirrored crate aside as "legacy inside-out".

### The mirrored crate (`.9.8`)

**Before the fix.** The `.9.8` tree without the fix reproduced `.9.2`'s
result. The mirrored crate's normal max was 165, with 100% of pixels over
tolerance. Its depth max was 115, with 99.04% over. Legacy depth was farther
than portable depth at 1,971 of 2,392 pixels, so legacy was drawing back
faces.

**Root cause.** `.9.2` cited `vk_scene_renderer.cpp:2769`, but that is the
composite pipeline. The face being culled is set elsewhere:

- The legacy G-buffer raster pipeline (`mesh_pipelines.cpp:149`) culls back
  faces with `frontFace` counter-clockwise.
- `cull.comp` packed every instance of a cluster/LOD bucket into one indirect
  command, whatever the sign of its transform's determinant.
- A det<0 placement reverses screen-space winding, so the pipeline culled its
  front faces and drew its back faces.

A diagnostic run with culling disabled matched all four crates, which confirms
the cause. That run was not a fix, and the change was reverted.

**Fix.** No tolerance changed. Mirrored instances get a second command table,
drawn with a clockwise-front pipeline, rather than per-instance pipeline
switches:

- **Mirrored flag.** `GpuInstance.mirrored` is set from
  `gpu_mat4_reverses_winding` (`gpu_matrix_pack.h:47`, applied in
  `vk_scene_renderer.cpp`). It occupies the former `water_pad1` padding word.
- **Second command table.** `cull.comp` routes mirrored instances to a mirrored
  command table at `commands[capacities.z + bucket]` (`cull.comp:557`). That
  table is reserved by `reserve_mirror_slot` from the tail of the bucket's
  transform region, and is gated on `vis_params.z` so it costs nothing when no
  mirrored instance exists.
- **Clockwise-front pipeline.** MatterGraphics builds a `raster_mirrored`
  pipeline with `frontFace` clockwise (`mesh_pipelines.cpp:221`).
  `MeshRasterRecord` draws the mirrored table with it.
- **Occlusion.** Mirrored instances are excluded from the occlusion ID pass
  (`cull.comp:469`), because that pass draws counter-clockwise-front.
- **Hierarchy pages.** These stay in the primary table.
- **Test.** `matrix_tests.cpp` gains a determinant-sign test, which
  `make -C MatterEngine3/tests run-matrix` runs.

### Warm WORLD resolve cache (`.9.8`)

`run_matched_view.sh` was rerun against a copy of the cold run's cache. The
run logged:

- `resolve cache: hit 7ebbe4bb08420776`;
- `resolve census: 0 parts baked, 2 cache hits`;
- `bake finished (0 errors)`;
- `portable scene captured 4 draws at revision 1`.

`portable.json` lists the asset as ready with 0 refused draws. The identity and
depth buffers are byte-identical to the cold run's. Every instance passed with
the same maxima as the cold run
([matched-view-warm.txt](qualification-2026-10-20/matched-view-warm.txt)).

## Warm-cache, late-product and material defects (`.9.8`)

The three `.9.1` defects that `.9.2` found by reading the code are fixed and
now have tests.

**Warm WORLD resolve cache.** The cause was in
`LocalProvider::restore_from_cache`. It rebuilt the world from the resolve
cache without passing the parts back through `LegacyPartRoute::resolve`. As a
result, the portable scene saw parts with no retained product and skipped them
silently.

- **Restore.** The restore now calls `part_graph::reauthor_portable`
  (`part_graph.cpp:681`, called at `local_provider.cpp:5024` under
  `provider.restore-portable`). It re-authors every PortableV1 node from its
  pinned source and parameters, so the retained products exist again.
- **Hash check.** If a re-authored hash differs from the cached hash, the cache
  is refused rather than trusted.
- **Missing products.** `portable_scene.cpp:153` no longer skips a resolved part
  that has no product. It refuses the draw with `no product: <reason>` taken
  from `retained_refusal`.

**Late products.** `update()` used to republish only on `instances_changed ||
pending`. It now also republishes when `retained_generation()` has advanced
past the generation it last published (`portable_scene.cpp:228-230`).

**Dynamic materials.** `classify_legacy_material` sorts a material id into one
of three kinds: builtin palette, per-world dynamic, or unknown
(`legacy_part_route.cpp:345`). `legacy_material_refusal` then refuses any
non-builtin material (`legacy_part_route.cpp:356`), and the portable scene
refuses the draw (`portable_scene.cpp:119`). Such a part no longer falls back
to factor-1 white.

`make -C MatterEngine3/tests run-authoring-route` passed. It printed
`authoring_route_tests --warm: PASS`,
`dynamic refusal: material 'matter.legacy.material.31' is per-world dynamic id
31 with no immutable factors`, and `authoring_route_tests: all tests passed`.
The tests are in `authoring_route_tests.cpp`:

- `test_material_ids` covers the builtin, dynamic and unknown kinds.
- `test_warm_restore` starts with a cold resolve, which must advance
  `retained_generation()`. It then runs an in-process restore, which must reuse
  the product without a new signal.
- `test_warm_restore` then re-runs the binary with `--warm`. In that fresh
  process, a stale pinned source must refuse the cache. The restore must then
  rehydrate a product with the cold content digest and advance the generation
  that the portable scene watches. Last, the warm part cache must be a pure
  hit.

**PortableScene on a live device.** `portable_scene_tests.cpp` runs a real
`PortableScene` on a live Vulkan device and reads results back from the
adapter's own VulkanRaster capture (`MATTER_PORTABLE_SCENE_CAPTURE`). Each
step's draws and per-instance pixel counts come from that capture. Run it with
`make -C MatterEditor run-test-portable-scene`. On this Linux host it also
needs the `VULKAN_INCLUDE`, `VULKAN_LIB_DIR`, `GLSLC` and `BOX3D_LIB`
overrides, because the committed `libbox3d.a` is a MinGW archive.

The test covers these cases:

- **Cold.** Two instances share one part, `Shared`. A third part, `Dynamic`,
  uses a per-world dynamic material. After admission there are 2 draws, A and
  B at 1591 px each, from one shared asset. `Dynamic` is refused with the
  material reason, logged exactly once, and never drawn.
- **Fresh process and warm restore.** A child process cold-bakes `Late` into
  its own cache root. This process then restores that root through
  `part_graph::reauthor_portable`, which advances `retained_generation()`.
- **Late product under unchanged instances.** `Late`'s instance was placed
  before its product existed, so it was neither drawn nor refused. After the
  restore, with `instances_changed=false`, the scene republishes 3 draws, and
  `Late` covers 1554 px.
- **Shared-instance removal.** Removing B leaves 2 draws, and the shared asset
  is kept. Removing A as well leaves 1 draw (`Late`), and the shared asset is
  retired. `Dynamic` stays refused.

The test printed `ALL PASS`. This host has no Khronos validation layer, so the
device ran unvalidated. The test said so: `validation layer unavailable:
running without validation` and `validation errors: 0 (unvalidated)`. When the
layer is present, the test enables it and fails on any validation error.

## Native/browser matched view

This view compares native VulkanRaster with the browser viewer's AssetView,
pixel for pixel, on the four graphics authoring generators. Both sides use the
same orthographic camera, the same seed (`0123456789abcdef`) and a 128x128
output. Evidence: [browser-matched.txt](qualification-2026-10-20/browser-matched.txt).

**Registering the graphics e2e.** The graphics authoring e2e is now registered
on this host. MatterGraphics is configured against the vendored
`third_party/Vulkan-Headers`, the system `libvulkan.so.1` and a `glslc`:

```sh
cmake -S MatterEngine3/src/component_adapters/authoring -B $OUT/authoring-gfx -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DMATTER_GRAPHICS_VULKAN=ON \
  -DVulkan_INCLUDE_DIR=$PWD/third_party/Vulkan-Headers/include \
  -DVulkan_LIBRARY=<libvulkan.so> -DMATTER_GRAPHICS_GLSLC=<glslc>
cmake --build $OUT/authoring-gfx
DISPLAY=:0 ctest --test-dir $OUT/authoring-gfx -R authoring_e2e
```

All 9 of 9 tests passed, in 32.95 s:

| Test | Time |
| --- | --- |
| authored | 4.89 s |
| geometry | 2.65 s |
| material | 0.02 s |
| mirror | 0.02 s |
| shared | 0.02 s |
| seeded | 0.06 s |
| errors | 0.02 s |
| dedup | 0.02 s |
| graphics | 25.25 s |

**Camera fix.** The e2e's reversed-depth projection row was (0, 0, −0.1, 0.9).
With that row, the nearest surface was not the front face. It is now
(0, 0, 0.1, 0.8) (`authoring_e2e_tests.cpp:290`), so depth testing keeps the
front face.

**Running the comparison.**

1. **Native dump.** `MATTER_AUTHORING_E2E_DUMP=<dir>` makes the graphics case
   write each generator's albedo, world normal, reversed depth and instance
   identity to `<dir>/<generator>.native.json`.
2. **Browser render.** [tools/browser_matched.py](qualification-2026-10-20/tools/browser_matched.py)
   runs the unmodified parity generators through the worker runtime and the
   viewer's `createAssetView` in Chromium (SwiftShader WebGL2). It replaces only
   the shading, writing the same four buffers from the viewer's own geometry,
   instance matrices and material colours.
3. **Comparison.** The pixel rules are:
   - instance coverage must match on at least 99.5% of covered pixels;
   - on pixels both renderers cover with the same instance, albedo and encoded
     normal must be within 2/255, and depth within 1e-4;
   - each rule may fail on no more than 0.5% of an instance's pixels.

```sh
/usr/bin/python3.12 docs/component-separation/qualification-2026-10-20/tools/browser_matched.py \
  --wasm-module $OUT/wasm-lib/matter_geometry_wasm.mjs --native <dir> --scale 17
```

| Generator | Instances | Coverage mismatch | Albedo max | Normal max | Depth max | Result |
| --- | --- | --- | --- | --- | --- | --- |
| primitive | body (1591 px) | 0 | 0.00098 | 0 | 2.4e-10 | PASS |
| materialAssignment | clay_part, metal_placed (378 px each) | 0 | 0.00196 | 0 | 1.6e-10 | PASS |
| orderedCsg | cut (1580 px) | 0 | 0.00098 | 0.000677 | 9.4e-6 | PASS |
| sharedInstances | row.0, row.1, row.2 (90/90/54 px) | 0 | 0.00098 | 0 | 0 | PASS |

**Why `--scale 17`.** The browser renders at 17x and reads only each output
pixel's centre sample. The sample positions stay the same, but vertex
precision becomes 1/(16·17) px. This removes a rasterizer-precision
difference without changing the tolerance:

- SwiftShader reports `SUBPIXEL_BITS` 4, so it snaps vertices to 1/16 px;
- native lavapipe snaps them to 1/256 px (`subPixelPrecisionBits` 8).

**At `--scale 1`** (the plain render), three generators still pass, but
orderedCsg fails. Its normal max is 0.0153 (2.2% of pixels over tolerance) and
its depth max is 1.92e-4 (4.4%), all on grazing CSG facets.

**Attribution.** [tools/subpixel_snap_check.py](qualification-2026-10-20/tools/subpixel_snap_check.py)
ray-casts the exact depth at every pixel centre from the triangles and
compares both renderers to it:

- Native is within 8.3e-8 of exact depth.
- The scale-1 browser render is 1.92e-4 off on 221 pixels.
- Snapping the vertices to 1/16 px reproduces the browser render to within
  4.2e-8.

So the scale-1 difference is SwiftShader's vertex snap, not the viewer's
geometry, transforms or materials.

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

## Named measurements and corpus cases

`.9.2` listed a number of profile parts as not measured. In `.9.8`, every one
of those named cases was run on this host: native GCC builds of
`MatterEngine3/tests` and the Node world-script suites. All of them exited 0.
Raw output, the commands and the build times are in
[named-cases.txt](qualification-2026-10-20/named-cases.txt).

These are single-process runs of each case's existing suite. They are not the
5-warmup/30-sample protocol in [validation.md](validation.md), so the times are
observations only. Wall time and peak RSS come from `/usr/bin/time`.

| Profile | Fixture | Suite | Result | Wall | Peak RSS |
| --- | --- | --- | --- | --- | --- |
| solid-source-csg | PROP-STONE (current 0.006 m detail) | `obj_export_golden_tests` with `-DMATTER_CODEC_SOURCE_QUALIFICATION` (real bakes of CastleStone, CastlePavingSlab, AlpineFlower, `World.exports`) | ALL PASS. CastleStone's pinned digest: 45,506 vertices, 54,054 triangles, 223 charts, hash `730a3217c9a13581`, equal to PROP-STONE in `legacy-assets.json` | 76.72 s (CPU shared with a build) | 856 MiB |
| solid-source-csg | seeds 0–7 of the 23-operation source, EVAL-BOUNDED | `solid_source_evaluation_tests` | ALL PASS. 8 distinct hashes; eval 1.90–2.86 ms per seed; 0 artifact writes and 0 GPU calls | 0.05 s | 8.8 MiB |
| solid-source-csg | (default digest-only run) | `obj_export_golden_tests` | ALL PASS | 0.00 s | 4.9 MiB |
| assembly-shared | BUILD-CASTLE | `castle_plan_tests.mjs` (Node) | 40 walls, 9 modules, all checks passed | 0.15 s | 77 MiB |
| assembly-shared | BUILD-CASTLE (native flatten oracle) | `part_flatten_tests` | ALL PASS | 29.71 s | 191 MiB |
| assembly-shared | BUILD-SHARED: cold, warm and edited DAG | `part_graph_tests` | All passed (invalidation counts below) | 0.00 s | 4.5 MiB |
| terrain-boundaries | TERRAIN-FLAT | `terrain_field_tests` | ALL PASS | 0.00 s | 4.4 MiB |
| terrain-boundaries | TERRAIN-FLAT, TERRAIN-ADJACENT | `terrain_mesher_tests` | ALL PASS; determinism over 11,808 triangles and 1,353 boundary vertices, 0 differing record bytes | 3.77 s | 10.2 MiB |
| terrain-boundaries | TERRAIN-ADJACENT | `seam_weld_tests` | ALL PASS; deterministic over 180 triangles (12,960 bytes, twice) | 0.02 s | 3.8 MiB |
| terrain-boundaries | TERRAIN-CONTOUR | `contour_seam_tests` | ALL PASS | 0.05 s | 5.8 MiB |
| render-mountain | RENDER-MOUNTAIN: rock site and terrain-only sectors | `mountain_geometry_site_tests.mjs`, `mountain_terrain_only_tests.mjs` (Node) | Rock site: 3 placements / 3 dense assets (stress profile 1,283 / 19). Terrain-only sectors: rule checks passed | 0.03 s each | 56 / 48 MiB |
| render-mountain | RENDER-MOUNTAIN: editor at 1920x1080, static w45 recipe | `MatterEditor/build/linux/editor` (lavapipe) | Bake finished with 0 errors. FATAL before the perf window: surface prototype > 128 MiB `maxStorageBufferRange`. No perf.json. See [render-mountain on this host](#render-mountain-on-this-host) | 1278 s (exit 1) | 13.6 GiB |

**Invalidation scope (`part_graph_tests`).** The suite's assertions pin the
cache behaviour:

| Step | Parts baked | Cache hits |
| --- | --- | --- |
| First install | 3 | 0 |
| Second install, unchanged | 0 | 3 |
| Edit one leaf | 2 (the leaf and its ancestor root) | 1 (the unrelated branch) |

The suite also checks two more cases:

- **Identical parameters.** A child placed with identical parameters collapses
  to one artifact: 4 bakes. Differing parameters give two artifacts: 3 bakes.
- **Cycles.** A cycle fails the install, names the modules involved and bakes
  nothing.

**Not measured as validation.md specifies:**

- cancellation latency for `solid-source-csg`;
- the separate late-result coordinator test for `terrain-boundaries`.

Cancellation behaviour is covered by R-CANCEL-RUNNING, G-CANCELLATION and
G-LATE-INVALIDATION. The portable `U-TERRAIN-*` cases stay unsupported because
the browser has no terrain producer.

### render-mountain on this host

This run follows the static w45 recipe on Linux lavapipe. The recipe is
`docs/agent/evidence/2026-09-30-static-default-candidate/capture.py` plus
`tools/streammountain_attribution.sh pom_off`. It used the same environment
and FIFO commands at 1920x1080, with `MATTER_PERF_WARMUP_SECONDS=45` and
`MATTER_PERF_SAMPLE_SECONDS=20`. Lavapipe has no ray tracing, so the run added
`MATTER_DISABLE_VK_RT=1`, `MATTER_VK_TEST_FORCE_RT_UNAVAILABLE=1` and
`MATTER_VOLUMETRICS=0`. Evidence:
[render-mountain-lavapipe.txt](qualification-2026-10-20/render-mountain-lavapipe.txt),
the log with repeated per-frame lines counted rather than kept.

**Result: functional evidence only, and the run did not reach the perf
window.**

- **Bake.** `bake finished (0 errors)` was reached with the FIFO
  `wait_event bake.finished`. The three GPU texture-detail tilesets then took
  38–74 s each on the CPU rasterizer. `[bake-timing]` reported
  `world=555096ms publish=584429ms total=1139591ms`.
- **Streaming.** Streaming started with 12 bake workers. The first impostor
  parts registered: about 700k vertices each, 16 clusters, 0.9–1.0 s per part.
- **No frames drawn before the fatal.** A `stats` probe during the tileset
  bakes showed a 3.9 s frame and 0 instances drawn. The perf harness only
  starts warming once static uploads are stable, so it never logged
  `warming for`. No `perf.json` or view PNG was produced.
- **Fatal.** After 1278 s the editor exited 1 with
  `FATAL: render: surface prototype exceeds GPU triangle buffer limit`. That
  check is `vk_sparse_voxel.cpp:360`, which compares the surface-proxy
  triangle buffer against `maxStorageBufferRange`. Lavapipe reports 134217728
  (128 MiB), and StreamMountain's surface prototypes exceed it. This branch
  does not change `vk_sparse_voxel.cpp`, so the fatal is this host's device
  limit, not a `.9.8` regression.
- **Anomalous counter.** With RT disabled, every frame logged an implausible
  `[vk] RT rejected <~1e9-4e9> part record(s)` count. That points to an
  unwritten readback being reported, and it is filed as a follow-up. It did not
  affect the fatal.

The profile's CPU/GPU percentiles therefore remain unmeasured. They need the
Windows MSVC recipe on a hardware GPU.

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

- **No MSVC rerun.** The canonical Windows editor and tests were not rebuilt
  for this report.
- **render-mountain performance is not measured.** validation.md's recipe
  needs the Windows MSVC editor on a hardware GPU, which this host does not
  have. The Linux lavapipe run in
  [render-mountain on this host](#render-mountain-on-this-host) is functional
  evidence only. It baked with 0 errors, then stopped on lavapipe's 128 MiB
  `maxStorageBufferRange` before the perf window, so it produced no frame times
  at all.
- **Portable terrain is still unsupported.** The browser has no terrain
  producer, so the `U-TERRAIN-*` parity cases stay unsupported. Terrain
  boundaries are proven on the native suites only.
- **Parity, the browser runtime and the viewer suites were not rerun for
  `.9.8`.** Their results above come from `.9.2`. None of the `.9.8` changes
  touches a source those builds compile:
  - `matter_parity_native` builds `native_runner.cpp` plus
    `bindings/geometry-c`;
  - the portable host, the WASM module, the runtime and the viewer do not
    reference `part_graph`, `local_provider`, the graphics adapter,
    `MatterGraphics` or the Vulkan scene renderer.

  The `.9.8` browser matched view did exercise the viewer's `AssetView` and
  the worker runtime in Chromium.
- **`run-vk-scene-renderer` has one failure that predates `.9.8`.** It scores
  119/120. The failing check is "RT carries the same two words…": at
  `vk_scene_renderer_tests.cpp:545` it reads
  `../shaders_vk/rt_surface_common.glsl`, but commit 189b8cfa moved that file
  to `libs/MatterGraphics/shaders/`. This is a stale test path, not a renderer
  regression. It is filed as follow-up work.
