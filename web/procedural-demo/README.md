# Matter procedural workshop

A standalone Vite / TypeScript / three.js application using the Matter procedural
WASM package and the thirteen resource-free SDK recipes in `src/recipes/`.
Geometry evaluation runs in the package's module workers. There is no mock
geometry fallback: missing assets or native errors appear explicitly.

## Run and build

Node 22.18+ is required (24 recommended for the Node TypeScript tests). The
lockfile pins all dependencies. Build the WASM package once using its pinned
toolchain, then run the frontend:

```sh
# From the repository root; see the package README for toolchain prerequisites.
python3 web/matter-procedural-wasm/build.py --emsdk /path/to/emsdk
cd web/procedural-demo
npm ci
npm run dev
```

`npm run dev` and `npm run build` copy the complete package `dist/` into the
ignored `public/matter-procedural-wasm/` directory. Set `MATTER_WASM_DIST` to use
an existing package build elsewhere. Staging checks required files before
replacing assets and preserves worker/WASM relative URLs.

```sh
npm run build
npm run preview
```

Serve the resulting `dist/` directory with any static HTTP server. Relative
asset URLs support subdirectory hosting. WASM must be served as
`application/wasm`; module workers must be allowed by the host's CSP. Cross-origin
isolation and SharedArrayBuffer are unnecessary.

## Controls and construction

Select an object in the gallery. Numeric lil-gui controls rebuild the immutable
recipe and debounce worker evaluation by 150 ms. The hexadecimal seed controls
deterministic variation. Reset restores dimensions and the default seed.
Orbit, pan and zoom with the pointer; Fit view frames transformed mesh bounds.
Wireframe and Normals change display without reevaluating geometry. Normals
shows surface-normal colors. Geometry time comes from the worker; upload time
includes the first render, which uploads buffers. Triangle totals count placements.

The construction scrubber traverses reachable recipe nodes in dependency order,
including forward references. Each mesh operation replaces its consumed operands
while independent components accumulate. Ordered CSG steps show their declared
union/subtract sequence. Profile-only steps are labelled and have no solid
preview until extrusion; they are never sent as invalid empty-output requests.
Final object evaluates the exact original document, including its authored
outputs, so reusable prototypes are not accidentally drawn as extra objects.

Only the selected stage is evaluated; at most four JS-owned stage results are
retained. Parameter changes clear that cache. Superseding requests abort the
worker job, and a generation check rejects late results even if an adapter ignores
abort. Errors clear old geometry and expose native diagnostics. Shared prototype
geometry is drawn once per placed instance with the contract's row-major
matrix; material groups, part IDs and per-triangle material IDs are retained.
GPU objects are disposed on replacement, hot reload and page exit.

## Verification

```sh
npm test
npm run build
# Install Chromium once if not already in Playwright's cache:
npx playwright install chromium
npm run test:browser
# Equivalent browser gate with AQ's resource controls, from repository root:
aq test web/procedural-demo/frontend-tests/test_browser.py
```

The Node tests cover dependency traversal, exact final outputs, cancellation,
stale completions, cache eviction, shared geometry, row-major transforms and
disposal. The Chromium gate loads the real worker/WASM, renders all thirteen
default objects, edits dimensions, scrubs construction, toggles display modes,
tests rapid selection/seed errors/reset, checks mobile overflow and serves the
static build below `/workshop/`. Screenshots and JSON are written to the ignored
`frontend-tests/artifacts/` directory. Set `DEMO_TEST_URL` to apply the same gate
to a running development server. These browser checks use software WebGL2;
they do not establish performance on every physical GPU.

The recipe owner's native topology and parameter-range gates remain in
`tests/` and are documented in `src/recipes/README.md`.
