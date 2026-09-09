# Task 5 Report: RiverFloatLab Visual Playtest

## Status

The Ruling 13 visual-first slice is complete at implementation commit
`d4eeb2a72dfd6e7cacd8d524947c052dbb562c07` (`feat: add RiverFloatLab visual
playtest`) on base `2a715ad1d7a3dcb98f0a4a22748cb0d352d50ed1`.

RiverFloatLab is an explicit, playable scene backed by the accepted shared
RiverHydrology definition. It authors 24 ordinary dynamic bodies and 12 exact
static boulder colliders. The MSVC/PhysX editor built successfully, a cold
scene bake completed with zero errors, five screenshots plus sidecars were
captured, and the exact worktree editor remains open visibly in Play for
review.

This is not the full automated traversal acceptance. Per Ruling 13, the
read-only body trace/diagnostics seam and marker-order gate are explicitly
deferred; no trace or marker result is inferred from the screenshots.

## Authored scene

- The former RiverHydrology definition, network recipe, terrain field, and
  biomes now live in `projects/world_demo/shared-lib/river_hydrology_definition.js`.
  RiverHydrology and RiverFloatLab call the same exported functions.
- RiverFloatLab authors exactly 24 stable-id dynamic recipes: 12 crates and 12
  flattened rafts across upper rapids, boulder wakes, waterfall approach, and
  the first spillway. Every body has `LocalTransform`, `PartInstance`,
  `RigidBody`, `BoxCollider`, and `RiverFloatBody`.
- `reference-crate` is the existing 3 x 3 x 3 m Crate at distance 16 m,
  density 620 kg/m3, with a 2 x 2 x 2 probe lattice.
- `reference-raft` is 10 m behind it at distance 6 m, density 420 kg/m3, with
  a 3 x 2 x 3 probe lattice. Its centred bark visual and collider are exactly
  4.8 x 0.7 x 3.0 m / half-extents `[2.4, 0.35, 1.5]`.
- Both reference bodies use ordinary gravity, continuous collision, and
  disabled sleep. There are no authored velocities, translations, teleports,
  waterfall warps, or respawns.
- Every one of the 12 accepted boulder roots has one explicit static rigid
  body and transformed sphere collider using the exact authored fluid sphere.
  The shared river generator does not spawn gameplay entities implicitly.
- Ruling 14's minimal recipe repair makes instantiation retain the already
  reflected `sleepThreshold`, `enableSleep`, and `continuous` RigidBody fields.

## TDD evidence

### Scene contract RED

Both scene tests were authored before the shared module and RiverFloatLab.
Each native Node command failed meaningfully with an assertion identifying the
missing `projects/world_demo/shared-lib/river_hydrology_definition.js`:

```powershell
node projects/world_demo/tests/river_hydrology_scene_tests.mjs
node projects/world_demo/tests/river_float_lab_scene_tests.mjs
```

### Ruling 14 parser RED

A focused `scene_registry_tests` case was added before the parser repair. The
VS CTest run failed at:

```text
FAIL: instantiation preserves every authored RigidBody transport flag
```

Inspection showed the scene descriptor/reflection advertised all three fields
while `instantiate()` stopped after `gravityScale`. Production code was then
changed only to parse the missing float/bools.

### Runtime part-discovery RED

The first real editor bake found a defect not visible in the pure scene model:

```text
bake error [RiverRaft]: failed to resolve hash for part: RiverRaft
```

A focused Node assertion was added first and failed because the scene-local
part used an ESM `export class` declaration instead of the runtime script
convention. Removing only `export` made the test green; the next real bake
reported `bake 14/0 RiverRaft` and completed with zero errors.

## Verification

Final fresh results before the implementation commit:

```text
node projects/world_demo/tests/river_hydrology_scene_tests.mjs
river_hydrology_scene_tests: PASS

node projects/world_demo/tests/river_float_lab_scene_tests.mjs
river_float_lab_scene_tests: PASS

ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo \
  -R ^scene_registry_tests$ --output-on-failure
1/1 Test #44: scene_registry_tests ... Passed
100% tests passed, 0 tests failed out of 1

git diff --check
exit 0
```

The PhysX-enabled editor was built only through the repository MSVC wrapper:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor `
  -EnablePhysx -PhysxRoot 'D:\PhysX-5.6.1' `
  -CudaRoot 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8'
```

The wrapper reported VS 2022 17.14.7 / MSVC 14.44.35207 / Windows SDK
10.0.26100.0 and:

```text
MATTER_PHYSX_VALIDATE=PASS
PhysX commit=5ca9f472105a90d70d957c243cb0ef36fe251a9f
SDK=5.6.1
CUDA=12.8.61
MATTER_WINDOWS_ARTIFACT=C:\Users\webde\.codex\worktrees\af80\matter-engine-cpp\MatterEditor\build\windows-msvc\editor.exe
```

Artifact audit:

- Size: 34,973,696 bytes
- SHA-256: `1D96A45A9371CCCFEC6F70C54C5F1A8F965F7AA992BB1B864C908D9BA60CAD94`
- No Make, GCC, g++, MinGW, MSYS, UCRT, or collect2 build/launch was used.

The cache target was resolved and checked as exactly
`C:\Users\webde\.codex\worktrees\af80\matter-engine-cpp\projects\world_demo\.cache\RiverFloatLab`
before deletion. No broader cache path was removed.

## Visual evidence and observed behavior

Controlled capture root:

`C:\Users\webde\.codex\worktrees\af80\matter-engine-cpp\MatterEditor\build\baselines\msvc\river-float-visual\20260824T230850112`

The repository driver launched the audited MSVC executable, waited for
`bake.finished`, entered Play, advanced bounded presented-frame intervals, and
exited zero after verifying all five PNG and `.done` pairs:

- `screenshots\upper-rapids.png`
- `screenshots\waterfall-approach.png`
- `screenshots\waterfall-side.png`
- `screenshots\plunge-recovery.png`
- `screenshots\first-spillway.png`

Original-resolution inspection showed the accepted two-section river and its
boulders, with crate and flattened-raft visuals present in the authored upper,
approach, waterfall, and downstream views. The frames were captured while Play
advanced from frame serial 2482 through 2884. These images establish that the
real runtime discovers and renders the scene bodies; they do not establish
reference-body identity or ordered marker crossing.

The final user-observation process remains open visibly:

- PID: `17300`
- Executable: `C:\Users\webde\.codex\worktrees\af80\matter-engine-cpp\MatterEditor\build\windows-msvc\editor.exe`
- World: `RiverFloatLab`
- FIFO: `...\20260824T230850112\live-play.timeline` (contains no `quit`)
- Prelaunch audit: zero competing editor processes and zero
  GCC/g++/collect2/Make-family processes
- Live confirmation: responsive process; `bake ready`; post-Play
  `wait_frames 120` completed at frame serial 2654; fresh
  `screenshots\live-play-observation.png` plus sidecar exists and its UI reads
  `PLAYING`.

## Files in implementation commit

- `MatterEngine3/src/ecs/scene_registry.cpp`
- `MatterEngine3/tests/scene_registry_tests.cpp`
- `docs/agent/qa-cookbook.md`
- `projects/world_demo/shared-lib/river_hydrology_definition.js`
- `projects/world_demo/scenes/RiverHydrology/RiverHydrology.js`
- `projects/world_demo/scenes/RiverFloatLab/RiverFloatLab.js`
- `projects/world_demo/scenes/RiverFloatLab/objects/RiverRaft.js`
- `projects/world_demo/tests/river_hydrology_scene_tests.mjs`
- `projects/world_demo/tests/river_float_lab_scene_tests.mjs`

## Explicitly deferred gate

Ruling 13 defers these original Task 5 deliverables:

- `MatterEngine3/tools/river_float_physics.timeline`
- `tools/run-river-float-physics-proof.ps1`
- `trace/body-reference-crate.csv`
- `trace/body-reference-raft.csv`
- `trace/river-float-diagnostics.json`
- automated assertions that both references cross upper rapids, waterfall,
  first pool, spillway, and lower-entry markers in order while all bodies stay
  finite and float systems remain enabled

The current external control surface does not expose the required per-body
read-only transform/velocity/wet-probe/checksum/disabled rows. No C++ trace
seam was added in this visual-first slice, no CSV or diagnostics data was
fabricated, and no marker/traversal success is claimed. That seam and the
deterministic automated gate should be implemented and reviewed in the resumed
proof slice before Task 5 is called fully accepted.
