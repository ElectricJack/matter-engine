# Engine Profiler — design

**Date:** 2026-08-07
**Status:** spec, awaiting approval
**Motivation:** "I don't know where to focus without better profiling."

## Problem

The engine already has good measurement *primitives*, but they are three
disconnected islands, and the dominant streaming cost is not measured at all.

1. **CPU render-thread build region** — `vk_build_profile.h`
   (`MATTER_VK_BUILD_PROFILE=1`). 21 RAII-scoped zones, aggregate-windowed to one
   stderr line per 2 s. Deliberately never per-frame/per-item: the codebase
   *measured* that per-sector stderr halved fill throughput and inverted a
   conclusion (`docs/sector-bake-time-findings-2026-07-30.md`). This discipline
   is load-bearing and the new library must preserve it.
2. **GPU passes** — real `VkQueryPool` timestamps
   (`vk_scene_renderer.cpp` `write_gpu_timestamp`), zones for BLAS, TLAS, RT,
   denoise, cull, G-buffer, volumetrics, VT, total. Read back per frame, surfaced
   separately from the CPU zones.
3. **Bake / stream / workers** — `MATTER_STREAM_FILL_PROFILE`,
   `MATTER_STREAM_BAKE_PROFILE`, `MATTER_LOD_BAKE_PROFILE`, `MATTER_GPU_JOB_SLOW_MS`
   on the `async_bake` worker/pump threads.

Three mechanisms, three env vars, three output formats, **no shared clock, no
correlation**. You cannot see one frame as "CPU build = X by zone, GPU = Y by
pass, bake backlog = Z" in a single picture.

**The coverage hole.** `matter_engine.cpp:765` admits the biggest per-sector cost
is unmeasured:

> `MATTER_STREAM_BAKE_PROFILE` times `bake_source` ONLY, which is a minority of
> what an executor actually does per sector — `stage_load` and the `VkScenePart`
> prebuild are the rest, and neither was ever measured.

`stage_load`'s LOD ladder is the "closer clusters get re-baked at finer detail"
work — the 57% chunk the sector-bake breakdown fingered. The thing most suspected
of being expensive is the thing no profiler covers.

## Goals

- **One shared profiling library** with a **single clock** used by CPU, GPU, and
  worker-thread events, so everything correlates on one timeline.
- **Trivial to instrument a section**: one macro, one line, at any call site in
  any subsystem.
- **Compiled out entirely in release builds** — no branch, no timer, no memory —
  via a compile-time switch, with a separate runtime enable when compiled in.
- **A dedicated in-editor Profiler window** to dig in: last-frame time, per-zone
  breakdown, min/max/mean, and smoothness/jitter metrics.
- **Persist a tail of profile data to disk with every report screenshot**, in the
  existing issue-report directory, next to `issue.md` / `state.json` / the log
  tail.
- **Offline-explorable output** (Chrome-trace / Perfetto JSON) from the same tail
  buffer, for deep one-time analysis.

## Non-goals

- Replacing the existing `MATTER_PERF_OUTPUT` headless fps-sampling harness
  (batch benchmark; can feed from the new library later, out of scope here).
- A general external profiler UI (we integrate an in-editor window instead — see
  Alternatives on Tracy).
- Per-instance or per-cluster event granularity (observer-effect landmine — we
  stay at the section/zone level, aggregated per frame).

## Design

### Where it lives

A new foundation library `libs/ProfileLib` (header-first, like the existing
`vk_perf`/`vk_build_profile` style), beneath MatterEngine3 in the dependency
chain and dependency-free (only `<chrono>`, `<cstdint>`, `<atomic>`, `<vector>`).
The editor's UI window and the report hook live in MatterEditor and depend on it;
the engine subsystems include the header and instrument themselves. No engine
types leak into the library — events carry integer zone ids + a string table.

### Compile-out — the core requirement

A single compile-time switch `MATTER_PROFILE_ENABLED`.

**Build topology (verified 2026-08-07).** There is no debug/release/perf flavor
split — the editor is one `-O2 -DNDEBUG` build (`MatterEditor/Makefile:338`,
"Windows is the release build path for now"), and `vulkan-instancing-perf` runs
that same build. So **the perf build IS the normal build**, and the switch must
NOT key off `NDEBUG` — that would compile the profiler out of the exact build we
profile in. Mapping instead:

- Normal `make -C MatterEditor windows` / `linux` (= dev = perf build):
  `MATTER_PROFILE_ENABLED ?= 1` — **on**.
- `make -C MatterEditor dist` (the actual shipping artifact): passes
  `MATTER_PROFILE_ENABLED=0` — **off**, the one place it is compiled out.

The `?=` default keeps it on everywhere unless a build explicitly opts out, so a
future real "release" flavor can force 0 without touching call sites.

```cpp
#if MATTER_PROFILE_ENABLED
  #define PROFILE_SCOPE(name)     ::profile::Scope _p_##__LINE__(name)
  #define PROFILE_SCOPE_ID(zone)  ::profile::Scope _p_##__LINE__(zone)
  #define PROFILE_FRAME()         ::profile::frame_mark()
  #define PROFILE_TAG(name, val)  ::profile::tag(name, val)
#else
  #define PROFILE_SCOPE(name)     ((void)0)
  #define PROFILE_SCOPE_ID(zone)  ((void)0)
  #define PROFILE_FRAME()         ((void)0)
  #define PROFILE_TAG(name, val)  ((void)0)
#endif
```

When `MATTER_PROFILE_ENABLED == 0` every macro expands to `((void)0)` — the
zone-name string literals are never emitted, no `Scope` object is constructed,
nothing links. When it is 1, there is still a **runtime enable** (an atomic bool,
toggled from the UI or an env var) so a dev build can leave instrumentation in but
pay only a predicted branch when the profiler is off — exactly today's
`vk_build_profile` cost model, preserved.

Zones may be registered two ways: an ad-hoc **string name** (hashed + interned on
first sight) for one-off instrumentation, or a **pre-declared enum id** for
hot/permanent zones (the migrated `vk_build_profile` set), which skips the hash.

### Shared clock & threading

One monotonic source: `std::chrono::steady_clock`, wrapped as
`profile::now_ns()`. Every event — CPU scope, GPU zone resolve, worker bake job —
stamps begin/end in the same nanosecond domain, so cross-thread lanes line up.

Each thread owns a **lock-free single-producer ring buffer** of events
(`{zone_id, tid, phase(begin/end), t_ns, frame_index}`). The collector drains
per-thread rings at frame end on the render thread. No locks on the hot path; the
only cross-thread handoff is a published atomic write cursor per ring. This is how
we keep worker-thread bake events (async_bake) and render-thread events on the
same timeline without contention.

**GPU correlation.** GPU zones are asynchronous — `vkGetQueryPoolResults` for
frame N lands a frame or two later. We already tolerate this
(`vk_scene_renderer.cpp:9374`). The library tags each resolved GPU zone with the
`frame_index` it was recorded under, so it slots into the right frame record when
it finally arrives, rather than being smeared onto the current frame. The UI marks
GPU rows as "resolves +N frames late" so nobody misreads the lag as a stall.

### Data model

- **Event**: `{ zone_id, tid, phase, t_ns, frame_index }` — 24 bytes, POD.
- **FrameRecord**: computed by the collector at frame end from the drained events:
  `{ frame_index, cpu_frame_ms, gpu_frame_ms, per_zone_ms[], bake_backlog,
  instances/clusters/parts/commands counts }`.
- **Ring of FrameRecords**: the last N frames (N configurable, default ~512 ≈
  8–17 s), the "tail" persisted with reports and dumped as a trace. This subsumes
  today's `hud_frame_ms` hitch ring.

### Migration (fold in, don't fork)

- Re-express the 21 `vk_build_profile` zones as pre-declared enum zones in the new
  library; `vk_build_profile.h` becomes a thin compatibility shim (or is deleted
  with call sites swapped to `PROFILE_SCOPE_ID`). Same zones, same discipline.
- Route the GPU timestamp zones into the library's GPU-zone channel (they already
  exist; we only change where their resolved values are deposited).
- **Fill the hole**: wrap `stage_load` and the `VkScenePart` prebuild (the known
  unmeasured 57%) in `PROFILE_SCOPE`s on the worker thread, so "rebuilding closer
  clusters" finally appears next to render cost. This is the single most valuable
  instrumentation add and the direct answer to the original question.

### The Profiler window (MatterEditor)

A new dockable ImGui panel `"Profiler"` (sibling of the existing `"Performance"`
panel at `ui.cpp:717`; that one keeps its RT/GPU toggles, this one is the timing
view). Contents:

- **Last frame**: total CPU ms + GPU ms, and a sorted per-zone breakdown
  (ms + %), the same shape `vk_build_profile` prints to stderr today but live and
  interactive, with CPU and GPU in one list.
- **Statistics over the window**: per-zone and whole-frame **min / max / mean**,
  plus **jitter/smoothness**: frame-time standard deviation, worst spike, and
  **% of frames over budget** (16.6 / 33.3 ms, configurable). A single "smoothness
  score" (e.g. 1 − p99/median clamped) for an at-a-glance read.
- **Frame-time graph**: a scrolling plot of the FrameRecord ring with the budget
  line drawn; hover a spike to see that frame's zone breakdown (drill-down).
- **Controls**: pause/resume capture, clear, budget selector, "Dump trace now"
  (writes a Chrome-trace JSON of the current tail), and the runtime enable toggle.

### Disk tail with every report screenshot

Hook `write_issue_report` (`issue_reporter.cpp:616`), alongside the existing
`write_log_tail`: add `write_profile_tail(dir / "profile_tail.json", ring)` which
serialises the FrameRecord ring as **Chrome-trace JSON** (loadable directly in
`chrome://tracing` / Perfetto). So every captured report ships with the last
~8–17 s of correlated CPU+GPU+bake timing leading up to the shot — the smoothness
history and any spike that prompted the report are preserved with it. Guarded so a
release build (profiler compiled out) simply writes nothing / a stub note.

### Output format

Chrome-trace JSON from the FrameRecord+event ring: `X` (complete) events per zone
per frame, `tid` lanes per thread (render + bake workers + a synthetic GPU lane),
`ts`/`dur` in µs. Multi-threaded by construction — exactly what correlating render
vs bake-worker needs — and needs no bespoke viewer.

## Milestones (testable)

- **P0 — library core.** `libs/ProfileLib`: clock, per-thread ring, collector,
  `PROFILE_SCOPE`/`PROFILE_FRAME` macros, compile-out switch, FrameRecord ring.
  Unit test: begin/end pairing, frame attribution, and a **compile-out proof** —
  build a TU with `MATTER_PROFILE_ENABLED=0` and assert (via `nm`/codegen diff)
  that no zone strings or timer calls are emitted. Every guard failable.
- **P1 — migrate + fill the hole.** Swap `vk_build_profile` call sites to the
  library; route GPU zones in; instrument `stage_load` + `VkScenePart` prebuild.
  Acceptance: a StreamMountain fill shows a per-zone breakdown that now includes
  the stage-load ladder, and `[vk.build]`-equivalent numbers match the old ones
  within noise (no regression, no double-counting).
- **P2 — Chrome-trace dump.** "Dump trace now" + `write_profile_tail` in the
  report path. Acceptance: a report dir contains a `profile_tail.json` that loads
  in `chrome://tracing` with render/bake/GPU lanes.
- **P3 — Profiler window.** The ImGui panel: last frame, min/max/mean, jitter,
  frame graph, drill-down. Acceptance: manual — hitch a fill and see the spike +
  its zone breakdown; smoothness score tracks a stutter.

## Risks / caveats

- **Observer effect.** The whole reason the current code aggregates and never logs
  per-item. Mitigation: hot path is a ring append (no I/O, no lock, no string
  formatting); all formatting/serialisation happens on dump/report only. P1
  acceptance explicitly checks fill throughput is unchanged vs the old profiler.
- **Cross-thread clock skew.** Single `steady_clock` source avoids per-thread
  clock drift; only the ring handoff is atomic.
- **GPU async lag** — handled by frame-tagging resolved zones (above), not by
  pretending they're synchronous.
- **Compile-out completeness** — the P0 codegen-diff test is the guard that
  release truly pays nothing; without it, "compiled out" is an unverified claim.

## Alternatives considered

- **Tracy / Optick (vendored).** Mature, compile-time gated, superb external
  viewer, GPU zones out of the box. Rejected as the primary because the user wants
  an **in-editor** window and integration with **this repo's screenshot-report**
  system — both bespoke hooks Tracy doesn't provide, and Tracy is a separate
  external app + server. We borrow its proven *model* (compile switch, scoped
  zones, frame marks) without the external dependency. Could still be added later
  as an optional backend behind the same macros.
- **Extend `vk_build_profile` in place.** Rejected: it is render-thread-only by
  design and has no cross-thread clock, no GPU lane, no ring/UI. The new library
  subsumes it rather than stretching it past its contract.
