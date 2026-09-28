# Batch geometry snapshot invalidation

GeometryWorldRuntime previously scanned every loaded asset for each published
page. Publication now collects page hashes in reusable scratch, sorts once,
then visits each valid asset once and probes the smaller side of the intersection.
All owners are checked, including assets referencing not-yet-resident children.
No snapshots are rebuilt between publications, so batching retains the same
invalidation set. The retain-for-frame failure path also drains invalidation
before returning. Scene resources invalidate before any scene reuse decision.

Editor native MSVC build passed (/tmp/publication-batch-editor-build.log).
Repeated movement uses the same 2GiB reservations and 240-camera path as
moving-command-reuse-v1, with no concurrent build or GPU validation.
This exploratory path permits cache misses; it is not cached sub-1s acceptance.

Completed moving-publication-batch-v1 exit0. Published 51,571 pages versus
51,537 previously; no reported page failures, watchdogs, evictions or budget stalls.
Reported coverage remains zero rejected/unready/source-fallback assets.

Publication mean 2.197 -> 1.772ms; max 26.557 -> 16.989ms. This did not improve
whole movement: median 43.108 -> 46.941ms, p95 57.387 -> 60.732ms,
max 150.624 -> 183.023ms. Geometry update mean 10.137 -> 11.247ms.
Hierarchy packing and cut upload were more expensive in this run. These are
single sequential runs with warm-cache/scheduling differences, not a controlled
proof that batching caused the frame-time regression. No general speedup claim.
Full measured values are in comparison.json. Goal remains unmet.

Next target: full-scene hierarchy repacking/cut upload for individual ready pages,
and the renderer command-template rebuild that follows changing page capacities.

Geometry-pages GPU regression suite completed exit0, ALL PASS (log:
/tmp/publication-batch-gpu.log). This suite checks GPU traversal/rendering;
the editor movement run exercises the changed world-runtime publication path.
