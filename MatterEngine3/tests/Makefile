# Makefile for MatterEngine3 tests.
# MatterEngine3 consumes the MatterSurfaceLib prototype READ-ONLY:
#   new headers via -I../include, consumed prototype headers via
#   -I../../MatterSurfaceLib/include, consumed prototype sources as
#   ../../MatterSurfaceLib/src/<dep>. raylib paths match MatterSurfaceLib/tests
#   (same directory depth).

# Detect platform
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Default to Linux settings
PLATFORM = PLATFORM_DESKTOP
LIBTYPE = STATIC
RAYLIB_RELEASE_PATH = ../../../Libraries/raylib
RAYLIB_PATH = $(RAYLIB_RELEASE_PATH)/src

ifeq ($(UNAME_S),Linux)
    PLATFORM = PLATFORM_DESKTOP
    GRAPHICS = GRAPHICS_API_OPENGL_33
endif

ifeq ($(UNAME_S),Darwin)
    PLATFORM = PLATFORM_DESKTOP
    GRAPHICS = GRAPHICS_API_OPENGL_33
    ifeq ($(UNAME_M),arm64)
        MACOS_VERSION = 11.0
    else
        MACOS_VERSION = 10.15
    endif
endif

# Compiler settings
CC = g++
CFLAGS = -std=c++17 -Wall -Wno-missing-braces -Wno-unused-variable -D$(PLATFORM) -D$(GRAPHICS)

ifeq ($(UNAME_S),Darwin)
    CFLAGS += -mmacosx-version-min=$(MACOS_VERSION)
    LDFLAGS = -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
else
    LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11
endif

# Include directories: NEW MatterEngine3 headers, then consumed MatterSurfaceLib
# headers, then raylib.
INCLUDE_PATHS = -I../include -I../../MatterSurfaceLib/include -I$(RAYLIB_PATH) -I../../Libraries/raylib/src

# Library paths (match MatterSurfaceLib/tests; same depth).
ifeq ($(UNAME_S),Linux)
    LDLIBS = ../../Libraries/raylib/build/linux/libraylib.a
else ifeq ($(UNAME_S),Darwin)
    LDLIBS = ../../Libraries/raylib/build/macos/libraylib.a
else
    LDLIBS = ../../Libraries/raylib/src/libraylib.a
endif

.PHONY: clean run-partv2 run-script run-graph run-graph-integration run-trivar run-shlib run-comp run-dev run-example run-gallery run-viewer-logic run-iso run-polytri run-treebake run-meadow run-meadow-check run-lighting run-flatten run-grasslod run-stressforest run-tilesetphysics run-tilesetcore

# Part Artifact v2 round-trip + resolved-hash + format-guard tests (headless, GL-free).
# NEW impl ../src/part_asset_v2.cpp + consumed MatterSurfaceLib backend sources.
# material_registry.c via gcc to keep its extern "C" symbols unmangled.
PARTV2_TARGET = part_asset_v2_tests
PARTV2_CPP = part_asset_v2_tests.cpp ../src/part_asset_v2.cpp \
             ../../MatterSurfaceLib/src/part_asset.cpp \
             ../../MatterSurfaceLib/src/blas_manager.cpp \
             ../../MatterSurfaceLib/src/bvh.cpp \
             ../../MatterSurfaceLib/src/tlas_manager.cpp \
             ../../MatterSurfaceLib/src/vertex_ao.cpp \
             ../../MatterSurfaceLib/src/occupancy.cpp
PARTV2_C   = ../../MatterSurfaceLib/src/material_registry.c
PARTV2_C_OBJ = material_registry.o

$(PARTV2_TARGET): $(PARTV2_CPP) $(PARTV2_C)
	gcc -c $(PARTV2_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(PARTV2_CPP) $(PARTV2_C_OBJ) -o $(PARTV2_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(PARTV2_C_OBJ)

run-partv2: $(PARTV2_TARGET)
	./$(PARTV2_TARGET)

clean:
	rm -f $(PARTV2_TARGET) $(SCRIPT_TARGET) $(GRAPH_TARGET) $(GRAPH_INT_TARGET) $(TRIVAR_TARGET) $(SHLIB_TARGET) $(COMP_TARGET) $(DEV_TARGET) $(EXAMPLE_TARGET) $(VIEWER_LOGIC_TARGET) $(TREEBAKE_TARGET) $(MEADOW_TARGET) $(MEADOWCHECK_TARGET) $(GRASSLOD_TARGET) $(STRESSFOREST_TARGET)

# Script host (QuickJS-ng) + DSL/CSG bake tests (headless, GL-free for the host;
# the BLAS surface path links raylib like the part-asset tests). QuickJS-ng C
# sources are compiled via gcc so their symbols stay unmangled for extern "C".
# v0.10.0 requires xsum.c (xsum_small_* symbols) in addition to the plan's core set.
QJS_DIR  = ../../Libraries/quickjs-ng
QJS_INC  = -I$(QJS_DIR)
QJS_C    = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c \
           $(QJS_DIR)/libunicode.c $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c
QJS_OBJ  = quickjs.o libregexp.o libunicode.o cutils.o xsum.o

SCRIPT_TARGET = script_host_tests
# NEW MatterEngine3 sources keep ../src/...; SP-1's NEW part_asset_v2.cpp (../src/)
# supplies save_v2/compute_resolved_hash/LodLevels. Consumed prototype sources are
# referenced read-only as ../../MatterSurfaceLib/src/<dep>.
SCRIPT_CPP = script_host_tests.cpp ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
             ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
             ../../MatterSurfaceLib/src/cluster.cpp ../../MatterSurfaceLib/src/cell.cpp ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
             ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/bvh_analyzer.cpp \
             ../../MatterSurfaceLib/src/mesh_worker_pool.cpp ../../MatterSurfaceLib/src/mesh_build_utils.cpp \
             ../../MatterSurfaceLib/src/meshing_algorithm.cpp ../../MatterSurfaceLib/src/marching_cubes_algorithm.cpp \
             ../../MatterSurfaceLib/src/oriented_cube_algorithm.cpp ../../MatterSurfaceLib/src/vertex_ao.cpp \
             ../../MatterSurfaceLib/src/occupancy.cpp ../../MatterSurfaceLib/src/tlas_manager.cpp ../../MatterSurfaceLib/src/part_asset.cpp \
             ../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp
SCRIPT_C   = ../../MatterSurfaceLib/src/surface.c ../../MatterSurfaceLib/src/open_particle_surface.c \
             ../../MatterSurfaceLib/src/fat_primitive.c \
             ../../MatterSurfaceLib/src/spatial_hash.c ../../MatterSurfaceLib/src/object_allocator.c ../../MatterSurfaceLib/src/material_registry.c
SCRIPT_C_OBJ = surface.o open_particle_surface.o fat_primitive.o spatial_hash.o object_allocator.o material_registry.o

$(SCRIPT_TARGET): $(SCRIPT_CPP) $(SCRIPT_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(SCRIPT_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(SCRIPT_CPP) $(QJS_OBJ) $(SCRIPT_C_OBJ) -o $(SCRIPT_TARGET) \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(SCRIPT_C_OBJ)

run-script: $(SCRIPT_TARGET)
	./$(SCRIPT_TARGET)

# Phase 1 typed iso-primitives + ordered CSG: drives the REAL surface.c field eval
# (via ProbeFieldScalar) from a lowered DslState buffer and compares against the
# csg_lowering analytic oracle. GL-free, no QuickJS. fat_primitive.c is the new MSL
# SDF dispatch; material_registry.c/object_allocator.c keep their extern "C" symbols
# unmangled via gcc.
ISO_TARGET = iso_primitive_tests
ISO_CPP = iso_primitive_tests.cpp field_probe.cpp \
          ../src/csg_lowering.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
          ../src/part_asset_v2.cpp \
          ../../MatterSurfaceLib/src/part_asset.cpp ../../MatterSurfaceLib/src/blas_manager.cpp \
          ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/tlas_manager.cpp \
          ../../MatterSurfaceLib/src/vertex_ao.cpp ../../MatterSurfaceLib/src/occupancy.cpp
ISO_C   = ../../MatterSurfaceLib/src/surface.c ../../MatterSurfaceLib/src/fat_primitive.c \
          ../../MatterSurfaceLib/src/spatial_hash.c ../../MatterSurfaceLib/src/object_allocator.c \
          ../../MatterSurfaceLib/src/material_registry.c
ISO_C_OBJ = surface.o fat_primitive.o spatial_hash.o object_allocator.o material_registry.o

$(ISO_TARGET): $(ISO_CPP) $(ISO_C)
	gcc -c $(ISO_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(ISO_CPP) $(ISO_C_OBJ) -o $(ISO_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(ISO_C_OBJ)

run-iso: $(ISO_TARGET)
	./$(ISO_TARGET)

# SP-3 end-to-end install through the REAL FileModuleResolver/HostBaker adapters,
# which drive the SP-2 ScriptHost (eval_requires + resolve_hash + bake_source ->
# save_v2). Built with -DMATTER_HAVE_SCRIPT_HOST so the guarded adapters in
# part_graph.{h,cpp} compile, and links the full host/QuickJS source set (mirrors
# SCRIPT_CPP) plus ../src/part_graph.cpp. material_registry.c stays gcc-compiled.
GRAPH_INT_TARGET = part_graph_integration_tests
GRAPH_INT_CPP = part_graph_integration_tests.cpp ../src/part_graph.cpp \
                ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
                ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
                ../../MatterSurfaceLib/src/cluster.cpp ../../MatterSurfaceLib/src/cell.cpp ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
                ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/bvh_analyzer.cpp \
                ../../MatterSurfaceLib/src/mesh_worker_pool.cpp ../../MatterSurfaceLib/src/mesh_build_utils.cpp \
                ../../MatterSurfaceLib/src/meshing_algorithm.cpp ../../MatterSurfaceLib/src/marching_cubes_algorithm.cpp \
                ../../MatterSurfaceLib/src/oriented_cube_algorithm.cpp ../../MatterSurfaceLib/src/vertex_ao.cpp \
                ../../MatterSurfaceLib/src/occupancy.cpp ../../MatterSurfaceLib/src/tlas_manager.cpp ../../MatterSurfaceLib/src/part_asset.cpp \
                ../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp
GRAPH_INT_C   = ../../MatterSurfaceLib/src/surface.c ../../MatterSurfaceLib/src/open_particle_surface.c \
                ../../MatterSurfaceLib/src/fat_primitive.c \
                ../../MatterSurfaceLib/src/spatial_hash.c ../../MatterSurfaceLib/src/object_allocator.c ../../MatterSurfaceLib/src/material_registry.c
GRAPH_INT_C_OBJ = surface.o open_particle_surface.o fat_primitive.o spatial_hash.o object_allocator.o material_registry.o

$(GRAPH_INT_TARGET): $(GRAPH_INT_CPP) $(GRAPH_INT_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(GRAPH_INT_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(GRAPH_INT_CPP) $(QJS_OBJ) $(GRAPH_INT_C_OBJ) -o $(GRAPH_INT_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(GRAPH_INT_C_OBJ)

run-graph-integration: $(GRAPH_INT_TARGET)
	./$(GRAPH_INT_TARGET)

# Part graph resolve/dedup/topo/cycle/reachability/cache-miss tests (headless, GL-free).
# NEW MatterEngine3 sources ../src/<new>.cpp + consumed MatterSurfaceLib backend.
GRAPH_TARGET = part_graph_tests
GRAPH_SOURCES = part_graph_tests.cpp ../src/part_graph.cpp ../src/part_asset_v2.cpp \
                ../../MatterSurfaceLib/src/part_asset.cpp \
                ../../MatterSurfaceLib/src/blas_manager.cpp \
                ../../MatterSurfaceLib/src/bvh.cpp \
                ../../MatterSurfaceLib/src/tlas_manager.cpp \
                ../../MatterSurfaceLib/src/vertex_ao.cpp \
                ../../MatterSurfaceLib/src/occupancy.cpp
GRAPH_C   = ../../MatterSurfaceLib/src/material_registry.c
GRAPH_C_OBJ = material_registry.o

$(GRAPH_TARGET): $(GRAPH_SOURCES) $(GRAPH_C)
	gcc -c $(GRAPH_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(GRAPH_SOURCES) $(GRAPH_C_OBJ) -o $(GRAPH_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(GRAPH_C_OBJ)

run-graph: $(GRAPH_TARGET)
	./$(GRAPH_TARGET)

# Direct-triangle emission + line tuber + variation dedup tests (headless, GL-free).
# NEW impl ../src/triangle_emit.cpp + SP-1's ../src/part_asset_v2.cpp (for the
# real part_asset::compute_resolved_hash / ChildInstance used by VariationRecorder)
# + consumed MatterSurfaceLib backend sources (read-only). material_registry.c is
# compiled via gcc so its extern "C" symbols stay unmangled.
TRIVAR_TARGET = triangle_variation_tests
TRIVAR_CPP = triangle_variation_tests.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp ../src/part_asset_v2.cpp \
             ../../MatterSurfaceLib/src/part_asset.cpp \
             ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp \
             ../../MatterSurfaceLib/src/tlas_manager.cpp \
             ../../MatterSurfaceLib/src/vertex_ao.cpp ../../MatterSurfaceLib/src/occupancy.cpp
TRIVAR_C   = ../../MatterSurfaceLib/src/material_registry.c
TRIVAR_C_OBJ = material_registry.o

$(TRIVAR_TARGET): $(TRIVAR_CPP) $(TRIVAR_C)
	gcc -c $(TRIVAR_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(TRIVAR_CPP) $(TRIVAR_C_OBJ) -o $(TRIVAR_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(TRIVAR_C_OBJ)

run-trivar: $(TRIVAR_TARGET)
	./$(TRIVAR_TARGET)

# Phase 3 pure ear-clipping triangulator (holes + concave). Standalone, no MSL/
# raylib/QuickJS deps -> a fast unit test of polygon_triangulate.{hpp,cpp} alone.
POLYTRI_TARGET = polygon_triangulate_tests
POLYTRI_CPP = polygon_triangulate_tests.cpp ../src/polygon_triangulate.cpp

$(POLYTRI_TARGET): $(POLYTRI_CPP)
	$(CC) $(POLYTRI_CPP) -o $(POLYTRI_TARGET) $(CFLAGS) -I../include

run-polytri: $(POLYTRI_TARGET)
	./$(POLYTRI_TARGET)

# Shared script library: module-resolution + source-fold unit + JS-helper bake tests
# PLUS the SP-7 end-to-end bake test (-DSP2_SCRIPT_HOST), which drives the real
# SP-2 ScriptHost (set_shared_lib_root -> fold-into-hash + QuickJS module loader).
# That end-to-end path links the full host + QuickJS source set (mirrors SCRIPT_CPP
# + QJS_OBJ + SCRIPT_C). QuickJS-ng C sources are gcc-compiled so their extern "C"
# symbols stay unmangled; material_registry.c likewise.
SHLIB_TARGET = shared_lib_tests
SHLIB_CPP = shared_lib_tests.cpp ../src/module_resolver.cpp ../src/script_rng_binding.cpp \
            ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp ../src/dsl_bindings.cpp ../src/csg_lowering.cpp \
            ../src/part_asset_v2.cpp \
            ../../MatterSurfaceLib/src/cluster.cpp ../../MatterSurfaceLib/src/cell.cpp ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
            ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/bvh_analyzer.cpp \
            ../../MatterSurfaceLib/src/mesh_worker_pool.cpp ../../MatterSurfaceLib/src/mesh_build_utils.cpp \
            ../../MatterSurfaceLib/src/meshing_algorithm.cpp ../../MatterSurfaceLib/src/marching_cubes_algorithm.cpp \
            ../../MatterSurfaceLib/src/oriented_cube_algorithm.cpp ../../MatterSurfaceLib/src/vertex_ao.cpp \
            ../../MatterSurfaceLib/src/occupancy.cpp ../../MatterSurfaceLib/src/tlas_manager.cpp ../../MatterSurfaceLib/src/part_asset.cpp \
            ../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp
SHLIB_C   = ../../MatterSurfaceLib/src/surface.c ../../MatterSurfaceLib/src/open_particle_surface.c \
            ../../MatterSurfaceLib/src/spatial_hash.c ../../MatterSurfaceLib/src/object_allocator.c ../../MatterSurfaceLib/src/material_registry.c
SHLIB_C_OBJ = surface.o open_particle_surface.o spatial_hash.o object_allocator.o material_registry.o

$(SHLIB_TARGET): $(SHLIB_CPP) $(SHLIB_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(SHLIB_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(SHLIB_CPP) $(QJS_OBJ) $(SHLIB_C_OBJ) -o $(SHLIB_TARGET) \
	      $(CFLAGS) -DSP2_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(SHLIB_C_OBJ)

run-shlib: $(SHLIB_TARGET)
	./$(SHLIB_TARGET)

# Composition-to-world (SP-4): LOD bake + flatten + sector grid + LOD select.
# Headless, GL-free CPU steps. raylib linked only for MemAlloc/MemFree in the
# Tri<->Mesh bridge. NEW impl ../src/*.cpp + consumed MatterSurfaceLib backend.
# Includes SP-1's part_asset_v2.cpp for the save_v2/load_v2 LOD round-trip (Task 3);
# material_registry.c via gcc to keep its extern "C" symbols unmangled.
COMP_TARGET = composition_tests
COMP_CPP = composition_tests.cpp ../src/lod_bake.cpp ../src/world_flatten.cpp \
           ../src/sector_grid.cpp ../src/lod_select.cpp ../src/part_asset_v2.cpp \
           ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
           ../../MatterSurfaceLib/src/blas_manager.cpp \
           ../../MatterSurfaceLib/src/bvh.cpp \
           ../../MatterSurfaceLib/src/tlas_manager.cpp \
           ../../MatterSurfaceLib/src/part_asset.cpp \
           ../../MatterSurfaceLib/src/vertex_ao.cpp \
           ../../MatterSurfaceLib/src/occupancy.cpp
COMP_C   = ../../MatterSurfaceLib/src/material_registry.c
COMP_C_OBJ = material_registry.o

$(COMP_TARGET): $(COMP_CPP) $(COMP_C)
	gcc -c $(COMP_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(COMP_CPP) $(COMP_C_OBJ) -o $(COMP_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(COMP_C_OBJ)

run-comp: $(COMP_TARGET)
	./$(COMP_TARGET)

# Bake-time subtree flattening + error-bounded LOD ladder (headless, GL-free;
# raylib linked only for MemAlloc/MemFree in the Tri<->Mesh bridge, like COMP).
FLATTEN_TARGET = part_flatten_tests
FLATTEN_CPP = part_flatten_tests.cpp ../src/part_flatten.cpp ../src/lod_bake.cpp \
              ../src/part_cluster.cpp \
              ../src/part_asset_v2.cpp \
              ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
              ../../MatterSurfaceLib/src/blas_manager.cpp \
              ../../MatterSurfaceLib/src/bvh.cpp \
              ../../MatterSurfaceLib/src/tlas_manager.cpp \
              ../../MatterSurfaceLib/src/part_asset.cpp \
              ../../MatterSurfaceLib/src/vertex_ao.cpp \
              ../../MatterSurfaceLib/src/occupancy.cpp
FLATTEN_C   = ../../MatterSurfaceLib/src/material_registry.c
FLATTEN_C_OBJ = material_registry.o

$(FLATTEN_TARGET): $(FLATTEN_CPP) $(FLATTEN_C)
	gcc -c $(FLATTEN_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(FLATTEN_CPP) $(FLATTEN_C_OBJ) -o $(FLATTEN_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(FLATTEN_C_OBJ)

run-flatten: $(FLATTEN_TARGET)
	./$(FLATTEN_TARGET)

# SP-5 dev live-edit: debounce + upward-cone scoping + last-good (headless, GL-free).
# All-new MatterEngine3 sources; inotify_watcher.cpp is Linux-only (guarded by __linux__ internally).
DEV_TARGET = dev_live_edit_tests
DEV_SOURCES = dev_live_edit_tests.cpp ../src/live_edit.cpp ../src/inotify_watcher.cpp

$(DEV_TARGET): $(DEV_SOURCES)
	$(CC) $(DEV_SOURCES) -o $(DEV_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-dev: $(DEV_TARGET)
	./$(DEV_TARGET)

# End-to-end example world (terrain + trees + grass): drives the FULL pipeline on
# committed assets under ../examples/world_demo. Links the SP-2 host + QuickJS set
# (like the integration test, -DMATTER_HAVE_SCRIPT_HOST) PLUS the SP-4 composition
# sources (lod_bake/world_flatten/sector_grid/lod_select). script_rng_binding.cpp
# is included for the host's seeded Math.random. Must run from this tests/ dir so
# the driver's relative asset paths resolve. material_registry.c + QuickJS C
# sources are gcc-compiled to keep their extern "C" symbols unmangled.
EXAMPLE_TARGET = example_world
EXAMPLE_CPP = example_world.cpp ../src/part_graph.cpp \
              ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp ../src/dsl_bindings.cpp \
              ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/script_rng_binding.cpp \
              ../src/part_asset_v2.cpp ../src/lod_bake.cpp ../src/world_flatten.cpp ../src/part_flatten.cpp \
              ../src/part_cluster.cpp \
              ../src/sector_grid.cpp ../src/lod_select.cpp \
              ../../MatterSurfaceLib/src/cluster.cpp ../../MatterSurfaceLib/src/cell.cpp ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
              ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/bvh_analyzer.cpp \
              ../../MatterSurfaceLib/src/mesh_worker_pool.cpp ../../MatterSurfaceLib/src/mesh_build_utils.cpp \
              ../../MatterSurfaceLib/src/meshing_algorithm.cpp ../../MatterSurfaceLib/src/marching_cubes_algorithm.cpp \
              ../../MatterSurfaceLib/src/oriented_cube_algorithm.cpp ../../MatterSurfaceLib/src/vertex_ao.cpp \
              ../../MatterSurfaceLib/src/occupancy.cpp ../../MatterSurfaceLib/src/tlas_manager.cpp ../../MatterSurfaceLib/src/part_asset.cpp \
              ../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp
EXAMPLE_C   = ../../MatterSurfaceLib/src/surface.c ../../MatterSurfaceLib/src/open_particle_surface.c \
              ../../MatterSurfaceLib/src/spatial_hash.c ../../MatterSurfaceLib/src/object_allocator.c ../../MatterSurfaceLib/src/material_registry.c \
              ../../MatterSurfaceLib/src/fat_primitive.c
EXAMPLE_C_OBJ = surface.o open_particle_surface.o spatial_hash.o object_allocator.o material_registry.o fat_primitive.o

$(EXAMPLE_TARGET): $(EXAMPLE_CPP) $(EXAMPLE_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(EXAMPLE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(EXAMPLE_CPP) $(QJS_OBJ) $(EXAMPLE_C_OBJ) -o $(EXAMPLE_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(EXAMPLE_C_OBJ)

run-example: $(EXAMPLE_TARGET)
	./$(EXAMPLE_TARGET)

# Headless bake smoke test for the primitive test world (examples/primitive_demo).
# Reuses the full example_world pipeline source set (minus that driver) and points
# it at the Gallery manifest; asserts every baked artifact round-trips with
# geometry. Must run from this tests/ dir so the relative asset paths resolve.
GALLERY_TARGET = gallery_bake_tests
GALLERY_CPP = gallery_bake_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))
$(GALLERY_TARGET): $(GALLERY_CPP) $(EXAMPLE_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(EXAMPLE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(GALLERY_CPP) $(QJS_OBJ) $(EXAMPLE_C_OBJ) -o $(GALLERY_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(EXAMPLE_C_OBJ)

run-gallery: $(GALLERY_TARGET)
	./$(GALLERY_TARGET)

# Headless bake check for the world_demo Tree (Trunk/Tree split validation).
TREEBAKE_TARGET = tree_bake_check
TREEBAKE_CPP = tree_bake_check.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))
$(TREEBAKE_TARGET): $(TREEBAKE_CPP) $(EXAMPLE_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(EXAMPLE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(TREEBAKE_CPP) $(QJS_OBJ) $(EXAMPLE_C_OBJ) -o $(TREEBAKE_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(EXAMPLE_C_OBJ)

run-treebake: $(TREEBAKE_TARGET)
	./$(TREEBAKE_TARGET)

# Headless bake tests for the Meadow schemas (terrain noise seam, rock/pebble/
# grass variants). Fresh sandbox per run.
MEADOW_TARGET = meadow_bake_tests
MEADOW_CPP = meadow_bake_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))
$(MEADOW_TARGET): $(MEADOW_CPP) $(EXAMPLE_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(EXAMPLE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(MEADOW_CPP) $(QJS_OBJ) $(EXAMPLE_C_OBJ) -o $(MEADOW_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(EXAMPLE_C_OBJ)

run-meadow: $(MEADOW_TARGET)
	./$(MEADOW_TARGET)

# Full Meadow world bake check (persistent sandbox; first run bakes ~276 variants).
MEADOWCHECK_TARGET = meadow_bake_check
MEADOWCHECK_CPP = meadow_bake_check.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))
$(MEADOWCHECK_TARGET): $(MEADOWCHECK_CPP) $(EXAMPLE_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(EXAMPLE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(MEADOWCHECK_CPP) $(QJS_OBJ) $(EXAMPLE_C_OBJ) -o $(MEADOWCHECK_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(EXAMPLE_C_OBJ)

run-meadow-check: $(MEADOWCHECK_TARGET)
	./$(MEADOWCHECK_TARGET)

# --- Viewer GL-free logic suite (WorldState, resolvers, PartStore, LocalProvider).
# Reuses the example_world pipeline source set (SP-2 host + QuickJS + SP-4 compose
# + MSL backend) and adds the viewer's GL-free units. Same dir depth as tests/, so
# every ../ / ../../ path resolves identically. NOT a GL build (no renderer/ui/main).
VIEWER_LOGIC_TARGET = viewer_logic_tests
# Pipeline sources = EXAMPLE_CPP minus the example_world.cpp driver. Using
# filter-out keeps this in sync if EXAMPLE_CPP gains/loses a source.
VIEWER_PIPELINE_CPP = $(filter-out example_world.cpp, $(EXAMPLE_CPP))
VIEWER_LOGIC_CPP = viewer_logic_tests.cpp \
              ../viewer/world_state.cpp \
              ../viewer/resolvers.cpp \
              ../viewer/part_store.cpp \
              ../viewer/local_provider.cpp \
              ../viewer/world_composer.cpp \
              ../viewer/raster_mesh.cpp \
              ../src/world_lights.cpp ../src/probe_volume.cpp \
              ../src/world_tracer.cpp ../src/probe_bake.cpp \
              $(VIEWER_PIPELINE_CPP)
# raster_composer.cpp is intentionally NOT linked: after Task 12 it depends on
# GpuCuller (GL) via draw_gpu_driven, so it can't compile in the GL-free tests
# path. The CPU batch tests it used to exercise (build_batches / fingerprint /
# mesh_cache) were deleted together with the CPU raster path; GPU parity of the
# per-cluster cull is covered by gpu_cull_tests in viewer/gpu_tests.

$(VIEWER_LOGIC_TARGET): $(VIEWER_LOGIC_CPP) $(EXAMPLE_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(EXAMPLE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(VIEWER_LOGIC_CPP) $(QJS_OBJ) $(EXAMPLE_C_OBJ) -o $(VIEWER_LOGIC_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST -pthread $(INCLUDE_PATHS) -I../viewer $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(EXAMPLE_C_OBJ)

run-viewer-logic: $(VIEWER_LOGIC_TARGET)
	./$(VIEWER_LOGIC_TARGET)

# Tasks 1-3 — World light list, ProbeVolume, WorldTracer.
# GL-free, no QuickJS. Mirrors the FLATTEN stanza dep set plus world_tracer.cpp
# and part_asset_v2.cpp so the tracer's load_v2 round-trip compiles.
LIGHTING_TARGET = lighting_tests
LIGHTING_SOURCES = lighting_tests.cpp ../src/world_lights.cpp ../src/probe_volume.cpp \
                   ../src/world_tracer.cpp ../src/probe_bake.cpp \
                   ../src/part_graph.cpp ../src/part_asset_v2.cpp \
                   ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
                   ../../MatterSurfaceLib/src/blas_manager.cpp \
                   ../../MatterSurfaceLib/src/bvh.cpp \
                   ../../MatterSurfaceLib/src/tlas_manager.cpp \
                   ../../MatterSurfaceLib/src/part_asset.cpp \
                   ../../MatterSurfaceLib/src/vertex_ao.cpp \
                   ../../MatterSurfaceLib/src/occupancy.cpp
LIGHTING_C   = ../../MatterSurfaceLib/src/material_registry.c
LIGHTING_C_OBJ = material_registry.o

$(LIGHTING_TARGET): $(LIGHTING_SOURCES) $(LIGHTING_C)
	gcc -c $(LIGHTING_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(LIGHTING_SOURCES) $(LIGHTING_C_OBJ) -o $(LIGHTING_TARGET) $(CFLAGS) -pthread $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(LIGHTING_C_OBJ)

run-lighting: $(LIGHTING_TARGET)
	./$(LIGHTING_TARGET)

# Grass.js prefix-subset triangle-budget LOD contract (Stage 3).
# Bakes Grass.js at several lodBudget fractions and verifies:
#   (1) per-level tri count == 3*ceil(k*BLADES)
#   (2) blade TIP vertices are a strict prefix subset of full-budget tips
#   (3) two separate cache dirs produce the same resolved hash + byte-identical files
#   (4) TriEx (tint) populated at every budget level
# Mirrors the TREEBAKE_TARGET stanza: same objects, -DMATTER_HAVE_SCRIPT_HOST,
# filter-out example_world.cpp, shared-lib root ../examples/world_demo/shared-lib.
GRASSLOD_TARGET = grass_lod_tests
GRASSLOD_CPP = grass_lod_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))
$(GRASSLOD_TARGET): $(GRASSLOD_CPP) $(EXAMPLE_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(EXAMPLE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(GRASSLOD_CPP) $(QJS_OBJ) $(EXAMPLE_C_OBJ) -o $(GRASSLOD_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(EXAMPLE_C_OBJ)

run-grasslod: $(GRASSLOD_TARGET)
	./$(GRASSLOD_TARGET)

# Stage-4 stress fixture scatter determinism (StressForest50k.js): two separate
# cache dirs must produce the same resolved hash, byte-identical .part files,
# and an identical FNV-1a hash over the flattened placement stream. Mirrors the
# GRASSLOD stanza (full host + QuickJS set, -DMATTER_HAVE_SCRIPT_HOST).
STRESSFOREST_TARGET = stress_forest_tests
STRESSFOREST_CPP = stress_forest_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))
$(STRESSFOREST_TARGET): $(STRESSFOREST_CPP) $(EXAMPLE_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(EXAMPLE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(STRESSFOREST_CPP) $(QJS_OBJ) $(EXAMPLE_C_OBJ) -o $(STRESSFOREST_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(EXAMPLE_C_OBJ)

run-stressforest: $(STRESSFOREST_TARGET)
	./$(STRESSFOREST_TARGET)

# Box3d physics smoke test: dropping cube on static slab, verifying settle and sleep.
BOX3D_DIR = ../../Libraries/box3d

tileset_physics_tests: tileset_physics_tests.cpp $(BOX3D_DIR)/libbox3d.a
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I$(BOX3D_DIR)/include \
	    -o $@ tileset_physics_tests.cpp $(BOX3D_DIR)/libbox3d.a -lm

$(BOX3D_DIR)/libbox3d.a:
	$(MAKE) -C $(BOX3D_DIR)

run-tilesetphysics: tileset_physics_tests
	./tileset_physics_tests

tileset_core_tests: tileset_core_tests.cpp ../src/tileset_layout.cpp ../src/tileset_collider.cpp
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include \
	    -o $@ tileset_core_tests.cpp ../src/tileset_layout.cpp ../src/tileset_collider.cpp

run-tilesetcore: tileset_core_tests
	./tileset_core_tests
