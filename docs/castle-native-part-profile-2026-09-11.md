# Native castle part profile, 2026-09-11

This is an isolated small-part profile, not a full-castle or GPU acceptance.
The fixtures retain the production CastleStone/Beam/Plank source detail and
simplification. No cache-hit number is a fresh-generation result.

## Reproduction and provenance

Build `castle_part_bench` and `castle_dependency_bench` with the repository's
MSVC wrapper. The benchmark links `matter_engine_headless`, calls the real
QuickJS ScriptHost through FileModuleResolver/HostBaker/PartGraph, decodes REP0,
then stages and commits into a CPU PartStore. It creates only a caller-supplied
**new** output directory. Each process handles one fixture; each miss uses a new
cache directory with the same host service, then repeats that install as a hit.

```powershell
./tools/build-windows.ps1 -Config RelWithDebInfo -Target castle_fast_bake_cpu_tools
py MatterEngine3/tools/castle_part_profile.py --exe MatterEditor/build/cmake/windows-msvc/relwithdebinfo/castle_part_bench.exe --repo . --output D:/tmp/castle-profile-new --samples 3 --flatten
```

Use the executable path printed by the wrapper if the local build preset differs.
The Python runner also supports WSL native-path overrides. Direct invocation:

```text
castle_part_bench REPO_ROOT NEW_OUTPUT_DIR stone0 3 --flatten
castle_dependency_bench REPO_ROOT 3
```

`stone0`, `stone1`, `beam_short`, `beam_long`, `plank`, and `slab` are accepted.
The beam lengths are exactly 1 m and 4 m. Other dimensions are schema defaults:
stone 0.72 × 0.28 × 0.42 m, timber width 0.24/height 0.28 m, plank
length 2/width 0.28/thickness 0.12 m, paving slab 1 × 1 × 0.2 m.
The slab is the existing 12-triangle CastlePavingSlab polygon extrusion.

The initial binary was frozen **before** A2 normals/bounds edits:
`D:/tmp/castle-a1-baseline-bin/castle_part_bench.exe`, SHA256
`6791e00d7fedf34379fa57d0707a9de22c0b690abed4f39e5b3274bffbfa4bc2`.
It reports `_MSC_VER=1944`. The source snapshot and per-file SHA256 manifest are
in `D:/tmp/castle-a1-baseline-sources` (66 engine/castle files).
The starting merged revision was `44220e96`. Raw output/cache bundles remain in
`D:/tmp/castle-a1-results/baseline-{fixture}.{jsonl,log}` and sibling fixture
cache directories. Runs were serialized with native builds held until completion.

The baseline uses the original harness; later additions split bundle I/O and
optionally run flatten/get_or_load. Binary hashes, source snapshots, and run
labels must therefore remain separate. The runner hashes its current source
inputs and binary; when running a frozen executable, attach its original source
manifest rather than claiming the current checkout was its build input.

## Frozen baseline

Times are milliseconds. There is one cold-service miss, two warm-service misses,
and three disk hits per fixture. This is an engineering sample, not a robust
p95 population. The runner reports nearest-rank p95 and n explicitly; with two
warm samples p95 is just their maximum.

| Fixture | Cold miss | Warm misses | Hit median | Source triangles | Bundle bytes | Fallback stage median |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| Stone seed 0 | 5298.36 | 6485.70, 7232.09 | 23.38 | 3382 | 776029 | 57.19 |
| Stone seed 1 | 2038.69 | 1813.99, 2187.46 | 25.07 | 3394 | 778765 | 53.95 |
| Beam 1 m | 2128.73 | 3018.54, 2457.00 | 62.55 | 15188 | 3467889 | 339.63 |
| Beam 4 m | 38048.94 | 38446.79, 39429.63 | 137.06 | 40620 | 9258065 | 3056.84 |
| Plank 2 m | 9209.03 | 10549.06, 10225.77 | 72.02 | 18752 | 4274593 | 632.00 |
| Paving slab | 206.99 | 137.45, 141.32 | 20.32 | 12 | 7669 | 0.49 |

The install column excludes the separately measured decode probe, CPU stage,
and commit. Decode deliberately precedes staging, so stage file reads have a
warm OS cache. CPU staging/publication has **no Vulkan upload or GPU execution**.
“Cold” describes the process/service, not an OS disk-cache purge. Warm service
means the same ScriptHost instance; individual QuickJS evaluations still create
isolated contexts. The host's shared-source fold cache is allowed to remain warm.

The exact baseline REP0 census is zero explicit persisted LOD records for all
six leaves. The explicit `stage_load` compositional fallback creates **three**
CPU LODs for each, despite their `lodBudgets=[1]` declarations. This proves a
fallback contract issue, not that normal castle publication uses that path:
`part_flatten` separately consumes VARS, and its published flat path must be
measured separately. The optional `--flatten` diagnostic now does that and
reports FLAT levels, cluster levels, and mesh counts. Three fields must not be
conflated: REP0's LOD table, a VARS budget list, and the runtime PartStore ladder.

## Attribution

| Fixture | Mesh trace range | Save trace range |
| --- | ---: | ---: |
| Stone seed 0 | 1428.72–1463.75 | 1955.49–2881.14 |
| Stone seed 1 | 1550.91–1742.88 | 108.50–186.12 |
| Beam 1 m | 1733.85–2216.83 | 132.03–357.08 |
| Beam 4 m | 37497.75–38853.56 | 204.78–226.81 |
| Plank 2 m | 8836.17–10209.37 | 109.34–171.00 |
| Paving slab | 0.03–0.09 | 41.02–89.83 |

The 4 m beam spends nearly all installation time inside meshing. The slab
already has a very cheap mesh, yet publication/storage keeps it far beyond the
1–5 ms target. Stone seed 0 experienced large write stalls that did not repeat
for seed 1; reporting one uniform storage cost would misrepresent this run.
The trace's `mesh` phase includes the current host/modifier pipeline and is not
a pure lattice-evaluation timer. The original baseline has no per-cell counters;
A2 adds candidate-count attribution for subsequent matched runs. Requested
6 mm stone spacing is still capped by the existing one-metre/pow-6 cell lattice
at 1/63 m ≈ 15.87 mm in this baseline; these timings do not prove 6 mm geometry.

Source inspection found four cold-leaf bundle replacements: render policy,
REP0 geometry, the no-impostor PLAN, and VARS. Each `write_section` reads and
parses the current bundle, serializes its complete replacement, writes/flushes,
performs `_commit`, then atomically replaces with `MOVEFILE_WRITE_THROUGH`.
The optional thread-local write observer splits these stages and records bytes
and replacement attempts without linking the header to bake_trace. No durability
operation was removed by the instrumentation. Existing hit paths already skip
up-to-date PLAN/VARS writes, so metadata no-op checks alone cannot fix cold costs.

A safe first reduction is an explicit multi-section write for static RNDR+REP0,
preserving all other sections and one durable atomic replacement. The animation
candidate/ANLK/manifest ordering must remain unchanged. Full leaf metadata
coalescing requires an explicit immutable policy payload; a broad implicit
cross-call transaction with read overlays is deliberately outside this first
change.

## Native dependency probe

`castle_dependency_bench` evaluates only CastleStone and CastleWingMasonry
with default parameters, repeating merged-parameter extraction, requires, hash,
and LOD/no-impostor metadata. It performs no geometry or full dependency walk.
The requires result is checked for expected nonempty masonry dependencies.
These are real isolated native QuickJS contexts, unlike the earlier Node profile.
Measured results will be recorded after the instrumented binary is available.

## Instrumented A2 checkpoint

The instrumented CPU executable was frozen separately at
`D:/tmp/castle-a1-instrumented-bin/castle_part_bench.exe`, SHA256
`b0b72922e3f8629dd88f9b534f1ad486b93943a6baea16f4ffee7bca537e92dc`.
Its checkpoint includes A2 conservative box candidate bounds, exact ordered-field
normals and the root's modifier-normal changes; it predates static write
coalescing. The adjacent `sources.sha256.json` records relevant source hashes.

The physical 4 m beam's mesh phase fell from 37498/37861/38854 ms to
5170/5476/5680 ms across cold and two warm misses, approximately sevenfold.
Warm install fell from 38447/39430 ms to 6421/6220 ms. Source output remains
40620 triangles and 9258065 bundle bytes; this census alone does not assert
byte-identical normals or prove visual parity. The compositional fallback's
three-rung CPU stage still costs approximately 2.9–3.1 seconds.

The slab isolates storage overhead without expensive geometry:

| Slab install | End-to-end install | Four durable flushes | Four atomic replaces | fwrite | Merge/serialize |
| --- | ---: | ---: | ---: | ---: | ---: |
| Cold service | 739.40 | 666.30 | 22.46 | 0.26 | 0.06 |
| Warm miss 1 | 124.39 | 93.15 | 3.64 | 0.24 | 0.04 |
| Warm miss 2 | 134.27 | 103.16 | 3.17 | 0.27 | 0.04 |

All these installs perform exactly four replacement attempts total, meaning no
replacement retries. File durability flush, not byte copying, dominates. Cache
hits perform zero writes/flushes. The four writes emit 22955 bytes in total for
the final 7669-byte bundle. Durability is retained by the proposed coalescing;
reducing four flushes to three still cannot justify a 1–5 ms end-to-end claim.

The slab's persisted VARS has one budget, PLAN has `no_impostor=true`, and no
FLAT exists before the optional diagnostic. `flatten_part` then writes one
level and one cluster. A fresh PartStore `get_or_load` publishes one whole-part
rung and one per-cluster rung, with two mesh arrays. Those two arrays are not
two distance LODs. Flatten costs 17.76/36.17/40.75 ms and get_or_load costs
6.17/6.22/5.62 ms, separately from install. The fallback instead creates three
12-triangle meshes. This pins the extra-rung problem to that fallback.

The initial dependency probe deliberately rejected CastleWingStructure's empty
default manifest/record identifiers: those defaults are not a resolved structure
record, so its fail-closed empty requires result must not count as a successful
measurement. The corrected probe uses CastleWingMasonry's valid default wing,
which invokes the same full-site manifest path and has nonempty stock dependencies.
Native leaf statics alone cost around 2 ms per extraction and 4 ms for requires
(which also merges defaults); this is independent of meshing and bundle flushes.

## Two-section checkpoint and native dependency result

The tested static RNDR+REP0 API publishes both sections once; the native
`part_asset_v2_tests` passed byte-equivalence, unrelated-section preservation,
policy/geometry round-trip, exactly-one-publish, and invalid-input/candidate-I/O
failure coverage. Existing animation candidate calls retain their ordering.
The checkpoint executable SHA256 is
`f17b303a1bb5d8d436116e6fdb8ee3b5fe10f1df0e3798506fe9f071186de923`;
receipts are in `D:/tmp/castle-a1-coalesced-bin` and `coalesced-*.jsonl`.

The slab now performs three durable flushes, but these samples do **not** show a
wall-time speedup: warm misses are 165.78/181.35 ms, including 121.44/146.21 ms
in the three flushes. Earlier four-flush samples were faster because per-flush
latency varied substantially. The verified improvement is one fewer atomic
publication/flush, not an invented stable latency reduction. All nine native
beam/floor-shell geometry cases also pass at this checkpoint.

The corrected native dependency probe succeeds and returns 20 stock dependencies
for CastleWingMasonry:

| Native operation | Three samples, ms |
| --- | --- |
| requires (includes default merge) | 3809.01, 3864.88, 4527.07 |
| separate default merge | 46.87, 36.17, 51.21 |
| hash resolution | 41.30, 47.62, 36.47 |
| budget extraction | 38.47, 51.72, 34.51 |
| static LOD extraction | 36.58, 47.33, 34.09 |
| no-impostor extraction | 35.45, 47.42, 34.01 |

The same host is reused, yet each `requires` evaluation again builds the full
site/wing manifest in its isolated QuickJS context. Native evidence now supports
the earlier dependency diagnosis. Repeated stock placements are not themselves
repeated mesh recipes, but authoring/requires work remains repeated. A compact
immutable module descriptor or install-scoped resolved-data snapshot must
preserve exact source/params/dependencies and reload invalidation; arbitrary live
JS object caching across contexts is not a safe substitute.

A following implementation combines source-proven singleton VARS and optional
no-impostor PLAN with RNDR+REP0 in one durable publish. It carries the same
source hash into retained geometry and treats only a strict matching singleton
VARS as proof for disk fallback staging. No-impostor by itself is not singleton
proof. Terrain, authored child dependencies, animation, accessor metadata and
non-singleton ladders retain their original path. Focused singleton tests and
post-change measurements gate acceptance of this subsequent step.

## Selected-wing dependency work

`castleSiteWingManifest` now decorates only the requested wing when the full
site program is not already available. `castleSiteProgram` keeps its complete
site contract. A private selected-plan cache can supply that later full-site
call through a clone, avoiding aliases to its returned mutable authoring data.
Raw site plans are never decorated in place. This is a per-context JavaScript
change, not cross-context caching, and imported source hashes invalidate affected
parent recipes normally.

`castle_site_catalog_lazy_tests.mjs` passes 60 exact manifest and masonry-child
list comparisons over all three sites, two seeds, every wing, and both API call
orders. Neighbor-seed separation, repeated memo identity, mutable full-site data,
and independent module-context caches are covered. The existing all-three-site
program integration/footing suite also passes. Native before/after requires
measurements are still required to quantify this change.

## Final singleton and filesystem comparison

The final frozen native executable is
`D:/tmp/castle-a1-singleton-bin/castle_part_bench.exe`, SHA256
`7e558a8165b803d21fd25f11a133630b15c257d607f4ba18be7b6dc264643cd5`.
The adjacent source/executable manifests record this checkpoint. All five native
gates pass: singleton policy, ScriptHost, PartStore, artifact v2, and the expanded
25-recipe shell gate. Logs are `D:/tmp/castle-a1-results/singleton-*.log`.
The final serial timing batch was surfaced in the visible progress console via
`D:/tmp/castle-live-tests/progress.log`; no concurrent build ran during timings.

Eligible static leaves now publish RNDR, REP0, VARS and optional PLAN in **one**
durable atomic replacement. Exact source/hash proof gives both retained-memory
and disk compositional staging one full representation. Malformed/foreign VARS,
authored children, functional dependencies, accessor metadata, terrain and
animation do not acquire this optimization. The normal FLAT route already
honored the singleton budget; this fixes the separate compositional fallback.

| Fixture/output volume | Cold miss ms | Warm service misses ms | Durable flush ms, cold/warm/warm | Replace ms, cold/warm/warm |
| --- | ---: | --- | --- | --- |
| slab D: | 1434.17 | 1073.01, 648.33 | 806.78, 650.22, 532.93 | 611.35, 396.27, 99.80 |
| slab C: | 18.03 | 16.57, 16.22 | 1.17, 1.01, 1.15 | 0.37, 0.50, 0.39 |
| 1 m beam D: | 2252.53 | 1990.79, 1973.01 | 336.24, 106.55, 80.88 | 5.06, 0.62, 0.59 |
| 1 m beam C: | 1768.93 | 1825.57, 1892.55 | 2.48, 2.72, 2.81 | 0.41, 0.43, 0.46 |

All these misses have one replacement attempt, one durable flush, and identical
fixture triangle counts/bundle sizes across volumes: slab 12 / 7669 bytes,
beam 15188 / 3467889 bytes. Each uses a fresh explicit cache root. C: and D:
were measured sequentially, so this isolates a large observed storage-path
association, not a controlled hardware attribution. D: latency also varies
substantially within the session. Durability has not been weakened.

On C:, the slab warm-miss remaining work includes native requires 6.76–6.92 ms,
hash resolution 3.41–3.62 ms, and bake/encode/write 5.84–5.87 ms. Post-bake
metadata hooks are effectively free from live-context extraction. Disk hits
still perform independent metadata validation/evaluation and take 19.62–23.42 ms;
cache hits are not evidence of the cold-bake target. The 1 ms full bake target
is not met, even on the faster output volume.

A bounded final 4 m beam miss takes 4790.18 ms, with 40620 triangles and
9258065 bytes, matching prior counts/size. Its compositional stage now takes
145.05 ms (140.91 ms after the hit), versus roughly 3 seconds for the old
three-rung fallback. It publishes one rung. This checkpoint combines A2
candidate/normal changes and singleton handling; identical counts alone are
not proof of byte or visual equivalence. GPU upload, rendering and BLAS upload
are outside these CPU-publication measurements.

Native selected-wing requires now takes 2098.87, 2352.04 and 2932.33 ms with
20 dependencies. Compared with 3809.01, 3864.88 and 4527.07 ms before the
catalogue change, the three-sample median improves 1.64x. The 60 exact JS
manifest/child-list comparisons establish output equivalence; native timings
establish a real QuickJS improvement, while also showing substantial remaining
seconds of authoring work. The compact fixed-part shell path avoids this
whole-site dependency work; no full-castle geometry bake was used here.
