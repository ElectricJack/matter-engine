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
gaps to the global runtime owner. The subsequent authoritative disposition
(recorded in task comments at 1792522712/1792522715) confirms that no display,
`/dev/dri`, hardware RT runner or prebuilt editor is available or coming. Native
UI, hardware RT and device performance stay **NOT RUN**, with real-device
acceptance deferred to Jack; those unavailable checks do not block worker close.
This does not waive code completeness or runnable software/headless/focused
checks. The failed close here is based on uncompiled/unexecuted implementation
and the remaining world-replacement code gap, not a request for another runner. The one registered durable reply wait was
cancelled when that reply arrived and independent build work resumed; no second
wait or runner provisioning was used. Exact characterization, bounded admission
and native build logs accompany this report in `sparse-admission-2026-10-20/`.

## Remaining work before a pass

Code checkpoint: `3172dd18da1c18e0f6aa0414a6a443accf4f1db6`, descending
from preserved checkpoints `c302cc24102c4c33b1e704ed490fcccefe713fad` and
`77ecadacd15b6ddfb734db952f777993404f3b6c`. No implementation regression or
production C++ compilation passed at any of these revisions.

Run the CPU regressions, compile the affected production translation units and
execute available headless/software admission fixtures under validation. Prove session mixed
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

## Epoch 2 continuation after supervisor message

Message `msg-63f016f9c1194a7e988007157715093b` was read and handled under
claim epoch 2. The guarded `aq git pull` restored this task's own published
`5b9e49e29f3942398747b162458bdd383a67d341` checkpoint. No other worker's
unpublished source was copied. The earlier sections are the historical epoch 1
record; the following evidence supersedes their compilation status.

The reset job now constructs a separate `WorldRenderPublication` containing the
renderer, store, world/LOD/resolver data, ordinary/shared catalog owners and
placements, material records, geometry runtime, dynamic bridge, picking map and
temporal state. The original accepted publication is retained while the new
candidate loads. Repeated rejected replacements preserve that original owner.
Candidate rendering waits for the finalize barrier, including all required part
loads. Rendering or completing a retained frame borrows that same publication
with scope-based restoration; it suppresses candidate catalog/material, ECS and
lighting reconciliation. A rejection still returns the typed admission error,
so the existing editor path rejects capture/performance success. Initial loads
without an accepted owner retain a clear shell. A successful candidate draw
releases the retained owner through normal renderer teardown. The atomic worker
fluid diagnostic pointer remains outside the swapped state. Session shutdown
explicitly releases retained ownership before the ECS and renderer dependencies.

`tools/tests/test_sparse_scene_admission.py` now adds an ownership regression
using the production `WorldScenePublication`, `WorldState`,
`VulkanInstanceCache`, `TemporalState` and count-only admission planner. It checks
accepted catalog lifetime and world/cache identity during retained rendering,
temporal history continuity, exception restoration, repeated failed candidate
supersession, fresh history for a smaller successor, and no retained owner for a
first load. Catalog GPU behavior remains the separate native fixture's concern;
this CPU ownership test does not substitute for a WorldSession/device fixture.

The focused command `aq test --aq-timeout 75
 tools/tests/test_sparse_scene_admission.py -q` exited **75 before collection**.
All six slots remained held by `agile-stone-80` throughout the bounded admission
window. No implementation regression ran or passed. No unchanged admission
retry, direct pytest bypass, slot modification or additional runner request was
made. Python syntax compilation and `git diff --check` passed; those are static
checks only.

The supported Linux Make object rules successfully compiled
`matter_engine.cpp` and `geometry_world_runtime.cpp`; the editor's changed
`main.cpp` also compiled. The final object pass and full product build outcomes
are recorded below with their exact logs. The runtime restrictions remain the
supervisor's authoritative disposition: no display, `/dev/dri`, hardware RT or
device-performance runner is available or authorized. The repository's native
Vulkan smoke entry point creates a GLFW window; this attempt did not invent a
headless substitute. Native UI, primary/shadow fault recovery under validation,
software/device captures, subsequent smaller-world rendering, capture/perf
failure artifacts, hardware RT and device performance remain **NOT RUN**.

There are still source-level limits to the complete previous-world guarantee.
The worker's streamed reload path calls `clear_streaming_once` and drains old
sector evictions before `publish_pipeline` stages renderer retention. The
existing terrain-collision transaction also installs before this reset job.
Those established streaming and physics ownership seams have not been changed
or proven atomic with the later visual admission transaction. The new renderer
retention must not be read as a guarantee that those earlier world mutations
roll back. A successful full-world acceptance requires resolving that ordering
while preserving the frozen ownership contracts and adding executable
WorldSession fixtures. The CPU fixture is unexecuted and has no such claim.

**Epoch 2 remains a failed, preserved attempt; do not integrate it as accepted
recovery.** Its code and evidence are provided for review and continuation.


Epoch 2 source checkpoint: `4ca222ea`. Object verification used the supported
Make rules, `VIEWER_TAG=_linux VIEWER_CXX=g++ VIEWER_CC=gcc
HAVE_STREAMLINE=0`, the vendored Vulkan include path and the existing glslc.
The object commands had explicit 180-second bounds and returned **0**:

- `make -C MatterEngine3 build/obj_viewer_linux/matter_engine.o
  build/obj_viewer_linux/geometry_world_runtime.o ...`
  ([log](sparse-admission-2026-10-20/epoch2-objects.log)).
- `make -C MatterEngine3 -W src/matter_engine.cpp
  build/obj_viewer_linux/matter_engine.o
  build/obj_viewer_linux/vk_sparse_voxel.o
  build/obj_viewer_linux/vk_scene_renderer.o ...`
  ([log](sparse-admission-2026-10-20/epoch2-final-objects.log)).
- `make -C MatterEditor build/linux/obj/main.o VULKAN_INCLUDE=<vendored>
  VULKAN_LIB_DIR=/usr/lib/x86_64-linux-gnu`
  ([log](sparse-admission-2026-10-20/epoch2-editor-object.log)).

The source checkpoint includes one final preprocessor formatting cleanup
(joining adjacent identical Vulkan guards) after the engine object build;
there was no behavioral change after compilation. Existing warnings remain.
These are successful compilation checks, not a link or runtime pass. The
separate CPU test admission failure is preserved in
[its log](sparse-admission-2026-10-20/epoch2-cpu-tests.log).


The full product command returned **124** at its explicit 600-second bound:

```bash
timeout --signal=TERM --kill-after=10s 600s env \
  LD_LIBRARY_PATH=/tmp/sharp-zenith-52-shaderc/root/usr/lib/x86_64-linux-gnu \
  make -C MatterEditor linux -j1 \
  GLSLC=/tmp/sharp-zenith-52-shaderc/root/usr/bin/glslc \
  VULKAN_INCLUDE=/home/jkern/dev/matter-engine-cpp/.aq/worktrees/slot-2/third_party/Vulkan-Headers/include \
  VULKAN_LIB_DIR=/usr/lib/x86_64-linux-gnu
```

[Full build log](sparse-admission-2026-10-20/epoch2-linux-build.log).
It completed shader generation and autoremesher dependencies and progressed
through viewer source compilation to `live_edit.cpp`. The viewer archive and
editor executable did not link before termination. There is no compiler error
in that log; timeout is not a product build pass. No identical product build
was resubmitted. The affected translation units compiled via the separate
supported object rules above, with Streamline disabled and without the optional
autoremesher define. This distinction is preserved rather than claiming a full
product or optional-configuration pass.
