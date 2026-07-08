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
INCLUDE_PATHS = -I../include -I../src -I../src/render -I../src/provider -I../../MatterSurfaceLib/include -I$(RAYLIB_PATH) -I../../Libraries/raylib/src

# Library paths (match MatterSurfaceLib/tests; same depth).
ifeq ($(UNAME_S),Linux)
    LDLIBS = ../../Libraries/raylib/build/linux/libraylib.a
else ifeq ($(UNAME_S),Darwin)
    LDLIBS = ../../Libraries/raylib/build/macos/libraylib.a
else
    LDLIBS = ../../Libraries/raylib/src/libraylib.a
endif

# ---------------------------------------------------------------------------
# Shared MatterSurfaceLib source sets (eliminate copy-paste across ~20 targets).
#
# COMMON_MSL_BLAS_SRC — minimal BLAS/TLAS backend; used by targets that only
#   need asset round-trip / BVH (no full mesher): partv2, graph, comp, flatten,
#   lighting, tilesettorusbvh, tilesetmeadowmanifest.
#
# COMMON_MSL_FULL_SRC — full mesher + BLAS/TLAS; used by targets that invoke
#   the marching-cubes / mesh-build pipeline: script, graph-integration, shlib,
#   example, gallery, treebake, meadow, meadow-check, grasslod, stressforest,
#   viewer-logic, tilesetbake.
#
# COMMON_MSL_C     — full C-source set (surface.c et al.) used alongside
#   COMMON_MSL_FULL_SRC; compiled via gcc to keep extern "C" symbols unmangled.
# COMMON_MSL_C_OBJ — the resulting .o list.
# ---------------------------------------------------------------------------
MSL = ../../MatterSurfaceLib/src

COMMON_MSL_BLAS_SRC = \
    $(MSL)/blas_manager.cpp \
    $(MSL)/bvh.cpp \
    $(MSL)/tlas_manager.cpp \
    $(MSL)/vertex_ao.cpp \
    $(MSL)/occupancy.cpp \
    $(MSL)/part_asset.cpp

COMMON_MSL_FULL_SRC = \
    $(COMMON_MSL_BLAS_SRC) \
    $(MSL)/cluster.cpp \
    $(MSL)/cell.cpp \
    $(MSL)/mesh_simplifier.cpp \
    $(MSL)/mesh_indexed.cpp \
    $(MSL)/bvh_analyzer.cpp \
    $(MSL)/mesh_worker_pool.cpp \
    $(MSL)/mesh_build_utils.cpp \
    $(MSL)/meshing_algorithm.cpp \
    $(MSL)/marching_cubes_algorithm.cpp \
    $(MSL)/oriented_cube_algorithm.cpp \
    $(MSL)/lattice.cpp \
    $(MSL)/particle_culling.cpp

COMMON_MSL_C = \
    $(MSL)/surface.c \
    $(MSL)/open_particle_surface.c \
    $(MSL)/fat_primitive.c \
    $(MSL)/spatial_hash.c \
    $(MSL)/object_allocator.c \
    $(MSL)/material_registry.c
COMMON_MSL_C_OBJ = surface.o open_particle_surface.o fat_primitive.o \
                   spatial_hash.o object_allocator.o material_registry.o

.PHONY: clean run-partv2 run-script run-graph run-graph-integration run-trivar run-shlib run-comp run-dev run-example run-gallery run-viewer-logic run-iso run-polytri run-treebake run-meadow run-meadow-check run-lighting run-flatten run-retopo-integration run-grasslod run-stressforest run-tilesetphysics run-tilesetcore run-tilesetplacement run-tilesetdsl run-tilesetbake run-tilesetgtex run-tilesettorusbvh run-tilesetmeadowmanifest run-shader-source run-asyncq gpu-tests tileset-gpu-tests tileset-seam-tests tileset-provider-tests tileset-load-tests api-tests run-tilesetgpu run-tilesetseam run-tilesetprovider run-tilesetload run-api-tests

# Part Artifact v2 round-trip + resolved-hash + format-guard tests (headless, GL-free).
# NEW impl ../src/part_asset_v2.cpp + consumed MatterSurfaceLib backend sources.
# material_registry.c via gcc to keep its extern "C" symbols unmangled.
PARTV2_TARGET = part_asset_v2_tests
PARTV2_CPP = part_asset_v2_tests.cpp ../src/part_asset_v2.cpp \
             $(COMMON_MSL_BLAS_SRC)
PARTV2_C   = $(MSL)/material_registry.c
PARTV2_C_OBJ = material_registry.o

$(PARTV2_TARGET): $(PARTV2_CPP) $(PARTV2_C)
	gcc -c $(PARTV2_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(PARTV2_CPP) $(PARTV2_C_OBJ) -o $(PARTV2_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(PARTV2_C_OBJ)

run-partv2: $(PARTV2_TARGET)
	./$(PARTV2_TARGET)

clean:
	rm -f $(PARTV2_TARGET) $(SCRIPT_TARGET) $(GRAPH_TARGET) $(GRAPH_INT_TARGET) $(TRIVAR_TARGET) $(SHLIB_TARGET) $(COMP_TARGET) $(DEV_TARGET) $(EXAMPLE_TARGET) $(VIEWER_LOGIC_TARGET) $(TREEBAKE_TARGET) $(MEADOW_TARGET) $(MEADOWCHECK_TARGET) $(GRASSLOD_TARGET) $(STRESSFOREST_TARGET) $(TILESETMEADOWMANIFEST_TARGET) $(RETOPO_INT_TARGET) $(SHADER_SOURCE_TARGET)

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
             ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
             $(COMMON_MSL_FULL_SRC)
SCRIPT_C   = $(COMMON_MSL_C)
SCRIPT_C_OBJ = $(COMMON_MSL_C_OBJ)

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
          $(COMMON_MSL_BLAS_SRC)
ISO_C   = $(MSL)/surface.c $(MSL)/fat_primitive.c \
          $(MSL)/spatial_hash.c $(MSL)/object_allocator.c \
          $(MSL)/material_registry.c
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
                ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
                $(COMMON_MSL_FULL_SRC)
GRAPH_INT_C   = $(COMMON_MSL_C)
GRAPH_INT_C_OBJ = $(COMMON_MSL_C_OBJ)

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
                $(COMMON_MSL_BLAS_SRC)
GRAPH_C   = $(MSL)/material_registry.c
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
             $(COMMON_MSL_BLAS_SRC)
TRIVAR_C   = $(MSL)/material_registry.c
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
            ../src/part_asset_v2.cpp ../src/lod_bake.cpp ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
            $(COMMON_MSL_FULL_SRC)
SHLIB_C   = $(COMMON_MSL_C)
SHLIB_C_OBJ = $(COMMON_MSL_C_OBJ)

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
           $(MSL)/mesh_simplifier.cpp \
           $(MSL)/mesh_indexed.cpp \
           $(COMMON_MSL_BLAS_SRC)
COMP_C   = $(MSL)/material_registry.c
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
              ../src/retopo_blacklist.cpp \
              $(MSL)/mesh_simplifier.cpp \
              $(MSL)/mesh_indexed.cpp \
              $(MSL)/mesh_transform.cpp \
              $(COMMON_MSL_BLAS_SRC)
FLATTEN_C   = $(MSL)/material_registry.c
FLATTEN_C_OBJ = material_registry.o

$(FLATTEN_TARGET): $(FLATTEN_CPP) $(FLATTEN_C)
	gcc -c $(FLATTEN_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(FLATTEN_CPP) $(FLATTEN_C_OBJ) -o $(FLATTEN_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(FLATTEN_C_OBJ)

run-flatten: $(FLATTEN_TARGET)
	./$(FLATTEN_TARGET)

# Phase 5 Task 14 — end-to-end retopo integration test. Same dep set as the
# FLATTEN target (this test drives flatten_part) PLUS:
#   * -DMATTER_HAVE_AUTOREMESHER so part_flatten.cpp compiles the retopo hook
#     path (apply_retopo_hook + compute_retopo_cache_key + friends).
#   * MSL's mesh_retopo.cpp + mesh_transform.cpp source files (retopo() +
#     from_tri/to_tri live in MSL under the same -DMATTER_HAVE_AUTOREMESHER
#     gate as part_flatten's hook).
#   * autoremesher_core header include path (for autoremesher/remesh.h /
#     AUTOREMESHER_CORE_VERSION), the static lib, and the TBB shared lib +
#     $$ORIGIN-relative rpath so libtbb.so.2 is found without LD_LIBRARY_PATH.
#     The rpath pattern mirrors MSL/tests/Makefile's RETOPO_TARGET stanza and
#     Libraries/autoremesher_core/tests/Makefile (Tasks 6, 10).
# Runtime prerequisite: libautoremesher_core.a and TBB must be built —
# build-all.sh handles this. If the library is missing, the link step fails
# with a clear "-lautoremesher_core not found" error.
AR_CORE_PATH = ../../Libraries/autoremesher_core
TBB_BUILD_DIR := $(firstword $(wildcard $(AR_CORE_PATH)/thirdparty/tbb/build/linux_*_release))
TBB_BUILD_SUBDIR := $(patsubst $(AR_CORE_PATH)/thirdparty/tbb/build/%,%,$(TBB_BUILD_DIR))

RETOPO_INT_TARGET = retopo_integration_tests
RETOPO_INT_CPP = retopo_integration_tests.cpp ../src/part_flatten.cpp ../src/lod_bake.cpp \
                 ../src/part_cluster.cpp \
                 ../src/part_asset_v2.cpp \
                 ../src/retopo_blacklist.cpp \
                 ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
                 ../../MatterSurfaceLib/src/mesh_indexed.cpp \
                 ../../MatterSurfaceLib/src/mesh_transform.cpp \
                 ../../MatterSurfaceLib/src/mesh_retopo.cpp \
                 ../../MatterSurfaceLib/src/blas_manager.cpp \
                 ../../MatterSurfaceLib/src/bvh.cpp \
                 ../../MatterSurfaceLib/src/tlas_manager.cpp \
                 ../../MatterSurfaceLib/src/part_asset.cpp \
                 ../../MatterSurfaceLib/src/vertex_ao.cpp \
                 ../../MatterSurfaceLib/src/occupancy.cpp
RETOPO_INT_C   = ../../MatterSurfaceLib/src/material_registry.c
RETOPO_INT_C_OBJ = material_registry.o
RETOPO_INT_CFLAGS  = $(CFLAGS) -DMATTER_HAVE_AUTOREMESHER -I$(AR_CORE_PATH)/include
# --allow-multiple-definition: raylib bundles stb_image_write.h in rtextures.c
# and autoremesher_core's geogram bundles its own copy in
# image_serializer_stb.o. Both archives get pulled in here — raylib because
# MSL's mesh_simplifier calls MemAlloc/MemFree, autoremesher_core because
# apply_retopo_hook references autoremesher::retopo. The stb sources are
# identical vendor copies so picking either definition is safe. This is a
# link-order-only fix; the compiled objects are byte-equivalent in practice.
RETOPO_INT_LDFLAGS = $(LDFLAGS) \
                     -L$(AR_CORE_PATH) -lautoremesher_core \
                     -L$(TBB_BUILD_DIR) -ltbb \
                     -Wl,-rpath,'$$ORIGIN/../../Libraries/autoremesher_core/thirdparty/tbb/build/$(TBB_BUILD_SUBDIR)' \
                     -Wl,--allow-multiple-definition

$(RETOPO_INT_TARGET): $(RETOPO_INT_CPP) $(RETOPO_INT_C)
	gcc -c $(RETOPO_INT_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(RETOPO_INT_CPP) $(RETOPO_INT_C_OBJ) -o $(RETOPO_INT_TARGET) \
	      $(RETOPO_INT_CFLAGS) $(INCLUDE_PATHS) $(RETOPO_INT_LDFLAGS) $(LDLIBS)
	rm -f $(RETOPO_INT_C_OBJ)

run-retopo-integration: $(RETOPO_INT_TARGET)
	./$(RETOPO_INT_TARGET)

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
              ../src/retopo_blacklist.cpp \
              ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
              ../src/part_cluster.cpp \
              ../src/sector_grid.cpp ../src/lod_select.cpp \
              $(MSL)/mesh_transform.cpp \
              $(COMMON_MSL_FULL_SRC)
EXAMPLE_C   = $(COMMON_MSL_C)
EXAMPLE_C_OBJ = $(COMMON_MSL_C_OBJ)

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
              ../src/async_bake.cpp \
              ../src/render/world_state.cpp \
              ../src/provider/resolvers.cpp \
              ../src/render/part_store.cpp \
              ../src/provider/local_provider.cpp \
              ../src/render/world_composer.cpp \
              ../src/render/raster_mesh.cpp \
              ../src/render/tileset_provider.cpp ../src/render/tileset_gl_ctx.cpp \
              ../src/tileset_phase.cpp ../src/tileset_phase_gpu.cpp \
              ../src/tileset_bake_primary.cpp ../src/tileset_bake_ao.cpp \
              ../src/tileset_settle.cpp ../src/tileset_gtex.cpp \
              ../src/tileset_bake.cpp ../src/tileset_bake_gpu.cpp ../src/tileset_collider.cpp \
              ../src/tileset_part_collider.cpp ../src/tileset_torus_bvh.cpp \
              ../src/world_lights.cpp ../src/probe_volume.cpp \
              ../src/world_tracer.cpp ../src/probe_bake.cpp \
              ../src/shader_source.cpp \
              $(VIEWER_PIPELINE_CPP)
# raster_composer.cpp is intentionally NOT linked: after Task 12 it depends on
# GpuCuller (GL) via draw_gpu_driven, so it can't compile in the GL-free tests
# path. The CPU batch tests it used to exercise (build_batches / fingerprint /
# mesh_cache) were deleted together with the CPU raster path; GPU parity of the
# per-cluster cull is covered by gpu_cull_tests in viewer/gpu_tests.

../shaders_gen/embedded_shaders.h:
	$(MAKE) -C .. shaders_gen/embedded_shaders.h

$(VIEWER_LOGIC_TARGET): $(VIEWER_LOGIC_CPP) $(EXAMPLE_C) $(QJS_C) | ../shaders_gen/embedded_shaders.h
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(EXAMPLE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(VIEWER_LOGIC_CPP) $(QJS_OBJ) $(EXAMPLE_C_OBJ) -o $(VIEWER_LOGIC_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB -pthread $(INCLUDE_PATHS) \
	      -I../../Libraries/box3d/include $(QJS_INC) $(LDFLAGS) $(LDLIBS) ../../Libraries/box3d/libbox3d.a
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
                   $(MSL)/mesh_simplifier.cpp \
                   $(COMMON_MSL_BLAS_SRC)
LIGHTING_C   = $(MSL)/material_registry.c
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

tileset_physics_tests: tileset_physics_tests.cpp ../src/tileset_settle.cpp ../src/tileset_collider.cpp $(BOX3D_DIR)/libbox3d.a
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include -I$(BOX3D_DIR)/include \
	    -o $@ tileset_physics_tests.cpp ../src/tileset_settle.cpp ../src/tileset_collider.cpp \
	    $(BOX3D_DIR)/libbox3d.a -lm

$(BOX3D_DIR)/libbox3d.a:
	$(MAKE) -C $(BOX3D_DIR)

run-tilesetphysics: tileset_physics_tests
	./tileset_physics_tests

tileset_core_tests: tileset_core_tests.cpp ../src/tileset_layout.cpp ../src/tileset_collider.cpp
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include \
	    -o $@ tileset_core_tests.cpp ../src/tileset_layout.cpp ../src/tileset_collider.cpp

run-tilesetcore: tileset_core_tests
	./tileset_core_tests

tileset_placement_tests: tileset_placement_tests.cpp ../src/tileset_placement.cpp
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include \
	    -o $@ tileset_placement_tests.cpp ../src/tileset_placement.cpp

run-tilesetplacement: tileset_placement_tests
	./tileset_placement_tests

# Tileset DSL root: TilesetSpec recording, tile/base verbs, eval_tileset.
# Mirrors run-script's recipe: same QuickJS objects + full script-host source set.
TILESETDSL_TARGET = tileset_dsl_tests
TILESETDSL_CPP = tileset_dsl_tests.cpp $(filter-out script_host_tests.cpp, $(SCRIPT_CPP))

$(TILESETDSL_TARGET): tileset_dsl_tests.cpp $(SCRIPT_CPP) $(SCRIPT_C) $(QJS_C)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(SCRIPT_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(TILESETDSL_CPP) $(QJS_OBJ) $(SCRIPT_C_OBJ) -o $(TILESETDSL_TARGET) \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(SCRIPT_C_OBJ)

run-tilesetdsl: $(TILESETDSL_TARGET)
	./$(TILESETDSL_TARGET)

# Tileset bake bridge: bakes Pebble + Twig fixtures via HostBaker, then tests
# collider_for_part / scale_fit / fit_half_height. Full stack: ScriptHost +
# QuickJS + MSL backend (mirrors run-graph-integration) + tileset_collider +
# tileset_part_collider + box3d (box3d.h transitively included via tileset_settle.h;
# linked for completeness since collider_for_part ultimately feeds box3d).
# -DMATTER_HAVE_SCRIPT_HOST enables FileModuleResolver + HostBaker in part_graph.h.
TILESETBAKE_TARGET = tileset_bake_tests
TILESETBAKE_CPP = tileset_bake_tests.cpp ../src/part_graph.cpp \
                  ../src/tileset_bake.cpp ../src/tileset_settle.cpp \
                  ../src/tileset_collider.cpp ../src/tileset_part_collider.cpp \
                  ../src/tileset_phase.cpp \
                  ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp \
                  ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
                  ../src/dsl_bindings.cpp ../src/csg_lowering.cpp \
                  ../src/module_resolver.cpp ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
                  ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
                  $(COMMON_MSL_FULL_SRC)
TILESETBAKE_C   = $(COMMON_MSL_C)
TILESETBAKE_C_OBJ = $(COMMON_MSL_C_OBJ)

$(TILESETBAKE_TARGET): $(TILESETBAKE_CPP) $(TILESETBAKE_C) $(QJS_C) $(BOX3D_DIR)/libbox3d.a
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(TILESETBAKE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(TILESETBAKE_CPP) $(QJS_OBJ) $(TILESETBAKE_C_OBJ) \
	      -o $(TILESETBAKE_TARGET) \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST \
	      $(INCLUDE_PATHS) -I$(BOX3D_DIR)/include \
	      $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(BOX3D_DIR)/libbox3d.a -lm
	rm -f $(QJS_OBJ) $(TILESETBAKE_C_OBJ)

run-tilesetbake: $(TILESETBAKE_TARGET)
	./$(TILESETBAKE_TARGET)

# tileset .gtex format round-trip + cache-hit tests (headless, GL-free).
TILESETGTEX_TARGET = tileset_gtex_tests
TILESETGTEX_CPP    = tileset_gtex_tests.cpp ../src/tileset_gtex.cpp

$(TILESETGTEX_TARGET): $(TILESETGTEX_CPP)
	$(CC) $(TILESETGTEX_CPP) -o $(TILESETGTEX_TARGET) $(CFLAGS) -I../include -I../../Libraries/raylib/src

run-tilesetgtex: $(TILESETGTEX_TARGET)
	./$(TILESETGTEX_TARGET)

# tileset_torus_bvh: assemble SettledTorus into BLAS + TLAS (CPU-only, GL-free).
# Mirrors the TILESETBAKE stanza dep set (full MSL BLAS/TLAS backend +
# part_asset::load_v2 + part_asset::register_prebuilt). No QuickJS needed.
TILESETTORUSBVH_TARGET = tileset_torus_bvh_tests
TILESETTORUSBVH_CPP    = tileset_torus_bvh_tests.cpp ../src/tileset_torus_bvh.cpp \
                         ../src/part_asset_v2.cpp \
                         $(COMMON_MSL_BLAS_SRC)
TILESETTORUSBVH_C      = $(MSL)/material_registry.c
TILESETTORUSBVH_C_OBJ  = material_registry.o

$(TILESETTORUSBVH_TARGET): $(TILESETTORUSBVH_CPP) $(TILESETTORUSBVH_C)
	gcc -c $(TILESETTORUSBVH_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(TILESETTORUSBVH_CPP) $(TILESETTORUSBVH_C_OBJ) -o $(TILESETTORUSBVH_TARGET) \
	      $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(TILESETTORUSBVH_C_OBJ)

run-tilesettorusbvh: $(TILESETTORUSBVH_TARGET)
	./$(TILESETTORUSBVH_TARGET)

# Meadow world.manifest tileset-flag parse test (headless, GL-free, no QuickJS).
# Verifies ForestFloor is declared with the `tileset` flag in Meadow's manifest.
TILESETMEADOWMANIFEST_TARGET = tileset_meadow_manifest_tests
TILESETMEADOWMANIFEST_CPP = tileset_meadow_manifest_tests.cpp ../src/part_graph.cpp \
                            ../src/part_asset_v2.cpp \
                            $(COMMON_MSL_BLAS_SRC)
TILESETMEADOWMANIFEST_C   = $(MSL)/material_registry.c
TILESETMEADOWMANIFEST_C_OBJ = material_registry.o

$(TILESETMEADOWMANIFEST_TARGET): $(TILESETMEADOWMANIFEST_CPP) $(TILESETMEADOWMANIFEST_C)
	gcc -c $(TILESETMEADOWMANIFEST_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(TILESETMEADOWMANIFEST_CPP) $(TILESETMEADOWMANIFEST_C_OBJ) -o $(TILESETMEADOWMANIFEST_TARGET) \
	      $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(TILESETMEADOWMANIFEST_C_OBJ)

run-tilesetmeadowmanifest: $(TILESETMEADOWMANIFEST_TARGET)
	./$(TILESETMEADOWMANIFEST_TARGET)

# Stage 1a — embedded shader lookup (headless, GL-free).
# Needs the generated header; run `make -C .. shaders_gen/embedded_shaders.h` first.
SHADER_SOURCE_TARGET = shader_source_tests
SHADER_SOURCE_CPP = shader_source_tests.cpp ../src/shader_source.cpp

$(SHADER_SOURCE_TARGET): $(SHADER_SOURCE_CPP) ../shaders_gen/embedded_shaders.h
	$(CC) $(SHADER_SOURCE_CPP) -o $(SHADER_SOURCE_TARGET) \
	      $(CFLAGS) -I../include -I..

run-shader-source: $(SHADER_SOURCE_TARGET)
	./$(SHADER_SOURCE_TARGET)

# Phase B Task 1 — async bake primitives (headless, GL-free, no GALLIUM env).
# Sources: test driver + the new async_bake.cpp only; links pthreads via LDFLAGS.
ASYNCQ_TARGET = async_queue_tests
ASYNCQ_CPP = async_queue_tests.cpp ../src/async_bake.cpp

$(ASYNCQ_TARGET): $(ASYNCQ_CPP)
	$(CC) $(ASYNCQ_CPP) -o $(ASYNCQ_TARGET) $(CFLAGS) $(INCLUDE_PATHS) -lpthread

run-asyncq: $(ASYNCQ_TARGET)
	./$(ASYNCQ_TARGET)

# ---------------------------------------------------------------------------
# GPU tests (require GL 4.6 + GALLIUM_DRIVER=d3d12 on WSLg).
# These were formerly in MatterEngine3/viewer/; Stage 5 moved them here.
#
# GPU_PIPELINE_CPP — full engine+MSL C++ sources (mirrors MatterViewer PIPELINE_CPP).
# GPU_RENDER_CPP   — render/provider/facade kernel sources.
# GPU_MSL_CPP      — MSL C++ backend.
# GPU_MSL_C        — MSL+QJS C sources (compiled via gcc, extern "C" safe).
# GPU_MSL_C_OBJ    — corresponding .o names.
# GPU_IMGUI        — Dear ImGui sources (needed for viewer-app GPU tests only if
#                    ui.cpp were included; excluded here: GPU tests are headless).
# ---------------------------------------------------------------------------
BOX3D_DIR_GPU = ../../Libraries/box3d

AR_CORE_PATH_GPU = ../../Libraries/autoremesher_core
TBB_BUILD_DIR_GPU := $(firstword $(wildcard $(AR_CORE_PATH_GPU)/thirdparty/tbb/build/linux_*_release))
TBB_BUILD_SUBDIR_GPU := $(patsubst $(AR_CORE_PATH_GPU)/thirdparty/tbb/build/%,%,$(TBB_BUILD_DIR_GPU))
HAVE_AUTOREMESHER_GPU := $(and $(wildcard $(AR_CORE_PATH_GPU)/libautoremesher_core.a),$(TBB_BUILD_DIR_GPU))
ifneq ($(HAVE_AUTOREMESHER_GPU),)
  GPU_AR_CORE_CFLAGS  := -DMATTER_HAVE_AUTOREMESHER -I$(AR_CORE_PATH_GPU)/include
  GPU_AR_CORE_LDFLAGS := -L$(AR_CORE_PATH_GPU) -lautoremesher_core \
                         -L$(TBB_BUILD_DIR_GPU) -ltbb \
                         -Wl,-rpath,'$$ORIGIN/../../Libraries/autoremesher_core/thirdparty/tbb/build/$(TBB_BUILD_SUBDIR_GPU)' \
                         -Wl,--allow-multiple-definition
  GPU_AR_CORE_CPP     := ../../MatterSurfaceLib/src/mesh_retopo.cpp
else
  GPU_AR_CORE_CFLAGS  :=
  GPU_AR_CORE_LDFLAGS :=
  GPU_AR_CORE_CPP     :=
endif

GPU_RENDER_CPP = \
    ../src/async_bake.cpp \
    ../src/render/renderer.cpp ../src/render/raster_composer.cpp \
    ../src/render/raster_mesh.cpp ../src/render/part_store.cpp \
    ../src/render/world_composer.cpp ../src/render/world_state.cpp \
    ../src/render/gpu_culler.cpp ../src/render/probe_texture.cpp \
    ../src/render/tileset_provider.cpp ../src/render/tileset_gl_ctx.cpp \
    ../src/provider/local_provider.cpp ../src/provider/resolvers.cpp \
    ../src/matter_engine.cpp

GPU_PIPELINE_CPP = \
    ../src/part_graph.cpp \
    ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp \
    ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/module_resolver.cpp \
    ../src/script_rng_binding.cpp \
    ../src/polygon_triangulate.cpp \
    ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
    ../src/world_flatten.cpp ../src/part_flatten.cpp ../src/triangle_emit.cpp \
    ../src/part_cluster.cpp \
    ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
    ../src/tileset_torus_bvh.cpp \
    ../src/tileset_gtex.cpp ../src/tileset_bake_gpu.cpp \
    ../src/sector_grid.cpp ../src/lod_select.cpp \
    ../src/world_lights.cpp ../src/probe_volume.cpp \
    ../src/world_tracer.cpp ../src/probe_bake.cpp \
    ../src/shader_source.cpp \
    ../src/tileset_phase.cpp ../src/tileset_phase_gpu.cpp \
    ../src/tileset_bake.cpp ../src/tileset_settle.cpp \
    ../src/tileset_collider.cpp ../src/tileset_part_collider.cpp \
    ../src/tileset_bake_primary.cpp ../src/tileset_bake_ao.cpp

GPU_MSL_CPP = \
    $(MSL)/cluster.cpp $(MSL)/cell.cpp \
    $(MSL)/mesh_simplifier.cpp $(MSL)/mesh_indexed.cpp $(MSL)/mesh_transform.cpp \
    $(MSL)/blas_manager.cpp $(MSL)/bvh.cpp $(MSL)/bvh_analyzer.cpp \
    $(MSL)/mesh_worker_pool.cpp $(MSL)/mesh_build_utils.cpp \
    $(MSL)/meshing_algorithm.cpp $(MSL)/marching_cubes_algorithm.cpp \
    $(MSL)/oriented_cube_algorithm.cpp $(MSL)/vertex_ao.cpp \
    $(MSL)/occupancy.cpp $(MSL)/tlas_manager.cpp $(MSL)/part_asset.cpp \
    $(MSL)/lattice.cpp $(MSL)/particle_culling.cpp \
    $(GPU_AR_CORE_CPP)

GPU_MSL_C   = $(COMMON_MSL_C)
GPU_MSL_C_OBJ = $(COMMON_MSL_C_OBJ)

GPU_ALL_CPP = $(GPU_PIPELINE_CPP) $(GPU_RENDER_CPP) $(GPU_MSL_CPP)

# GPU tests compile the render sources (renderer.cpp, raster_composer.cpp, etc.)
# which require GL 4.3 compute shaders. Override GRAPHICS to OPENGL_43 here so
# the render sources see the same define as in libmatter_engine3.a (which is also
# built with OPENGL_43). Using OPENGL_33 would silently omit gl*Compute* from
# rlgl.h, causing compile or runtime failures in GPU test binaries.
GPU_GRAPHICS = GRAPHICS_API_OPENGL_43
GPU_CFLAGS = $(filter-out -D$(GRAPHICS),$(CFLAGS)) -D$(GPU_GRAPHICS) \
             -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB \
             $(INCLUDE_PATHS) -I$(BOX3D_DIR_GPU)/include $(QJS_INC) \
             $(GPU_AR_CORE_CFLAGS)

# gpu_cull_tests binary: full pipeline + render + gpu_cull_tests.cpp driver.
gpu-tests: gpu_cull_tests.cpp $(GPU_ALL_CPP) $(GPU_MSL_C) $(QJS_C) | ../shaders_gen/embedded_shaders.h
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(GPU_MSL_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) gpu_cull_tests.cpp $(GPU_ALL_CPP) $(QJS_OBJ) $(GPU_MSL_C_OBJ) \
	      -o gpu_tests $(GPU_CFLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR_GPU)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)
	rm -f $(QJS_OBJ) $(GPU_MSL_C_OBJ)

# tileset_gpu_tests binary: full pipeline + render + tileset_gpu_tests.cpp driver.
tileset-gpu-tests: tileset_gpu_tests.cpp $(GPU_ALL_CPP) $(GPU_MSL_C) $(QJS_C) | ../shaders_gen/embedded_shaders.h
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(GPU_MSL_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) tileset_gpu_tests.cpp $(GPU_ALL_CPP) $(QJS_OBJ) $(GPU_MSL_C_OBJ) \
	      -o tileset_gpu_tests $(GPU_CFLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR_GPU)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)
	rm -f $(QJS_OBJ) $(GPU_MSL_C_OBJ)

run-tilesetgpu: tileset-gpu-tests
	GALLIUM_DRIVER=d3d12 ./tileset_gpu_tests

# tileset_seam_tests binary.
tileset-seam-tests: tileset_seam_tests.cpp $(GPU_ALL_CPP) $(GPU_MSL_C) $(QJS_C) | ../shaders_gen/embedded_shaders.h
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(GPU_MSL_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) tileset_seam_tests.cpp $(GPU_ALL_CPP) $(QJS_OBJ) $(GPU_MSL_C_OBJ) \
	      -o tileset_seam_tests $(GPU_CFLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR_GPU)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)
	rm -f $(QJS_OBJ) $(GPU_MSL_C_OBJ)

run-tilesetseam: tileset-seam-tests
	GALLIUM_DRIVER=d3d12 ./tileset_seam_tests

# tileset_provider_tests binary.
tileset-provider-tests: tileset_provider_tests.cpp $(GPU_ALL_CPP) $(GPU_MSL_C) $(QJS_C) | ../shaders_gen/embedded_shaders.h
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(GPU_MSL_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) tileset_provider_tests.cpp $(GPU_ALL_CPP) $(QJS_OBJ) $(GPU_MSL_C_OBJ) \
	      -o tileset_provider_tests $(GPU_CFLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR_GPU)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)
	rm -f $(QJS_OBJ) $(GPU_MSL_C_OBJ)

run-tilesetprovider: tileset-provider-tests
	GALLIUM_DRIVER=d3d12 ./tileset_provider_tests

# tileset_load_tests binary.
tileset-load-tests: tileset_load_tests.cpp $(GPU_ALL_CPP) $(GPU_MSL_C) $(QJS_C) | ../shaders_gen/embedded_shaders.h
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(GPU_MSL_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) tileset_load_tests.cpp $(GPU_ALL_CPP) $(QJS_OBJ) $(GPU_MSL_C_OBJ) \
	      -o tileset_load_tests $(GPU_CFLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR_GPU)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)
	rm -f $(QJS_OBJ) $(GPU_MSL_C_OBJ)

run-tilesetload: tileset-load-tests
	GALLIUM_DRIVER=d3d12 ./tileset_load_tests

# api-tests binary: full engine pipeline + api_tests.cpp driver.
# Run from MatterViewer/ so cache=cache resolves (bake cache lives at MatterViewer/cache).
api-tests: api_tests.cpp $(GPU_ALL_CPP) $(GPU_MSL_C) $(QJS_C) | ../shaders_gen/embedded_shaders.h
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(GPU_MSL_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) api_tests.cpp $(GPU_ALL_CPP) $(QJS_OBJ) $(GPU_MSL_C_OBJ) \
	      -o api_tests $(GPU_CFLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR_GPU)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)
	rm -f $(QJS_OBJ) $(GPU_MSL_C_OBJ)

run-api-tests: api-tests
	cd ../../MatterViewer && GALLIUM_DRIVER=d3d12 ../MatterEngine3/tests/api_tests
