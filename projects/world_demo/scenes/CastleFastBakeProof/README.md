# CastleFastBakeProof

Opt-in native proof scene; production castle scenes and detailed primitives remain unchanged. Open with `MATTER_WORLD=CastleFastBakeProof` and `MATTER_HIDE_WINDOW=0` using the canonical native editor from `MatterEditor/`. Keep the editor visible for every render test and screenshot run so the process can be watched; do not use a headless or hidden launch. No terrain/world-sector dependencies are required.

This is a structural and shading-contract proof. Surface normal/height/ORM texture baking is not integrated yet, so clean untextured timber and stone envelopes are expected. That status belongs here, not in scene text. Native visual acceptance is pending; Node geometry tests are not screenshots.

The proof helper Parts live at project tier: `projects/world_demo/objects/CastleFastBakeFixtures.js` and `projects/world_demo/objects/CastleFastBakeResidual.js`. Every scene importing the shared proof definition can resolve them; no scene-local duplicate modules remain.

## Contents and bounds

- 107 proper rigid root instances, including 63 repeats of one physical 2×2 m floor flag. No scale fitting.
- All ten wall templates: physical 1/2/4 m solids; 2 m door/window; 15/30/45° miter corners; R4 m arc and curved window. The north doorway retains 1 m width and 2.2 m clear height.
- A post/beam/rafter pavilion uses actual 0.25×0.25 m posts, 0.24×0.28 m beams and 0.14×0.18 m rafters. Physical 1/2/4 m stocks repeat. The low joinery bench displays a 4+2+0.75 m span with an explicit 0.75 m cut residual.
- Closed glass and gold framing occupy the north window; the doorway remains open. Point/spot lights include traced shadows.
- One isolated `CastleStoneSource` on the southeast plinth: seed 0, 0.30×0.14×0.20 m, 3 mm requested sampling, bottom at y=0.9. This is the A3 opt-in source module, not a production brick replacement. `castleFastBakeProofDefinition(materials,{sourceBrick:false})` is available to isolate shell diagnosis if that module is unavailable.
- 98 static collision/player entities. Wall collision hulls use the exact same occupied profiles as visible walls. A single supporting floor collider intentionally bridges millimetre flag bevel seams.

Expected world bounds: **[-9,-0.4,-7] to [9,4.46,7] m**. Default camera: **eye [14,10,16], target [-0.4,1.8,-0.5]**. Source brick closeup: eye [5.4,1.5,6.1], target [4.8,0.98,4.8].

`river-player` starts at [-2.6,0.95,5.8], radius 0.35 m, height 1.8 m. A clear floor route goes around the bench's west end, between the miter studies and through the north doorway:

```text
XZ: (-2.6,5.8) -> (-5.1,5.8) -> (-5.1,3) -> (-2.6,3)
    -> (-2.6,-0.5) -> (-1,-0.5) -> (-1,-4.7)
```

## Capture commands

Create `C:/tmp/castle-fast-proof` first and point `MATTER_CMD_FIFO` at an append-only command file. These commands assume the scene is selected at launch. Use the same native binary and fixed exposure for the raster/RT comparison. Check every `.done` sidecar and command failure.

```text
wait_event bake.finished 300
wait_idle 2 300
render_path raster
cam 14 10 16 -0.4 1.8 -0.5
history_reset
wait_frames 48
shot C:/tmp/castle-fast-proof/overview-raster.png
render_path native_rt
history_reset
wait_frames 64
shot C:/tmp/castle-fast-proof/overview-rt.png
cam -2.8 2.4 5.2 -5 1.4 1
history_reset
wait_frames 48
shot C:/tmp/castle-fast-proof/miter-15.png
cam 2.3 2.4 5.2 0 1.4 1
history_reset
wait_frames 48
shot C:/tmp/castle-fast-proof/miter-30.png
cam 7.4 2.4 5.2 5 1.4 1
history_reset
wait_frames 48
shot C:/tmp/castle-fast-proof/miter-45.png
cam 9.8 2.4 2 6.4 1.6 -1.5
history_reset
wait_frames 48
shot C:/tmp/castle-fast-proof/curved-window.png
cam -1 1.65 -1 -1 1.5 -4.5
history_reset
wait_frames 48
shot C:/tmp/castle-fast-proof/door-clearance.png
cam 2.8 2.1 7 -0.8 1.1 4.5
history_reset
wait_frames 48
shot C:/tmp/castle-fast-proof/timber-cut.png
cam 5.4 1.5 6.1 4.8 0.98 4.8
history_reset
wait_frames 48
shot C:/tmp/castle-fast-proof/source-brick.png
```

For authored walking, `character walk on`, `character intent <world_x> <world_z> 0`, `character intent clear`, and `character status <label>` use the documented control surface. Use fixed-tick stepping/status to verify the route; frame waits alone are not distance assertions. End with `character walk off` and `sim stop` before restoring capture cameras.

## Checks

```bash
node projects/world_demo/tests/castle_surface_shells_tests.mjs
node projects/world_demo/tests/castle_surface_spans_tests.mjs
node projects/world_demo/tests/castle_wall_surfaces_tests.mjs
node projects/world_demo/tests/castle_fast_bake_proof_tests.mjs
```

The scene test checks root matrix orthonormality/proper determinant, exact catalogue membership, source isolation, collision-backed portal width, and conservative capsule-route clearance against every vertically relevant collider AABB. It does not replace native movement, lighting, or screenshot review.
