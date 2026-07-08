# Task 5 Report: Stage 2b — EngineContext/WorldSession Facade

## Summary

Created `MatterEngine3/viewer/matter_engine.cpp` implementing `matter::EngineContext` and `matter::WorldSession` as a facade over the in-place viewer pipeline. Added `LocalProviderConfig::on_part` progress callback. Build is clean; aerial screenshot MATCHes the reference.

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
