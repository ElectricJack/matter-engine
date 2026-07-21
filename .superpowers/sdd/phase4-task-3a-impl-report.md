# Phase 4 Task 3A Implementation Report

## Status

DONE_WITH_CONCERNS. The example project authoring and parity slice is complete. Manifest/runtime compatibility deletion and fixture migration are intentionally deferred to the remaining Task 3 slices.

## Bounded migration inventory

- Renamed 21 object modules byte-for-byte from `examples/world_demo/schemas/` to `examples/world_demo/objects/`: BranchGallery, CornellBox, FloorDemo, ForestFloor, Grass, Leaf, LightingGarden, Meadow, Pebble, Rock, RockGallery, StressForest50k, StressForest100k, StressForest200k, StressForest500k, Terrain, Tree, TreeBranch, TreeGallery, Twig, and WorldSector.
- Moved the existing procedural `MeadowWorld.js` byte-for-byte from `schemas/` to `worlds/`.
- Added declarative world definitions for Demo, Meadow, CornellBox, LightingGarden, FloorDemo, RockGallery, StressForest50k, StressForest100k, StressForest200k, and StressForest500k.
- Preserved Meadow and MeadowWorld as separate selectable identities.
- Added `examples/world_demo/shared-lib/.gitkeep`; the project tier exists but engine shared-lib fallback remains available and ordered by the existing loader contract.
- Replaced the generated `WorldData/*.gtex` ignore with the project-local `.cache/` rule.

## Parity encoded

`world_definition_tests.cpp` now checks all 11 worlds for selectable identity, root count and order, `{}` root params, explicit identity transforms, `expand`/`tileset` flags, normalized runtime sun directions, sun/sky colors, and MeadowWorld's `(64, -16, 240)` procedural settings. It also checks the new `objects/` source locations and absence of `schemas/`.

## RED / GREEN evidence

- RED: direct UCRT64 compile/link/run produced 25 failures. Every world failed with `unable to read world source` because `worlds/<name>.js` did not yet exist; accompanying root/light/settings assertions failed as expected.
- GREEN: after migration, a fresh direct UCRT64 compile/link/run of `world_definition_tests` exited 0 with `ALL PASS`.
- `git diff --check` exited 0.
- Move audit: HEAD contained 22 schema files; the new layout contains 21 object files plus the byte-identical MeadowWorld world file. All 22 moved source hashes match their HEAD blobs.

## Files

- Modified: `.gitignore`, `MatterEngine3/tests/world_definition_tests.cpp`
- Renamed: `MatterEngine3/examples/world_demo/schemas/*.js` into `objects/*.js`, except MeadowWorld into `worlds/MeadowWorld.js`
- Added: ten declarative `MatterEngine3/examples/world_demo/worlds/*.js` files and `shared-lib/.gitkeep`

## Concerns

- This is deliberately only Task 3A. Legacy manifests and consumers still refer to the old example layout until the remaining atomic migration slices land; the focused WorldDefinition parity target is the valid gate for this commit.
- No production fallback, manifest parser, provider compatibility, or unrelated fixture source is changed here.
