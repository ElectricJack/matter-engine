# Component packages: consumers and migration

Task `clear-crest-48.9.6`, 2026-10-20. This guide is for someone who wants a
Matter component without the editor checkout: which package to take, what it
contains, how to build against it, and which host seams the application still
has to supply. Packages are **local redistributable artifacts only**. Nothing
here is published to a registry, and every npm package stays `"private": true`.

Packages are produced and checked by one script:

```sh
python3 tools/component-separation/package_components.py            # native + @matter/authoring
python3 tools/component-separation/package_components.py --emsdk ~/emsdk   # also the WASM package and demo bundle
```

Output goes to `artifacts/component-packages/` (gitignored). What goes into each
package is declared in [packages.json](../../tools/component-separation/packages.json);
the dependency rules come from [rules.json](../../tools/component-separation/rules.json).

## What the packager checks

A package is refused, not written, when any of these fails:

1. **Standalone build.** Each component is configured from its own source
   directory in Release, with the `rules.json` `probe_cache` options (tests and
   interface-only scaffolds off), and only its declared targets are built.
2. **Dependencies match the rules.** The CMake File API codemodel lists every
   target each library links. Each one must be owned by a package in
   `packages.json`, and its component must be in that component's
   `allowed_components`. The owning packages become the package's `requires`
   and its generated `find_dependency()` calls.
3. **Notices match what was compiled.** `ninja -t deps` records every header
   the compiler read for the package's own objects. Every `third_party/` file
   in that list must be covered by a declared notice, and every declared
   notice must be used. Both kinds of mismatch fail. (The first run caught
   `matter-geometry-c` declaring raylib: only the geometry package it requires
   reads a raylib header, so the notice belongs there.)
4. **A fresh consumer builds and runs.** The package and its required
   packages, and nothing else, are unpacked into an empty prefix under the
   system temp directory, outside the checkout. Existing component sources
   (examples and test drivers, copied unchanged) are built with
   `find_package(<name> <version> CONFIG REQUIRED)` against that prefix, with
   the CMake package registries off, and then run. Any header the consumer
   build reads from the checkout fails the check.

## Package matrix

Rules column = `rules.json` component and its `allowed_components`.
Notices are the `THIRD_PARTY_NOTICES.txt` sections. Matter's own sources carry
no separate notice.

| Package | Rules (allowed) | CMake targets | Requires | Third-party notices | Fresh consumer |
| --- | --- | --- | --- | --- | --- |
| `matter-contracts` 1.0.0 | contracts (none) | `matter::contracts` | — | none | `libs/MatterContracts/tests/contracts_tests.cpp` |
| `matter-geometry` 1.0.0 | geometry (contracts) | `matter::geometry`, `matter::geometry_field` | contracts | raylib (header only) | `examples/direct_recipe.cpp` |
| `matter-geometry-c` 1.0.0 | geometry_c (contracts, geometry) | `matter::geometry_c` | contracts, geometry | none | `tests/consumer.c` (C11) with `requests.json` |
| `matter-terrain` 0.1.0 | terrain (contracts, geometry) | `matter::terrain` | contracts, geometry | none | `terrain_headless_tool.cpp` writes a package, `terrain_product_consumer.cpp` (contracts only) accepts it |
| `matter-build` 0.1.0 | build (contracts) | `matter::build` | contracts | none | `tests/coordinator_tests.cpp`, 6 fixture groups |
| `matter-asset-pipeline` 1.0.0 | asset_pipeline (contracts, geometry) | `matter::asset_pipeline` = archive, surface, surface_native, prepared, storage, gltf, legacy, lod | contracts, geometry | raylib (header only), stb_image, stb_image_write | `tests/asset_tests.cpp`, 11 fixture groups |
| `matter-graphics` 1.0.0, headless | graphics (contracts) | `matter::graphics` | contracts | none | `tests/graphics_tests.cpp` |
| `matter-host-quickjs` 0.1.0 | host_quickjs (contracts) | `matter::host_quickjs` (+ `matter::quickjs`) | contracts | QuickJS-ng | `tests/host_tests.cpp` |
| `@matter/authoring` 0.1.0 | authoring | ES module | — | none | unpacked `index.mjs` + `examples/recipes.mjs` import |
| `@matter/procedural-wasm` 0.1.0 | geometry_c in WASM | ES module + `.wasm` | — | raylib, Emscripten, musl, libc++, libc++abi, compiler-rt | `node --test tests/package.test.mjs` from the unpacked tarball |
| `matter-web-demo` 0.1.0 | web_procedural (authoring, geometry_c) | static site | — | as WASM, plus three.js | every file served over HTTP and byte-checked |

Notes on the matrix:

- **raylib (header only).** `matter-geometry`'s optional mesh modifiers
  (`MATTER_GEOMETRY_MODIFIERS`, ON) compile the in-place MatterSurfaceLib
  simplifier, which reads raylib's POD `Vector3`/`Mesh` declarations. No raylib
  code is linked. The notice is still shipped, because a header was compiled.
- **`matter-terrain` requires geometry.** Its archive links
  `matter_geometry_field` (the native field kernel, `a09983dd`) privately.
  A static library still needs that archive at link time, so the package
  requires `matter-geometry` and its CMake target links `matter::geometry_field`.
- **`matter-host-quickjs`** contains the vendored QuickJS-ng archive
  `libmatter_quickjs.a`. Link `matter::host_quickjs` only; the generated
  config adds QuickJS, `m`, Threads and `${CMAKE_DL_LIBS}`.
- **`matter-graphics` headless** is the default `MATTER_GRAPHICS_VULKAN=OFF`
  build. Its library has no Vulkan code, so the package leaves out
  `vulkan_composition.h`, `vulkan_raster.h`, `vulkan_resources.h` and
  `dlss_mode.h`, whose implementations are not in it. The Vulkan device is a
  native-only option (below).
- `lod_kernels` ships as `libmatter_asset_lod_kernels.a` because
  `matter_asset_lod` links it. It is internal: do not include it directly.

Each native tarball, `<name>-<version>-linux-x86_64.tar.gz`, contains:

```
include/                      public headers of the component (contracts adds MathLib's mm:: headers)
lib/                          static archives, Release
share/cmake/<name>/           <name>-config.cmake, <name>-config-version.cmake
share/doc/<name>/             the component README(s) and examples
THIRD_PARTY_NOTICES.txt
matter-package.json           source commit, CMake options, compiler, requires, the third-party
                              headers actually compiled, and a SHA-256 for every file
```

## Using a native package

Unpack the package and its required packages into one prefix:

```sh
for p in matter-contracts-1.0.0 matter-geometry-1.0.0; do
  tar -xzf $p-linux-x86_64.tar.gz && cp -r $p/. /opt/matter/
done
```

```cmake
cmake_minimum_required(VERSION 3.25)
project(prop_tool LANGUAGES CXX)
find_package(matter-geometry 1.0 CONFIG REQUIRED)   # finds matter-contracts itself
add_executable(prop_tool direct_recipe.cpp)          # share/doc/matter-geometry/examples/
target_link_libraries(prop_tool PRIVATE matter::geometry)
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/opt/matter && cmake --build build && ./build/prop_tool
# GEOM-DIRECT-RECIPE: 1 shared prototype, 2 instances, 12 triangles
```

Each package's README in `share/doc/<name>/` documents its API. In the
repository they are:

| Package | Public headers | API reference |
| --- | --- | --- |
| contracts | `matter/contracts/{contracts,jobs,generation,hosts,graphics,terrain,abi}.h`, `mm/*` | [README](../../libs/MatterContracts/README.md), [SERVICES](../../libs/MatterContracts/SERVICES.md), [contracts v1](contracts-v1.md) |
| geometry | `matter/geometry/{geometry,terrain_field}.h` | [README](../../libs/ProceduralGeometry/README.md) |
| geometry-c | `matter/geometry_c.h` (C11) | [README](../../bindings/geometry-c/README.md) |
| terrain | `matter/terrain/{field,meshing}.h` | [README](../../libs/TerrainGeneration/README.md) |
| build | `matter/build/coordinator.h` | [README](../../libs/ProceduralBuild/README.md) |
| asset-pipeline | `matter/asset/{archive,surface,surface_tape,prepared,storage,gltf,legacy,lod}.h` | [README](../../libs/AssetPipeline/README.md) |
| graphics | `matter/graphics/{graphics,backend,scene_backend,composition}.h` | [README](../../libs/MatterGraphics/README.md), [INTEGRATION](../../libs/MatterGraphics/INTEGRATION.md) |
| host-quickjs | `matter/portable_host/quickjs_host.h` | [README](../../MatterEngine3/src/script/portable_host/README.md) |

## Using the browser packages

`@matter/authoring` is plain ES modules with TypeScript declarations and no
dependencies:

```sh
npm install ./matter-authoring-0.1.0.tgz
```

```js
import {RecipeBuilder} from '@matter/authoring';
import {orderedCsg} from '@matter/authoring/examples/recipes.mjs';  // or your own builder
```

See [packages/matter-authoring/README.md](../../packages/matter-authoring/README.md).
`@matter/procedural-wasm` evaluates those recipes in a worker; its API is
[API.md](../../web/matter-procedural-wasm/API.md):

```sh
npm install ./matter-procedural-wasm-0.1.0.tgz
```

```js
import {createProceduralEngine} from '@matter/procedural-wasm';
import {RecipeBuilder} from '@matter/procedural-wasm/authoring';
const engine = await createProceduralEngine();
const result = await engine.evaluate(recipe, {lodRatios: [0.5, 0.25]});
engine.dispose();
```

The package bundles its own copy of the authoring SDK build outputs, so it
needs no separate `@matter/authoring`. Serve `dist/` with
`application/wasm` for `.wasm`; no COOP/COEP headers are needed.

`matter-web-demo-0.1.0.tar.gz` is the [viewer demo](../../integrations/web-procedural/viewer/README.md)
as a static site: unpack it and serve the directory over HTTP (any static
server, including a hosting subdirectory). It loads three.js from its own
`vendor/` directory and fetches nothing from other hosts.

## Ownership and versioning

Ownership is fixed in [ownership.md](ownership.md); `rules.json` enforces it.
Each component owns its directory and public headers. Central build files,
`ScriptHost`/`DslState` and live world wiring belong to integration tasks.

Three versions apply, and they move independently:

- **Data contracts.** Recipe, asset and binding wires carry schema versions
  (`binding_version: 1`, `matter.recipe.core@1`, `matter.host.quickjs.v1`).
  v1.0 consumers reject other schema versions and unknown structural fields.
  New meaning needs a declared capability or a new major schema
  ([contracts README](../../libs/MatterContracts/README.md#identity-and-canonical-encoding)).
- **C ABI.** `matter/geometry_c.h` structures carry
  `{sizeof, MATTER_ABI_MAJOR, MATTER_ABI_MINOR}`; mismatches are refused.
- **Packages.** The version is the component's CMake `project(VERSION)` when it
  has one (contracts, geometry, geometry-c, asset-pipeline, graphics: 1.0.0)
  and otherwise the `packages.json` value (terrain, build, host-quickjs:
  0.1.0, pre-1.0). The packager refuses a `packages.json` version that
  disagrees with CMake. The generated version file accepts a request with the
  same major version and no newer minor, so `find_package(x 1.0)` takes any
  1.y.z. Packages are tied to one source commit (recorded in
  `matter-package.json`): mix packages from different commits only through the
  serialized contracts, not through headers.

## Build configurations

Packages are built with the default component options plus the `probe_cache`
overrides. Changing these produces a different package:

| Component | Option (packaged value) | Effect |
| --- | --- | --- |
| geometry | `MATTER_GEOMETRY_MODIFIERS` (ON) | CPU simplify/smooth modifiers; adds the raylib header notice |
| geometry | `MATTER_GEOMETRY_MESH_WORKER` (OFF) | subprocess mesh backend, native Linux only |
| terrain | `MATTER_TERRAIN_BUILD_EXAMPLES` (OFF) | packaging disables the in-tree tool; the fresh consumer builds the same sources |
| asset-pipeline | `MATTER_ASSET_{STORAGE,GLTF,LEGACY,SURFACE,SURFACE_NATIVE,LOD}` (ON) | each removes its target and its producer's capability when OFF |
| asset-pipeline | `MATTER_ASSET_ANIMATION`, `MATTER_ASSET_PHYSICS` (OFF) | optional modules, not packaged |
| graphics | `MATTER_GRAPHICS_VULKAN` (OFF) | injected Vulkan device; needs the Vulkan SDK and glslc |
| contracts | `MATTER_SCAFFOLD_*` (OFF) | interface-only scaffolds, never packaged |

Only Linux x86-64 GCC packages were produced. The canonical Windows build is
the MSVC CMake graph (`tools/build-windows.ps1`); the packager has no Windows
mode and was not run there.

## Host seams a consumer supplies

The components contain no filesystem discovery, renderer device, thread pool,
QuickJS world or live Matter world. The application provides these through the
v1 service interfaces in `matter/contracts/hosts.h` and `generation.h`
([SERVICES.md](../../libs/MatterContracts/SERVICES.md)):

| Seam | Interface | Supplied in the packages | The editor's implementation |
| --- | --- | --- | --- |
| Source discovery | `ResolverService` → frozen `SourceSnapshot` | none; tests use in-memory snapshots | engine resolvers and `local_provider` |
| Authoring | `AuthoringHost` | `matter::host_quickjs` (frozen modules, no fetch/fs) | `ScriptHost`/`DslState` |
| Geometry | `GeometryService` | `matter::geometry::PrimitiveGeometry` | engine bake via the same component |
| Build coordination | `BuildService` | `matter::build::Coordinator(resolver, host, geometry, options, &cache)` | engine `part_graph` until integration migrates it |
| Cache | `CacheService` (`CacheKey`, null lookup = miss) | `matter_asset_storage` explicit filesystem cache, optional | engine part/material caches |
| Legacy artifacts | the loader injected into `import_legacy` | none; absent loader fails `unsupported` | the existing Matter loader |
| Graphics device | `GraphicsService`, `SceneBackend` | headless `matter::graphics` facade | `VkSceneRenderer` (native) |
| Terrain field source | `matter/terrain/field.h` tapes and regions | `matter::terrain` | sector streaming in the engine |
| C/WASM | `matter_geometry_c` versioned table | `matter::geometry_c`, `@matter/procedural-wasm` | — |

Callers supply request ID, source revision and generation epoch on every job,
and keep inputs alive until completion; see
[SERVICES.md](../../libs/MatterContracts/SERVICES.md#ownership-threads-and-budgets).
Unknown required capabilities fail explicitly in every package, including the
browser viewer (`A-UNKNOWN-CAPABILITY`, `ERROR-UNKNOWN-REQUIRED`).

## Native-only optional features

None of these is in a package; each needs the native build of its module, and
most need the full editor:

| Feature | Where | How to enable |
| --- | --- | --- |
| Animation products, ozz adapter | `libs/AssetPipeline/animation` | `MATTER_ASSET_ANIMATION`, `MATTER_ANIMATION_NATIVE`; ozz archives are built by their own CMake flow |
| Collision products, Box3d settling | `libs/AssetPipeline/physics` | `MATTER_ASSET_PHYSICS`, `MATTER_PHYSICS_BOX3D` |
| GPU solid compute | `libs/GeometryCompute` | `MATTER_GEOMETRY_COMPUTE` (Vulkan) |
| Subprocess mesh worker | `libs/ProceduralGeometry` | `MATTER_GEOMETRY_MESH_WORKER` (Linux) |
| Vulkan resource device, raster | `libs/MatterGraphics` | `MATTER_GRAPHICS_VULKAN` |
| RT lighting/shadows, VT/chart atlases, POM, LOD rungs and imposters, compute cull, terrain sector streaming, retopology, transparency | MatterEngine3 / MatterEditor | editor build only |

The optional-module results are in
[capability-matrix-2026-10-20.md](capability-matrix-2026-10-20.md). The
browser viewer's capability table is `CAPABILITY_MATRIX` in
`integrations/web-procedural/viewer/src/capabilities.mts`:

| Status in the browser | Capabilities |
| --- | --- |
| Supported | `matter.asset.core`, `matter.material.pbr`, `matter.recipe.core`, `matter.asset.attachments` |
| Diagnosed fallback | `matter.asset.lod` (base prototypes), `matter.asset.collision` (not rendered), `matter.asset.animation` (bind pose) |
| Unsupported | `matter.asset.terrain` |

## Browser rendering: the qualified profile

Browser rendering is **not** claimed to match native in general. The `.9.8`
matched-view check in [qualification-2026-10-20.md](qualification-2026-10-20.md#nativebrowser-matched-view)
qualified exactly one profile:

- Chromium with SwiftShader WebGL2, the viewer's `createAssetView`;
- rendered at 17x and read at each output pixel's centre sample (`--scale 17`);
- the four parity generators (primitive, materialAssignment, orderedCsg,
  sharedInstances) against native lavapipe, within 2/255 albedo/normal and
  1e-4 depth.

At the plain 1x render the orderedCsg case **fails** (normal max 0.0153 on
2.2% of pixels, depth max 1.92e-4 on 4.4%, all on grazing CSG facets). The
cause is SwiftShader's 1/16 px vertex snap, not the viewer's geometry.
Hardware GPUs, other browsers, other drivers and other scenes are not
qualified.

## Migrating from the engine

The extraction moved code; it did not fork it. Each component now owns the
code it was moved from, and the engine compiles it from the component
directory. So migration means changing includes and links, not behaviour:

| Was | Now |
| --- | --- |
| Engine recipe/asset structs and JSON | `matter::contracts` data and canonical codec |
| Engine CPU primitive/CSG bake for props | `matter::geometry::PrimitiveGeometry::evaluate` on a `RecipeDocument` |
| `part_graph` dependency evaluation | `matter::build::Coordinator` with injected services |
| `ScriptHost`/`DslState` for authoring | `matter::portable_host` QuickJS host on a frozen snapshot, or the JS SDK directly |
| Legacy `Part` DSL verbs | `legacyPart()` in `@matter/authoring` (explicit subset; voxel CSG requires `matter.legacy.voxel-meshing@1`) |
| Legacy cached artifacts (v1/v2, FNV identities) | `matter::asset::import_legacy` with the existing loader injected; old identities are kept, not relabelled |
| Engine terrain field/mesher | `matter::terrain` field and meshing headers |
| Engine LOD/surface/glTF stages | `matter::asset_pipeline` |
| Renderer-owned asset lifetime | `matter::graphics` facade, Vulkan device injected |

The editor still links the full engine and keeps every native feature. The
engine-side consumers (`part_graph`, `ScriptHost`/`DslState`, live world
wiring) migrate in their own integration tasks.

## Packaging metrics

Run on this host (WSL2 Linux, 1 CPU, GCC 13.3, CMake 3.28, Ninja). Times are
single-job wall clock; sizes are the compressed tarballs.

| Package | Archive (B) | Files | Libraries (B) | Configure + build (s) | Consumer (s) | Consumer runs |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `matter-contracts` | 326,346 | 19 | 1,034,656 | 0.34 + 7.18 | 9.42 | 1 × exit 0 |
| `matter-geometry` | 465,026 | 10 | 1,349,694 | 0.35 + 8.41 | 2.79 | 1 × exit 0 |
| `matter-geometry-c` | 107,634 | 10 | 309,420 | 0.33 + 8.75 | 0.98 | 1 × exit 0 |
| `matter-terrain` | 186,430 | 11 | 564,162 | 0.33 + 7.61 | 4.81 | 2 × exit 0 |
| `matter-build` | 116,034 | 7 | 422,654 | 0.39 + 7.66 | 6.89 | 6 × exit 0 |
| `matter-asset-pipeline` | 556,888 | 22 | 1,755,438 | 0.42 + 12.71 | 13.85 | 11 × exit 0 |
| `matter-graphics` | 170,807 | 13 | 517,870 | 0.32 + 7.87 | 7.09 | 1 × exit 0 |
| `matter-host-quickjs` | 628,592 | 8 | 1,648,646 | 0.38 + 18.24 | 3.98 | 1 × exit 0 |
| `@matter/authoring` | 22,400 | 10 | — | no build step | — | unpacked index.mjs and examples/recipes.mjs import |
| `@matter/procedural-wasm` | 356,459 | 22 | — | build 31.33 s, `.wasm` 918,492 B | — | node --test tests/package.test.mjs |
| `matter-web-demo` | 797,404 | 45 | — | build 1.78 s | — | python http.server served all 45 files byte-identical; rendering needs a browser |

Produced by `package_components.py --emsdk ~/emsdk` (exit 0), run on the
working tree of this change over `cd981928`. Every package's third-party inputs
matched its declared notices, and every fresh consumer ran from a prefix
outside the checkout. Library bytes are the uncompressed static archives.
Native timings include building required components again, because each
package is built standalone.

## Not run here

- **Windows/MSVC packages.** The packager is Linux-only; the MSVC wrapper was
  not run for this task.
- **Native UI and editor.** There is no display or prebuilt editor on this
  host.
- **Hardware RT and device performance.** No hardware GPU (`/dev/dri`) here.
- **Browser rendering of the demo bundle.** Playwright is not installed. The
  bundle was served and every file fetched, but not rendered. The qualified
  profile above comes from `.9.8`.
