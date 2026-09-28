# Block-local hierarchy child links

VkGeometryCutJob carries a node_base (default0 preserves existing callers).
Job word7 retains the source-VT flag in bit0 and carries node_base in bits1..31.
The shader adds that base to non-sentinel child links when reading a node.
Root indices and feedback indices remain global so retired-frame owner/page maps
retain their interpretation. Source-VT tests now mask bit0 explicitly.

World assembly bulk-appends immutable prepared node arrays rather than copying
and modifying every node's child links. RT proxy enumeration remains independent;
both raster and RT-assisted hierarchy jobs receive their block base. CPU selection
still uses its original per-asset local indices. GPU node format stays48 bytes.

Production GPU tests relocate the hierarchy after an unrelated node while keeping
child references local, exercise fine/coarse raster selections, and assert draw
triangle counts and raster coverage/materials. Existing base0 RT/GPU tests remain.
This is a prerequisite for reusable sector blocks and background scene assembly;
it does not itself move whole-scene assembly to a worker.

Native MSVC GPU suite ALL PASS, zero validation errors; editor build passed.
Logs: /tmp/hierarchy-local-links-gpu.log and /tmp/hierarchy-local-links-editor-build.log.
Movement measurement runs after both compilation and GPU validation exit.

Movement run exited0. Median33.704ms/p95 50.678ms/max78.241ms versus prior
32.389/49.748/72.802ms. No demonstrated frame-time improvement from this change.
Hierarchy pack mean0.00832ms per instance versus0.00801ms previously; measured
CPU cut preparation mean3.354ms versus2.843ms. The68.885ms maximum cut-preparation
sample occurred before STATS,movement_start, not during measured camera movement.
No paging failures, watchdogs, evictions or reservation stalls were reported.
Retain the change as the structural prerequisite for immutable block publication,
not as a validated performance win. Whole-scene background assembly, command
preparation and cached frustum-complete loading acceptance remain outstanding.
