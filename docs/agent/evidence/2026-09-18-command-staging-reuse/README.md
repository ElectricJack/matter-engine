# Remove redundant command-template staging writes

upload_scene_buffers previously called upload_gpu twice with the same CPU command
template and same frame.command_upload buffer, once per main/visibility target.
Second call repeated ensure_buffer and upload even though no bytes changed.
Now it reuses the first CPU fill while retaining both GPU copies and barriers.
Missing/undersized reused staging fails explicitly. Standalone no-command-buffer
uploads retain their synchronous behavior. Frame-slot fences retain staging for
both GPU reads. No command layout or draw selection changes.

Also gated VT route census atomics on MATTER_VT_CHART_LOG, matching the census
report's existing enable condition. Default mapping semantics unchanged.
Exploratory moving-census-gate-v1 showed no clear speedup; build activity overlapped
its tail, so do not treat that run as a controlled performance comparison.
The shared command staging reuse is validated separately below.


Geometry-pages native GPU suite ALL PASS, zero Vulkan validation errors
(/tmp/command-staging-reuse-gpu.log). Editor build passed. Isolated repeated
movement run moving-command-reuse-v1 completed exit0, with no overlapping build
or other GPU validation. Same 2GiB reservation profile and camera path.

512-frame render profiler tail: pf.commands total523.3ms/max2.79ms versus
883.0ms/max3.5ms in moving-profile-2gb-v1; pf.vtslots2087.7ms and
pf.flushtmpl1974.9ms remain substantial. Warm cache differences and single runs
limit general speed claims; removal of the duplicate fill is direct code evidence.

Movement interval summary (last retired feedback serial before movement_start
as boundary; may include a few boundary frames):

```json
{
  "n": 959,
  "median_ms": 43.10829999999987,
  "p95_ms": 57.38730000000214,
  "max_ms": 150.62389999999868
}
```

Goal remains incomplete. Paging report for baseline moving-profile-2gb-v1
shows update_cpu32.0s over3048 samples (includes warmup), publish_cpu7.0s,
hierarchy_pack7.8s and cut_upload5.6s. These overlap other scopes and must not be
added to frame totals. invalidate_page currently scans all assets for each newly
published page; invalidation forces subsequent immutable snapshot reconstruction.
That publication/repacking path is the next larger optimization target.
