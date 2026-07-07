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

### Locally authored (not vendored)

- `src/quad_to_tri.cpp` — trivial fan/diagonal triangulator; no upstream
  equivalent (see Deviation #1). MatterEngine2 MIT copyright.
- `include/autoremesher/quad_to_tri.h` — companion header for the above.
- `tests/quad_to_tri_test.cpp` — standalone unit test for the triangulator
  (single quad, two quads, empty input, malformed input). Not yet wired into
  a Makefile; Task 6 will create `tests/Makefile`.

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
  **License caveat:** Eigen ships GPL/LGPL-licensed submodules as source
  (e.g. `Eigen/src/SuperLUSupport/`, `Eigen/src/UmfPackSupport/`,
  `Eigen/src/CholmodSupport/`, `Eigen/src/PastixSupport/`,
  `Eigen/src/SPQRSupport/`). Vendored code MUST NOT `#include` any of these
  — MPL-2 core only. The pipeline uses only `<Eigen/Dense>` (MPL-2), so
  this is enforced by the fact that no source pulls in the GPL/LGPL paths.
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
  **Task 4 addendum 1:** the file also uses `std::this_thread::sleep_for` inside
  the progress-lock guard. Upstream got `<thread>` transitively via `<QDebug>`;
  the Qt-free shim (this Deviation) drops that path, so an explicit
  `#include <thread>` was added at the same time as the shim. Marked with an
  inline comment referencing this Deviation.

  **Task 4 addendum 2 (`nl_ext_stubs.c`):** upstream's `NL/nl_amgcl.cpp` was
  excluded from the build (upstream typo + missing amgcl dep — see Task 4
  Makefile comment). To keep the OpenNL fallback intact, a `nlSolveAMGCL()`
  no-op stub was added to `src/nl_ext_stubs.c` matching the style of the
  other stubs in that file (returns `NL_FALSE`). This is a modification to
  a Task 3-vendored file and is logged here for paper-trail hygiene; upstream
  `nl_ext_stubs.c` has no MIT copyright block to preserve.
- **TBB `__has_include` branch.** Upstream `autoremesher.cpp` already guards
  TBB headers with `__has_include`, supporting both legacy `<tbb/...>` and
  oneAPI `<oneapi/tbb/...>`. Task 2/3 must verify which branch our vendored
  `thirdparty/tbb/` snapshot triggers and confirm only that branch compiles.

  **Task 2 verified:** The vendored `thirdparty/tbb/` snapshot (wjakob/tbb,
  TBB 2017-era) exposes `include/tbb/` only — there is no `include/oneapi/`
  directory. The `__has_include(<tbb/parallel_for.h>)` branch fires; the
  `<oneapi/tbb/...>` branch does not apply. Task 3/4 should add
  `-I../thirdparty/tbb/include` and use the legacy `<tbb/...>` headers.
- **`QuadRemesher` alias is stale (confirmed Task 3).** Upstream
  `include/AutoRemesher/QuadRemesher` re-includes
  `../src/AutoRemesher/quadremesher.h`, but no such file exists in upstream
  `src/AutoRemesher/` at the pinned SHA (only `quadextractor.{cpp,h}` does).
  The alias is dead; not vendored.

## Task 6 additions

- **`src/remesh.cpp` — headless driver.** Locally authored (MatterEngine2 MIT
  copyright), composes upstream's `AutoRemesher::AutoRemesher` class end-to-end
  (Option A per the Task 6 brief) rather than reimplementing the sanitize →
  parameterize → quad-extract pipeline stage-by-stage. Adds the
  `autoremesher::remesh()` public entrypoint and the
  `AUTOREMESHER_CORE_VERSION = "0.1.0-2026-07-07"` string constant.
- **`tests/smoke_cube.cpp` — spherified subdivided cube.** The plan's original
  unit cube (12 tris) is too coarse for the upstream cross-field parameterizer
  — the extractor returns zero quads on inputs that flat and simple. The
  smoke test now builds a subdivided cube projected onto a unit sphere
  (N=8, 386 verts / 768 tris), which the pipeline handles cleanly. Result:
  `OK: 265 verts, 526 tris, elapsed≈0.05s`.
- **Fan-triangulation of quad-dominant output.** `QuadExtractor::remeshedQuads()`
  is quad-DOMINANT, not quad-only: it may emit 3- and 5-gons at parametric
  singularities. The driver fan-triangulates all n-gons directly in
  `faces_to_triangles()`. Task 3's `ar_internal::triangulate` is intentionally
  strict quad-only (matches the header contract) and remains available for
  callers with a guaranteed-quad input stream.
- **Geogram initialization.** Upstream's Qt main.cpp called `GEO::initialize()`
  + `GEO::CmdLine::import_arg_group()` for the "standard", "algo", and "remesh"
  groups. Neither is called from anywhere in the vendored pipeline sources;
  the headless driver does both under `std::call_once` guard on first
  `remesh()` invocation. Also calls `GEO::Logger::instance()->set_quiet(true)`.
- **Explicit TBB `task_scheduler_init`.** On gcc-13 + glibc 2.39 + WSL2 kernel
  6.18, the wjakob 2017 TBB static observer path segfaults inside
  `observer_list::do_notify_entry_observers` on the first `tbb::parallel_for`
  call from the pipeline. Constructing a `tbb::task_scheduler_init` at the top
  of the driver forces early, well-ordered scheduler init and works around it.
  This is deprecated in oneAPI TBB but still supported in the vendored TBB.
- **`opts.iterations`, `opts.seed`, `opts.threads` accepted but not wired.**
  Upstream's `AutoRemesher` class has no setter for these; changing solver
  iterations or RNG seed would require modifying vendored `param_hdc.cpp` /
  geogram internals. Threading pinning would require `tbb::global_control`
  which we don't currently invoke (see driver comment). Fields are accepted
  for API-surface stability and revisited in Phase 6+.
- **`upstream.setScaling(1.0)`.** Upstream's `AutoRemesher::m_scaling` default
  is 0.0, which passes through to `Parameterizer::m_scaling` and ultimately to
  `GEO::GlobalParam2d::quad_cover(scaling=0.0, ...)`, producing zero quads on
  every input we tried. `Parameterizer.h`'s own header-declared default is
  1.0; the driver passes 1.0 explicitly to override.
- **Additional geogram third_party sources compiled.** The `mesh_io.cpp`
  translation unit in geogram unconditionally references symbols from
  `libMeshb` (`Gmf*`), `rply` (`ply_*`), and `zlib` (`gz*`). These are never
  called from our headless pipeline (no file I/O), but the linker requires the
  symbols to resolve. Added under `GEO_TP_C` in the Makefile with a comment.
  Licenses: MIT (libMeshb, rply) and zlib (permissive) — no LGPL/GPL taint.
  `lua/`, `HLBFGS/`, `PoissonRecon/`, `triangle/`, `xatlas/`, `stb_image/`
  remain excluded from compile.
- **Vendored TBB built (release only).** `thirdparty/tbb/build/linux_*_release/
  libtbb.so.2`. Two build-config edits made under `thirdparty/tbb/build/
  linux.gcc.inc`:
  - `WARNING_SUPPRESS` extended with `-Wno-changes-meaning -Wno-class-memaccess
    -Wno-deprecated-declarations -Wno-cast-function-type
    -Wno-stringop-overflow` to survive gcc-13.
  - `ITT_NOTIFY = -DDO_ITT_NOTIFY` cleared for the `intel64` arch — VTune
    profiling hooks; we don't use VTune and the observer path had ancillary
    fragility (see task_scheduler_init note above). Comment inline.
- **`tests/Makefile` links against vendored TBB by rpath.** Uses `$ORIGIN`
  relative rpath to avoid embedding the absolute build path (which contains
  spaces and breaks `ld -rpath`). The `-rpath` includes the versioned TBB
  build subdir name.
