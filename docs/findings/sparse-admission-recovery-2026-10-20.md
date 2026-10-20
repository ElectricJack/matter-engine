# Sparse scene admission recovery: implementation and remaining acceptance

Task: eager-nexus-71. Base: 9d846e66961cf275be4fb29182281961018f93e5.
Status: **work in progress; required acceptance has not passed**.

The production uploader uses 160-byte surface triangles and previously grew
flattened arrays before its per-append storage-range rejection. The session
published its expansion cache and catalog ownership before forest preparation
could fail, and the editor treated every render failure as a device fatal.
The CPU characterization selected the actual source admission expression,
without running the separate RT-counter probes from the research appendix.

## Changes preserved

- Internal count-only admission planning checks cumulative storage bytes,
  addressing and arithmetic before flattening GPU geometry. It uses the actual
  upload layouts, physical storage range and AS/dispatch bounds. A scoped test
  limit can lower the physical range; production budgets are unchanged.
- Forest publication preflights primary and shadow catalogs before preparing
  either, stages catalog metadata before GPU preparation, and swaps the pair
  only after both succeed. Scoped test faults cover primary and shadow phases
  for both forest and placement publication.
- Session expansion caches, catalog owners, placements and dirty/option flags
  are candidates until forest publication succeeds. Rejected source identities
  suppress unchanged retries. Temporal identities are not promoted on rejection.
- The editor recognizes only the internal admission tag as recoverable, shows
  an actionable capacity diagnostic and keeps its UI loop active. Capture and
  performance qualification reject that frame; requested screenshot/performance
  automation exits unsuccessfully. Other failures retain the fatal path.
- Added CPU planner/batch-walker tests and a native smoke fixture that checks
  capacity rejection, primary/shadow preparation rollback, placement rollback,
  accepted geometry identities/residency and later valid publication.

## Evidence and capability limits

Executed before implementation:

```
aq test /tmp/eager-nexus-71/test_characterization.py -q -s -k actual_surface_admission
1 passed in 2.04s
```

This is characterization, **not** an implementation regression pass.

Submitted after implementation:

```
aq test tools/tests/test_sparse_scene_admission.py -q
```

Admission remained queued for over two minutes: all six slots were held by
agile-stone-80. Only this worker's unstarted launcher was interrupted. No test
process ran, no parallelism limit was raised, and no producer was interrupted.
The tests added here are **NOT RUN**, including exact 128 MiB boundary,
cumulative triangle and overflow regressions.

Native Linux editor/build/UI/session acceptance is **NOT RUN**. This slot has
no glslc, Xvfb/xvfb-run, /dev/dri, generated embedded SPIR-V header or prebuilt
editor. A Lavapipe ICD file exists, which does not prove the required Vulkan
capabilities. Root CMake rejects non-Windows; the documented native product
entrypoint is `make -C MatterEditor linux` with explicit Vulkan paths. No
runner/toolchain was provisioned and no unsupported substitute qualified this
patch. The supervisor was asked for an existing authorized runner/capability
decision. Windows acceptance was explicitly waived by the task, independently
of the unmet Linux gate.

## Remaining work before a pass

Run the CPU regressions, compile the affected production translation units and
execute the native rollback fixture under validation. Prove session mixed
ordinary/shared rollback, catalog ownership and temporal identities across
primary/shadow failures, first-load UI usability, subsequent smaller-world
rendering, and capture/performance rejection with no success artifact. Neither
source review nor the new unexecuted fixtures satisfies these requirements.

The current session rejection path retains accepted GPU snapshots and metadata
but displays an error viewport even when an accepted scene exists. Restoring
that accepted scene for interactive display remains unfinished. Ordinary
force-LOD replacement is deferred until after forest publication, so a later
ordinary preparation failure still requires a broader publication transaction.
Full hierarchy fallback-capacity admission and direct shared-shadow entrypoint
preflight also need review. Do not integrate this branch as accepted recovery.
