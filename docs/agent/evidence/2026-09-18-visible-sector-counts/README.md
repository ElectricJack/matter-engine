# Visible sector target counters

Added desired/pending visible cube counts to the worker-published streaming
status, with validity and camera-view revision. Pending includes unissued,
held, cooling-down and coarse-resident sectors whose desired rung has not
published. This measures sector publication only: GPU geometry refinement and
VT demand must still be combined for end-to-end visible readiness. No acceptance
predicate was relaxed. Camera plane changes increment revision; anchor position
updates preserve it. Editor CACHE_VISIBLE_SECTORS output is recorded by the audit
as visible_sector_samples. Cube tests cover unqueued holes, publication progress
and invalid-view rejection.

Editor build passed (/tmp/visible-controls-editor-build.log). User requested an
interactive review, so isolated scene validation is deferred while they use the
editor. Review launcher: C:/tmp/matter-blas-mountain/controls-frustum-v1/launch.py.
UI enabled; raster rendering with RT/GI/POM off; cache reads enabled with misses
allowed for exploration outside the prepared area. Frozen asset build untouched.


Review launcher correction: MATTER_HIDE_UI is presence-based, so setting it to
0 still hides panels. Removed the variable entirely and restarted the same
binary as PID 22328. Initial hidden-UI log preserved as editor-ui-hidden.log.
Sector selector suite completed ALL PASS (/tmp/visible-count-tests.log).

While review is open, prepared visible-detail telemetry changes in source:
GPU selected status bit 1 records refinement fallback, including traversal/depth
limits; readback logs cut generation and overflow. Both selection/probe shaders
compile with glslc Vulkan 1.2. Python audit parser syntax passed. Renderer binary
has NOT been rebuilt with those later telemetry changes and GPU validation is
pending. They do not yet establish current-camera visible readiness.


UI-enabled startup subsequently hung in AssetBrowser::annotate_project ->
resolve_object_hash -> ScriptHost::eval_requires on the UI thread. Local-symbol
noninvasive stack: /tmp/editor-ui-startup-local-stacks.log. Added opt-in
MATTER_ASSET_BROWSER_SKIP_CACHE_STATUS for the review launcher; unannotated rows
show unknown status rather than incorrectly claiming "not baked". This bypasses
badge dependency evaluation; it does not remove asset browsing or scene controls.
The original badge-scan behavior remains outside this review setting and needs
an asynchronous redesign. The hidden-UI benchmark did not exercise this cost.
