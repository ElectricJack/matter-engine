# Sparse scene admission recovery: implementation and remaining acceptance

Task: eager-nexus-71. Base: 9d846e66961cf275be4fb29182281961018f93e5.
Latest source checkpoint: `419513acca4b16d5d98e6ce9d5633a7bd41deab5` (epoch 4).
Product code remains `c8c4b834`; `419513ac` adds only the focused smoke mode.
**Five focused native checks, Linux editor compile/link, quick Vulkan smoke
and real Vulkan admission rollback/retry pass. The final epoch 4 follow-up
records graphics execution and remaining UI/performance limits.**
The epoch 4 section at the end records current behavior and evidence; preceding
sections preserve the earlier failed checkpoints and their then-current gaps.

The production uploader uses 160-byte surface triangles and previously grew
flattened arrays before its per-append storage-range rejection. The session
published its expansion cache and catalog ownership before forest preparation
could fail, and the editor treated every render failure as a device fatal.
The CPU characterization selected the actual source admission expression,
without running the separate RT-counter probes from the research appendix.

## Epoch 1 changes preserved

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

## Epoch 1 evidence and capability limits

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

## Epoch 1 remaining work before a pass

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

## Epoch 3: eviction and collision ordering

The continuation retains the accepted render publication on the app thread
before the existing full-reload streaming eviction barrier. The coordinator
still owns and drains its original ledger, but those evictions operate on the
candidate store/state/renderer. The retained draw catalog and expansion cache
remain available for replay. No coordinator or public ownership contract was
changed.

For rendered sessions, terrain collision Clear/Replace is deferred through
`WorldScenePublication` until visual admission succeeds. The existing Box3D
replacement transaction remains the sole physics commit operation. Candidate
geometry has a deferred lifetime owner, including cancellation and supersession;
headless sessions keep their existing immediate publication path. Initial
streaming fill cannot release the old publication: candidate frames service
residency and composite the retained world last until the coordinator reports
the first complete fill. Both renderer owners receive frame completion, while
hidden candidate frames do not become presented temporal history. Cancellation
does not turn an accepted world into an empty shell or a device error.

The CPU ownership fixture now exercises eviction against the candidate,
retained world/catalog/cache/temporal identities, deferred collision effects,
cancellation lifetime release, failed collision commit, and a smaller successor.
The native terrain-collision fixture uses a real headless WorldSession and its
Box3D context to check retained body/shape handles through the same publication
gate, including injected mesh-install failure. It does **not** execute the
Vulkan WorldSession render loop: the gate is driven explicitly in that fixture.

The runner still has no authorized display/device route. Vulkan ordinary/LOD,
primary/shadow and placement rollback fixtures, editor first-load/reload/capture
behavior, hardware RT and device performance are **NOT RUN**. No new runner or
display was provisioned. The source-only duplicate render/completion path during
streaming fill particularly needs device validation; CPU helper success cannot
stand in for that evidence.

The first admitted run exposed fixture build defects: the batch walker needed
`matrix_math.cpp`, and the real instance-cache/temporal fixture needed the render
and MatterSurfaceLib include paths. Those are repaired. The fixture leaves the
existing transitive `vt_finite_sources.h` indentation warning nonfatal, matching
the product build, while retaining other `-Werror` checks. The production
planner and batch-walker checks passed in the third run (the ownership check
still failed on that warning); the corrected isolated ownership check then
passed in 11.04 seconds. These were all admitted through `aq test`.

The first full Linux product build reached vendored Flecs and failed because
its viewer C17 rule omitted the existing `FLECS_PLATFORM_FLAGS`. The one-line
Makefile fix uses the same Linux POSIX feature definition as the engine's main
Flecs rule and leaves non-Linux builds unchanged. The native session fixture
compiled but initially failed linking because the local raylib and Ozz archives
were absent. These build failures are distinct from runtime test failures.

Final epoch 3 verification:

- Production planner and sparse batch walker: **PASS**, two checks in the
  [CPU run](sparse-admission-2026-10-20/epoch3-cpu-tests.log). That run's third
  check failed on the existing indentation warning before the fixture flag fix.
- World/catalog/cache/temporal ownership, deferred effects, cancellation and
  smaller successor: **PASS**, corrected isolated
  [ownership run](sparse-admission-2026-10-20/epoch3-ownership-tests.log).
- Native terrain collision suite, including the added session-owned Box3D
  handle fixture: **ALL PASS**, admitted pytest wrapper **1 passed in 4.16s**
  ([log](sparse-admission-2026-10-20/epoch3-native-tests.log)). The headless
  session's existing immediate publication path is tested; this is not proof
  of the new rendered session call sequence.
- Full Linux editor: **compiled and linked successfully**, exit **0**, with
  current engine sources, optional autoremesher enabled and Streamline disabled
  ([final link log](sparse-admission-2026-10-20/epoch3-linux-link-final.log),
  [compile log](sparse-admission-2026-10-20/epoch3-linux-build-second.log)).
  The first link needed the already-installed `libvulkan.so.1` exposed as a
  temporary `libvulkan.so` link; the next exposed the checkout's MinGW Box3D
  archive. The successful command selects the native archive built by the
  existing session-test Make rules. No system package/display/device was added.
- `git diff --check`: **PASS**.

Dependencies were built from the checked-out vendored sources with CMake,
`Release`, one build job: Ozz's `ozz_animation_offline` target with tools,
samples, howtos, tests and postfix disabled; raylib's `raylib` target with
examples disabled. The raylib CMake archive at `build/linux/raylib/libraylib.a`
is linked locally at the legacy Make path `build/linux/libraylib.a`.
[Ozz log](sparse-admission-2026-10-20/epoch3-ozz-build.log),
[raylib log](sparse-admission-2026-10-20/epoch3-raylib-build.log).

The successful product command was:

```bash
timeout --signal=TERM --kill-after=10s 180s env \
  LD_LIBRARY_PATH=/tmp/sharp-zenith-52-shaderc/root/usr/lib/x86_64-linux-gnu \
  make -C MatterEditor linux -j1 \
  GLSLC=/tmp/sharp-zenith-52-shaderc/root/usr/bin/glslc \
  VULKAN_INCLUDE=/home/jkern/dev/matter-engine-cpp/.aq/worktrees/slot-2/third_party/Vulkan-Headers/include \
  VULKAN_LIB_DIR=/tmp/eager-nexus-71-vulkan-link \
  BOX3D_LIB=/home/jkern/dev/matter-engine-cpp/.aq/worktrees/slot-2/MatterEngine3/tests/build/box3d-linux/src/libbox3d.a
```

**Epoch 3 remains a failed checkpoint and must not be integrated as completed
session recovery.** Final source review found a remaining ordering defect:
`publish_pipeline` still enqueues `WorldStateCommandKind::Ready` and emits
`BakeFinished` after finalize, before the deferred collision callback runs in
`render`. `Runtime::drain_world_state_commands` therefore can advance content
generation and bootstrap candidate ECS entities while accepted collision is
still installed. A rejected visual candidate can already have been announced
Ready/finished. The new native fixture explicitly drives the ownership gate and
cannot detect that actual call-sequence defect.

Completion must coordinate Ready/terminal-event publication with visual and
collision admission, including generation stamping, cancellation, initial
streaming fill and authored ECS ownership, without breaking the frozen
contracts. Simply moving the notifications into `render` has not been proven
safe: the bake-run terminal scope is worker-owned and initial streaming can
depend on entity/bootstrap progression. That remaining source integration and
an executable fixture of the actual WorldSession sequence are the blockers;
the unavailable UI/hardware RT/performance runs are separately **NOT RUN**.

## Epoch 4: worker-owned admission completion

This continuation replaces the epoch 3 early `Ready`/`BakeFinished` sequence.
The worker keeps its existing bake-run scope and cancellation token while the
app thread admits the complete candidate. A small internal completion receipt
returns acceptance or rejection to that worker; only the worker emits the
terminal event. Streaming first fill continues on the same worker while the
receipt is pending. The editor creates its ordinary streaming anchor during
loading, removing the former `BakeFinished -> anchor -> fill -> admission`
cycle. A rejected run stops its idle refinement/streaming retries until a new
command arrives.

The app-side acceptance hook applies the deferred Box3D replacement and the
validated authored `Ready` command before acknowledging success. The internal
Runtime flush runs between ticks, on the app thread. Empty authored replacements
explicitly clear the former authored entities. Animation reset is deferred until
acceptance; pending frames and ticks do not reconcile the old ECS through the
candidate store. Authored dynamic raster bindings reconcile on the frame after
acceptance. The editor clears `bake_ready` on every `BakeStarted`, and the
rejection remains latched across retained frames so automation cannot use the
old scene to qualify a rejected request.

`test_world_session_admission_sequence` now exercises the **actual native
WorldSession worker/app pipeline**, streaming coordinator, source snapshots,
authored ECS bootstrap and Box3D installation. The only visual decision is
injected through an internal scoped fixture (`scene_admission_test.h`); this is
not a Vulkan, UI, presentation, screenshot or hardware validation result. The
existing collision fixture supplies a tiny real Box3D triangle. The new fixture
covers first fill with success withheld, first-load rejection, a smaller accepted
successor, replacement rejection retaining exact entity and native collision
handles, and supersession of a finalized pending bake. It checks each started
generation has exactly one nonzero terminal generation.

The new native sequence exposed a real initial-fill deadlock: an active profile
with an empty desired set never satisfied `resident_sectors > 0`. Completion
now uses a private snapshot of all pending desired sectors, including held and
cooling-down requests, and requires an active coordinator with no pending work,
no inflight work and an idle bake pool. The fixture covers both an empty target
set and an actually published streamed sector while success is withheld. A
separate actual-SectorStreamer regression proves that a failed request in
cooldown cannot be mistaken for a completed fill.

Verification so far:

- `aq test --aq-timeout 75 tools/tests/test_sparse_scene_admission.py -q -rP`:
  **4 passed in 49.70 s** (before the fifth test was added). Native suite printed
  `ALL PASS`; its first-fill log records zero-sector and one-sector completions.
- `aq test --aq-timeout 75 tools/tests/test_sparse_scene_admission.py -q -k first_fill -rP`:
  **1 passed in 2.87 s** for the newly added cooldown/empty-target regression.
- These are five passing checks total. Logs are preserved in this report's
  evidence directory under `epoch4-*.log`.

The first compile attempt caught a namespace typo, a stale intermediate build
was stopped after the test fixture was corrected, and the first native run was
stopped after reproducing the empty-fill deadlock. These are failed/interrupted
development attempts, not passing evidence. The final passing native run uses
the corrected source. The Linux editor build result and remaining capability
limits are recorded below. Earlier epoch sections are checkpoint history, not
evidence for the final source revision.

Linux product verification uses the supported Make path (not the Linux-rejecting
root CMake configuration):

```sh
timeout --signal=TERM --kill-after=10s 900s env \
  LD_LIBRARY_PATH=/tmp/sharp-zenith-52-shaderc/root/usr/lib/x86_64-linux-gnu \
  make -C MatterEditor linux -j1 \
  GLSLC=/tmp/sharp-zenith-52-shaderc/root/usr/bin/glslc \
  VULKAN_INCLUDE=/home/jkern/dev/matter-engine-cpp/.aq/worktrees/slot-3/third_party/Vulkan-Headers/include \
  VULKAN_LIB_DIR=/tmp/eager-nexus-71-vulkan-link \
  BOX3D_LIB=/tmp/eager-nexus-71-epoch4/deps/libbox3d.a
```

This fresh slot reused only native vendored dependency archives (raylib, Ozz,
Box3D) from the prior worker slot and the already available shader compiler;
it did not reuse an editor or engine binary as verification of changed source.
The first product attempt stopped because the fresh slot's Box3D archive path
was absent. The corrected command above compiled for its 900-second bound,
then was resumed incrementally with the same command. There was no source
compile error in that bounded attempt. Final compile/link status follows.

The GPU smoke translation unit, including `sparse_admission_tests.h`, also
passed this **compile-only** check (exit 0, no compiler diagnostics):

```sh
timeout --signal=TERM --kill-after=10s 90s g++ -std=c++17 -fsyntax-only \
  -DMATTER_VK_TEST_FAULT_INJECTION -DMATTER_VULKAN_VIEWER -DMATTER_VULKAN_ONLY \
  -I MatterEngine3/include -I MatterEngine3/src -I MatterEngine3 \
  -I MatterEngine3/src/render -I MatterEngine3/src/provider \
  -I libs/MatterSurfaceLib/include -I libs/SpatialQueryLib/include \
  -I libs/MemoryLib/include -I libs/MathLib/include -I libs/ProfileLib/include \
  -I libs/AssetStoreLib/include -I third_party/Vulkan-Headers/include \
  -I third_party/raylib/src -I third_party/raylib/src/external/glfw/include \
  -I third_party/flecs -I third_party/box3d/include \
  -I third_party/ozz-animation/include \
  MatterEngine3/tests/vulkan_smoke_tests.cpp
```

At the first epoch 4 close attempt, GPU primary/shadow preparation rollback,
actual mixed-scene rendered identity, interactive first-load UI recovery,
screenshot/performance rejection artifacts, validation layers and rendered
performance were **NOT RUN** under the prior no-display disposition.
The close was rejected because new supervisor feedback had arrived: the
existing X11 display `:0` was available and authorized for supported AQ checks.
The following graphics follow-up supersedes that environment limitation. The native session fixture deliberately
injects only the visual decision and does not substitute for those results.
Windows verification remains waived by the task's owner. No RT-counter probe,
runner provisioning or daemon lifecycle operation was performed.

Final Linux result: **PASS, exit 0**, producing
`MatterEditor/build/linux/editor` (ELF x86-64, build ID
`52ef9a07bbdfcb29bcf15afcbadcdf69ce8921ad`). The first successful full link is
preserved as `epoch4-linux-build-continuation.log`; the final changed-session
recompile and editor relink is `epoch4-linux-build-final.log`. The final command
used the same arguments above with a 300-second bound.

Final review also fixed viewport picking: during pending or rejected admission,
ID readback and identity resolution now select the retained renderer and its
matching identity map, rather than the hidden candidate. An initial compile of
that small follow-up caught a reference to session connection state on the
render-publication owner; it was corrected before the passing final build.
This GPU-only selection change has compile/link evidence, not interactive-pick
execution evidence. Native tests exercised the admission implementation committed
as `d5a9a3c2`; the later Vulkan-only picking changes do not alter that native
configuration. All final source is at `c8c4b834`.

The epoch 3 Ready/terminal-ordering blocker is resolved by the real session
fixture and worker/app acknowledgement. The remaining unexecuted graphics
checks above retain their capability labels; none is represented as a pass.


### Epoch 4 graphics follow-up after supervisor feedback

The attempted close at checkpoint `e076d134` was refused because messages
`msg-63100670472b4392a88ace4871c18bed` and
`msg-8303ade6df9c4ae39499ae1d831a3075` had arrived. They made the existing
`DISPLAY=:0` route available for ordinary AQ admission. Earlier statements that
no display was authorized are historical and no longer apply.

The AQ-wrapped `xdpyinfo` check passed (X.Org 24.1.6). The current-source Linux
editor and independently built fault-enabled smoke executable both created
Vulkan devices on **llvmpipe, Mesa 25.2.8, LLVM 20.1.2, Vulkan API 1.4.318**.
The editor reported RT available/enabled; this is software RT, not a hardware
RT or performance acceptance result. No display, driver or runner was installed.

The standalone smoke build uses the Make smoke source list, the current local
Linux engine archive for newer dependencies, and native GLFW/flecs/bc7enc
objects already built in this slot. The Vulkan context, resources, scene
renderer and sparse uploader were compiled afresh with
`MATTER_VK_TEST_FAULT_INJECTION`. The legacy Make list needed two newer mesher
test translation units; their Windows-only `_putenv_s` calls use the preserved
Linux setenv/unsetenv shim. No test result is stubbed. Initial missing include,
link dependency and platform-call build failures are not passing evidence.
The exact compile recipe and AQ wrapper are preserved in the evidence folder.

The quick Vulkan test **passed**: three 256x256 frames, triangle/background
readback, validation errors 0, elapsed 0.589s, peak RSS 141328384 bytes. This
validates device creation, presentation and basic readback only.

A focused `MATTER_VK_SMOKE_MODE=sparse-admission` mode calls the same
`admission_rollback` fixture used by the full sparse suite, with its real baked
volume and renderer. It avoids unrelated sparse population and LOD acceptance
work. The normal full sparse mode and renderer setup are unchanged.

The first FloorDemo screenshot attempt timed out at 120s while still active,
after baking and beginning renderer resource initialization. It produced no
accepted capture and is not a pass. A 360s line-buffered follow-up and the
240s focused admission attempt are recorded below.


The 360s editor follow-up also timed out without a screenshot or bake-ready
receipt. Its last output reached RT pipeline setup after successful FloorDemo
bake and renderer resource initialization. The focused admission attempt at
240s timed out too; its AQ run contains one quick-smoke pass and one timeout,
not a rollback pass. Preserved timeout logs are named explicitly. Neither
failure proves a scene-admission deadlock; neither is accepted evidence that
scene publication succeeds on this graphics route.

The final focused run uses a 900s bound, line-buffered output and the existing
`MATTER_PIPELINE_PROFILE=1` instrumentation to identify slow driver calls.

An experimental isolation attempt used the existing renderer RT-disable test
switch to omit full-scene lighting. That switch is suitable for the skin
compute fixture but did not leave valid descriptor state for ordinary scene
rendering: validation reported a null descriptor-set layout and the process
failed before admission assertions. **That shortcut was removed.** Its logs
are preserved as `epoch4-invalid-isolation*.log`; it is neither a product-path
rollback failure nor a passing test. The final attempt uses the full renderer.

Final graphics source is `419513acca4b16d5d98e6ce9d5633a7bd41deab5`;
product source remains `c8c4b834e49f4bcde5835ac9b9a7931b3334cc0f`.
The current-source smoke executable SHA256 is
`baa95cf284d52fc24f75ec64a8f93b279227f2a95145aed942f05a3a56bce8a7`.
The Linux editor SHA256 is
`053f4b2ec563abde1c46cb556ca7d0bc87d3a850edff2742385ddaead321406c`.


**Full-renderer Vulkan admission result: PASS.** The focused fixture emitted
`SPARSE_ADMISSION_GPU mixed_ordinary_lod_primary_shadow_rollback_and_retry=pass`,
`validation errors: 0` and `ALL PASS`, and the AQ wrapper passed. This run
kept the real RT pipeline enabled; its driver creation completed successfully
in 66128 ms. It exercises actual GPU preparation, readback and resource lifetime
through capacity rejection, primary/shadow faults, forest and placement
rollback, mixed ordinary geometry/LOD identity retention, new ordinary part
rollback and later valid forest publication. The earlier invalid-isolation
attempt is not the source or binary used for this result.

The successful run provides software Vulkan/RT evidence, not hardware RT,
StreamMountain scale or device performance acceptance. Existing loader
performance warnings about unused shader outputs remain visible in the log;
there were zero validation errors. Based on the now-measured pipeline cost,
a final editor first-load capture uses a 600s bound and pipeline tracing.


Graphics commands (all runtime work admitted through AQ; no direct runtime
bypass):

```sh
aq test --aq-timeout 75 -n0 /tmp/eager-nexus-71-epoch4/test_graphics.py -k display -q -s
aq test --aq-timeout 75 -n0 /tmp/eager-nexus-71-epoch4/test_graphics.py -k gpu -q -s
aq test --aq-timeout 75 -n0 /tmp/eager-nexus-71-epoch4/test_graphics.py -k sparse_admission -q -s
aq test --aq-timeout 75 -n0 /tmp/eager-nexus-71-epoch4/test_graphics.py -k editor -q -s
aq test --aq-timeout 75 -n0 /tmp/eager-nexus-71-epoch4/test_graphics.py -k editor_raster -q -s
```

The `gpu` command was the earlier 240s attempt (quick pass, admission timeout).
The final `sparse_admission` command passed in **223.11s** with the full renderer.
The earlier `editor` commands predate addition of the separate raster test.
The RT-enabled editor's last measured attempt used its default 1280x720 window,
three settling frames and a 600s bound. A separate task-relevant software-raster
capture sets the existing `MATTER_DISABLE_VK_RT=1`, 320x240 window and
`MATTER_HIDE_UI=1`, also with three settling frames and a 600s bound. It tests
first-load readiness and successful capture, not interactive capacity rejection
or full RT lighting. Its result is recorded below.


The default-size, RT-enabled editor capture **timed out at 600s**, with no
bake-ready receipt or PNG. Pipeline tracing shows successful RT pipeline
creation in 51684 ms; the log then reports actual RT dispatches. The process
remained CPU-active during the first frame. This is not a passing UI/capture
result and does not establish acceptable software-RT performance. The log also
contains an implausibly large RT rejected-record diagnostic; this change does
not own or claim to fix the sibling RT-counter work. No fatal editor diagnostic
or admission-capacity error was logged in that attempt. The separate raster
run avoids conflating this result with first-load admission under software
raster settings.


**Software-raster editor outcome: admission observed; capture timed out.**
The product emitted `[bake] bake finished (0 errors)`, selected FloorDemo, and
reported `effective=false dispatches=0` for RT. It then entered deferred
ForestFloor/VT work. The 600s bound expired without a PNG. Thus the successful
initial admission/terminal event is observed on the actual product, but this
run is **not** a passing screenshot test. No interactive capacity-rejection UI,
rejected-request screenshot/performance artifact, hardware RT, StreamMountain
scale or device-performance acceptance is claimed. Windows remains waived.

The original capture wrapper expected `viewer: bake ready`; source inspection
shows that line is FIFO-only. The preserved corrected wrapper checks the actual
non-FIFO `bake finished (0 errors)` log instead. Both capture attempts timed out
before reaching that assertion, so this harness correction changes neither
recorded failure. The original wrapper is also preserved for provenance.

Final acceptance evidence is the five focused native checks (including the
actual WorldSession worker/app/streaming/ECS/collision sequence), the complete
Linux editor build, and the full-renderer Vulkan admission fixture with zero
validation errors. The latter proves rendered mixed-scene rollback and retry;
the native fixture proves first-load rejection and corrected/superseding
success with exactly one terminal per nonzero generation. The editor capture
limitations above remain explicit rather than being counted as passes.
