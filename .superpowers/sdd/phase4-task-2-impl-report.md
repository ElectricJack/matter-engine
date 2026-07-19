# Phase 4 Task 2 Implementation Report

## Status

DONE_WITH_CONCERNS. Project-root runtime routing, JavaScript world loading,
cache identity, Viewer discovery, authored transforms/lights/settings, and
project-layout tileset paths are implemented. The two focused unit gates pass.
The Viewer-logic target compiles and links, but its direct runtime gate exceeded
70 seconds without flushed output and was interrupted at the parent agent's
request, so an all-PASS Viewer runtime result is not claimed.

## Implementation

- Added preferred `WorldDesc { project_dir, world_name,
  engine_shared_lib_dir, enable_live_edit }` routing. The old descriptor fields
  remain only behind the explicit `project_dir == nullptr` test seam.
- Added `LocalProviderConfig::for_project`, deriving `objects/`, `worlds/`, the
  optional project `shared-lib/`, engine shared-library tier, and
  `.cache/<world>/`.
- Replaced project manifest reads with Task 1 `load_world_definition`, adapting
  roots in authored order with canonical params, transforms, `expand`, and
  `tileset`; settings and lights are translated to the current runtime types.
- Routed authored root transforms through composition, including multiplication
  with expanded-child relative transforms.
- Changed resolve-cache identity to hash world JavaScript, recursive object
  sources, project and engine shared sources, and canonical params. Legacy
  manifests are ignored for project configurations.
- Moved project resolve, part, settle, and `.gtex` outputs beneath
  `.cache/<world>/`. Project tileset scripts resolve directly from `objects/`.
- Updated Viewer discovery on both platform variants to enumerate
  `worlds/*.js`; Viewer calls now use `project_dir` and never the legacy seam.
- Added loader sources to engine, Viewer, and focused-test builds.
- Made focused test temp fixtures portable under MSYS2/UCRT64 with
  `std::filesystem`.

## TDD Evidence

RED:

- `world_definition_tests.cpp`: `for_project` was not a member of
  `viewer::LocalProviderConfig`.
- `resolve_cache_tests.cpp`: the project-tier five-input `compute_key` call had
  too many arguments for the legacy four-input API.
- The provider adaptation test initially failed because
  `ProviderWorldDefinition` did not exist.

GREEN:

- `world_definition_tests.exe`: `ALL PASS`.
- `resolve_cache_tests.exe`: `162/162 passed`.
- UCRT64 compilation of the touched `local_provider.cpp` and
  `tileset_phase.cpp` Viewer-logic objects: PASS.
- `viewer_logic_tests.exe`: compiled and linked successfully; runtime remained
  active beyond 70 seconds with buffered stdout and was interrupted, so BLOCKED
  rather than PASS/FAIL.

## Transition-Seam Audit

- MatterViewer contains no use of `schemas_dir`, `world_data_dir`, or the legacy
  `shared_lib_dir` descriptor.
- Production provider/engine legacy field accesses are confined to branches
  guarded by `!uses_project_layout()` / `project_dir == nullptr`.
- New tests construct project paths through `for_project`; existing Viewer-logic
  fixtures remain on the explicitly authorized Task 2 compatibility seam for
  Task 3 migration.

## Verification Environment

MSYS2 UCRT64 with Windows Raylib/system libraries. The Viewer build additionally
required `-ldbghelp -lws2_32`. The tracked Box3D archive was temporarily rebuilt
for the Windows link, then restored byte-for-byte to `HEAD`; generated Box3D
objects, `.tmp`, and `MatterEngine3/shaders_gen` were removed before commit.

## Concern

The Viewer-logic binary needs one longer or output-unbuffered runtime pass in the
integration environment. No assertion failure from the current executable was
observed before interruption.

Task 1 world-module loading uses both shared-library tiers. The existing
`ScriptHost`/PartGraph API accepts only one shared-library root, so object-part
evaluation currently selects the project tier when it exists and the engine tier
otherwise; it cannot perform per-import engine fallback while a project
`shared-lib/` is present without a broader resolver API change.

## Review-Fix Addendum

This addendum supersedes the earlier `DONE_WITH_CONCERNS` status and shared-tier
concern. All three Important findings in `phase4-task-2-review.md` were addressed.

### I1: ordered shared-library tiers and exact live-edit paths

- `module_resolver` and `ScriptHost` now accept ordered roots and resolve every
  direct or transitive import project-first with per-import engine fallback.
- Fold-cache identity includes every root in order. PartGraph snapshots and the
  resolve cache retain the exact selected source path for each imported module.
- LocalProvider install/restore, procedural evaluation, sector baking, tileset
  evaluation, and project live edit all receive the ordered roots. The legacy
  descriptor retains its single-root live-edit fan-out behavior.
- RED: ordered-root resolver, ScriptHost, snapshot, vector live-edit constructor,
  and serialized-path assertions failed to compile or failed `163/164` cache
  assertions before the APIs/format existed.
- GREEN: `shared_lib_tests` reports `All shared_lib tests passed`,
  `live_edit_prod_tests` reports `ALL PASS`, and the resolve-cache path test is
  included in the final `168/168 passed` result.

### I2: authoritative project procedural settings

- Project `WorldSettings` now selects the runtime sector profile; its
  `sector_size`, `y_min`, and `y_max` feed streaming, the provider world binding,
  and every per-sector `BakeOptions`. Only the legacy layout uses values returned
  by the compatibility evaluator.
- RED: the non-default project profile test did not compile before
  `ProceduralWorldProfile` existed.
- GREEN: `resolve_cache_tests` reports `168/168 passed`, including authored
  `37/-23/141` propagation and legacy fallback assertions.

### I3: tileset-root params and cache identity

- Project tileset canonical root params now flow through `eval_requires` and
  `eval_tileset`, and their bytes participate in the settle-cache key. Legacy
  entry points continue to supply `{}`.
- RED: the focused test initially failed to compile because the parameterized
  project overload and three-argument settle key did not exist. Its first runtime
  run then exposed a production defect: imported tileset roots were evaluated as
  global scripts during `eval_requires`, so the child declaration was silently
  lost and `eval_tileset` reported `layer('Pebble'): undeclared module`.
- GREEN: `tileset_params_tests` reports `ALL PASS`, covering project shadowing,
  transitive engine fallback, parameter-driven placement, identical-parameter
  warm cache, and parameter-only invalidation.

### Final verification and self-review

- Fresh affected-suite command: shared-lib PASS; live-edit production PASS;
  resolve cache `168/168`; world definition PASS; tileset params PASS.
- `viewer_logic_tests` compiled and linked successfully (not run, per the Task 2
  verification boundary).
- The changed `matter_engine.cpp` GPU-flavor translation unit compiled
  successfully.
- `git diff --check` returned no whitespace errors.
- Self-review confirmed all project ScriptHost call paths receive ordered roots,
  exact-path behavior is project-only, every procedural sector uses the selected
  profile, tileset params reach both evaluation stages and the cache key, and no
  review/report/plan/progress artifact is included in the implementation commit.

## Second Re-review Addendum

The remaining functional Tileset `requires(params)` path now retains the caller's
canonical authored root params when `merge_params_canonical` reports the expected
no-`Part`-class fallback. The same JSON therefore reaches functional
`static requires(params)` and the existing `eval_tileset`/`build(params)` path.

Focused TDD evidence:

- RED: the direct regression could not resolve either authored dependency, and
  end-to-end `run-tilesetparams` failed with
  `child install failed: missing requires target: undefined` (7 cascading
  failures).
- GREEN: the test observes `Pebble` with `{"seed":3}` for the first authored
  root and `Stone` with `{"seed":7}` for the changed root; the complete
  `tileset_params_tests` binary reports `ALL PASS`.
- Final requested regression set: tileset params `ALL PASS`; shared-lib
  `All shared_lib tests passed`; resolve cache `168/168 passed`.
- The test builds recompiled `script_host.cpp`, `tileset_phase.cpp`,
  `tileset_bake.cpp`, and `resolve_cache.cpp` successfully.

Documentation was corrected to describe resolve-cache format v4's selected
shared-source paths and canonical root params in the settle-key input. The report,
review, progress ledger, and review-fix plan remain outside the implementation
commit.
