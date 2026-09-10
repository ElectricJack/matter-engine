# QA Cookbook

Copy-paste recipes for driving the engine/editor from a shell. Each recipe is a
fenced command plus what to expect. **Build-command detail is intentionally
minimal here — defer to the root `CLAUDE.md`** for the current toolchain
incantation; it is more likely to be current than a second copy of it pasted
into this file, and the build commands are known to be in flux.

See `docs/agent/control-surface.md` for the full FIFO verb table and env var
reference these recipes exercise, and `docs/agent/issue-system.md` for the
issue-report/replay recipes (5).

## 1. Configure and build the MSVC engine graph

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_engine_headless
```

The wrapper selects the pinned Visual Studio 2022 x64 environment and the
repository's CMake/Ninja preset. From WSL, use
`./tools/build-windows-from-wsl.sh RelWithDebInfo matter_engine_headless`.
**Check the exit code, not the output for the string "error"** — see the Traps
section.

## 2. Build the Windows editor

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor
```

Expect `MatterEditor/build/windows-msvc/editor.exe`. Requires `editor.exe` not
to already be running (file lock). The former
`make -C MatterEditor windows` path is MinGW rollback-only.

## 3. One-shot screenshot of a world

```bash
cd MatterEditor
MATTER_WORLD=StreamMountain \
MATTER_CAM="0,140,0,0,60,60" \
MATTER_SCREENSHOT="C:/tmp/shot.png" \
MATTER_SCREENSHOT_SETTLE=90 \
TMP="C:/Users/webde/AppData/Local/Temp" \
TEMP="C:/Users/webde/AppData/Local/Temp" \
  ./build/windows-msvc/editor.exe
```

Expect the process to bake, settle 90 frames (streamed worlds need far more
than the 3-frame default — see `docs/agent/control-surface.md` §c), write
`C:/tmp/shot.png`, and **exit on its own** (a plain `MATTER_SCREENSHOT` run is
capture-then-quit; no `quit` command needed).

## 4. FIFO-driven multi-shot session

`MatterEngine3/tools/drive.py` is the current, preferred way to drive a
multi-shot timeline: write the command lines to a file, hand it to `drive.py`,
and it launches the editor, tees the log, and verifies every screenshot the
timeline promised — no hand-rolled `sleep`-polling required. `shot`/`shot_now`
are **blocking** (§ Timeline semantics, `docs/agent/control-surface.md`): a
`shot` line does not release the next line in the timeline until its PNG
*and* `.done` sidecar are actually written, so **no trailing `wait_frames` is
needed after a `shot`** — that used to be necessary and is not any more.
`wait_idle <seconds>` replaces the old `sleep <n>` guess for "let LOD/batches
settle at the new view": it releases on an actual settle signal
(`resident_sectors` steady + bake ready), not a fixed wall-clock guess.

```bash
cat > /tmp/shots.txt <<'EOF'
cam 20 760 350 0 420 0
wait_idle 2
stats current-cost
shot C:/tmp/current-cost.png
quit
EOF

python MatterEngine3/tools/drive.py --world meadow --timeline /tmp/shots.txt \
    --out-dir C:/tmp/drive-out \
    --editor MatterEditor/build/windows-msvc/editor.exe
```

Expect exit 0, `C:/tmp/current-cost.png` + `current-cost.png.done` on disk,
and a `STATS,current-cost,...` line in `C:/tmp/drive-out/log.txt` (`drive.py`
tees the editor's full stdout/stderr there as well as to its own stdout).
`drive.py` unlinks any stale PNG/`.done` pair left over from a previous run
into the same `--out-dir` *before* launching, so a crash that fails to
actually produce a shot cannot false-pass by reading old files, and it warns
(non-fatally) if a timeline has no `quit` line. See `docs/agent/control-
surface.md`'s `drive.py` section for the full flag/behavior reference, and
its FIFO verb table for every other timeline verb (`wait_frames`,
`wait_event`, `world`, `set`/`get`, ...). `MatterEngine3/tools/viewer_shots.sh`
is an older, still-supported example of driving the raw FIFO by hand (append
lines, poll for `.done`) for cases `drive.py` doesn't cover — read it if you
need something outside a plain timeline. On Windows, `MATTER_CMD_FIFO` is a
polled plain file, not a real FIFO — `mkfifo`/blocking-open semantics only
apply on Linux; `drive.py` and `viewer_shots.sh` both handle the platform
difference for you.

## 4a. Structured agent request

Launch the editor with both `MATTER_CMD_FIFO=<commands.txt>` and
`MATTER_AGENT_RESULT_FILE=<results.jsonl>`, then issue a discoverable request:

```bash
python tools/matter_agent.py agent.commands \
  --cmd-file C:/tmp/matter-commands.txt \
  --result-file C:/tmp/matter-results.jsonl
```

Expect exactly one terminal JSON object on stdout. Client diagnostics go to
stderr, editor logs stay on its stdout/stderr, and the shared result file is
JSONL only. See `docs/agent/agent-protocol.md` for help/schema calls, expected
revision guards, status codes and bounds.

## 4b. Native Windows selection, capture, and rebake probe

Build the editor through the canonical MSVC graph first; do not substitute a
MinGW build for this acceptance.

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
```

Then use the `PhysicsPlayground` sequence in
[`control-surface.md`](control-surface.md#native-windows-selection-acceptance).
It is the reproducible native fixture with both dynamic authored entities and
baked roots.  The 2026-09-09 run produced 10 entity rows and 4 baked-root rows,
captured `C:/tmp/matter-agent-physics/selection.png` plus its `.done` marker,
and selected `{"kind":"entity","id":"637278442326563570"}` at the
capture-derived viewport coordinate `(227,218)` with
`geometry.source:"gpu_identity"`.  It also verified that `selection.remove`
leaves `selection.list` empty and that `view.focus` changes the camera without
changing selection.

Use a bad coordinate to assert the request/result failure contract.  The
client must exit 1, while its sole stdout record has `ok:false` and
`code:"invalid_input"`; do not parse editor logs for this result.

```bash
python3 tools/matter_agent.py viewport.pick \
  --cmd-file /mnt/c/tmp/matter-agent-physics/commands.txt \
  --result-file /mnt/c/tmp/matter-agent-physics/results.jsonl \
  --args '{"x":-1,"y":0}'
```

Exercise timeline sequencing separately through the existing FIFO grammar:

```bash
printf 'wait_idle 1 30\n' >> /mnt/c/tmp/matter-agent-physics/commands.txt
printf 'world PhysicsPlayground\nwait_event bake.finished 30\nwait_idle 1 30\n' \
  >> /mnt/c/tmp/matter-agent-physics/commands.txt
```

The second sequence can print `event: bake.finished aborted (session changed)`
when the world replacement supersedes the old session; this is the expected
asynchronous outcome, and the following idle wait still settles.  Do not
interpret an event timeout/abort as a client-command exit failure.

For an agent that needs the usual inspect → select → edit/regenerate → wait →
capture loop, save the target once and use a bounded batch.  This is inspired
by scene/object inspection plus viewport capture workflows, but keeps Matter
Engine's procedural changes explicit and non-transactional:

```bash
python3 tools/matter_agent.py session set physics \
  --cmd-file /mnt/c/tmp/matter-agent-physics/commands.txt \
  --result-file /mnt/c/tmp/matter-agent-physics/results.jsonl

python3 tools/matter_agent.py --session physics --batch /tmp/physics-plan.json \
  | jq .
```

`/tmp/physics-plan.json` is a version-1 `steps` document as shown in
[`agent-protocol.md`](agent-protocol.md#persistent-targets-and-bounded-batches).
Start with `scene.list_objects`, `scene.get_object`, or `selection.list` rather
than parsing editor text; then use `procedural.update` only for its documented
session override, wait for its returned `job_id` with `job.wait`, and capture
through `viewport.capture`.  The client sends each step only after the prior
terminal result.  Default `stop_on_error:true` preserves the completed prefix
in one JSON result and exits 3; no rollback is attempted.  Do source edits to
the procedural model through the ordinary repository/source-control workflow.

**Current limitation.** On the acceptance revision, appending `reload` while
the native `PhysicsPlayground` session is live reaches a renderer fatal error,
`dynamic instance part bucket is outside the active part table`, before a
positive rebake-invalidates-selection result can be observed.  The editor
auto-filed issue `53c8d7e8-2ba5-502d-d94f-3789f3db174a`; the implementation
follow-up is AQ task `nimble-dune.8`.  Keep the focused parser/selection/capture
tests below as the regression guard, but treat a live reload result as blocked
until that renderer defect is fixed.

## 4c. Bounded regeneration jobs

Reloading or rerolling a world used to leave an agent guessing: `reload` is a
FIFO verb with no receipt, so completion had to be inferred from a sleep or
from `wait_event bake.finished`, which cannot tell you WHICH reload finished.
`job.*` gives the same work an id, a state and a bounded wait.

```bash
# queue a seeded reroll; this returns immediately with state "accepted"
python3 tools/matter_agent.py job.start \
  --cmd-file /mnt/c/tmp/matter-agent-physics/commands.txt \
  --result-file /mnt/c/tmp/matter-agent-physics/results.jsonl \
  --args '{"operation":"regenerate","seed":"12345"}'

# wait for it, bounded by the request timeout (30 s max per call)
python3 tools/matter_agent.py job.wait \
  --cmd-file /mnt/c/tmp/matter-agent-physics/commands.txt \
  --result-file /mnt/c/tmp/matter-agent-physics/results.jsonl \
  --args '{"job_id":"1"}' --timeout 30
```

Read the result, not the exit status alone:

- `ok` with `job.state:"completed"` is the only success. A `timeout` record
  carries `timed_out:true`, `completed:false` and the job's last observed
  state — call `job.wait` again for a bake longer than 30 s rather than
  treating the timeout as a failure OR as a completion.
- `execution_failure` with `job.state:"superseded"` means something else
  restarted the world (another `job.start`, the toolbar Reload, the `reload`
  FIFO verb, or a world switch). It is not an error in your request.
- `execution_failure` with `job.state:"failed"` carries `job.diagnostics`, each
  with the engine's `code` and a parsed `source.file` / `source.line` when the
  script error had one.

For a determinism check, run two `regenerate` jobs with the same seed and
compare `job.result.content_digest` — it is FNV-1a over the sorted published
part-graph roots, so it does not depend on iteration order or on what a frame
happened to draw. A different seed changes it only for a world whose parts
actually declare `worldSeed`. A world-kind (streamed) world publishes no graph
roots at all, so there the digest reports `available:false` with that reason
and determinism has to be checked through `viewport.capture` +
`MatterEngine3/tools/img_diff.py`.

`job.cancel` only truly cancels a job that is still `accepted`. A running one
answers `unsupported_command` with `cancel.supported:false` — `WorldSession`
has no cancel, and the alternative it names is supersession by a newer
`job.start`. In practice a client that calls `job.start` and then `job.cancel`
as two separate requests will nearly always find the job already `running`: the
editor's post-frame seam starts it within a frame. To reach the cancellable
window, write both lines into the command file in ONE append so they dispatch
in the same frame:

```bash
printf '%s\n%s\n' \
 'agent {"version":1,"request_id":"a1","command":"job.start","args":{"operation":"reload"}}' \
 'agent {"version":1,"request_id":"a2","command":"job.cancel","args":{"job_id":"1"}}' \
 >> /mnt/c/tmp/matter-jobs/commands.txt
```

**2026-09-09 native MSVC acceptance.** On `StreamMountain`: a reload job
completed in 17.1 s (`queued_ms` 31, `running_ms` 17153); two `job.start`
requests in flight left job 5 `superseded` naming `superseded_by: "6"` while
job 6 completed. On `RockGallery`: the same-frame write above returned
`cancelled: true` and `job.status` then read `cancelled`; a `regenerate` with
seed `424242` completed in 2.29 s with `content_digest 0e76c92dc6e0b23e`; and
`job.start` + `job.wait` + `quit` in one write ended the wait as
`execution_failure` / `state:"failed"` / `completed:false` with
`"the editor shut down before this job finished"`.

## 4d. Verify what a change actually produced

`job.wait` tells you a bake finished. It does not tell you whether the world it
produced differs from the one before it, and on a procedural world you cannot
read that off two `scene.list_objects` pages: a baked root's id IS its content
hash, so any rebake re-addresses every root and a naive comparison reports
2xN removals-plus-additions. `scene.capture_snapshot` + `scene.diff` pair on
the LOGICAL key (module name / authored entity id) instead, so a rebake reads
as one `changed` row per object carrying `regenerated: true`.

```bash
CMD=/mnt/c/tmp/matter-agent/commands.txt
RES=/mnt/c/tmp/matter-agent/results.jsonl
A () { python3 tools/matter_agent.py "$1" --cmd-file "$CMD" --result-file "$RES" \
         ${2:+--args "$2"} ${3:+--timeout "$3"}; }

# 1. baseline
A scene.capture_snapshot '{"label":"before"}'      # -> result.snapshot.snapshot_id

# 2. the change under test
A job.start '{"operation":"regenerate","seed":"12345"}'
A job.wait  '{"job_id":"1"}' 30

# 3. what it produced, against the live scene
A scene.diff '{"from":"1"}'
```

Read `result.compatibility.level` first:

- `full` — every logical key paired;
- `partial` — the comparison still holds, but a named class could not be
  paired: `entity/session_allocated_id` (the two captures come from different
  session generations, so a runtime-minted id means different objects) or
  `baked_root/ambiguous_module_key` (a module published as several roots).
  Those objects are in `incomparable_objects`, never guessed at;
- `incomparable` — different worlds or projects. There are **no rows**, and
  `reason` says why. That is deliberate: "everything was removed and everything
  was added" reads like a finding.

Then `result.summary`:

- **a no-op reload** (a cache-hit rebake) is `added/removed/changed` all zero
  with `content_identical.value: true`;
- **a seeded reroll** is `changed == regenerated == <root count>`, each row
  carrying `incarnation`, `params_digest` and `world_seed` in `changed_fields`,
  plus `bounds` for the parts that actually moved;
- `content_identical` is an AVAILABILITY. A world-kind (streamed) world such as
  `StreamMountain` publishes no part-graph roots, so it has no digest and the
  field reports `available:false` with that reason — an absent digest never
  reads as identical content. Check those worlds with `viewport.capture` +
  `MatterEngine3/tools/img_diff.py`.

`scene.query` is the same capture with filters, including a world-space region:

```bash
# every baked root whose module mentions "rock", within 40 m of the origin
A scene.query '{"kinds":["baked_root"],"module_contains":"rock",
                "region":{"type":"sphere","center":[0,0,0],"radius":40},
                "limit":50}'

# everything wholly inside one box, in a snapshot captured earlier
A scene.query '{"snapshot":"1","region":{"type":"aabb","min":[-10,-5,-10],
                "max":[10,5,10],"mode":"contains"}}'
```

The region test runs against each object's captured world AABB — the same box
`scene.get_object` reports as `world_bounds` and the same one the selection
outline draws. `result.region.unresolved` is **not** a rejection count: an
object with no measured bounds (a root in the part graph placed nowhere, an
entity with no transform in the live ECS) was neither accepted nor rejected,
and that is a different answer from "outside the region". Page with
`offset`/`limit` exactly as `scene.list_objects` does; ordering is
`kind_then_logical_key` for a diff and `kind_then_id` for a query, both total,
so paging a fixed pair of snapshots is resumable.

Snapshots are retained 8 deep and cap at 20,000 objects. An id that aged out of
the ring answers `not_found` with `oldest_retained_snapshot_id`, so "evicted"
stays distinguishable from "never existed"; a capture over the object cap is an
ordered PREFIX with `capture.truncated: true`, so the missing tail cannot read
as a deletion.

**2026-09-09 native MSVC acceptance.** On `RockGallery` (1 baked root, 0
entities): the baseline capture reported `content_digest 99d4be1f1b73784b` and
`world_seed available:false`; a `reload` job completed and `scene.diff` returned
`compatibility.level:"full"`, `changed:0`, `unchanged:1`,
`content_identical.value:true`, zero rows. A `regenerate` with seed `424242`
completed with `content_digest 0e76c92dc6e0b23e`, the next capture attributed
`world_seed 424242` to `"source":"regeneration job 2"`, and `scene.diff` between
the two returned ONE row: `changed` / `regenerated:true` on logical key
`RockGallery`, `changed_fields
["incarnation","params_digest","world_seed","part_instance"]`, with
`incarnation` naming `5261899218771808037` -> `184616422800810673`.

On `PhysicsPlayground` (10 entities, 4 baked roots): the capture measured 11 of
14 objects (`bounds_unresolved:3` — three roots are in the part graph but placed
nowhere) and reported no ambiguous logical keys. `scene.query` with
`{"type":"sphere","center":[0,0,0],"radius":5}` matched `Floor Body`, `Crate 0`
and `PlaygroundFloor` with `region.tested:11, unresolved.count:3`; the same box
under `mode:"contains"` matched only `Floor Body`; `limit:5` paged
`has_more:true, next_offset:5` and `offset:10` closed the set at 14.

Both labelling paths were exercised live. Switching
`PhysicsPlayground -> RockGallery` and diffing across it returned
`level:"incomparable"` with zero rows and the reason naming both worlds.
Switching back and diffing against the ORIGINAL `PhysicsPlayground` snapshot
returned `level:"partial"` with `session_scoped_ids_comparable:false` and the
`entity/session_allocated_id` class named — while all 14 authored objects still
paired as `unchanged` across session `1` -> session `3`. Error paths answered
`not_found` (an unretained snapshot id, carrying
`oldest_retained_snapshot_id`) and `invalid_input` (`region.min` past
`region.max`, `from:"current"`, `limit:500`).

## 5. Replay an issue shot and diff

```bash
bash docs/baselines/capture-replay-baseline.sh issues/<guid> /tmp/before.png
# ... make the engine change, rebuild ...
bash docs/baselines/capture-replay-baseline.sh issues/<guid> /tmp/after.png
python MatterEngine3/tools/img_diff.py /tmp/before.png /tmp/after.png \
    --channel-tol 16 --max-diff-pct 0.5
```

`capture-replay-baseline.sh` **wipes `projects/world_demo/.cache/<world>`
before every capture**. This is deliberate, not a bug to work around:
`.cache` is content-addressed on the world's **JS source**, not on engine code,
so a warm-cache replay after an engine-only change reloads the old bake and
reproduces the old pixels exactly — passing while proving nothing. Full
before/after semantics require rebaking both sides.

`img_diff.py`'s own defaults (`--channel-tol 2 --max-diff-pct 0.5`) are
**unusable for RT shots**: two identical runs of the same build differ on
~22% of pixels at tolerance 2 (RT denoiser grain, not a real difference — see
`docs/baselines/README.md` for the full noise-floor table and the calibrated
per-shot gates).

## 6. Run the Vulkan smoke gate

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
MatterEditor/tools/smoke_vulkan_faults.ps1 `
    -TestPath MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe `
    -TimeoutMilliseconds 30000
```

The fault harness runs the smoke exe **12 times**, once per
`MATTER_VK_SMOKE_MODE` value, each
under a per-mode timeout (30 s from the Makefile's override, except `rt`
90 s and `rt-transmission` 45 s, which raise their own floor): the two
Streamline-proxy-missing fault modes, `rt`, `rt-transmission`, `rt-disabled`,
`rt-unavailable`, `animation-skin`, and the five chart-VT modes (`vt`,
`vt-surfaces`, `vt-rt`, `vt-enrich`, `vt-enrich-nort`). Each mode must print
`validation errors: 0` and `ALL PASS`, and exit 0, or the whole gate fails.

To run a single mode directly (faster iteration while chasing one failure):

```powershell
$env:MATTER_VK_SMOKE_MODE='vt-enrich'
& MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
```

The smoke exe supports more modes than the 12-mode gate exercises (e.g.
`cull`, `tileset`, `transform`, `outlive-unproven`, `retention-fault-*`) — grep
`MatterEngine3/tests/vulkan_smoke_tests.cpp` for `std::string(smoke_mode) ==`
for the full current set.

## 7. Run the seam suite

```bash
MatterEngine3/tools/seam_suite.sh /tmp/seam-out
```

Runs the editor twice against the `SeamLab` world (`projects/world_demo/scenes/SeamLab`
— a cave-free heightfield built so "anything visible below the surface" is
unambiguously a defect): once with welds drawn, once with
`MATTER_NO_SEAM_WELD_DRAW=1`. Reports six checks, each the only detector of one
seam-defect class: (1) `hole_scan.py` — enclosed background pixels, exact zero
gate; (2) `crack_scan.py` — thin depth spikes showing farther terrain instead
of sky; (3) `MATTER_SEAM_TRACE` — the welder's own accounting invariants; (4)
shading — pixels the weld paints and how much darker, the only check that sees
a geometrically-closed-but-visibly-wrong seam; (5) flicker — two depth
captures of one static pose several seconds apart must be bitwise identical;
(6) residue — the welder's own `missing_landing`/`missing_coarse_pair`/
degenerate counts. Output: `<out-dir>/report.txt` + `report.json`; exit 1 if
any gated check fails. See `docs/seam-suite-2026-08-13.md` for the design
rationale.

## 8. Headless perf run

```bash
cd MatterEditor
MATTER_WORLD=StreamMountain \
MATTER_PERF_OUTPUT="C:/tmp/perf.jsonl" \
MATTER_PERF_WARMUP_SECONDS=5 \
MATTER_PERF_SAMPLE_SECONDS=20 \
TMP="C:/Users/webde/AppData/Local/Temp" \
TEMP="C:/Users/webde/AppData/Local/Temp" \
  ./build/windows-msvc/editor.exe
```

All three `MATTER_PERF_*` vars **must be set together** — setting any subset is
a fatal startup error (`main.cpp`'s `read_perf_run_config`). The run waits for
bake-ready, warms for `_WARMUP_SECONDS`, samples for `_SAMPLE_SECONDS`, writes
one JSON line to `_OUTPUT`, and exits. A run that observes any Vulkan
validation errors during sampling is a fatal failure, not a warning.

## 9. Fly-through soak

```bash
cd MatterEditor
MATTER_WORLD=StreamMountain \
MATTER_CAM_PATH=../MatterEngine3/tools/streammountain_flythrough.path \
MATTER_CAM_PATH_EXIT=1 \
MATTER_CAM_PATH_WARMUP=30 \
  ./build/windows-msvc/editor.exe
```

Consumes one pose per **rendered frame** (frame-indexed, not wall-clock) from
the `.path` fixture, holds `_WARMUP` frames at the first pose once the world is
drawable, then quits when the path (plus its FIFO drain tail) ends. Path
fixtures live in `MatterEngine3/tools/` (`streammountain_flythrough.path`,
`streamcaverns_flythrough.path`, `lod_flythrough_pomproofbrick.path`,
`lod_flythrough_rockgallery.path`); `docs/baselines/seam-soak.sh` is a worked
example that also enables `MATTER_SEAM_TRACE` and polls
`WorldSession::seam_weld_status()` at the end.

## 10. Windows CPU CTest gate

The canonical Windows CPU gate is the MSVC `cpu` label rather than a manually
maintained list of Make targets:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' `
    --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo `
    -L cpu --output-on-failure
```

### MinGW rollback target inventory

For rollback diagnosis only, the old `tests/Makefile` targets that link
`-lGL -lX11 -ldl -lrt` (Linux-only libs) fail at *link* time on Windows —
compilation (the syntax/semantic check) still succeeds, so a green `g++`
compile does not imply the binary runs. Targets that don't depend on raylib/GL
link **and run** fully headless on Windows. The known-good set:

```
run-world-definition   run-script            run-evalworld
run-lod-distance       run-eventchannel       run-eventhub
run-errorevents         run-eventcommand      run-eventproperty
run-sectorstream        run-sectorcoord       run-terrainfield
run-terrainmesh         run-seamweld          run-contourseam
run-contourmesh         run-contourengine     run-viewer-logic
run-props
```

(`run-eventchannel`/`run-eventhub`/`run-errorevents`/`run-eventcommand`/
`run-eventproperty` are "the run-event* five" — the event-system test targets,
`MatterEngine3/tests/Makefile` around the `event_channel_tests.cpp` block.)
Run one directly, e.g.:

```bash
make -C MatterEngine3/tests run-world-definition GRAPHICS=GRAPHICS_API_OPENGL_43
```

`vulkan-smoke` and `run-vt-compositor` are special-cased in the rollback graph: they delegate to
`MatterEditor`'s cross-build rules (recipe 6) because the Vulkan/GLFW link line
lives there, not in this Makefile.

## 11. JS world-script tests

```bash
node --experimental-default-type=module projects/world_demo/tests/alpine_ecology_tests.mjs
```

There is no `package.json` anywhere in the repo (deliberately — see
`CLAUDE.md`: the same `.js` modules are loaded by the engine's QuickJS host,
which has its own module resolution and wouldn't see one), so a plain
`node file.mjs` dies with "is a CommonJS module" — the `--experimental-default-type=module`
flag (or `--experimental-detect-module` on Node ≥ 20.10) is required every time.

## 12. RiverFloatLab visual playtest

The visual-first floating-body slice has two native Node scene-contract tests
and an opt-in PhysX MSVC editor build:

```powershell
node projects/world_demo/tests/river_hydrology_scene_tests.mjs
node projects/world_demo/tests/river_float_lab_scene_tests.mjs
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor `
    -EnablePhysx -PhysxRoot $physxRoot -CudaRoot $cudaRoot
```

Before a cold visual run, resolve the cache root and verify the deletion target
is exactly `projects/world_demo/.cache/RiverFloatLab`; never clear the project
cache broadly. Launch only `MatterEditor/build/windows-msvc/editor.exe` with
`MATTER_WORLD=RiverFloatLab`, wait for `bake.finished`, and enter Play. A live
review window should remain visible and open; do not set `MATTER_HIDE_UI` or put
`quit` in its FIFO.

This recipe is a visual playtest, not the automated traversal acceptance gate.
The body CSV/diagnostics seam, marker-order assertions, and the dedicated
`river_float_physics.timeline`/PowerShell runner remain deferred until that
read-only control surface exists. Do not infer or fabricate those results from
screenshots.

## 13. Raster-water forward-optics acceptance

After building the native MSVC editor, compare the forward raster-water path
against the committed-date local baseline with one five-camera RiverFloatLab
capture and matched 1, 10, and 16 shadow-sample performance runs:

```powershell
$baseline = (Resolve-Path 'build/qa/raster-water-forward-2026-08-29/baseline').Path
$candidate = (New-Item -ItemType Directory -Force `
    'build/qa/raster-water-forward-2026-08-29/candidate').FullName
& MatterEngine3/tools/run_raster_water_forward_acceptance.ps1 `
    -BaselineDir $baseline -OutputDir $candidate
```

The runner exits nonzero if a screenshot or its completion sidecar is missing
or empty, if Vulkan validation/water-decode/steady-allocation counters are
nonzero, if the copied HDR-plus-depth payload is not exactly 12 logical bytes
per internal pixel, or if any matched timing exceeds its allowed regression.
It writes the five native-size PNGs under `<candidate>/screenshots/`, the
three performance JSON files at the candidate root, and complete run logs in
the sibling `capture/` and `run-*/` directories. Visual acceptance still
requires opening all five PNGs at native size and checking the criteria in
`docs/findings/raster-water-forward-optics-acceptance-2026-08-29.md`.

## 14. Animated-water section-handoff acceptance

Build the PhysX-enabled MSVC editor, then give the Stage 1 runner a new or
empty output directory:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor `
    -EnablePhysx -PhysxRoot $physxRoot -CudaRoot $cudaRoot
& MatterEngine3/tools/run_water_mesh_continuity_acceptance.ps1 `
    -Stage Stage1 `
    -OutputDir build/qa/water-mesh-continuity-2026-08-29/stage1
```

The runner isolates its RiverFloatLab project and cache, performs a cold bake,
an unchanged warm bake, and a fixture-only downstream edit, then runs the
native Vulkan gates and the strict Stage 1 comparator. It retains hydrology
traces, a JSON summary, and 20 `section-handoff` PNG/`.done` pairs: frames
`0,7,15,22,29` in normal, geometry-normal, foam-driver, and identity views.
Passing the comparator is necessary but not sufficient: inspect every retained
PNG at native size and record the visual verdict in
`docs/findings/animated-water-section-continuity-acceptance-2026-08-29.md`.
Task 10 remains blocked unless that finding says `handoffVisualGate: pass`.

---

## Traps

- **Rollback-only TEMP behavior.** MSYS2's `make` used to
  clobber the Windows `TEMP` env var, so GCC failed with "Cannot create
  temporary file in C:\WINDOWS\" unless you passed `TMP=`/`TEMP=` explicitly on
  every `make` invocation. `platform.mk` (included by every Makefile in this
  repo) now exports both automatically, so recipes 1, 2, and 10 need no env
  prefix. **This does not cover launching `editor.exe` directly** (recipes 3,
  8, and the FIFO session in recipe 4 still need `TMP=`/`TEMP=` on the exe
  invocation itself — see launch rule 1 in `control-surface.md` §e) — a native
  Windows exe doesn't inherit MSYS2's TEMP the way a `make`-driven compile now
  does. If a `make` invocation ever hits the old error anyway, `platform.mk`'s
  Windows detection didn't fire for that path (see its own top comment); fall
  back to passing `TMP`/`TEMP` explicitly as before.
- **Exit code, not grep.** Never decide a build passed by grepping stdout for
  the string "error" — check the actual exit code. Log lines containing
  "error" appear in passing builds (warnings, expected-failure test output).
- **The editor exe file lock.** `editor.exe` holds a lock on its own binary
  while running; a build started while it's up fails or silently no-ops. Kill
  it first.
- **Symlinks removed, not junctioned.** The two directory symlinks the build
  used to require (`MatterEngine3/shaders`, `MatterEditor/shaders`) were
  removed outright on 2026-08-14 — every Makefile now references
  `libs/MatterSurfaceLib/shaders` directly, so there is nothing left to
  junction. `setup-worktree.sh` is a deprecated stub (prints an explanation,
  exits 0) rather than deleted, so the old `bash setup-worktree.sh` habit after
  `git worktree add` fails loudly-but-kindly instead of silently no-op'ing.
  Still true either way: never `git stash` in a worktree.
- **RETOPO.** `MatterEditor/Makefile` defaults to `RETOPO=1`. The vendored-TBB
  link failure that used to force `RETOPO=0` on Windows was fixed by replacing
  it with a header-only shim, so the default build does not currently need
  `RETOPO=0` — re-check `MatterEditor/Makefile`'s `RETOPO ?=` line (same
  verify-before-trusting rule as `CLAUDE.md`'s toolchain section) before
  assuming that's still true.
- **`.gtex` bake determinism.** The smoke suite stubs the bake out, so it
  cannot catch a `.gtex` (tileset texture bake) non-determinism regression.
  Drive the real bake instead: launch the editor with
  `MATTER_WORLD=FloorDemo MATTER_TILESET_DUMP_PNG=<dir>`, bake twice into two
  different dump directories, and `img_diff.py` the pairs — a double-bake
  bitwise (or near-bitwise) compare is the real gate, not the smoke suite.
