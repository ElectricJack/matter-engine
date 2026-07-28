# Issues

One directory per reported issue, written by the in-editor reporter
(`MatterEditor/src/issue_reporter.h`). Committed — the screenshots are the
evidence, and an issue without them is usually not reproducible.

```
issues/
  README.md
  3f9a1c02-7d41-4e88-b1a6-9c2f0e5d7a13/
    issue.md        the report (frontmatter + note + evidence)
    state.json      per-shot camera/counts + deep engine state at file time
    log-tail.txt    console lines leading up to the report
    shot-1.png      captures, cropped as selected
    shot-2.png
```

## Reporting

Capture first, describe second — that ordering is the whole point. By the time
a form is up the frame has moved on, and every field you must fill before
capturing is a reason not to bother.

| Key | What it does |
|---|---|
| **F9** | Freezes the screen immediately, then you drag a box over the part that's wrong. `Enter` keeps the whole screen, `Esc` cancels. |
| **F10** | Grabs the active viewport as-is, no drag. |

Either one drops the shot into the open report and brings the window up. Keep
hitting F9/F10 to add more; they accumulate until you file. Each shot gets an
optional caption you type afterwards, and a thumbnail in the window (hover for
a large version).

The crop for F9 comes out of the **frozen** frame, so an animating scene can't
slide out from under the selection while you drag. The reporter window hides
itself on the frame that's read back, so it never lands in its own evidence.
The report directory is created on the first shot — a capture you abandon
leaves nothing behind.

`MATTER_ISSUE_DIR` overrides the destination (default `../issues`, i.e. this
directory, since the editor is always launched from `MatterEditor/`).

## What the reporter does *not* ask for

There is no title, kind, severity or area field. Classifying a defect is work
the ingestion pass can do by reading the report, and asking for it at capture
time buys a slower capture and worse-labelled data — nobody picks a careful
severity mid-flight. The reporter collects what only the person at the keyboard
knows: the pixels, the machine state, and a sentence.

Directories are GUIDs for the same reason. Naming costs thought at exactly the
wrong moment, and a name chosen in the first ten seconds outlives the
misunderstanding that produced it. Ordering lives in the frontmatter.

## Schema

```yaml
---
id: 3f9a1c02-7d41-4e88-b1a6-9c2f0e5d7a13
world: Meadow
shots: 2
status: unprocessed
reported: 2026-07-27T21:14:03Z
---
```

Body sections: **Report** (the note), **Repro**, **Evidence**, **Acceptance**.

The **Repro** section is a runnable command — `MATTER_WORLD` plus a
`MATTER_CAM` reconstructed from the camera at the first shot. Shots taken from
a different viewpoint get their own launch line. Launch it from a shell that
has **not** exported the MSYS2 UCRT64 PATH: env vars only reach a native exe
through its own prefix, and an MSYS bash swallows them.

**Acceptance** is left as an explicit `_TODO —_`. It is the field that makes an
issue safe to hand to an agent, and the reporter cannot know it.

## Replaying a shot

Every shot records enough state to be taken again: world, full camera
(pose *and* projection), framebuffer size, viewport rect, crop rect, and the
render toggles that change pixels. `MatterEditor/src/shot_replay.h` reads it
back and reproduces the capture headlessly:

```bash
cd MatterEditor && MATTER_REPLAY=../issues/<guid>/state.json MATTER_REPLAY_SHOT=2 MATTER_REPLAY_OUT=/tmp/after.png ./build/windows/editor.exe
```

The output is cropped to the recorded rect, so it is directly comparable with
the shot it came from. This is what makes a visual defect verifiable: replay
before the fix, replay after, diff the PNGs
(`python3 MatterEngine3/tools/img_diff.py before.png after.png`) instead of
arguing about whether it looks right.

**Replay is bit-exact**, because each shot also stores the ImGui layout
(`shot-N.png.layout.ini`) and the replay restores it while leaving the ambient
`imgui.ini` untouched. Measured on CornellBox with a crop inside the 3D
viewport: three replays, two from different working directories, produced
identical images — 0 differing pixels.

The viewport rect is entirely a function of the docked layout, so recording the
window size alone is not enough. Before layout pinning, consecutive replays
differed by ~27% of pixels — each run saved a slightly different layout on exit
and the next loaded it, resizing the viewport and changing the whole render.
That looked exactly like ray-tracing noise and wasn't.

The one thing that must match between two images you compare is the settle
count, since the denoiser accumulates: 30 vs 90 frames differs by 27.8%. Leave
`MATTER_REPLAY_SETTLE` alone, or set it for both sides.

Not reproducible: DLSS (temporal — replay forces Native),
Play-mode animation phase (pause before capturing anything you mean to diff),
and streaming residency. `MATTER_WORLD` still overrides, so a shot can be
deliberately re-aimed at another world.

**UI crops behave differently.** ImGui draws deterministically, so a crop of
static panels is bit-exact replay to replay — 0 differing pixels measured over a
410×560 left-column crop. Live telemetry is the exception: a crop of the HUD
column differed in 0.07% of pixels (max delta 186), all of it on the twelve rows
carrying fps/ms/counter text.

But UI crops are **not** robust to window size. Panels keep their widths while
the viewport absorbs extra space (1280×720 → 1600×900 grew the viewport from
400×340 to 720×520), and panel heights track the window, so content shifts
vertically — the same crop differed in 24.7% of pixels between those two sizes.
A UI crop is comparable only at the size and layout it was taken at.

| Crop | Layout restored (normal replay) | Layout differs |
|---|---|---|
| Inside the 3D view | bit-exact | not comparable — warns |
| Static panels | bit-exact | 24.7% differ at 1600×900 vs 1280×720 |
| HUD / telemetry | differs on ~12 text rows (live counters) | not comparable |

Replay also validates what it cannot control. If the framebuffer or the
viewport differs from the recording — a different window size, or a different
panel layout in `imgui.ini` — it warns loudly and remaps the crop through
`viewport_uv` so the same content stays in frame. `MATTER_REPLAY_STRICT=1` turns
those warnings into a hard failure, which is what an automated acceptance check
should use: the dangerous outcome is not a failed replay but a plausible-looking
one that was framed differently.

Each shot also records `viewport_uv` and `overlaps_viewport`, so a reader can
tell whether a crop covers the 3D view, the panels, or straddles both — and a
replay at a different resolution can crop the equivalent slice of the view
rather than the same absolute pixels.

## Ingestion

`status: unprocessed` is the hook. Reporting is deliberately cheap and lossy;
ingestion is where the rigor gets added. A pass over unprocessed reports should:

1. **Read** `issue.md`, the shots, and `log-tail.txt`.
2. **Classify** — add `kind` (`bug` · `change` · `revision`), `severity`
   (`blocker` · `major` · `minor` · `polish`), and `area`. Give it a real
   title.
3. **Rename** the directory from its GUID to `<area>-<short-slug>`. Keep `id:`
   unchanged — that's the stable handle; the directory name is for humans.
4. **Locate** the owning subsystem and name the likely files.
5. **Write Acceptance**, and split the note into Symptom/Expected if it carries
   both.
6. **Merge duplicates** — several captures of one defect are one issue.
7. Set `status: triaged`.

Then fan out: one issue → one worktree → one agent → one branch, batched by
subsystem so no two agents share a file. The agent's contract is *reproduce
first* — prove the repro fails before touching anything — then fix, then prove
Acceptance passes and the test baseline did not move. An agent that cannot
reproduce reports back instead of fixing.

Before any of that, record the known-red baseline: several suites fail on
`main` itself (hardcoded POSIX `/tmp` paths), and an agent that does not know
which will chase ghosts.
