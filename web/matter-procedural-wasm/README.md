# Portable procedural geometry package

Build from Linux with an activated **Emscripten 5.0.2** SDK (revision in
`bindings/geometry-c/wasm/toolchain.json`), its bundled Node **22.16.0**, CMake
>=3.25, Ninja, Python 3 and TypeScript **5.9.3** (`tsc` on PATH):

```sh
python3 web/matter-procedural-wasm/build.py --emsdk "$EMSDK"
# Optional build directory on the Linux filesystem for faster WSL builds:
python3 web/matter-procedural-wasm/build.py --emsdk "$EMSDK" --build-dir /tmp/matter-procedural-build
node --test web/matter-procedural-wasm/tests/package.test.mjs
tsc -p web/matter-procedural-wasm/tests/tsconfig.json
# AQ workers use the resource-gated test entry:
aq test web/matter-procedural-wasm/tests/test_package.py
```

The script configures `linux-wasm-release` from the package's CMake presets,
builds with two compiler jobs, compiles the existing worker scheduler, then
assembles `dist/`. It reuses the native C ABI, canonical geometry sources and
the locked scalar/no-pthread WASM profile. No editor, renderer, QuickJS, GPU,
network access or filesystem access is needed during evaluation. There is no
runtime npm dependency. Build prerequisites are host tools; the script never
installs or downloads them. A clean build uses a new `--build-dir`; no existing
build or user files are deleted. Build timings include configure, compile, link,
TypeScript and packaging, excluding SDK installation.

`dist/build-info.json` records the actual build time, raw `.wasm` bytes, SHA-256,
compiler/tools, commands and profile hash. Binaries and machine-specific reports
are ignored. Commit the source, rebuild the package on the serving machine, then
copy **all of dist**, retaining filenames and subdirectories.

For the new Vite demo, use the local package dependency
`"@matter/procedural-wasm": "file:../matter-procedural-wasm"` for type checking.
Copy `dist/` to the app's `public/matter-procedural-wasm/` before `vite build`.
Import at runtime without bundling these files:

```ts
const url = `${import.meta.env.BASE_URL}matter-procedural-wasm/index.mjs`;
const {createProceduralEngine} = await import(/* @vite-ignore */ url) as
  typeof import('@matter/procedural-wasm');
const engine = await createProceduralEngine();
```

For Node import `dist/index.mjs` directly. See [API.md](API.md) for ownership,
placements, cancellation and diagnostic semantics. SDK recipe examples live in
`packages/matter-authoring/examples/recipes.mjs`; the three fast package tests
build recipes in memory and require no model assets or renderer.

Modifiers/LOD use the existing smooth/simplify backend. Unsupported retopology,
asset assembly/resource resolution and unsupported CSG transforms remain explicit
errors. Native budgets apply to each evaluate/process stage; the execution timeout
covers the entire worker job (including initialization and all LOD stages).

Measured on Linux/WSL, 2026-10-12: **914,223 bytes** raw WASM; clean package
build **236.380 seconds** including first-use C++ sysroot library generation.
A subsequent configure/rebuild took **133.431 seconds** and produced the same
WASM SHA-256. See [verification evidence](evidence/2026-10-12-package.json).
Node 22/24, the unchanged binding suite, TypeScript consumer checks, and a real
Chromium worker served from `dist/` passed. These are package checks; they do
not qualify the separate Vite/three.js frontend.
