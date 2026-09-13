# Castle GI quality options — 2026-09-12

**Follow-up:** reduced diffuse GI now has guided lighting reconstruction, and
reflections/glass have an independent resolution control. The measurements
below describe the earlier shared-resolution implementation. See
[GI reconstruction and resolution split](gi-reconstruction-2026-09-12.md)
for the current controls and comparison.

The user asked to reduce GI quality and questioned whether full GI is needed.
The strongest practical option is to disable **diffuse bouncing only**, while
retaining full-resolution traced reflections, glass and direct shadows.
Existing sky ambient, local direct lighting, sun, emission and material detail
remain. The room loses warm bounced fill and has darker corners and floors.

## Measured options

Same CastleUpgraded hall at visible 1280×720 Native resolution on RTX 4090.
The four final runs use the same editor binary, four primary area-shadow
samples, reference POM, weighted secondary lighting, 15s warmup and 20s sampling.
`MATTER_VSYNC=0` selected MAILBOX (present mode 1) in all four runs.

| Mode | Median frame | p95 frame | Median FPS | Raw GPU median | Raw GI median |
|---|---:|---:|---:|---:|---:|
| Full-resolution GI | 38.488ms | 42.455ms | 25.98 | 38.531ms | 30.982ms |
| Half width/height, all indirect effects | 15.856ms | 18.658ms | 63.07 | 15.562ms | 7.483ms |
| Diffuse off; full-resolution reflections/glass | 8.326ms | 10.083ms | 120.10 | 8.128ms | 1.009ms |
| All traced indirect effects off | 6.265ms | 8.024ms | 159.61 | 6.104ms | no GI dispatch |

Artifacts: `C:/tmp/castle-gi-quality/{full,half,reflections,direct}-hall-unlocked/`.
Each contains the executable hash, exact command timeline, environment, perf
JSON, log and screenshot. All renderer windows were visible and unminimized.
The half-resolution window was foreground throughout; the other three were
background but visible. GPU clocks were not locked. Treat the numbers as
observed performance for this view, not universal FPS guarantees.

Earlier default-FIFO runs are retained as `reflections-hall` and `direct-hall`.
The latter was presentation/wait limited (31.3ms frames versus 9.3ms GPU), so it
does **not** show that GI-off is slower. The final MAILBOX runs remove that
particular presentation policy difference and report frame/GPU times separately.

## Quality and recommendation

Half resolution retains the room's warm indirect fill, but visibly breaks up
thin window details in the hall. All three indirect signals share the lower
resolution, and final composite currently uses nearest-neighbor sampling
without full-resolution depth/material-aware reconstruction. It is exposed
as an experiment, not selected as the recommended mode. A better low-GI mode
should reduce diffuse work independently and preserve sharp glass/reflection
coverage, or add appropriate reconstruction.

Diffuse-off at full resolution avoids that resolution artifact and preserves
traced scene reflections/refraction. It sacrifices indirect fill and color
bleeding. Turning the overall GI switch off saves another roughly 2ms in this
hall, but also removes traced scene reflections/refraction; glass falls back
to sky lighting. The remaining direct specular highlights do not replace
scene reflections.

Lowering a nonzero diffuse multiplier merely dims the same work. Exactly zero
activates the existing early ray skip. It still leaves the zero diffuse output
and its temporal/spatial passes scheduled; no claim is made that every diffuse
pipeline operation has been removed.

## Controls and implementation

The editor now exposes `render.gi.trace_scale` in the existing session-only
Tunables group, backed by the renderer's existing supported scale control.
Its label and tooltip state that reflections and glass are affected too.
The diffuse slider is labeled **Diffuse bounce strength**, and its tooltip
distinguishes brightness changes from the zero-work shortcut.

Recommended fast mode:

```text
set render.gi.enabled true
set render.gi.trace_scale 1
set render.gi.diffuse_multiplier 0
```

Restore full diffuse GI with `set render.gi.diffuse_multiplier 1`.
For the cheapest direct-lighting mode, use `set render.gi.enabled false`.
Half-resolution comparison uses `enabled=true`, `diffuse_multiplier=1` and
`trace_scale=0.5`. These settings are live and session-only, not global quality
defaults or persisted world edits.

`tools/castle_rt_perf.py` accepts `--diffuse-multiplier`, `--gi-trace-scale`
and `--vsync`. It records sampled renderer-window foreground/minimized/visible
state without repeatedly forcing focus. Omitting the new optional scale flag
keeps compatibility with older editor binaries.

Native MSVC RelWithDebInfo editor build passed. The new property was exercised
through visible FIFO runs, including resolution/history changes. Engine and
shader algorithms were not changed in this task; the prior native RT gates
remain their validation record. Python helper compilation and diff whitespace
checks passed. Gold/glass comparisons are captured separately under
`gold-comparison/`; the selected mode is left open for exploration.
