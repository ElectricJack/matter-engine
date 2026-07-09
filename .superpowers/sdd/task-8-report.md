# Task 8 Report: Viewer conversion — pump per frame, interactive bake, gates

**Branch:** feature/phase-b-async-bake
**Date:** 2026-07-08

---

## Summary

The viewer (`MatterViewer/main.cpp`) was converted from blocking-drain bake to the Phase B async protocol. All three code steps are complete. A kernel-side crash in the WSL D3D12 environment blocks the Meadow screenshot gate; the Demo world gate passes cleanly. Details below.

---

## Step 1: open_world_and_start_bake

`open_and_bake()` (lines 165–185 in original) was renamed to `open_world_and_start_bake()` and stripped of its blocking `while (poll_event)` drain loop. The new function calls `engine->open_world()` + `s->request_bake()` and returns immediately. The session is non-null on success regardless of bake state — errors arrive through the per-frame event drain.

The initial stats-fill block (which read `frame_stats()` assuming bake was complete) was replaced with a single `stats.connected = true` line. Stats fill happens every frame via the existing `BeginDrawing()` / `frame_stats()` block.

The world-switch path was also updated from `open_and_bake` to `open_world_and_start_bake` with the same stats-init simplification.

## Step 2: Per-frame pump + event drain

After `session->tick()`:

```cpp
session->pump_gpu_jobs(4.0f);

{
    matter::Event ev;
    while (session->poll_event(ev)) {
        if (ev.type == matter::EventType::BakePartDone)
            printf("bake %d/%d %s\n", ev.done, ev.total, ev.module.c_str());
        else if (ev.type == matter::EventType::BakeFinished) {
            printf("bake finished (%d errors)\n", ev.errors);
            if (fifo_path)
                printf("viewer: bake ready\n");
            fflush(stdout);
        } else if (ev.type == matter::EventType::BakeError)
            printf("bake error [%s]: %s\n", ev.module.c_str(), ev.message.c_str());
    }
}
```

The `pump_gpu_jobs(4.0f)` call runs queued GL bake work up to 4ms per frame. The event drain is non-blocking (poll loop exits when queue is empty). `BakeFinished` prints the readiness signal `"viewer: bake ready\n"` for tooling (see Step 4).

## Step 3: Reload path

The reload block (lines 371–391) was reduced to:

```cpp
if (stats.reload_requested) {
    stats.reload_requested = false;
    if (camera_capture) { camera_capture = false; EnableCursor(); }
    session->reload();
}
```

The blocking drain (`while (poll_event(...))`) and `reload_ok` tracking were removed. Events and errors surface through the per-frame drain. Fail-closed behavior (render() no-ops until reload succeeds) is handled by the kernel via the `connected` atomic flag.

---

## Step 4: Tooling compatibility finding

`MatterEngine3/tools/viewer_shots.sh` polls the viewer log for `"MATTER_CMD_FIFO: listening"` (Phase A readiness signal). In Phase A (sync bake), this line was printed **after** the blocking bake completed — so the script correctly waited for bake completion before taking shots.

In Phase B (async bake), `"MATTER_CMD_FIFO: listening"` is printed immediately after `InitWindow()`, well before the bake even starts. The original script would take shots with an all-zero renderer (bake not yet complete, `connected=false`, render() no-ops).

**Fix implemented:** The viewer now prints `"viewer: bake ready\n"` on the `BakeFinished` event (only when a FIFO is active). `viewer_shots.sh` and `meadow_sweep.sh` were updated to poll for `"viewer: bake ready"` with a 300s cap (up from 180s/120s). This ensures shots are taken only after bake completion.

Note: The Phase A sync binary does NOT emit `"viewer: bake ready"` and will time out in the updated scripts. This is intentional — Phase B supersedes Phase A.

---

## Step 5: Interactive verification (cold bake)

Cold bake with `rm -rf cache`:

1. Viewer started with FIFO enabled, MATTER_WORLD=meadow
2. FIFO readiness fired at ~1s (immediately after InitWindow) — window was responsive
3. Camera moves sent via FIFO at +3s, +6s, +9s while bake was in progress
4. Bake processed 279 install-phase events (bake 1/0...279/0) within the test window
5. The viewer rendered sky-clear color while `connected=false` (parts not yet visible)
6. The viewer accepted FIFO commands throughout and never stalled

**Confirmed:** The window is interactive during the cold bake CPU phase (install/compose on worker thread). Parts would visibly pop in once the GL publish jobs begin (after reset job sets `connected=true`).

**Limitation:** The test window (120s) did not reach the GL-side publish phase for the cold Meadow bake (install alone took >120s). The GL publish phase is where the D3D12 crash occurs (see Step 6).

---

## Step 6: Screenshot gate

### Demo world — PASS

`viewer_shots.sh` with MATTER_WORLD=demo completed cleanly:
- Cold bake: `BakeFinished` at ~69s, "viewer: bake ready" printed
- All 5 poses captured with non-zero stats
- Mechanism validated: shots taken only after bake completion

Demo world stats (as captured):
```
STATS,aerial,16.67,0.00,0.16,1.10,1,0,118471,13,0
STATS,corner,16.67,0.00,0.18,1.05,1,0,9232,31,0
STATS,midfield,16.67,0.00,0.16,0.87,1,0,0,32,0
STATS,far,16.67,0.00,0.16,1.11,1,0,9232,31,0
STATS,empty,16.67,0.00,0.15,0.97,1,0,0,32,0
```

### Meadow world — FAIL (kernel crash)

`viewer_shots.sh` with MATTER_WORLD=meadow crashes the viewer:
- Viewer segfaults at GpuCuller slot 83–84 (xforms SSBO ~198–200MB)
- D3D12: Removing Device error printed immediately before crash
- "viewer: bake ready" never reached — script times out at 300s

The crash reproduces consistently at the same SSBO size threshold and is caused by concurrent GPU memory pressure from:
1. Active frame rendering (rasterizer, GpuCuller cull dispatch, HiZ pass)
2. Incremental xforms SSBO reallocation via `recompute_regions()` called on every `add_part()` in the publish jobs

In Phase A (sync bake), the SSBO grew from 2MB to 735MB without concurrent rendering. Phase B interleaves 276+ `glBufferData` SSBO reallocations with active frame rendering, causing D3D12 device removal at ~200MB combined GPU memory pressure.

**Phase A reference comparison: NOT POSSIBLE** — Meadow Phase A refs were generated from the main worktree viewer (sync bake, no crash), but Phase B Meadow never completes the bake.

Screenshot gate result: **FAIL for Meadow** (kernel fix required — see Concern C1).

---

## Step 7: grep-gate

`bash MatterEngine3/tools/grep_gate.sh` → **grep-gate: clean** (exit=0)

The viewer gained only public API usage:
- `matter::EventType::BakePartDone/BakeFinished/BakeError` from `matter/events.h`
- `session->pump_gpu_jobs()` and `session->poll_event()` from `matter/world_session.h`

No kernel-internal headers were introduced.

---

## Self-review checklist

- [x] No blocking drains remain in main.cpp
- [x] `pump_gpu_jobs(4.0f)` called every frame regardless of bake state
- [x] Event drain is non-blocking (exits poll loop when queue is empty)
- [x] Reload path enqueues without draining — one-liner `session->reload()`
- [x] Old `open_and_bake` blocking drain removed completely
- [x] World-switch path updated to `open_world_and_start_bake`
- [x] Stats fill simplified — no assumption of bake completion at startup
- [x] "viewer: bake ready" signal printed at BakeFinished for tooling compat
- [x] viewer_shots.sh + meadow_sweep.sh updated to poll "viewer: bake ready"
- [x] grep-gate: clean
- [x] Build: `make -C MatterViewer` succeeds with no warnings from main.cpp

---

## Concerns

### C1 (BLOCKER): Meadow screenshot gate fails — GpuCuller SSBO D3D12 crash

**Priority: HIGH — blocks Phase B exit criteria for the primary test world**

**Root cause:** `GpuCuller::recompute_regions()` is called on every `add_part()` invocation, each time doing a full `glDeleteBuffers` + `glGenBuffers` + `glBufferData` on the xforms SSBO. With 276 Meadow parts, this causes 276 SSBO reallocations growing from 2MB to 735MB. In Phase A (sync bake), this happened before any rendering. In Phase B, these reallocations interleave with active frame rendering, exceeding WSL D3D12's combined GPU budget at ~200MB.

**Evidence:** Crash is deterministic at slot 83–84 (~198–200MB SSBO). Phase A sync binary loads all 276 parts to 735MB without crash (no rendering during bake). Phase B crashes during publish-job GL work while the frame loop renders concurrently.

**Recommended kernel fix:** Pre-allocate the xforms SSBO to its final size in a single GPU job before publish jobs begin. In `execute_bake`, after the reconcile job returns the `want` list, post a "pre-size" GPU job that calls `recompute_regions()` with all known parts registered at 4096 region_cap, then fire publish jobs without per-part reallocations.

Alternatively: batch `recompute_regions()` to run once at finalize time, and accept that the SSBO state is temporarily uninitialized for render() calls during publish. The render() → cull() path should handle empty `resolved` gracefully (it already returns false when resolved is empty).

### C2 (INFO): Phase A sync viewers not compatible with updated viewer_shots.sh

The updated `viewer_shots.sh` polls for `"viewer: bake ready"` which only Phase B viewers emit. Running the Phase A main worktree binary with the updated script will time out at 300s. The Phase A binary must be run against the original viewer_shots.sh or the Phase B binary must be used.

### C3 (INFO): Cold bake GL-phase interactivity not fully verified

Interactivity during the GL publish phase (where parts pop in) was not verified for Meadow due to the D3D12 crash. The Demo world demonstrated full async bake completion with correct incremental rendering. Meadow-scale verification depends on C1 fix.

### C4 (INFO): "viewer: bake ready" only printed when FIFO is active

The readiness signal is gated on `fifo_path != nullptr`. Headless `MATTER_SCREENSHOT` mode does not set a FIFO and will not print this signal. This is intentional for the current scripting pattern.
