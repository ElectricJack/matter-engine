# autoremesher_core — vendoring notes

**Upstream**: https://github.com/huxingyi/autoremesher
**Upstream license**: MIT (see LICENSE)
**Pinned commit**: 061f740aae78ef562a07e076474f2b4c3d442295
**Pinned date**: 2026-07-07

## What's vendored

Cherry-picked pipeline files (see extraction rules below). None of upstream's
Qt UI, main entry, or resource files are pulled in.

Upstream layout note: upstream's top-level `src/` is entirely Qt UI code
(mainwindow, widgets, OpenGL viewers, etc). The actual remesh pipeline lives
under `src/AutoRemesher/`. Public headers under `include/AutoRemesher/` are
extensionless alias headers that just `#include "../src/AutoRemesher/<name>.h"`.
This directory layout is preserved in intent: we vendor from
`src/AutoRemesher/` into our `src/` (renamed to the spec's stage names) and
their companion headers into our `include/autoremesher/` (renamed to match).

### Pipeline sources (from upstream `src/AutoRemesher/`)

- `src/AutoRemesher/isotropicremesher.cpp` → `src/mesh_sanitize.cpp`
- `src/AutoRemesher/isotropicremesher.h`   → `include/autoremesher/mesh_sanitize.h`
- `src/AutoRemesher/parameterizer.cpp`     → `src/param_hdc.cpp`
- `src/AutoRemesher/parameterizer.h`       → `include/autoremesher/param_hdc.h`
- `src/AutoRemesher/quadextractor.cpp`     → `src/quad_extract.cpp`
- `src/AutoRemesher/quadextractor.h`       → `include/autoremesher/quad_extract.h`
- `src/AutoRemesher/autoremesher.cpp`      → `src/autoremesher.cpp` (top-level driver)
- `src/AutoRemesher/autoremesher.h`        → `include/autoremesher/autoremesher.h`

Supporting utilities and types (headers-only or single-TU) that the four
pipeline stages transitively include from upstream `src/AutoRemesher/`:

- `src/AutoRemesher/meshseparator.cpp` → `src/mesh_separator.cpp`
- `src/AutoRemesher/meshseparator.h`   → `include/autoremesher/mesh_separator.h`
- `src/AutoRemesher/positionkey.cpp`   → `src/position_key.cpp`
- `src/AutoRemesher/positionkey.h`     → `include/autoremesher/position_key.h`
- `src/AutoRemesher/double.h`          → `include/autoremesher/double.h`
- `src/AutoRemesher/radians.h`         → `include/autoremesher/radians.h`
- `src/AutoRemesher/vector2.h`         → `include/autoremesher/vector2.h`
- `src/AutoRemesher/vector3.h`         → `include/autoremesher/vector3.h`
- `src/AutoRemesher/nl_ext_stubs.c`    → `src/nl_ext_stubs.c` (OpenNL extension stubs required by geogram MIQ path)

The alias header shims under upstream `include/AutoRemesher/` (extensionless
files like `include/AutoRemesher/QuadExtractor` that just re-include the real
header from `src/AutoRemesher/`) are **not** vendored — we consume the
renamed real headers directly.

### Thirdparty subtrees (from upstream `thirdparty/`)

Upstream vendors these as raw source trees rather than git submodules
(there is no `.gitmodules` file). Absent submodule records, the "pinned SHA"
we record is the SHA of the last upstream autoremesher commit that touched
each subtree — this is the tightest reproducibility bound available and
matches what a future `git subtree pull --squash` would key off.

- `thirdparty/geogram/` — BSD-3 core only, no tetgen wrapper or any GPL-tagged
  module. Upstream vendors it as `thirdparty/geogram/geogram-1.8.3/` (that
  literal directory name; upstream Geogram version 1.8.3).
  Pinned SHA (last autoremesher commit touching this subtree):
  `b61363da1fe69c6f631f64b68518ad3f36cb6449`
- `thirdparty/isotropicremesher/` — MIT, standalone half-edge remesher lib.
  Pinned SHA: `533c6bada09c256fc1c704e73e076a8901db06b8`
- `thirdparty/eigen/` — MPL-2, headers only. Upstream vendors the full Eigen
  source tree; we only need the `Eigen/` header cluster.
  Pinned SHA: `be6a3ae1fd14d8f6861ab1314ec549c5d1f199be`
- `thirdparty/tbb/` — Apache-2. Pinned SHA: `012d9f2a909fc6245a1f6e35f34dcbf0daea3c0a`

Not vendored from upstream `thirdparty/`:
- `thirdparty/QtAwesome/` — Qt-only.
- `thirdparty/QtWaitingSpinner/` — Qt-only.

## Extraction rules

- Every synced file preserves its upstream copyright header verbatim.
- Zero Qt files. Anything under `resources/`, `shaders/`, `*.ui`, `main.cpp`,
  `mainwindow.*`, or including `<Q...>` is excluded. Upstream's entire
  top-level `src/` (Qt UI) is skipped; only `src/AutoRemesher/` is mined.
- Non-pipeline utilities (upstream logging, Qt argument parsers, GUI helpers)
  are excluded. The pipeline call graph is: sanitize → parameterize →
  quad-extract → (see Deviations re: triangulate).
- One in-place edit is expected in `src/autoremesher.cpp`: see Deviation 6
  (Qt logging concession) in "Deviations from spec" below.
- The `nl_ext_stubs.c` file has no MIT copyright header upstream (it is a
  ~30-line stub); preserved as-is.
- Every future sync is a deliberate cherry-pick against an updated pinned SHA,
  not a `git subtree pull`.

## Deviations from spec

- **`quad_to_tri.cpp` does not exist upstream.** The upstream pipeline ends at
  quad extraction (`QuadExtractor::extract()`) and returns quads via
  `AutoRemesher::remeshedQuads()`; there is no built-in quad→triangle stage.
  Task 2/3 will need to either (a) implement `quad_to_tri.cpp` locally in
  MatterEngine2 as a trivial fan/diagonal triangulation, or (b) drop the file
  and consume quads directly. Recommend (a): a ~50-line local file authored
  in-repo, marked `Copyright (c) 2026 MatterEngine2` (MIT), no upstream code
  to preserve.
- **Spec file names do not match upstream.** Mapping applied on import
  (already listed above, repeated here for the "Deviations" ledger):
  - spec `mesh_sanitize.cpp` ← upstream `isotropicremesher.cpp`
  - spec `param_hdc.cpp`     ← upstream `parameterizer.cpp`
  - spec `quad_extract.cpp`  ← upstream `quadextractor.cpp`
  - spec `quad_to_tri.cpp`   ← authored locally (see above)
- **Extra sources beyond the spec's four are required.** `autoremesher.cpp`
  (top-level driver), `meshseparator.cpp`, `positionkey.cpp`, and
  `nl_ext_stubs.c` are all part of the minimal buildable pipeline. They are
  vendored under their existing names (snake_cased). Task 2 must not treat the
  spec's four-file list as exhaustive.
- **Thirdparty subtree SHAs are approximated.** Upstream vendors thirdparty
  deps as raw source trees, not git submodules — there is no `.gitmodules`
  file and no companion SHA record. The SHAs recorded above are the last
  upstream autoremesher commits that touched each subtree, which is the
  tightest reproducibility anchor available. Actual upstream project SHAs
  (of Geogram, Eigen, TBB, etc.) at the time autoremesher last synced them
  are not recoverable from this repo alone.
- **Include path scheme differs.** Upstream uses extensionless alias headers
  under `include/AutoRemesher/` (e.g. `#include <AutoRemesher/Vector3>`). We
  drop that scheme: our vendored code will use conventional
  `#include "autoremesher/vector3.h"` style. Task 2 must rewrite the
  extensionless includes at import time.
- **Qt logging concession.** `src/AutoRemesher/autoremesher.cpp` upstream
  includes `<QDebug>` and calls `qDebug()/qWarning()`. Task 2 must strip these
  and replace with a Qt-free logging shim (e.g. `fprintf(stderr, ...)` or a
  thin `#define qDebug() ...` shim). Header copyright must be preserved.
- **TBB `__has_include` branch.** Upstream `autoremesher.cpp` already guards
  TBB headers with `__has_include`, supporting both legacy `<tbb/...>` and
  oneAPI `<oneapi/tbb/...>`. Task 2/3 must verify which branch our vendored
  `thirdparty/tbb/` snapshot triggers and confirm only that branch compiles.

  **Task 2 verified:** The vendored `thirdparty/tbb/` snapshot (wjakob/tbb,
  TBB 2017-era) exposes `include/tbb/` only — there is no `include/oneapi/`
  directory. The `__has_include(<tbb/parallel_for.h>)` branch fires; the
  `<oneapi/tbb/...>` branch does not apply. Task 3/4 should add
  `-I../thirdparty/tbb/include` and use the legacy `<tbb/...>` headers.
- **`QuadRemesher` alias may be stale.** Upstream `include/AutoRemesher/`
  ships an extensionless `QuadRemesher` alias header, but there is no
  corresponding `src/AutoRemesher/quadremesher.cpp`. Task 2 should confirm
  whether this alias resolves to anything real; if not, do not vendor it.
