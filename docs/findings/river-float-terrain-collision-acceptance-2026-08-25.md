# RiverFloatLab terrain-collision acceptance — 2026-08-25

Historical evidence snapshot, retained during the 2026-08-30 integration. Open
items below describe that snapshot, not the current roadmap. See
[the current roadmap](../../ROADMAP.md) and
[the subsequent character-controller acceptance](river-character-controller-integration-acceptance-2026-08-30.md)
for current scope and newer evidence.

## Result and scope

RiverFloatLab reached Ready and exited cleanly in a clean semantic-v3 cold run,
an immediate cache-hit run, and three Play-mode capture runs. The cold and hit
runs installed identical geometry and generation keys, identical sector and mesh
counts, and identical artifact/Box3D byte counts. The measured collision stage
dropped from 10,604.16 ms cold to 367.14 ms on the hit (96.54%); total root bake
time dropped from 37,458 ms to 11,031 ms (70.55%). Seven inspected 1280x720
screenshots provide useful visible-state coverage of the authored river sections.

This evidence was captured at acceptance HEAD
`dd76db91db4c80a675ee4cd0b4da796f6d87d5fd`. The implementation lineage relevant
to the evidence is:

- `416649a254b18443d0fa5994be65a9f090e537cc` — RiverFloatLab collision authorship.
- `75b0851e42bd1f21097d6f3dae8238f3a3dd35d5` — overlaid runtime identity validation.
- `d4c0892ced28f4440ecc63e5f2556f1d30cc0e78` — outward orientation for steep terrain triangles.
- `73f8341214c74ad7ddc61993bb13ec231de925a2` — exact zero-area primitive suppression and semantic-v3 geometry.
- `dd76db91db4c80a675ee4cd0b4da796f6d87d5fd` — exact surviving-mate regression pin.

The final documentation commit is intentionally pending. Independent repeated
Windows artifact-suite stress exposed a separate, pre-existing intermittent
cache-publication race in the Task 2 `MoveFileExW` path. That stress finding is
not conflated with the clean single-process RiverFloatLab cold/cache evidence
below, but it remains an open stability gate at this snapshot.

## MSVC verification

Only the native Windows/MSVC path was used. No Make, GCC, g++, MinGW, MSYS/UCRT,
or collect2 result contributed to acceptance. No editor process was running
before either build/relink or launch. The editor inherited native Windows
`TEMP` and `TMP`, both resolved to
`C:\Users\webde\AppData\Local\Temp`.

Commands:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_engine_cpu_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor
ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -L cpu --output-on-failure
node --experimental-default-type=module projects/world_demo/tests/river_float_lab_scene_tests.mjs
node --experimental-default-type=module projects/world_demo/tests/river_hydrology_scene_tests.mjs
```

Results:

- The CPU aggregate and editor wrapper builds passed. The current acceptance
  relink produced
  `C:\Users\webde\.codex\worktrees\af80\matter-engine-cpp\MatterEditor\build\windows-msvc\editor.exe`,
  29,543,424 bytes, with `LastWriteTimeUtc=2026-08-25 13:38:33Z`.
- The initial Task 6 editor rebuild visibly changed its timestamp from
  `2026-08-25 12:04:18Z` to `2026-08-25 12:14:17Z`; the later relink above put
  the reviewed semantic-v3 fixes in the executable.
- The exact current-tree controller CPU label gate reported 44/44 passed. The
  preceding full post-zero-area gate reported 44/44 passed in 228.58 s, and the
  post-mate focused mesher/artifact/session/seam/physics gate reported 8/8 in
  2.92 s.
- Fresh Node scene results at the evidence HEAD were
  `river_float_lab_scene_tests: PASS` and
  `river_hydrology_scene_tests: PASS`.
- The editor directory contained `editor.exe`, its MSVC debug artifacts,
  `imgui.ini`, and `PhysXGpu_64.dll`; the GCC-runtime DLL scan found zero files.
- The build emitted only the existing baseline MSVC warnings. No new warning
  cleanup was folded into acceptance.

The first sandboxed Node invocation failed before loading either suite with
Windows `EPERM` while resolving `C:\Users\webde`. Both exact commands were then
rerun with native access and passed; that infrastructure-only failure is not a
scene-test failure.

## Exact cache isolation

The cache root resolved to:

`C:\Users\webde\.codex\worktrees\af80\matter-engine-cpp\projects\world_demo\.cache\RiverFloatLab\terrain_collision`

No broad cache root was deleted. Before the final semantic-v3 cold run, only the
exact partial `v1` directory was moved to the recoverable timestamped sibling:

`C:\Users\webde\.codex\worktrees\af80\matter-engine-cpp\projects\world_demo\.cache\RiverFloatLab\terrain_collision\v1.v2-partial-20260825T133843Z`

That backup remains intact with 15 MTCT files, no MTCM manifest, and 4,161,024
bytes. An earlier pre-v2 diagnostic backup also remains separately intact at
`v1.pre-v2-partial-20260825T130412Z` with 5 MTCT files, no manifest, and 899,796
bytes. Neither stale semantic-version backup was restored over the accepted
cache, so both remain recoverable without affecting the current `v1`.

The cold run recreated canonical `v1` with 42 MTCT files plus one MTCM manifest,
43 files and 14,359,232 physical bytes in total. The installed artifact total is
14,355,816 bytes; the 3,416-byte difference is exactly the generation manifest
`generations/718bb26177c7e3d4.mtcm`. The newest cache-file timestamp was
`2026-08-25 13:39:06.1340562Z` after cold creation and remained unchanged through
the immediate hit and all later Play launches.

## Cold and immediate cache-hit runs

The ignored wait timeline was
`.codex-tmp/terrain-collision-acceptance-2026-08-25/cache-acceptance.timeline`.
Exact launch commands, from the repository root, were:

```powershell
py -3 MatterEngine3/tools/drive.py --world RiverFloatLab --timeline .codex-tmp/terrain-collision-acceptance-2026-08-25/cache-acceptance.timeline --out-dir .codex-tmp/terrain-collision-acceptance-2026-08-25/cold-dd76db91 --editor MatterEditor/build/windows-msvc/editor.exe --timeout 1200 --hide-ui --env MATTER_VSYNC=0
py -3 MatterEngine3/tools/drive.py --world RiverFloatLab --timeline .codex-tmp/terrain-collision-acceptance-2026-08-25/cache-acceptance.timeline --out-dir .codex-tmp/terrain-collision-acceptance-2026-08-25/cache-hit-dd76db91 --editor MatterEditor/build/windows-msvc/editor.exe --timeout 1200 --hide-ui --env MATTER_VSYNC=0
```

Evidence logs:

- [cold log](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/cold-dd76db91/log.txt)
  (`C:\Users\webde\.codex\worktrees\af80\matter-engine-cpp\.codex-tmp\terrain-collision-acceptance-2026-08-25\cold-dd76db91\log.txt`)
- [cache-hit log](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/cache-hit-dd76db91/log.txt)
  (`C:\Users\webde\.codex\worktrees\af80\matter-engine-cpp\.codex-tmp\terrain-collision-acceptance-2026-08-25\cache-hit-dd76db91\log.txt`)

| Measurement | Clean cold | Immediate cache hit |
| --- | ---: | ---: |
| Generation/install key | `718bb26177c7e3d4` | `718bb26177c7e3d4` |
| Geometry key | `382bf7bdfe5930ca` | `382bf7bdfe5930ca` |
| Cell size / rung | 0.500 / 2 | 0.500 / 2 |
| Regions / required sectors | 1 / 42 | 1 / 42 |
| Nonempty / empty sectors | 20 / 22 | 20 / 22 |
| Triangles / vertices | 793,365 / 402,449 | 793,365 / 402,449 |
| Artifact / retained Box3D bytes | 14,355,816 / 32,105,536 | 14,355,816 / 32,105,536 |
| Build time | 9,517.24 ms | 0.00 ms |
| Cache-load time | 0.00 ms | 67.23 ms |
| Validation time | 859.38 ms | 75.48 ms |
| Install time | 227.54 ms | 224.43 ms |
| Collision sum above | 10,604.16 ms | 367.14 ms |
| Root bake install / world / publish | 324 / 32,337 / 2,292 ms | 321 / 6,078 / 2,229 ms |
| Root bake total | 37,458 ms | 11,031 ms |
| Completion | Ready, `bake finished (0 errors)`, exit 0 | Ready, `bake finished (0 errors)`, exit 0 |

Each log has zero `[terrain-collision] build-failed`, zero `bake error`, and zero
fatal/assert lines. The editor printed `bake finished (0 errors)` and
`viewer: bake ready`; `drive.py` reported editor exit 0 and success for both runs.

The runtime install summary does not expose the requested numeric `built_tiles`
or `cache_hit_tiles` fields, so no such counters are invented here. The hit is
instead directly evidenced by `build=0.00ms`, nonzero `cache=67.23ms`, all 42
required MTCT files remaining present, and no cache file timestamp changing after
the cold run. Together with identical keys/counts/bytes, that demonstrates no
tile rebuild and reuse of every required cached sector for this run, while the
absence of explicit counters remains an observability limitation.

The logs retain existing renderer/streaming warnings such as empty draw-record
windows, missing optional tracer `load_v2` parts, and part variants producing no
LOD geometry. They did not produce a terrain-collision failure, a bake error, or
prevent Ready, and are not reclassified as terrain-collision errors.

## Play-mode visual evidence

The capture timeline was derived from
`MatterEngine3/tools/river_hydrology_sections.timeline`. Every run waited for
`bake.finished`, entered Play, advanced 300 frames, paused, and then captured
clean UI-hidden views. Exact primary command:

```powershell
py -3 MatterEngine3/tools/drive.py --world RiverFloatLab --timeline .codex-tmp/terrain-collision-acceptance-2026-08-25/play-dd76db91.timeline --out-dir .codex-tmp/terrain-collision-acceptance-2026-08-25/play-dd76db91 --editor MatterEditor/build/windows-msvc/editor.exe --timeout 1200 --hide-ui
```

The corresponding [Play log](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/play-dd76db91/log.txt)
records the stable accepted keys/counts, Ready with zero bake errors, completion
of 300 Play frames, and all eight primary writes. Two targeted retake timelines
used the same command shape with `play-retake-dd76db91.timeline` and
`play-retake2-dd76db91.timeline`; both exited 0 and verified all requested PNGs.

All 15 produced PNGs were opened and inspected at their original 1280x720
resolution. These seven are the useful acceptance set:

| Coverage | Screenshot | Honest visible observation |
| --- | --- | --- |
| Upper crates and raft | [01-upper-crates-raft.png](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/play-dd76db91/01-upper-crates-raft.png) | Several smaller white crates and tan rafts are visibly within the upper water channel; the closest bodies are visually resting against the bed/bank at the captured instant. |
| Boulder curve | [03-boulder-curve.png](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/play-dd76db91/03-boulder-curve.png) | A large dark boulder occupies the curved channel with crates/rafts visibly distributed around it. The still establishes the body/boulder relationship, not a time-resolved deflection event. |
| Waterfall side | [10-waterfall-side.png](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/play-retake-dd76db91/10-waterfall-side.png) | The fall, approach channel, first pool, surrounding terrain, and bodies near the drop are visible together from the authored side pose. |
| First pool | [05-waterfall-pool.png](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/play-dd76db91/05-waterfall-pool.png) | The broad pool and enclosing terrain are unobstructed, with bodies remaining inside the visible channel/pool at capture time. |
| Canonical x=64 sector area | [15-sector-boundary-x64-overhead.png](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/play-retake2-dd76db91/15-sector-boundary-x64-overhead.png) | The camera is centered on canonical x=64 and shows continuous water/terrain plus multiple crates and rafts across the section. The boundary plane is not rendered, and the still is not claimed as per-body traversal telemetry. |
| Spillway | [06-spillway.png](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/play-dd76db91/06-spillway.png) | The spillway/pool, enclosing terrain, and a raft at the lower terrain edge are clearly visible; no artificial domain wall is visible. |
| Lower overview | [11-lower-spillway-wide.png](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/play-retake-dd76db91/11-lower-spillway-wide.png) | A wide view shows the continuous lower river/spillway and visible bodies without a body visibly below the terrain at the sampled instant. |

Each linked file also has the absolute prefix
`C:\Users\webde\.codex\worktrees\af80\matter-engine-cpp\`; the table links are
the corresponding repository-relative paths. File sizes are 1,419,349;
1,414,887; 1,584,960; 1,588,760; 1,514,991; 1,551,889; and 1,573,678 bytes in
table order.

Rejected captures remain preserved as evidence rather than being deleted:
primary `02` was bank-occluded, `07` was a marginal lower view, and `08` exposed
an unhelpful distant terrain void; retakes `09` and `12` had the same occlusion/
void problems. Useful retakes `10`, `11`, and `15` replace those weak views.

## Failures encountered and resolved before GREEN

Three real production-shaped blockers were recorded rather than hidden:

1. At `416649a2`, the first discovery run failed with
   `terrain collision source field hash does not match the runtime`.
   [Log](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/cache-discovery/log.txt).
   Commit `75b0851e` fixed overlaid identity validation.
2. At `75b0851e`, the next run failed with
   `terrain collision mesher winding opposes its normals`.
   [Log](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/post-fix-discovery/log.txt).
   Commit `d4c0892c` oriented steep ordinary triangles outward.
3. At `d4c0892c`, the clean semantic-v2 run failed with
   `terrain collision mesher emitted a zero-area triangle`.
   [Log](../../.codex-tmp/terrain-collision-acceptance-2026-08-25/cold-d4c0892c/log.txt).
   Commits `73f83412` and `dd76db91` suppressed only exact collapsed primitives,
   bumped the mesher semantic version, and pinned the surviving nonzero mate.

Each fix was developed under a dedicated failing MSVC regression, independently
reviewed, and followed by focused/full CPU gates. The final semantic-v3 cold run
encountered no further geometry blocker.

## Required limitations and remaining risk

- The water mesh remains static. Shader-only water motion is visual and does not
  imply moving collision geometry or fluid-body coupling.
- Generated terrain collision is bounded to the authored union of RiverFloatLab
  regions. There is deliberately no hidden domain-boundary wall.
- River boulders remain explicit authored collision shapes; they are not folded
  into the generated terrain collision mesh.
- Automated per-body river-traversal CSV telemetry remains deferred. The PNGs
  show visible state after 300 Play frames, not fabricated body histories,
  crossing IDs, impulses, or collision-event telemetry.
- Exact `built_tiles`/`cache_hit_tiles` numeric fields are not emitted by the
  installed summary; cache reuse is evidenced operationally as described above.
- The separate Windows cache-publication stress race remains open at this
  snapshot. The final report commit and feature-complete/stability claim must
  wait for its reviewed fix and fresh final-tree verification.
