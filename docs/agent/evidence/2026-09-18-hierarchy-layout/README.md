# Reuse prepared shader node layout and renderer bindings

VkGeometryCutNode now begins with the exact12-word shader node layout, with
compile-time byte-offset checks. The existing hierarchy worker fills bounds,
error and child references in that layout. Publication copies the prepared
record instead of repeating scalar float-to-word encoding for unchanged nodes.
Scene-global child relocation still occurs on main, as before.

Snapshot capture resolves ready page slots once on the owner lane. Subsequent
publication uses the hint only when its current part hash matches the immutable
page identity; mismatches fall back to lookup. It checks page readiness and
single-cluster eligibility again before publishing. Readiness/cluster words in
input memory are never authority for a missing page. Workers do not dereference
renderer state. Node storage grows from64 to80 bytes; this trades CPU scratch
size for avoided repeated lookups/encoding. GPU node format remains48 bytes.

GPU tests exercise both fine/coarse cuts with prepared bindings, deliberately
mismatched slot hints, and bogus readiness on missing pages. Assertions cover
triangle counts, CPU/GPU selection, actual raster material/depth and feedback.

This does not complete background whole-scene assembly or draw-command building.
The next boundary needs immutable per-sector blocks and consistent publication
of instance indices, node offsets, capacities and feedback ownership.

Native editor and GPU-test builds passed. Geometry-pages GPU suite ALL PASS,
zero Vulkan validation errors (/tmp/hierarchy-layout-gpu.log). The prepared
binding/mismatched hint/missing-page readiness cases are part of that run.

Next structural opportunity: runtime currently relocates every child index while
flattening every cached node. Keep child indices block-local and give each job a
node base so shader traversal performs relocation. Existing root/feedback indices
must remain globally attributable, and legacy test callers must retain base0
semantics. That permits immutable worker-packed sector blocks to feed incremental
GPU ranges without main-thread rewriting of unchanged node payloads.

Isolated movement run moving-hierarchy-layout-v1 exited0. Median32.389ms,
p95 49.748ms, max72.802ms versus33.994/50.420/77.133ms previously. Cut preparation
mean4.855 -> 2.843ms; max12.014 -> 8.428ms. Snapshot capture cost increased
0.114 -> 0.129ms per rebuild due to resolving bindings at capture; whole-scene
hierarchy pack increased0.00762 -> 0.00801ms per instance, consistent with larger
CPU nodes. Net cut-stage reduction is substantial; single-run/cache variability
still limits general speed claims. No paging failures, watchdogs, evictions or
budget stalls. Full results in results.json. Under10ms motion and under1s cached
visible completion remain unmet/unproven. Background migration remains partial.
