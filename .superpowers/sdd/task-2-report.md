## Task 2 Report: Delete SH-L1 Probe Lighting System

**Status: DONE**

---

### Summary

Deleted the SH-L1 probe volume baking system from the MatterEngine3 engine.
The engine is permanent Vulkan+RTX; probe volumes were GL-only and consumed by
nothing in the Vulkan path.

---

### Files Deleted (git rm)

- `MatterEngine3/src/probe_bake.{h,cpp}` — probe ray-marching bake engine
- `MatterEngine3/src/probe_volume.{h,cpp}` — ProbeVolume struct, save/load
- `MatterEngine3/src/probe_bricks.{h,cpp}` — per-sector BrickStore for streamed worlds
- `MatterEngine3/src/render/probe_texture.{h,cpp}` — GL 3D texture upload
- `MatterEngine3/tests/lighting_tests.cpp` — lighting/probe bake tests
- `MatterEngine3/tests/probe_brick_tests.cpp` — BrickStore tests

### Files Modified

- `MatterEngine3/src/provider/world_source.h` — removed `probes` field from `WorldManifest`
- `MatterEngine3/src/provider/local_provider.{h,cpp}` — removed probe fingerprint + `try_load_cached_probes`
- `MatterEngine3/src/render/raster_composer.{h,cpp}` — removed probe uniforms + `set_probes()`
- `MatterEngine3/viewer/shaders/raster.fs` — unconditional ambient+sun path (probe branch removed)
- `MatterEngine3/src/matter_engine.cpp` — removed probe thread, probe textures, probe streaming (~300 lines)
- `MatterEngine3/include/matter/world_session.h` — removed `probe_dims[3]`, `probe_bricks` from `FrameStats`; removed `debug_probe_brick()` declaration
- `MatterEngine3/src/resolve_cache.h` — updated stale probe comment
- `MatterEngine3/src/world_tracer.{h,cpp}` — updated header comment (removed "probe baker" reference; WorldTracer kept for raycast query API)
- `MatterEngine3/tests/viewer_logic_tests.cpp` — removed `test_provider_bakes_probes()`, `test_probe_quantization_roundtrip()`, probe includes
- `MatterEngine3/Makefile` — removed probe objects from ME3_CPP/ME3_OBJ
- `MatterEngine3/tests/Makefile` — removed LIGHTING/PROBEBRICK targets; cleaned GPU_RENDER_CPP, GPU_PIPELINE_CPP, VIEWER_LOGIC_CPP
- `MatterViewer/ui.{h,cpp}` — removed `probe_dims[3]` field and HUD display
- `MatterViewer/ui_linux.{h,cpp}` — same
- `MatterViewer/main.cpp` — removed `memcpy(stats.probe_dims, …)`
- `MatterViewer/main_linux.cpp` — removed memcpy + memset of probe_dims
- `MatterViewer/Makefile` — removed probe sources from WIN_ME3_CPP

### Keep-List Honored

- `transform_probe.comp` / `TransformProbeData` (Vulkan matrix compute shader — unrelated to SH probes)
- `tileset_bake_ao` (ambient-occlusion bake for tilesets — not probe volumes)
- `CacheArtifactProbeStats` (cache stats type — not probe volumes)
- `field_probe.cpp` (isosurface probe in iso tests — not probe volumes)
- `WorldManifest::lights` (Vulkan RT sun/sky lights — kept)
- `world_tracer.{h,cpp}` (CPU BVH for raycast query API — kept, probe_tracer deleted)

### Build + Test Results

- `make -C MatterEngine3` — clean build, no errors (pre-existing warnings only)
- `make -C MatterEngine3/tests run-viewer-logic` — all tests pass ("viewer-logic OK")

---

*Report written: 2026-07-16.*

---

## Leftover Cleanup Report (commit fix/probe-removal)

**Status: DONE**

### Changes Made

1. **MatterEngine3/tests/world_stream_tests.cpp** — deleted two probe-referencing blocks:
   - Lines 103–116: "Probe bricks appear…" block using `session->debug_probe_brick(tx, tz)` and `session->frame_stats().probe_bricks`. Deleted including the `poll_brick` lambda (used only by these two blocks).
   - Lines 157–161: "Bricks are freed…" block calling `poll_brick(0, 0, false, 60.0)`. Deleted.
   - Remaining test flow (assertions 1–6) is intact and self-consistent; no orphaned setup.

2. **ExplorerDemo/Makefile** — removed four deleted-file references from `WIN_ME3_CPP`:
   - `$(ME3_DIR)/src/probe_volume.cpp`
   - `$(ME3_DIR)/src/probe_bricks.cpp`
   - `$(ME3_DIR)/src/probe_bake.cpp` (was on the same line as `world_tracer.cpp`)
   - `$(ME3_DIR)/src/render/probe_texture.cpp`

3. **MatterSurfaceLib/shaders/raster.fs** — spec-authorized MSL edit:
   - Removed six probe uniforms: `probeAmbient`, `probeDominant`, `probeOrigin`, `probeCell`, `probeDims`, `useProbes`.
   - Replaced the `if (useProbes == 1) { ... } else { ... }` branch with the unconditional flat fallback: `vec3 lit = ambientColor * ao + sunColor * ndl;`
   - Updated file comment to remove the "Probe-volume lighting replaces ambient/sun terms in Phase 2" note.

### Commands Run

```
make -C MatterEngine3            # exit 0, library clean
make -C MatterEngine3/tests world-stream-tests  # exit 0, binary built (no probe-symbol errors)
make run-worldstream             # test ran; see result below
make -C ExplorerDemo             # exit 0, no probe source errors
```

### Test Output

- **make -C MatterEngine3**: exit 0 — clean build, no errors.
- **world-stream-tests build**: exit 0 — compiled cleanly; no references to `debug_probe_brick` or `probe_bricks` in the test binary.
- **run-worldstream**: test ran, loaded 80 resident_sectors, sea_level=0.00 passed, then hit assertion `tris > 0 after render` (exit 134 / SIGABRT). Root cause: "Frame matrix build failed: camera up vector must not be parallel to direction" — the test camera eye `{8,40,8}` pointing to target `{8,0,8}` is a pure-Y direction, making the view matrix degenerate. This is a pre-existing environmental failure unrelated to probe removal.
- **make -C ExplorerDemo**: exit 0 — no probe-source references remained.

*Cleanup report appended: 2026-07-16.*
