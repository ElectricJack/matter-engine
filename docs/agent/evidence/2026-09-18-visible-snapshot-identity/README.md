# Matching visible-detail feedback to camera snapshots

Coordinator::submit_view now returns a monotonic revision. Unchanged views keep
it; rotation advances it; clearing/recreating an anchor cannot reuse a previous
identity. Invalid or foreign-owner submissions return zero. Renderer records
this identity with the geometry feedback submission and retains the successful
frame serial. Diagnostic output includes retired/current view revisions and
retired/current geometry-cut generations. Audit snapshot_matches requires
nonzero frame/view and equality of both revisions. Frozen-cull inspection
supplies view=0 and cannot count as live-camera acceptance.

Native coordinator tests ALL PASS (/tmp/view-revision-tests.log), including
unchanged view, anchor resampling, rotation, foreign-owner rejection and anchor
recreation. Python parser syntax passed. Snapshot matching is necessary but not
sufficient for readiness: sector counts, VT targets, source fallback coverage
and stable successful presentation must also match before final acceptance.

User finished review and explicitly authorized closure. PID 28296 exited before
GPU validation/build work. GPU test and scene results follow below.


GPU geometry-pages suite ALL PASS, zero Vulkan validation errors
(/tmp/visible-detail-gpu-tests.log), including new traversal-budget fixture and
24 CPU/GPU comparisons. Editor build passed (/tmp/visible-snapshot-editor-build.log).

Turn audit C:/tmp/matter-blas-mountain/visible-turn-v1 completed both phases,
zero geometry coverage gaps, but strict overall validity FAILED: new orientation
had 241 encoded VT misses (zero cache errors). Initial readiness 10.365s;
turn at 25.418s, first matching view=2 complete-detail feedback at 28.755s
(~3.337s later), full global readiness at 29.462s. First post-turn detail records
correctly had snapshot_matches=false (retired view=1, live view=2). Sector view=2
snapshot at 26.451s had 267 desired and zero pending. Do not call this a fully
cached turn or claim the one-second target achieved.

User reported good settled frame rates but severe spikes while turning/moving.
Performance acceptance must now explicitly include repeated camera motion and
streaming, with worst-frame attribution rather than settled medians alone.
