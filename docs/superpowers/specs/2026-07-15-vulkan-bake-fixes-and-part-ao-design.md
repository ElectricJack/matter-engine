# Vulkan bake regressions + baked part AO — design

Date: 2026-07-15
Branch: feature/rt-lighting-phase2
Status: approved by Jack (sections 1-5 approved in conversation)

## Background

After the OpenGL → Vulkan+RTX port, world bake times regressed badly and all
baked ambient occlusion disappeared. Root-cause investigation found:

1. **Cache-hit checks read entire artifacts.** `HostBaker::cached()`
   (`part_graph.cpp:562-575`) used to read 16 header bytes; the branch replaced
   it with `is_cache_artifact_compatible()` (`part_asset_v2.cpp:316`), which
   reads the whole `.part`/flat file and FNV-hashes it byte-by-byte. It is also
   called in `local_provider.cpp` at `ensure_part_flattened()` (:502),
   `artifact_root()` (:523 — full-file hash just to pick a directory), and the
   `compose_world` pre-load check (:896 — immediately before `load_flat_v3`
   reads the same file again). On /mnt/d (9p) this dominates warm bakes/loads.
2. **`kFormatVersionFlat` 6 → 7** invalidated every flat artifact (expected,
   one-time; not addressed by this work).
3. **Flatten double-streams clusters.** `flatten_part_impl`
   (`part_flatten.cpp:1361-1381`) materializes every cluster once to collect
   canonical boundary normals, then again for the QEM ladder.
4. **Baked AO was the SH-L1 probe-volume lighting**, sampled only by the GL
   `raster.fs` (`useProbes`). The Vulkan renderer has zero probe references, so
   the probe bake (`local_provider.cpp:705`) is pure wasted bake time and its
   output is invisible. Per-vertex AO (`TriEx.ao0/1/2`) is plumbed end-to-end
   through the Vulkan path (`vertex.surface.z` → G-buffer `orm.z` →
   `rt_lighting.rgen:262` GI multiply and `composite.frag:64` ambient) but the
   engine has always written `ao = 1.0` ("AO is not baked in SP-2",
   `script_host.cpp:871`), so no occlusion signal reaches the screen.

Decisions: Vulkan is permanent (probe system deleted, not ported). AO comes
back as **part-local self-occlusion baked into `.part` artifacts** with an
adaptive, schema-defined quality knob.

## 1. Cache-check fix: cheap identity probe + validate-while-loading

Two validation tiers replace the single full-file probe:

- **New `is_cache_artifact_header_compatible(path, expected_hash,
  expected_version, stats*)`** in `part_asset_v2`. Reads the 40-byte header
  plus the material-table prefix (`2*u32 + MaterialRegistryCount() *
  sizeof(MaterialDef)`, which sits at the start of the body). Validates magic,
  format version, resolved hash, struct sizes, material schema version, count,
  and byte-exact material table. Does NOT hash the body.
- **Loaders validate content while reading.** `load_v2` and `load_flat_v3`
  compute the FNV-1a content hash over body bytes as they read and reject on
  mismatch (fail-closed corruption detection preserved with zero extra I/O).
- Call sites switch to the header probe: `HostBaker::cached()`,
  `LocalProvider::artifact_root()`, `LocalProvider::ensure_part_flattened()`,
  and the `compose_world` pre-load check.
- `is_cache_artifact_compatible` (full hash) is deleted; its corruption tests
  move to the loaders' validate-while-reading path. `CacheArtifactProbeStats`
  is kept and returned by the header probe so tests can assert bounded read
  sizes.

## 2. Probe system removal

Delete:

- `src/probe_bake.{h,cpp}`, `src/probe_volume.{h,cpp}`,
  `src/probe_bricks.{h,cpp}`, `src/render/probe_texture.{h,cpp}`
- `src/world_tracer.{h,cpp}` — believed probe-only; **planning must verify no
  other consumers** before deletion
- `matter_engine.cpp`: probe thread machinery (`probe_thread_*`, `probe_stop`,
  `probe_pending`, `probe_window`, `probe_tracer`, `probe_epoch`, `BrickReq`,
  `probe_store`), `probe_tex` upload/release, `update_probe_dims`,
  probe includes
- `local_provider.cpp`: probe bake in `compose_world`, `.probes` cache
  read/write, `try_load_cached_probes`, probe fingerprint
- `WorldManifest::probes` (`provider/world_source.h`) and all consumers
- `raster_composer` `set_probes` + probe texture bindings
- GL `viewer/shaders/raster.fs`: `useProbes` sampling block and probe uniforms
  (runtime-compiled GLSL — safe to edit on Linux)
- Remaining probe references (planning enumerates the full list via grep of
  `probe`, excluding the keep-list below): known sites include
  `resolve_cache.h` and `part_asset_v2`; probe-specific tests are deleted

Keep: `lights` collection (RT lighting consumes it), `transform_probe.comp`
(unrelated Vulkan matrix-contract probe), `tileset_bake_ao` (ground tileset
texture AO — separate system).

## 3. Flatten: single streaming pass

Constraint: canonical boundary normals must be complete before ANY cluster is
decimated, so the two loops cannot merge naively.

Design: the collection pass **retains materialized clusters in memory up to a
byte budget** (default 512 MB of Tri+TriEx, overridable via env for tests).
The ladder pass consumes retained clusters and re-materializes only those past
the budget. Typical parts materialize each cluster exactly once; giant parts
keep the streaming memory bound. Boundary-normal results must be byte-identical
to the current two-pass implementation (existing seam/canonical-normal tests in
`part_flatten_tests` must pass unchanged).

## 4. Baked part AO (part-local self-occlusion)

- **Scope:** part-local only. Cross-part contact darkening remains the RT GI's
  job at runtime. Parts stay content-addressed and placement-independent.
- **Where:** end of `bake_source`, after all groups/CSG/modifiers are
  registered, before `save_v2`. Traces against the part's full assembled
  geometry (all groups), writes `TriEx.ao0/1/2` in the serialized data. No
  format change (fields already exist and serialize).
- **How:** per unique vertex position (position-keyed map so shared vertices
  get identical AO and duplicate work is skipped): cosine-weighted hemisphere
  rays around the vertex normal, origin offset by epsilon along the normal,
  distance-attenuated occlusion within a max radius (~2 units, matching MSL
  `AoParams` conventions). `ao = 1 - occlusion`. Sampling is seeded from the
  quantized vertex position — fully deterministic; byte-identical `.part`
  determinism tests must keep passing.
- **Tracing:** CPU, against the part's already-built BVHs (BLASManager/TLAS or
  an equivalent traversal). No GPU dependency; works in headless/async bakes.
- **Quality knob:** schema-level static field in the part definition, e.g.
  `static ao = { quality: 1.0 }`. Absent → quality 1.0. `quality: 0` disables
  the bake (ao stays 1.0). Rays/vertex = `clamp(round(quality * 32), 4, 128)`;
  a per-part total ray budget (e.g. 8M rays) scales rays/vertex down for huge
  parts (the adaptive behavior). Schema source participates in
  `resolve_hash`, so quality edits re-bake only that part.
- **LOD:** AO rides `TriEx` through the flatten QEM ladder. Planning must
  verify `decimate_to_error` / `MeshIndexed` round-trip carries `ao0/1/2`
  (normals/uvs already survive, so expected yes; fix if not).
- **Cache invalidation:** existing cached parts have `ao = 1.0` baked in and
  unchanged resolved hashes. Salt `resolve_hash` with an engine bake-version
  constant (bumped now, and on future bake-semantics changes) → one full cold
  rebake of the world, accepted by Jack.

## 5. Verification

- Per-task suites only (per test policy), run sequentially (WSL2 OOM
  constraint): part-asset/transient tests (§1), build + affected suites (§2),
  `part_flatten_tests` (§3), new AO unit tests + determinism suite (§4).
- Final gate: full `./build-all.sh test`, then `make windows` with a **clean
  rebuild** (struct/header changes; stale objects cause wandering crashes).
- Visual check in the live viewer (FIFO-driven, self-terminating scripts) that
  parts show crevice/cavity darkening under Vulkan.
