# Procedural compatibility corpus

[manifest.json](manifest.json) expands the [C0 inventory](../../docs/component-separation/fixtures.json)
into 54 mapped samples covering all 13 mandatory contract cases. Each sample
names its inputs, sources, expected behavior and checks. Source blobs and
artifacts have SHA-256 provenance. Producer sources for this capture are pinned
to `406b72509dbcf4dd644b73a7f095792ce2512ba5`; C0 retains its original
`98af23851dd3424ddbfc62624cd2afb850e9d09d` provenance.

The supported Windows MSVC v143 RelWithDebInfo capture passes the new runner's
172 checks and three of five selected legacy suites. It retains two legacy
suite failures: the shader-source assertion at line 88, and 12 OBJ comparisons
against missing slab/flower `.obj` fixtures. Native export metadata, material
files and structural checks passed. The generated small OBJ/MTL/texture outputs
are separately retained in [fixtures/native-export](fixtures/native-export).
Neither the original goldens nor producer algorithms were changed. The existing
unclassified `MemoryLib/src/mem_bank.c` manifest failure is also retained.

## Inputs and expectations

| Case | Input / expected behavior |
| --- | --- |
| Primitive | Centred 2 m cube recipe; explicit closed cube, volume 8 m³ |
| Extrusion | Unit XY polygon extruded 2 m along +Z; closed solid, volume 2 m³ |
| Ordered subtraction | Outer cube minus centred smaller cube; cavity, volume 7 m³; reversing operands yields empty geometry |
| Repeated assembly | One shared triangle mesh and one shared assembly; distinct root/child IDs, four flattened instances; removing one root leaves two |
| Material views | Two triangles with distinct clay/metal ownership; 2×2 RGBA8 texture, sRGB RGB/linear alpha; fixed front/oblique cameras and render settings |
| Mirrored/nonuniform placement | Parent translation times scale `[-2,3,4]`; inverse-transpose normal, reversed winding and tangent sign |
| Seeded variation | Frozen native SplitMix64 and legacy script xoshiro128** draws; repeatable analytical prop dimensions; existing actual host geometry assertions remain mapped through C0 |
| Malformed input | Missing reference, cycle, unknown required capability, unsupported version, invalid primitive/index/transform, nonuniform CSG transform, duplicate JSON key, nonfinite number |
| Exhaustion | Decoder node/vertex/index/value limits and cooperative operation/vertex/index/output/working/query/depth budgets; zero query budget is invalid input |
| Cancellation | Before work and from a progress callback; no product published, prior owner preserved |
| Terrain neighbors | Native flat-field/rung/no-skirt and same-build noise assertions; analytical shared plane, explicit neighbor keys and large binary64 origin |
| Terrain refinement | Coarse/fine boundary samples and an explicit linear transition policy/refinement request |
| Late terrain completion | Request, source revision or generation epoch changes; stale admission preserves previous owner |

[comparison-policy.json](comparison-policy.json) was set before comparisons.
Positions/UVs and normal/material values retain the existing export tolerances
of `1e-5` and `1e-4`; legacy flat terrain height uses `1e-3` m. Analytical
mesh bounds, volume and affine normals use `1e-6`. Exact canonical bytes,
semantic IDs, material bindings, RNG draws and job envelopes have exact gates.
Geometry uses bounds, signed volume, closed opposite-edge topology, shell
count and cavity occupancy; vertex numbering may differ. Duplicate seam
positions are welded exactly; approximate welding needs its own producer
policy. The CLI comparator accepts one already validated mesh asset at a time.

`legacy-assets.json` explicitly namespaces the old resolved and texture-map
hash domains. Those labels describe the corpus's legacy values; portable
content identity uses the separate SHA-256 canonical profile.

The hand-authored cube, extruded prism and cavity meshes are mathematical
oracles. Codec roundtrips do not execute those recipes. The portable terrain
plane and material camera fixtures are expectations for future adapters;
production terrain transitions and rendered material pixels remain unqualified.
The existing terrain overlap-band behavior is recorded separately. No native
versus WASM, component-independence or performance pass is claimed here.

## Reproduce

Run from the repository root in WSL. Data and JS checks require Python 3 and
Node; `aq test` supplies the project's managed pytest invocation.

```sh
python3 docs/component-separation/verify.py
python3 tests/component-separation/verify.py
aq test tests/component-separation/test_oracles.py -q
node tests/component-separation/check-js.mjs
```

Build the real legacy sources with the supported wrapper before the local
runner. The local build uses the same Windows toolchain resolver, compiles the
original `part_graph.cpp` without its host adapter, and resolves its legacy
FNV dependencies through the supported headless/spatial/memory/profile
archives. Its use of those archives makes it a legacy comparison harness.

```sh
./tools/build-windows-from-wsl.sh RelWithDebInfo preflight
./tools/build-windows-from-wsl.sh RelWithDebInfo part_graph_tests
native_powershell=/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe
"$native_powershell" -NoProfile -ExecutionPolicy Bypass -File \
  "$(wslpath -w "$PWD/tests/component-separation/build-windows.ps1")"
"$native_powershell" -NoProfile -ExecutionPolicy Bypass -File \
  "$(wslpath -w "$PWD/tests/component-separation/run-legacy.ps1")"
```

`build-windows.ps1` must pass. `run-legacy.ps1` reports the retained failures
and returns CTest exit code 8 at the pinned source commit. It leaves
`MATTER_EXPORT_GOLDEN_UPDATE` unset and runs serially. Detailed results and
binary/toolchain/machine provenance are in [evidence/capture.json](evidence/capture.json).
Build output lives under ignored `build/`. Native capture diagnostic times
are unsuitable for product latency attribution.

After the legacy run, reproduce small exported assets into a **new** directory
and compare them with the checked-in snapshots using `legacy-export-v1`.
The capture script refuses an existing destination and never writes goldens.

```sh
python3 tests/component-separation/capture-exports.py --output /tmp/matter-corpus-recapture
python3 tests/component-separation/verify.py --exports /tmp/matter-corpus-recapture
python3 tests/component-separation/verify.py --compare ordered_csg \
  --asset tests/component-separation/fixtures/csg-oracle.asset.json
```

The comparator checks geometry/material ownership against the frozen
analytical profile. Integration runners must first validate the full asset
contract, including resource payloads, transforms and all required capabilities.
Malformed/unknown-required cases in `validation-cases.json` must fail with their
specified error codes and leave outputs unchanged.

The native export comparator uses the existing numeric OBJ/MTL tolerances and
exact face/material/resource identities. Texture bytes are exact same-build
bake answers; this comparison does not qualify a renderer pixel threshold.

## Visual and performance references

[evidence/historical-render.json](evidence/historical-render.json) pins the
existing four StreamMountain images, raw summaries and archives to capture
commit `b387c15d81ff4d5b26a195b14f1afd6793b6ad6b` and the original binary digest.
The archived logs identify an RTX 4090, NVIDIA driver 610.74 and Vulkan 1.4.341.
They record a 1920×1080 matched camera, scene seed 20260722, POM off, warmups
45/300 seconds and 20-second measurement intervals. Commands and GPU admission
are preserved with those artifacts. The historical campaign did not record
the CPU, OS build or machine identifier; those fields are explicitly unknown.
The current [machine snapshot](evidence/machine.json) applies to this task's
CPU captures and must not be backdated to the historical campaign.

Reuse the original camera/protocol and compatible source/binary/assets for a
new campaign. Preserve coverage differences and raw samples. No portable
pixel threshold, equal-coverage parity, browser/WASM timing, 16.7 ms target or
product memory budget is qualified. Material view fixtures specify matched
inputs for later rendering; the native exported texture maps are CPU bake
references.

## Maintaining the corpus

Validation never regenerates frozen answers. Keep C0 identities and known
failures when adding captures. New producer outputs need a source commit,
supported build/toolchain, exact commands, machine information and a stated
comparison profile. Store new captures separately and review changed oracles;
do not loosen tolerances to accept a regression. `manifest.json` indexes the
frozen data/evidence files; its own digest and code/document hashes belong in
the delivery commit. Producer source hashes are verified against the pinned
Git objects so future extractions do not silently replace the baseline.
