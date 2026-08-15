# Editor/Engine Control Surface

The complete external control surface of `MatterEditor/build/windows/editor.exe`
(and the Linux build) for driving it from scripts/agents rather than a mouse.

## a) How control works

`main()` in `MatterEditor/src/main.cpp` takes **no arguments** — there is no
argv-based CLI. Every startup and runtime behavior is driven by:

- **Environment variables**, read once at startup (camera, world, screenshot,
  replay, streaming/bake/VT tuning — see §c), or polled per-frame (`editor_props`
  `.env()` bindings for lighting/atmosphere/DLSS).
- **One command file**, `MATTER_CMD_FIFO` (§b), polled every frame for new lines
  after startup.

Output is `stdout`/`stderr` text (one line per event/ack/error — see the verb
table in §b for exact wording) plus PNG sidecars: any screenshot write also
writes `<path>.done` once the PNG is on disk, so a script can poll for the
sidecar instead of racing the file write.

Two readiness lines matter for scripting:

- `MATTER_CMD_FIFO: polling command file <path>` (Windows) or
  `MATTER_CMD_FIFO: listening on <path>` (Linux/POSIX `mkfifo`) — printed once
  the FIFO is open, immediately after startup. Printing happens before the bake
  is ready; it only means the command channel is live.
- `viewer: bake ready` — printed once the initial world bake completes and the
  viewer is actually drawing something. This is the line every scripted harness
  polls the log for (`MatterEngine3/tools/viewer_shots.sh`,
  `MatterEngine3/tools/seam_suite.sh`) before sending any commands.

When `MATTER_CMD_FIFO` is set, stdout is switched to fully unbuffered
(`std::setvbuf(stdout, nullptr, _IONBF, 0)`) specifically because native Windows
stdout is otherwise fully buffered under a redirecting harness and FIFO
automation depends on acknowledgements arriving promptly.

## b) The command FIFO

`MATTER_CMD_FIFO=<path>` names an **append-only polled file** on Windows
(`CreateFileA` + periodic `ReadFile` from a tracked offset — there is no POSIX
FIFO on Windows) or a real `mkfifo`'d named pipe on Linux. A script/agent
appends newline-terminated command lines to this file; the editor tails new
bytes once per frame, splits on `\n`, and dispatches each recognized line.

Implementation: `MatterEditor/src/viewer_commands.h` declares the typed command
structs and the FIFO line parser (`parse_fifo_line` + the inline `sscanf`/prefix
dispatch in `main.cpp`); `MatterEditor/src/main.cpp`'s frame loop reads the file,
converts each line to a typed command, and calls
`matter::evt::CommandRegistry::dispatch()` (cross-thread-safe, ticketed) rather
than handling most verbs inline. The registry is pumped once per frame
(`registry.pump(app_lane, 5.0)`) **before** `begin_frame()`/the camera snapshot,
so a `cam`/`budget` command sent this "frame" (FIFO line) affects the render
that same engine frame. UI-triggered actions (buttons) reach the same command
handlers via `registry.execute()` (synchronous) — FIFO and UI are two front
ends onto one command registry.

### Verb table

| Verb | Grammar | Effect |
|---|---|---|
| `cam` | `cam ex ey ez tx ty tz` (6 floats) | Sets camera eye/target immediately. |
| `shot` | `shot <path>` | **Blocking.** Settles 3 frames (`instances_drawn > 0` gated), writes the PNG, then writes `<path>.done`. No later FIFO line dispatches until the write completes (§ Timeline semantics below) — a bounded 30s deadman (§ shot deadman) prevents a world that never settles from hanging the timeline forever. |
| `shot_now` | `shot_now <abs .png>` | **Blocking**, same as `shot` (landed alongside it for consistency — neither spelling needs a trailing `wait_frames` to be safe to follow with another timeline line). Queued via `FifoPresentSequencer`; captured on the next **presented** frame with no settle wait. Path must be an absolute, filesystem-safe `.png` path (`fifo_safe_absolute_png_path` rejects reserved Windows names, `..`, control chars, etc). Also writes `<path>.done`. Covered by the same shot deadman as `shot`. |
| `wait_frames` | `wait_frames <n>` (positive uint32) | Completes once `n` more frames have **presented successfully** (`FifoPresentSequencer::advance` only advances on `presented == true`). A **blocking wait**: no later FIFO line dispatches until it releases (§ Timeline semantics below). A dispatch rejected synchronously (no handler / registry shut down / queue full) prints `wait_frames: dispatch failed (<status>)` and does NOT arm the block. |
| `wait_idle` | `wait_idle <seconds> [timeout_seconds]` (`<seconds>` float > 0; `[timeout_seconds]`, if given, float > 0) | **Blocking.** Releases once `resident_sectors` has held steady for `<seconds>` of wall-clock time AND the bake is ready; prints `idle: settled after N.Ns`. If the optional `timeout_seconds` elapses first, releases anyway and prints `idle: timeout after N.Ns` (not a failure — the script continues, mirroring `wait_event`'s timeout). |
| `wait_event` | `wait_event <name> [timeout_seconds]` | **Blocking.** Releases when the named engine event fires (prints `event: <name>`) or the optional timeout expires (prints `event: <name> timeout after Xs` and continues — a timeout is not a failure). See the event-name table below. |
| `world` | `world <name>` | Switches the active world (same case-insensitive resolution as `MATTER_WORLD`). **Not** itself a blocking wait — pair it with `wait_idle` to sequence a multi-world sweep. Prints `world: unknown '<name>'` for an unrecognized name. |
| `render_path` | `render_path raster\|native_rt` | Switches the render path. `native_rt` fails loudly (`render_path: native_rt unavailable`, command result `failed`) if the device has no ray tracing. |
| `history_reset` | `history_reset` (no args) | Calls `session->request_atmosphere_history_reset()`. |
| `stats` | `stats <label>` | Arms one `STATS,<label>,...` row (see below), emitted on the next frame. |
| `budget` | `budget <float>` | Sets `stats.pixel_budget`, clamped to `[0.05, 4.0]`. Shorthand for `set viewer.budget.pixel_budget <f>`. |
| `set` | `set <group.path>.<field> <value>` | Generic property setter over `editor_props.registry()`. `value` is the rest of the line, unquoted, so it may contain spaces/commas. The path splits on the **last** `.` — everything before is the group path (itself dotted), everything after is the field name. Draft-only groups (`RequiresReload`) write to a draft copy and report `(draft; \`reload\` to apply)`. Env-forced fields (bound via `.env()`, see §c) refuse with `set: <path> is forced by <ENV_VAR>; ignored`. Unknown paths and unparsable values also report, never fail silently. |
| `get` | `get <path>` | Prints `get: <path> = <value>`, or `get: unknown property '<path>'`. |
| `dlss` | `dlss native\|quality\|balanced\|performance` | Resolves against the same label table the DLSS combo box uses, then writes through `EditorProps::set_dlss_mode` — identical effect to `set render.gpu.dlss_mode <mode>`. |
| `workbench` | `workbench <module>` | Opens `<module>` in the Bake Lab's Workbench isolation session (same as the Asset Browser's "Open in Workbench" button); finds the owning project by probing `objects/<module>.js`. |
| `reveal` | `reveal <module>` | Selects + focuses the camera on `<module>`'s baked root in the **active** production world (Asset Browser "Reveal"). Reports (not an error) when the module isn't loaded in the current world. |
| `reload` | `reload` (no args) | Reloads the active world in place. |
| `wireframe` | `wireframe` \| `wireframe toggle` \| `wireframe on` \| `wireframe off` | Toggles/sets the debug wireframe flag. Fails closed (`wireframe: unavailable (...)`) on devices without `VK_POLYGON_MODE_LINE`. |
| `hiz` | `hiz <anything>` | **Deprecated stub.** Always prints `hiz: removed -- use \`set viewer.debug.occlusion_draw_cull true\`` and does nothing; kept only so old scripts get an answer instead of "unrecognized". |
| `quit` | `quit` (no args) | Sets the quit flag. **Deferred**, not immediate: the process exits once no FIFO screenshot capture is still in flight, so a `quit` right after a `shot` (no intervening wait) never truncates that capture. Waits on the exact same in-flight-shot condition the `shot`/`shot_now` block does, so it too is bounded by the shot deadman (§ shot deadman) rather than hanging forever against a capture that can never complete. |
| `timescale` | `timescale <f>` | Sets simulation time scale, clamped to `[0.05, 2.0]` (`kToolbarMinTimeScale`/`kToolbarMaxTimeScale` in `toolbar_panel.h`); out-of-range values are rejected with the bounds printed. |
| `play` / `pause` / `step` / `sim stop` | exact tokens | Drives `SimulationControl` — the same transport the toolbar buttons call. `sim stop` also clears selection. |

Any unrecognized line prints `cmd: unrecognized '<line>'`. **Every failure mode
of `set`/`get`/`render_path`/`dlss`/etc. is printed to stdout, never silent** —
this is intentional (see the `set`/`get` design note in `viewer_commands.h`: a
near-miss like a mistyped group path used to read as "group not in registry"
and sent an investigation down the wrong path).

The `stats` command arms an append-only CSV-ish line printed the next frame:

```
STATS,<label>,frame_ms,resolve_ms,build_ms,draw_ms,instances_active,raster_batches,raster_tris,culled_clusters,gpu_occlusion_culled,vt_variants,vt_rejected_variants,vt_max_variants,vt_mesh_MiB,...,vk_gpu_total_ms,vk_gpu_cull_ms,vk_gpu_gbuffer_ms
```

New fields are only ever **appended** to the end (scripts parse by position) —
see the comment at the `STATS,` `printf` in `main.cpp` for the exact current
field list, which has grown twice (task 14's timing lanes, M4's GPU-timestamp
lanes) without breaking older parsers.

## c) Environment variables

Grouped by area. All are read via `std::getenv("MATTER_...")` unless noted as an
`editor_props` `.env()` binding (§ note at the end).

### Editor/QA (startup + capture control)

- `MATTER_CMD_FIFO` — command file path (§b).
- `MATTER_WORLD` — world/scene name to open at startup.
- `MATTER_CAM` — `"ex,ey,ez,tx,ty,tz"`, one-shot initial camera pose.
- `MATTER_CAM_PATH` — path to a scripted fly-through file, one pose per line
  (`eye_x eye_y eye_z target_x target_y target_z`; `#`-comments and blanks
  ignored), consumed **one pose per rendered frame** (frame-indexed, not
  wall-clock — this is what makes M1d-style determinism gates possible).
  - `MATTER_CAM_PATH_EXIT=1` — quit once the path (plus its FIFO drain tail) ends.
  - `MATTER_CAM_PATH_WARMUP=<n>` — frames to hold at the first pose after the
    world is drawable; default **30**.
  - `MATTER_CAM_PATH_SETTLE=<seconds>` — wall-clock seconds of unchanged
    `resident_sectors` required before the path starts (0 = off, frame-warmup
    only). Deliberately wall-clock, not a frame count, so it means the same
    thing at any framerate.
- `MATTER_SCREENSHOT=<path>` — capture-then-quit: writes one PNG after settling
  and exits.
  - `MATTER_SCREENSHOT_SETTLE=<n>` — frames to hold before capture; default **3**.
    A streamed world needs far more than 3 to fill in — this exists because an
    un-tuned streamed screenshot photographs an empty horizon.
- `MATTER_REPLAY=<state.json path>` — replay a single recorded issue shot
  headlessly (see `docs/agent/issue-system.md`).
  - `MATTER_REPLAY_SHOT=<n>` — 1-based shot index, default 1.
  - `MATTER_REPLAY_OUT=<path>` — output PNG path, default `replay.png`.
  - `MATTER_REPLAY_SETTLE=<n>` — frames to hold before capture; default **90**
    (RT worlds accumulate through a temporal denoiser, so replays default much
    higher than a live screenshot so two replays are comparable to each other).
  - `MATTER_REPLAY_STRICT=1` — turn any framebuffer/layout mismatch into exit 1,
    for use in automated diff gates.
- `MATTER_ISSUE_DIR` — overrides where issue reports are written/read (see
  `docs/agent/issue-system.md`).
- `MATTER_HIDE_UI` — hides all ImGui panels at startup.
- `MATTER_CAPTURE_LIGHTING_UI` — capture-only aid: focuses the Lighting panel
  for automated verification screenshots; inert outside a capture.
- `MATTER_TIME_SCALE` — initial simulation time scale.
- `MATTER_CACHE_ROOT` — overrides the bake cache root.
- `MATTER_LIVE_EDIT` — enables live-edit (hot script reload) file watching.
  **The functional backend is Linux-only** (`InotifyWatcher`); the Windows
  backend (`WinDirWatcher`, `MatterEngine3/src/file_watcher.h`) is a stub.
- `MATTER_VK_VALIDATION=1` — opt in to Vulkan validation layers (off by default
  so machines without the Vulkan SDK aren't broken by default).
- `MATTER_TEST_RESIZE` — exercises a forced window resize once baked, for resize
  regression testing.
- `MATTER_FORCE_LOD_TINT` — forces the LOD-rung debug tint view on.
- `MATTER_SEAM_TRACE=1` — turns on the terrain seam welder's in-app accounting
  trace (level gaps, drawn-level violations, build errors, sign conflicts);
  zero-cost to collect, used by `seam_suite.sh` check 3.
- `MATTER_PROFILE_TRACE=<path>` — on exit, dumps the profiler's FrameRecord tail
  as a Chrome trace (`chrome://tracing`-loadable) — the same data every issue
  report embeds as `profile_tail.json`.
- `MATTER_PERF_OUTPUT`, `MATTER_PERF_WARMUP_SECONDS`, `MATTER_PERF_SAMPLE_SECONDS`
  — headless perf run (§ recipe 8 in the QA cookbook). **Must be set together**;
  setting any subset is a fatal startup error
  (`read_perf_run_config` in `main.cpp`).
- `MATTER_HIZ` — dead. Legacy env var, retained only to print
  `MATTER_HIZ: not available in Vulkan milestone; ignored` instead of silently
  doing nothing.

### Lighting/sky (property-registry `.env()` bindings)

These are bound through `matter::props` `.env()`/`.env_negated()` — an `editor_props.cpp`
mechanism distinct from a raw `getenv()` call (see the note at the end of this
section). A FIFO `set` on one of these fields refuses with "forced by <VAR>".

`MATTER_SUN_AZIMUTH_DEG`, `MATTER_SUN_ELEVATION_DEG`, `MATTER_SUN_SIZE_DEG`,
`MATTER_SUN_SHADOW_SAMPLES`, `MATTER_ATMOSPHERE_SEA_LEVEL_Y`,
`MATTER_ATMOSPHERE_RAYLEIGH_SCALE`, `MATTER_ATMOSPHERE_MIE_SCALE`,
`MATTER_ATMOSPHERE_MIE_ANISOTROPY`, `MATTER_ATMOSPHERE_OZONE_SCALE`,
`MATTER_ATMOSPHERE_GROUND_ALBEDO`, `MATTER_DAY_AMBIENT_MULTIPLIER`,
`MATTER_TWILIGHT_AMBIENT_MULTIPLIER`, `MATTER_SKY_IRRADIANCE_MULTIPLIER`,
`MATTER_SUNSET_DIRECT_RATIO`, `MATTER_VOLUMETRICS`, `MATTER_FROXEL_XY_SCALE`,
`MATTER_FROXEL_DEPTH_SLICES`, `MATTER_CLOUD_SCATTER_ORDERS`,
`MATTER_DLSS_MODE`, `MATTER_DISABLE_VK_RT` (negated — `=1` clears
`render.gpu.ray_tracing`).

### Streaming

`MATTER_STREAM_RINGS`, `MATTER_STREAM_INFLIGHT`, `MATTER_STREAM_HYSTERESIS`,
`MATTER_STREAM_FAIL_COOLDOWN`, `MATTER_STREAM_FIRST_RUNG`,
`MATTER_STREAM_NO_EVICT`, `MATTER_STREAM_NO_STAGING`, `MATTER_STREAM_NO_LATERAL`,
`MATTER_STREAM_SKIP_PART_WRITE`, `MATTER_STREAM_STAGE_FROM_MEMORY`,
`MATTER_STREAM_STAGE_VERIFY`, `MATTER_STREAM_FILL_PROFILE`,
`MATTER_STREAM_PARK_PROFILE`, `MATTER_STREAM_BAKE_PROFILE`,
`MATTER_STREAM_PUBLISH_PROFILE`, `MATTER_STREAM_PREBUILD_VERIFY`,
`MATTER_STREAM_TICK_TRACE`, `MATTER_NO_MERGE_DEFER` (`matter_engine.cpp`,
`MatterEngine3/src/streaming/sector_streaming_coordinator.cpp`,
`MatterEngine3/src/sector_streamer.cpp`) — sector streaming tuning/profiling/
fault-injection knobs, mostly off by default.

`MATTER_TERRAIN_LOD`, `MATTER_NESTED_SECTORS`, `MATTER_VOLUMETRIC_SECTORS` —
select the streaming/LOD scheme. `MATTER_NO_SEAM_WELD`, `MATTER_NO_SEAM_WELD_DRAW`
(disable the runtime seam welder / just its draw pass — the latter is
`seam_suite.sh`'s check-5 knob), `MATTER_SEAM_VERIFY`, `MATTER_SEAM_CHURN`.

### Virtual Texture (VT)

Budget/sizing fields are property-registry `.env()` bindings, declared together
in `MatterEngine3/include/matter/vt_budgets.h`: `MATTER_VT_MAX_VARIANTS`
(read-only after launch), `MATTER_VT_FILLS_PER_FRAME`,
`MATTER_VT_TAIL_FILLS_PER_FRAME`, `MATTER_VT_ENRICH_PER_FRAME`,
`MATTER_VT_MESH_BUDGET_MB`, `MATTER_VT_INDIRECTION_MB` (read-only),
`MATTER_VT_POOL_MB` (read-only — pool is allocated once at renderer init),
`MATTER_VT_POOL_PAGES` (read-only, kept for backwards compatibility with
pre-`MATTER_VT_POOL_MB` overrides),
`MATTER_VT_EVICT_PROTECT_FRAMES`, `MATTER_VT_LINGER_FRAMES`,
`MATTER_VT_REQUESTS_PER_FRAME`, `MATTER_VT_REQUEST_BUDGET_MS`,
`MATTER_VT_QUEUE_CAP`, plus six Tier-2 hemisphere-AO enrichment settings
(`MATTER_VT_ENRICH_SAMPLES`, `_STRENGTH`, `_CAP_TEXELS`, `_CAP_METERS`,
`_MIN_AO`, `_AS_CACHE` — the last read-only).

Raw `getenv()` VT toggles (not registry-bound): `MATTER_VT_UNIFY`,
`MATTER_VT_CHART_LOG`, `MATTER_VT_EAGER`, `MATTER_VT_DISABLE`, `MATTER_VT_WARP`,
`MATTER_VT_MEM_LOG`, `MATTER_VT_TAPE_GPU`, `MATTER_VT_DEBUG_GENERATIONS`
(`vt_residency.cpp` — arms per-page-release reuse-discipline assertions).

### Bake/LOD

`MATTER_BAKE_PROFILE`, `MATTER_LOD_BAKE_PROFILE`. `MATTER_CONTOUR_SEAMS` — the
contour mesher's own-vs-welder mode; **default ON since 2026-08-13**
(`MatterEngine3/src/bake_mode.h`); `MATTER_CONTOUR_SEAMS=0` is the documented
rollback to the old overlap+welder path, kept supported while the welder still
exists. `MATTER_FLATTEN_LADDER`, `MATTER_FLATTEN_PEAK`, `MATTER_FLATTEN_RETAIN_MB`,
`MATTER_LOD_CASCADE`, `MATTER_LOD_MAX_MESH_RUNGS`, `MATTER_LOD_TRACE`,
`MATTER_IMPOSTOR`, `MATTER_IMPOSTOR_CELL_PX`, `MATTER_IMPOSTOR_DEBUG`,
`MATTER_IMPOSTOR_DISTANCE`, `MATTER_GPU_JOB_SLOW_MS`.

### Vulkan

`MATTER_VSYNC`, `MATTER_VK_ROBUSTNESS`, `MATTER_VK_SMOKE_MODE` (§ QA cookbook
recipe 6), `MATTER_VK_STATIC_RESERVE_CLUSTER_MB` / `_VERTEX_MB` / `_INDEX_MB`
(static-buffer reservation sizing, `vk_scene_renderer.cpp`), `MATTER_RASTER_CULL`,
`MATTER_RT_WALK_ALPHA_TEST`. Fault-injection family (test builds only):
`MATTER_VK_TEST_FORCE_RT_UNAVAILABLE`, `MATTER_VK_TEST_END_FRAME_FAULT`,
`MATTER_VK_TEST_FORCE_CLEANUP_UNPROVEN`,
`MATTER_VK_TEST_FORCE_IMMEDIATE_WAIT_AMBIGUOUS`,
`MATTER_VK_TEST_FORCE_IMMEDIATE_COMPLETED_FAILURE`. A longer tail of dev-only
diagnostic/tuning vars exists beyond this list (temporal buffer recycling,
diagnostic materials, vector growth logging) — grep `vk_context.cpp`,
`vk_resources.cpp`, `vk_perf.h`, `vk_scene_renderer.cpp` for the current set.

### Misc

`MATTER_SHADER_DIR` (override the embedded-shader search path),
`MATTER_TILESET_DUMP_PNG` (dump `.gtex` bake intermediates as PNG — see the QA
cookbook's `.gtex` determinism recipe), `MATTER_SCRIPT_PROFILE` (JS DSL
profiling), `MATTER_PROFILE` (ProfileLib master enable, on unless `=0`),
`MATTER_PROFILE_LOG=1` (periodic aggregate stderr profiler report, 2 s interval).

**Two distinct mechanisms** back all of the above: most engine-internal vars
(streaming, VT toggles, bake, most Vulkan ones) are a plain `std::getenv()` call
at the point of use — no registry entry, no FIFO visibility, read once or
polled directly. The lighting/sky/DLSS group is instead a `matter::props`
`.env()`/`.env_negated()` **binding**: the property is visible to FIFO `get`,
and a FIFO `set` on it is refused with an explicit "forced by" message rather
than silently losing to the env value on the next frame.

## d) Events (for context)

`matter::evt::Hub` (`MatterEngine3/include/matter/event/event_hub.h`) is the
in-process pub-sub core; `matter::evt::CommandRegistry`
(`MatterEngine3/include/matter/event/command.h`) is the deliver-once command
layer the FIFO and UI both dispatch/execute through (§b). Declared event types
today: `bake.started`, `bake.part_done`, `bake.finished`, `bake.error`
(`MatterEngine3/include/matter/events/bake_events.h`), `stream.refine_tile`
(`.../events/stream_events.h`), `cmd.completed`/`cmd.failed`
(`.../event/command.h`), `scene.rows_upserted`/`scene.rows_removed`
(`.../scene/scene_events.h`), `error.live_edit`/`error.part_instance`/
`error.part_instance_clear` (`.../events/error_events.h`), and `phys.step`
(`.../events/physics_events.h`). **There is no external subscribe channel
today** — these are consumed in-process (editor panels, tests); nothing in the
FIFO grammar above lets a script subscribe to them directly.

### Timeline semantics: blocking waits (landed 2026-08-14)

`wait_frames`, `wait_idle`, `wait_event`, `shot`, and `shot_now` are
**blocking waits**: while one is armed, `main.cpp` stops popping lines off
`fifo_pending_lines` (the queue already split out of the raw FIFO bytes)
until it releases. Reading more bytes off the command file/pipe still
happens every frame regardless — only *dispatch* is gated — so a
pre-written multi-line command file behaves as a timeline script rather than
"every buffered line lands in one frame" (the pre-2026-08-14 drain-loop
behavior). At most one blocking wait is ever in flight at a time. Concretely:

- **`wait_frames <n>`** now blocks every later line in the file, not just
  screenshot captures — a `set`/`cam`/`shot` line written after a
  `wait_frames` cannot dispatch before it completes. The frame-count release
  itself is still `FifoPresentSequencer`'s (unchanged): it advances only on a
  successfully presented frame, and prints
  `wait_frames: complete <n> frame_serial=<serial>` on release.
- **`wait_idle <seconds> [timeout_seconds]`** releases once
  `frame_stats.resident_sectors` has held the same value for `<seconds>` of
  wall-clock time *and* the bake is ready (mirrors `MATTER_CAM_PATH_SETTLE`'s
  plateau logic — wall time, not a frame count, because bake progress is
  itself wall-time rate-limited). On release it prints `idle: settled after
  N.Ns` (the elapsed time since the `wait_idle` line was parsed, not the
  settle duration itself). The optional `timeout_seconds` (landed alongside
  the `shot` deadman below) mirrors `wait_event`'s: if it elapses before
  `resident_sectors` ever settles, the wait releases anyway and prints `idle:
  timeout after N.Ns` — not a failure, the script continues past it.
- **`shot <path>` / `shot_now <abs .png>`** now block the same way (landed
  2026-08-14, alongside the `wait_idle` timeout above): without this, `shot
  a.png` / `set X` / `shot b.png` with no intervening wait dispatched `set`
  before `a.png` was actually written, so `a.png` came out showing the
  post-`set` state instead of what was on screen when it was requested.
  `shot` releases once its 3-frame settle (`instances_drawn > 0` gated) has
  written the PNG and its `.done` sidecar; `shot_now` releases once its
  `FifoPresentSequencer`-queued capture has done the same (normally the very
  next presented frame). **Shot deadman**: a world where presents never
  succeed, or where `instances_drawn` never goes positive, would otherwise
  hang a blocked timeline (and a deferred `quit`, which waits on the same
  in-flight-shot condition) forever. Checked every loop iteration
  (independent of whether `begin_frame` even succeeds that frame, so it
  cannot itself get stuck behind the hang it exists to break): if a shot has
  not completed **30 seconds** after being armed, it prints `shot: timeout,
  abandoned <path>`, abandons it, and releases the block (and any pending
  `quit`).
- **`wait_event <name> [timeout_seconds]`** subscribes to one named event and
  releases when it fires, when an optional timeout expires, or — for a
  session-scoped name — when the session changes out from under it. The
  supported names, and which hub they subscribe on
  (`main.cpp`'s `fifo_begin_wait_event`):

  | Name | Hub | Session-scoped |
  |---|---|---|
  | `bake.started` | session (`session->events()`) | yes |
  | `bake.finished` | session | yes |
  | `bake.part_done` | session | yes |
  | `bake.error` | session | yes |
  | `stream.refine_tile` | session | yes |
  | `cmd.completed` | `app_hub` | no |
  | `cmd.failed` | `app_hub` | no |

  `cmd.*` lives on `app_hub`, which outlives world switches, so it is never
  session-scoped. `bake.*`/`stream.*` subscribe on the session's own hub,
  which is destroyed on a world switch/reload — if the session or generation
  changes while one of those is pending, the wait releases early rather than
  hanging forever, printing `event: <name> aborted (session changed)`. An
  unrecognized name prints `wait_event: unknown event '<name>'` and does not
  block. Release prints `event: <name>` (fired) or
  `event: <name> timeout after Xs` (timed out — this is a normal, non-error
  release; the script continues). A `wait_event` is not itself the same as
  `wait_idle` — pair the two (`wait_event bake.finished` then `wait_idle
  <settle>`) when a script needs both "the bake started/finished" and "nothing
  is still moving" as separate checkpoints. Each arm carries its own
  generation stamp, checked inside the callback before it sets the fired
  flag, so a callback still in flight from a *previous* arm (`bake.*`/
  `stream.*` fire on the bake worker thread, concurrently with the main
  thread unsubscribing and rearming) can never satisfy a later, unrelated
  `wait_event`.
- **`world <name>`** switches the active world (case-insensitive, same
  resolution as `MATTER_WORLD`) via the same intent-recording path `reload`
  uses — the actual session destroy/recreate runs at the post-frame seam, not
  mid-parse. It is deliberately **not** itself a blocking wait; sequence a
  multi-world sweep as `world X` → `wait_idle <n>` → `shot ...` → `world Y` →
  ...
  **Trap**: `world X` immediately followed by `wait_event bake.*` on the very
  next line does **not** work — `wait_event`'s `bake.*`/`stream.*` names
  subscribe on the *current* session's hub (§ table above) at the moment the
  `wait_event` line is parsed, but `world`'s actual session destroy/recreate
  is deferred to the post-frame seam, so by the time `wait_event` runs it
  either subscribes to the outgoing (about-to-die) session or, once the seam
  has already run, misses the new session's `bake.started` entirely if the
  bake had already begun. Pair `world` with `wait_idle <n>` instead (as
  above) to wait out a world switch.
- **`quit`** is deferred, not immediate, for the same reason `wait_frames` was
  extended to block everything: a `quit` line right after a `shot` (with no
  intervening wait) must not truncate that capture. The main loop sets a
  pending-quit flag and only actually exits once `shot_settle == 0` and no
  `shot_now` capture is still queued in `FifoPresentSequencer`.

### Log markers: what to grep for

Exact `printf` strings (verified against `main.cpp`; re-check there if this
table and the binary ever disagree — the table is descriptive, the source is
authoritative). All go to stdout unless noted.

| Marker | Exact format string | Meaning |
|---|---|---|
| wait_frames complete | `wait_frames: complete %u frame_serial=%llu` | The `wait_frames <n>` block released — `n` more frames presented. |
| wait_frames dispatch failed | `wait_frames: dispatch failed (%s)` | The dispatch was rejected synchronously (no handler / registry shut down / queue full); the block was never armed. |
| idle settled | `idle: settled after %.1fs` | `wait_idle` released because `resident_sectors` held steady long enough and the bake is ready. |
| idle timeout | `idle: timeout after %.1fs` | `wait_idle`'s optional `timeout_seconds` expired first; released anyway, not a failure. |
| event fired | `event: %s` | `wait_event <name>` released because `<name>` fired. |
| event timeout | `event: %s timeout after %.1fs` | `wait_event`'s optional `timeout_seconds` expired first; released anyway, not a failure. |
| event aborted | `event: %s aborted (session changed)` | A session-scoped `bake.*`/`stream.*` wait released early because the world switched/reloaded out from under it. |
| event unknown | `wait_event: unknown event '%s'` | The name isn't in the supported-names table (§ above); does not block. |
| shot timeout | `shot: timeout, abandoned %s` | The shot deadman fired — a `shot`/`shot_now` sat unresolved past 30s and was abandoned; the block (and any pending `quit`) released anyway. |
| shot_now queued | `shot_now: queued %s` | `shot_now` was accepted and queued in `FifoPresentSequencer`; the actual write comes later (see "screenshot written to" below). |
| screenshot written | `screenshot written to %s` | Either `shot` or `shot_now`'s PNG was actually written to disk — the reliable "this shot happened" line for either verb (`shot_now: queued` only means it was accepted, not that it completed). |
| bake ready | `viewer: bake ready` | The world bake finished and the viewer is actually drawing — the line every scripted harness polls the log for before sending commands (§a). |
| `.done` sidecar | *(no log line)* | Not a printed marker — a **filesystem artifact**: `<path>.done` is created (empty file) immediately after "screenshot written to `<path>`" for both `shot` and `shot_now`. Poll for the file, not a log line, when racing the write from outside the process (this is what `drive.py` does). |

### `drive.py`: scripted timeline runs

`MatterEngine3/tools/drive.py` launches the editor against a pre-written
timeline file and verifies the screenshots it promised, so a CI-style caller
doesn't have to reimplement the poll-the-log/append-to-FIFO idiom from
recipe 4 of the QA cookbook by hand:

```bash
python MatterEngine3/tools/drive.py --world CornellBox --timeline shots.txt \
    --out-dir out/ [--timeout 600] [--editor <path>] [--hide-ui] [--env K=V ...]
```

It copies `--timeline` to `<out-dir>/cmd.txt`, then scans it for every
`shot`/`shot_now` line and **unlinks each expected PNG and its `.done`
sidecar if either already exists** — before the editor is ever launched.
Without this, a run that crashes or hangs before actually producing a shot
can false-pass by reading stale files a *previous* run into the same
`--out-dir` left behind; the post-run check only ever confirms the files
exist, not that *this* run wrote them. It also **warns (non-fatal, to
stderr) if the timeline has no `quit` line** — such a timeline only ever
ends via `--timeout` (or being closed some other way), which is sometimes
intentional but is also the most common way a broken timeline silently
burns the whole `--timeout` instead of failing fast.

It then launches `MatterEditor/build/windows/editor.exe` (override with
`--editor`) with `cwd=MatterEditor/`, `MATTER_WORLD=<world>`,
`MATTER_CMD_FIFO=<abs path to cmd.txt>`, `TMP`/`TEMP` pointed at the OS temp
dir, `MATTER_HIDE_UI=1` if `--hide-ui` was passed, and any `--env K=V`
overrides applied last (repeatable; each can override any of the defaults
above). It tees stdout+stderr to `<out-dir>/log.txt` while waiting up to
`--timeout` seconds (default 600); on timeout it kills the process tree and
exits 2. On a normal exit it checks the same `shot`/`shot_now` PNG + `.done`
pairs from the pre-launch unlink actually exist now — any missing file, or a
nonzero editor exit code even with every screenshot present (a crash
mid-script must never read as a pass), is exit 1. Otherwise exit 0 with a
one-line summary. `shot`/`shot_now` paths in a timeline resolve relative to
`MatterEditor/` (the editor's own cwd), not `--out-dir` or wherever
`drive.py` was invoked from — prefer absolute paths into `--out-dir` in
timelines to avoid the ambiguity.

## e) The three launch rules

Repeated in every harness in this repo; violating any one produces confusing,
hard-to-diagnose failures:

1. **Pass `TMP`/`TEMP` as the command's own env prefix**, from a shell that has
   NOT exported the MSYS2 UCRT64 `PATH`. A native Windows exe does not inherit
   MSYS2's TEMP otherwise, `fs::temp_directory_path()` falls back to
   `C:\WINDOWS`, and fixtures fail with confusing "coherent load failed"-style
   errors.
2. **Launch from the `MatterEditor/` working directory.** Asset/shader/project
   lookup, the default `../issues` resolution, and relative script paths are
   all cwd-relative to `MatterEditor/`.
3. **Never build while `editor.exe` is running** — it holds a file lock on its
   own binary; a build started while it's up fails or silently no-ops.
