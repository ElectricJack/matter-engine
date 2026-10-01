# StreamMountain persistent geometry cut — 2026-09-29

Task `clear-ridge.8`, queue row 1.5. The starting commit is `794c029a`.
The implementation commits are `e51271af` and `a621fe4a`. The task keeps
POM disabled and measures the opt-in paged terrain path.

## Cut and scene lifetime

The starting renderer already used immutable asset snapshots and
`IndexedCutNode` arrays. `NodeView`/child-vector copies, ownership insertion
and resident resource-handle copies happen when residency changes, rather
than during the indexed per-view walk. This task retains that separation.

`PersistentIndexedCut` keeps each instance's selected frontier and expanded
groups. Changes apply complete-parent refinement/coarsening deltas; a missing
sibling, traversal limit, selection limit or depth limit retains the parent.
Its dense selection slots provide constant-time RT membership and replace
the hierarchy-sized `traced.assign` on every instance assembly. Moving the
view walks the active tree; an unchanged view does not walk it. Hierarchy
replacement resets the slots because indices belong to a specific snapshot.

The runtime keys a cut by world instance identity, residency lease, adopted
snapshot generation, object transform, camera position and effective LOD
error reach. Separate instances of one asset retain separate cuts. Legacy
zero identities use input ordinal, as the renderer does. Departure prunes
the instance state; world reset clears it. The cut owns indices, not resource
handles. Immutable asset snapshots and renderer frame pins retain resources
through GPU retirement.

Unchanged RT scenes now reuse their assembled node/root/job and proxy data,
extending the earlier raster-only reuse. Camera position, transforms,
membership, viewport height/FOV, LOD bias/detail, maximum draw distance and
relevant residency publication invalidate that reuse. Partial roots of an
unrelated, still-incomplete asset preserve the scene. Its first complete root
coverage activates the paged replacement; displayed and pending hierarchy
dependencies still invalidate on publication. Raster-only camera movement
still uses the live GPU selection. Memory pressure disables scene reuse so
selected pages and their ancestors still receive eviction protection.
Asynchronous scene assembly remains raster-only.

The implementation does not move changed-scene assembly off the owner lane:
publication still rebuilds the scene description, and moving-camera updates
still evaluate the active tree. It does not change GPU LOD selection or remove
triangles. The new counters are `paging_cut reused/updated/refined/coarsened`
and ProfileLib's `geometry.cut_*`, `geometry.*_groups`, `geometry.scene_reused`.

## Capture protocol

Canonical MSVC RelWithDebInfo editor, RTX 4090 / 610.74, 1920×1080,
StreamMountain default camera, visible immediate presentation, no frame cap,
RT/GI on, POM verified off. Geometry reservation 3,072 MiB; VT pool 2,048 MiB.
Both versions use the same existing cache, forbid missing terrain-page cooks,
and sample for 20 seconds after the original 30-static-stable-frame gate and
the requested warmup. Background loading may continue after that gate.

```sh
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
PAGED_TERRAIN=1 PAGED_CACHE_ONLY=1 VARIANTS=pom_off WARMUP=45 \
  tools/streammountain_attribution.sh C:/tmp/clear-ridge-8-after-selective-45
PAGED_TERRAIN=1 PAGED_CACHE_ONLY=1 VARIANTS=pom_off WARMUP=300 \
  tools/streammountain_attribution.sh C:/tmp/clear-ridge-8-after-selective-300
```

Raw perf JSON, traces, logs, commands, executable hashes and GPU memory logs
remain under `C:/tmp/clear-ridge-8-before-paged-{45,300}` and
`C:/tmp/clear-ridge-8-after-selective-{45,300}`. The saved
unchanged reference is `editor-cr8-before.exe` in the same build directory;
the late before command uses `EDITOR_NAME=editor-cr8-before.exe`.

## Early measurement

Hitch counts below use frame intervals, as the task-1 protocol does. They
include CPU stalls and are separate from GPU timestamp totals.

| 45-second warmup | Before | Persistent cut |
|---|---:|---:|
| GPU median / p99 / max, ms | 25.81 / 33.44 / 34.36 | 77.71 / 103.65 / 106.31 |
| Interval median / p99 / max, ms | 30.73 / 52.64 / 87.97 | 92.28 / 192.07 / 223.40 |
| Intervals >100 ms / >1 s | 0/637 / 0/637 | 90/222 / 0/222 |
| Static uploads in sample | 122 | 214 |
| Geometry update mean / p95, ms | 7.467 / 16.201 | 19.167 / 53.100 |
| Scene assembly mean / p95, ms | 3.523 / 7.300 | 5.281 / 25.237 |
| RT-scanned instances mean / p95 | 1,401 / 1,431 | 4,861 / 8,950 |
| Last logged visible ready / desired roots | 1,014 / 1,443 | 1,443 / 1,612 |
| Last logged geometry reservation, MiB | 179.39 | 390.74 |
| Peak whole-GPU memory, MiB | 12,940 | 9,577 |

These launches do not isolate a speedup. The after sample admits more visible
root coverage and substantially more fine-page work: 15,376 final bindings
versus 2,237 before, more static uploads, and many more RT-scanned instances.
Its elapsed launch-to-exit time is 197.56 seconds
versus 215.70 seconds before; this is also a loading-population comparison.
CPU statistics use the trace ring's available sampling tail: 512 of 637
before frames and all 222 after frames.

The after trace directly records **68,905 reused cuts / 11,119 updates**,
**26,064 refined groups**, and **157/222 whole-scene reuse frames (70.7%)**.
Within that window, geometry update averages 11.27 ms on reused-scene frames
and 38.24 ms on rebuilt frames; upload/publication work also differs between
those cohorts, so this is not an isolated per-frame timing delta. Every trace
has zero rejected zone/counter registrations.

The selective-publication fix is necessary for that reuse. An earlier
checkpoint, retained under `after-paged-{45,300}`, invalidated the scene even
when publication only advanced another asset's incomplete roots. Its late
sample reused all 66,912 evaluated cuts yet rebuilt the scene on all 51
frames. The final code preserves dependency and first-coverage transitions
without that global invalidation.

## Late measurement and remaining costs

| 300-second warmup | Before | Persistent cut / selective publication |
|---|---:|---:|
| GPU median / p99 / max, ms | 89.31 / 93.20 / 93.20 | 266.96 / 294.00 / 294.00 |
| Interval median / p99 / max, ms | 225.86 / 385.45 / 385.45 | 306.04 / 594.12 / 594.12 |
| Intervals >100 ms / >1 s | 87/87 / 0/87 | 63/63 / 0/63 |
| Static uploads in sample | 55 | 63 |
| Geometry update mean / p95, ms | 81.289 / 108.569 | 29.514 / 40.131 |
| Scene assembly mean / p95, ms | 38.735 / 57.171 | 3.255 / 0.000 |
| Scene completion mean / p95, ms | 0.009 / 0.017 | 9.768 / 12.995 |
| LOD-scanned instances mean / p95 | 736 / 762 | 275 / 278 |
| RT-scanned instances mean / p95 | 25,025 / 25,046 | 27,083 / 27,130 |
| Last logged visible ready / desired roots | 16,956 / 18,130 | 5,726 / 6,534 |
| Last logged geometry reservation, MiB | 2,696.26 | 1,183.87 |
| Last logged scene bindings | 52,141 | 62,209 |
| Peak whole-GPU memory, MiB | 12,042 | 10,094 |
| Launch-to-exit, seconds | 1,202.66 | 464.81 |

The after window reuses **60/63 scenes (95.2%)**. Its three rebuilt frames
reuse all **3,936** evaluated cuts, with no refinement/coarsening delta.
Geometry update averages 24.30 ms on reused frames versus 133.87 ms on the
three rebuilt frames. Scene-assembly p95 is zero because it is absent on
95.2% of frames; the remaining rebuilds are still expensive.

Against the checkpoint before selective publication, elapsed loading states
are closer (476.33 versus 464.81 seconds), with 55,135 versus 62,209 bindings.
Geometry update falls from 145.24 to 29.51 ms mean, scene assembly from 65.43
to 3.26 ms, and interval median from 389.14 to 306.04 ms. GPU median stays
similar (272.28 versus 266.96 ms). Populations still differ, so the direct
evidence is the eliminated scene rebuilds and preserved cut state rather
than an isolated speedup factor.

The reference's later stability gate loaded far more terrain roots and used
88% of the geometry reservation. The after capture samples an earlier loading
state with less than 40% reserved and more scene bindings. Runtime admission
pauses offscreen work above 75% and optional fine pages above 90% of that cap.
These are different detail/residency states, despite identical camera,
warmup duration and budgets. The raw GPU increase must be reported; this task
does **not** establish an overall GPU/frame-time improvement.

Remaining owner-lane costs include changed-scene assembly/publication,
instance-array publication (93.84 ms mean in the final late window), snapshot
retention/output copies, RT rung scanning (22.91 ms), and the renderer's GPU
work. Per-frame assembly is removed when nothing relevant changes, but a
complete changed scene still copies the full description. Incremental GPU
metadata/instance publication and a fixed-population geometry/detail census
remain the next gates; the high-density end-to-end frame-time goal is open.

## Historical task-1 comparison

Task 1 used ordinary source geometry, not this paged terrain profile. Its
three early GPU medians were 227.5–248.3 ms and p99/max 394.1–594.3 ms;
pooled intervals >100 ms were 161/172 and >1 s 6/172. The new early sample
is 77.71/103.65/106.31 ms, with 90/222 and 0/222 intervals respectively.
Intervening static-buffer, VT and vertex-cache changes, plus different
geometry/residency populations, prevent attributing this historical gain to
the retained cut. The late historical baseline is 402.1–454.2 ms GPU median,
433.5–811.4 ms p99/max, with 138/141 intervals >100 ms and 1/141 >1 s.
The final late after sample is **266.96/294.00/294.00 ms**, with **63/63**
intervals >100 ms and **0/63** >1 s. Neither historical comparison isolates
this task from the intervening changes or different geometry population.

Both completed early samples and the late reference have POM off, effective
RT on and zero Vulkan validation errors; both final after samples do too.
The measured executable hashes start `a8f649cd9c9f` before and `fd16b35f97be`
after. The late reference needed
1,202.66 seconds from launch to exit because its stability gate arrived
late; its 300-second warmup therefore sampled a much later loaded world than
the early captures.

## Verification

`geometry_cut_tests` checks persistent state against the independent fresh
indexed traversal through 2,000 randomized view/readiness/budget changes.
It checks complete, nonoverlapping source-leaf coverage, selection and visit
bounds, missing siblings, separate near/far instances, multiple root islands,
same-sized hierarchy replacement, unchanged frontier storage, depth-limited
fallback and invalid input recovery.

The native `geometry-pages` fixture now feeds persistent membership into
actual RT proxies through refine/coarsen/refine and compares GPU-authored
raster groups/triangles, TLAS membership and sampled raster coverage against
the independent residency reference.

Exact native checks run serially:

```sh
./tools/build-windows-from-wsl.sh RelWithDebInfo geometry_cut_tests
./tools/build-windows-from-wsl.sh RelWithDebInfo geometry_hierarchy_tests
./tools/build-windows-from-wsl.sh RelWithDebInfo partstore_tests
./tools/build-windows-from-wsl.sh RelWithDebInfo vulkan_smoke_tests
```

CTest runs with `-j1 --output-on-failure` and the selected test expressions.
Logs are `C:/tmp/clear-ridge-8-*-tests.log`; build logs use the same prefix.

All four native suites pass: cut/hierarchy 2/2 in 10.69 seconds, PartStore
1/1 in 5.01 seconds, and `smoke_geometry_pages` 1/1 in 10.11 seconds. The
GPU test prints `validation errors: 0`. The CPU expressions were
`^(geometry_cut_tests|geometry_hierarchy_tests)$` and `^partstore_tests$`;
the GPU expression was `^smoke_geometry_pages$`, with `--no-tests=error`
and a 600-second timeout. GPU testing ran between the before and after
captures. Native build work overlapped the reference's loading/warmup;
all builds and tests finished before its sampled window.

After selective publication changed, the rebuilt hierarchy and GPU suites
passed again, serially, in 11.56 seconds (4.02 CPU, 7.53 GPU), with zero
validation errors. The exhaustive page-dependency checks additionally cover
unrelated partial roots, displayed dependencies, pending replacement
dependencies and first complete root activation.
