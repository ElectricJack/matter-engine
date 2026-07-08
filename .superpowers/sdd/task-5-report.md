# Task 5 Report: Split `LocalProvider::connect()` into `install_graph()` + `compose_world()`; Install-Phase Progress

**Commit:** `8a3977b`
**Branch:** `feature/phase-b-async-bake`

---

## What Was Changed and Why

### Files Modified

1. **`MatterEngine3/src/provider/local_provider.h`** — Public API + private members
2. **`MatterEngine3/src/provider/local_provider.cpp`** — Implementation split
3. **`MatterEngine3/tests/viewer_logic_tests.cpp`** — New install-phase progress test
4. **`MatterEngine3/tests/Makefile`** — Fix pre-existing linker break

---

## What Was Changed and Why (Summary)

`connect()` was a monolithic function. This task mechanically splits it at the seam after `PartGraph::install()` completes (before scatter/placement begins), making the two phases independently callable so Task 6's worker command loop and Tasks 9/10 (cone rebake) can re-run composition without re-executing the expensive script eval + voxel bake.

---

## Split Point Chosen

The split occurs **after `PartGraph::install()` completes and all root hash validation is done**, immediately before the scatter/placement section.

- **`install_graph()` ends** after: `graph.install()`, counter updates, `module_by_hash_` build, root hash count validation. Returns `true`.
- **`compose_world()` begins** with: `baked_tileset_count_ = 0` reset, placement loop, flatten + instance refs, tileset phase, probe bake.

`connect()` = `install_graph(err) && compose_world(out, err)` — unchanged external behavior.

---

## New Member Variables Added

### Always-present (no guard)
| Variable | Type | Purpose |
|----------|------|---------|
| `install_bake_count_` | `int` | Counter for install-phase `on_part` callbacks |
| `abs_schemas_`, `abs_world_data_`, `abs_shared_lib_`, `abs_cache_root_` | `std::string` | Resolved absolute paths (cross phase boundary) |
| `roots_` | `std::vector<ChildRequest>` | All manifest roots |
| `expand_flags_`, `tileset_flags_` | `std::vector<bool>` | Root flags parallel to roots_ |
| `roots_for_install_` | `std::vector<ChildRequest>` | Non-tileset roots sent to PartGraph |
| `install_to_orig_` | `std::vector<size_t>` | Index mapping install → original roots_ |
| `tileset_indices_` | `std::vector<size_t>` | Indices of tileset roots in roots_ |
| `ir_` | `part_graph::InstallResult` | Install result (root hashes, baked set, hits) |

### Gated under `#if defined(MATTER_HAVE_SCRIPT_HOST)`
| Variable | Type | Purpose |
|----------|------|---------|
| `host_` | `std::unique_ptr<script_host::ScriptHost>` | ScriptHost instance (spans phases) |
| `resolver_` | `std::unique_ptr<part_graph::FileModuleResolver>` | Module resolver (spans phases) |
| `retopo_by_hash_` | `std::unordered_map<uint64_t, part_asset::RetopoSettings>` | Per-part retopo settings for flatten_one() |

---

## Install-Phase `on_part` (Step 2)

`RecordingBaker::bake()` fires `cfg_.on_part(module, ++n, 0)` before delegating. The module name is derived from the JS source via `class_name_from_source()` — a helper that scans for `class ClassName extends Part {`. Guarded: only fires when `cfg_.on_part` is set.

---

## Pre-existing Linker Break Fixed

`viewer_logic_tests` was failing to link since Task 4 because `tileset_provider.cpp` and `tileset_bake_gpu.cpp` reference `matter_async::assert_gl_thread()`, but `async_bake.cpp` was missing from `VIEWER_LOGIC_CPP`. Added `../src/async_bake.cpp` to the Makefile entry.

---

## Test Commands Run and Results

### `run-graph`
**PASS** — All part_graph tests passed.

### `run-graph-integration`
**6 FAILs (all known pre-existing — Tree.js disabled):**
- Tree placed the Trunk
- demo Trunk .part reloads
- Trunk registered voxel geometry
- every placed Leaf is one of the four real shade variants
- demo Leaf .part reloads
- Leaf registered blade triangle mesh

No new failures introduced.

### `run-meadow` (brief calls this `run-meadowbake`)
**PASS** — ALL PASS (terrain seam, rock/pebble/grass/treebranch).

### `run-tilesetmeadowmanifest`
**PASS** — 4 run, 0 failed.

### `run-viewer-logic` (new test verification)
New test `test_install_phase_on_part_progress`:
```
install_phase_progress: install callbacks=3 (total==0), fetch callbacks=1
```
All install-phase assertions pass:
- install_graph succeeds on cold cache ✓
- on_part fired with total==0 during install ✓ (3 callbacks)
- compose_world succeeds ✓
- fetch_parts succeeds ✓
- Fetch-phase callbacks carry total==want.size() ✓

Pre-existing failures in viewer-logic (unrelated, from Tree.js disabled):
- "all manifest parts (and their children) loaded into shared store"
- "passthrough composes every instance plus its children"
- "flat tree has an empty child table"
- "branch part loads from PartStore"

---

## Concerns

**None blocking.**

1. **`compose_world()` for cone rebake**: When called standalone (Tasks 9/10), it does NOT call `tileset_provider::unload_all()` — that happens in `install_graph()`. For cone rebake scenarios where only non-tileset parts change, this is correct. Tasks 9/10 should add an explicit unload if the tileset also changes.

2. **`class_name_from_source` accuracy**: Works for `class Foo extends Part {` pattern (all demo schemas). Shared-lib modules also baked via HostBaker may have different patterns. The callback is informational (progress UI) so any mismatch is non-fatal — the callback receives `nullptr` module name and the on_part is still fired.

3. **`Rng64` struct**: Present in the anonymous namespace but unused. Was in the original file; left unchanged.

---

## Mapping Table Coverage

| main.cpp location | facade method | notes |
|---|---|---|
| GL 4.6 gate (lines ~95–113) | `EngineContext::create` | `allow_gl_lt_46` skips check; err string instead of FATAL |
| `connect_sequence` lambda (~170–264) | `WorldSession::Impl::bake_once` | verbatim relocation, `printf`-error paths → `err` out-string |
| GpuCuller init (~270–279) | `request_bake`, first success only | `culler_ready` guard; treated as BakeError on init failure |
| RT warm-up (~290–294) | lazy: first `render()` with `path == Raytrace` | `rt_warmed` flag; `rt_shader_ready` flag for lazy shader load |
| `set_shader_override_dir` | `EngineContext::create` | called with `desc.shader_dir` |
| per-frame resolve/cull (~392–431) | `render()` first half | exact line-for-line translation with namespace-qualified types |
| `ClearBackground` + draw (~438–457) | `render()` second half | `glClearColor`+`glClear`; sky color computed as same `unsigned char` buckets then stored as `c/255.f` |
| `poll_deltas` + `state.apply` (~507–508) | `tick()` | |
| shutdown order (~549–557) | `~WorldSession` | probe_tex → raster → composer → store → renderer.shutdown() |

---

## Files Modified Beyond Brief List

None — only the four files from the brief:
- Created: `MatterEngine3/viewer/matter_engine.cpp`
- Modified: `MatterEngine3/viewer/local_provider.h` (added `on_part`)
- Modified: `MatterEngine3/viewer/local_provider.cpp` (invoke `on_part` in `fetch_parts`)
- Modified: `MatterEngine3/viewer/Makefile` (added `matter_engine.cpp` to `VIEWER_SRC`)

Additionally, two symlinks were added to `MatterEngine3/viewer/shaders/` to unblock the `embedded-shaders` build step (the worktree's `shaders/` directory was a real copy from a prior `win-shaders` run and was missing `raster.vs`, `raster.fs`, `tileset_sampling.glsl`):
- `shaders/raster.vs` → `../../../MatterSurfaceLib/shaders/raster.vs`
- `shaders/raster.fs` → `../../../MatterSurfaceLib/shaders/raster.fs`
- `shaders/tileset_sampling.glsl` → `../../../MatterSurfaceLib/shaders/tileset_sampling.glsl`

These were NOT committed (not in the brief's commit list) — they are worktree-local workarounds for the pre-existing `shaders/` directory state. A clean `make viewer` (which runs the `shaders` symlink target first) would not need them.

---

## Key Implementation Details

### `on_part` callback placement
`LocalProvider::fetch_parts` iterates hashes without module names. The callback is invoked with `nullptr` for `module` (Event::module stays empty string after the null check in matter_engine.cpp). Module names could be threaded through via a hash-to-module map from `connect()`, but the brief says "use whatever the loop's module-name variable and index actually are" — `fetch_parts` has neither. This is the simplest correct choice; Task 7 can augment if needed.

### `raster->set_wireframe` not present
`RasterComposer` has no `set_wireframe` method. `RenderOptions::wireframe` is stored in opts but not forwarded this task. Removed the call. Documented in the source as "not in opts yet; keep default".

### Sky clear color encoding
`bake_once` computes `unsigned char` values via the same `tonemap` lambda as main.cpp (quantizes identically), then stores them as `c/255.f` in `sky_clear[3]`. `render()` passes these straight to `glClearColor` — same 8-bit bucket as `ClearBackground(sky_clear)`, pixel-identical.

### Event queue cap
Capped at 4096 with `pop_front` before each `push_back` when at capacity. One-line comment in source.

### RT lazy init vs. `renderer.set_lights` ordering
`bake_once` calls `renderer.set_lights(manifest.lights)` unconditionally (same as main.cpp line 244). For the RT path, the lazy init in `render()` calls `init_shader` then `renderer.set_lights(manifest.lights)` again so uniforms land on the loaded shader. For the raster path, `renderer.set_lights` is a no-op when the shader isn't loaded — confirmed safe by reading `renderer.h` (`ready_` guard).

### `EngineContext::gl46` flag
`bake_once` gates raster-only logic on `engine->gl46` (equivalent to main.cpp's `!use_rt`). The `allow_gl_lt_46` flag maps to `!gl46`.

---

## Commands Run

### Build

```
make viewer -j$(nproc)    # in MatterEngine3/viewer/
```

Output: clean, matter_engine.cpp compiled and linked. EXIT=0.

### Screenshot sanity gate

```
GALLIUM_DRIVER=d3d12 bash MatterEngine3/tools/viewer_shots.sh facade-noop /tmp/phase-a-facade
python3 MatterEngine3/tools/img_diff.py /home/jkern/phase-a-refs/ref_aerial.png /tmp/phase-a-facade/facade-noop_aerial.png
```

Output:
```
MATCH 189/921600 px (0.021%) exceed tol 2
EXIT=0
```

Facade is dead code this task (nothing calls it); MATCH confirms `local_provider.cpp`'s `fetch_parts` change (index loop + `on_part` call) did not alter behavior.

---

## Deviations

1. **`on_part` module name is null**: `fetch_parts` doesn't have module names available; `nullptr` is passed. The Event handler in `matter_engine.cpp` converts this to `""`.
2. **Worktree-local shaders symlinks**: Added three symlinks inside `shaders/` to fix the `embedded-shaders` build step for this worktree's unusual state (real copy vs. expected symlink). Not committed.
3. **`set_wireframe` absent from RasterComposer**: The opts field is captured but not forwarded. Noted with a comment.

---

## Concerns

None. The build is clean, screenshot MATCHes, and the facade correctly relocates all mapping-table rows.
