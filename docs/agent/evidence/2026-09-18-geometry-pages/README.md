# Binary pages and CPU geometry hierarchy checkpoint

Date: 2026-09-18. Implementation checkpoint, **not production geometry acceptance**.

## Implemented

- AssetStoreLib now exposes versioned binary page directories and borrowed section
  views, bounded coalesced reads, retained immutable allocations, and bounded bulk
  writes. Repeated resident reads avoid disk access and checksumming. Allocation
  accounting includes padding, over-read and external pins.
- Manifest publication orders durable pack/index publication before semantic
  references. Index validation rejects invalid extents and overlapping records.
  Corrupted content can be explicitly regenerated under the same content ID.
- Shared reader leases prevent compaction from deleting packs still reachable by
  lazy readers. Page-aware maintenance follows transitive dependencies, including
  explicitly retained revisions, and fails closed on missing/corrupt survivors.
- A generic indexed-mesh compiler partitions connected attribute charts, merges
  adjacent groups, simplifies interior connections, propagates conservative
  geometric error, and persists independently readable geometry nodes.
- A CPU reference selector chooses a complete cut of the hierarchy. Missing
  children, request limits and work limits retain parent coverage. Selected page
  handles keep their payloads alive.
- The existing simplifier has an opt-in border-connectivity preservation mode.
  Default behavior for existing consumers is unchanged.

Implementation: [AssetStoreLib](../../../../libs/AssetStoreLib/README.md),
[geometry contract](../../../../MatterEngine3/src/geometry/geometry_hierarchy.h),
[geometry tests](../../../../MatterEngine3/tests/geometry_hierarchy_tests.cpp).

## Validation

Native Windows MSVC RelWithDebInfo:

- [Storage suite](native-storage-tests.log): **562 checks, 0 failures**.

- [Focused CTest run](native-gates.log): all four suites passed (asset storage,
  hierarchy, existing mesh simplifier plus border regression, viewer build graph).
- [Unique 128 × 128 grid](native-geometry-128.log): **32,768 source triangles →
  512 coarse triangles**, 2,047 hierarchy nodes; all 512 outer boundary edges
  retained. Compiler time was **2,448.671 ms** in this run. Root accumulated
  conservative geometric error was **0.434243 asset-local metres**. This is a
  deliberately coarse root; the selector must refine according to the accepted
  error criterion. Reduction ratio alone does not establish visual quality.
- Hierarchy tests exhaust small-fixture availability masks, check exact source
  coverage and selected-mesh border topology, test nonplanar surfaces, undercuts,
  disconnected detail and material boundaries, enforce affine UV interpolation
  on a planar fixture, and exercise corrupt pages and tiny-cache fallback.
- Storage tests cover crash recovery, concurrent readers, bounded reads/writes,
  malformed indexes/pages, corruption repair, publication failures, cancellation,
  pinned buffers and quiescent transitive compaction. A deterministic interleaving
  also verifies that a commit arriving during an index read remains discoverable.
- [Linux storage suite](linux-storage-tests.log): **562 checks, 0 failures**,
  additionally exercising POSIX leases. This supplements native validation.

The [final editor build](native-editor-build.log) completed successfully. No editor scene was launched
or captured for this checkpoint, and no GPU/RT performance claim is made.

## Synthetic page measurements

[Raw CSV](native-page-benchmark.csv), 32 MiB payload corpus, 100 retained-read
passes. New writes make the filesystem cache warm: these are **not cold SSD
measurements**. First page-cache reads still perform checksum validation.
Retained reads return existing immutable page handles; they do not rescan payload.

| Page size | Pages | Write ms | First cache read ms | Retained pass ms | Read calls | Extra retained reads / checksums |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 KiB | 512 | 52.550 | 26.762 | 0.012 | 16 | 0 / 0 |
| 256 KiB | 128 | 50.929 | 26.829 | 0.004 | 16 | 0 / 0 |
| 1 MiB | 32 | 54.326 | 27.706 | 0.002 | 16 | 0 / 0 |

These numbers establish functional batching and retained hits, not a speedup
against the current world cache. There is no equal-integrity legacy-cache
comparison or actual streaming trace yet. Payload residency is bounded; index,
directory and container memory are not included in that payload counter.

## Remaining acceptance work

The editor still uses its existing production geometry path. The following are
**not implemented by this checkpoint**:

1. World/PartStore ownership adapters, asynchronous page demand, generation-safe
   publication, retries and coordinated global budgets.
2. GPU hierarchy traversal, indirect draws, upload residency and matching RT
   resource publication/retirement. The CPU selector is a reference only.
3. Parent reclustering and a fully attribute-aware simplification metric. Current
   group pairing is conservative; attribute seams may limit reduction. The
   compiler retains the source/hierarchy in memory under explicit count budgets;
   it is not an external-memory baker for arbitrarily large sources.
4. Displacement/debris producers, material/VT integration, terrain transitions,
   imported-mesh visual references and real StreamMountain conversion.
5. Native visual and RT/GI comparison, actual cold/warm streaming, camera
   teleport, edits, memory-pressure and source-complexity performance sweeps.

No P0–P6 milestone or full design acceptance is marked complete. Continue using
[the implementation plan](../../../superpowers/plans/2026-09-18-virtualized-procedural-geometry.md)
and [storage design](../../../superpowers/specs/2026-09-18-binary-asset-page-cache-design.md).

## Reproduce

From the repository root in WSL, with no editor running during its rebuild:

```sh
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_asset_store_tests
./tools/build-windows-from-wsl.sh RelWithDebInfo geometry_hierarchy_tests
./tools/build-windows-from-wsl.sh RelWithDebInfo mesh_simplifier_tests
./tools/build-windows-from-wsl.sh RelWithDebInfo editor
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/matter_asset_store_tests.exe
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/geometry_hierarchy_tests.exe 128
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/mesh_simplifier_tests.exe
MatterEditor/build/cmake/windows-msvc/relwithdebinfo/matter_asset_store_tests.exe --page-bench
```

For the four-suite CTest run, use the configured native CTest executable from the
MSVC developer environment, with `--test-dir
MatterEditor/build/cmake/windows-msvc/relwithdebinfo` and
`-R '^(matter_asset_store_tests|geometry_hierarchy_tests|mesh_simplifier_tests|viewer_graph_tests)$'
--output-on-failure`. The graph gate invokes isolated CMake configure fixtures.

[Source hashes](source-sha256.txt) identify the tested component files. The shared
working tree contains unrelated pre-existing changes; no commit or reset was
performed.
