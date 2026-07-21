# Phase 4 Task 3B Implementation Report

## Status

DONE. `async_bake_tests.cpp` now creates project-root JavaScript fixtures only,
uses the preferred `WorldDesc` contract throughout, and passes its existing async
runtime target under MSYS2 UCRT64.

## Implementation

- Replaced shell setup/cleanup with `std::filesystem` helpers rooted beneath
  `temp_directory_path()` and a per-run timestamp.
- Added a focused fixture contract that requires `worlds/<Name>.js` and rejects
  the legacy world-data tree.
- Added shared world/root writers so every generated root has the former
  manifest identity transform and retains authored order and flags.
- Migrated the shared `Box` fixture and exceptional `Multi`, `Broken2`,
  `FocusWorld`, and `SeedBox` fixtures to `objects/`, `worlds/`, `shared-lib/`,
  and `.cache/<World>/parts`.
- Preserved three ordered `Box` roots, ordered `Multi` roots, valid-before-broken
  `Broken2` roots, `FocusWorld`'s `World` module with `expand: true`, and
  `SeedBox`'s empty root params so `Box.static params.worldSeed` remains the
  default and `regenerate()` remains the only root override.
- Routed all six descriptor construction sites through one aggregate helper that
  supplies project root, world name, and the engine shared-library tier without
  touching the transitional production seam.
- Moved the Linux guard boundary to cover only the inotify test. The seed test
  was already called unconditionally; this makes the existing assertion compile
  and run on UCRT64 rather than weakening or skipping it.

## TDD Evidence

### Source-contract RED

The contract helper was added while `Box` still wrote its old layout. After
compiling and linking the existing target, the binary stopped before async work
with the expected failures:

```text
FAIL: project fixture provides worlds/<Name>.js
FAIL: project fixture has no legacy manifest tree
```

The first UCRT64 attempts exposed environment/build prerequisites before that
assertion could run: PowerShell had no `make`, the checkout stored the shader
symlink as a placeholder, the seed test's Linux guard did not match its
unconditional call, and the tracked Box3D archive was not the UCRT64 archive.
The ignored embedded-shader header and existing `build-mingw` Box3D archive were
used only for verification.

### Conversion and semantic RED/GREEN

The first complete converted run compiled and linked, and all non-focus async
cases passed. Focus assertions failed eight times with the same
`BoxB BoxA BoxC` order for both focus points, accompanied by near-singular root
transform diagnostics. This demonstrated that omitted JavaScript transforms did
not preserve the legacy manifest's identity default.

The root helper was corrected to emit the approved explicit identity matrix for
every root. The next run produced the required orders:

```text
[focus near C] order: BoxC BoxB BoxA
[focus near A] order: BoxA BoxB BoxC
[focus near C, repeat] order: BoxC BoxB BoxA
```

and ended with:

```text
ALL PASS
```

## Verification

- Exact source contract:

  ```text
  rg -n "world\.manifest|schemas_dir|world_data_dir|shared_lib_dir|/tmp|system\(" \
    MatterEngine3/tests/async_bake_tests.cpp
  ```

  No matches.

- Fresh UCRT64 target after the final fix:

  ```text
  make -C MatterEngine3/tests GRAPHICS=GRAPHICS_API_OPENGL_33 \
    LDFLAGS=-lm \
    LDLIBS='../../Libraries/raylib/src/libraylib.a -lopengl32 -lgdi32 \
      -lwinmm -lshell32 -ldbghelp -lws2_32' \
    run-asyncbake
  ```

  Compile and link succeeded; runtime ended `ALL PASS`. Covered immediate return,
  completion, determinism, reload, supersession/cancellation, destruction,
  injected OOM/script/load failures, focus ordering, and seed regeneration.

- `git diff --check`: exit 0.
- Scoped status before commit: only
  `MatterEngine3/tests/async_bake_tests.cpp` was modified. No Makefile or
  production source changed.
- The tracked Box3D archive was restored to its original SHA-256
  `795FD2C8FCEC1B5937B4A812B7999F5E4D47CCCE9EC08E20CADE9CD9230D07F6`;
  temporary verification files were removed.

## Self-review

- Root parity: all former manifest roots retain module order, empty params,
  explicit identity transform, and the sole `expand` flag. No fixture used
  `tileset`, lights, or non-empty manifest root params.
- Async behavior: no timing threshold, timeout, event sequence, error code,
  failure injection, focus expectation, live-edit assertion, or seed assertion
  was relaxed.
- Portability: no process ID API, POSIX headers, shell commands, or hard-coded
  temporary directory remains.
- Lifetime: aggregate descriptor pointers refer to caller-owned project/world
  strings that remain alive through `open_world`, matching the existing API's
  borrowed-view requirement.
- Scope: the implementation diff is one test file. This report remains
  intentionally uncommitted.

## Concerns

- The UCRT64 run cannot execute the Linux-only inotify live-edit case; its body
  and assertions were migrated but remain covered only on Linux, as before.
- UCRT64 emits the existing unused `ev_type_name` warning because that helper is
  used only by the Linux live-edit case. No new warning was introduced on Linux.
- The repository test Makefile still needs Windows link overrides and a
  Windows-compatible Box3D archive when invoked from UCRT64; these are existing
  environment/build concerns and were not changed in this fixture-only slice.
