# QA Cookbook

Copy-paste recipes for driving the engine/editor from a shell. Each recipe is a
fenced command plus what to expect. **Build-command detail is intentionally
minimal here — defer to the root `CLAUDE.md`** for the current toolchain
incantation; it is more likely to be current than a second copy of it pasted
into this file, and the build commands are known to be in flux.

See `docs/agent/control-surface.md` for the full FIFO verb table and env var
reference these recipes exercise, and `docs/agent/issue-system.md` for the
issue-report/replay recipes (5).

## 1. Build the engine lib

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -C MatterEngine3 \
  TMP="C:/Users/webde/AppData/Local/Temp" \
  TEMP="C:/Users/webde/AppData/Local/Temp"
```

Expect `build/libmatter_engine3.a` plus a regenerated embedded-shader header on
exit code 0. **Check the exit code, not the output for the string "error"** —
see the Traps section.

## 2. Build the Windows editor

```bash
make -C MatterEditor windows \
  TMP="C:/Users/webde/AppData/Local/Temp" \
  TEMP="C:/Users/webde/AppData/Local/Temp"
```

Expect `build/windows/editor.exe`. Requires the shader junctions from
`setup-worktree.sh` to already exist (see the Traps section) and requires
`editor.exe` not to already be running (file lock).

## 3. One-shot screenshot of a world

```bash
cd MatterEditor
MATTER_WORLD=StreamMountain \
MATTER_CAM="0,140,0,0,60,60" \
MATTER_SCREENSHOT="C:/tmp/shot.png" \
MATTER_SCREENSHOT_SETTLE=90 \
TMP="C:/Users/webde/AppData/Local/Temp" \
TEMP="C:/Users/webde/AppData/Local/Temp" \
  ./build/windows/editor.exe
```

Expect the process to bake, settle 90 frames (streamed worlds need far more
than the 3-frame default — see `docs/agent/control-surface.md` §c), write
`C:/tmp/shot.png`, and **exit on its own** (a plain `MATTER_SCREENSHOT` run is
capture-then-quit; no `quit` command needed).

## 4. FIFO-driven multi-shot session

The pattern every scripted capture in this repo follows: launch with a command
file, poll the log for readiness, append commands, wait for `.done` sidecars,
`quit`. `MatterEngine3/tools/viewer_shots.sh` is the exemplar — read it before
writing a new one:

```bash
FIFO=/tmp/matter_shots_$$.fifo
mkfifo "$FIFO"
cd MatterEditor
MATTER_WORLD=meadow MATTER_CMD_FIFO="$FIFO" ./build/windows/editor.exe > viewer.log 2>&1 &
PID=$!

# poll readiness
until grep -q 'viewer: bake ready' viewer.log; do sleep 1; done

echo "cam 20 760 350 0 420 0" > "$FIFO"
sleep 2                          # let LOD/batches settle at the new view
echo "stats current-cost" > "$FIFO"
echo "shot C:/tmp/current-cost.png" > "$FIFO"
until [ -e "C:/tmp/current-cost.png.done" ]; do sleep 1; done

echo "quit" > "$FIFO"
wait "$PID"
```

Expect one PNG + its `.done` sidecar per `shot`, and a `STATS,<label>,...` line
in `viewer.log` per `stats` command. On Windows, `MATTER_CMD_FIFO` is a polled
plain file, not a real FIFO — `mkfifo`/blocking-open semantics only apply on
Linux, but the append-then-poll shell idiom above works on both because the
editor itself handles the platform difference.

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

```bash
make -C MatterEngine3/tests vulkan-smoke
```

This delegates to `make -C MatterEditor vulkan-smoke`, which builds
`vulkan_smoke_tests.exe`/`vulkan_compat_tests.exe` and drives
`MatterEditor/tools/smoke_vulkan_faults.ps1 -TimeoutMilliseconds 30000`, which
runs the smoke exe **12 times**, once per `MATTER_VK_SMOKE_MODE` value, each
under a per-mode timeout (30 s from the Makefile's override, except `rt`
90 s and `rt-transmission` 45 s, which raise their own floor): the two
Streamline-proxy-missing fault modes, `rt`, `rt-transmission`, `rt-disabled`,
`rt-unavailable`, `animation-skin`, and the five chart-VT modes (`vt`,
`vt-surfaces`, `vt-rt`, `vt-enrich`, `vt-enrich-nort`). Each mode must print
`validation errors: 0` and `ALL PASS`, and exit 0, or the whole gate fails.

To run a single mode directly (faster iteration while chasing one failure):

```bash
cd MatterEditor
MATTER_VK_SMOKE_MODE=vt-enrich ./build/windows/vulkan_smoke_tests.exe
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
  ./build/windows/editor.exe
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
  ./build/windows/editor.exe
```

Consumes one pose per **rendered frame** (frame-indexed, not wall-clock) from
the `.path` fixture, holds `_WARMUP` frames at the first pose once the world is
drawable, then quits when the path (plus its FIFO drain tail) ends. Path
fixtures live in `MatterEngine3/tools/` (`streammountain_flythrough.path`,
`streamcaverns_flythrough.path`, `lod_flythrough_pomproofbrick.path`,
`lod_flythrough_rockgallery.path`); `docs/baselines/seam-soak.sh` is a worked
example that also enables `MATTER_SEAM_TRACE` and polls
`WorldSession::seam_weld_status()` at the end.

## 10. Which `MatterEngine3/tests` run-* targets run fully on Windows

Per `CLAUDE.md` and the `tests/Makefile` comments: targets that link
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
make -C MatterEngine3/tests run-world-definition \
  TMP="C:/Users/webde/AppData/Local/Temp" \
  TEMP="C:/Users/webde/AppData/Local/Temp" \
  GRAPHICS=GRAPHICS_API_OPENGL_43
```

`vulkan-smoke` and `run-vt-compositor` are special-cased: they delegate to
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

---

## Traps

- **The TEMP incantation.** MSYS2's `make` clobbers the Windows `TEMP` env var,
  so GCC fails with "Cannot create temporary file in C:\WINDOWS\" unless you
  pass `TMP=`/`TEMP=` explicitly on every `make` invocation (see recipes 1–2).
  **This is reported to be getting fixed by a `platform.mk` on a parallel
  branch as of this writing** — if that has merged, check whether the explicit
  `TMP`/`TEMP` passing above is still required before copying it verbatim.
- **Exit code, not grep.** Never decide a build passed by grepping stdout for
  the string "error" — check the actual exit code. Log lines containing
  "error" appear in passing builds (warnings, expected-failure test output).
- **The editor exe file lock.** `editor.exe` holds a lock on its own binary
  while running; a build started while it's up fails or silently no-ops. Kill
  it first.
- **Junction rules.** Git worktrees on Windows materialize tracked symlinks as
  plain text files, which breaks the build in a confusing way. Run
  `bash setup-worktree.sh` from repo root after creating a worktree — it
  recreates `MatterEngine3/shaders` and `MatterEditor/shaders` as NTFS
  junctions pointing at `libs/MatterSurfaceLib/shaders`. **Symlink removal for
  other directories is reported in progress on a parallel branch** — if merged,
  re-check `setup-worktree.sh` and `CLAUDE.md` before assuming this junction
  list is still current. Also: never `git stash` in a worktree — it destroys
  these junctions and every later build silently no-ops without an error.
- **`.gtex` bake determinism.** The smoke suite stubs the bake out, so it
  cannot catch a `.gtex` (tileset texture bake) non-determinism regression.
  Drive the real bake instead: launch the editor with
  `MATTER_WORLD=FloorDemo MATTER_TILESET_DUMP_PNG=<dir>`, bake twice into two
  different dump directories, and `img_diff.py` the pairs — a double-bake
  bitwise (or near-bitwise) compare is the real gate, not the smoke suite.
