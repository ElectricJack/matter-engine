# VT reliability and throughput implementation plan

Date: 2026-09-14. Status: implementation in progress; V0–V4 partially exercised. No phase exit gate is complete.

Design: [reliable, responsive VT](../specs/2026-09-14-vt-reliability-and-throughput-design.md).
Evidence: [current-source audit](../../findings/vt-stability-and-latency-review-2026-09-14.md).
Follow-on: [layered texturing plan](2026-09-14-layered-surface-texturing.md).

## Priority update — 2026-09-15

The user moved procedural material generation, splats and visual development
into the immediate focus. See the [revised layered-material working objective](../plans/2026-09-14-layered-surface-texturing.md)
and its [generation/splat specification](../specs/2026-09-14-layered-surface-texturing-design.md).
Maintain relevant VT correctness checks, valid fallback, bounded work and
lightweight timing during that work. Full performance acceptance is deferred;
the original targets and incomplete tasks below remain unchanged and open.
References below to materials as a follow-on describe scope separation, not a
requirement to finish every VT performance gate before material development.

## Objective and boundaries

Implement and demonstrate reliable, responsive VT for terrain and buildings. Fix mandatory work loss, unnecessarily broad invalidation, visible cache destruction during updates, preparation churn and poorly bounded frame work. Preserve current POM and rendering behavior. Finish with native evidence against the design's correctness and performance targets.

Source-texture artifact changes, richer material generation, new splat authoring and composed-height POM follow this goal. Source loading still gets a separate baseline so its cost is not confused with VT page streaming. General LOD/visibility/sparse-geometry rewrites are outside this implementation slice.

## Working rules

- Preserve the dirty working tree, including active forest/renderer work. Record source fingerprints and task-owned hunks. Do not reset, clean, stash or revert unrelated changes.
- Follow [CLAUDE.md](../../../CLAUDE.md) for the canonical MSVC/CMake/Ninja build. Do not build over a running editor binary or run competing GPU captures/builds during measurements.
- Trace callers/consumers before changing ownership, public signatures, page layout, shader bindings or resource lifetime. Include raster, RT, feedback, enrichment and headless/GPU tests.
- New production `.cpp` files enter the existing CMake source manifests and supported Make inventories. Follow existing C++17, shader embedding and assertion policies.
- Tests exercise production scheduling/lifetime logic and actual GPU publication where needed. Do not validate a duplicate toy scheduler or treat a screenshot of a settled final frame as a flashing regression test.
- Every completed task records changed behavior, exact commands/exits, artifacts and remaining limitations. Check boxes only after its exit gate passes. No broad feature migration is needed to prove VT reliability.

## V0 — Establish reproducible native evidence

**Files:** existing `MatterEditor/src/main.cpp` diagnostics, `MatterEngine3/src/render/vt_residency.*`, `vt_compositor.cpp`, `vk_scene_renderer.*`, `MatterEngine3/src/matter_engine.cpp`, existing GPU timestamp facilities; add `tools/vt_acceptance.py` and focused parser/comparator tests if existing capture tools do not provide the required analysis. Reuse `MatterEngine3/tools/drive.py` for editor sessions.

- [ ] Record working-tree fingerprints, toolchain, actual GPU/driver/CPU, output/internal resolution, frame cap, lighting and effective VT budgets. Fix the reference quality and camera paths before changing behavior.
- [ ] Capture StreamMountain and CastleUpgraded stationary, turn-away/return, LOD-boundary, flight/return and local-edit sequences. Add a small deterministic VT fixture if real scenes cannot isolate page ownership and local edits. The fixture must use the production registration, feedback and compositor paths.
  The native `vt-feedback` fixture now reproduces hidden-surface detail requests and verifies the correction through the production G-buffer, asynchronous feedback and compositor. It covers charted/uncharted occluders, both registration/instance orders, reveal/return reuse and odd-extent resize. Raster feedback now follows final depth visibility and is extracted on the GPU into the existing compact transport. Real-scene latency/continuity and repeated performance acceptance remain open; see [the visibility report](../../agent/evidence/2026-09-15-vt-feedback/README.md).
  The terrain retry now passes the strict 600-frame gate after removing unused per-part CPU tracer reservations and making loader capacity follow the serialized count. Native loader/tracer/VT/POM regressions pass. Validation-enabled terrain CPU p95 remains above target at 0.2953 ms; production timing and the other V0 metrics remain open.
  The subsequent production-mode captures both pass the strict 600-frame settled gate. Terrain hook/registration CPU p95 is 0.1948 ms and castle is 0.2637 ms. These older captures omit demand CPU time and cannot establish the combined settled target. Dedicated VT GPU p95 is 0.249760/0.037440 ms for terrain/castle, excluding G-buffer work. Whole-renderer GPU p95 remains 74.43/55.50 ms. One run per scene does not complete repeated performance or active/edit/return acceptance. Exact source, environment, traces and visual limitations are in [the production timing report](../../agent/evidence/2026-09-15-vt-feedback/README.md).
- [ ] Export existing counters plus reason-coded invalidation/eviction, owner/content generations, mandatory/detail backlog, oldest request age, preparation hits/misses/bytes, failed/cancelled fills, and displayed fallback coverage. Preserve the oldest request time through deduplication.
- [ ] Capture CPU stages and retired GPU timestamps separately. Attribute registration, allocations, descriptor/input changes and waits to VT. Correlate sector/geometry publication and lighting-history resets to distinguish material refresh from other flashing.
  The dedicated trace now includes demand selection, which was previously only in the Chrome profile. The analyzer preserves the narrower hook/registration metric and refuses a combined CPU result for old or incomplete traces. Both current scenes pass the settled counter gate, but demand + hooks + registration p95 is 0.8118 ms for terrain and 0.3376 ms for castle. The CPU target remains missed. A packed feedback-scan candidate improves populated CPU benchmarks and the first terrain scan measurement, with a documented empty-buffer regression. Demand selection/linger costs and the next reuse work are recorded in [current evidence](../../agent/evidence/2026-09-15-vt-feedback/README.md).
- [x] Measure memory density separately: allocated physical capacity versus occupied slots, per-channel bytes, physical borders, chart-block padding, and triangle-covered texels within occupied pages. Record coarse-tail utilization separately. Atlas bounding-box occupancy alone is not useful-texel occupancy, and virtual holes do not imply physically allocated pages. Use a separate diagnostic capture for costly occupancy measurements so they do not contaminate timing samples. Native castle/terrain captures, analytic CPU tests, parser gates, and source/artifact manifests are recorded in [the density findings](../../findings/vt-memory-density-2026-09-14.md). Coverage uses the union of projected triangle-covered texel centers, not an exact GPU rasterization oracle or a directly reclaimable byte count.
- [ ] Store raw logs, frame/event series, camera timelines, actual settings and representative images under a uniquely owned `docs/agent/evidence/<run>-vt/` directory. Keep large local captures indexed by a manifest with hashes when unsuitable for the repository.

**Exit:** a rerunnable baseline and machine-readable measurements for all design metrics, with reproduction status for the user's symptoms. Missing metrics are implementation work, not zero-valued results. Preserve three 600-frame active samples, a 600-frame settled sample, and 30 bounded warm edits for the final comparison protocol.

## V1 — Protect mandatory tails and stale-owner handling

**Files:** `MatterEngine3/src/render/vt_residency.h/.cpp`, `vt_types.h` if identities need extension, `include/matter/vt_budgets.h`, `tests/vt_residency_tests.cpp`, GPU integration coverage in existing VT/smoke tests. Add `src/render/vt_request_queue.h` only if extracting the production queue policy enables direct tests without Vulkan.

- [x] Exercise more than 272 mandatory tails with a detail cap of 256 and tail budget 16; reproduce dropped coverage work through the actual scheduling policy.
- [ ] Separate/coalesce mandatory pending state from expendable detail requests. Bound mandatory state by live admitted owners; preserve forced refreshes until successful or explicitly cancelled/superseded.
- [ ] Carry owner generation and desired content revision through pending and in-flight jobs. Cancel released owners; reject late results after index reuse and repeated edits.
- [ ] Test initial tails, already-active tail refreshes, sustained detail pressure, failed producer fills, pause/resume under allocation pressure, owner release/reuse, and duplicate requests. Assert all live required tails eventually complete once resources/success permit it.
- [ ] Wire header-only policy tests into the native CMake test graph if absent. Retain the existing activation/slot-pool tests and add a GPU integration case proving initialized content is visible only after proper publication.
- [ ] Correct the queue-cap property's documentation and expose separate queue/drop/failure counts. A cap adjustment must not discard mandatory state.

**Exit:** no mandatory request is lost by queue limiting; no failed/stale fill becomes visible; memory and queue sizes remain bounded. This correctness fix may land independently before larger replacement work.

## V2 — Add dependency-scoped dirtiness

**Files:** `vk_scene_renderer.h/.cpp` (`update_materials`, `push_vt_compositor_inputs`, surface-update and tileset paths), `vt_residency.h/.cpp`, `vt_types.h`; relevant material/surface publication in `matter_engine.cpp`.

- [ ] Inventory every global invalidation and dirty-input writer. Assign reason codes and identify actual inputs used by each owner, including scalar fallback materials, aliases and source-texture slots.
- [ ] Add reverse dependency tracking and dirty generations. Compare effective material/source content; unrelated revision bumps and unused table additions dirty zero existing pages.
- [ ] Implement owner-scoped updates for changed material/tape dependencies. Add page-range selection for explicit local surface bounds, with conservative border and ancestor-mip expansion. If exact bounds are unavailable, dirty that owner and record the fallback reason.
- [ ] Handle source deletion, replacement, failed loading, surface-tape removal, owner release, and shared parameterization aliases. Remove obsolete reverse links; bound their retained memory.
- [ ] Test two independent owners, two aliases of one owner, shared/unused materials, bounded edits crossing a page border, and a coarse page spanning affected/unaffected fine regions.

**Exit:** local edits never invalidate unrelated owners/pages outside the conservative affected set. Device-idle/content replacement behavior is still tracked as open until V3; do not claim flicker-free replacement from this task alone.

## V3 — Retain valid content while replacements are built

**Files:** `vt_residency.h/.cpp`, slot/indirection helpers, `vt_compositor.h/.cpp`, `vt_enricher.*`, `vk_scene_renderer.h/.cpp`, `vt_types.h`, and shader metadata/bindings only where snapshot compatibility requires it.

- [ ] Add bounded replacement capacity and states for resident, dirty, preparing and publishing content. Account for current, candidate, pinned and retiring pages; preserve progress with a full pool.
- [ ] Produce candidates without overwriting visible pages or removing valid mappings. On failure or supersession, retain current content and retry the newest required revision.
- [ ] Introduce immutable compositor-input snapshots and lifetime ownership for material buffers, tileset images/descriptors and surface inputs. Commit page/binding revisions together where existing near-detail shaders require a single compatible snapshot.
  Compositor batches now retain renderer source-image allocation/device tokens alongside their captured buffers and descriptors. A native four-version pre-submit replacement regression fails before the change and passes afterward, matching all output channels and reclaiming images after retired-ring reuse. Common image helpers now support the tilesets' mip/array ranges; source uploads retain allocation dependencies. Three native builds and 17 GPU runs pass. Page/source/POM visible binding compatibility and ordinary wait removal remain open; see [source ownership evidence](../../agent/evidence/2026-09-15-vt-feedback/README.md).
  Residency now publishes per-physical-page input IDs with successful page copies, retains old IDs/bindings through failure and supersession, and retags clean compatible dependencies without fills. The native GPU fixture verifies multiple recorded readers, consecutive edits and the existing eight-frame retirement horizon. Three final-revision builds and 11 GPU executions pass. The production visible-detail regression still fails in all 12 held frames: raster/RT descriptor/material/POM consumption and ordinary wait removal remain unfinished. See [page identity evidence](../../agent/evidence/2026-09-15-vt-feedback/README.md).
  The subsequent renderer integration resolves captured material/source inputs in the G-buffer and secondary RT surface paths. Per-frame descriptor/material banks now pass all 12 held-page detail checks. A 30-edit pressure fixture publishes 21 versions immediately, safely defers nine and recovers the latest version. Three rebuilt binaries and 16 native GPU executions pass without validation errors. Primary screen-space RT material selection, held-edit POM continuity, capability checks, ordinary wait removal and performance acceptance remain open. See [draw binding evidence](../../agent/evidence/2026-09-15-vt-feedback/README.md). The user completed StreamMountain evaluation and authorized continuing work. A fresh run measures whole-GPU p95 87.68 ms and settled VT CPU p95 0.3515 ms; the older run has different geometry, limiting attribution. A tested G-buffer address-lifetime candidate records 83.24 ms / 0.2888 ms in one matching-geometry run. Both strict settled gates pass, but repeated performance acceptance and the CPU target remain open.
  The primary RT follow-through is now applied: the existing visibility attachment transports the displayed input snapshot, and all four held raw-reflection F0 regressions pass. The source-height fixture also proves held POM depth continuity. Three ordinary device-idle sites are removed; all 35 native event-observer assertions pass. Recorded-but-unsubmitted source replacement/unload preserves old frame data. Seventeen relevant native modes and the registered snapshot CTest pass without validation errors. Two wider RT/light-cull modes still fail; the archived evaluation binary reproduces the exact same one/six assertion lists. Two current StreamMountain captures pass the settled counter gate but miss the CPU target and do not establish a speedup. Filtered history at delayed publication, adapter/source-memory bounds and full performance acceptance remain open. See [current evidence](../../agent/evidence/2026-09-15-vt-feedback/README.md).
- [ ] Publish after validated fill success and GPU dependencies; retire old mappings/resources only after all raster/RT/feedback/enrichment readers. Avoid device-wide idle waits for ordinary edits and input updates.
- [ ] Tie AO candidates to the page generation and content revision. Prevent late enrichment on reused slots, repeated darkening and accidental destruction of unchanged geometry acceleration data.
- [ ] Add GPU tests for failed replacement, repeated edits during a fill, delayed old frames, full pool, tail-only ownership, input-source replacement and draw/RT agreement. Capture intermediate frames, not just the final result.

**Exit:** current valid coverage survives edits and failures; stale content is replaced locally and safely; ordinary VT updates show zero device-wide idle waits and zero validation errors. Initial/new surfaces retain the existing readiness fallback until coverage is genuinely usable.

## V4 — Reuse preparation and preserve compatible LOD identity

**Files:** `vt_compositor.*`, `vt_enricher.*`, `vk_scene_renderer.*` demand/eviction paths, `vt_residency.*` alias ownership, `matter_engine.cpp` registration service; inspect `src/lod_bake.cpp` and chart/LOD tests for existing `MATTER_VT_UNIFY` behavior.

- [x] Split immutable geometry/chart preparation from material weights/tape inputs and source bindings. Key and refcount prepared data by the actual compatible surface identity. Geometry/surface GPU streams are separate; residency-owned alias references govern exact owner-key/generation retirement. Native surface-edit, overlapping-owner and pre-submit retirement evidence is in [the preparation report](../../agent/evidence/2026-09-15-vt-feedback/README.md). Source-image batch ownership is now implemented; draw/POM snapshot compatibility remains V3 work.
- [x] Make one-rung release preserve surviving aliases' data. Replace coarse part-wide invalidation with precise release/update operations. The native renderer regression reproduces unnecessary compositor/AO rebuilds before the fix and passes afterward; residency tests cover old/new owner overlap, alias edits and exactly-once release. Compatible coarse-to-finer promotion now stages mesh data within the CPU budget and retains the owner, aliases and valid pages through failed/current/stale candidate publication; full-capacity and rendered-fin regressions pass. Real-scene LOD/return acceptance and incompatible parameterization fallback remain open below.
- [ ] Apply measured byte/rebuild-cost admission and eviction. Eliminate allocation-pressure cache wipes on the reference path where a bounded eviction/retry suffices; record irreducible oversized owners explicitly.
- [ ] Move costly CPU registration/preparation to existing worker facilities; bound render-thread upload/publication units. Reject stale worker output using the V1/V3 identities. Compositor chart/surface/tape preparation uses the existing bounded channel on a CPU worker, with immutable inputs and a 32-job / 256 MiB reservation limit. Owned requests now stage GPU buffers across frames before refinement-slot acquisition: defaults are two allocation attempts and 1 MiB copied per frame, with elapsed-time checks between driver operations and 64 KiB copy slices. CPU output leases keep their reservation until upload publication/cancellation. Native tests cover held workers, pressure, partial uploads, zero-copy pause, supersession, abandonment and GPU byte equivalence. The short native startup diagnostic reduces geometry-upload maxima from 8.57 to 0.41 ms across these runs; different startup work prevents a controlled whole-scene speedup claim. Initial registration, AO, cache byte/cost policy, nonpreemptible driver allocation and full timing acceptance remain open; see [the preparation report](../../agent/evidence/2026-09-15-vt-feedback/README.md).
- [ ] Test the existing unified parameterization path across representative terrain and building LODs. Enable sharing by default only for verified compatible families, with an explicit, measured fallback for unsupported chart mappings.
- [ ] Exercise more than the old 512-entry preparation limit and repeated LOD crossings. Verify that warm camera return reuses pages and prepared data within the measured working-set budget.

**Exit:** compatible LOD changes retain material identity; local material edits do not rebuild unchanged geometry preparation; preparation memory remains bounded and cold work is separated from render-thread publication in measurements.

## V5 — Schedule against measured time and latency

**Files:** `vt_residency.*`, `vt_compositor.*`, `vt_budgets.h`, renderer demand pass and engine registration service, editor property wiring as necessary.

- [ ] Add estimated CPU/GPU cost to admission while retaining hard batch/count/byte safety bounds. Include preparation, uploads, compression, feedback handling and enrichment in the relevant accounting.
- [ ] Reserve coverage and aged-visible service; use screen contribution/mip deficit for remaining refinement. Keep durable dirty state when an execution request is deferred.
- [ ] Split jobs that can individually exceed the frame target. Use asynchronous timing feedback with bounded adaptation; test abrupt workload changes and delayed timing samples.
- [ ] Reserve optional enrichment bandwidth only after latency-critical work; avoid repeated enrichment-driven visual changes in settled views.
- [ ] Tune against the fixed V0 workload and design targets, documenting effective settings. Do not meet timing by reducing density, POM quality, resolution or retained working-set size unnoticed.

**Exit:** design CPU/GPU and bounded warm-edit targets pass, with reported p50/p95/p99, outliers, queue age, completed pages and total-frame impact. Cold and broad-edit results are reported separately. Any target miss remains open.

## V6 — Run integrated pressure and rendering regressions

- [ ] Run focused production queue/identity/dependency tests and GPU publication/replacement tests with validation enabled.
- [ ] Run existing VT compositor, chart normal-frame, tileset and finished-surface POM checks. Exercise raster plus RT on rotated/scaled building instances and terrain seams.
- [ ] Capture stationary, camera return, LOD crossing, local edit, unused material, broad material edit, large registration burst, constrained pool, allocation/fill failure, resize and world reload cases.
- [ ] Prove bounded memory/queues and recovery after pressure ends. Treat visible coarse fallback under deliberate oversubscription separately from missing/uninitialized content.
- [ ] Repeat performance runs with validation disabled and identical reference settings. No competing build/GPU process; compare three matched active runs and 30 edits. Verify the correctness-counter conclusions also hold without diagnostic timing overhead.

**Exit:** all correctness gates pass on both scene families and the deterministic fixture; native evidence establishes performance without regressions to POM or material continuity.

## V7 — Record acceptance and finish the VT goal

- [ ] Write `docs/findings/vt-reliability-acceptance-<date>.md` with before/after measurements, exact build/test/capture commands and exits, source fingerprints, artifact paths/hashes, settings and representative intermediate frames.
- [ ] Map every design gate and V0–V6 task to concrete evidence. List any remaining unrelated geometry/lighting/source-loading issues separately, without claiming they were fixed by VT.
- [ ] Update this plan, the design status, documentation index and the texturing roadmap entry only to the demonstrated endpoint. Keep layered materials marked as follow-on work.
- [ ] Mark the active VT goal complete only when implementation, native correctness and measured performance acceptance are all done. If evidence cannot yet be captured or a target is missed, retain the open work accurately.

## Build and capture entry points

Execution evidence: [2026-09-14 native baseline and queue regression](../../agent/evidence/2026-09-14-vt-01/README.md). This records partial progress and remaining gaps; it is not the final acceptance report.

From the repository root:

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
./tools/build-windows-from-wsl.sh RelWithDebInfo vt_compositor_tests
./tools/build-windows-from-wsl.sh RelWithDebInfo vulkan_smoke_tests
```

Add the V1 policy test target through the current native test registration helper if it is not already present. Use the wrapper-resolved native CTest and generated test inventory; run exact focused names with `--output-on-failure --no-tests=error`. Do not assume a custom aggregate such as `castle_surface_parallax_checks` runs its dependencies: it builds them; execute the tests/modes too.

Use [QA cookbook](../../agent/qa-cookbook.md) and [control surface](../../agent/control-surface.md) for `drive.py`, timeline commands, stats and screenshot completion receipts. Native POM regression mode is `MATTER_VK_SMOKE_MODE=surface-parallax`; retain the established normal-frame/tileset/raster modes. Extend discovery/docs when adding diagnostics instead of inventing unsupported FIFO verbs in acceptance scripts.
