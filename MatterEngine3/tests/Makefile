# Makefile for MatterEngine3 tests.
# MatterEngine3 consumes the MatterSurfaceLib prototype READ-ONLY:
#   new headers via -I../include, consumed prototype headers via
#   -I../../MatterSurfaceLib/include, consumed prototype sources as
#   ../../MatterSurfaceLib/src/<dep>. raylib paths match MatterSurfaceLib/tests
#   (same directory depth).
#
# Build layout (Phase B incremental):
#   OBJDIR = build/
#     build/<flavor>/<mangled-src-path>.o     compiled objects, per-flavor
#     build/<flavor>/<mangled-src-path>.d     GCC dep files (-MMD -MP)
#
# A "flavor" is a distinct compile-flag combination. TUs compiled with
# byte-identical flags share one object across all binaries that link them;
# TUs compiled with differing defines get separate flavor dirs, so no two
# binaries ever collide on an object path. Per-recipe `rm -f *.o` cleanups
# are gone; `clean` removes $(OBJDIR) wholesale.

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

OBJDIR := build
DEPFLAGS := -MMD -MP

# ---------------------------------------------------------------------------
# Object-path mangling. Two DIFFERENT source paths must never map to the SAME
# object path. Strategy: replace every '/' with '__' and every '..' with 'up'
# so ../src/foo.cpp -> up__src__foo.cpp, then append .o.
# ---------------------------------------------------------------------------
mangle = $(subst /,__,$(subst ..,up,$(1)))
obj_of = $(OBJDIR)/$(1)/$(call mangle,$(2)).o

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

# QuickJS-ng sources
QJS_DIR  = ../../Libraries/quickjs-ng
QJS_INC  = -I$(QJS_DIR)
QJS_C    = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c \
           $(QJS_DIR)/libunicode.c $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c

BOX3D_DIR = ../../Libraries/box3d

# Autoremesher_core detection (shared by the retopo integration test AND the GPU tests)
AR_CORE_PATH = ../../Libraries/autoremesher_core
TBB_BUILD_DIR := $(firstword $(wildcard $(AR_CORE_PATH)/thirdparty/tbb/build/linux_*_release))
TBB_BUILD_SUBDIR := $(patsubst $(AR_CORE_PATH)/thirdparty/tbb/build/%,%,$(TBB_BUILD_DIR))

# ---------------------------------------------------------------------------
# Flavors. Each flavor defines its own compile flags. A TU compiled under one
# flavor produces an object at $(OBJDIR)/<flavor>/<mangled>.o and is shared by
# every binary that lists that TU under that flavor.
#
#   def     — default C++: $(CFLAGS) $(INCLUDE_PATHS)
#   qjs     — default C++ + QuickJS include: $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC)
#             (QJS_INC only adds -I, so shared TUs that don't include quickjs.h
#             produce byte-identical objects to `def`; kept separate for the
#             TUs that DO need it, and for defensive clarity.)
#   sh      — script-host: def + -DMATTER_HAVE_SCRIPT_HOST + QJS_INC
#   sp2     — SP2 script host: def + -DSP2_SCRIPT_HOST + QJS_INC
#   vl      — viewer_logic: sh + -DTILESET_GTEX_USE_RAYLIB_STB + -pthread
#             + -I$(BOX3D_DIR)/include
#   retopo  — retopo integration: def + -DMATTER_HAVE_AUTOREMESHER
#             + -I$(AR_CORE_PATH)/include
#   gpu     — GPU tests: OPENGL_43 override + sh flags + TILESET_GTEX_USE_RAYLIB_STB
#             + -I$(BOX3D_DIR)/include + optional autoremesher CFLAGS
#   mslc    — MSL C sources (gcc): -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
#   qjsc    — QuickJS C sources (gcc): -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
#
# GPU autoremesher CFLAGS need to be resolved at parse time so the flavor's
# compile line sees them. Detection replicates the original block.
# ---------------------------------------------------------------------------
HAVE_AUTOREMESHER := $(and $(wildcard $(AR_CORE_PATH)/libautoremesher_core.a),$(TBB_BUILD_DIR))
ifneq ($(HAVE_AUTOREMESHER),)
  GPU_AR_CORE_CFLAGS  := -DMATTER_HAVE_AUTOREMESHER -I$(AR_CORE_PATH)/include
  GPU_AR_CORE_LDFLAGS := -L$(AR_CORE_PATH) -lautoremesher_core \
                         -L$(TBB_BUILD_DIR) -ltbb \
                         -Wl,-rpath,'$$ORIGIN/../../Libraries/autoremesher_core/thirdparty/tbb/build/$(TBB_BUILD_SUBDIR)' \
                         -Wl,--allow-multiple-definition
  GPU_AR_CORE_CPP     := $(MSL)/mesh_retopo.cpp
else
  GPU_AR_CORE_CFLAGS  :=
  GPU_AR_CORE_LDFLAGS :=
  GPU_AR_CORE_CPP     :=
endif

# GPU uses OPENGL_43 in place of the default GRAPHICS setting.
GPU_GRAPHICS := GRAPHICS_API_OPENGL_43

FLAVOR_def_FLAGS      := $(CFLAGS) $(INCLUDE_PATHS)
FLAVOR_qjs_FLAGS      := $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC)
FLAVOR_sh_FLAGS       := $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC)
FLAVOR_sp2_FLAGS      := $(CFLAGS) -DSP2_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC)
FLAVOR_vl_FLAGS       := $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB -pthread $(INCLUDE_PATHS) -I$(BOX3D_DIR)/include $(QJS_INC)
FLAVOR_retopo_FLAGS   := $(CFLAGS) -DMATTER_HAVE_AUTOREMESHER $(INCLUDE_PATHS) -I$(AR_CORE_PATH)/include
FLAVOR_gpu_FLAGS      := $(filter-out -D$(GRAPHICS),$(CFLAGS)) -D$(GPU_GRAPHICS) \
                         -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB \
                         $(INCLUDE_PATHS) -I$(BOX3D_DIR)/include $(QJS_INC) $(GPU_AR_CORE_CFLAGS)

# C-source flavors are compiled with gcc (not g++). Split compiler var too.
FLAVOR_mslc_CC        := gcc
FLAVOR_mslc_FLAGS     := -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
FLAVOR_qjsc_CC        := gcc
FLAVOR_qjsc_FLAGS     := -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)

# C++ flavor list (compiled with $(CC) = g++). Order matters only for pattern-rule
# generation; the alt-compile-cmd rules are all distinct.
CPP_FLAVORS := def qjs sh sp2 vl retopo gpu
C_FLAVORS   := mslc qjsc
ALL_FLAVORS := $(CPP_FLAVORS) $(C_FLAVORS)

# ---------------------------------------------------------------------------
# Rule template: for each flavor F, define
#   $(OBJDIR)/F/<mangled>.o : <src>
# via a static-pattern-style helper. We use $(foreach)+$(eval) with a canned
# compile command specialized per flavor.
#
# Because two source paths that mangle to the same key would collide, the
# mangle() function includes every path component; verified against the source
# lists in this file (no collisions).
# ---------------------------------------------------------------------------

# obj_list <flavor> <srcs> -> list of object files for those srcs under flavor
obj_list = $(foreach s,$(2),$(call obj_of,$(1),$(s)))

# For each CPP flavor: a pattern that maps build/F/%.o -> %, but since the
# source path doesn't literally live at build/F/%, we instead emit explicit
# per-source rules via the objrule template.
define objrule_cpp
$(call obj_of,$(1),$(2)): $(2) | $(OBJDIR)/$(1)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(FLAVOR_$(1)_FLAGS) $$(DEPFLAGS) -c $$< -o $$@
endef

define objrule_c
$(call obj_of,$(1),$(2)): $(2) | $(OBJDIR)/$(1)
	@mkdir -p $$(dir $$@)
	$$(FLAVOR_$(1)_CC) $$(FLAVOR_$(1)_FLAGS) $$(DEPFLAGS) -c $$< -o $$@
endef

$(OBJDIR)/%:
	@mkdir -p $@

# ---------------------------------------------------------------------------
# Source-set definitions PER FLAVOR. We collect the union of every TU that
# needs to be compiled under a given flavor, then instantiate one rule per
# (flavor, src) pair via foreach+eval.
# ---------------------------------------------------------------------------

# ===== flavor def =====================================================
# Default C++ flavor is used by every non-QJS CPU-only test's shared C++ TUs.
# Targets: partv2, graph, trivar, comp, flatten, lighting, tilesettorusbvh,
# tilesetmeadowmanifest, iso, dev_live_edit (needs default cpp with INCLUDE_PATHS).

# Convenience sub-lists reused by many binaries (unchanged names).
PARTV2_TARGET = part_asset_v2_tests
PARTV2_CPP = part_asset_v2_tests.cpp ../src/part_asset_v2.cpp \
             $(COMMON_MSL_BLAS_SRC)
PARTV2_C   = $(MSL)/material_registry.c

GRAPH_TARGET = part_graph_tests
GRAPH_SOURCES = part_graph_tests.cpp ../src/part_graph.cpp ../src/part_asset_v2.cpp \
                $(COMMON_MSL_BLAS_SRC)
GRAPH_C   = $(MSL)/material_registry.c

TRIVAR_TARGET = triangle_variation_tests
TRIVAR_CPP = triangle_variation_tests.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp ../src/part_asset_v2.cpp \
             $(COMMON_MSL_BLAS_SRC)
TRIVAR_C   = $(MSL)/material_registry.c

COMP_TARGET = composition_tests
COMP_CPP = composition_tests.cpp ../src/lod_bake.cpp ../src/world_flatten.cpp \
           ../src/sector_grid.cpp ../src/lod_select.cpp ../src/part_asset_v2.cpp \
           $(MSL)/mesh_simplifier.cpp \
           $(MSL)/mesh_indexed.cpp \
           $(COMMON_MSL_BLAS_SRC)
COMP_C   = $(MSL)/material_registry.c

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

LIGHTING_TARGET = lighting_tests
LIGHTING_SOURCES = lighting_tests.cpp ../src/world_lights.cpp ../src/probe_volume.cpp \
                   ../src/world_tracer.cpp ../src/probe_bake.cpp \
                   ../src/part_graph.cpp ../src/part_asset_v2.cpp \
                   $(MSL)/mesh_simplifier.cpp \
                   $(COMMON_MSL_BLAS_SRC)
LIGHTING_C   = $(MSL)/material_registry.c

TILESETTORUSBVH_TARGET = tileset_torus_bvh_tests
TILESETTORUSBVH_CPP    = tileset_torus_bvh_tests.cpp ../src/tileset_torus_bvh.cpp \
                         ../src/part_asset_v2.cpp \
                         $(COMMON_MSL_BLAS_SRC)
TILESETTORUSBVH_C      = $(MSL)/material_registry.c

TILESETMEADOWMANIFEST_TARGET = tileset_meadow_manifest_tests
TILESETMEADOWMANIFEST_CPP = tileset_meadow_manifest_tests.cpp ../src/part_graph.cpp \
                            ../src/part_asset_v2.cpp \
                            $(COMMON_MSL_BLAS_SRC)
TILESETMEADOWMANIFEST_C   = $(MSL)/material_registry.c

ISO_TARGET = iso_primitive_tests
ISO_CPP = iso_primitive_tests.cpp field_probe.cpp \
          ../src/csg_lowering.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
          ../src/part_asset_v2.cpp \
          $(COMMON_MSL_BLAS_SRC)
ISO_C   = $(MSL)/surface.c $(MSL)/fat_primitive.c \
          $(MSL)/spatial_hash.c $(MSL)/object_allocator.c \
          $(MSL)/material_registry.c

DEV_TARGET = dev_live_edit_tests
DEV_SOURCES = dev_live_edit_tests.cpp ../src/live_edit.cpp ../src/inotify_watcher.cpp

POLYTRI_TARGET = polygon_triangulate_tests
POLYTRI_CPP = polygon_triangulate_tests.cpp ../src/polygon_triangulate.cpp

SHADER_SOURCE_TARGET = shader_source_tests
SHADER_SOURCE_CPP = shader_source_tests.cpp ../src/shader_source.cpp

ASYNCQ_TARGET = async_queue_tests
ASYNCQ_CPP = async_queue_tests.cpp ../src/async_bake.cpp

TILESETGTEX_TARGET = tileset_gtex_tests
TILESETGTEX_CPP    = tileset_gtex_tests.cpp ../src/tileset_gtex.cpp

# Simple standalone targets (they use tiny custom flag sets — kept as-is with
# per-target rules below, no shared objects).

# ===== flavor sh (script-host) =========================================
# script_host_tests uses just def+QJS_INC (no MATTER_HAVE_SCRIPT_HOST). But
# the graph_integration/example/meadow/gallery/treebake/grasslod/stressforest
# family all use -DMATTER_HAVE_SCRIPT_HOST + QJS_INC.
SCRIPT_TARGET = script_host_tests
SCRIPT_CPP = script_host_tests.cpp ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
             ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
             ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
             $(COMMON_MSL_FULL_SRC)
SCRIPT_C   = $(COMMON_MSL_C)

GRAPH_INT_TARGET = part_graph_integration_tests
GRAPH_INT_CPP = part_graph_integration_tests.cpp ../src/part_graph.cpp \
                ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
                ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
                ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
                $(COMMON_MSL_FULL_SRC)
GRAPH_INT_C   = $(COMMON_MSL_C)

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

GALLERY_TARGET = gallery_bake_tests
GALLERY_CPP = gallery_bake_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))

TREEBAKE_TARGET = tree_bake_check
TREEBAKE_CPP = tree_bake_check.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))

MEADOW_TARGET = meadow_bake_tests
MEADOW_CPP = meadow_bake_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))

MEADOWCHECK_TARGET = meadow_bake_check
MEADOWCHECK_CPP = meadow_bake_check.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))

GRASSLOD_TARGET = grass_lod_tests
GRASSLOD_CPP = grass_lod_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))

STRESSFOREST_TARGET = stress_forest_tests
STRESSFOREST_CPP = stress_forest_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))

# ===== Task 9: live-edit production seams ==================================
LIVEPROD_TARGET = live_edit_prod_tests
LIVEPROD_CPP = live_edit_prod_tests.cpp \
               ../src/live_edit_prod.cpp \
               ../src/part_graph_snapshot.cpp \
               $(filter-out example_world.cpp, $(EXAMPLE_CPP))
LIVEPROD_C   = $(COMMON_MSL_C)

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

# ===== flavor qjs (script_host_tests + tileset_dsl_tests) ===============
# NOTE: script_host_tests originally compiled with CFLAGS + INCLUDE_PATHS +
# QJS_INC (no -DMATTER_HAVE_SCRIPT_HOST). That maps to `qjs`, not `sh`.
TILESETDSL_TARGET = tileset_dsl_tests
TILESETDSL_CPP = tileset_dsl_tests.cpp $(filter-out script_host_tests.cpp, $(SCRIPT_CPP))

# ===== flavor sp2 (SP2_SCRIPT_HOST) =====================================
SHLIB_TARGET = shared_lib_tests
SHLIB_CPP = shared_lib_tests.cpp ../src/module_resolver.cpp ../src/script_rng_binding.cpp \
            ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp ../src/dsl_bindings.cpp ../src/csg_lowering.cpp \
            ../src/part_asset_v2.cpp ../src/lod_bake.cpp ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
            $(COMMON_MSL_FULL_SRC)
SHLIB_C   = $(COMMON_MSL_C)

# ===== flavor vl (viewer_logic) =========================================
VIEWER_LOGIC_TARGET = viewer_logic_tests
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

# ===== flavor retopo (retopo_integration_tests) ========================
RETOPO_INT_TARGET = retopo_integration_tests
RETOPO_INT_CPP = retopo_integration_tests.cpp ../src/part_flatten.cpp ../src/lod_bake.cpp \
                 ../src/part_cluster.cpp \
                 ../src/part_asset_v2.cpp \
                 ../src/retopo_blacklist.cpp \
                 $(MSL)/mesh_simplifier.cpp \
                 $(MSL)/mesh_indexed.cpp \
                 $(MSL)/mesh_transform.cpp \
                 $(MSL)/mesh_retopo.cpp \
                 $(MSL)/blas_manager.cpp \
                 $(MSL)/bvh.cpp \
                 $(MSL)/tlas_manager.cpp \
                 $(MSL)/part_asset.cpp \
                 $(MSL)/vertex_ao.cpp \
                 $(MSL)/occupancy.cpp
RETOPO_INT_C   = $(MSL)/material_registry.c
# --allow-multiple-definition: raylib bundles stb_image_write.h in rtextures.c
# and autoremesher_core's geogram bundles its own copy in
# image_serializer_stb.o. Both archives get pulled in here.
RETOPO_INT_LDFLAGS = $(LDFLAGS) \
                     -L$(AR_CORE_PATH) -lautoremesher_core \
                     -L$(TBB_BUILD_DIR) -ltbb \
                     -Wl,-rpath,'$$ORIGIN/../../Libraries/autoremesher_core/thirdparty/tbb/build/$(TBB_BUILD_SUBDIR)' \
                     -Wl,--allow-multiple-definition

# ===== flavor gpu (GPU tests) ==========================================
GPU_RENDER_CPP = \
    ../src/async_bake.cpp \
    ../src/render/renderer.cpp ../src/render/raster_composer.cpp \
    ../src/render/raster_mesh.cpp ../src/render/part_store.cpp \
    ../src/render/world_composer.cpp ../src/render/world_state.cpp \
    ../src/render/gpu_culler.cpp ../src/render/probe_texture.cpp \
    ../src/render/tileset_provider.cpp ../src/render/tileset_gl_ctx.cpp \
    ../src/provider/local_provider.cpp ../src/provider/resolvers.cpp \
    ../src/matter_engine.cpp \
    ../src/live_edit.cpp \
    ../src/live_edit_prod.cpp \
    ../src/inotify_watcher.cpp \
    ../src/part_graph_snapshot.cpp

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
GPU_ALL_CPP = $(GPU_PIPELINE_CPP) $(GPU_RENDER_CPP) $(GPU_MSL_CPP)

# ---------------------------------------------------------------------------
# Enumerate the source-per-flavor sets and instantiate object rules.
# ---------------------------------------------------------------------------

# Collect the sources compiled under each flavor.
def_CPP_SRCS := $(sort \
    $(PARTV2_CPP) \
    $(GRAPH_SOURCES) \
    $(TRIVAR_CPP) \
    $(COMP_CPP) \
    $(FLATTEN_CPP) \
    $(LIGHTING_SOURCES) \
    $(TILESETTORUSBVH_CPP) \
    $(TILESETMEADOWMANIFEST_CPP) \
    $(ISO_CPP) \
    $(DEV_SOURCES) \
    $(POLYTRI_CPP) \
    $(SHADER_SOURCE_CPP) \
    $(ASYNCQ_CPP) \
    $(TILESETGTEX_CPP))

qjs_CPP_SRCS := $(sort \
    $(SCRIPT_CPP) \
    $(TILESETDSL_CPP))

sh_CPP_SRCS := $(sort \
    $(GRAPH_INT_CPP) \
    $(EXAMPLE_CPP) \
    $(GALLERY_CPP) \
    $(TREEBAKE_CPP) \
    $(MEADOW_CPP) \
    $(MEADOWCHECK_CPP) \
    $(GRASSLOD_CPP) \
    $(STRESSFOREST_CPP) \
    $(TILESETBAKE_CPP) \
    $(LIVEPROD_CPP))

sp2_CPP_SRCS := $(sort $(SHLIB_CPP))
vl_CPP_SRCS  := $(sort $(VIEWER_LOGIC_CPP))
retopo_CPP_SRCS := $(sort $(RETOPO_INT_CPP))
gpu_CPP_SRCS := $(sort \
    gpu_cull_tests.cpp \
    tileset_gpu_tests.cpp \
    tileset_seam_tests.cpp \
    tileset_provider_tests.cpp \
    tileset_load_tests.cpp \
    api_tests.cpp \
    async_bake_tests.cpp \
    $(GPU_ALL_CPP))

# C-source flavors. Union across binaries that use them.
mslc_C_SRCS := $(sort $(COMMON_MSL_C))
qjsc_C_SRCS := $(sort $(QJS_C))

# Object lists per flavor.
def_CPP_OBJS    := $(call obj_list,def,$(def_CPP_SRCS))
qjs_CPP_OBJS    := $(call obj_list,qjs,$(qjs_CPP_SRCS))
sh_CPP_OBJS     := $(call obj_list,sh,$(sh_CPP_SRCS))
sp2_CPP_OBJS    := $(call obj_list,sp2,$(sp2_CPP_SRCS))
vl_CPP_OBJS     := $(call obj_list,vl,$(vl_CPP_SRCS))
retopo_CPP_OBJS := $(call obj_list,retopo,$(retopo_CPP_SRCS))
gpu_CPP_OBJS    := $(call obj_list,gpu,$(gpu_CPP_SRCS))
mslc_C_OBJS     := $(call obj_list,mslc,$(mslc_C_SRCS))
qjsc_C_OBJS     := $(call obj_list,qjsc,$(qjsc_C_SRCS))

ALL_OBJS := $(def_CPP_OBJS) $(qjs_CPP_OBJS) $(sh_CPP_OBJS) $(sp2_CPP_OBJS) \
            $(vl_CPP_OBJS) $(retopo_CPP_OBJS) $(gpu_CPP_OBJS) \
            $(mslc_C_OBJS) $(qjsc_C_OBJS)

# Instantiate per-source compile rules.
$(foreach s,$(def_CPP_SRCS),$(eval $(call objrule_cpp,def,$(s))))
$(foreach s,$(qjs_CPP_SRCS),$(eval $(call objrule_cpp,qjs,$(s))))
$(foreach s,$(sh_CPP_SRCS),$(eval $(call objrule_cpp,sh,$(s))))
$(foreach s,$(sp2_CPP_SRCS),$(eval $(call objrule_cpp,sp2,$(s))))
$(foreach s,$(vl_CPP_SRCS),$(eval $(call objrule_cpp,vl,$(s))))
$(foreach s,$(retopo_CPP_SRCS),$(eval $(call objrule_cpp,retopo,$(s))))
$(foreach s,$(gpu_CPP_SRCS),$(eval $(call objrule_cpp,gpu,$(s))))
$(foreach s,$(mslc_C_SRCS),$(eval $(call objrule_c,mslc,$(s))))
$(foreach s,$(qjsc_C_SRCS),$(eval $(call objrule_c,qjsc,$(s))))

# Load dep files.
-include $(wildcard $(OBJDIR)/*/*.d)

# ---------------------------------------------------------------------------
# Convenience: derived object-list helpers per binary (link-time inputs).
# ---------------------------------------------------------------------------
PARTV2_OBJS = $(call obj_list,def,$(PARTV2_CPP)) $(call obj_list,mslc,$(PARTV2_C))
GRAPH_OBJS  = $(call obj_list,def,$(GRAPH_SOURCES)) $(call obj_list,mslc,$(GRAPH_C))
TRIVAR_OBJS = $(call obj_list,def,$(TRIVAR_CPP)) $(call obj_list,mslc,$(TRIVAR_C))
COMP_OBJS   = $(call obj_list,def,$(COMP_CPP)) $(call obj_list,mslc,$(COMP_C))
FLATTEN_OBJS = $(call obj_list,def,$(FLATTEN_CPP)) $(call obj_list,mslc,$(FLATTEN_C))
LIGHTING_OBJS = $(call obj_list,def,$(LIGHTING_SOURCES)) $(call obj_list,mslc,$(LIGHTING_C))
TILESETTORUSBVH_OBJS = $(call obj_list,def,$(TILESETTORUSBVH_CPP)) $(call obj_list,mslc,$(TILESETTORUSBVH_C))
TILESETMEADOWMANIFEST_OBJS = $(call obj_list,def,$(TILESETMEADOWMANIFEST_CPP)) $(call obj_list,mslc,$(TILESETMEADOWMANIFEST_C))
ISO_OBJS = $(call obj_list,def,$(ISO_CPP)) $(call obj_list,mslc,$(ISO_C))
DEV_OBJS = $(call obj_list,def,$(DEV_SOURCES))
POLYTRI_OBJS = $(call obj_list,def,$(POLYTRI_CPP))
SHADER_SOURCE_OBJS = $(call obj_list,def,$(SHADER_SOURCE_CPP))
ASYNCQ_OBJS = $(call obj_list,def,$(ASYNCQ_CPP))
TILESETGTEX_OBJS = $(call obj_list,def,$(TILESETGTEX_CPP))

SCRIPT_OBJS = $(call obj_list,qjs,$(SCRIPT_CPP)) $(call obj_list,mslc,$(SCRIPT_C)) $(qjsc_C_OBJS)
TILESETDSL_OBJS = $(call obj_list,qjs,$(TILESETDSL_CPP)) $(call obj_list,mslc,$(SCRIPT_C)) $(qjsc_C_OBJS)

GRAPH_INT_OBJS = $(call obj_list,sh,$(GRAPH_INT_CPP)) $(call obj_list,mslc,$(GRAPH_INT_C)) $(qjsc_C_OBJS)
EXAMPLE_OBJS = $(call obj_list,sh,$(EXAMPLE_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
GALLERY_OBJS = $(call obj_list,sh,$(GALLERY_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
TREEBAKE_OBJS = $(call obj_list,sh,$(TREEBAKE_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
MEADOW_OBJS = $(call obj_list,sh,$(MEADOW_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
MEADOWCHECK_OBJS = $(call obj_list,sh,$(MEADOWCHECK_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
GRASSLOD_OBJS = $(call obj_list,sh,$(GRASSLOD_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
STRESSFOREST_OBJS = $(call obj_list,sh,$(STRESSFOREST_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
TILESETBAKE_OBJS = $(call obj_list,sh,$(TILESETBAKE_CPP)) $(call obj_list,mslc,$(TILESETBAKE_C)) $(qjsc_C_OBJS)
LIVEPROD_OBJS    = $(call obj_list,sh,$(LIVEPROD_CPP)) $(call obj_list,mslc,$(LIVEPROD_C)) $(qjsc_C_OBJS)

SHLIB_OBJS = $(call obj_list,sp2,$(SHLIB_CPP)) $(call obj_list,mslc,$(SHLIB_C)) $(qjsc_C_OBJS)
VIEWER_LOGIC_OBJS = $(call obj_list,vl,$(VIEWER_LOGIC_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
RETOPO_INT_OBJS = $(call obj_list,retopo,$(RETOPO_INT_CPP)) $(call obj_list,mslc,$(RETOPO_INT_C))

# GPU binaries reuse the same gpu-flavor object set. Each binary is:
# <driver>.o + $(GPU_ALL_CPP) objects + mslc + qjsc.
GPU_SHARED_OBJS := $(call obj_list,gpu,$(GPU_ALL_CPP)) $(call obj_list,mslc,$(GPU_MSL_C)) $(qjsc_C_OBJS)

# ---------------------------------------------------------------------------
# .PHONY targets: preserve every name build-all.sh and other scripts rely on.
# ---------------------------------------------------------------------------
.PHONY: clean run-partv2 run-script run-graph run-graph-integration run-trivar run-shlib run-comp run-dev run-live run-example run-gallery run-viewer-logic run-iso run-polytri run-treebake run-meadow run-meadow-check run-lighting run-flatten run-retopo-integration run-grasslod run-stressforest run-tilesetphysics run-tilesetcore run-tilesetplacement run-tilesetdsl run-tilesetbake run-tilesetgtex run-tilesettorusbvh run-tilesetmeadowmanifest run-shader-source run-asyncq gpu-tests tileset-gpu-tests tileset-seam-tests tileset-provider-tests tileset-load-tests api-tests run-tilesetgpu run-tilesetseam run-tilesetprovider run-tilesetload run-api-tests run-asyncbake async-bake-tests run-liveprod

# ---------------------------------------------------------------------------
# Binary link rules (link-only; every .o was built above).
# ---------------------------------------------------------------------------

# Part Artifact v2 round-trip + resolved-hash + format-guard tests.
$(PARTV2_TARGET): $(PARTV2_OBJS)
	$(CC) $(PARTV2_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-partv2: $(PARTV2_TARGET)
	./$(PARTV2_TARGET)

# Script host (QuickJS-ng) + DSL/CSG bake tests.
$(SCRIPT_TARGET): $(SCRIPT_OBJS)
	$(CC) $(SCRIPT_OBJS) -o $@ \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-script: $(SCRIPT_TARGET)
	./$(SCRIPT_TARGET)

# Phase 1 typed iso-primitives + ordered CSG.
$(ISO_TARGET): $(ISO_OBJS)
	$(CC) $(ISO_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-iso: $(ISO_TARGET)
	./$(ISO_TARGET)

# SP-3 end-to-end install through the REAL FileModuleResolver/HostBaker adapters.
$(GRAPH_INT_TARGET): $(GRAPH_INT_OBJS)
	$(CC) $(GRAPH_INT_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-graph-integration: $(GRAPH_INT_TARGET)
	./$(GRAPH_INT_TARGET)

# Part graph resolve/dedup/topo/cycle/reachability/cache-miss tests.
$(GRAPH_TARGET): $(GRAPH_OBJS)
	$(CC) $(GRAPH_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-graph: $(GRAPH_TARGET)
	./$(GRAPH_TARGET)

# Direct-triangle emission + line tuber + variation dedup tests.
$(TRIVAR_TARGET): $(TRIVAR_OBJS)
	$(CC) $(TRIVAR_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-trivar: $(TRIVAR_TARGET)
	./$(TRIVAR_TARGET)

# Phase 3 pure ear-clipping triangulator.
$(POLYTRI_TARGET): $(POLYTRI_OBJS)
	$(CC) $(POLYTRI_OBJS) -o $@ $(CFLAGS) -I../include

run-polytri: $(POLYTRI_TARGET)
	./$(POLYTRI_TARGET)

# Shared script library.
$(SHLIB_TARGET): $(SHLIB_OBJS)
	$(CC) $(SHLIB_OBJS) -o $@ \
	      $(CFLAGS) -DSP2_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-shlib: $(SHLIB_TARGET)
	./$(SHLIB_TARGET)

# Composition-to-world (SP-4).
$(COMP_TARGET): $(COMP_OBJS)
	$(CC) $(COMP_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-comp: $(COMP_TARGET)
	./$(COMP_TARGET)

# Bake-time subtree flattening + error-bounded LOD ladder.
$(FLATTEN_TARGET): $(FLATTEN_OBJS)
	$(CC) $(FLATTEN_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-flatten: $(FLATTEN_TARGET)
	./$(FLATTEN_TARGET)

# Phase 5 Task 14 — end-to-end retopo integration test.
$(RETOPO_INT_TARGET): $(RETOPO_INT_OBJS)
	$(CC) $(RETOPO_INT_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_AUTOREMESHER $(INCLUDE_PATHS) -I$(AR_CORE_PATH)/include $(RETOPO_INT_LDFLAGS) $(LDLIBS)

run-retopo-integration: $(RETOPO_INT_TARGET)
	./$(RETOPO_INT_TARGET)

# SP-5 dev live-edit.
$(DEV_TARGET): $(DEV_OBJS)
	$(CC) $(DEV_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-dev: $(DEV_TARGET)
	./$(DEV_TARGET)

# End-to-end example world.
$(EXAMPLE_TARGET): $(EXAMPLE_OBJS)
	$(CC) $(EXAMPLE_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-example: $(EXAMPLE_TARGET)
	./$(EXAMPLE_TARGET)

# Headless bake smoke test for the primitive test world.
$(GALLERY_TARGET): $(GALLERY_OBJS)
	$(CC) $(GALLERY_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-gallery: $(GALLERY_TARGET)
	./$(GALLERY_TARGET)

# Headless bake check for the world_demo Tree.
$(TREEBAKE_TARGET): $(TREEBAKE_OBJS)
	$(CC) $(TREEBAKE_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-treebake: $(TREEBAKE_TARGET)
	./$(TREEBAKE_TARGET)

# Headless bake tests for the Meadow schemas.
$(MEADOW_TARGET): $(MEADOW_OBJS)
	$(CC) $(MEADOW_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-meadow: $(MEADOW_TARGET)
	./$(MEADOW_TARGET)

# Full Meadow world bake check.
$(MEADOWCHECK_TARGET): $(MEADOWCHECK_OBJS)
	$(CC) $(MEADOWCHECK_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-meadow-check: $(MEADOWCHECK_TARGET)
	./$(MEADOWCHECK_TARGET)

# --- Viewer GL-free logic suite.
../shaders_gen/embedded_shaders.h:
	$(MAKE) -C .. shaders_gen/embedded_shaders.h

$(VIEWER_LOGIC_TARGET): $(VIEWER_LOGIC_OBJS) | ../shaders_gen/embedded_shaders.h
	$(CC) $(VIEWER_LOGIC_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB -pthread $(INCLUDE_PATHS) \
	      -I$(BOX3D_DIR)/include $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(BOX3D_DIR)/libbox3d.a

run-viewer-logic: $(VIEWER_LOGIC_TARGET)
	./$(VIEWER_LOGIC_TARGET)

# Tasks 1-3 — World light list, ProbeVolume, WorldTracer.
$(LIGHTING_TARGET): $(LIGHTING_OBJS)
	$(CC) $(LIGHTING_OBJS) -o $@ $(CFLAGS) -pthread $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-lighting: $(LIGHTING_TARGET)
	./$(LIGHTING_TARGET)

# Grass.js prefix-subset triangle-budget LOD contract (Stage 3).
$(GRASSLOD_TARGET): $(GRASSLOD_OBJS)
	$(CC) $(GRASSLOD_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-grasslod: $(GRASSLOD_TARGET)
	./$(GRASSLOD_TARGET)

# Stage-4 stress fixture scatter determinism.
$(STRESSFOREST_TARGET): $(STRESSFOREST_OBJS)
	$(CC) $(STRESSFOREST_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-stressforest: $(STRESSFOREST_TARGET)
	./$(STRESSFOREST_TARGET)

# Task 9: live-edit production seam tests (ProdGraphResolver/Baker/Flattener).
$(LIVEPROD_TARGET): $(LIVEPROD_OBJS)
	$(CC) $(LIVEPROD_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-liveprod: $(LIVEPROD_TARGET)
	./$(LIVEPROD_TARGET)

# Alias: run-live = dev_live_edit regression (SP-5 seam interfaces, fake graph).
run-live: $(DEV_TARGET)
	./$(DEV_TARGET)

# Box3d physics smoke test.
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

# Tileset DSL root.
$(TILESETDSL_TARGET): $(TILESETDSL_OBJS)
	$(CC) $(TILESETDSL_OBJS) -o $@ \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-tilesetdsl: $(TILESETDSL_TARGET)
	./$(TILESETDSL_TARGET)

# Tileset bake bridge.
$(TILESETBAKE_TARGET): $(TILESETBAKE_OBJS) $(BOX3D_DIR)/libbox3d.a
	$(CC) $(TILESETBAKE_OBJS) \
	      -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST \
	      $(INCLUDE_PATHS) -I$(BOX3D_DIR)/include \
	      $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(BOX3D_DIR)/libbox3d.a -lm

run-tilesetbake: $(TILESETBAKE_TARGET)
	./$(TILESETBAKE_TARGET)

# tileset .gtex format round-trip.
$(TILESETGTEX_TARGET): $(TILESETGTEX_OBJS)
	$(CC) $(TILESETGTEX_OBJS) -o $@ $(CFLAGS) -I../include -I../../Libraries/raylib/src

run-tilesetgtex: $(TILESETGTEX_TARGET)
	./$(TILESETGTEX_TARGET)

# tileset_torus_bvh.
$(TILESETTORUSBVH_TARGET): $(TILESETTORUSBVH_OBJS)
	$(CC) $(TILESETTORUSBVH_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-tilesettorusbvh: $(TILESETTORUSBVH_TARGET)
	./$(TILESETTORUSBVH_TARGET)

# Meadow world.manifest tileset-flag parse test.
$(TILESETMEADOWMANIFEST_TARGET): $(TILESETMEADOWMANIFEST_OBJS)
	$(CC) $(TILESETMEADOWMANIFEST_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-tilesetmeadowmanifest: $(TILESETMEADOWMANIFEST_TARGET)
	./$(TILESETMEADOWMANIFEST_TARGET)

# Stage 1a — embedded shader lookup.
$(SHADER_SOURCE_TARGET): $(SHADER_SOURCE_OBJS) ../shaders_gen/embedded_shaders.h
	$(CC) $(SHADER_SOURCE_OBJS) -o $@ \
	      $(CFLAGS) -I../include -I..

run-shader-source: $(SHADER_SOURCE_TARGET)
	./$(SHADER_SOURCE_TARGET)

# Phase B Task 1 — async bake primitives.
$(ASYNCQ_TARGET): $(ASYNCQ_OBJS)
	$(CC) $(ASYNCQ_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) -lpthread

run-asyncq: $(ASYNCQ_TARGET)
	./$(ASYNCQ_TARGET)

# ---------------------------------------------------------------------------
# GPU tests (require GL 4.6 + GALLIUM_DRIVER=d3d12 on WSLg).
# All share the same $(GPU_SHARED_OBJS) — one flavor `gpu` for the C++ pipeline
# TUs, `mslc` for MSL C sources, `qjsc` for QuickJS C sources.
# ---------------------------------------------------------------------------

gpu-tests: $(call obj_of,gpu,gpu_cull_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h
	$(CC) $(call obj_of,gpu,gpu_cull_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o gpu_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

tileset-gpu-tests: $(call obj_of,gpu,tileset_gpu_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h
	$(CC) $(call obj_of,gpu,tileset_gpu_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o tileset_gpu_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-tilesetgpu: tileset-gpu-tests
	GALLIUM_DRIVER=d3d12 ./tileset_gpu_tests

tileset-seam-tests: $(call obj_of,gpu,tileset_seam_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h
	$(CC) $(call obj_of,gpu,tileset_seam_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o tileset_seam_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-tilesetseam: tileset-seam-tests
	GALLIUM_DRIVER=d3d12 ./tileset_seam_tests

tileset-provider-tests: $(call obj_of,gpu,tileset_provider_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h
	$(CC) $(call obj_of,gpu,tileset_provider_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o tileset_provider_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-tilesetprovider: tileset-provider-tests
	GALLIUM_DRIVER=d3d12 ./tileset_provider_tests

tileset-load-tests: $(call obj_of,gpu,tileset_load_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h
	$(CC) $(call obj_of,gpu,tileset_load_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o tileset_load_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-tilesetload: tileset-load-tests
	GALLIUM_DRIVER=d3d12 ./tileset_load_tests

api-tests: $(call obj_of,gpu,api_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h
	$(CC) $(call obj_of,gpu,api_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o api_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-api-tests: api-tests
	cd ../../MatterViewer && GALLIUM_DRIVER=d3d12 ../MatterEngine3/tests/api_tests

# Phase B Task 6 — async bake worker command loop tests.
async-bake-tests: $(call obj_of,gpu,async_bake_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h
	$(CC) $(call obj_of,gpu,async_bake_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o async_bake_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-asyncbake: async-bake-tests
	./async_bake_tests

# ---------------------------------------------------------------------------
# Clean: remove OBJDIR and all binaries (test-binary-name list, since they
# aren't $(TARGET)-style variables in every case).
# ---------------------------------------------------------------------------
clean:
	rm -rf $(OBJDIR)
	rm -f $(PARTV2_TARGET) $(SCRIPT_TARGET) $(GRAPH_TARGET) $(GRAPH_INT_TARGET) \
	      $(TRIVAR_TARGET) $(SHLIB_TARGET) $(COMP_TARGET) $(DEV_TARGET) \
	      $(EXAMPLE_TARGET) $(VIEWER_LOGIC_TARGET) $(TREEBAKE_TARGET) $(MEADOW_TARGET) \
	      $(MEADOWCHECK_TARGET) $(GRASSLOD_TARGET) $(STRESSFOREST_TARGET) \
	      $(TILESETMEADOWMANIFEST_TARGET) $(RETOPO_INT_TARGET) $(SHADER_SOURCE_TARGET) \
	      $(POLYTRI_TARGET) $(ISO_TARGET) $(FLATTEN_TARGET) $(LIGHTING_TARGET) \
	      $(TILESETTORUSBVH_TARGET) $(TILESETDSL_TARGET) $(TILESETBAKE_TARGET) \
	      $(TILESETGTEX_TARGET) $(ASYNCQ_TARGET) $(GALLERY_TARGET) \
	      $(LIVEPROD_TARGET) \
	      tileset_physics_tests tileset_core_tests tileset_placement_tests \
	      gpu_tests tileset_gpu_tests tileset_seam_tests tileset_provider_tests \
	      tileset_load_tests api_tests async_bake_tests
