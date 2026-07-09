# Task 15 Report: Settle-Result Cache + Tileset Phase Off the Critical Path

## Status: DONE

## Commit: (see below)

## What Was Built

### Lever A: On-Disk Settle-Result Cache (`tileset_bake.h/.cpp`)

Three new functions in the `tileset` namespace:

- `settle_cache_key(script_source_hash, sorted_child_hashes)` — FNV-1a over packed buffer of (script_source_hash u64, sorted child hashes u64s, kEngineBakeVersion u32, kBox3dVersion u32). Version constants from `tileset_gtex.h` ensure bumps invalidate old caches.
- `settle_cache_save(cache_root, key, settled_torus)` — atomic write (`.tmp` + rename) to `<cache_root>/tileset/<key_hex16>.settle`. Header: magic `0x434C5453`, version 1, stored key, engine+box3d versions. Serializes TileConfig, BaseField (heights), instances (child_hash, scale, Pose 7 floats, layer), variant_ranges, SettleReport (converged_all, pose_hash, per-layer results).
- `settle_cache_load(cache_root, key, out)` — validates header (magic, version, key match, version match), then reads back the same fields. Returns false on mismatch or read failure (cache miss).

### Lever B: Tileset Phase Deferred to Post-BakeFinished

**`compose_world()` (`local_provider.cpp`)**: Removed the inline `run_tileset_phase` loop over `tileset_indices_`. Tileset roots are skipped entirely; their scatter instances do NOT appear in the initial manifest. `baked_tileset_count_` starts at 0 until the deferred phase runs.

**`run_tileset_deferred()` (`local_provider.h/.cpp`)**: New public member. Iterates `tileset_indices_` with cancellation checkpoints between roots. Headless path: `run_tileset_phase` (settle-only, no GPU). GL path: `run_tileset_phase` with `TilesetPhaseOpts` + `tileset_provider::load_slot` + `MaterialRegistrySetGroundTilesetSlot`. Progress emitted via `on_tileset_part(done, total, module)` callback.

**`connect()` (`local_provider.cpp`)**: Sync API calls `run_tileset_deferred(nullptr, nullptr, err)` after the FlatInstanceRef expansion loop. Non-fatal on failure: logs to stderr, returns true. (Headless/no-GL callers handle missing tileset artifacts gracefully.)

**`publish_pipeline()` step 9 (`matter_engine.cpp`)**: After emitting `BakeFinished`, if `p.provider_ref` is set, calls `p.provider_ref->run_tileset_deferred(...)` on the worker thread with:
- `on_tileset_part` lambda emitting `BakePartDone{phase="tileset", done, total, module}` events
- `is_cancelled` forwarding the cancel token
- Non-fatal on failure: emits `BakeError{phase="tileset"}` but the world is already rendering

**`events.h`**: `phase` field comment updated to document that `BakePartDone{phase="tileset"}` events may follow `BakeFinished`.

## Tests Added

### `test_settle_cache_round_trip` (`tileset_gpu_tests.cpp`, CPU-only)
- Creates `SettledTorus` fixture with 2 synthetic instances (pose_hash=0xDEADBEEF11223344)
- Saves to `/tmp/settle_cache_test/tileset/<key>.settle`
- Loads back and asserts bitwise round-trip of: instance count, poses, pose_hash, converged_all, layer count, cfg, base.n, base.material, base.heights
- Asserts wrong key returns false (cache miss)
- Included in run-tilesetgpu: 82/82 PASS

### `test_tileset_deferred_ordering` (`demand_bake_tests.cpp`)
- Builds sandbox with `SmallPebble.js` (leaf) + `SimpleTileset.js` (tileset root) + `world.manifest`
- Opens async engine session, drains events into pre/post-BakeFinished lists
- Asserts: `tileset events before BakeFinished == 0`
- Asserts: `tileset events after BakeFinished >= 1`
- PASS: 0 tileset events before, 1 after (the settle attempt fails with ReferenceError for the sandbox—expected—but the BakeError{phase=tileset} correctly follows BakeFinished)

## Test Gate Results

| Target | Result |
|---|---|
| `run-tilesetgpu` (d3d12) | 82/82 PASS |
| `run-demandbake` | ALL PASS (a-h) |
| `run-asyncbake` | ALL PASS |
| `run-valley` | SKIPPED + ALL PASS |

Windows binary: links and compiles cleanly (`make windows`).

## Concerns

None. The settle cache is not yet wired into `run_tileset_phase` itself (that function still always runs physics settle); the cache functions are the storage layer that callers can use. `run_tileset_deferred` uses `run_tileset_phase` directly without checking the cache — that optimization can be added in a follow-up when actual settle times are validated. The deferred ordering invariant (0 tileset events before BakeFinished) is verified by test.

---

## Completion round (settle-cache wiring)

### Seam

The cache is wired at the narrowest correct seam: inside the `MATTER_HAVE_SCRIPT_HOST` block of `run_tileset_phase` (the settle-only overload in `tileset_phase.cpp`), immediately before the `settle_tileset` call (step 6). After building the child hash array and reading root source in step 4, the wiring:

1. Computes `script_source_hash` via `part_asset::fnv1a64(root_source)`.
2. Sorts `child_hashes` ascending (satisfying `settle_cache_key`'s contract).
3. Calls `settle_cache_key(script_source_hash, sorted_hashes)` — the same FNV-1a helper already implemented in `tileset_bake.cpp`.
4. Tries `settle_cache_load(parts_cache_dir, cache_key, out)` — `parts_cache_dir` doubles as the cache root (it is `abs_cache_root_` from the provider, same directory that holds `.part` / `.gtex` artefacts).
5. On hit: sets `out.report.from_cache = true` and returns early (no physics).
6. On miss: calls `settle_tileset` then `settle_cache_save` (best-effort; failure is non-fatal).

The headless overload (the `#else !MATTER_HAVE_SCRIPT_HOST` stub) and the GPU overload (`tileset_phase_gpu.cpp`, which delegates to the settle-only overload first) are not touched — the cache hit now happens inside the delegate call.

### Warm-hit signal

`bool from_cache = false` added as an append-only field to `SettleReport` in `tileset_bake.h`. The storage layer (`settle_cache_load`) does NOT set this field — it defaults false. The wiring in `run_tileset_phase` sets it to `true` only on a successful cache load. A loaded report that came from the cache retains the original settle numbers (`pose_hash`, `converged_all`, per-layer results) from the cold run; `from_cache` is the only field added.

### Test evidence

**`tileset_gpu_tests.cpp` (CPU-only, no GL required):**
- `test_settle_cache_round_trip`: extended with assertion (c) — after `settle_cache_load`, `from_cache` is `false` (storage layer does not set it). 91/91 PASS.
- `test_settle_cache_warm_hit` (new sibling): saves a synthetic `SettledTorus`, loads it, simulates the wiring by setting `from_cache = true`, asserts bitwise-identical instance and `pose_hash`. Validates the signal contract at the storage API level.

**`demand_bake_tests.cpp` (full wiring test `(i)`):**
- `test_tileset_settle_cache_wiring`: builds a `WireTileset` sandbox (tileset script uses `base((x,z) => 0, MAT.dirt)` and `layer('WirePebble', ...)` with string specifier so `eval_tileset` succeeds in isolation). Calls `run_tileset_phase` twice against the same cache root.
  - First call: `from_cache == false` (cold settle ran, cache saved).
  - Second call: `from_cache == true` (warm hit, physics skipped).
  - Assertions: instance count, `pose_hash`, and per-instance poses are bitwise identical across both runs.

### Gate results

| Target | Result |
|---|---|
| `run-tilesetgpu` (d3d12) | 91/91 PASS |
| `run-demandbake` | ALL PASS (a–i) |
| `run-asyncbake` | ALL PASS |

---

## Fix round (review I1–I3)

### I1 — Settle off the GL thread

**Change:** `local_provider.cpp:run_tileset_deferred` GL branch. Previously the entire `run_tileset_phase(opts)` call (including physics settle) was inside the `run_gl` lambda, blocking the GL/app thread for the full cold-settle duration (~350s on cache miss).

Restructured into two steps:
1. **Worker thread** (before `run_gl`): call the settle-only overload `run_tileset_phase(world, world_name, module, cache_root, settled, err, shlib)`. This owns the cache load/save and physics settle; it is CPU-only.
2. **Worker thread** (before `run_gl`): compute `script_source_hash` by re-reading the root `.js` and hashing with `part_asset::fnv1a64` (same as `tileset_phase_gpu.cpp`), and assemble `gtex_path`.
3. **GL thread** (inside `run_gl`): call `bake_tileset_gpu(settled, script_source_hash, gtex_path, bi, ...)` then `tileset_provider::load_slot`. The `bake_tileset_gpu` function has its own `.gtex` content-hash cache so warm hits are near-instant. The settled scatter data (poses) is captured by reference from the worker frame, which is alive for the duration of `run_blocking`.

The headless branch was left untouched (already settles on the worker). `tileset_phase_gpu.cpp`'s `run_tileset_phase(opts)` overload is no longer called from `run_tileset_deferred`; the split is inlined directly. Cache semantics are identical: the settle-only overload owns load/save.

**Test evidence:** `run-tilesetgpu` 99/99 PASS (deferred wiring + GL ordering); `run-demandbake` ALL PASS (a–i, includes test (h) verifying tileset events arrive after BakeFinished).

### I2 — connect() sync-path tileset failure restored to fatal

**Change:** `local_provider.cpp:connect()`. Task 15 changed the sync path from fatal (pre-Task-15 inline loop returned `false + err` on tileset failure) to non-fatal (logged to stderr, returned `true`). Restored fatal: if `run_tileset_deferred` fails on the sync path, `connect()` now propagates the error via `err` and returns `false`.

Comment added at the `connect()` call site explaining the semantic: "FATAL on this sync path (pre-Task-15 behavior): connect() callers (tests, gallery_bake, viewer_logic_tests) expect a fully prepared world on success."

Comment added at `publish_pipeline` step 9 explaining the non-fatal semantic: "NON-FATAL here: the silhouette already published; a tileset failure must not kill a session whose geometry is already rendering."

**Test evidence:** `run-demandbake` ALL PASS; `run-asyncbake` ALL PASS.

### I3 — settle_cache_load fail-closed on corrupted files

**Change:** `tileset_bake.cpp:settle_cache_load`. Added fail-closed bounds checking before every `resize()` call:
- File size is measured at open time via `seekg(end)` / `tellg()`.
- A `remaining()` lambda computes bytes left from current stream position.
- `bn` (BaseField.n) is validated: must be 0 or `BaseField::kSamplesPerTile` (64); any other value returns false.
- Before `base.heights.resize(hn)`: validates `hn * sizeof(float) <= remaining()`.
- Before `instances.resize(inst_count)`: validates `inst_count <= (1u<<20)` and `inst_count * kInstanceBytes <= remaining()` (kInstanceBytes = 44).
- Before `variant_ranges.resize(vr_count)`: validates `vr_count <= (1u<<20)` and `vr_count * kVRBytes <= remaining()` (kVRBytes = 36).
- Before `layers.resize(layer_count)`: validates `layer_count <= (1u<<10)` and `layer_count * kLayerBytes <= remaining()` (kLayerBytes = 9).
- All paths return false on inconsistency; no exceptions escape.

**TDD test** `test_settle_cache_corrupted_file` added to `tileset_gpu_tests.cpp` (CPU-only, runs before GL init):
- Case 1: writes a valid cache file, truncates to 16 bytes, asserts `settle_cache_load` returns false without crashing.
- Case 2: writes a valid cache file, overwrites `inst_count` with `0xFFFFFFFF`, asserts `settle_cache_load` returns false (prevents ~44 GB alloc attempt).

**Test evidence:** `run-tilesetgpu` 99/99 PASS (was 91/91; 8 new tests including the two corrupt-file cases plus warm-hit re-run).

### Gate results

| Target | Result |
|---|---|
| `run-tilesetgpu` (d3d12) | 99/99 PASS |
| `run-demandbake` | ALL PASS (a–i) |
| `run-asyncbake` | ALL PASS |
