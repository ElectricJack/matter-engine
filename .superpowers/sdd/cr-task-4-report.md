# Task 4 Report: De-duplicate shared sources into consumers (T1)

## MD5 Comparison Table (Consumer copies vs. pre-fix source-of-truth @ 8fa6f77)

Pre-fix truth checksums:
- `spatial_hash.c`:   `3edac22ebf8e811a3a7f41f942c129eb`
- `spatial_hash.h`:   `77e1fd85c2f266f9f39e99e66cf5604b`
- `object_allocator.c`: `f11008583fe94b8d16a4b3c5b473a85d`
- `object_allocator.h`: `8588eba175947bd386e8bea1172629f0`

| Project | File | Consumer MD5 | Matches truth? | Action |
|---|---|---|---|---|
| SpatialQueryLib | src/object_allocator.c | f11008583fe94b8d16a4b3c5b473a85d | YES | Deleted, compile from ObjectAllocatorLib |
| SpatialQueryLib | include/object_allocator.h | 8588eba175947bd386e8bea1172629f0 | YES | Kept (spatial_hash.c truth file references it via `"../include/object_allocator.h"`) |
| ParticleDynamicsExample | src/object_allocator.c | f11008583fe94b8d16a4b3c5b473a85d | YES | Deleted |
| ParticleDynamicsExample | src/spatial_hash.c | 3edac22ebf8e811a3a7f41f942c129eb | YES | Deleted |
| ParticleDynamicsExample | include/object_allocator.h | 8588eba175947bd386e8bea1172629f0 | YES | Deleted |
| OpenParticleSurfaceLib | src/object_allocator.c | f11008583fe94b8d16a4b3c5b473a85d | YES | Deleted |
| OpenParticleSurfaceLib | src/spatial_hash.c | 3edac22ebf8e811a3a7f41f942c129eb | YES | Deleted |
| OpenParticleSurfaceLib | include/object_allocator.h | 8588eba175947bd386e8bea1172629f0 | YES | Deleted |
| OpenParticleSurfaceLib | include/spatial_hash.h | 77e1fd85c2f266f9f39e99e66cf5604b | YES | Deleted |
| OpenParticleSurfaceLib | src/mc_tables.h | 4e5beaa56a0ba58b6b7b0f8ed8581199 | matches include/mc_tables.h | Deleted |
| SurfaceLib | src/object_allocator.c | f11008583fe94b8d16a4b3c5b473a85d | YES | Deleted |
| SurfaceLib | src/spatial_hash.c | 3edac22ebf8e811a3a7f41f942c129eb | YES | Deleted |
| SurfaceLib | include/object_allocator.h | 8588eba175947bd386e8bea1172629f0 | YES | Deleted |
| SurfaceLib | include/spatial_hash.h | 77e1fd85c2f266f9f39e99e66cf5604b | YES | Deleted |
| GPURayTraceExample | src/object_allocator.c | f11008583fe94b8d16a4b3c5b473a85d | YES | Deleted |
| GPURayTraceExample | include/object_allocator.h | 8588eba175947bd386e8bea1172629f0 | YES | Deleted |
| MatterSurfaceLib | src/object_allocator.c | f11008583fe94b8d16a4b3c5b473a85d | YES | EXCLUDED (Task 10) |
| MatterSurfaceLib | src/spatial_hash.c | 7988c0785ca3e660736247b99a9bd02a | **DIVERGED** | EXCLUDED (Task 10) |
| MatterSurfaceLib | include/object_allocator.h | 8588eba175947bd386e8bea1172629f0 | YES | EXCLUDED (Task 10) |
| MatterSurfaceLib | include/spatial_hash.h | f26a7aab77b41f16e91d9affde313530 | **DIVERGED** | EXCLUDED (Task 10) |

## Divergent Copies Found

- **MatterSurfaceLib/src/spatial_hash.c**: MD5 `7988c0785ca3e660736247b99a9bd02a` — diverged from pre-fix truth. Left untouched per exclusion (Task 10).
- **MatterSurfaceLib/include/spatial_hash.h**: MD5 `f26a7aab77b41f16e91d9affde313530` — diverged. Left untouched per exclusion (Task 10).

No unexpected divergences found outside MatterSurfaceLib.

## Makefile Changes

### SpatialQueryLib/Makefile
- Added `OA_LIB = ../ObjectAllocatorLib` variable
- Added `-I$(OA_LIB)/include` to `CFLAGS`
- Changed `SRCS` to enumerate sources explicitly (removed wildcard which would have picked up the now-deleted object_allocator.c)
- Added explicit rule `$(OBJ_DIR)/src/object_allocator.o: $(OA_LIB)/src/object_allocator.c`
- Updated `TEST_SRCS` and `BVH_TEST_SRCS` to reference `$(OA_LIB)/src/object_allocator.c`
- Added `-I$(OA_LIB)/include` to `TEST_FLAGS` and `BVH_TEST_FLAGS`

### ParticleDynamicsExample/Makefile
- Added `OA_LIB = ../ObjectAllocatorLib` variable
- Added `-I$(OA_LIB)/include` to `CFLAGS` and `CXXFLAGS`
- Removed `src/object_allocator.c` and `src/spatial_hash.c` from `SRC` and `OBJ` lists
- Removed `dependencies` target (had `cp` rules for both files)
- Changed `all:` to no longer depend on `dependencies`
- Added explicit compile rules for sibling sources:
  - `$(OBJ_DIR)/object_allocator.o: $(OA_LIB)/src/object_allocator.c`
  - `$(OBJ_DIR)/spatial_hash.o: $(SPATIAL_QUERY_PATH)/src/spatial_hash.c`
- Removed `dependencies` from `.PHONY`

### GPURayTraceExample/Makefile
- Added `OA_LIB = ../ObjectAllocatorLib` variable
- Added `-I$(OA_LIB)/include` to `CFLAGS` and `CXXFLAGS`
- Removed `src/object_allocator.c` from `SRC` list
- Removed `dependencies` target (had `cp ../ObjectAllocatorLib/src/object_allocator.c src/`)
- Changed `all:` to no longer depend on `dependencies`
- Updated existing `$(OBJ_DIR)/object_allocator.o` rule to compile from `$(OA_LIB)/src/object_allocator.c`
- Removed `dependencies` from `.PHONY`

### OpenParticleSurfaceLib/Makefile
- Added `SPATIAL_QUERY_PATH = ../SpatialQueryLib` and `OA_LIB = ../ObjectAllocatorLib` variables
- Added `-I$(SPATIAL_QUERY_PATH)/include -I$(OA_LIB)/include` to `CFLAGS`
- Changed `SRCS` to enumerate only project-specific sources
- Changed `OBJS` to list all object files explicitly
- Added explicit compile rules for sibling sources:
  - `$(OBJ_DIR)/$(SRC_DIR)/object_allocator.o: $(OA_LIB)/src/object_allocator.c`
  - `$(OBJ_DIR)/$(SRC_DIR)/spatial_hash.o: $(SPATIAL_QUERY_PATH)/src/spatial_hash.c`

### SurfaceLib/Makefile
- Added `SPATIAL_QUERY_PATH = ../SpatialQueryLib` and `OA_LIB = ../ObjectAllocatorLib` variables
- Added `-I$(SPATIAL_QUERY_PATH)/include -I$(OA_LIB)/include` to `CFLAGS`
- Removed `src/spatial_hash.c` and `src/object_allocator.c` from `SRC`
- Updated `OBJ` to enumerate explicitly (including sibling object files placed in `src/`)
- Added explicit compile rules for sibling sources:
  - `src/object_allocator.o: $(OA_LIB)/src/object_allocator.c`
  - `src/spatial_hash.o: $(SPATIAL_QUERY_PATH)/src/spatial_hash.c`

## Source File Include Updates

Updated relative `#include "../include/..."` references to bare-form includes (resolved via `-I` flags) in project-specific source files that were not being deleted:

- `OpenParticleSurfaceLib/src/open_particle_surface.c`: `../include/object_allocator.h` → `object_allocator.h`, `../include/spatial_hash.h` → `spatial_hash.h`
- `OpenParticleSurfaceLib/src/surface.c`: `../include/spatial_hash.h` → `spatial_hash.h`
- `SurfaceLib/src/surface.c`: `../include/spatial_hash.h` → `spatial_hash.h`

Note: `SpatialQueryLib/src/spatial_hash.c` (the source-of-truth itself) still uses `#include "../include/object_allocator.h"` — this resolves to `SpatialQueryLib/include/object_allocator.h` which is kept in place to avoid modifying the canonical file.

## Files Deleted

```
SpatialQueryLib/src/object_allocator.c
ParticleDynamicsExample/src/object_allocator.c
ParticleDynamicsExample/src/spatial_hash.c
ParticleDynamicsExample/include/object_allocator.h
OpenParticleSurfaceLib/src/object_allocator.c
OpenParticleSurfaceLib/src/spatial_hash.c
OpenParticleSurfaceLib/include/object_allocator.h
OpenParticleSurfaceLib/include/spatial_hash.h
OpenParticleSurfaceLib/src/mc_tables.h
SurfaceLib/src/object_allocator.c
SurfaceLib/src/spatial_hash.c
SurfaceLib/include/object_allocator.h
SurfaceLib/include/spatial_hash.h
GPURayTraceExample/src/object_allocator.c
GPURayTraceExample/include/object_allocator.h
```

Total: 15 files deleted.

## Build & Test Evidence

### SpatialQueryLib
```
$ make clean && make
gcc ... -c ../ObjectAllocatorLib/src/object_allocator.c -o obj/src/object_allocator.o
gcc ... -o spatialquerylib ... [SUCCESS]

$ make test
=== Results: 8/8 passed ===  (spatial_hash_tests)
=== Results: 5/5 passed ===  (bvh_tests)
```

### ObjectAllocatorLib
```
$ make clean && make && make test
All tests passed!
```

### ParticleDynamicsExample
```
$ make clean && make TARGET=linux
gcc ... -c ../ObjectAllocatorLib/src/object_allocator.c -o build/linux/obj/object_allocator.o
gcc ... -c ../SpatialQueryLib/src/spatial_hash.c -o build/linux/obj/spatial_hash.o
✓ Build successful! build/linux/particle_dynamics
```

### OpenParticleSurfaceLib
```
$ make clean && make
gcc ... -c ../ObjectAllocatorLib/src/object_allocator.c -o build/linux/obj/src/object_allocator.o
gcc ... -c ../SpatialQueryLib/src/spatial_hash.c -o build/linux/obj/src/spatial_hash.o
✓ Copied to ./open_particle_surface
```

### SurfaceLib
```
$ make clean && make
gcc -c ../ObjectAllocatorLib/src/object_allocator.c ... -o src/object_allocator.o
gcc -c ../SpatialQueryLib/src/spatial_hash.c ... -o src/spatial_hash.o
gcc -o surface_app ... [SUCCESS]
```

### GPURayTraceExample
```
$ make clean && make WSL_LINUX=1
gcc -c ../ObjectAllocatorLib/src/object_allocator.c ... -o build/linux/obj/object_allocator.o
✓ Copied to ./gpu_raytrace
```

All 6 projects (SpatialQueryLib, ObjectAllocatorLib, ParticleDynamicsExample, OpenParticleSurfaceLib, SurfaceLib, GPURayTraceExample) build cleanly. Both library test suites (13 tests total) pass.
