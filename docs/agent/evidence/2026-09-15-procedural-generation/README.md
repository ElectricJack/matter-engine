# Procedural generation: revised direction and first implementation

Date: 2026-09-15. Native Windows x64 MSVC, RelWithDebInfo, driven from WSL through the canonical build wrapper.

## Working objective

The user requested visual development first: GPU DSP/SDF material generation, analytic geometry placement, selective cached physics, and authored/procedural splats sharing those sources. The [design](../../../superpowers/specs/2026-09-14-layered-surface-texturing-design.md) specifies this for terrain and buildings; the [implementation plan](../../../superpowers/plans/2026-09-14-layered-surface-texturing.md) records the revised objective, proof recipes and acceptance gates. Strict VT performance acceptance is deferred with its original targets intact.

The app goal was still paused when checked after native validation. Its API exposes neither objective editing nor resume. The working instruction is saved in the plan and foreground implementation has resumed; the stored goal has not been replaced or falsely marked complete.

## Implemented: skip unused physics worlds

`settle_tileset` now creates a `SettleWorld` only when its placement plan contains actual drop or physics-layer spawns. Analytic-only recipes and empty physics layers skip world construction, terrain collision setup inside that world, simulation and final relaxation. Output assembly uses the existing analytic transforms and preserves the empty physics pose hash. Physical recipes retain their solver settings, batch order and defaults.

The actual world constructor emits a `physics_worlds_created` bake-trace counter. The analytic fixture checks its absence and verifies 48 placements, exact transforms/order, zero simulation time and the compatible report/hash. An existing mixed physical fixture checks one observed world as a positive control. Geometry/placement preparation still runs; this change does not implement DSP/SDF evaluation, new splats or a new material appearance, and it is not an end-to-end generation latency measurement.

Both existing CPU suites are now registered in the native CMake test graph. The standalone Make physics target includes the new trace dependency. Three C compound literals in its existing smoke fixture were changed to equivalent standard C++ initialization so MSVC can compile the previously Make-only test.

## Validation

The new regression was run before the orchestration fix: the bake build succeeded and the test exited 2 with exactly the two expected failures, for analytic-only and empty-layer world creation. See [red checks](analytic-red-checks.json), [output](analytic-red-test-tileset_bake_tests.log) and [source identity](analytic-red-source.json).

The first integration attempt then found the three existing MSVC `C4576` initializer errors in the physics test. That failed build is preserved in [intermediate checks](analytic-green-checks.json). After correcting the syntax, the final sequential native run passed:

| Check | Exit |
| --- | ---: |
| Build `tileset_bake_tests` | 0 |
| Build `tileset_physics_tests` | 0 |
| Run `tileset_bake_tests` | 0 |
| Run `tileset_physics_tests` | 0 |
| Build `matter_editor` | 0 |

[Final commands, durations and log paths](analytic-integration-checks.json) record no monitored source changes during any job. [Bake output](analytic-integration-test-tileset_bake_tests.log), [physics output](analytic-integration-test-tileset_physics_tests.log) and [editor build](analytic-integration-build-matter_editor.log) are retained. [The integration manifest](analytic-integration-source.json) fingerprints 74 source/build inputs and both test executables; [final identity verification](final-verification.json) also identifies the rebuilt editor. These fingerprints describe the dirty working tree, including preserved earlier VT/foliage work, rather than an isolated commit. No GPU scene or material visual acceptance was performed in this slice.

The [documentation check](spec-checks.json) records local link/fence validation and current document hashes. `git diff --check` and comparison against the pre-change file archive complete the review. Only the no-physics-world item in L2b is checked off; the direct GPU evaluator and the terrain/brick visual proofs remain next.
