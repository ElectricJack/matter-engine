# Background scene hierarchy assembly

Per-asset GPU node arrays now have shared ownership. Once a hierarchy build is
adopted, the array is immutable; replacement swaps ownership instead of editing
an array that a scene worker may read. Scene inputs capture those arrays, local
root indices, controller jobs, matching source/draw instance lists, LOD policy
and ResidentHierarchy resource pins. A bounded one-item pipeline assembles the
flat node/root/job arrays without renderer or residency access.

For matching raster membership, the previous published scene remains renderable
while assembly runs. A completion publishes only when membership, transforms,
instance flags, viewport and LOD policy still match. A newer residency revision
can use the completed older detail while remaining dirty for follow-up, avoiding
starvation during continuous arrivals. Changed membership or memory pressure
cancels the worker generation and takes the synchronous current-scene path.
Reset cancels generations before later admission. Resource pins live with the
published scene and pending input; canceled workers keep their own resources
until they stop. Large assembly vector capacity is returned to reusable scratch.

First coverage, non-raster and changed-membership assembly remain synchronous.
Renderer cut encoding/validation/publication and draw-command construction remain
on the owner lane. Residency capture, CPU cut selection and LRU touches also
remain there. This is an actual assembly worker, but not completion of all
requested thread migration or the performance goal.

MATTER_GEOMETRY_SCENE_ASYNC=0 selects the synchronous comparison path.
scene_worker times worker assembly; paging_scene reports submissions,
publications, discards, previous-scene reuse and whether work is pending.

## Validation and decision

Editor native build passed after retrying a transient executable link lock;
no editor process was live when inspected. Isolated worker movement completed
exit0:1,060 scene assemblies,1,047 published,12 discarded,1,060 previous-scene
reuses (windowed counters omit tail work). Worker assembly totaled1,140.949ms,
mean1.076ms. No paging failures, watchdogs, evictions or budget stalls.

Worker movement median37.794ms/p95 50.791ms/max79.347ms. Same executable with
MATTER_GEOMETRY_SCENE_ASYNC=0 measured32.426/49.373/82.863ms. This is not a
speedup. Last512-frame traces show scene-worker main geometry.update mean28.158ms
versus27.307ms sync, and prepare_frame10.220ms versus7.128ms sync. Frame/cut
publication, command rebuilding and VT routing remain expensive. Single sequential
runs limit causal attribution, but do not justify enabling the experiment by default.

Final source defaults scene assembly to synchronous; set MATTER_GEOMETRY_SCENE_ASYNC=1
for the implemented worker path. The saved v1 worker/reload scripts were run before
that default changed; explicitly add the flag when repeating them with newer builds.
The per-asset hierarchy worker remains enabled by default.

scene-worker-reload-v1 completed two world reloads during camera-turn streaming,
reached after_reload_0 and after_reload_1, then quit exit0. Final sampled geometry
requests/refinement fallback reached zero. AsyncStagePipeline native tests ALL PASS
for bounded admission, cancellation, failures and shutdown pins. This validates
functionality/lifecycle, not sub1s cached visible completion or sub10ms movement.

Remaining: off-thread or incremental renderer cut encoding/validation, draw-command
preparation, residency capture/CPU cut/LRU work, and VT synchronous-work audit.
Goal and the user's all-candidate threading request remain unfinished.
