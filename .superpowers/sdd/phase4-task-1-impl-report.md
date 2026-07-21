# Phase 4 Task 1 Implementation Report

## Status

DONE. The engine now owns an isolated World JavaScript definition loader while the
existing field-world `ScriptHost::eval_world` behavior remains intact.

## Summary

- Added the owned public `WorldRoot`, `WorldLight`, `WorldSettings`,
  `RawEntityRecipe`/`EntityRecipe`, `WorldDefinition`, `WorldLoadDesc`, and
  `WorldLoadError` contract.
- Added `matter::load_world_definition`, which reads one world source, resolves
  shared-library imports project-first with engine fallback, evaluates in a fresh
  restricted QuickJS context, extracts roots/lights/settings, and normalizes static
  plus `buildEntities()` records into ordered owned JSON strings.
- Preserved the established renderer lighting shape by retaining sun/sky settings
  and optional spot direction/cone data in addition to the planned point-light
  fields; no renderer/runtime migration was performed.
- Added a narrow `ScriptHost::eval_world_definition` forwarding boundary and shared
  World-class lexical selection with the existing field evaluator.
- Added the focused `run-world-definition` test target.

## TDD Evidence

### RED

Command:

```text
make -C MatterEngine3/tests GRAPHICS=GRAPHICS_API_OPENGL_33 run-world-definition
```

Observed expected failure before production files existed:

```text
world_definition_tests.cpp:3:10: fatal error:
../src/script/world_definition_loader.h: No such file or directory
make: *** [Makefile:785: build/qjs/world_definition_tests.cpp.o] Error 1
```

### GREEN

Focused command after implementation:

```text
make -C MatterEngine3/tests GRAPHICS=GRAPHICS_API_OPENGL_33 run-world-definition
```

Observed:

```text
./world_definition_tests
ALL PASS
```

Fresh specified regression command (MSYS2 UCRT64; Windows raylib/system-link
overrides replace the Makefile's Linux defaults):

```text
make -C MatterEngine3/tests GRAPHICS=GRAPHICS_API_OPENGL_33 \
  LDFLAGS=-lm \
  LDLIBS='../../Libraries/raylib/src/libraylib.a -lopengl32 -lgdi32 -lwinmm -lshell32' \
  run-world-definition run-evalworld run-script
```

Observed exit code 0; all three binaries reported `ALL PASS`.

## Files

- `MatterEngine3/include/matter/world_definition.h` — owned public contract.
- `MatterEngine3/src/script/world_definition_loader.h` — loader entry point and
  shared World-class selector.
- `MatterEngine3/src/script/world_definition_loader.cpp` — hermetic evaluator,
  tiered module loading, extraction, and owned copying.
- `MatterEngine3/src/script_host.h` — statics-loader forwarding API.
- `MatterEngine3/src/script_host.cpp` — shared class-selection helper use.
- `MatterEngine3/tests/world_definition_tests.cpp` — focused contract coverage.
- `MatterEngine3/tests/Makefile` — focused QuickJS test target.

## Self-review

- Completeness: covers base rejection with source/property diagnostics, ordered
  roots/lights/settings/entities extraction, canonical owned JSON, project override
  and engine fallback, no-entity worlds, explicit seed/parameter bindings, and
  field/constructor non-execution.
- Hermeticity: uses a fresh raw QuickJS context; exposes no Date, fetch, require,
  process, ECS pointers, frame hooks, or post-load handle. `Math.random` is removed;
  authored generation must consume the explicit seed.
- Failure behavior: malformed sources/statics/imports return a source location and
  property path; failed partial definitions are cleared.
- Quality/YAGNI: no component registry validation, ECS mutation, runtime path cut,
  caching, watcher, spawning lifetime, or Viewer UI was added.
- Test validity: RED failed specifically for the missing production boundary, and
  GREEN exercises real filesystem fixtures and the real QuickJS/module loader.

## Concerns

- The repository's test Makefile defaults to Linux link flags on MSYS2 and the
  checkout lacked a built raylib archive. Verification therefore built the bundled
  ignored `Libraries/raylib/src/libraylib.a` and supplied Windows system libraries
  on the command line. This does not change committed product sources.
- Existing evaluator builds emit pre-existing compiler/runtime warnings; all
  requested tests still returned exit code 0.

## Review-fix follow-up

### Findings addressed

1. Bootstrap dispatch now installs `entity` as a loader-owned native function on
   the ephemeral instance with neither writable nor configurable flags. Authored
   prototype methods cannot intercept, suppress, or replace recipe collection.
2. Canonical JSON copying now accepts only a JavaScript string result from
   `JSON.stringify`. The extractor distinguishes absent optional properties from
   explicitly authored `undefined`, so function/undefined top-level params and
   components fail with their contextual property path rather than producing the
   invalid bytes `undefined`.
3. Getter exceptions, non-callable values, and thrown `buildEntities()` calls all
   reset `WorldDefinition` to its default empty value before returning failure.
   Exception diagnostics now retain both the error message and stack.

### Follow-up RED evidence

After adding the review regressions and before changing production code:

```text
./world_definition_tests
FAIL: loader-controlled entity dispatch cannot be shadowed by authored prototype
FAIL: function-valued root params are rejected
FAIL: non-JSON params report their contextual property
FAIL: non-JSON params leave no partial definition
FAIL: explicit undefined components are rejected
FAIL: undefined components report their contextual property
FAIL: undefined components leave no partial definition
FAIL: non-callable buildEntities clears extracted roots and lights
FAIL: buildEntities exception retains contextual diagnostics
FAIL: throwing buildEntities clears extracted roots and settings
11 FAILURE(S)
```

The fixes were applied one contract at a time: the intermediate focused runs reduced
the failure count from 11 to 9, then from 9 to 3, before reaching GREEN.

### Follow-up GREEN evidence

Focused:

```text
./world_definition_tests
ALL PASS
```

Fresh requested regression command:

```text
make -C MatterEngine3/tests GRAPHICS=GRAPHICS_API_OPENGL_33 \
  LDFLAGS=-lm \
  LDLIBS='../../Libraries/raylib/src/libraylib.a -lopengl32 -lgdi32 -lwinmm -lshell32' \
  run-world-definition run-evalworld run-script
```

Observed exit code 0; `world_definition_tests`, `eval_world_tests`, and
`script_host_tests` each reported `ALL PASS`.

### Follow-up self-review

- The native append callback duplicates the authored record into the single
  loader-owned ordered array and exposes no persistent handle or host pointer.
- A non-configurable own property wins normal lookup over any authored class
  prototype method and rejects reassignment from strict class methods.
- Missing `params`/`components` still preserve their documented `{}` default;
  only explicitly present unserializable values are rejected.
- Every post-extraction bootstrap failure now follows the same fail-closed output
  behavior as earlier extraction failures.
- No entity component validation, ECS mutation, runtime migration, or Viewer work
  was added by these review fixes.
