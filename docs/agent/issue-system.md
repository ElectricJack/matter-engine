# Issue System

The editor's in-app defect capture and filing system, and how to consume what
it writes.

Source files: `MatterEditor/src/issue_reporter.h`/`.cpp` (writer — ImGui-free,
so the schema and crop math are headlessly testable; see
`MatterEditor/tests/test_issue_reporter.cpp`), `issue_reporter_panel.cpp` (the
ImGui dialog), `shot_replay.h`/`.cpp` (reader/replayer), `main.cpp`
(orchestration: hotkeys, capture readback, `write_issue_report` call site).

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

**There is no automatic filing.** `write_issue_report` has exactly one call
site in the codebase (the "File report" button handler in `main.cpp`) — nothing
files a report automatically on device-lost, a validation error, or a perf
regression. Those conditions are visible in logs/stats but require a human (or
an agent watching the log) to press F9/F10 and file.

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
