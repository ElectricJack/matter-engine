# Scene and object organization — 2026-09-16

## Result

- 62 scenes grouped by subject and proof family; 94 shared objects grouped by family.
- 53 scene-local object modules remain with their owning scenes.
- All 260 moved files accounted for; all 223 authored JS/settings files are byte-identical to the pre-move snapshot, including after the editor review.
- Bare scene/module names, scene-local precedence and cache identities are preserved.
- Engine discovery, module/tileset lookup, live-edit search roots, editor property paths, Assets folder trees, shared-object Workbench lookup, tests and capture paths support the groups.
- [Folder guide](../../../../projects/world_demo/README.md) and [move mapping](moves.json).

## Validation

- Native Windows/MSVC RelWithDebInfo editor, world_definition_tests and eval_world_tests build successfully. An initial linker PDB-write failure was resolved by regenerating the generated editor PDB.
- [Native world_definition_tests](world_definition_tests.log) loads **62/62 scenes** and finishes with only the existing Kreuzenstein ownership failure. New regression checks cover nested scenes, local/shared precedence, single and expanded resolver roots, hidden folders, scene leaves, duplicate names, and unchanged cache identities.
- [Filesystem lookup probe](layout-probe.log): 62 scenes, 94 shared objects and 53 local objects resolve.
- [eval_world_tests](eval_world_tests.log): ALL PASS.
- [Node authoring tests](node-tests.json): **49/52 pass** after fixing one remaining dynamic object path in the structure-catalogue test.
- Editor smoke: ClayBrickMaze opens by its original name, publishes 21 instances / 610 triangles, and renders the weathered maze. No Vulkan validation errors or VUID messages in this run. Folder filtering exposes the grouped brick scenes and objects in the [review screenshot](organized-bricks-ui.png).
- `git diff --check` passes for the changed source/test/tool paths.

## Existing failures

These were not changed as part of the folder organization:

- `castle_paving_tests`: inherited stone relief exceeds a joint cell. [Same assertion on Git HEAD](paving-baseline.log).
- `castle_surface_paving_tests`: source-shape assertion rejects a current wrapper. [Same assertion on Git HEAD](castle_surface_paving_tests-baseline.log).
- `castle_upgraded_scene_tests`: expects one root, receives two. [Same assertion on Git HEAD](castle_upgraded_scene_tests-baseline.log).
- `world_definition_tests`: the existing ownership check finds that shared-lib/kreuzenstein.js names the scene-local KreuzensteinBrick. Git HEAD also contains that shared reference, a scene-local definition, and no shared definition. All scene loads succeed; this ownership assertion remains outstanding.

Historical evidence manifests retain the paths that identified sources during their original runs.
