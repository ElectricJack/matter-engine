# Current-view geometry dispatch and upload priorities

Prepared while editor PID 41140 remained open for user review. The running
editor executable was not rebuilt, replaced or restarted.

## Implementation

- Residency accepts replacement visible-lease membership. A page is visible
  when any owner belongs to that set. The queue sorts visible work before
  background work, then retains root-first/numeric priority/hash ordering within
  each band. Thus visible refinement can precede offscreen mandatory roots.
- Membership changes rebuild only the queued index using extracted set nodes,
  reusing its allocations. Unchanged membership does no queue rebuild. Resident
  pages are not scanned; delayed requests pick up current priority on requeue.
- GeometryWorldRuntime computes unjittered frame planes and tests each asset's
  aggregate root bounds under its instance transform. Invalid views conservatively
  mark all admitted geometry visible. Bounds are computed once at attachment.
  All instances contribute, so a shared asset can be visible through any instance.
- Decoded pages waiting for upload are processed in two passes: currently visible
  first, background second, under the existing byte/CPU budgets. This avoids a
  large temporary sorting allocation. Already-issued I/O and GPU work is not
  cancelled; old-view decoded pages are reclassified before upload.

This is priority, not culling or removal: background sectors remain admitted,
retain root protection and can finish when visible work has consumed its budget.
GPU feedback from an old view cannot independently pin a priority band because
membership is recomputed from current instances and camera each update.

## Validation and remaining work

Native geometry_hierarchy_tests: ALL PASS, exit 0. Added coverage proves visible
refinement precedes background roots, camera turns replace priorities, delayed
requests use new membership, shared owners preserve visibility, and detaching a
visible owner removes its influence. Existing residency tests remain passing.
Log: /tmp/geometry-view-priority-tests.log.

Native matter_engine_viewer_objects compilation passed (exit 0), without linking
the editor. Log: /tmp/geometry-view-priority-viewer-objects.log.

Runtime cache-load, movement-frame, camera-turn and reload validation are still
required after the user finishes editor review. No loading/frame-time improvement
is claimed yet. In particular, cost of rebuilding priorities on repeated camera
turns and the extra decoded-queue scans must be measured. The unchanged in-flight
worker queue can still contain old-view work until it completes.

Graft lookup timed out without a savings count; exact source reads were used.

## Priority telemetry prepared during review

Added windowed visible/background dispatch and successful upload counters to
paging_queue, plus reprioritize_cpu timing for replacement of the queue's view
membership. The counters classify at the moment of dispatch/upload; a page may
change bands between those events, so differences are not missing-page counts.
Reprioritize timing excludes camera/frustum construction, which remains inside
admission/update timing. Existing geometry_paging_report.py aggregates these
fields without a schema change; a two-window synthetic check passed.

Native viewer-object compilation with telemetry passed (exit 0):
/tmp/geometry-view-priority-telemetry-build.log. Editor PID 41140 was verified
live/responding during this work; its executable was not relinked. A review-status
question is pending before executable rebuild and isolated GPU/performance runs.

Next validation: rebuild editor after review ends, repeat the cache-only load
command from tools/terrain_cache_audit.py with the prepared sector and encoded VT
stores, compare first matching visible-detail feedback and the new priority
counters, then repeat the saved 240-camera movement path. Also exercise reload
and rapid turns before claiming runtime safety or speed improvement.

## Repeated view/ownership change test

Added 96 successive priority changes with two distinct queued root pages and a
transient shared owner. The test alternates visible sets, raises numeric request
priority after reprioritization, detaches a visible shared owner, dispatches both
pages, and defers them until a future epoch. Assertions verify exactly-once
queue membership, visible-first ordering, fresh ticket issuance, inactive old
tickets, and unchanged delayed-request deadlines. The full native hierarchy
suite passed with exit 0: /tmp/geometry-priority-churn-tests.log. Build log:
/tmp/geometry-priority-churn-build.log.

This is CPU queue correctness evidence, not an editor/GPU performance test.
The user-review editor was verified live/responding and left untouched.
