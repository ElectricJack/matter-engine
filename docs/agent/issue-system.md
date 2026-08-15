# Issue System

The editor's in-app defect capture and filing system, and how to consume what
it writes.

Source files: `MatterEditor/src/issue_reporter.h`/`.cpp` (writer — ImGui-free,
so the schema and crop math are headlessly testable; see
`MatterEditor/tests/test_issue_reporter.cpp`), `issue_reporter_panel.cpp` (the
ImGui dialog), `shot_replay.h`/`.cpp` (reader/replayer), `main.cpp`
(orchestration: hotkeys, capture readback, and all three `write_issue_report`
call sites — the button, the FIFO `issue file` verb, and the device-fault
auto-filer).

## Capture-first design

The flow is deliberately capture-first, not form-first: opening a dialog and
*then* asking for a screenshot loses the moment, and every field you must fill
before capturing is a reason not to bother.

- **F9** — freezes the screen immediately, then you drag a box over the part
  that's wrong. The crop comes out of the frozen frame, so an animating scene
  can't slide out from under the selection.
- **F10** — grabs the active viewport as-is, no drag.

Both keys are handled at the **GLFW level**, not through ImGui — `main.cpp`
claims them unconditionally alongside TAB (camera capture) rather than gating
on `io.WantCaptureKeyboard`, specifically because ImGui's
`ImGuiConfigFlags_NavEnableKeyboard` makes that flag true whenever any panel
holds nav focus, which would make the hotkey fire only sometimes.

Shots accumulate in an open report — keep hitting F9/F10 to add more — until
you press **"File report"** in the reporter window, which calls
`write_issue_report`.

**Filing is still manual by default, with two exceptions.** `write_issue_report`
now has three call sites in `main.cpp`: the "File report" button handler, the
FIFO `issue file` verb (§ "Headless filing" below — an agent-driven equivalent
of pressing the button), and a best-effort auto-filer that runs once, right
before the process exits, when a fatal error traces back to a Vulkan/device
fault (§ "Automatic filing on device fault" below). Nothing files on a
validation error or a perf regression — those stay visible only in logs/stats
and still require a human (or an agent watching the log) to press F9/F10, or
send `issue capture`/`issue file`, and file.

## On-disk layout: `issues/<guid>/`

- **`issue.md`** — the report. YAML frontmatter: `id`, `world`, `shots` (count),
  `status: unprocessed`, `reported` (UTC timestamp). Body has no title/kind/
  severity/area — the reporter doesn't ask for them; that's the ingestion
  pass's job, reading the content below. The **`## Repro`** section is a
  runnable command line (`MATTER_WORLD=... MATTER_CAM="..." ./build/windows/editor.exe`,
  with a note to launch from a shell that hasn't exported the MSYS2 UCRT64
  `PATH`), plus one per shot whose camera moved from the first. Ends with an
  `## Acceptance` TODO section prompting for a headless
  `make -C MatterEngine3/tests run-*` target or a scripted capture as the check
  that closes the report.
- **`state.json`** — machine-readable. Per shot: camera/render toggles/viewport
  rect/crop rect, plus a **240-frame raw history ring** of frame timing samples
  captured *at that shot* (see "The timing gotcha" below), and cumulative
  counters (`instance_cache_expansions`, `command_layout_rebuilds`,
  `immediate_submits`, `resident_sectors`) at capture. At top level,
  `at_file_time`: deep engine state (camera, sim, framebuffer, frame/loop/GPU
  timing) sampled once, when the report is written — plus a `"props"` object
  (present only when a registry was supplied) holding every registered
  property that differs from its baseline, via `matter::props::dump_modified`.
- **`shot-N.png`** — one per capture, cropped as selected.
- **`shot-N.png.layout.ini`** — the ImGui docked-layout sidecar
  (`ImGui::SaveIniSettingsToMemory` at capture time). The viewport rect is
  entirely a function of the docked layout, so a replay needs this to land the
  camera in the same pixels the original shot did.
- **`log-tail.txt`** — the last **200** console-ring lines before filing,
  unfiltered (ignores the console panel's current severity filter).
- **`profile_tail.json`** — the recent profiler FrameRecord history as a
  Chrome trace (`chrome://tracing`-loadable). **Best-effort**: a
  profiler-disabled build or an empty ring just skips it and never blocks
  filing an otherwise-complete report.

## GUID scheme

Report directories are named by a bare GUID (`make_guid()` in
`issue_reporter.cpp`) — **not RFC-4122-certified** (a seeded `mt19937_64`
formatted into the canonical `8-4-4-4-12` hex shape) and **not
content-hashed**, so there is no dedup: filing the same defect twice makes two
directories. It only has to not collide on one workstation. Naming by GUID
rather than title is deliberate — "naming costs thought at exactly the wrong
moment" — with ordering left to the frontmatter's `reported:` timestamp.
Humans commonly refer to a report by its first-8-hex short form. An ingestion/
triage pass is expected to rename the directory to something readable (e.g.
`<area>-<slug>`) once it has read the report, while keeping `id:` in the
frontmatter stable — the renaming convention itself is a workflow practice,
not something the writer code enforces.

## `issues/` directory resolution

Precedence, in `issues_dir()` (`issue_reporter.cpp`) and `issues_root()`
(`main.cpp`): **`MATTER_ISSUE_DIR`** env var, then whatever `set_issues_dir()`
was called with (main.cpp resolves the real location the same way it resolves
asset roots — walking up from the executable, then the working directory —
because the editor is routinely launched from `build/windows/` or a packaged
dist folder, not `MatterEditor/`), then the literal fallback `"../issues"`
(only correct when launched from `MatterEditor/`).

`issues/` has been **gitignored since commit `0cbb5e92`** ("Add issues to
.gitignore", 2026-07-30 — it also deleted the committed `issues/README.md`).
`main.cpp`'s `issues_root()` comment has been corrected to match: it now says
`issues/` is gitignored (untracked, not committed), and that the direct lookup
wins only because the directory exists on disk for developers who have
already generated reports into it.

`.gitignore` also has a `!/issues/**/*.layout.ini` negation (added to
un-ignore layout sidecars from the blanket `*.ini` rule, with the comment
"issue-report layout sidecars ... are evidence rather than local config"). **It
does not work**: verified with `git check-ignore -v`, both `issue.md` and
`shot-1.png.layout.ini` under a synthetic `issues/<guid>/` path are still
reported ignored, attributed to the separate bare `issues` rule added later in
the same file. Per git's own documented limitation, a negation cannot
re-include a path once a parent directory is excluded by another pattern — so
this exception has had no effect since the `issues` line was added, and no
`.layout.ini` file has actually been trackable. Whether that's intended (the
exception is simply dead) or a bug worth fixing is unresolved; don't rely on
layout sidecars being committed.

## Headless filing: `issue capture` / `issue file`

Two FIFO verbs (§b of `docs/agent/control-surface.md` has the full grammar)
let a script/agent drive the exact same capture-first flow F9/F10 + "File
report" do, without a mouse:

- **`issue capture`** — the headless F10: reuses `begin_viewport_capture` and
  the same `AwaitingCapture` readback handshake main.cpp already runs every
  frame, so the report directory is created on the first shot exactly as the
  interactive flow does, and repeated captures accumulate into the same
  draft. **Blocking**: no later FIFO line dispatches until the readback and
  `record_shot` actually complete, with the same ~30s deadman shape as `shot`/
  `shot_now` (`issue: capture timeout, abandoned` on expiry — see the log
  marker table in `control-surface.md`).
- **`issue file <note>`** — sets the draft's note to the rest of the line
  (the note may be empty only if the draft already has a shot) and calls
  `write_issue_report` with the same `IssueContext` the button builds (props
  registry, camera, sim mode/time scale, frame size). Prints `issue: filed
  <path>` on success or `issue: file failed (<reason>)` on failure, and
  resets the draft afterward — same as pressing the button. Unlike `issue
  capture`, this is not itself a blocking wait; it does not need to be, since
  a `quit` line after it cannot even be *dispatched* until the write starts
  (dispatch is already serialized behind any earlier blocking verb), and the
  write itself finishes within the same frame it starts.

Both verbs work identically whether or not `MATTER_HIDE_UI` is set — neither
depends on the "File report" button or the ImGui window actually drawing
(the button's own trigger only exists inside the `!hide_ui` UI pass; `issue
file`'s FIFO handling deliberately runs outside it).

Example `drive.py` timeline (see `MatterEngine3/tools/drive.py` and the QA
cookbook) that bakes a world, captures one shot, and files a report — a
scripted version of "hit F10, type a note, click File report":

```
wait_event bake.finished 300
issue capture
issue file camera clips through the north wall near spawn
quit
```

```bash
python MatterEngine3/tools/drive.py --world CornellBox \
    --timeline timeline.txt --out-dir out/ \
    --env MATTER_ISSUE_DIR=/abs/path/to/scratch-issues
```

Always point `MATTER_ISSUE_DIR` at a scratch directory for a scripted run —
`drive.py`'s `--env` flag exists for exactly this — so testing the issue
system does not write into the repo's real `issues/`.

## Automatic filing on device fault

The one exception to "filing needs a person at the keyboard": if a fatal
error traces back to a Vulkan/device fault, `main.cpp` files a report on its
own, once, immediately before the process exits.

**What triggers it.** `main.cpp` tracks a `fatal_error_reason` string
alongside the existing `fatal_error` flag, set only by the fatal sites that
are Vulkan/device-surfacing: `render()` failing, `end_frame()` failing (the
seam `MATTER_VK_TEST_END_FRAME_FAULT` and a real `VK_ERROR_DEVICE_LOST` from
`vkQueueSubmit2` both go through), the ImGui Vulkan backend's prepare/
end-of-frame calls failing, and five consecutive swapchain-readback
failures. Non-device fatal exits (`MATTER_REPLAY_STRICT` mismatch, a perf
run's Vulkan-validation-error count) leave `fatal_error_reason` empty and are
**not** auto-filed — those behave exactly as before.

**Where it runs.** A single seam, right after the main loop exits and before
any teardown (`session`/`vulkan`/`ui` are all still alive) — main-thread
only, so there is no cross-thread rendezvous to get wrong. This covers any
fault that surfaces through the main thread's own render/present calls, which
is what every fault-injection env var and a real device loss both do. A
device fault on a **background thread** (e.g. inside a bake worker, if one
ever made its own Vulkan calls) would not reach this seam — there is no clean
main-thread rendezvous for that case, so it is explicitly out of scope.

**What the report contains.** Note only: `auto-filed: device fault
(<fatal_error_reason>)`. **No shots** — a post-loss readback would either
hang against a dead device or hand back garbage, and the entire premise of
this path is that nobody is left to press F9/F10. `state.json`, `log-tail.txt`
and `profile_tail.json` are written the same as any other report (all
CPU-side; none of it touches the Vulkan device). If
`MatterEditor/vulkan_device_fault.log` exists and its mtime is at or after
process start, it is copied into the report directory too — a **real**
`VK_ERROR_DEVICE_LOST` reaches `vk_context.cpp`'s `log_device_fault()` (which
writes that file via `vkGetDeviceFaultInfoEXT`), but the *injected*
`MATTER_VK_TEST_END_FRAME_FAULT` faults do not (they substitute a different
`VkResult` before submit; they never put the device itself into the lost
state `vkGetDeviceFaultInfoEXT` requires) — so an injector-driven test run
correctly gets a report with no `vulkan_device_fault.log`, while a real
device loss gets both.

**Guarantees.** Best-effort by construction: every filesystem/engine call in
the auto-filer is wrapped in one `try`/`catch(...)`, and nothing in it can
change `fatal_error` or the process's exit code — a failure while trying to
record the original fault is swallowed (`issue: auto-file failed (<reason>)`
or `issue: auto-file threw ...`, both to stderr) rather than escalated into a
second fatal error. It never blocks or delays the exit path beyond the one
synchronous filing attempt.

**Build note.** The fault-injection env vars this depends on for testing
(`MATTER_VK_TEST_END_FRAME_FAULT` and the rest of the `MATTER_VK_TEST_*`
family, see `control-surface.md` §c) only compile in under
`-DMATTER_VK_TEST_FAULT_INJECTION`, which the default `make -C MatterEditor
windows` build does **not** define (see the comment above `FAULT_INJECTION`
in `MatterEditor/Makefile`) — the shipped editor.exe never carries test-only
fault injection. Build with `make -C MatterEditor windows FAULT_INJECTION=1`
to get an editor.exe that responds to those env vars; the auto-filer itself
has no such gate and is always compiled in, since it only depends on
`fatal_error`/`fatal_error_reason`, not on the injectors.

## Listing reports: `issues_list.py`

`MatterEngine3/tools/issues_list.py` (python3, stdlib only) tabulates
`issues/<guid>/` report directories without opening the editor — for a quick
"what's outstanding" check or for another script to consume:

```bash
python MatterEngine3/tools/issues_list.py [--dir <issues-root>] [--json]
```

`--dir` resolution mirrors `issues_dir()`'s own precedence as closely as a
standalone script can: `--dir` if given, else `$MATTER_ISSUE_DIR`, else
`../issues` relative to the current working directory if that exists, else
`./issues`. Each row shows a short id (first 8 hex of the frontmatter `id:`,
falling back to the directory name), the `reported:` timestamp, `world:`,
`status:`, a shot count (**counted from `shot-*.png` files on disk**, not the
frontmatter `shots:` field — the two can disagree, e.g. a report whose
frontmatter still says an earlier, since-superseded count), and the first
line of the `## Report` section (or `(no note)`). Sorted newest first.
Malformed or partial reports (missing `issue.md`, no well-formed frontmatter
block) are still listed, never skipped and never a crash — their
unparseable fields read `(unparseable)`. `--json` prints a JSON array and
nothing else on stdout, safe to pipe into
`python -c "import json,sys; json.load(sys.stdin)"`.

## Consuming a report: `MATTER_REPLAY`

Reproduces one shot headlessly, for verification (fix, replay before/after,
diff the PNGs — turning "does it look right now?" into a check with an exit
code, not an argument):

```
MATTER_REPLAY=<issues/guid/state.json path>
MATTER_REPLAY_SHOT=<n>        # 1-based, default 1
MATTER_REPLAY_OUT=<path>      # output PNG
MATTER_REPLAY_SETTLE=<n>      # frames to hold before capture, default 90
MATTER_REPLAY_STRICT=1        # any mismatch -> exit 1, instead of a warning
```

A replay descriptor is **a single pose plus render state — it is NOT an input
recording.** It restores camera, viewport, crop, DLSS mode, pixel budget,
debug view mode, UI visibility, sim mode/time-scale, and the ImGui layout
sidecar; it does not replay input events or a tick history.

Explicitly **non-reproducible**, per `shot_replay.h`:
- **DLSS** is temporal and resolution-dependent — replay forces `Native` and
  says so when the shot used something else.
- **Play-mode shots** depend on where the simulation had got to; the
  descriptor records transport state, not a tick count, so an animated subject
  lands at a different phase. Pause before capturing anything you intend to
  diff.
- **Streaming worlds** depend on which sectors happen to be resident.
- **Bakes are not bit-deterministic** (see `CLAUDE.md`).

Once the layout is pinned and `IniFilename` is disabled for the replay run,
replay of a static (non-RT-noisy) crop is measured bit-exact (0 differing
pixels across repeated replays from different working directories).

## Workflow: `docs/baselines/capture-replay-baseline.sh` + `img_diff.py`

```bash
bash docs/baselines/capture-replay-baseline.sh <issue-dir> before.png
# ... make the change, rebuild ...
bash docs/baselines/capture-replay-baseline.sh <issue-dir> after.png
python MatterEngine3/tools/img_diff.py before.png after.png \
    --channel-tol 16 --max-diff-pct 0.5
```

The script wipes the world's `.cache` before every capture (it's keyed on JS
source, not engine code — a warm-cache replay after an engine change silently
reproduces the old pixels). **RT is not bit-exact run to run**: two identical
runs of the same build differ on **22% of pixels at the default
`--channel-tol 2`** — that default is calibrated for a raster path and is
useless for RT shots. Use the per-shot gates in `docs/baselines/README.md`
(PomProofBrick's primary gate is `--channel-tol 16 --max-diff-pct 0.5`, with a
measured noise floor of 0.066–0.069%).

## The timing gotcha

Its own section because it has caused real investigations to chase phantoms.
**Per-shot telemetry samples at capture; `at_file_time` samples at submit** —
seconds later, by which time a spike has recovered. Quoting the rationale
comment in `issue_reporter.h`:

> Every hitch report filed against this engine so far has carried a CALM
> frame... Three reports in a row recorded 29-31 ms while the user was
> describing 500 ms stalls, and each one sent the investigation after a
> phantom.

So each `IssueShot` carries its own `history` — a ring of `IssueFrameSample`
entries (frame/render/build/gpu ms, triangles, instances, resolve/draw ms, and
four `zone_*` timing lanes) **sampled AT CAPTURE**, i.e. the instant F9/F10's
pixels were grabbed, not when "File report" is pressed. `peak_frame_ms`/
`peak_render_ms`/`peak_build_ms` cover the case where even the ring has rolled
past the spike. The history ring records **raw frame cadence, never the HUD's
smoothed/EMA value** — the whole point is to see the spike the HUD hides.

One more consequence of capture-vs-submit timing: F9's **pixels** can be
seconds older than their **telemetry**, because the pixels are stamped at
drag-commit (when the region selection is finalized) while the frame the crop
comes from was frozen earlier, at the F9 press.

`state.json`'s `history_columns` lists **12** names —
`frame_ms, render_ms, build_ms, gpu_ms, triangles, instances_drawn, resolve_ms,
draw_ms, zone_vt_ms, zone_cull_ms, zone_skin_ms, zone_comp_ms` — and each row
in the `history` array has exactly **12** values in the same order
(`issue_reporter.cpp`'s `write_shot_json`). A reader that zips columns to
values positionally is safe.
