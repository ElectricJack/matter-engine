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

.PHONY: clean run-partv2 run-script run-graph run-graph-integration run-trivar run-shlib

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
	rm -f $(PARTV2_TARGET) $(SCRIPT_TARGET) $(GRAPH_TARGET) $(GRAPH_INT_TARGET) $(TRIVAR_TARGET) $(SHLIB_TARGET)

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
SCRIPT_CPP = script_host_tests.cpp ../src/script_host.cpp ../src/dsl_state.cpp \
             ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/part_asset_v2.cpp \
             ../../MatterSurfaceLib/src/cluster.cpp ../../MatterSurfaceLib/src/cell.cpp ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
             ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/bvh_analyzer.cpp \
             ../../MatterSurfaceLib/src/mesh_worker_pool.cpp ../../MatterSurfaceLib/src/mesh_build_utils.cpp \
             ../../MatterSurfaceLib/src/meshing_algorithm.cpp ../../MatterSurfaceLib/src/marching_cubes_algorithm.cpp \
             ../../MatterSurfaceLib/src/oriented_cube_algorithm.cpp ../../MatterSurfaceLib/src/vertex_ao.cpp \
             ../../MatterSurfaceLib/src/occupancy.cpp ../../MatterSurfaceLib/src/tlas_manager.cpp ../../MatterSurfaceLib/src/part_asset.cpp \
             ../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp
SCRIPT_C   = ../../MatterSurfaceLib/src/surface.c ../../MatterSurfaceLib/src/open_particle_surface.c \
             ../../MatterSurfaceLib/src/spatial_hash.c ../../MatterSurfaceLib/src/object_allocator.c ../../MatterSurfaceLib/src/material_registry.c
SCRIPT_C_OBJ = surface.o open_particle_surface.o spatial_hash.o object_allocator.o material_registry.o

$(SCRIPT_TARGET): $(SCRIPT_CPP) $(SCRIPT_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(SCRIPT_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(SCRIPT_CPP) $(QJS_OBJ) $(SCRIPT_C_OBJ) -o $(SCRIPT_TARGET) \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(SCRIPT_C_OBJ)

run-script: $(SCRIPT_TARGET)
	./$(SCRIPT_TARGET)

# SP-3 end-to-end install through the REAL FileModuleResolver/HostBaker adapters,
# which drive the SP-2 ScriptHost (eval_requires + resolve_hash + bake_source ->
# save_v2). Built with -DMATTER_HAVE_SCRIPT_HOST so the guarded adapters in
# part_graph.{h,cpp} compile, and links the full host/QuickJS source set (mirrors
# SCRIPT_CPP) plus ../src/part_graph.cpp. material_registry.c stays gcc-compiled.
GRAPH_INT_TARGET = part_graph_integration_tests
GRAPH_INT_CPP = part_graph_integration_tests.cpp ../src/part_graph.cpp \
                ../src/script_host.cpp ../src/dsl_state.cpp \
                ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/part_asset_v2.cpp \
                ../../MatterSurfaceLib/src/cluster.cpp ../../MatterSurfaceLib/src/cell.cpp ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
                ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/bvh_analyzer.cpp \
                ../../MatterSurfaceLib/src/mesh_worker_pool.cpp ../../MatterSurfaceLib/src/mesh_build_utils.cpp \
                ../../MatterSurfaceLib/src/meshing_algorithm.cpp ../../MatterSurfaceLib/src/marching_cubes_algorithm.cpp \
                ../../MatterSurfaceLib/src/oriented_cube_algorithm.cpp ../../MatterSurfaceLib/src/vertex_ao.cpp \
                ../../MatterSurfaceLib/src/occupancy.cpp ../../MatterSurfaceLib/src/tlas_manager.cpp ../../MatterSurfaceLib/src/part_asset.cpp \
                ../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp
GRAPH_INT_C   = ../../MatterSurfaceLib/src/surface.c ../../MatterSurfaceLib/src/open_particle_surface.c \
                ../../MatterSurfaceLib/src/spatial_hash.c ../../MatterSurfaceLib/src/object_allocator.c ../../MatterSurfaceLib/src/material_registry.c
GRAPH_INT_C_OBJ = surface.o open_particle_surface.o spatial_hash.o object_allocator.o material_registry.o

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
TRIVAR_CPP = triangle_variation_tests.cpp ../src/triangle_emit.cpp ../src/part_asset_v2.cpp \
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

# Shared script library: module-resolution + source-fold unit + JS-helper bake tests
# (headless, GL-free; ModuleResolver requires no QuickJS).
SHLIB_TARGET = shared_lib_tests
SHLIB_CPP = shared_lib_tests.cpp ../src/module_resolver.cpp ../src/part_asset_v2.cpp \
            ../../MatterSurfaceLib/src/part_asset.cpp \
            ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp \
            ../../MatterSurfaceLib/src/tlas_manager.cpp \
            ../../MatterSurfaceLib/src/vertex_ao.cpp ../../MatterSurfaceLib/src/occupancy.cpp
SHLIB_C   = ../../MatterSurfaceLib/src/material_registry.c
SHLIB_C_OBJ = material_registry.o

$(SHLIB_TARGET): $(SHLIB_CPP) $(SHLIB_C)
	gcc -c $(SHLIB_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(SHLIB_CPP) $(SHLIB_C_OBJ) -o $(SHLIB_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(SHLIB_C_OBJ)

run-shlib: $(SHLIB_TARGET)
	./$(SHLIB_TARGET)
