# Full castle startup investigation — 2026-09-11

Scene: `CastleUpgraded`, 267 roots, 84 unique finished Parts, 3,892 entities,
136 local lights, one brick atlas from eight transient source variants.
Native MSVC RelWithDebInfo, RTX 4090, visible editor, SSD artifact cache at
`C:/tmp/castle-upgraded-cache`; source project remains on D:.

## Established causes and changes

- **Repeated structural planning on cold bake.** Each structural/connector/paving
  Part rebuilt the complete layout before selecting its record. The world now
  supplies versioned, bounded, URI-encoded construction records. Part wrappers
  validate identity/geometry and emit the same geometry directly. URI encoding
  avoids the existing native parameter JSON string escaping limitation. The
  1,031,712-vertex main structure stream is unchanged. Full world JSON grows
  from 4.83 to 9.92 MB; Node world construction/serialization adds about 46 ms.
- **World evaluation on a resolve-cache hit.** The measured unchanged reload
  still spent 9,356 ms in `provider.world-evaluate-validate`. A versioned
  authored-definition snapshot now restores the entire static WorldDefinition
  and ordered MaterialDef registry. Live tileset slots are never persisted;
  detail textures rebind normally. Checksum, material schema, size/count caps,
  finite values, enum bounds and exact handle replay reject invalid snapshots.
  Field/hydrology/river/terrain-collision worlds retain JS evaluation.
- **Redundant CPU BVH rebuild.** Eligible singleton FLAT meshes now adopt their
  saved acceleration structure after checking exact post-chart triangle order.
  Newly charted attributes remain authoritative for raster and local meshes.
- **Repeated whole-bundle reads for tiny sections.** Canonical admission,
  render policy and FLAT decode share a scoped, fully verified bundle snapshot.
  Writers bypass it; the scope closes before final live-file revalidation.
  Policy reading fell from 3,677 ms to 3.3 ms, aggregate FLAT decode from
  4,169 ms to 901 ms. These are observational phase measurements from different
  loads, not independent additive speedups.
- **Readiness logging could split inside a word.** BakeFinished now uses the
  common engine logger so concurrent detail publication cannot corrupt the
  automation barrier's line.

## Observed timings

Engine intervals include full publication and texture binding. Process-ready
adds editor bootstrap; first screenshot additionally includes deliberate settling.
The older accepted warm run enabled Vulkan validation; the diagnostic runs below
left validation at its default. Window size was not pinned in the exploratory
runs, so GPU/frame-dependent time is not a controlled comparison.

| Run | Install / restore before publish | Publish | Engine total | Process to ready |
| --- | ---: | ---: | ---: | ---: |
| Prior full rebuild, 82 rebakes / 2 hits | 159.6 s | 24.5 s | 184.1 s | — |
| Construction payloads, 82 rebakes / 2 hits | 75.7 s | 32.9 s | 108.6 s | 110.0 s |
| Prior accepted warm, 84 hits / 0 bakes | 10.4 s | 20.3 s | 30.6 s | 31.5 s |
| New resolve-format seed, existing meshes | 28.5 s | 20.2 s | 48.7 s | 50.1 s |
| Authored snapshot + verified bundle reuse, warm | 0.064 s | 15.3 s | 15.35 s | 16.39 s |

The seed run evaluates the world and resolves the graph once to write the new
format; it bakes no meshes. Its census counts the install and demand checks
(168 hits), not 168 unique Parts. The optimized warm run restores all 84 meshes,
no geometry bakes, and completes paired raster/RT screenshots with normal exit.

The measured pipeline creation calls sum to roughly 50 ms in the instrumented
cold run. They do not explain its startup delay. An earlier uninstrumented
warm run had a much longer publish pause; its cause remains unproven.

## Remaining measured work

First optimized warm run:

- Reference discovery fully decoded every FLAT mesh merely to inspect references:
  4.20 s on the worker, although this castle's finished Parts have no references.
- PartStore initial canonical snapshot: 3.00 s; final revalidation: 2.29 s.
- FLAT decode: 0.90 s; UV charting: 1.59 s; raster mesh creation: 2.42 s;
  BLAS adoption: 0.14 s.
- Detail atlas binding: 0.62 s.
- Verified bundle bytes across 84 parts: 776,297,257 (~740 MiB). These files retain
  both canonical and flattened representations; runtime geometry is ~1.70M
  unique triangles. A compact runtime-ready representation is the next larger
  opportunity after redundant decoding.

Worker reference scans and GPU-thread publication overlap. Do not sum these
figures as a serial critical path. Subsecond startup is not achieved.

## Validation and limitations

Serializer and provider integration tests cover complete roundtrip, material
optics/flag32/detail descriptors, exact handles, source-key invalidation, corrupt
snapshot JS fallback, throwing-source bypass, props, partial registry rollback,
truncation/schema/count limits. PartStore tests cover prebuilt BVH geometry and
attributes, chart UVs, dedup/refcounts, scoped bundle generations and corruption.

One exploratory warm reload failed with `VK_ERROR_DEVICE_LOST` during partial
RT publication. The fault report had no fresh address diagnostics; historical
addresses in its cumulative file belong to an older failure. Two subsequent
visible runs completed raster and RT captures. The intermittent fault is not
claimed fixed by the startup optimizations.

Evidence: `C:/tmp/castle-startup-full/`, with separate run folders containing
capture receipts, timestamped logs, screenshots and JSON bake traces. Prior
accepted captures: `C:/tmp/castle-upgraded-captures/final/`.

## Final native validation

Final production code also uses `load_flat_instance_refs`: it shares the
validated common/trailer grammar with the complete reader and checks BVH/index
arrays in place without allocating geometry or constructing BLAS/TLAS.
Complete FLAT file reads and checksums remain.

At a fixed 1280×720 with Vulkan validation enabled, the final full ten-capture run
loaded in 14.751 s (14.659 s publication),16.493 s process-to-ready. Reference
scanning measured 3.305 s. All ten raster/RT images were reviewed; geometry,
parallax masonry, gold, glass, furnishings and interior lighting remain present.
The run exited 0 with zero bake errors and no validation errors.

Seven native suites pass: PartStore, resolve cache (213/213), bake trace,
authored world serializer, authored world provider, Part v2 and FLAT reference
reader. The reference tests cover checksum/identity/count/BVH/index failures,
truncated mandatory trailers, emitter compatibility, output preservation and
unchanged dynamic materials. Expected file-write rejection logs belong to
explicit atomic-publication failure tests. Three focused Node suites pass
(construction payloads, connector/paving records, full upgraded scene); the
full-scene suite requires `--experimental-vm-modules`.

Reviewed screenshot gallery:
`D:/tmp/Castle Screenshots/2026-09-11 Startup Optimized Castle/gallery.html`.
Exact screenshots and receipts: `C:/tmp/castle-startup-full/validated-final/`.

An unchanged repeat with the same final binary, source, cache, 1280×720 window
and validation settings measured 14.606 s engine time (14.534 s publication),
15.416 s process-to-ready. No CPU test suite ran alongside this repeat. It
exited normally after paired raster/RT captures, with zero bake or validation
errors. Evidence: `C:/tmp/castle-startup-full/validated-repeat/`.

The next implementation priority is a compact runtime cache containing prepared
chart/raster streams and reference metadata, so loading avoids repeating mesh
preparation and reading duplicate canonical/flattened geometry. Preserve source
content invalidation and atomic publication; verify runtime bytes once when
opening a generation. GPU uploads and hardware acceleration construction must
be measured independently. This is a roadmap item, not implemented here.
