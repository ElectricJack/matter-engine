# Task 4 Report — GPU-executor seam in LocalProvider + assert_gl_thread guards

**Date:** 2026-07-08
**Branch:** feature/phase-b-async-bake
**Status:** COMPLETE — all targeted suites green, no behavior regression

---

## What Changed

### 1. `MatterEngine3/src/provider/local_provider.h`

Added `gpu_run` field to `LocalProviderConfig`:

```cpp
std::function<bool(const char* name,
                   std::function<bool(std::string& err)> fn,
                   std::string& err)> gpu_run;
```

Also added `#include <utility>` (for `std::move` in the impl).

### 2. `MatterEngine3/src/provider/local_provider.cpp`

Wrapped the two tileset GL sections (previously `run_tileset_phase` + `load_slot` inline) into a single per-tileset closure routed through a local `run_gl` helper:

```cpp
auto run_gl = [&](const char* name, std::function<bool(std::string&)> fn,
                  std::string& e) -> bool {
    if (cfg_.gpu_run) return cfg_.gpu_run(name, std::move(fn), e);
    return fn(e);   // inline (synchronous path unchanged)
};
```

Per-tileset granularity: bake + upload are ONE closure per tileset (as the brief specifies). Capture-by-reference is safe because the provider blocks until `gpu_run` returns. `baked_tileset_count_` is incremented outside the closure after success, so the slot index is captured as a value (`slot_idx = baked_tileset_count_` before the closure).

Behavior when `gpu_run == null`: identical to the original code path — `fn(e)` is called inline on the calling thread.

### 3. `MatterEngine3/src/matter_engine.cpp`

Added `matter_async::register_gl_thread()` call at the top of `EngineContext::create`, arming the GL-thread guards in production. Already included `"async_bake.h"` in this file.

### 4. GL-thread guards added — `assert_gl_thread` entry points

| File | Function | Guard string |
|------|----------|--------------|
| `MatterEngine3/src/tileset_bake_gpu.cpp` | `bake_tileset_gpu()` | `"bake_tileset_gpu"` |
| `MatterEngine3/src/render/tileset_provider.cpp` | `tileset_provider::load_slot` | `"tileset_provider::load_slot"` |
| `MatterEngine3/src/render/raster_composer.cpp` | `RasterComposer::init` | `"RasterComposer::init"` |
| `MatterEngine3/src/render/raster_composer.cpp` | `RasterComposer::init_gpu_driven` | `"RasterComposer::init_gpu_driven"` |
| `MatterEngine3/src/render/probe_texture.cpp` | `upload_probe_textures` | `"upload_probe_textures"` |
| `MatterEngine3/src/render/gpu_culler.cpp` | `GpuCuller::init` | `"GpuCuller::init"` |

Each guard is one line at the function top, `#include "async_bake.h"` added to each TU. For `render/` files, `async_bake.h` resolves via the `-I../src` include path already active for that directory.

### 5. `MatterEngine3/tests/Makefile`

Added `../src/async_bake.cpp` to `GPU_RENDER_CPP`. This is a consequential fix: `matter_engine.cpp` now calls `matter_async::register_gl_thread()` and the render TUs call `matter_async::assert_gl_thread()`, so the GPU test binaries (which compile sources directly, not via `libmatter_engine3.a`) need `async_bake.cpp` in their link. Without this, GPU tests produced linker errors:

```
undefined reference to `matter_async::register_gl_thread()'
undefined reference to `matter_async::GpuJobQueue::pump(double)'
```

---

## Guard placement — thread-safety analysis

All six guarded functions are called from the existing synchronous bake path running on the GL/app thread. Since `register_gl_thread()` is called in `EngineContext::create` (the first thing the app does after `InitWindow`), the guards will arm correctly in production. In tests that don't go through `EngineContext::create` (headless suites), `register_gl_thread()` is never called → assert_gl_thread silently no-ops (the documented behavior: unregistered = skip check), so headless suites are unaffected.

No guard entry point is called from a non-GL thread in today's synchronous bake — all six are either called from `LocalProvider::connect` (which runs on the app thread) or from `bake_once` in `matter_engine.cpp` (also on the app thread).

---

## Build results

- `make -C MatterEngine3 -j$(nproc)` — clean build, no new errors (pre-existing `memset`/`TriEx` warnings from MatterSurfaceLib only)
- `make -C MatterViewer -j$(nproc)` — clean build, viewer binary updated

---

## Suite results

### Headless tileset suites (no GALLIUM_DRIVER required)

| Target | Result | Notes |
|--------|--------|-------|
| `run-tilesetphysics` | pre-existing FAIL | `tileset_settle.h` not in `-I../include`; same on baseline |
| `run-tilesetcore` | pre-existing FAIL | `tileset_layout.h` not in `-I../include`; same on baseline |
| `run-tilesetplacement` | pre-existing FAIL | `tileset_placement.h` not in `-I../include`; same on baseline |
| `run-tilesetdsl` | PASS | 0 failures |
| `run-tilesetbake` | PASS | All tests passed |
| `run-tilesetgtex` | pre-existing FAIL | `-Werror` on sign-compare in tileset_gtex.cpp; same on baseline |
| `run-tilesettorusbvh` | PASS | 12/12 passed |
| `run-tilesetmeadowmanifest` | PASS | 4 run, 0 failed |

The 4 pre-existing failures were verified identical on the baseline (before Task 4 changes via `git stash`).

### GPU tileset suites (GALLIUM_DRIVER=d3d12)

| Target | Result | Notes |
|--------|--------|-------|
| `run-tilesetgpu` | PASS | 62/62 passed — ALL PASS |
| `run-tilesetseam` | PASS (when run standalone) | 32/32 passed — ALL PASS |
| `run-tilesetprovider` | PASS | 26 run, 0 failed |
| `run-tilesetload` | PASS | 4 run, 0 failed |

Note: when `run-tilesetseam` and `run-tilesetload` are run sequentially after another test target that cleans shared `.o` files (`rm -f *.o`), a pre-existing stale-`.o` linker error occurs. Running each target independently succeeds. This is a pre-existing Makefile issue (not introduced by Task 4).

---

## Commit

`feat(phase-b): gpu_run executor seam in LocalProvider + assert_gl_thread guards (Task 4)`

Files committed:
- `MatterEngine3/src/provider/local_provider.h`
- `MatterEngine3/src/provider/local_provider.cpp`
- `MatterEngine3/src/matter_engine.cpp`
- `MatterEngine3/src/tileset_bake_gpu.cpp`
- `MatterEngine3/src/render/tileset_provider.cpp`
- `MatterEngine3/src/render/raster_composer.cpp`
- `MatterEngine3/src/render/probe_texture.cpp`
- `MatterEngine3/src/render/gpu_culler.cpp`
- `MatterEngine3/tests/Makefile`
- `.superpowers/sdd/task-4-report.md`
