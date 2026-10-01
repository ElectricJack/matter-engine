# Static-default integration candidate validation

Task `quick-meadow-68.4`, September 30, 2026. The merged static-default
candidate builds natively, passes the focused checks except the retained
shader-source baseline, and completes all four serial scene captures. VG
stays opt-in, POM stays off and the restored three-rock population retains
the terrain work. **Visual parity and 16.7 ms acceptance remain unproven.**
Publication of this candidate to `main` is a separate delivery step.

The candidate merges main `8aab2e54bbbffbb0f6044ec3bda36974e30b864b`
with the complete VG/terrain branch, the VG-off runtime changes, and the
three-rock restoration. Merge commit
`8e2a9b923f7ef7c142c085bb7828ab40f67cffe9` has parents `b16fbffa3` and
`8aab2e54b`. The task branch is `aq/quick-meadow-68.4`; main publication is
owned by the subsequent delivery task and AQ integration service.

## Candidate behavior and merge resolution

VG remains opt-in through `MATTER_GEOMETRY_PAGES=1`, and POM remains off.
The [rock restoration](streammountain-rock-density-restore-2026-09-30.md)
keeps three focal placements at cube resolution 128, three assets and
589,824 authored source triangles. Natural mountain rocks, forest clearance,
128 terrain texels/metre, 0.25 m finest terrain sampling, the continuous
mountain surface and its relief remain intact. Authored source triangles
are not resident or rendered triangles.

The eight conflicts predicted by the
[provenance audit](vg-integration-provenance-and-rock-population-2026-09-30.md)
were resolved by retaining both capabilities: geometry and terrain tests plus
main's GI bake and OBJ export; targeted world-definition test selection plus
GI assertions; the grouped Kreuzenstein scene plus project-tier brick policy.
The engine source manifest also removes four duplicated AssetStore entries.
The validated source graph contains 168 core, 21 surface and 27 viewer
translation units, plus optional retopology: 217 with retopology, 216 without.

## Native correctness evidence

Canonical MSVC RelWithDebInfo builds completed successfully. Resource-heavy
C++ suites and GPU checks ran serially. The repeatable commands and raw logs
are in the [evidence directory](../agent/evidence/2026-09-30-static-default-candidate/README.md).

The initial 23-test CTest run passed 21 tests and returned exit 8. Regenerating
stale export fixtures and rerunning the unchanged golden assertions repaired
one failure; the final result is **22 passing suites and one retained baseline
failure**, rather than a clean CTest run.

| Area | Result |
|---|---|
| Geometry runtime default-off, hierarchy/cut, PartStore | Pass |
| Actual geometry runtime/pages GPU tests | Pass |
| World definition/evaluation, surface fields, sector bake/LOD, world tracer | Pass |
| Terrain field and mesher, vertex cache ordering | Pass |
| VT residency, surface material, normal-frame smoke | Pass |
| Main's GI bake, structural OBJ export, repaired OBJ goldens | Pass |
| Merged viewer source graph | Pass |
| RT, transmission, local-direct and composed-VT parallax smoke modes | All four pass; zero validation errors |
| `shader_source_tests` | Retained failure at line 88: missing literal `if (instance.water_pad0 != 0u) return;` |

The shader assertion and cull shader are unchanged from the retained VG head
`6c70efe2e`. The failure is recorded in the
[historical performance report](hdgeo-performance-pass-2026-09-30.md).
No assertion was weakened or skipped. The normal-frame readiness test passes
with the retained readiness repair and original normal/parallax oracles.
Composed-VT smoke checks independent depth, normal, registration and scale;
its deepest-endpoint maximum error is 0.000000238.

The merged bake keeps engine version 13 and representation version 4 instead
of main's 11 and 2. Main's exporter and the relocated test objects are unchanged.
CastleStone's authored 0.006 m sampling is no longer clamped to 1/63 m;
its golden consequently changes from 3,382 triangles / 2,977 vertices /
24 charts to 54,054 triangles / 45,506 vertices / 223 charts. Albedo, normal
and roughness digests change; metallic and AO do not. CastlePavingSlab and
AlpineFlower change resolved-hash metadata only. Existing
`MATTER_EXPORT_GOLDEN_UPDATE=1` regenerated six fixture files in `b555e2635`;
an update-disabled native rerun then reports ALL PASS.

Six focused JS suites pass: Kreuzenstein scene, mountain geometry site,
detailed rocks, natural rocks, forest and terrain-only. The restored
population comparison passes against `563945f01`: 1,283 to three site
placements, 19 to three detailed assets, identical natural placement sets
at each tested tile scale. These synthetic-fixture counts compare authored
trees, not the real mountain's live population. Makefile source-basename
census passes, and the frame-attribution Python tests pass all 11 checks.

## Capture protocol and results

The serial campaign starts from source
`b387c15d81ff4d5b26a195b14f1afd6793b6ad6b`. Its native editor was built at
merge `8e2a9b923`; intervening commits change only export fixtures, test
status text and evidence recipes. Editor SHA-256:
`22bb85901df3a2313bf4966545686a8c47580937a64853f4d385bbe07b28671c`.
The exact executable is preserved under
`C:/tmp/quick-meadow68-4-20260930/editor-candidate.exe`.

Both paths use the same restored authored population, world seed 20260722,
camera eye `(380,90,1600)` targeting `(420,55,1420)`, 1920×1080 output,
POM off, full shading, effective RT/GI, saved volumetric settings, IMMEDIATE
presentation and no frame limit. The camera is explicitly reapplied before
each screenshot and timed sample. Early/late warmups are 45/300 seconds,
followed by 20-second samples; screenshots are requested during warmup at
20/180 seconds. The VG opt-in uses the existing terrain profile, 3,072 MiB
requested geometry GPU budget and 2,048 MiB VT pool budget, with cold compilation allowed for
the restored scene hashes. Each run is a separate editor process.

The campaign initially waited at the existing <2,048 MiB whole-GPU gate.
The device held about 21,648 MiB at approximately 82% utilization; Windows
counters attributed about 20,681 MiB to `vmwp` PID 11420, and read-only
`ollama ps` reported a 17 GB GPU-resident model. The supervisor and dashboard
operator were notified. No competing process was stopped or unloaded.
The GPU freed automatically: static early launched at 20:43:19 PDT with
929 MiB / 0% utilization, VG early at 20:46:34 with 922 MiB / 0%, and static
late at 20:51:06 with 922 MiB / 0%, and VG late at 20:58:52 with 929 MiB / 0%.
Native smoke timings under the earlier
load are correctness evidence, not product performance.

All four runs exit 0, capture their screenshots and have effective RT,
POM off and zero validation errors. Both static runs have no `geometry.*`
trace names and no paging diagnostics. Both VG runs execute `geometry.update`.
These are single captures per condition, each measuring end-to-end cadence
over 20 seconds; GPU timestamps are reported separately. They do not estimate
variation across repeated runs.

| Path / warmup | Frames | Cadence median / p99 / max ms | >100 ms | >1 s | GPU median / p99 / max ms |
|---|---:|---|---:|---:|---|
| Static / 45 s | 170 | 108.87 / 285.11 / 309.79 | 143 | 0 | 111.11 / 169.55 / 173.20 |
| VG / 45 s | 1,412 | 12.02 / 35.17 / 9,371.54 | 1 | 1 | 12.05 / 20.39 / 21.98 |
| Static / 300 s | 258 | 75.55 / 190.00 / 218.47 | 4 | 0 | 75.91 / 88.44 / 110.85 |
| VG / 300 s | 147 | 133.48 / 222.15 / 239.76 | 133 | 0 | 133.73 / 163.90 / 164.59 |

| Path / warmup | Active instances at screenshot request | Raster batches | Raster triangles |
|---|---:|---:|---:|
| Static / 45 s | 102 | 65 | 1,417,836 |
| VG / 45 s | 91 | 20 | 38,960 |
| Static / 300 s | 531 | 138 | 9,121,592 |
| VG / 300 s | 8,722 | 4,321 | 1,612,185 |

The census is the engine's resolved draw-instance count at screenshot request
(20/180 seconds into warmup), not an authored-object count or a sample-end
census. VG cut/root representation differs from static. Streaming continues:
static vertex/cluster/instance upload deltas during the four timed windows are
respectively `5/5/20`, `85/85/97`, `5/4/15`, `63/63/67`. These windows are not
fully settled scenes.

| VG warmup | Visible assets | Ready / visible roots | Unready assets / source fallbacks | Visible unready assets |
|---|---:|---|---|---:|
| 45 s | 12 | 563 / 563 | 3 / 3 | 0 |
| 300 s | 60 | 4,388 / 4,388 | 5 / 5 | 0 |

Coverage counters describe the currently adopted representation, rather than
proof of parity with static or completion of all desired terrain. The final
logs contain cold terrain-page compilation as well as late cache hits; cache
outcome `missing` is counted explicitly in the summary. Both paths' VT census
reports 12,800 page capacity; the profile budget override is recorded as a
requested setting, rather than an inferred actual capacity. Whole-device
memory peaks are 9,575 / 9,497 / 12,302 / 10,442 MiB in table order and include
the entire device. The background resource blocker before admission is not
included in any timed sample.

The early images contain mountain surface tiles and rocks at the fixed pose;
loaded extent and rock surface detail differ substantially. Tile boundaries
are visible. VG is not visually equivalent to static in these captures, and
its lower loaded triangle count prevents interpreting the timing difference
as an equal-coverage algorithmic speedup. The 9.37-second VG interval is a
measured hitch, not removed as an outlier. The historical 1,283-rock captures
also cannot establish a speedup for this restored three-rock candidate.

Static late also passes the no-geometry/no-paging trace checks and preserves
the rendered mountain/forest scene. Its image shows the restored detailed
rocks alongside natural scatter, a forest and distant slopes. Tile boundaries
and distant horizon/coverage discontinuities remain visible. The VG late image
has a brighter surface, different distant extent and coarser foreground rock
facets. The late VG run also passes its required trace and artifact checks;
its median cadence is slower than static's despite fewer raster triangles.
The retained static default is appropriate. No source-level terrain rollback
or population reduction is credited as an algorithmic improvement.

The reducer verifies all four completed runs; partial output is explicitly
marked incomplete. Raw stdout/stderr writers interleave
inside the warmup units and early static STATS record. Numeric phase tokens
remain intact. The parser reconstructs the interrupted census only by removing
complete, known PartStore or static-upload diagnostics in memory, requiring all 28 numeric
fields and preserving the original raw log. Early static census is marked
interleaved in the summary, as is VG late. The complete raw artifacts are
preserved in four checksum-verified archives alongside the screenshots,
protocol, complete summary and host-load observations in the
[evidence directory](../agent/evidence/2026-09-30-static-default-candidate/README.md).

Screenshot pairs: [static early](../agent/evidence/2026-09-30-static-default-candidate/static-w45.png),
[VG early](../agent/evidence/2026-09-30-static-default-candidate/vg-w45.png),
[static late](../agent/evidence/2026-09-30-static-default-candidate/static-w300.png),
[VG late](../agent/evidence/2026-09-30-static-default-candidate/vg-w300.png).
