# Matter procedural WASM API v1

The normative TypeScript contract is [index.d.ts](index.d.ts). The package name
is `@matter/procedural-wasm`; its built entry point is `dist/index.mjs`.
`dist/` is a self-contained ES-module package with the worker, WASM module and
copied build outputs of the existing geometry binding/runtime (no renderer).

```js
import {createProceduralEngine, ProceduralError} from '@matter/procedural-wasm';
const engine = await createProceduralEngine();
const controller = new AbortController();
try {
  const result = await engine.evaluate(recipe, {
    signal: controller.signal, lodRatios: [0.5, 0.25],
  });
  // result.meshes: shared prototypes. For every result.instances entry,
  // render meshId with transform; do not render unplaced prototypes again.
} catch (error) {
  if (error instanceof ProceduralError) console.error(error.code, error.diagnostics);
} finally {
  engine.dispose();
}
```

`recipe` is an authoring-SDK `RecipeDocument`, a `RecipeBuilder` (its `build()`
is called synchronously), or a JSON string encoding that document. Pass the
recipe itself, not the lower-level `{binding_version, recipe}` request envelope.
Inputs are snapshotted when submitted. No user source code is evaluated.
The package also exposes the existing SDK through `@matter/procedural-wasm/authoring`.

Geometry includes primitives, ordered CSG/meshing, polygon profiles, extrusion,
sweeps and affine placement supported by `libs/ProceduralGeometry`. Optional
`modifiers` apply simplify/smooth in order to all prototypes. Each `lodRatios`
entry independently simplifies the modified base with the native simplifier;
ratios are targets, not guaranteed triangle counts. Unsupported operations,
capabilities and dependencies fail explicitly with native diagnostics.

Positions/normals are packed xyz, UVs packed xy (UV set zero), indices are
uint32 triangle lists, CCW in local coordinates. Missing normals/UVs are empty
arrays; values are never invented. All arrays are independent JavaScript-owned
copies, safe after subsequent evaluations, worker cancellation or engine disposal.
`transferables(result)` returns a deduplicated buffer list for onward transfer;
transferring detaches those arrays in the sender, as usual.

Prototype `id` and `parts[]` are stable recipe node IDs. `partIds` and
`materialIds` have one entry per triangle and index `parts` and `materials`.
Groups use index offsets/counts, suitable for three.js `BufferGeometry.addGroup`.
Each instance has its own stable `id`/`partId`, shared `meshId` and row-major
local-to-asset matrix. Coordinates are right-handed, Y up, metres. Extrusion uses
the canonical transport frame: a +Z path maps profile (u,v) to (-u,-v,z). Bounds are
native conservative AABBs; smoothing/simplification need not shrink them.
LODs retain the base part/material tables and placement identities.

Evaluation always runs off the calling thread. Abort rejects with code
`cancelled`; running cancellation terminates that worker, queued cancellation
removes the job, and subsequent jobs create a replacement as necessary. No
SharedArrayBuffer, pthreads or cross-origin isolation is required. `dispose()`
rejects outstanding work with `disposed`. Timeouts reject with `resource_limit`.
All evaluation failures reject with `ProceduralError {code, diagnostics}`;
there is no partial-success mesh. Native source/node diagnostics are preserved.

Serve `dist/` intact from a static URL (WASM MIME `application/wasm`). For a Vite
app, copy it into `public/matter-procedural-wasm/` and import its index with
`/* @vite-ignore */` from that public URL. This preserves module/worker/WASM
relative URLs in development and production. Alternatively provide absolute
`workerURL` and `moduleURL` when hosting those assets separately. Browser CSP
must permit same-origin module workers and WASM compilation. Node uses
`worker_threads` automatically; no DOM or renderer is needed.

Build: `python3 web/matter-procedural-wasm/build.py --emsdk /path/to/emsdk`.
The Linux CMake preset and script use the repository's pinned Emscripten 5.0.2
profile. `dist/build-info.json` records actual tool versions, build time and
WASM bytes/hash. Generated binaries are not committed.
