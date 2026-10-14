# CPU runtime fixture migration checkpoint

Task: steady-harbor-62. This is an incomplete validation checkpoint, not a full
suite acceptance report. The root CMake project remains Windows/MSVC-only.

Generated sources use owned `SourceSnapshot` inputs. Generated cache artifacts
use explicit `CacheByteStorage` publication and immutable read handles. The
production codecs, checksums, version vectors, hash validation, bundle section
merge locks, and default filesystem behavior remain in place. Worker callbacks
retain the fixture IO and bind the cache on the executing thread. No global VFS
or new persistence policy is introduced.

## Coverage changes to disclose in the PR

| Runtime change | Retained guard or qualification | Reduced coverage |
| --- | --- | --- |
| Asset root lookup uses injected directory metadata | Same precedence cases | OS directory probes are no longer runtime |
| Shader override lookup uses injected source bytes | Same environment/explicit/embedded precedence; real source predicates and negative controls run in `matter_shader_source_qualification` | Runtime no longer scans production shader text |
| World definition generated fixtures use RAM | Grammar, values, diagnostics, import semantics use actual loader; shipped-world/layout/source checks in `matter_world_source_qualification` | Shipped geometry and OS layout are build qualification |
| Part bundle fixtures use RAM | Actual codecs, checksums, schema/version/hash rejection, corrupt bytes, merged sections, failed publication, immutable generations | OS write/rename transport replaced by injected outcomes; actual replace/static publication and Windows sharing retry in `matter_part_io_qualification` |
| PartStore generated bundles use RAM | Existing policy, staged load, LOD, culling, allocation and verified checksum guards | Prepared geometry pages and animation sibling publication run in `matter_partstore_source_qualification` |
| Flatten fixtures use RAM and child pipes | Two real cold processes with distinct triangle padding poison compare every generated bundle byte | Repeated impostor samples use supported 16px instead of 32px; explicit 16/32 scaling/identity/stale guards remain |
| Async generated fixtures use RAM | Existing event, cancellation, supersession, retention and publication guards | Linux disk live-edit and shipped animation/fluid cases run in `qualify_async_sources` |
| Async request timing uses an actual worker barrier | Checks caller thread, return independence, terminal/pump behavior | The former 50ms performance threshold is removed |
| Viewer runtime uses a tiny generated tree hierarchy | Cold/warm cache, flat root, retained children, hash chain and fetch progress checks; actual Demo checks in `matter_viewer_source_qualification` | Large particle-flow/import/material demo geometry is build qualification |
| Async return proof blocks immediate worker delivery of `BakeStarted` | Request returns while the real command worker is blocked; no completion before pumping, eventual completion and fault-injection cases remain | The app cleanup barrier is pumped after the original pre-pump check so the worker reaches the subscriber; the old publication fault hook cannot be reached without pumping |
| Resolve cache, world tracer and chart atlas use RAM | Same serialized source/key/corruption, allocation/duplicate decode/ray lifetime, chart byte-equivalence checks | OS artifact transport no longer runtime |
| Flat reference scan uses RAM bundles | Full-loader parity, sentinel registry, corrupt grammar/trailers/hash and caller preservation remain | OS artifact transport no longer runtime |
| Authored provider restore uses owned source snapshots | Actual cold JS load, key invalidation, throwing-source bypass, material/props restoration and corruption fallback remain | Mutable files become explicit source generations; final valid fallback uses a provider owning the next snapshot |
| Water boundary and animation sidecar suites | Numeric packing, digest, metadata, bounded requested-frame decode and corrupt-byte guards remain; exact immutable-save predicates in mandatory filesystem qualification | OS immutable publication runs during builds |
| Hydrology artifact and network suites | CPU fallback, manifest ordering, dependency/path confinement, identity/digest, serialized corruption and transactional output guards remain | Atomic cache hits/replacement, animation package/native identity, allocation-handle closure, namespace confinement and source predicates run in mandatory filesystem qualification |
| Surface vertex and tileset bake use owned RAM sources/cache | Original attributes, errors, collider, settle, trace, pose-hash and missing-child guards remain | OS source/artifact existence and chdir no longer runtime |
| Shared modules use build-embedded exact fixture bytes and owned generations | Parsing, transitive/ordered folds, hashing, bytecode isolation, RNG and scatter assertions remain | Mutable-FS edit/clear-only invalidation becomes setter-generation invalidation; prior-generation isolation added |
| Finite/solid receivers and brick detail use exact build-embedded import closure | Original eight-seed recipes, face bounds, geometry/material identity, weathering, triangle counts and cache-recovery guards remain | Artifact-free checks observe RAM publication rather than an OS directory census |
| Eval-world shipped scenes and river Make source closure move to filesystem qualification | Original shipped predicates and2401-sample mountain grid retained during builds; generated field/material/habitat guards remain runtime | Shipped scene/grid/source predicates no longer runtime |
| Brick atlas GTEX roundtrip moves to filesystem qualification | Original writer/reader byte-equality check retained during builds; all numeric atlas guards remain runtime | OS GTEX transport no longer runtime |
| Encoded VT persistence/async storage moves to filesystem qualification | Original storage, eviction, alias, admission, cancellation and shutdown guards retained during builds; complete numeric codecs/layout/version/size/corruption plus owned lease/replacement guards remain runtime | Bank eviction, persistent atomic publication and async store behavior no longer runtime |
| Physical adapter session checks move to filesystem qualification | Original WorldSession fluid predicates retain the actual compile definition; frame/backend/emitter/collision/product validators remain runtime | Generated filesystem session and package behavior run during builds |
| Complete asset export package moves to filesystem qualification | Original 21-file receipt, nonoverwrite and serialization remain during builds; actual validator and transactional failure admission remain runtime | Runtime no longer serializes a valid package to disk |
| Sparse-voxel diagnostic fixture roundtrip moves to filesystem qualification | Original writer/reader support-plane checks retained during builds; generated RAM voxel, area and moment guards remain runtime | Diagnostic OS fixture transport is build qualification |
| Projected face cache uses explicit RAM storage | Same version, recipe identity, size/checksum, six corruption cases, cancellation/currentness and one-repair guards; injected failed publication retains prior generation | OS staging-file census runs in filesystem qualification |
| Conifer, singleton and material surface providers use RAM sources/cache | Original production hierarchy, LOD/impostor, policy repair, source provenance, warm preparation, cancellation, binding and publication guards remain | Directory absence becomes absence of RAM publications |
| Geometry hierarchy uses actual page codecs with owned RAM page leases | Full cut and exhaustive availability coverage, dense/reference equivalence, numeric compilers and corrupt page guards remain | Persistent writer lifecycle, root-bank admission, prepared identities, residency and page-cache pressure run in filesystem qualification |
| OBJ transport and shipped golden export move to filesystem qualification | Original roundtrips, deterministic exports, golden bake/text/digest/DSL comparison remain during builds; runtime formatting, texture/UV, admission plus bounded embedded OBJ/MTL/digest consistency remain | Runtime no longer compares a fresh export against the shipped golden |
| BLAS device cache persistence moves to filesystem qualification | Original committed-write, reopened identity, batch association and damaged pack checks remain during builds; real worker admission, cancellation, disabled completion and byte-size rejection remain runtime | Runtime no longer verifies persistent successful reads/writes |
| Water animation playback uses serialized RAM fixture bytes | Original complete-file/high-water accounting, frame clock/capacity/span validation count, decode oracle and transactional typed fallback remain; original FS fixtures qualified | OS artifact transport is build qualification |
| Sector bake uses exact build-embedded WorldSector/imports and RAM bundles | Original requires, hash/field invalidation, nested terrain/scatter, habitat and profiling checks remain | Source reads and bundle-size observation use owned bytes |
| Script-host cases use private RAM byte stores and exact embedded particleflow/strands/rng modules | All original 65 cases plus owned-source generation isolation remain; actual bundle corruption/reload checks use the production codecs | Runtime no longer tests OS bundle transport; the separate frozen separation/source-hash/oracle-tamper audit is outside the RAM runtime graph |
| `PFstamp_paths_positive` fixture shrinks | Stamp geometry, expiration and cache reuse remain | Particles16→4, maxAge60→8, ticks80→12 reduce dense overlap and long-history coverage |
| `PFstamp_paths_outside_session` fixture shrinks | Outside-session failure and stamp behavior remain | Particles16→4, maxAge60→8, ticks80→12 reduce dense overlap and long-history coverage |
| `PFdeterminism_double_bake` fixture shrinks | Two independent bakes still compare their actual serialized bundles | Particles32→4, maxAge60→8, ticks120→12 reduce dense overlap and long-history coverage |
| Terrain collision artifacts use the owner's explicit storage adapter | Original codec, identity, cancellation, transactional publication, retry, winner and parallel/nested isolation predicates remain | Runtime injects transport outcomes; OS persistence and Windows sharing/native move behavior are no longer runtime fixtures |
| Terrain ravine mesh fixture crops the outer field | Resolution0.5, the authored ravine and retained material/mesh predicates remain | Outside scan extent64m→8m reduces large peripheral scan coverage |
| Demand bake and terrain verbs use owned source snapshots and private caches | All nine demand groups, hierarchy, roots-only/lazy/full policies, actual section validation and session behavior remain | OS directory creation, source writes and cache cleanup become owned generations |
| Sector-streamer uniform part-count comparison uses the actual desired map | Same full authored reach/pitch,78201-sector count, independent grid census and nested ratio; historical complete uniform publication drain runs in mandatory filesystem qualification | Runtime no longer drains every uniform request merely to count it; cap8, hole/upgrade/dwell/publication/interleaving guards are unchanged |
| Sector-streamer stationary setup ends after eight quiet updates | Requests and evictions both reset the quiet counter; full 4000-update setup loops remain in mandatory filesystem qualification; moving coverage, variant-mismatch, failed-child and abandonment predicates remain | Runtime drops the remaining stationary idle soak after the setup has settled |
| Dense flattening integration uses400 triangles/160-triangle clusters over the original200x100 extent | Split, format/codec, triangle conservation, retained-versus-rematerialized byte equality remain; original40k/16000 integration cases run in mandatory CMake and Make qualification; direct40k splitter/determinism remain runtime | Runtime integration drops the dense40k scalability workload; the full workload moves to builds |
| Terrain collision session uses owned source snapshots and byte cache | All 13 lifecycle groups retain original app-publication/candidate-observer timing, tile-N replacement before releasing blocked A, collision-free B, cancellation/winner/manifest/failure predicates | OS source/cache persistence and IO-error behavior become snapshot/byte-store semantics; those transport behaviors are not qualified |
| Cave-field parity uses original ray endpoints | All 1600 rays, exact y192/-127.75 endpoint signs, mesh/roof/seam predicates and original parity assertion remain | 2044800 interior density evaluations are no longer executed |
| Large-index QEM uses 128 real triangles above UINT16_MAX | Real indexed simplifier, >65535 input vertices/indices, significant reduction, finite/bounded output and other collapse fixtures remain | The original 166400-triangle/83781-referenced-position dense scalability workload is optional --dense-qem, outside default CTest and mandatory build qualification |
| Editor viewport capture uses explicit byte publication | Exact capture header, extents, plane bytes, existing-path and failed publication guards | OS fopen/rename outcomes replaced by injected publication |
| Screenshot completion marker uses RAM publication | Exact captured-newline bytes and failed publication | Runtime OS fopen/fwrite/fclose/remove guard is dropped |
| Editor registration census and Windows wrapper contract become ALL build qualification | Original registry/device/PE/source/contract predicates | Runtime no longer starts native editor for census; startup allowance increased from30s to120s |
| Vulkan smoke CPU mode returns before GLFW/device startup | Existing CPU lighting/GI/raster/payload/capability/cache/temporal/Streamline/DLSS checks | Previous default GPU/window smoke retained as separate `vulkan_smoke_gpu_tests`; source predicates moved to shader qualification |

## Evidence so far

- Published source/cache foundation: `91b98edce80e4af4fbb40136311c86e6174c49e3`.
- Published actual consumer scopes: `2dd2844099c6d00a61a3cc59cacea457779245fb`.
- Explicit partial Linux CTest graph:11/11 passed with `ctest -j4`,0.09s.
  JUnit:`/tmp/steady-harbor-core-linux.xml`.
- Production part-asset-v2 runtime:all guards passed,0.04s.
  Log:`/tmp/steady-harbor-part-codec-runtime.log`.
- Its actual OS qualification built and passed through the existing Make closure.
  Log:`/tmp/steady-harbor-part-codec-build.log`.
- Actual Raylib Linux prerequisite built in `third_party/raylib/build/linux`.

Exact terrain61f46fbc276e1c7282506ead981c74bf9eb203f5 AssetStore include and
exact importer93cf3359ad345d20aee0136fe19e0cc942068700 four-file payloads are integrated.
The common owned source-tier adapter supplies the selected module bytes to actual
provider and tileset hosts. The updated actual fixture IO executable compiled
and passed all guards, including transitive/canonical folding and prior source
generation isolation. These exact importer files, the adapter and fixture guards
are recorded in local checkpoint `5144cc753`. Later test migrations are undergoing
real GNU syntax checks. The first pass found an ADL collision in the shared-module
test helper, a dependent-template keyword missing from the river phase probe and
a missing explicit `<cstring>` include in the physical adapter tests. The final
provider checks also found an incorrect `ResolvedModule` namespace in three
fixture files. These issues were corrected without changing their predicates;
the sequential follow-up batch included every corrected file and all13 variants
passed (`/tmp/steady-harbor-followup-fixture-syntax.log`).
Logs are `/tmp/steady-harbor-new-fixture-syntax.log`. Syntax checks do not
establish linkage or runtime success.

The registrar discovers the common engine CPU tests and10 canonical editor CPU
closures. It preserves the full literal source list, including viewer controller
translation units. Core tests link their actual translation units to the real
canonical Make headless archive; there are no passing stubs. Missing archive or
link prerequisites fail the build. Five peer-owned graphs remain separate, and
Linux Vulkan smoke linkage is still unsupported in this common graph.

The Ozz and Raylib Linux prerequisites built successfully. The first canonical
Make archive attempt failed at the missing Linux glslc prerequisite; its failure
log is `/tmp/steady-harbor-linux-core-archive.log`. An isolated Linux glslc is now
available in `/tmp/steady-harbor-glslc-linux`; package hashes are recorded there.
The new real archive build uses that compiler at-j1, with log
`/tmp/steady-harbor-linux-core-archive-glslc.log`. The archive progressed through real shaders and several engine translation units,
then failed at the POSIX `struct stat final{}` declaration in hydrology network
loading. The local was renamed `final_status` without changing metadata checks;
the incremental real archive resume log is
`/tmp/steady-harbor-linux-core-archive-resume.log`. No archive or full-suite pass is
claimed yet. Full common CMake configuration succeeded.

The authorized single canonical native full build has not been used. Its
sequencing prerequisite is satisfied: the supervisor verified scripting final
`9b689a8c03609948e454a890d52ec900a8a00328` default configure/build exit0,
941.48s, parallel1, with18 manifest entries and four executable hashes.
Full log SHA256 is
`22e2b2452d4dda0e437c02cb1327b1201b2be6e8e7fa8655780214d227fe6b86`.
The peer's83-test scripting slices passed on Linux in15.56s and Windows in17.17s;
the full66-case Linux script host trace contains no fixture reads. These are
peer-scoped receipts, not this worker's full-suite acceptance.

Integrated owner payloads are terrain081292a61f129556e98bcb4d5f868cb5df50c85f
(ten owned files and selective Make closure) and scripting final9b (three test/
embed files, four importer files already checkpointed byte-exact). The incoming
script payload and four evidence hashes were independently verified before
integration. Common native/Linux generated-header rules retain real source
dependencies; no generated module is checked in as a second source of truth.

The original sector-streamer two-TU Linux binary passed all guards in254.62s.
Test blob:e033f9afaa0bb7f720bd6fa35a796c17f37d6648;
production blob:c733cb5515ea068f8812d44d1ddc2a959a252e91.
A `/tmp` candidate with a read-only desired census and unchanged scheduling
guards passed in41.41s. This is provisional candidate evidence: its actual
workspace integration is complete; full-suite acceptance remains pending. The candidate's complete historical
qualification branch also passed all guards in275.44s; no publication guard was
discarded to obtain the shorter runtime.
Logs:/tmp/steady-harbor-sector-streamer-{before,census}-runtime.log.

The portable Vulkan candidate shares15 existing native CPU contract functions
and four helpers through one include:65178 bytes moved verbatim, with recorded
per-function original line and SHA256 identity. The native Vulkan production
closure remains exactly80 unique source files. The Linux candidate links actual
renderer/device/cache/temporal/GI/lighting/Streamline/raster/indexed-geometry/
matrix/editor implementations plus actual Flecs and ProfileLib, with dead code
removal for the unused GPU entry points. All guards passed in0.06s. Its file
syscall trace contains only executable/dynamic-loader reads and no sources,
assets, cache, evidence or build artifacts. This candidate has not yet been
validated by the one fresh native build; its shared test include and actual
production closure are now integrated into the workspace.
Evidence:/tmp/steady-harbor-vulkan-cpu-candidate/.

The canonical archive's original Flecs C17 recipe independently failed GNU
compilation becauseCLOCK_MONOTONIC/nanosleep require POSIX feature declarations.
A Linux-only `-D_POSIX_C_SOURCE=200809L` build-policy fix is integrated, and the
same actual Flecs source compiled and linked successfully in the portable
Vulkan candidate. It changes no Windows flags or production algorithm; the
archive build was harvested with exit1 at that exact Flecs admission failure
before source/Make changes. Failure-log SHA256 is
`3c2d4ace30ff3602612159f5a81d39ddf7e603f92e206f6e7921c03c6142fde7`.

The original inactive geometry runtime CPU guards are registered on Linux
against the actual runtime, residency, asset-store and memory-bank code.
The native `--gpu` lifecycle and Windows thread census are unchanged. The
portable CPU build rejects GPU arguments with exit2 rather than silently
accepting an unavailable mode. Its candidate passed in0.03s with only executable/
loader file reads. Evidence:/tmp/steady-harbor-geometry-cpu-candidate/.

The first integrated common Linux graph configured135 actual CPU tests,
including geometry, Vulkan, cloud-layer and volumetric-quality suites. Both
invoke the environment owner's shared qualifier. The collision-session owner
has since delivered19369273bffa6becab8cbfd1c0eece055f0634ee. Its two files are
imported selectively without the integration parent; the root registrar now
includes that real target. Final-source full Linux/native acceptance is pending.

The integrated worker reload/collision route passed actual GNU `matter_engine.cpp`
syntax checking and the actual fixture-IO codec/atomic-publication executable.
Logs:/tmp/steady-harbor-integrated-{worker-syntax,fixture-io-build,fixture-io-runtime}.log.
Its synchronized
IO generation is atomically published by the existing worker after cancellation/
quiescence; provider copies and callbacks retain strong generations. The original
configured IO remains immutable. Collision transport delegates atomic no-replace/
replacement moves to the explicit cache; failed moves preserve both generations
and old immutable read leases survive successful replacement. These checks do
not establish a complete linked session runtime or native success.

Before acceptance: finish runtime fixture census and reductions, run the full real Linux CTest graph and native CPU
suite under one minute, run the one authorized full native build from the active
workspace, record before/after slowest20, publish commits and open the PR.

## Reconnect context blocker

After a transient daemon restart, claim recovery confirmed the same held task
and epoch. The required prime call returned `context.required_over_budget`,
without a retry delay or refusal count. The supervisor was notified once.
The edits and held claim were preserved with one durable wait. The supervisor's
short current dependency brief supplied the missing exact payloads and instructed
continued CPU work; that wait was cancelled once. There has been no repeated
prime retry, blocker report or native full attempt. GNU syntax evidence covers
six earlier fixture/qualification translation units plus resolve cache, tracer
and chart atlas. The later migrations still need compilation and runtime checks.

## Later integration checks

All ten actual integrated editor CPU executables passed under strace in0.36s
CTest time (runner0.494s), with no non-loader path calls in their test processes.
Trace SHA256:8c236a5806dd2b5459ad85601ee777e47af5688023a393a8ff9f84fcfa9b048e.
This is an editor-only result, not full-suite acceptance.

The collision owner found mixed-generation reads in the provisional adapter:
size/header/payload calls reopened mutable cache keys. The adapter now retains
one immutable handle per caller/path from file_size through reads; a later
file_size refreshes it, including a missing acquisition. Two concurrent readers
acquire different generations of the same path, then replacement and erasure
precede their complete reads. Each must retain its own exact bytes. The corrected
fixture executable passed every guard; the original adapter negative control
failed the new concurrent-generation and missing-acquisition guards (exit1).
Actual module_resolver, part_asset_v2 and SurfaceLib part_asset TUs were compiled
and linked without substitutes. Corrected runtime tracing has no non-loader
path calls; SHA256:636acd9183c282aee4bfb0bf6be50fd0a8a9d0b59dfd5885058cf35f2f317bec.
Receipts:/tmp/steady-harbor-collision-read-lease-candidate/build-receipt.json
and runtime-evidence.json. Full integrated session acceptance remains pending.

Geometry4e90883 imports preserve its endpoint-parity/MSVC fixes and the terrain
owner's8m ravine crop/ordered mate together. The dense-QEM and cave interior
sampling reductions are disclosed above.

Supervisor allocation places geometry's canonical native DEFAULT ALL build
ahead of CORE. CORE follows its completion/source/log/artifact receipt, from
final frozen sources and parallel1. That initial allocation has now been consumed by the failed46702bcf5 attempt
documented below; a further allocation is pending.

The b05d60ba4 frozen-source Linux DEFAULT build completed with exit1 in1423.28s
(total includes the successful real Linux Box3d44.29s). All1216 captured source/
build identities remained unchanged. The actual Make archive failed because
the source search omitted src/terrain_collision and ar could not find its
objects. The Makefile now includes that actual directory; no producer is removed
or substituted. Full log:/tmp/steady-harbor-b05d60ba4-linux-default-build.log,
SHA256:d7871ad08fe70a8ed3b32cfe81c4811434c62eb83392b31c1b91f90dd78f4486.
Final integrated graph configures136 real tests, with no peer fixture omission.
This configuration is not yet a full linked-suite or successful build receipt.

The d75101260 frozen-source Linux DEFAULT build failed in151.10s at the next
archive member:gpu_visual_mesher_common.o. A census of every archive member
found exactly three missing objects, all with canonical sources in
src/render/gpu_meshing. Make's source search now includes that directory too;
the producer and archive membership remain unchanged. All1557 captured source
identities matched after the completed build. Log:
/tmp/steady-harbor-d75101260-linux-default-build.log,
SHA256:aa5f92a1f6dad99e42d8030a4c9b3d499a363f151b9c4e2edad69d34f08035f7.

The46702bcf5 actual Linux DEFAULT ALL run failed its mandatory OBJ golden
qualification after1676.29s. CastleStone and AlpineFlower texture hashes differ,
and AlpineFlower fails its original UV tolerance. The guards were neither
disabled nor updated. Full log:/tmp/steady-harbor-46702bcf5-linux-default-build.log,
SHA256:00e047b386f8eeb9509dc503de4070f144cc7ff26d0e1090a95ad5750c525a38.
The original pre-migration test TU from held-task base1f4ff089a, compiled with
the actual qualifier policy and linked against the actual current producer
archive/closure, reproduces all three identical failures (33.59s,exit1).
Control:/tmp/steady-harbor-obj-golden-migration-control/receipt.json;
runtime-log SHA256:1dd9dbecc8a5bed06b8227d8a57c43151e273416cf4d95dc9e219f0208fdc479.
Production exporter and MeshChartingLib have no changes against that base.
This isolates the test migration; it does not establish a complete original
production baseline or qualify the failing export. Investigation remains open.

CORE's authorized fresh canonical native DEFAULT RelWithDebInfo allocation at
46702bcf5 completed exit1 after1741.86s. The actual pre-start native process
census was empty; the running Ninja PID96788 was independently verified-j1.
Source HEAD and tracked status were clean and unchanged before/after. The
build failed the viewer matter_engine.cpp at7200:static build_weld_part called
the instance fixture_inputs without an object. No editor artifact was produced.
Complete source/start/end/artifact receipt:
/tmp/steady-harbor-46702bcf5-full-native-receipt.json. Full-log SHA256:
5a32a535d7cd636ccbdf5a3f0a787a54253279388fcdee3c697573dd27fa6c1c.
The private weld helper is now an instance member, preserving its session cache
scope and sole instance caller. The actual canonical TU with the full Vulkan
viewer branch passes GNU syntax-only checking (30.66s); this is not a corrected
native build receipt. Additional native allocation and full Linux/runtime
acceptance remain pending. Actual native/Linux CPU registrations both contain
the same136 test names, with no omission or extra test.


The final terrain-ownedf7384de4 collision source is now integrated exactly
(blob2d9c70b83b38f88454bc8984e5a0af723d6e2470); the registrar already matches
blob4694f63d7e77fba6e9c39be1ecd858ea66977e7a. The14th guard uses the real
production worker without a build callback, requiring nonempty encoded tile
and manifest bytes, Box3D installation, identical cold/warm collision identity,
and reuse of both immutable artifact handles. All13 existing lifecycle/race
groups remain. The owned payload adds no production TU or shared build change.
The read-lease adapter and fixture_io hashes exactly match the owner's
independently qualified delivery. Their actual Make build passes and focused
fixture_io/collision14/demand9 executions pass; the two-test CTest run takes0.99s.
Receipt:/tmp/eager-dune-79-retained-read-handoff.json. Those are peer-scoped
results, not full CORE Linux/native acceptance. OS persistence/IO-error coverage
and the earlier fixture reductions remain disclosed; no guard is relaxed here.

The integrated990c19653 source passed the actual canonical Linux collision
CMake target (real Make archive rebuilt,104.55s), then all14 groups in0.843s
from an empty working directory. Every runtime path call was reviewed: one
executable execve and six system-loader calls only; no fixture/source/asset/
cache/evidence IO. Tracked source remained clean and unchanged across the run.
Receipt:/tmp/steady-harbor-final-collision-check/receipt.json;
trace SHA256:f5eed25f12808cf99e6b15ff15da2d9d9994425629358d98769fd4536129a9b5.
This focused target result leaves the mandatory full Linux OBJ qualification
failure and corrected full native allocation/acceptance unresolved.


A complete original-producer GNU control now reproduces all three exact
failures. The held-task base1f4ff089a was exported to a private directory and
all7876 Git blobs verified before building. The original full Make engine
archive has215 members, exactly matching the current canonical inventory;
archive SHA256:dcf1b644d30e0a959a1cb32466cc09c3daf79f2f106684c218d223770f5ae544.
Original test, headers, world_demo, shared-lib and fixtures were used. The same
actual Linux Box3D/Ozz/Raylib binaries were held constant and their hashes
verified unchanged. Five isolated portability fixes were recorded: the two
Make source-search directories, Flecs POSIX feature macro, hydrology's local
final-to-final_status identifier rename, and a direct climits include for
INT_MAX. No exporter/chart/mesh/material logic, numeric flag or guard changed.
The build resumed after recorded portability failures; aggregate build time
was411.37s, not just the final99.87s completion step. Original test compile/link
passed3.67s; runtime failed25.18s. Its complete failure log is byte-identical to
the original-test/current-archive control, SHA256:
1dd9dbecc8a5bed06b8227d8a57c43151e273416cf4d95dc9e219f0208fdc479.
Receipt:/tmp/steady-harbor-original-production-control/receipt.json contains
source identities, build history, explicit exceptions, commands and artifacts.
These failures occur with the pre-migration production code plus the recorded
portability fixes in this GNU environment. Their numerical/compiler/platform
cause is still unisolated. Existing golden files and mandatory predicates stay
unchanged; this diagnostic neither qualifies the failing export nor replaces
full Linux/native acceptance. The consumed supervisor wait confirmed the
previous test-only control's limitation; no further native allocation has yet
been received.

The geometry owner's corrected read-only UV audit was independently checked
against the retained outputs. AlpineFlower vt16 (one-based) is expected
(0.367188,0.754078), actual (0.542969,0.754078). All228 emitted positions,
normals, face lines and V coordinates match;24 U components exceed the original
tolerance. The complete OBJ from the original-producer control is byte-identical
to the current-producer OBJ, SHA256:
621c88a8a837ac0088b158bb3a245a87131fce5215c83e20f3e2ec0c03e7b4d6.
Receipt:/tmp/steady-harbor-obj-corrected-coordinate-audit.json.
The current shelf_pack comparator orders only by chart height, without a
tie-break. Reciprocal horizontal placement changes are consistent with an
equal-height ordering difference, but exact chart heights and sorted placement
order have not been captured; the cause remains unproven. No source, golden,
tolerance or qualification predicate was changed by this read-only check.

The subsequently delivered supervisor order msg-88d02c49 allocates one corrected
canonical native DEFAULT RelWithDebInfo run at parallel1, following frontend's
actual configure-only completion. The frontend ON/OFF checks both passed; its
qualified viewer graph blobf0d72bd2 matches CORE exactly. Reuse of the retained
partial native tree requires actual compatibility and a fresh host census.
Before freezing, corrected CORE7b04a1a21 passed the three canonical CMake targets
fixture_io, collision and demand (actual Make archive,12.31s). Runtime fixture
I/O including concurrent retained reads passed0.020s; all14 collision groups
passed0.714s; all9 demand groups passed0.273s. Every traced file operation was
reviewed: executable/system loader only, plus demand's read-only kernel
overcommit setting. No fixture/source/asset/cache/evidence file access occurred.
Source remained clean and unchanged. Receipt:
/tmp/steady-harbor-corrected-focused-check/receipt.json. These scoped checks do
not qualify the failing full Linux build or the corrected native DEFAULT graph.

The original full producer control was reused for a private atlas diagnosis,
replacing exactly mesh_charting.o and mesh_export.o with actual original-source
logging, then a one-comparator candidate. All other213 archive members, the
original test object, fixtures and linked dependencies stayed byte-identical.
AlpineFlower chart2/7 have exactly equal raw height0x1.3ed4dp-3, and12/38 have
height0x1.db22d2p-4; their widths are near-equal, not identical. GNU height-only
sort puts7 before2 and38 before12, at final scale0x1.456d9cp+7. Their placements
are ox135/90,oy45 and ox157/139,oy144. The index tie-break reverses those pairs,
fixing the original24 U deltas, but introduces12 other U deltas at vt172-177
and196-201 (chart32/37, exactly equal height0x1.3ceb5cp-3). Both texture digest
failures remain; both original and candidate runs exit1 with three failures.
No candidate source or golden changes were integrated. This demonstrates the
reported pair permutation, not a complete explanation or qualified correction.
A cheap private actual-library42-rectangle equal-height order guard fails the
old comparator and passes the candidate (0.002/0.003s); adjacent unequal float
heights still sort tallest-first. Exact commands, hex traces, hashes and
untouched-golden output are in /tmp/steady-harbor-atlas-order-control/receipt.json,
affected-pair-analysis.json and tie_order_guard_receipt.json.

The common native and GNU registration now also includes the unchanged real
polygon_triangulate, mesh_indexed, mesh_transform, mesh_smooth and oriented_cube
test sources, linked to the actual common engine implementation archive. Their
implementation TUs are already in the engine/surface manifests; no additional
shared implementation copies were added to these executables. The compute OFF
and ON cases use separate builds of the actual GeometryCompute standalone
project, with its real ComputeDevice/Contracts dependencies. ON compiles the
actual Vulkan device, four real shaders and generated embedding, and uses the
MATTER_GEOMETRY_COMPUTE=1 macro on the library and test. OFF omits that macro and
backend by its actual canonical configuration. Both parent DEFAULT ALL targets
always configure/build the corresponding actual child DEFAULT ALL graph; the
source/generated-graph qualifier remains an unconditional ALL target and a
runtime-executable dependency. Both generated closure audits have unique
compiled source paths (OFF8, ON10), with no scene/host/terrain dependency.
The five cases and OFF are CPU; ON is GPU with resource lock matter_gpu.
The GNU inventory is now142 CPU cases plus this one compute GPU case.

The seven own-source GNU targets built in93.73s and both dependency qualifiers
passed. The first GPU runtime correctly failed because the required validation
layer was absent. An actual Ubuntu1.3.275.0-1 validation-layer package was then
extracted into an independent tool directory (package SHA256
3f5e9b53fbaabe32d770d3608912c04a33b8683385df9a5472fe062c16850746).
With the real layer, parallel CTest and file tracing passed all7 in10.64s.
All six CPU traces contain only executable/system loader accesses. GPU uses
actual Vulkan driver/layer configuration, shared libraries and device probes;
its source enables in-memory validation and disables disk shader caches. An
unchanged build of both compute graphs reruns both mandatory qualifiers in
1.06s. These are scoped costs, not full common build or runtime acceptance.
The GNU registrar accepts explicit actual layer/tool directories and records
them in registration metadata and the GPU test environment. Evidence is in
/tmp/steady-harbor-geometry-supplement-check/receipt.json, including actual
source/macro/target closures, binary hashes, ALL graphs, JUnit and file traces.
At that checkpoint the native common graph remained pending the host release
wait; the subsequent corrected native attempt is recorded below.
The final registration was also exercised with inherited VK_LAYER_PATH and
LD_LIBRARY_PATH removed: all7 passed under parallel CTest/file tracing in12.61s,
using only the GPU test's registered actual-tool environment. The same six CPU
traces remain loader-only; successful GPU opens are actual driver/layer/system
files, with no fixture/source/asset/cache/evidence opens or shader-cache writes.

After PR25 completed, the corrected canonical native DEFAULT ALL continuation
actually ran at frozen source ad635e0b8482c92842faafe127a0e66d4a83495f,
RelWithDebInfo, CMAKE_BUILD_PARALLEL_LEVEL=1, with no target override. Actual
Win32 prestart censuses were empty at 12:14:31.2528733Z and 12:15:58.5929225Z.
The original failed467 tree and all44 artifact hashes were preserved in
relwithdebinfo.failed-46702bcf5-evidence before compatible retained-tree reuse.
Source identities for7970 files and the Git index remained unchanged through
harvest. Start was2026-10-13T12:16:47.155375Z, configure passed at12:16:51.895434Z,
and the attempt ended at12:17:56.530919Z:69.331112s, exit1. Independent Win32
Ninja/parent-chain evidence confirmed the actual root serial build.

The headless engine TU compiled and its actual archive was updated, but the
viewer compilation of MatterEngine3/src/matter_engine.cpp failed with MSVC
C1128 (ordinary COFF section limit exceeded), after the previous C2352 fix.
The compiler requested /bigobj. The source-specific MSVC COMPILE_OPTIONS
property now appends that flag for this shared TU in both core/viewer object
targets; other sources and existing target assertion/definition policies are
unchanged. This new correction still requires actual native compilation.
No editor artifact, completed DEFAULT build, full CTest result or acceptance
is claimed. The granted corrected native allocation was consumed by this run.

Receipt: /tmp/steady-harbor-ad635e0b8-after-pr25-corrected-native-receipt.json.
Full log SHA256:
1a3e7bbde0016386a9ae34402b1786928f940dcb10fb45af00ce8d69551244f0.
Build log SHA256:
b91269e73d4718b86b87140abea685c58cb11da2e9975c43f80fcf36a06aec34.
Independent process proof SHA256:
2f2b0ed4258c30a75a5dee1a371cf4786b6a40e16287c219f6929504c1e47741.
The receipt records tool identities, all44 resulting partial artifact hashes,
original failed-tree provenance and complete source-freeze verification.

### Owned golden reference and portable chart packing

The retained ad635 native log records the unchanged OBJ golden qualifier
passing at step26 before the later C1128 failure. Its surviving exports were
preserved with timestamps inside that run. Independent PNG CRC/filter decoding
recovered all15 raw maps; their framed dimensions/pixels hashes match every
checked-in golden. This qualifies the reference producer's golden outputs,
not the failed DEFAULT build or the current source.

Reference receipt: /tmp/steady-harbor-ad635-native-golden-reference/receipt.json,
SHA256 e909dbc152b2b88d11564380d7e47a5aac3db1bb486f1d7ffc79f27100281f6a.
The original/current GNU boundary controls remain sufficient and were not
repeated. Comparison against the owned native outputs recovered every chart
placement. The pinned MSVC median-partition/insertion algorithm, applied to
the actual GNU heights and deterministic triangle-construction input order,
reproduces all223 CastleStone,42 AlpineFlower and6 CastlePavingSlab placements.
This explains the previously unsupported equal-height permutations without
an asset-specific or chart-index comparator.

The texel packer now fixes that ordering algorithm in an internal header,
with a deterministic heap fallback and a leaf check before index doubling.
Microsoft STL attribution, pinned compiler-header identity and its Apache2.0
with LLVM exception license are retained. Generic RAM guards cover empty,
single, ordered, reversed, equal and repeated keys, the31/32/33 and40/41/42
thresholds, repeatability, descending heights and permutation preservation.
All156 cases, including48 forced heap cases, and the prior charting guards
passed with address/undefined-behavior sanitizers before source integration.

One private real-producer candidate run retained the original three mandatory
part cases and DSL predicates:35.310s, exit1, one CastleStone normal-digest
failure. Its14 other raw maps match the owned native reference byte for byte,
including every AlpineFlower predicate and CastleStone albedo/roughness/UVs.
The remaining91 normal bytes on Stone charts0/26/7 differ only as127 versus128;
30 exported normal lines also differ in their last decimal. Neither observation
justifies a quantization change or tolerance. Fixtures, all original failures
and both failed native artifact sets remain preserved.

Candidate receipt: /tmp/steady-harbor-portable-pack-order-candidate/receipt.json,
SHA256 7f56e5d11a913abb392681ec79448e7847497f1ab6717776cd96df9e04ce56fa.
The integrated source still requires its owned mandatory qualification and
the remaining normal repair. No full Linux/native result or acceptance is
claimed. The existing native continuation must bind to the final corrected
source rather than launch the old ed1 freeze.

### Epoch 9 Linux continuation

The supervisor-delivered startup packet resumed actual owned HEAD
7badf2b24d25e988ee7e55caa3e6be9442b71e10. The older temporary evidence trees
are absent on this host; their historical receipts above are not claimed as
fresh results. A new complete registrar graph contains143 tests (131 core,
10 editor, two compute), with142 CPU tests. Actual Ozz, Box3D and Raylib
Linux prerequisites built; a private packaged glslc and Vulkan validation
layer supply the missing host tools. The serial DEFAULT build of the real
Make headless archive and generated CMake qualification graph is ongoing.

All10 editor tests pass at-j2 in0.05s. A separate file-syscall trace of their
actual processes shows only executable/dynamic-loader accesses, with no
fixture/source/asset/cache/evidence IO. Trace SHA256:
e0919415e5ae1c3ec17512be559ed1d2fc068b715aff74163e91b3b557efb20c.
The chart sort suite passes156 generic cases and48 forced heap cases.

The stationary setup change above passes the complete existing standalone
sector suite. Same-host, same-O2-flags runs take50.20s before and28.48s after;
their printed regression results are identical. The ordinary unoptimized
Make baseline takes288.65s. These measurements ran alongside the serial
archive build and are not whole-suite acceptance or before/after slowest20.
Logs and timing files: /tmp/steady-harbor-epoch9-sector-o2-{before,after}.*.
The complete historical qualification variant passes in98.56s, including
the original uniform publication drain and all five4000-update setup soaks.
Log: /tmp/steady-harbor-epoch9-sector-o2-qualification.log.

An owned-source Windows export at C:/tmp/steady-harbor-62-e9-native-0z97ad7s
matches all10650 regular blobs of26d3e93c and the one tracked symlink target.
Native Git verification used bounded batches, avoiding a stalled large
interop pipe. Receipt: /tmp/steady-harbor-epoch9-native-source.json, SHA256
ab5f4a66d3eb32797d2d01ac4249313808f0a5a5ef76d26dee7bc657f853d3d9.
No native build has launched. The supervisor confirmed CI producer first,
clear-stone-99 second, then this task, with explicit handoff and a fresh
launch-time resource check. This source export must receive subsequent
owned changes and be reverified before its build.

The real Linux archive completed. The first serial DEFAULT build failed at
step351/409 on the mandatory OBJ golden qualifier: CastleStone expected
normal digest281757a9a9886a07, actuala9b38f2de4de4fe2, with unchanged resolved
input hash. CastlePavingSlab, AlpineFlower and the DSL checks passed. Original
goldens, tolerances and normal-production code are unchanged. All46 produced
files and the failed build log are preserved at
/tmp/steady-harbor-epoch9-golden-failure. Log SHA256:
14a0a79ae7275246f95e5d9088142aa6d199c022e37eac82931121d29f2da01b.
Independent PNG CRC/filter decoding gives998 pixels(127,128,255),46 pixels
(128,127,255), and64492 pixels(128,128,255) in the256x256 RGB normal map.
This supports further numeric diagnosis; it does not waive the failed guard.

A keep-going continuation collects the remaining independent qualifications.
It exposed a river-float test abort: its intentional absent measurement-hook
lookups return entity0, while Flecs ecs_is_alive requires nonzero input.
An external abort trace identifies ecs_is_alive called by the warmed
allocation test. The test now checks the sentinel before querying liveness;
both absence predicates and the real-system presence/liveness guard remain.
The exact Release-flags candidate qualifier passes in0.81s, including phase
order/replay/publication checks and all1000 measured phases with zero
C++/Flecs/Box3D allocations. No production or regression coverage is removed.
Trace: /tmp/steady-harbor-epoch9-river-abort.log. Candidate source/compile/link
commands and result: /tmp/steady-harbor-epoch9-river-fix/.
The corrected actual CMake target subsequently builds and passes its mandatory
qualification; log: /tmp/steady-harbor-epoch9-river-cmake-build.log.

The keep-going DEFAULT continuation completed with three failures: the unchanged
CastleStone normal golden, the now-corrected river sentinel precondition, and
the world qualifier's lexical project-cache alias predicate. The actual Demo
viewer and the other independent qualifications passed, including both real
compute-closure sub-builds. Continuation log SHA256:
0157f6233ac162343743012cd1d33ba4edfe40a9ad9846af13e4ac2b07bb797f.
This remains a failed DEFAULT build, not whole-suite acceptance.

The world failure is a production cache-identity normalization defect. GNU
weakly_canonical(existing/project/unused/..) returns existing/project/ with an
empty filename; hashing that spelling differs from existing/project. The cache
helper strips the trailing empty filename after canonicalization, preserving
roots and symlink traversal. Source project paths retain their original spelling.
The original filesystem alias predicate remains; RAM runtime guards additionally
cover the alias, explicit trailing separator, and filesystem root. Private
actual-helper checks pass with both lexical-only and filesystem normalization:
/tmp/steady-harbor-epoch9-cache-alias/helper-receipt.json and helper.log.
The actual world CMake target passes its mandatory full qualification, including
all72 shipped worlds and the original alias predicate, and builds the runtime
executable. Log: /tmp/steady-harbor-epoch9-world-cmake-build.log, SHA256
af183e6c37075c33d89e2057c44e800c6282ee75b6644657c0ee445360442029.

Private CastleStone diagnosis captures the actual model and options entering
the production texture baker. The captured input SHA256 is
2c4395dc2787704943a2c9a13c57af954dba269411858a2f8dcede21924a205c.
An owned replay calls the real production baker and hash implementation. GNU
baseline, -ffp-contract=off, -fno-tree-vectorize and -O0 variants produce identical
bytes for every map and the same failed normal digest. Private instrumentation
records actual triangle, barycentric, shading-normal, face/basis, projected,
unorm and unclamped scaled-plus-half values as hexadecimal floats. Instrumented
maps remain byte-identical to baseline. Trace receipt SHA256:
e98c2d06c483518783f647c398688ca6bb94149a25e2906fc7df679b653dc0bb.
Artifacts: /tmp/steady-harbor-epoch9-normal-stage/. No production normal arithmetic,
goldens or tolerances changed. The GNU controls do not establish a cross-compiler
cause; actual native reference inputs/stages remain necessary.

The first complete CPU diagnostic registration contains142 tests. With two
CTest jobs,138 passed, three failed and one was not run; CTest wall time was
336.70s. The external timing command reported345.24s. The baseline substituted
only the original sector-streamer setup binary; other binaries came from the
actual CMake build, which remains blocked by the OBJ qualifier. This diagnostic
is not a passing baseline or final-source acceptance. Logs and JUnit:
/tmp/steady-harbor-epoch9-cpu-before.{log,time,xml}; original sector source and
compile/link receipt: /tmp/steady-harbor-epoch9-timing-before/source.json.

| Slowest CPU diagnostic tests before | Seconds |
| --- | ---: |
| part_flatten_tests | 326.251 |
| event_hub_tests | 41.377 |
| sector_streamer_tests | 24.849 |
| chart_atlas_tests | 4.642 |
| script_host_tests | 2.927 |
| conifer_lod_provider_tests | 2.850 |
| surface_proxy_tests | 2.368 |
| async_bake_tests (failed) | 1.785 |
| finite_surface_recipe_tests | 1.643 |
| gi_bake_tests | 1.364 |
| terrain_mesher_tests | 1.100 |
| terrain_collision_session_tests | 0.907 |
| sector_bake_tests | 0.877 |
| physics_tests | 0.756 |
| brick_bond_atlas_tests | 0.747 |
| river_float_system_tests | 0.656 |
| tileset_bake_tests | 0.631 |
| character_controller_tests | 0.581 |
| sdf_candidate_bounds_tests | 0.487 |
| ecs_tests | 0.413 |

Failures exposed three generated-fixture setup problems: the async barrier
waited for a publication hook before pumping the preceding app cleanup barrier;
the tiny viewer Tree lacked the original seed42 default and its gallery was
inlined rather than expanded; the RAM AuthoredCurve world lacked the actual
river_curve shared module. The OBJ runtime binary was not built because its
mandatory qualifier failed. The async proof now blocks the actual worker's
immediate BakeStarted subscriber after the original pre-pump checks. The viewer
fixture supplies seed42 and explicitly expands its gallery, preserving every
original flat-root/child/hash/fetch assertion. The exact module attachment and
the proposed dense-flattening qualification are pending peer CMake coordination.

Private candidates use the actual CMake Release compile/link commands and the
production archive. Viewer passes in0.165s, and the revised async barrier passes
in0.867s. Their source hashes and commands are in
/tmp/steady-harbor-epoch9-runtime-candidates/{receipt.json,async-event-pump-receipt.json}.
Earlier failed candidates are retained there. One earlier async run additionally
observed an open publish span after BakeFinished; the final successful run does
not establish that this existing snapshot race is resolved.
The revised async source also passes the actual CMake target's full mandatory
source qualification and its registered runtime test. Build log SHA256:
7b75b6226b03e8b2104b9a9f75faff6a3135f4b6b413420bcf6810cfdaa6f1fe.
Registered runtime log SHA256:
5c4e804529c1ff21ae82134d2edf7862d8f8879e7620034d7a31cb5c1ac48e5e.

Private timing instrumentation around the original flattening main preserves
its production algorithms and assertions: all groups passed, with92.256s in
test_flatten_clustered_v3 and190.338s in test_flatten_retain_budget_identical.
Receipt and complete log: /tmp/steady-harbor-epoch9-flatten-profile/.
The pending runtime candidate uses400 triangles and a160-triangle cluster
target over the same200x100 extent for those two groups, retaining codec,
conservation and every-byte comparison predicates; its private run passes in
10.358s. Original direct40k splitter and determinism guards remain runtime.
The full40k/16000 integration workloads remain in the qualification macro
branch, but its mandatory build attachment is not yet established. These
private results do not establish a passing DEFAULT build, complete suite,
runtime filesystem trace or native result.

Supervisor message msg-c008c1519fe049fb8b81f34fec9b6130 confirms Jack's
operative global directive msg-cd61d32c8a4145639a78966a94924d2d: Windows CI
is not required for current Matter completion; Linux builds and tests must pass.
This supersedes the earlier native completion/resource wait. No additional
native build is launched, and the failed GNU golden remains a required repair.
Existing native artifacts are diagnostic references only.

Peer reply msg-c47db26f2b534a728bba2fd5bebfda32 confirms no overlapping edit in
MatterEngine.cmake, the Linux registrar or tests/Makefile. The existing exact
shared-module generation rule is now attached to world_definition_tests in
CMake, the actual Linux registrar and Make; the AuthoredCurve RAM fixture
selects the embedded repository river_curve.js bytes. Its private actual
Release runtime passes in0.115s. The original filesystem qualification branch
is unchanged. Dense flattening is now attached to the mandatory CMake ALL
qualification and the Make runtime target's order-only qualification prerequisite;
the updated Linux DEFAULT build is in progress. No qualification pass is
inferred from the attachment itself.

The already completed native CI run38514005777 at
2fb388d7739fc38437cfdbc1e14230409c8914ad supplies a concrete old-source pass
reference: original obj_export_golden_tests passed all three parts and the DSL
checks in27.6812s. The run failed only shader_source_tests. Downloaded actual
artifact: /tmp/steady-harbor-epoch9-existing-ci-artifact/ctest.xml, SHA256
84a4e75e6c4885187aaff8eb0f8d0fe700935bac67ef6c9c7a5ddaa7af8720f9.
It contains no model or texture bytes, so it does not establish the first
cross-compiler divergence. A further private control compiles the unchanged
production texture source with Clang and replays the same captured GNU input:
all maps remain byte-identical to the GCC control, including the failed normal
digest. Receipt: /tmp/steady-harbor-epoch9-normal-stage/clang-receipt.json.

The actual mandatory part_flatten_tests_filesystem_qualification passes the
complete original workload in357.174s (Ninja command timing), including the
four-cluster40000-triangle census and both dense retention modes. This source
bound result is in /tmp/steady-harbor-epoch9-linux-default-v2.log; the DEFAULT
build itself remains failed on the unchanged normal golden and is continuing
through independent qualifications. Make's dry-run confirms the qualifier
compile/link/run precedes its runtime target; that is a graph check, not a
second qualification execution.

World and flattening private runtime traces contain no fixture data IO.
World trace SHA256:
17667dac3451f20d4b6387ae5fc8ae10f8e11c7c0877020dce3ae21f1e734700.
Flatten trace SHA256:
203d454e993bbaca642dc251097761e0d9848ce0e9f14e19740a88e884f7d613.
Its two actual child processes still compare serialized artifacts with separate
RAM stores; their shell launches stat the inherited working directory, while
fixture paths occur only in argv. Review receipt:
/tmp/steady-harbor-epoch9-runtime-candidates/world-flatten-trace-review.json.

The updated Linux DEFAULT v2 build completed with only the retained CastleStone
normal-map golden failure. All independent qualifications passed, including the
original dense flattening, shipped worlds, viewer Demo and compute ON/OFF
closures. Complete build log SHA256:
b1ee95340b0cb9ae927e2f39d6caae788f74f8b3aed5140e1ef1568b7b4eba9a.

The actual updated registered CPU run took40.86s (41.77s process wall time),
with141 passes and the OBJ golden runtime not built because its mandatory
qualification failed. This is a diagnostic comparison, not full acceptance.
The before run took336.70s with138 passes, three failures and one not-run test.
After JUnit SHA256:
7c00371e29b3ceff9065e5ced0155b125bfe8ee8ee85faecf77942eb08ffe811.
The slowest20 updated tests were:

| Test | Seconds |
|---|---:|
| event_hub_tests | 36.291 |
| sector_streamer_tests | 9.566 |
| part_flatten_tests | 6.626 |
| chart_atlas_tests | 3.460 |
| script_host_tests | 2.745 |
| conifer_lod_provider_tests | 2.068 |
| surface_proxy_tests | 1.703 |
| gi_bake_tests | 1.015 |
| sector_bake_tests | 1.006 |
| finite_surface_recipe_tests | 0.987 |
| terrain_mesher_tests | 0.814 |
| async_bake_tests | 0.715 |
| sdf_candidate_bounds_tests | 0.652 |
| brick_bond_atlas_tests | 0.648 |
| tileset_bake_tests | 0.566 |
| river_float_system_tests | 0.548 |
| terrain_collision_session_tests | 0.547 |
| physics_tests | 0.495 |
| ecs_tests | 0.465 |
| character_controller_tests | 0.424 |

The separate whole-process syscall trace repeated141 passes and one not-run
test in51.74s. It exposed remaining filesystem accesses, so it is not an
IO-free suite receipt: the core BLAS queue wrapper created a BlobStore and the
singleton ANLK rejection probed a missing animation commit file. Trace SHA256:
17eb73da8f7fb7b33ec0332f9832bbeb27bf9d048c87595c66a465210adaca51.
The reviewed residual calls and explicit failure disposition are in
/tmp/steady-harbor-epoch9-cpu-after-trace-review.json. Loader calls, CTest's
own log/graph IO, the compute dependency checker, shell cwd metadata, Linux
allocator overcommit metadata and CPU-online queries are separate from fixture
data IO.

The BLAS queue runtime now uses the existing disabled initial worker directory
for the same32-result backpressure, poll/ticket, cancellation and completed-miss
predicates. It gives up opening a native BlobStore during runtime; the original
canonical MatterGraphics disk test is unchanged and passes the actual mandatory
CMake qualification. Its core runtime target depends on that qualification.
Animation manifest reads now honor the explicitly selected bundle byte storage
before the original native FILE path. This preserves the singleton test's exact
ANLK missing-manifest rejection and parser predicates in owned RAM. The native
FILE path and publication protocol remain unchanged. These final trace fixes
still require their source-bound build/runtime verification; neither the earlier
40.86s timing nor the earlier trace establishes acceptance of the new reader.

Source-bound verification of those final fixes passes the original mandatory
PartStore and BLAS queue disk qualifications, then all three registered runtime
tests (singleton, PartStore and queue) in0.12s with no non-loader filesystem
calls. Build log SHA256:
c1f7cddbd3bcefd2823e5ff41939f6ebc3fe8f4ffabcd11ff936032528d625ca.
Scoped trace SHA256:
fc85afe10f558e4b320dc76bde52616dcc27126d20148584ed48b4f984353d27.
Source hashes and reviewed scope:
/tmp/steady-harbor-epoch9-fixture-tail-receipt.json. The full suite still needs
an exact-source DEFAULT build and runtime run after the remaining normal golden
repair; this scoped result does not replace that acceptance gate.
