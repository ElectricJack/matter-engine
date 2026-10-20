# Sparse scene admission recovery: implementation and remaining acceptance

Task: eager-nexus-71. Base: 9d846e66961cf275be4fb29182281961018f93e5.
Status: **failed attempt preserved for follow-up; required acceptance has not passed**.

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
- Ordinary instance/draw snapshots and debug-LOD threshold metadata are staged
  through an internal publication guard before sparse publication. A rejected
  candidate restores the accepted static snapshot and retires only newly added
  ordinary registrations through the existing in-flight recycler. Existing
  ordinary geometry is not released to change LOD thresholds. The final LOD
  metadata commit does not allocate or upload. This code is not compiled or
  runtime verified in this attempt.
- Same-world capacity rejection now records the accepted ordinary/shared scene
  with the live camera when an accepted snapshot exists. Only an initial
  rejection clears the viewport. Both paths return the rejection, so neither
  advances successful capture/performance qualification or temporal acceptance.
- Hierarchy depth, cumulative child fallback counts and initial root fallback
  capacity are checked before flattening. Direct shared-shadow creation and
  placement updates preflight outer placements before GPU preparation. AS input
  buffers retain AS/address bounds; storage descriptor ranges apply where those
  buffers are actually bound as storage. Legacy flattened shadow metadata
  admission remains conservative and unchanged.
- The editor recognizes only the internal admission tag as recoverable, shows
  an actionable capacity diagnostic and keeps its UI loop active. Capture and
  performance qualification reject that frame; requested screenshot/performance
  automation exits unsuccessfully. Other failures retain the fatal path.
- Added CPU planner/batch-walker tests and a native smoke fixture that checks
  capacity rejection, primary/shadow preparation rollback, placement rollback,
  accepted geometry identities/residency and later valid publication. The native
  fixture now also stages distinct ordinary LOD geometry, ordinary transforms
  and a new ordinary registration around rejected publication. These fixtures
  are unexecuted.

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
After supervisor guidance, one bounded retry was submitted:

```
aq test --aq-timeout 75 tools/tests/test_sparse_scene_admission.py -q
```

It exited **75**, reporting no slot free after 75 seconds, with the same six
slots occupied. This was an admission failure; no test process ran. No further
requests were submitted.

The tests added here are **NOT RUN**, including exact 128 MiB boundary,
cumulative triangle and overflow regressions.

Native Linux runtime/UI/session acceptance is **NOT RUN**; the product build
was attempted but did not complete. This slot has
no shader compiler on PATH, Xvfb/xvfb-run, /dev/dri or prebuilt editor. A
Lavapipe ICD file exists, which does not prove the required Vulkan capabilities.
The supervisor identified an existing executable compiler at
`/tmp/sharp-zenith-52-shaderc/root/usr/bin/glslc`. Its `libshaderc.so.1` resolved
by setting `LD_LIBRARY_PATH` to the accompanying existing `usr/lib/x86_64-linux-gnu`
directory. `glslc --version` reported shaderc 2023.8, spirv-tools 2023.6 and
glslang 14.0.0. This tool and library were reused read-only; nothing was
provisioned or copied into source. Root CMake rejects non-Windows; the
native product entrypoint is `make -C MatterEditor linux` with explicit Vulkan
paths. The following supported product build was attempted:

```
timeout --signal=TERM --kill-after=10s 120s env \
  LD_LIBRARY_PATH=/tmp/sharp-zenith-52-shaderc/root/usr/lib/x86_64-linux-gnu \
  make -C MatterEditor linux -j1 \
  GLSLC=/tmp/sharp-zenith-52-shaderc/root/usr/bin/glslc \
  VULKAN_INCLUDE=/home/jkern/dev/matter-engine-cpp/.aq/worktrees/slot-2/third_party/Vulkan-Headers/include \
  VULKAN_LIB_DIR=/usr/lib/x86_64-linux-gnu
```

The first attempt exited **124** at its 120-second bound during real shader generation
(`rt_lighting.rgen.spv`). The changed production translation units were not
compiled and no editor executable or actual device/capability banner was
obtained. This is neither a compiler rejection of the patch nor a build pass.
A follow-up of the same supported product command, after the publication
repairs, also exited **124** at its 120-second bound. It completed
`rt_lighting.rgen.spv` and reached `rt_lighting_culled.rgen.spv`; shader generation
still had not completed, and no changed C++ translation unit was compiled.
The follow-up log is `sparse-admission-2026-10-20/linux-build-followup.log`.
No unsupported substitute qualified this patch. Windows acceptance was
explicitly waived by the task, independently of the unmet Linux gate.

The supervisor reply identified the existing compiler and forwarded runtime
gaps to the global runtime owner. The one registered durable reply wait was
cancelled when that reply arrived and independent build work resumed; no second
wait or runner provisioning was used. Exact characterization, bounded admission
and native build logs accompany this report in `sparse-admission-2026-10-20/`.

## Remaining work before a pass

Run the CPU regressions, compile the affected production translation units and
execute the native rollback fixture under validation. Prove session mixed
ordinary/shared rollback, catalog ownership and temporal identities across
primary/shadow failures, first-load UI usability, subsequent smaller-world
rendering, and capture/performance rejection with no success artifact. Neither
source review nor the new unexecuted fixtures satisfies these requirements.

The earlier same-world error viewport, post-publication force-LOD replacement,
hierarchy fallback preflight and direct shared-shadow preflight gaps have source
repairs in this checkpoint. They have no successful C++ compile or regression
execution evidence. Python fixture syntax parsing and `git diff --check` passed;
neither is a regression pass. No additional unchanged test-admission retry or
second durable wait was used after the supervisor's refusal.

A remaining source gap is full world replacement: `publish_pipeline` resets
`vk_scene`, geometry pages, accepted catalog/cache and temporal state in its
reset job before admitting the new world. Retained-scene recovery implemented
here covers updates within an existing renderer lifetime; it cannot restore a
scene already discarded by that reset. Preserving an accepted renderer and its
world ownership across full replacement remains necessary before claiming the
complete previous-scene guarantee. First-load UI recovery, smaller-world retry,
and mixed WorldSession catalog/temporal behavior also still lack required native
fixtures and execution evidence. The ordinary guard copies static staging on
changed candidates; its streaming CPU/memory cost has not been measured. Do not
integrate this branch as accepted recovery.
