# Browser asset viewer and procedural demo

An independent three.js adapter for portable Matter asset documents
([contract v1](../../../libs/MatterContracts/README.md)) plus a parameter/seed
demo driven by the [browser worker runtime](../runtime/README.md). The renderer
dependency stays in this directory. The runtime, the authoring SDK and the
geometry WASM binding import neither three.js nor this module. The demo talks to
the runtime through a structural `DemoRuntime` interface
(`src/demo/session.mts`), so neither side imports the other's sources. This
module owns no live world, ScriptHost/DslState wiring, native renderer or
central build manifest.

three.js 0.186.1 is vendored unmodified in `vendor/three/`. `MANIFEST.json`
records the npm tarball, its integrity hash and the SHA-256 of every copied
file. There is no package install and no npm dependency. `src/three.d.ts`
declares only the three.js surface this module uses. Pages map the bare
`three` specifier to the vendored build with an import map.

## API

Build with `tsc -p integrations/web-procedural/viewer/tsconfig.json`, then
import `dist/index.mjs`.

```ts
import {createAssetView, ViewerError} from './dist/index.mjs';
const result = await runtime.submit(job);           // {value:{kind:'asset',asset,resources}, meshes}
try {
  const view = createAssetView(result, {fallbacks: 'label'});
  scene.add(view.root);
  view.diagnostics;   // fallback / ignored / derived notes; never silent
  view.attachments;   // Map of socket id -> non-rendering Object3D anchor
  view.stats;         // prototypes, geometries, materials, textures, batches, triangles
  // ...later, or before showing a newer result:
  view.dispose();     // idempotent; releases every geometry, material and texture it created
} catch (error) {
  if (error instanceof ViewerError) console.log(error.code, error.diagnostics);
}
```

`createAssetView` accepts a runtime/binding result or a bare contract asset
document. Wire-form buffers and resource bytes are accepted too. Typed
`meshes` are used without copying when present. GPU uploads happen only when
the view is first rendered. A failure inside `createAssetView` releases every
three.js object it created and raises a `ViewerError` with the contract code
and every diagnostic seen so far. Its codes are
`invalid_input`, `unsupported_capability`, `unsupported_version`,
`missing_dependency`, `cycle`, `resource_limit` and `disposed`.

| Option | Default | Meaning |
| --- | --- | --- |
| `fallbacks` | `'label'` | `'label'` draws fallback products and reports them; `'reject'` fails with `unsupported_capability` |
| `maxInstances` | 100,000 | flattened mesh placements before `resource_limit` |
| `maxDepth` | 32 | assembly nesting depth before `resource_limit` |
| `maxTextureSize` | 4096 | texture width/height before `resource_limit` |
| `resolveResource` | none | supplies bytes for resources the result did not carry |

## Contract mapping

| Contract v1 | three.js |
| --- | --- |
| Metres, right-handed, +Y up | Same convention; no axis conversion |
| 16 row-major numbers on column vectors, parent * local | `Matrix4.set` takes the same order; world = parent * local |
| CCW front faces | `FrontSide`; a negative determinant makes three.js flip to CW, so mirrored placements stay front-facing |
| Prototype mesh | One shared `BufferGeometry` per prototype, whatever its instance count |
| Two or more orthogonal (rotation/translation/scale, no shear) placements of a mesh | One `InstancedMesh` |
| Two or more mirrored orthogonal placements | One `InstancedMesh` whose object matrix is `MIRROR_X`, so its instances share CW winding |
| Sheared or single placements | Individual `Mesh` objects sharing the prototype geometry |
| Submeshes | Geometry groups with one material each |
| `uv_sets[n]` with an explicit texture UV set | Attribute `uv`/`uv1`/`uv2`/`uv3` and `texture.channel = n` (one cloned texture per extra channel; texels shared) |
| Tangents (xyz, w = ±1) | Uploaded only for normal-mapped materials; mirrored placements use a w-flipped variant because three.js forms the bitangent as cross(N, T) * w |
| Opaque metallic/roughness PBR | `MeshStandardMaterial`: base color, packed G = roughness / B = metallic, tangent-space normal, emissive |
| Texture rows (row 0 at v = 0) | `DataTexture`, `flipY = false` |
| `srgb` / `linear` color space | `SRGBColorSpace` / `NoColorSpace` |
| Wrap and filters | repeat/clamp/mirror; nearest/linear with consumer-derived mipmaps |
| Attachments | `Object3D` anchors at instance world * local |

The supported/fallback/unsupported decision per capability is the
`CAPABILITY_MATRIX` in `src/capabilities.mts`, which the demo shows verbatim.
Core assets, PBR materials, recipes and attachments are supported. LOD,
collision and animation products have labeled fallbacks: base prototypes only,
not rendered, and static bind pose respectively. Terrain is rejected. Unknown
capabilities, unknown versions and required extensions fail with
`unsupported_capability`. Optional extensions are reported as `ignored`.
A document carrying a product without declaring its capability is
`invalid_input`. `NATIVE_ONLY_FEATURES` lists native renderer features
with no web equivalent, such as RT lighting, virtual texturing, POM, LOD
rung selection, the compute cull, terrain streaming and processing modules.
Assets never require any of them; the demo labels them so a web view does not
pass for native output.

## Demo

The `matter_web_demo` CMake target writes a standalone static HTTP directory at
`<build>/demo-bundle/`. It contains the compiled runtime/viewer, real WASM,
vendored three.js and its license, authoring SDK, contract utilities and the
contract example used by the fixture picker. No source checkout, editor caches,
world data, CDN or package install is needed to run it. Copy the entire directory
and serve it with any static HTTP server (module workers cannot run from `file:`):

```sh
python3 -m http.server 8080 --directory /tmp/matter-web-viewer/demo-bundle
# Open http://localhost:8080/ (also works under a hosting subdirectory).
```

`demo/index.html` loads the compiled runtime and viewer from the repository
layout. Serve them with the test runner, which maps the compiled outputs and
the WASM module:

```sh
python3.12 integrations/web-procedural/viewer/tests/browser_tests.py --serve \
  --wasm-module /tmp/matter-web-wasm/matter_geometry_wasm.mjs \
  --runtime-dir /tmp/matter-web-viewer/runtime --viewer-dir /tmp/matter-web-viewer/viewer
```

The runner prints the demo URL. Query parameters `fixture=`, `seed=` and
`wasm=` set the initial fixture, the seed and the module URL. The fixtures are
listed below; every generated fixture runs as an authoring module in a runtime
worker.

| Fixture | Content |
| --- | --- |
| `WEB-VIEW-PROP` | UV-mapped n-gon prism with a seeded checker texture (sRGB, nearest) on an extruded plinth |
| `WEB-VIEW-CSG` | Box minus a sphere at a seeded offset |
| `WEB-VIEW-ASSEMBLY` | Two shared prototypes with seeded rotations, mirrored and sheared placements |
| `WEB-VIEW-CONTRACT` | Hand-made contract asset: nested assembly, mirrored non-uniform placement, attachment, labeled LOD/collision |
| `WEB-VIEW-ERR-*` | Module syntax error, mirrored CSG operand, required extension, texture over the web limit |

The geometry kernel's `profile.extrude` currently fills UV set 0 with zeros
(`libs/ProceduralGeometry/src/geometry.cpp`, `convert`), so a texture on an
extruded mesh shows a single texel. The prop therefore authors its textured
prism as `mesh.triangles` with explicit normals and `uv0`, and keeps the
extrusion for the untextured plinth. `mesh.triangles` is an optional kernel
operation: the module offers the builder exactly the host's capabilities, so a
host without it fails with `unsupported_capability`.

Each parameter or seed change starts a new generation epoch. The runtime
rejects older work as stale, and the demo also aborts it. A newer request
supersedes an older one; superseded results are never shown. A successful
result is attached before the previous view is disposed. A failure keeps the
last good view and shows the stage (`input`, `runtime` or `adapter`), the
contract code, a plain explanation and the diagnostics, together with which
generation is still on screen. Seeds are unsigned 64-bit, entered as decimal or
`0x` hex; an invalid seed is an `input` error and changes nothing. The fallback
policy toggle switches between labeling and rejecting fallbacks.
`dispose()` cancels in-flight work, releases the view and the renderer, and
is idempotent.

three.js r186 keeps one module-global DFG lookup texture, created on the first
PBR draw and never released. `renderer.info.memory.textures` therefore reads 1
after full disposal. The tests warm it up before taking GL baselines and assert
that exact residue.

## Builds and verification

Required tools are TypeScript, Python with Playwright, and a Chromium
executable. The WASM module comes from the geometry binding build (see the
[runtime README](../runtime/README.md)). Missing tools fail; they are never
skipped.

```sh
tsc -p integrations/web-procedural/viewer/tsconfig.json
tsc -p integrations/web-procedural/viewer/tests/tsconfig.json
cmake -S integrations/web-procedural/viewer -B /tmp/matter-web-viewer -G Ninja \
  -DMATTER_WEB_WASM_MODULE=/tmp/matter-web-wasm/matter_geometry_wasm.mjs \
  -DMATTER_WEB_TEST_PYTHON=/usr/bin/python3.12
cmake --build /tmp/matter-web-viewer --parallel 1
ctest --test-dir /tmp/matter-web-viewer --output-on-failure --parallel 1
```

Build the real module first with the pinned binding toolchain:

```sh
python3 bindings/geometry-c/wasm/build-smoke.py --emsdk /home/jkern/emsdk \
  --build-dir /tmp/matter-web-wasm
```

Alternatively configure this viewer CMake root with the pinned Emscripten
toolchain, set `EM_CONFIG`/SDK Node on PATH as described in the runtime README,
and omit `MATTER_WEB_WASM_MODULE`. That builds the geometry WASM and the demo
bundle together. With `BUILD_TESTING=OFF`, the viewer library can still be built
without WASM; producing the demo requires either Emscripten or an existing real
module.

The CMake root compiles the viewer and, for the demo and tests only, the
runtime into its binary directory. It registers the type checks and the
browser run, and edits no central manifest. `MATTER_WEB_TEST_BROWSER`
defaults to `/opt/google/chrome/chrome`.

`matter_web_demo_smoke` loads the bundle's root entry point in headless Chromium,
generates the default prop in a WASM worker and checks that the canvas contains
triangles and non-background pixels. CTest enforces a ten-second total timeout;
the runner also rejects a successful smoke taking ten seconds or more. Run only
this fast check after building:

```sh
ctest --test-dir /tmp/matter-web-viewer -R '^matter_web_demo_smoke$' --output-on-failure
```

`matter_web_demo_bundle` runs all demo fixtures, parameter/seed controls,
fallback/error handling, repeated GPU allocation checks and disposal against
that same bundle, and saves canvas screenshots plus a full demo UI screenshot.
Both checks serve only the bundle beneath `/demo/`, reject HTTP/page errors and
requests outside that prefix, and require an actual WASM download. The AQ entry
point for the fast check is:

```sh
MATTER_WEB_BUNDLE_DIR=/tmp/matter-web-viewer/demo-bundle \
MATTER_WEB_TEST_PYTHON=/path/to/python-with-playwright \
  aq test integrations/web-procedural/viewer/tests/test_demo_bundle.py -q
```

The [2026-10-13 bundle evidence](evidence/2026-10-13-demo-bundle.json) records
all eight fixtures and controls with no page/HTTP errors or external requests.
The [smoke evidence](evidence/2026-10-13-demo-smoke.json) records a generated
44-triangle prop with 17.5% canvas coverage; its CTest process took 3.77 seconds.
The [screenshots](evidence/2026-10-13-demo-screenshots/) include all fixtures and
the full demo UI. [WASM build](evidence/2026-10-13-wasm.json) and
[adapter browser](evidence/2026-10-13-browser.json) evidence retain the clean
pinned build and existing fixture checks. These are Chromium/SwiftShader
measurements, not native GPU or other-browser performance claims.

`tests/browser_tests.py` serves the repository from a local HTTP server and runs
real Chromium. WebGL uses ANGLE SwiftShader, so pixels do not depend on the
host GPU. Page errors and any HTTP error fail the run. The checks are:

- **Vendor and isolation.** The vendored three.js hashes must match the
  manifest. A static scan of the runtime, authoring SDK and geometry binding
  must find no three.js or viewer import. `tsc --listFilesOnly` of the
  runtime program must contain no viewer or three.js file.
- **Runtime-only page.** `tests/runtime_only.html` generates an asset with
  the runtime alone. The run asserts that no viewer or three.js file was
  fetched.
- **Adapter fixtures** (`tests/viewer_tests.mjs`). These cover sRGB vs linear
  pixels, texture orientation, UV set 1, winding, coordinates, shear,
  mirrored tangents, disposal with GL-object accounting, and every error code.
  Each failure is checked to release its partial resources.
- **End-to-end fixtures.** Prop, CSG and assembly are regenerated through the
  worker runtime, preserving sharing, materials and transforms. The prop must
  carry at least one full UV repeat and draw adjacent checks in its two texel
  colors. The run also covers repeated updates with stable GPU counts,
  superseded requests, errors keeping the last view, error fixtures, the
  contract asset, and use after disposal.
- **Demo UI.** The run checks the capability table and native-only badges,
  every fixture with a canvas coverage check and a screenshot, parameter and
  seed controls, the fallback policy, and the attachment readout. It also
  makes 16 repeated configuration switches with per-configuration GPU memory
  equality, then disposes twice.

The run writes `--output` evidence JSON with fixture IDs, environment, served
files, GPU counts and screenshot hashes. The checked-in [evidence](evidence/)
records the run for this change. The four error screenshots are identical to the
contract screenshot because each error keeps the last good view on screen.
