# MatterViewer Sector Streaming Manual Acceptance

This checklist is intentionally manual, in-app acceptance for the MatterViewer
editor workflow. It covers UI interaction, live Flecs ownership transitions,
and visual/editor-camera independence that source checks and focused unit
executables cannot observe. Screenshot, long-flight, cinematic-timing,
performance, and GPU-runtime automation are intentionally not included in this
handoff.

## Setup

1. Build and start `MatterViewer` with a procedural world that has a valid
   streaming profile. Leave the Sector Streaming panel visible.
2. Keep the editor camera controls available and note the panel's selected
   entity, state, generation, resident, inflight, and recoverable-error fields.

## Acceptance checklist

1. Open the procedural world, create or select an anchor, and attach
   streaming. Expected: state moves from `Pending` to `Active`; resident and
   inflight counts begin to rise as work is admitted.
2. Enable **Follow editor camera**, then move the editor camera. Expected: the
   anchor, status, and residency move/update with the editor camera; no engine
   camera coupling is introduced.
3. Disable **Follow editor camera** while streaming remains attached, then use
   the XYZ translation gizmo. Expected: the detached anchor moves while the
   editor camera remains independent. Verify that hovering does not suppress
   camera input on frames where no gizmo is visible/submitted.
4. Remove `SectorStreaming`. Expected: `Detaching` appears as applicable and
   residency reaches zero. Re-add the component explicitly. Expected: a fresh
   generation starts and recovery proceeds normally.
5. Regenerate using an explicit seed. Expected: the same ECS entity and its
   component survive; exactly one new generation starts.
6. Create or select a duplicate anchor and attach it. Expected: the second
   anchor reports `OwnerAlreadyClaimed` with the first owner's full ID. It must
   not auto-promote; remove and re-add it to retry explicitly.
7. Attempt activation in a closed world. Expected: `UnsupportedWorld` is
   visible, the world remains usable, and the sector count stays zero.
8. During detach, reload, and regenerate, watch resident, inflight, and
   generation counters for stale growth. Record any unexpected counter growth
   or unrecovered state for follow-up.
