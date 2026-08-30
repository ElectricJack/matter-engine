# RiverFloatLab character integration acceptance — 2026-08-30

Accepted: the bounded fixed-step character-controller integration in
`RiverFloatLab`, against its existing installed terrain collision. Two fresh
native MSVC/PhysX editor processes passed the identical scripted sequence;
current screenshots were inspected by the implementer and coordinator. This
does not accept the unfinished waterfall/optics or a complete river playtest.

## Scope and provenance

Task 4 appends one root `river-player` at `[48,126,31]` with the default capsule
controller and no authored intent, rigid body, collider, float, part, or
streaming owner. Existing hydrology, 24 dynamic bodies, reference crate/raft,
boulders, and the collision union are unchanged by this task. Tasks 1–3 supply
the reviewed runtime, scene integration, and editor control surface.

Workspace: `C:/Users/webde/.codex/worktrees/af80/matter-engine-cpp`.
Launch HEAD: `7f1620e4aaf9c85eb4a5af432f64c68a3e59b96c`, plus the dirty-file
inventory and 96 source-script hashes in [provenance.json](../../build/qa/rcc2/provenance.json).
This was deliberately the current working-project candidate, not a clean-HEAD
claim. Unrelated river/UI/shader/provider/shared-DSL edits were preserved.
The coordinator's later docs-only `99e05761` interpolation finding is separate
and did not change the tested source or binary hashes.

Native toolchain: VS2022 Community v143 14.44.35207, Windows SDK 10.0.26100.0,
Vulkan SDK 1.4.357.0, Python 3.13.14. Feature check:
`MATTER_ENABLE_PHYSX:BOOL=ON`; root `D:/PhysX-5.6.1`; SDK commit
`5ca9f472105a90d70d957c243cb0ef36fe251a9f`, PhysX 5.6.1, CUDA 12.8.61.
All builds/preflights explicitly supplied `-EnablePhysx -PhysxRoot D:/PhysX-5.6.1`.

Source editor: `MatterEditor/build/windows-msvc/editor.exe`; isolated copy:
`build/qa/rcc2/f/bin/editor.exe`. Pinned GPU source:
`D:/PhysX-5.6.1/physx/bin/win.x86_64.vc143.mt/release/PhysXGpu_64.dll`.
Editor/GPU source-copy hashes were checked before launch; the final read-only
audit reconfirmed both and compared all five copied DLLs with their sources:

| Artifact | SHA256 |
| --- | --- |
| editor.exe | `E52F65DC855D5858A2F0731C5D32D01E2508C2A540251AD08BD04A5C997784E8` |
| PhysX_64.dll | `8EB3F8DE17419E4039211075E80E5FBD8A9742AF099FD9F02F37636D6C43E299` |
| PhysXCommon_64.dll | `0421434E3384F37D52DE53E3386A3CFD118AA07010E6039FE1472C349B1010A9` |
| PhysXCooking_64.dll | `D7F7ED61366BBD9A70DA2E80FD5B1C66F6A1639B3932D96F996CA972B778E1F0` |
| PhysXFoundation_64.dll | `F852C42A63BFA223FF179454BE183F743B52F06D0C3FBECE59E7A372D8181491` |
| PhysXGpu_64.dll | `23C6490D9D4C919527A64F0D6D0A2FCE4E5E3D6F843818FC04556B4273C27400` |

The runner copies the project and its existing cache, engine shared library,
driver, editor, and runtime DLLs into a new real fixture; no links, cache
invalidation, or source-cache deletion. The source cache remained 482 files /
3,392,034,690 bytes before, during, and after acceptance. Child `MATTER_*`
overrides are cleared; PATH adds only fixture/bin to inherited native SDK/OS
paths, not source build directories. The driver, editor preferences, command
file, project discovery, cache writes, logs, and screenshots use the fixture.
The unused default issue-report directory still resolves to the source
repository; no issue capture was requested or triggered.

## Executed gates

All commands below exited 0 unless explicitly identified as retained RED or
superseded evidence. Native Python/Node discovery and GUI runs used host
execution; a sandbox Node `EPERM lstat C:\Users\webde` was environmental,
not the test's RED proof.

```powershell
./tools/build-windows.ps1 -EnablePhysx -PhysxRoot 'D:/PhysX-5.6.1' -PreflightOnly
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -PhysxRoot 'D:/PhysX-5.6.1' -Target matter_character_integration_tests
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R '^(character_controller_tests|character_walk_controller_tests|ecs_tests|physics_tests|scene_registry_tests|entity_recipe_tests|scene_tracker_tests|simulation_control_tests|terrain_collision_(definition|artifact|physics|session)_tests|river_float_system_tests|viewer_logic_tests)$' --output-on-failure --no-tests=error
node --experimental-default-type=module projects/world_demo/tests/river_float_lab_scene_tests.mjs
node --experimental-default-type=module projects/world_demo/tests/river_hydrology_scene_tests.mjs
& 'C:/Users/webde/AppData/Local/Programs/Python/Launcher/py.exe' -m unittest discover -s MatterEngine3/tools/tests -p character_controller_acceptance_tests.py
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -PhysxRoot 'D:/PhysX-5.6.1' -Target matter_editor
./MatterEngine3/tools/run_character_controller_acceptance.ps1 -OutputDir build/qa/rcc2
& 'C:/Users/webde/AppData/Local/Programs/Python/Launcher/py.exe' MatterEngine3/tools/character_controller_acceptance.py --run build/qa/rcc2/run1 --run build/qa/rcc2/run2 --output build/qa/rcc2/implementer-final-recheck.json
```

- Native focused suite: 14/14 PASS, 106.66 s. Covers controller/static-filter
  body noninterference, scene/recipe/tracker, pause/Stop, terrain artifact and
  session installation, float snapshot, and viewer behavior. Editor freshness
  build passed (up-to-date Ninja graph), with no later non-PhysX configuration.
- Node: player invariant RED (`actual 0`, `expected 1`) before the recipe;
  both scene suites GREEN afterward and again after the real runs.
- Checker: complete valid synthetic trace rejected by stub (RED); initial
  implementation GREEN 14/14. Masked scene-id regression RED before correction;
  explicit installed/nonempty collision regression RED before strengthening.
  Final checker suite 15/15 PASS in 5.334 s; PowerShell parser PASS.
- Real checker: [summary](../../build/qa/rcc2/summary.json) PASS; independent
  coordinator [recheck](../../build/qa/rcc2/root-recheck.json) and final
  implementer [recheck](../../build/qa/rcc2/implementer-final-recheck.json) PASS.
- Separate coordinator-requested freshness repair, with no editor running:
  `./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -PhysxRoot 'D:/PhysX-5.6.1' -Target shader_source_tests`,
  followed by the same CTest executable/build/config and
  `-R '^shader_source_tests$' --output-on-failure --no-tests=error`:
  build PASS, 1/1 PASS in 0.15 s, no source edits. This is companion interpolation
  verification, not additional controller runtime evidence.

## Matched native acceptance

Both processes used normal rendering, Vulkan validation, 1280×720 output,
literal `step` / `wait_frames 1` pairs, and the same authored drop spawn.
The accepted camera-only presentation adjustment before Edit was eye
`[48,91,31]`, target `[16,86,7]`; walking's post-tick eye still follows the
capsule. After Stop, the separate river view uses target `[31,69,7]` and a
120-frame Play/Pause segment. No physics, intent, wait, count, or union
predicate was weakened.

| Evidence | Run 1 | Run 2 |
| --- | --- | --- |
| Log / driver | [log](../../build/qa/rcc2/run1/log.txt) / [driver](../../build/qa/rcc2/run1/drive.log) | [log](../../build/qa/rcc2/run2/log.txt) / [driver](../../build/qa/rcc2/run2/drive.log) |
| Timeline / metadata | [timeline](../../build/qa/rcc2/run1/timeline.txt) / [metadata](../../build/qa/rcc2/run1/run.json) | [timeline](../../build/qa/rcc2/run2/timeline.txt) / [metadata](../../build/qa/rcc2/run2/run.json) |
| Editor / driver exit | 0 / 0 | 0 / 0 |
| Current capture pairs / labels | 9 / 12 | 9 / 12 |
| Total wall duration | 687.469 s | 178.962 s |
| Successful idle settle | 127.2 s | 127.1 s |
| Collision cache / validation / install | 259.17 / 85.07 / 272.24 ms | 71.45 / 75.00 / 284.80 ms |

Each run explicitly installed generation `718bb26177c7e3d4`, geometry
`382bf7bdfe5930ca`, cell 0.500, rung 2, one region, 42 sectors: 20 nonempty /
22 empty, 793,365 triangles, 402,449 vertices, 14,355,816 artifact bytes,
32,105,536 Box3D bytes, zero tile-build time. Ready, `bake.finished`, and
`idle: settled after` precede character samples. **Every required wait
completed without timeout**, including `wait_idle`; neither run contains a
timeout marker. The existing source cache is copied once, and the second
fresh process can reuse the first process's fixture cache: this is not a
clean cold-cache acceptance claim.

Both runs returned these exact states (coordinates in metres):

| Label | Tick | Position | Observation |
| --- | ---: | --- | --- |
| edit | 0 | `[48,126,31]` | Edit; zero velocity/counters, walking off |
| grounded | 300 | `[48,86.869606,31]` | Grounded on installed collision |
| walk | 360 | `[52.5000458,86.9705124,31]` | +4.5000458 m X; VX 4.5 |
| sprint | 390 | `[55.8750229,86.9930801,31]` | +3.3749771 m X; VX 6.75 |
| jump_base | 510 | `[55.8750229,86.9930801,31]` | Grounded; zero velocity |
| latched / paused | 510 | `[55.8750229,86.9930801,31]` | Pending jump retained; no drift/consumption |
| jump | 511 | `[55.8750229,87.0736847,31]` | Airborne; VY 4.83650017; consumed/started 1/1 |
| landed | 631 | `[55.8750229,86.9930801,31]` | Grounded; consumed/started still 1/1 |
| resumed / paused_again | 662 | `[55.8750229,86.9930801,31]` | Play advanced; Pause then held state |
| stopped | 0 | `[48,126,31]` | Initial Edit snapshot; zero intent/latch/counters; walking off |

The eight deterministic samples have maximum absolute position/velocity
component delta **0 m**, against the 0.001 m limit; counters/flags match
exactly. The wall-time Resume samples happened to match at tick 662, but only
relational advancement and subsequent paused stability are asserted there.
All positions are inside the unchanged authored collision union. Flat-ground
speed/sprint ratio and filter semantics are established by CPU tests, not
inferred as general bank behavior from one path.

## Current visual inspection and limits

Inspected grounded, walk, jump, landed, stopped, and both river views from
each final process, together with Edit and telemetry. Terrain and horizon
remain visible; the camera displacement is consistent with the reported
movement/jump, with no visible disappearing ground, penetration, or teleport
on this short bank path. The reference crate/raft configuration is present
after Stop; the separate Play view shows movement/tilt/contact in the channel,
with no new obvious regression in this slice.

| Capture | Run 1 | Run 2 |
| --- | --- | --- |
| Grounded | [PNG](../../build/qa/rcc2/run1/grounded.png) | [PNG](../../build/qa/rcc2/run2/grounded.png) |
| Walk | [PNG](../../build/qa/rcc2/run1/walk.png) | [PNG](../../build/qa/rcc2/run2/walk.png) |
| Jump | [PNG](../../build/qa/rcc2/run1/jump.png) | [PNG](../../build/qa/rcc2/run2/jump.png) |
| Landed | [PNG](../../build/qa/rcc2/run1/landed.png) | [PNG](../../build/qa/rcc2/run2/landed.png) |
| Stopped | [PNG](../../build/qa/rcc2/run1/stopped.png) | [PNG](../../build/qa/rcc2/run2/stopped.png) |
| River restored | [PNG](../../build/qa/rcc2/run1/river-restored.png) | [PNG](../../build/qa/rcc2/run2/river-restored.png) |
| River Play | [PNG](../../build/qa/rcc2/run1/river-play.png) | [PNG](../../build/qa/rcc2/run2/river-play.png) |

A thin gold outline in run 1 disappears after Stop and is absent in run 2.
Its color/lifecycle are consistent with the editor's selected-entity bounds
overlay (`selection_outline.cpp`; overlays can render with UI hidden and
Stop clears selection), but the selected object was not logged. It remains
an unclassified/likely selection overlay, not physical controller evidence.
No renderer changes were made.

Retained host-loader diagnostics in both logs:

```text
[vk] Vulkan loader/general ERROR: loader_get_json: Failed to open JSON file D:\Epic Games\Epic Games\Launcher\Portal\Extras\Overlay\EOSOverlayVkLayer-Win32.json
[vk] Vulkan loader/general ERROR: loader_get_json: Failed to open JSON file D:\Epic Games\Epic Games\Launcher\Portal\Extras\Overlay\EOSOverlayVkLayer-Win64.json
```

These are general host layer-registration messages, not engine/Vulkan
validation errors. Duplicate overlay, unused shader-output (explicitly “not
invalid”), and streaming `load_v2 miss` warnings also remain in the raw logs.
No host registration was changed. There were no validation-error/VUID-error,
fatal, character-command failure, PhysX-disabled, or water/terrain fallback
markers. This is not a warning-free-host claim.

Whole-river traversal, buoyancy endurance, swimming, craft riding/control,
manual GLFW keyboard/focus/ImGui interaction, and general terrain obstacle
coverage are **not proven** by this scripted slice. Waterfall/plunge appearance
is visibly unfinished; broader raster-water optics and cold-bake/performance
acceptance remain separate open gates.

## Retained first-attempt evidence

`build/qa/rcc1/run1` completed with editor/driver exit 0 and all nine captures.
Its initial checker rejected the raw-FNV oracle: production masks bit 63, so
`river-player` has scene id `1317415599531854025`, not
`10540787636386629833`. The [original rejection](../../build/qa/rcc1/checker-original-rejection.log)
is retained. A RED regression preceded the oracle correction; the original
run subsequently passed the corrected status/install predicates.

Its downward camera made near-bank screenshots poor presentation proof.
With coordinator approval, only superseded run 2's exact owned editor
PID 46796 was stopped after read-only executable-path verification under
`build/qa/rcc1/f/bin`. Its [failure record](../../build/qa/rcc1/failure.json),
driver logs, and incomplete artifacts remain. This was deliberate cancellation,
not a product failure or timeout. `rcc2` then provided the complete matched pair
with the approved camera framing. No rejected or superseded evidence was deleted.
