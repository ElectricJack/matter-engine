# Task 17: Header Dependency Tracking (-MMD -MP) — Report

## Per-Project Table

| Project | Status | Action | Header Touched | Rebuild Evidence |
|---|---|---|---|---|
| BasicWindowApp | MODIFIED | `-MMD -MP` added to CFLAGS; `-include $(OBJ:.o=.d)` + `$(OBJ:.o=.d)` in clean | `main.cpp` (via CFLAGS flows to all g++ rules) | 8 .d files generated; touch `include/*` not applicable (no include/ dir; raylib/imgui headers tracked in .d files) |
| SurfaceLib | MODIFIED | `-MMD -MP` to CFLAGS; `-include` + clean updated | `include/surface.h` | touch triggered recompile of main.o, src/surface.o, tests.o; 5 .d files generated |
| OpenParticleSurfaceLib | MODIFIED | `-MMD -MP` on each explicit compile rule; `-include $(OBJS:.o=.d)` added; clean removes build/linux/ recursively | `include/open_particle_surface.h` | touch triggered recompile of main.o and open_particle_surface.o; 4 .d files in build/linux/obj/src/ |
| ParticleDynamicsExample | MODIFIED | `-MMD -MP` on 3 pattern rules + 2 explicit sibling rules; `-include $(OBJ:.o=.d)` added; clean removes build/ recursively | `include/particle_system.h` | touch triggered recompile of main.o, particle_system.o, cluster.o, cluster_manager.o, solar_system_demo.o (all includers); 10 .d files in build/linux/obj/ |
| SpatialQueryLib | MODIFIED | `-MMD -MP` on 3 explicit compile rules; `-include $(OBJS:.o=.d)` added; clean removes obj/ recursively | `include/spatial_hash.h` | touch triggered recompile of all 4 objects; 4 .d files in obj/src/ |
| ObjectAllocatorLib | MODIFIED | `-MMD -MP` on 2 explicit compile rules; `-include $(OBJS:.o=.d)` added; clean removes obj/ recursively | `include/object_allocator.h` | touch triggered recompile of both objects; 2 .d files in obj/src/ |
| MeshChartingLib | MODIFIED | `-MMD -MP` on `lib` target's compile line; `-include mesh_charting.d` + `mesh_charting.d` in clean | `include/mesh_charting.h` | touch triggered recompile of mesh_charting.o; 1 .d file generated |
| GPURayTraceExample | MODIFIED | `-MMD -MP` on 6 explicit compile rules; `-include $(OBJ:.o=.d)` added; clean removes build/linux/ recursively | `include/bvh.h` | touch triggered recompile of main.o, bvh.o, blas_manager.o, tlas_manager.o, bvh_visualizer.o; 6 .d files in build/linux/obj/ |
| MatterSurfaceLib | MODIFIED | `-MMD -MP` on all 32 explicit compile rules; `-include $(OBJ:.o=.d)` added; clean removes build/linux/ recursively | `include/cluster.h` | touch triggered recompile of cluster.o, cell.o, main.o, object_allocator.o; 32 .d files in build/linux/obj/ |
| MatterEngine3 (root) | SKIPPED | Batch compile rules (`$(ME3_OBJ): $(ME3_CPP)` / `$(QJS_OBJ): $(QJS_C)`) compile all sources in one invocation without per-file `-o` routing; `-MMD` would not produce useful per-TU dep files in this pattern. No .o-based per-file rules exist. | — | — |
| MatterEngine3/viewer | ALREADY DONE | Already had `-MMD -MP` in `CXX_FLAGS_BUILD` and `-include $(L_ALL_OBJ:.o=.d)` / `-include $(W_ALL_OBJ:.o=.d)`. No change needed. | — | — |
| MatterEngine3/tests | SKIPPED | All test targets compile all sources in a single g++ invocation with no intermediate .o files; intermediate .o files are immediately `rm -f`'d after link. `-MMD` produces no useful per-TU dep wiring in this pattern. | — | — |
| MeshChartingLib/tests | SKIPPED | Single-shot compilation with no intermediate .o files. | — | — |

## build-all.sh Result vs Known-Red

```
BasicWindowApp            OK
SurfaceLib                OK
ObjectAllocatorLib        OK
SpatialQueryLib           OK
MatterEngine3             OK
OpenParticleSurfaceLib    OK
GPURayTraceExample        OK
MatterSurfaceLib          FAIL   ← known baseline (primitive_sdf undefined at LINK; objects compile fine)
ParticleDynamicsExample   OK
```

No new failures. Exactly matches known-red baseline.

## .gitignore

Added `*.d` to the build artifacts section (was not previously covered).

## Deviations

1. **MatterEngine3 root Makefile**: Skipped because both compile rules are batch patterns that don't map one-to-one to the .o file list. Adding `-MMD` would generate a single .d file named after the final source file in the batch, not per-TU tracking. Per task brief guidance ("only add dep tracking where object files actually exist [in a useful per-file sense]"), this is correct to skip.

2. **MatterEngine3/tests Makefile**: Skipped for the same reason — all targets use single-invocation compilation with no persistent .o files.

3. **SurfaceLib LINK**: Fails due to wrong raylib.a (Windows cross-compiled library). Pre-existing baseline; objects and .d files are generated correctly.

4. **BasicWindowApp**: CFLAGS `-MMD -MP` is passed through to all g++ explicit rules via `$(CFLAGS)`. The `.d` files land in the project root (next to .o files), matching the OBJ variable layout.

## Concerns

None. All modified Makefiles generate .d files next to their corresponding .o files, and `-include` uses a `-` prefix so missing .d files on first build are silently ignored. Clean targets remove .d files either explicitly (BasicWindowApp, SurfaceLib, MeshChartingLib) or implicitly by `rm -rf` on the OBJ_DIR (all others).
