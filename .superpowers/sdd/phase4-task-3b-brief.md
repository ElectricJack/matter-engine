# Phase 4 Task 3B: Async Fixture Migration

**Parent plan:** `docs/superpowers/plans/2026-07-19-phase4-runtime-scene-editor-bridge.md`, Task 3 execution subdivision 3B.

## Scope

Migrate `MatterEngine3/tests/async_bake_tests.cpp` from temporary
`schemas/` + `world_data/<Name>/world.manifest` fixtures and legacy `WorldDesc`
members to one temporary project root containing `objects/`, `worlds/<Name>.js`,
optional `shared-lib/`, and `.cache/`. This slice changes no production source,
other tests, example sources, manifest parser, or compatibility fallback.

## Binding requirements

- Preserve every existing async behavior/assertion: immediate return, completion,
  determinism, reload, supersession/cancellation, destruction, injected failures,
  focus ordering, live edit, and seed regeneration.
- Preserve every former manifest root's module order, params, transform, `expand`,
  `tileset`, light, and world-module semantics in its generated World class.
- Use portable `std::filesystem` helpers and a writable temp directory; add no new
  `/tmp`, `system("mkdir ...")`, or shell fixture setup.
- Every `WorldDesc` in this file sets `project_dir`, `world_name`, and
  `engine_shared_lib_dir`; it does not set `schemas_dir`, `world_data_dir`, or
  `shared_lib_dir`.
- Do not delete the production compatibility seam; Task 3D owns final deletion.
- Do not change runtime behavior merely to make a migrated fixture pass.

## TDD and verification

- [ ] Add a focused source-contract assertion/helper test showing project fixtures
  require `worlds/<Name>.js` and no manifest; capture the failure while one fixture
  still uses the old layout.
- [ ] Convert the shared sandbox builder first, then each exceptional fixture
  (`Multi`, `Broken2`, `FocusWorld`, `SeedBox`) without weakening assertions.
- [ ] Run `rg -n "world\\.manifest|schemas_dir|world_data_dir|shared_lib_dir|/tmp|system\\(" MatterEngine3/tests/async_bake_tests.cpp`; expected no active matches.
- [ ] Build and run `make -C MatterEngine3/tests run-asyncbake`, or compile/link and
  run `async_bake_tests` directly under UCRT64 with PATH and TEMP configured.
- [ ] Run `git diff --check` and confirm no production or unrelated test files changed.
- [ ] Commit as `test(world): migrate async bake fixtures` and write the full TDD,
  test, file, and self-review evidence to
  `.superpowers/sdd/phase4-task-3b-impl-report.md` without committing the report.
