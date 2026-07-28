# Makefile for MatterEngine3 tests.
# MatterEngine3 consumes the MatterSurfaceLib prototype READ-ONLY:
#   new headers via -I../include, consumed prototype headers via
#   -I../../libs/MatterSurfaceLib/include, consumed prototype sources as
#   ../../libs/MatterSurfaceLib/src/<dep>. raylib paths match MatterSurfaceLib/tests
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
GRAPHICS = GRAPHICS_API_OPENGL_33
LIBTYPE = STATIC
RAYLIB_RELEASE_PATH = ../../../third_party/raylib
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
INCLUDE_PATHS = -I../include -I../src -I../src/render -I../src/provider -I../../libs/MatterSurfaceLib/include -I../../libs/SpatialQueryLib/include -I../../libs/MemoryLib/include -I../../libs/MathLib/include -I$(RAYLIB_PATH) -I../../third_party/raylib/src -I../../libs/ParticleFlowLib/include -I../../third_party/Vulkan-Headers/include -I$(BOX3D_DIR)/include
INCLUDE_PATHS += -I$(FLECS_DIR)
OZZ_DIR = ../../third_party/ozz-animation
OZZ_BUILD_DIR = $(OZZ_DIR)/build/matter
OZZ_INCLUDE = $(OZZ_DIR)/include
OZZ_BASE_LIB = $(OZZ_BUILD_DIR)/src/base/libozz_base.a
OZZ_ANIMATION_LIB = $(OZZ_BUILD_DIR)/src/animation/runtime/libozz_animation.a
OZZ_OFFLINE_LIB = $(OZZ_BUILD_DIR)/src/animation/offline/libozz_animation_offline.a
OZZ_RUNTIME_LIBS = $(OZZ_ANIMATION_LIB) $(OZZ_BASE_LIB)
OZZ_OFFLINE_LIBS = $(OZZ_OFFLINE_LIB) $(OZZ_RUNTIME_LIBS)
INCLUDE_PATHS += -I$(OZZ_INCLUDE)

# Library paths (match MatterSurfaceLib/tests; same depth).
ifeq ($(UNAME_S),Linux)
    LDLIBS = ../../third_party/raylib/build/linux/libraylib.a
else ifeq ($(UNAME_S),Darwin)
    LDLIBS = ../../third_party/raylib/build/macos/libraylib.a
else
    LDLIBS = ../../third_party/raylib/src/libraylib.a
endif

# Windows (MSYS2 MinGW/UCRT64, uname like MINGW64_NT-*): no X11/GL/rt here.
# Win32 system libs must come AFTER libraylib.a (ld resolves left to right),
# and LDLIBS is last in the link recipes, so they ride in LDLIBS.
ifneq (,$(findstring _NT,$(UNAME_S)))
    LDFLAGS = -lm -lpthread
    LDLIBS  = ../../third_party/raylib/src/libraylib.a -lopengl32 -lgdi32 -lwinmm -lws2_32 -ldbghelp
    FLECS_PLATFORM_LIBS = -lws2_32 -ldbghelp -lwinmm
endif

OBJDIR := build/obj
DEPFLAGS := -MMD -MP

# Top-level build/ dir: needed as an order-only prereq by the handful of
# binary rules that link straight from source (no $(OBJDIR)-based objects to
# implicitly mkdir -p it first).
build:
	@mkdir -p $@

MATRIX_TARGET = build/matrix_tests
MATRIX_CPP = matrix_tests.cpp ../src/render/matrix_math.cpp ../src/render/frame_matrices.cpp
MATRIX_HEADERS = check.h ../include/matter/math_types.h ../include/matter/camera.h \
                 ../src/render/matrix_math.h ../src/render/frame_matrices.h \
                 ../src/render/gpu_matrix_pack.h

ANIMATION_OZZ_TARGET = build/animation_ozz_smoke_tests
ANIMATION_OZZ_CPP = animation_ozz_smoke_tests.cpp ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp

ANIMATION_OZZ_ADAPTER_TARGET = build/animation_ozz_adapter_tests
ANIMATION_OZZ_ADAPTER_CPP = animation_ozz_adapter_tests.cpp ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp

ANIMATION_OZZ_RUNTIME_LINK_TARGET = build/animation_ozz_runtime_link_tests
ANIMATION_OZZ_RUNTIME_LINK_CPP = animation_ozz_runtime_link_tests.cpp ../src/animation/ozz_adapter.cpp ../src/animation/animation_ir.cpp

ANIMATION_IR_TARGET = build/animation_ir_tests
ANIMATION_IR_CPP = animation_ir_tests.cpp ../src/animation/animation_ir.cpp ../src/animation/animation_validate.cpp

ANIMATION_ASSET_TARGET = build/animation_asset_tests
ANIMATION_ASSET_CPP = animation_asset_tests.cpp ../src/animation/anim_asset.cpp ../src/animation/anim_bundle.cpp ../src/animation/animation_asset_store.cpp ../src/animation/animation_binding_bake.cpp ../src/animation/animation_ir.cpp ../src/render/raster_mesh.cpp ../src/render/indexed_part_geometry.cpp ../src/part_asset_v2.cpp $(COMMON_MSL_BLAS_SRC) $(MSL)/material_registry.c

ANIMATION_RUNTIME_ASSET_TARGET = build/animation_runtime_asset_tests
ANIMATION_RUNTIME_ASSET_CPP = animation_runtime_asset_tests.cpp ../src/animation/animation_runtime_asset.cpp ../src/animation/animation_binding_bake.cpp ../src/animation/animation_evaluator.cpp ../src/animation/animation_controllers.cpp ../src/animation/animation_targets.cpp ../src/animation/animation_budget.cpp ../src/animation/animation_validate.cpp ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp ../src/render/raster_mesh.cpp ../src/render/indexed_part_geometry.cpp

ANIMATION_DSL_RIG_TARGET = build/animation_dsl_rig_tests
# SCRIPT_CPP already owns indexed_part_geometry.cpp for ScriptHost's runtime
# asset conversion. Keep this closure to one copy so its object is linked once.
ANIMATION_DSL_RIG_CPP = animation_dsl_rig_tests.cpp $(filter-out script_host_tests.cpp,$(SCRIPT_CPP))
ANIMATION_DSL_MOTION_TARGET = build/animation_dsl_motion_tests
ANIMATION_DSL_MOTION_CPP = animation_dsl_motion_tests.cpp $(filter-out script_host_tests.cpp,$(SCRIPT_CPP))
ANIMATION_DSL_MOTION_OBJS = $(call obj_list,qjs,$(ANIMATION_DSL_MOTION_CPP)) $(call obj_list,mslc,$(SCRIPT_C)) $(qjsc_C_OBJS)
ANIMATION_DSL_RIG_OBJS = $(call obj_list,qjs,$(ANIMATION_DSL_RIG_CPP)) $(call obj_list,mslc,$(SCRIPT_C)) $(qjsc_C_OBJS)

.PHONY: check-animation-dsl-rig-sources
check-animation-dsl-rig-sources:
	@duplicates="$$(printf '%s\n' $(ANIMATION_DSL_RIG_CPP) | sort | uniq -d)"; \
	if test -n "$$duplicates"; then \
		echo "ERROR: animation DSL rig source list has duplicate entries:"; \
		printf '%s\n' "$$duplicates"; \
		exit 1; \
	fi

ANIMATION_BINDING_TARGET = build/animation_binding_tests
ANIMATION_BINDING_CPP = animation_binding_tests.cpp ../src/render/raster_mesh.cpp ../src/render/indexed_part_geometry.cpp ../src/animation/animation_binding_bake.cpp ../src/animation/animation_ir.cpp

# Phase D1: the bake boundary must report every animation diagnostic. Drives a
# real ScriptHost bake, so it needs the script-host closure (sh flavor) exactly
# like animation_dsl_motion does.
ANIMATION_DIAGNOSTICS_TARGET = build/animation_diagnostics_tests
ANIMATION_DIAGNOSTICS_CPP = $(sort animation_diagnostics_tests.cpp \
                            $(filter-out script_host_tests.cpp,$(SCRIPT_CPP)))
ANIMATION_DIAGNOSTICS_C   = $(COMMON_MSL_C)

ANIMATION_STORE_TARGET = build/animation_store_tests
ANIMATION_STORE_CPP = animation_store_tests.cpp ../src/animation/animation_store.cpp ../src/animation/animation_asset_store.cpp ../src/animation/animation_evaluator.cpp ../src/animation/animation_systems.cpp ../src/animation/animation_controllers.cpp ../src/animation/animation_targets.cpp ../src/animation/animation_budget.cpp ../src/animation/animation_world_queries.cpp ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp ../src/ecs/scene_registry.cpp ../src/ecs/ecs_runtime.cpp ../src/render/animation_skin_bridge.cpp ../src/render/vk_animation_bounds.cpp ../src/ecs/physics_context.cpp ../src/ecs/physics_shapes.cpp ../src/ecs/physics_systems.cpp ../src/ecs/streaming_systems.cpp ../src/ecs/transform_system.cpp ../src/streaming/sector_streaming_coordinator.cpp ../src/sector_streamer.cpp
ANIMATION_EVALUATOR_TARGET = build/animation_evaluator_tests
ANIMATION_EVALUATOR_CPP = animation_evaluator_tests.cpp ../src/animation/animation_evaluator.cpp ../src/animation/animation_budget.cpp ../src/animation/animation_targets.cpp ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp
ANIMATION_BUDGET_TARGET = build/animation_budget_tests
ANIMATION_BUDGET_CPP = animation_budget_tests.cpp ../src/animation/animation_budget.cpp

ANIMATION_SYSTEMS_TARGET = build/animation_systems_tests
ANIMATION_SYSTEMS_CPP = animation_systems_tests.cpp ../src/animation/animation_store.cpp ../src/animation/animation_asset_store.cpp ../src/animation/animation_evaluator.cpp ../src/animation/animation_systems.cpp ../src/animation/animation_controllers.cpp ../src/animation/animation_targets.cpp ../src/animation/animation_budget.cpp ../src/animation/animation_world_queries.cpp ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp ../src/ecs/scene_registry.cpp ../src/ecs/ecs_runtime.cpp ../src/render/animation_skin_bridge.cpp ../src/render/vk_animation_bounds.cpp ../src/ecs/physics_context.cpp ../src/ecs/physics_shapes.cpp ../src/ecs/physics_systems.cpp ../src/ecs/streaming_systems.cpp ../src/ecs/transform_system.cpp ../src/streaming/sector_streaming_coordinator.cpp ../src/sector_streamer.cpp

ANIMATION_SIMULATION_TARGET = build/animation_simulation_tests
ANIMATION_SIMULATION_CPP = animation_simulation_tests.cpp ../src/animation/animation_store.cpp ../src/animation/animation_asset_store.cpp ../src/animation/animation_evaluator.cpp ../src/animation/animation_systems.cpp ../src/animation/animation_controllers.cpp ../src/animation/animation_targets.cpp ../src/animation/animation_budget.cpp ../src/animation/animation_world_queries.cpp ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp ../src/ecs/simulation_control.cpp ../src/ecs/scene_registry.cpp ../src/ecs/ecs_runtime.cpp ../src/ecs/dynamic_scene_bridge.cpp ../src/render/animation_rigid_bridge.cpp ../src/render/animation_skin_bridge.cpp ../src/render/vk_animation_skinning.cpp ../src/render/vk_animation_bounds.cpp ../src/render/dynamic_instance_slots.cpp ../src/ecs/physics_context.cpp ../src/ecs/physics_shapes.cpp ../src/ecs/physics_systems.cpp ../src/ecs/streaming_systems.cpp ../src/ecs/transform_system.cpp ../src/streaming/sector_streaming_coordinator.cpp ../src/sector_streamer.cpp

# C4 headless acceptance owns both ends of the actual production contract:
# ScriptHost bakes/reloads AnimatedRigGallery, then Runtime and the rigid/skin
# bridges consume that immutable data.  Keep this closure explicit so a missing
# handoff TU fails the target rather than being hidden by an unrelated suite.
ANIMATION_C4_ACCEPTANCE_TARGET = build/animation_phase_c_acceptance_tests
# E6 fallout (docs/event-system.md S I.11): these suites reach
# physics_context.cpp through the ECS runtime closure, and it now emits
# events::PhysStep on the session hub -- so they need the event impl trio the
# def-flavor targets get via EVENT_IMPL_DEF_OBJS. Added to the source list
# rather than as link-only objects because these are the only sh-flavor
# targets that need them, so no sh obj rules exist yet; $(sort) dedupes.
ANIMATION_ACCEPTANCE_EVENT_CPP = ../src/event/event_hub.cpp \
                                 ../src/event/subscription.cpp \
                                 ../src/event/dispatch_context.cpp
ANIMATION_C4_ACCEPTANCE_CPP = $(sort animation_phase_c_acceptance_tests.cpp \
                              $(filter-out script_host_tests.cpp,$(SCRIPT_CPP)) \
                              ../src/animation/anim_bundle.cpp \
                              $(ANIMATION_ACCEPTANCE_EVENT_CPP) \
                              $(filter-out animation_simulation_tests.cpp,$(ANIMATION_SIMULATION_CPP)))
ANIMATION_C4_ACCEPTANCE_C = $(COMMON_MSL_C)
ANIMATION_C4_ACCEPTANCE_FLECS_C = $(FLECS_C)

# Phase B gate: a 10,000-tick replay of the same committed authored gallery.
# It deliberately shares C4's real ScriptHost/bundle/runtime closure instead
# of replacing controllers or poses with a hand-authored test fixture.
ANIMATION_B_ACCEPTANCE_TARGET = build/animation_phase_b_acceptance_tests
ANIMATION_B_ACCEPTANCE_CPP = $(sort animation_phase_b_acceptance_tests.cpp \
                             $(filter-out animation_phase_c_acceptance_tests.cpp,$(ANIMATION_C4_ACCEPTANCE_CPP)))
ANIMATION_B_ACCEPTANCE_C = $(COMMON_MSL_C)
ANIMATION_B_ACCEPTANCE_FLECS_C = $(FLECS_C)

ANIMATION_CONTROLLER_TARGET = build/animation_controller_tests
ANIMATION_CONTROLLER_CPP = animation_controller_tests.cpp ../src/animation/animation_controllers.cpp ../src/animation/animation_targets.cpp ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp
ANIMATION_IK_TARGET = build/animation_ik_tests
ANIMATION_IK_CPP = animation_ik_tests.cpp ../src/animation/animation_targets.cpp ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp

# CPU-only, no GL/VK/raylib link deps (mirrors MATRIX_TARGET's direct-compile
# pattern) — pure atlas-slicing + mip math, unit-testable without a device.
TILESETSLICER_TARGET = build/tileset_slicer_tests
TILESETSLICER_CPP = tileset_slicer_tests.cpp ../src/render/tileset_slicer.cpp
TILESETSLICER_HEADERS = check.h ../src/render/tileset_slicer.h

FLECS_DIR = ../../third_party/flecs
FLECS_C = $(FLECS_DIR)/flecs.c
ANIMATION_RUNTIME_CPP = ../src/animation/animation_store.cpp ../src/animation/animation_asset_store.cpp ../src/animation/animation_evaluator.cpp ../src/animation/animation_controllers.cpp ../src/animation/animation_targets.cpp ../src/animation/animation_budget.cpp ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp

# ECS Runtime owns AnimationSystems, its evaluator/service backing stores, the
# Box3D query adapter, and skin-asset validation.  Keep these implementations
# together for every headless target that instantiates Runtime.
ANIMATION_ECS_RUNTIME_REQUIRED = $(ANIMATION_RUNTIME_CPP) ../src/animation/animation_systems.cpp ../src/animation/animation_world_queries.cpp ../src/render/animation_skin_bridge.cpp ../src/render/vk_animation_bounds.cpp

# script_host.cpp and part_graph.cpp are animation BAKE hosts now: they compile
# authored rigs, publish the .part/.anim sibling pair, and load committed
# bundles. Any target that compiles either TU needs this closure and the Ozz
# archives, or it fails at link with animation:: undefined references. Kept as
# one variable so a new bake-host consumer picks it up by name rather than by
# rediscovering the symbol list.
ANIMATION_BAKE_HOST_CPP = ../src/dsl_animation.cpp \
                          ../src/animation/animation_ir.cpp \
                          ../src/animation/animation_validate.cpp \
                          ../src/animation/animation_binding_bake.cpp \
                          ../src/animation/animation_runtime_asset.cpp \
                          ../src/animation/anim_asset.cpp \
                          ../src/animation/anim_bundle.cpp \
                          ../src/animation/animation_asset_store.cpp \
                          ../src/animation/animation_evaluator.cpp \
                          ../src/animation/animation_controllers.cpp \
                          ../src/animation/animation_targets.cpp \
                          ../src/animation/animation_budget.cpp \
                          ../src/animation/ozz_adapter.cpp \
                          ../src/animation/ozz_bake.cpp \
                          ../src/render/indexed_part_geometry.cpp

ECS_TARGET = build/ecs_tests
ECS_CPP = ecs_tests.cpp $(ANIMATION_ECS_RUNTIME_REQUIRED) ../src/ecs/scene_registry.cpp ../src/ecs/ecs_runtime.cpp ../src/ecs/physics_context.cpp ../src/ecs/physics_shapes.cpp ../src/ecs/physics_systems.cpp ../src/ecs/streaming_systems.cpp ../src/ecs/transform_system.cpp ../src/streaming/sector_streaming_coordinator.cpp ../src/sector_streamer.cpp
ECS_C = $(FLECS_C)

PHYSICS_TARGET = build/physics-tests
PHYSICS_CPP = physics_tests.cpp $(ANIMATION_ECS_RUNTIME_REQUIRED) ../src/ecs/scene_registry.cpp ../src/ecs/ecs_runtime.cpp ../src/ecs/physics_context.cpp ../src/ecs/physics_shapes.cpp ../src/ecs/physics_systems.cpp ../src/ecs/streaming_systems.cpp ../src/ecs/transform_system.cpp ../src/streaming/sector_streaming_coordinator.cpp ../src/sector_streamer.cpp
PHYSICS_C = $(FLECS_C)

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
#   example, gallery, treebake, grasslod, stressforest,
#   viewer-logic, tilesetbake.
#
# COMMON_MSL_C     — full C-source set (surface.c et al.) used alongside
#   COMMON_MSL_FULL_SRC; compiled via gcc to keep extern "C" symbols unmangled.
# ---------------------------------------------------------------------------
MSL = ../../libs/MatterSurfaceLib/src
# Shared source-of-truth libraries (compiled from source, not copied into MSL).
SQL = ../../libs/SpatialQueryLib
MEMLIB = ../../libs/MemoryLib

COMMON_MSL_BLAS_SRC = \
    $(MSL)/blas_manager.cpp \
    $(SQL)/src/bvh.cpp \
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
    $(MSL)/mesh_transform.cpp \
    $(SQL)/src/bvh_analyzer.cpp \
    $(MSL)/mesh_worker_pool.cpp \
    $(MSL)/mesh_build_utils.cpp \
    $(MSL)/meshing_algorithm.cpp \
    $(MSL)/marching_cubes_algorithm.cpp \
    $(MSL)/oriented_cube_algorithm.cpp \
    $(MSL)/lattice.cpp \
    $(MSL)/particle_culling.cpp

COMMON_MSL_C = \
    $(MSL)/surface.c \
    $(MSL)/fat_primitive.c \
    $(SQL)/src/spatial_hash.c \
    $(MEMLIB)/src/mem_pool.c \
    $(MSL)/material_registry.c

# QuickJS-ng sources
QJS_DIR  = ../../third_party/quickjs-ng
QJS_INC  = -I$(QJS_DIR)
QJS_C    = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c \
           $(QJS_DIR)/libunicode.c $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c

BOX3D_DIR = ../../third_party/box3d
BOX3D_LIB = $(BOX3D_DIR)/libbox3d.a

# Autoremesher_core detection (shared by the retopo integration test AND the GPU tests)
AR_CORE_PATH = ../../third_party/autoremesher_core
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
                         -Wl,-rpath,'$$ORIGIN/../../third_party/autoremesher_core/thirdparty/tbb/build/$(TBB_BUILD_SUBDIR)' \
                         -Wl,--allow-multiple-definition
  GPU_AR_CORE_CPP     := $(MSL)/mesh_retopo.cpp
else
  GPU_AR_CORE_CFLAGS  :=
  GPU_AR_CORE_LDFLAGS :=
  GPU_AR_CORE_CPP     :=
endif

# Full GPU/shared-engine tests bake and then decode animation peers through the
# production ScriptHost + matter_engine.cpp path. Keep Ozz after the objects
# and other archives so static archive resolution remains left-to-right.
GPU_AR_CORE_LDFLAGS += $(OZZ_OFFLINE_LIBS)

# GPU uses OPENGL_43 in place of the default GRAPHICS setting.
GPU_GRAPHICS := GRAPHICS_API_OPENGL_43

FLAVOR_def_FLAGS      := $(CFLAGS) $(INCLUDE_PATHS)
FLAVOR_qjs_FLAGS      := $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC)
FLAVOR_sh_FLAGS       := $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) -I$(BOX3D_DIR)/include $(QJS_INC)
# Only the graph integration target carries the deterministic cache-race seam.
# Production and ordinary script-host test objects have no hook API or storage.
FLAVOR_sh_test_FLAGS  := $(FLAVOR_sh_FLAGS) -DMATTER_TEST_CACHE_VALIDATION_HOOK
# PartStore has its own cache-admission seam. Keep it test-only.
FLAVOR_partstore_test_FLAGS := $(FLAVOR_def_FLAGS) -DMATTER_TEST_CACHE_VALIDATION_HOOK
FLAVOR_sp2_FLAGS      := $(CFLAGS) -DSP2_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC)
FLAVOR_vl_FLAGS       := $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB -pthread $(INCLUDE_PATHS) -I$(BOX3D_DIR)/include $(QJS_INC)
FLAVOR_retopo_FLAGS   := $(CFLAGS) -DMATTER_HAVE_AUTOREMESHER $(INCLUDE_PATHS) -I$(AR_CORE_PATH)/include $(QJS_INC)
# MATTER_VULKAN_ONLY (Phase 5a): the GL renderer/raster-composer/GpuCuller
# classes matter_engine.cpp used to reference under #ifndef MATTER_VULKAN_ONLY
# are deleted outright; this flavor must define the macro so the facade
# compiles (mirrors MatterEngine3/Makefile). Test binaries in this flavor that
# call the legacy WorldSession::render(CameraDesc, int, int, RenderOptions)
# overload (api_tests.cpp, world_stream_tests.cpp) now hit the no-op
# MATTER_VULKAN_ONLY stub instead of real GL drawing -- same behavior the
# Windows/Vulkan editor.exe has always had for that entry point. That stub
# writes nothing to frame_stats()/the framebuffer, so it is NOT merely lower
# fidelity than real drawing: the render-output assertions in those two files
# (triangles > 0, non-black framebuffer) would deterministically fail, not
# just lose coverage. Both are now guarded out under MATTER_VULKAN_ONLY
# (with a comment at each site) so the suites keep their non-render coverage
# instead of asserting on output nothing produces.
FLAVOR_gpu_FLAGS      := $(filter-out -D$(GRAPHICS),$(CFLAGS)) -D$(GPU_GRAPHICS) \
                         -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB \
                         -DMATTER_VULKAN_ONLY \
                         $(INCLUDE_PATHS) -I.. -I$(BOX3D_DIR)/include $(QJS_INC) $(GPU_AR_CORE_CFLAGS)

# C-source flavors are compiled with gcc (not g++). Split compiler var too.
FLAVOR_mslc_CC        := gcc
FLAVOR_mslc_FLAGS     := -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
FLAVOR_qjsc_CC        := gcc
FLAVOR_qjsc_FLAGS     := -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
FLAVOR_flecsc_CC      := gcc
FLAVOR_flecsc_FLAGS   := -std=c17 -O2 -I$(FLECS_DIR)
FLAVOR_physicsc_CC    := gcc
FLAVOR_physicsc_FLAGS := -std=c17 -O2 -I$(FLECS_DIR)

# C++ flavor list (compiled with $(CC) = g++). Order matters only for pattern-rule
# generation; the alt-compile-cmd rules are all distinct.
CPP_FLAVORS := def qjs sh sh_test sp2 vl retopo gpu
C_FLAVORS   := mslc qjsc flecsc physicsc
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
PARTV2_TARGET = build/part_asset_v2_tests
PARTV2_CPP = part_asset_v2_tests.cpp ../src/part_asset_v2.cpp \
             $(COMMON_MSL_BLAS_SRC)
PARTV2_C   = $(MSL)/material_registry.c

GRAPH_TARGET = build/part_graph_tests
GRAPH_SOURCES = part_graph_tests.cpp ../src/part_graph.cpp ../src/part_asset_v2.cpp \
                $(COMMON_MSL_BLAS_SRC)
GRAPH_C   = $(MSL)/material_registry.c

TRIVAR_TARGET = build/triangle_variation_tests
TRIVAR_CPP = triangle_variation_tests.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp ../src/part_asset_v2.cpp \
             $(COMMON_MSL_BLAS_SRC)
TRIVAR_C   = $(MSL)/material_registry.c

COMP_TARGET = build/composition_tests
COMP_CPP = composition_tests.cpp ../src/lod_bake.cpp ../src/bake_trace.cpp \
           ../src/world_flatten.cpp \
           ../src/sector_grid.cpp ../src/lod_select.cpp ../src/part_asset_v2.cpp \
           $(MSL)/mesh_simplifier.cpp \
           $(MSL)/mesh_indexed.cpp \
           $(MSL)/mesh_transform.cpp \
           $(COMMON_MSL_BLAS_SRC)
COMP_C   = $(MSL)/material_registry.c

FLATTEN_TARGET = build/part_flatten_tests
FLATTEN_CPP = part_flatten_tests.cpp ../src/part_flatten.cpp ../src/lod_bake.cpp \
              ../src/bake_trace.cpp \
              ../src/part_cluster.cpp \
              ../src/part_asset_v2.cpp \
              ../src/retopo_blacklist.cpp \
              $(MSL)/mesh_simplifier.cpp \
              $(MSL)/mesh_indexed.cpp \
              $(MSL)/mesh_transform.cpp \
              $(COMMON_MSL_BLAS_SRC)
FLATTEN_C   = $(MSL)/material_registry.c

TILESETTORUSBVH_TARGET = build/tileset_torus_bvh_tests
TILESETTORUSBVH_CPP    = tileset_torus_bvh_tests.cpp ../src/tileset_torus_bvh.cpp \
                         ../src/part_asset_v2.cpp \
                         $(COMMON_MSL_BLAS_SRC)
TILESETTORUSBVH_C      = $(MSL)/material_registry.c

TILESETMEADOWMANIFEST_TARGET = build/tileset_meadow_manifest_tests
# Needs load_world_definition (script/world_definition_loader.cpp), which in
# turn calls module_resolver::parse_import_specifiers (module_resolver.cpp)
# and links against QuickJS-ng to actually execute the World .js file --
# hence the qjs flavor + qjsc_C_OBJS on TILESETMEADOWMANIFEST_OBJS below.
TILESETMEADOWMANIFEST_CPP = tileset_meadow_manifest_tests.cpp ../src/part_graph.cpp \
                            ../src/part_asset_v2.cpp \
                            ../src/script/world_definition_loader.cpp \
                            ../src/module_resolver.cpp \
                            $(COMMON_MSL_BLAS_SRC)
TILESETMEADOWMANIFEST_C   = $(MSL)/material_registry.c

ISO_TARGET = build/iso_primitive_tests
ISO_CPP = iso_primitive_tests.cpp field_probe.cpp \
          ../src/csg_lowering.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
          ../src/part_asset_v2.cpp \
          $(COMMON_MSL_BLAS_SRC)
ISO_C   = $(MSL)/surface.c $(MSL)/fat_primitive.c \
          $(SQL)/src/spatial_hash.c $(MEMLIB)/src/mem_pool.c \
          $(MSL)/material_registry.c

# Phase 3 (mathlib-and-raylib-removal) determinism harness. Shares field_probe.cpp
# (mesh_field_is_solid) and the same DSL/CSG-lowering sources as ISO_CPP/ISO_C
# above -- deliberately, so it exercises the exact code being migrated.
DSLDET_TARGET = build/dsl_determinism_tests
# dsl_state.cpp routes translate/rotate to clip_translate/clip_rotate while an
# animation clip is open, so the determinism harness now links dsl_animation.cpp.
DSLDET_CPP = dsl_determinism_tests.cpp field_probe.cpp \
             ../src/csg_lowering.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
             ../src/dsl_animation.cpp ../src/animation/animation_ir.cpp \
             ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp \
             ../src/animation/animation_validate.cpp \
             ../src/part_asset_v2.cpp \
             $(COMMON_MSL_BLAS_SRC)
DSLDET_C   = $(MSL)/surface.c $(MSL)/fat_primitive.c \
             $(SQL)/src/spatial_hash.c $(MEMLIB)/src/mem_pool.c \
             $(MSL)/material_registry.c

DEV_TARGET = build/dev_live_edit_tests
DEV_SOURCES = dev_live_edit_tests.cpp ../src/live_edit.cpp ../src/inotify_watcher.cpp

POLYTRI_TARGET = build/polygon_triangulate_tests
POLYTRI_CPP = polygon_triangulate_tests.cpp ../src/polygon_triangulate.cpp

SHADER_SOURCE_TARGET = build/shader_source_tests
SHADER_SOURCE_CPP = shader_source_tests.cpp ../src/shader_source.cpp

ASYNCQ_TARGET = build/async_queue_tests
ASYNCQ_CPP = async_queue_tests.cpp ../src/async_bake.cpp

# Bake Lab task 1.1 — BakeTrace span/counter collector (headless, pure CPU).
BAKETRACE_TARGET = build/bake_trace_tests
BAKETRACE_CPP    = bake_trace_tests.cpp ../src/bake_trace.cpp

TILESETGTEX_TARGET = build/tileset_gtex_tests
TILESETGTEX_CPP    = tileset_gtex_tests.cpp ../src/tileset_gtex.cpp

# Vulkan .gtex bake (V1) readback->save_gtex repack — pure CPU (header-only
# inline helpers from ../src/render/tileset_bake_vk.h, no Vulkan link).
TILESETVKREPACK_TARGET = build/tileset_bake_vk_repack_tests
TILESETVKREPACK_CPP    = tileset_bake_vk_repack_tests.cpp

# Vulkan .gtex bake (V2) AO seam-invariance mechanism + ORM.r fold — pure CPU
# (ported splitmix32/u01/cosine_hemi/make_basis + header-only gtex_pack_orm_ao,
# no Vulkan link).
TILESETVKAO_TARGET = build/tileset_bake_vk_ao_tests
TILESETVKAO_CPP    = tileset_bake_vk_ao_tests.cpp

# Vulkan .gtex bake (V3) horizon pass -- pure CPU (header-only inline
# gtex_bake_horizon_cpu from ../src/render/tileset_bake_vk.h), no Vulkan link.
TILESETVKHORIZON_TARGET = build/tileset_bake_vk_horizon_tests
TILESETVKHORIZON_CPP    = tileset_bake_vk_horizon_tests.cpp

# Volumetrics Task 3 — CPU emitter gather (distance filter, 256 cap).
EMITTERGATHER_TARGET = build/emitter_gather_tests
EMITTERGATHER_CPP    = emitter_gather_tests.cpp ../src/render/vk_emitter_gather.cpp

# Phase 4 Task 4 — SceneRegistry reflected ECS validation + instantiation (def flavor).
SCENE_REGISTRY_TARGET = build/scene_registry_tests
SCENE_REGISTRY_CPP    = scene_registry_tests.cpp $(ANIMATION_ECS_RUNTIME_REQUIRED) ../src/ecs/scene_registry.cpp \
                        ../src/ecs/ecs_runtime.cpp ../src/ecs/physics_context.cpp \
                        ../src/ecs/physics_shapes.cpp ../src/ecs/physics_systems.cpp \
                        ../src/ecs/streaming_systems.cpp ../src/ecs/transform_system.cpp \
                        ../src/streaming/sector_streaming_coordinator.cpp \
                        ../src/sector_streamer.cpp
SCENE_REGISTRY_C      = $(FLECS_C)

# Phase 4 Task 6 — CPU stable dynamic instance slots (pure CPU, def flavor,
# no Flecs/physics/ECS deps).
DYNAMIC_SLOTS_TARGET = build/dynamic_instance_slots_tests
DYNAMIC_SLOTS_CPP    = dynamic_instance_slots_tests.cpp ../src/render/dynamic_instance_slots.cpp

# Phase 4 Task 5 — Recipe normalization + transactional bootstrap (def flavor).
ENTITY_RECIPE_TARGET = build/entity_recipe_tests
# ANIMATION_STORE_CPP already carries the full scene_registry/ecs_runtime/
# physics/streaming closure main used to list explicitly here, so reuse it
# rather than re-listing: obj_list does not dedupe, and naming the same source
# twice makes ld see the object twice ("multiple definition").
ENTITY_RECIPE_CPP    = entity_recipe_tests.cpp \
                       $(filter-out animation_store_tests.cpp,$(ANIMATION_STORE_CPP))
ENTITY_RECIPE_C      = $(FLECS_C)

# Phase 4 Task 8 — ECS dynamic scene bridge (headless, Flecs, no GL/Vulkan).
DYNAMIC_BRIDGE_TARGET = build/dynamic_bridge_tests
DYNAMIC_BRIDGE_CPP    = dynamic_scene_bridge_tests.cpp ../src/animation/animation_store.cpp ../src/animation/animation_asset_store.cpp \
                        ../src/animation/animation_evaluator.cpp ../src/animation/animation_systems.cpp \
                        ../src/animation/animation_controllers.cpp ../src/animation/animation_targets.cpp ../src/animation/animation_budget.cpp \
                        ../src/animation/animation_world_queries.cpp ../src/animation/ozz_adapter.cpp \
                        ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp ../src/ecs/dynamic_scene_bridge.cpp \
                        ../src/render/animation_rigid_bridge.cpp ../src/render/animation_skin_bridge.cpp \
                        ../src/render/vk_animation_bounds.cpp ../src/render/dynamic_instance_slots.cpp \
                        ../src/ecs/scene_registry.cpp \
                        ../src/ecs/ecs_runtime.cpp ../src/ecs/physics_context.cpp \
                        ../src/ecs/physics_shapes.cpp ../src/ecs/physics_systems.cpp \
                        ../src/ecs/streaming_systems.cpp ../src/ecs/transform_system.cpp \
                        ../src/streaming/sector_streaming_coordinator.cpp \
                        ../src/sector_streamer.cpp
DYNAMIC_BRIDGE_C      = $(FLECS_C)

# Runtime::attach_animation_skinned_binding and DynamicSceneBridge's skin path
# are linked by this otherwise headless target. Keep their implementation pair
# explicit: the skin bridge validates bounds through the bounds implementation.
DYNAMIC_BRIDGE_RENDER_REQUIRED = ../src/render/animation_skin_bridge.cpp \
                                 ../src/render/vk_animation_bounds.cpp

# Phase B6 — expansion of authored rigid/attachment bindings into the
# renderer's existing dynamic-instance lane.  This is deliberately headless:
# the bridge owns no Flecs or evaluator state.
ANIMATION_RIGID_TARGET = build/animation_rigid_bridge_tests
ANIMATION_RIGID_CPP    = animation_rigid_bridge_tests.cpp ../src/render/animation_rigid_bridge.cpp \
                        ../src/render/dynamic_instance_slots.cpp \
                        ../src/animation/animation_store.cpp ../src/animation/animation_asset_store.cpp \
                        ../src/animation/animation_systems.cpp ../src/animation/animation_evaluator.cpp \
                        ../src/animation/animation_targets.cpp ../src/animation/animation_controllers.cpp ../src/animation/animation_budget.cpp \
                        ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp
ANIMATION_RIGID_C      = $(FLECS_C)

ANIMATION_SKIN_TARGET = build/animation_skin_bridge_tests
ANIMATION_SKIN_CPP    = animation_skin_bridge_tests.cpp ../src/render/animation_skin_bridge.cpp ../src/render/vk_animation_bounds.cpp \
                        ../src/animation/animation_store.cpp ../src/animation/animation_asset_store.cpp \
                        ../src/animation/animation_systems.cpp ../src/animation/animation_evaluator.cpp \
                        ../src/animation/animation_targets.cpp ../src/animation/animation_controllers.cpp ../src/animation/animation_budget.cpp \
                        ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/animation/animation_ir.cpp
ANIMATION_SKIN_C      = $(FLECS_C)

# Phase 4 Task 9 — Editor model (pure CPU, no GL/Vulkan/Flecs needed).
EDITOR_MODEL_TARGET = build/editor_model_tests
EDITOR_MODEL_CPP    = editor_model_tests.cpp ../../MatterEditor/src/editor_model.cpp

# Phase 4 Task 10 — Properties registry (needs scene_registry for ComponentDescriptor).
PROPERTIES_REGISTRY_TARGET = build/properties_registry_tests
PROPERTIES_REGISTRY_CPP    = properties_registry_tests.cpp $(ANIMATION_ECS_RUNTIME_REQUIRED) ../../MatterEditor/src/properties_registry.cpp \
                             ../src/ecs/scene_registry.cpp \
                             ../src/ecs/ecs_runtime.cpp ../src/ecs/physics_context.cpp \
                             ../src/ecs/physics_shapes.cpp ../src/ecs/physics_systems.cpp \
                             ../src/ecs/streaming_systems.cpp ../src/ecs/transform_system.cpp \
                             ../src/streaming/sector_streaming_coordinator.cpp \
                             ../src/sector_streamer.cpp
PROPERTIES_REGISTRY_C      = $(FLECS_C)

# Phase 4 Task 11 — Simulation control (Play/Pause/Step/Stop with snapshot).
SIMULATION_CONTROL_TARGET = build/simulation_control_tests
SIMULATION_CONTROL_CPP    = simulation_control_tests.cpp $(ANIMATION_ECS_RUNTIME_REQUIRED) ../src/ecs/simulation_control.cpp \
                            ../src/ecs/scene_registry.cpp \
                            ../src/ecs/ecs_runtime.cpp ../src/ecs/physics_context.cpp \
                            ../src/ecs/physics_shapes.cpp ../src/ecs/physics_systems.cpp \
                            ../src/ecs/streaming_systems.cpp ../src/ecs/transform_system.cpp \
                            ../src/streaming/sector_streaming_coordinator.cpp \
                            ../src/sector_streamer.cpp
SIMULATION_CONTROL_C      = $(FLECS_C)

# Phase 4 Task 12 — Specialized editors (pure CPU, no Flecs/GL).
SPECIALIZED_EDITORS_TARGET = build/specialized_editors_tests
SPECIALIZED_EDITORS_CPP    = specialized_editors_tests.cpp \
                             ../../MatterEditor/src/specialized_editors.cpp \
                             ../../MatterEditor/src/properties_registry.cpp \
                             ../src/ecs/scene_registry.cpp
SPECIALIZED_EDITORS_C      = $(FLECS_C)

# Phase 5 — entity -> ECS bridge (Ready world-state commands bootstrap Flecs
# entities via scene::bootstrap_transactional). Needs the full Runtime, same
# link set as SIMULATION_CONTROL.
ECS_ENTITY_BRIDGE_TARGET = build/ecs_entity_bridge_tests
ECS_ENTITY_BRIDGE_CPP    = ecs_entity_bridge_tests.cpp $(ANIMATION_ECS_RUNTIME_REQUIRED) \
                           ../src/ecs/scene_registry.cpp \
                           ../src/ecs/ecs_runtime.cpp ../src/ecs/physics_context.cpp \
                           ../src/ecs/physics_shapes.cpp ../src/ecs/physics_systems.cpp \
                           ../src/ecs/streaming_systems.cpp ../src/ecs/transform_system.cpp \
                           ../src/streaming/sector_streaming_coordinator.cpp \
                           ../src/sector_streamer.cpp
ECS_ENTITY_BRIDGE_C      = $(FLECS_C)

# Phase C Task 4 — RefineController tile-pairing + priority/eviction (pure CPU, def flavor).
REFINECTL_TARGET = build/refine_controller_tests
REFINECTL_CPP    = refine_controller_tests.cpp ../src/refine_controller.cpp \
                   ../src/part_graph_snapshot.cpp

# Phase C Task 8 — SectorStreamer (pure CPU, no JS, no GL).
SECTORSTREAM_TARGET = build/sector_streamer_tests
SECTORSTREAM_CPP    = sector_streamer_tests.cpp ../src/sector_streamer.cpp

# Phase 3 Task 2 - worker-only streaming coordinator (CPU-only).
SECTORCOORD_TARGET = build/sector_streaming_coordinator_tests
SECTORCOORD_CPP    = sector_streaming_coordinator_tests.cpp \
                     ../src/streaming/sector_streaming_coordinator.cpp \
                     ../src/sector_streamer.cpp
SECTORCOORD_C      = $(FLECS_C)

# Phase C Task 17 — resolve/manifest cache (headless, no GL needed).
RESOLVECACHE_TARGET = build/resolve_cache_tests
RESOLVECACHE_CPP    = resolve_cache_tests.cpp \
                      ../src/resolve_cache.cpp \
                      ../src/part_graph.cpp \
                      ../src/part_graph_snapshot.cpp \
                      ../src/part_asset_v2.cpp \
                      $(COMMON_MSL_BLAS_SRC)
RESOLVECACHE_C      = $(MSL)/material_registry.c

MODAPPLY_TARGET = build/modifier_apply_tests
MODAPPLY_CPP = modifier_apply_tests.cpp ../src/modifier_apply.cpp \
               ../src/retopo_blacklist.cpp \
               $(MSL)/mesh_smooth.cpp $(MSL)/mesh_simplifier.cpp $(MSL)/mesh_indexed.cpp

# Phase C Task 3 — terrain field program interpreter (pure CPU, no JS, no GL).
TERRAINFIELD_TARGET = build/terrain_field_tests
TERRAINFIELD_CPP    = terrain_field_tests.cpp ../src/terrain_field.cpp

# Phase C Task 5 — terrain mesher (pure CPU: terrain_field + terrain_mesher, no JS, no GL).
TERRAINMESH_TARGET = build/terrain_mesher_tests
TERRAINMESH_CPP    = terrain_mesher_tests.cpp ../src/terrain_field.cpp ../src/terrain_mesher.cpp

# Phase C Task 5 — terrain verb (sh flavor: bake_source + WorldBinding + terrainVolume).
TERRAINVERB_TARGET = build/terrain_verb_tests
TERRAINVERB_CPP = terrain_verb_tests.cpp ../src/part_graph.cpp ../src/bake_trace.cpp \
                  ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
                  ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
                  ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
                  ../src/modifier_apply.cpp ../src/retopo_blacklist.cpp $(MSL)/mesh_smooth.cpp \
                  ../src/terrain_field.cpp ../src/terrain_mesher.cpp \
                  ../src/pf_bindings.cpp \
                  ../../libs/ParticleFlowLib/src/pf_math.cpp ../../libs/ParticleFlowLib/src/pf_sim.cpp \
                  ../../libs/ParticleFlowLib/src/pf_fields.cpp ../../libs/ParticleFlowLib/src/pf_path_recorder.cpp \
                  $(COMMON_MSL_FULL_SRC)
TERRAINVERB_C = $(COMMON_MSL_C)

# Phase C Task 7 — WorldSector.js + native world query verbs (sh flavor).
SECTORBAKE_TARGET = build/sector_bake_tests
SECTORBAKE_CPP = sector_bake_tests.cpp ../src/part_graph.cpp ../src/bake_trace.cpp \
                 ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
                 ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
                 ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
                 ../src/modifier_apply.cpp ../src/retopo_blacklist.cpp $(MSL)/mesh_smooth.cpp \
                 $(TERRAIN_SRC) \
                 $(PF_CPP) \
                 $(COMMON_MSL_FULL_SRC)
SECTORBAKE_C = $(COMMON_MSL_C)

PARTSTORE_TARGET = build/partstore_tests
PARTSTORE_CPP = partstore_tests.cpp ../src/render/part_store.cpp \
                ../src/render/matrix_math.cpp \
                ../src/render/raster_mesh.cpp ../src/lod_select.cpp \
                ../src/sector_grid.cpp ../src/lod_bake.cpp ../src/bake_trace.cpp \
                ../src/part_asset_v2.cpp \
                ../src/animation/animation_asset_store.cpp ../src/animation/anim_asset.cpp \
                ../src/animation/anim_bundle.cpp ../src/animation/animation_binding_bake.cpp \
                ../src/animation/animation_ir.cpp ../src/render/indexed_part_geometry.cpp \
                $(MSL)/mesh_simplifier.cpp $(MSL)/mesh_indexed.cpp \
                $(MSL)/mesh_transform.cpp \
                $(COMMON_MSL_BLAS_SRC)
PARTSTORE_C   = $(MSL)/material_registry.c

# Simple standalone targets (they use tiny custom flag sets — kept as-is with
# per-target rules below, no shared objects).

# ===== flavor sh (script-host) =========================================
# script_host_tests uses just def+QJS_INC (no MATTER_HAVE_SCRIPT_HOST). But
# the graph_integration/example/gallery/treebake/grasslod/stressforest
# family all use -DMATTER_HAVE_SCRIPT_HOST + QJS_INC.
SCRIPT_TARGET = build/script_host_tests
SCRIPT_CPP = script_host_tests.cpp ../src/part_graph.cpp ../src/bake_trace.cpp ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_animation.cpp ../src/animation/animation_ir.cpp ../src/animation/animation_validate.cpp ../src/animation/animation_binding_bake.cpp ../src/animation/animation_runtime_asset.cpp ../src/animation/animation_evaluator.cpp ../src/animation/animation_controllers.cpp ../src/animation/animation_targets.cpp ../src/animation/animation_budget.cpp ../src/animation/anim_asset.cpp ../src/animation/anim_bundle.cpp ../src/render/indexed_part_geometry.cpp ../src/animation/ozz_adapter.cpp ../src/animation/ozz_bake.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
             ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
             ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
             ../src/modifier_apply.cpp ../src/retopo_blacklist.cpp $(MSL)/mesh_smooth.cpp \
             $(TERRAIN_SRC) \
             ../src/pf_bindings.cpp \
             ../../libs/ParticleFlowLib/src/pf_math.cpp ../../libs/ParticleFlowLib/src/pf_sim.cpp \
             ../../libs/ParticleFlowLib/src/pf_fields.cpp ../../libs/ParticleFlowLib/src/pf_path_recorder.cpp \
             $(COMMON_MSL_FULL_SRC)
SCRIPT_C   = $(COMMON_MSL_C)

# Modifier-region bake plumbing (Task 4): reuses the script-host object set
# with a different top-level .cpp.
MODBAKE_TARGET = build/modifier_region_bake_tests
MODBAKE_CPP = modifier_region_bake_tests.cpp $(filter-out script_host_tests.cpp, $(SCRIPT_CPP))
MODBAKE_C   = $(COMMON_MSL_C)

PF_LIB = ../../libs/ParticleFlowLib
# any target that links dsl_bindings.cpp must also link pf_bindings.cpp + ParticleFlowLib sources
PF_CPP = ../src/pf_bindings.cpp \
         $(PF_LIB)/src/pf_math.cpp \
         $(PF_LIB)/src/pf_sim.cpp \
         $(PF_LIB)/src/pf_fields.cpp \
         $(PF_LIB)/src/pf_path_recorder.cpp

# any target that links dsl_bindings.cpp must also link the terrain sources
# (dsl_bindings.cpp now registers the terrainVolume verb which calls mesh_sector)
TERRAIN_SRC = ../src/terrain_field.cpp ../src/terrain_mesher.cpp

GRAPH_INT_TARGET = build/part_graph_integration_tests
# SCRIPT_CPP is a superset of the explicit part_graph/bake_trace/script_host/
# dsl/tileset/terrain/PF/MSL list this used to spell out, and additionally
# carries the animation-authoring closure this suite now needs. The extra
# render/LOD sources below are the A8 additions (part_store.h consumers).
GRAPH_INT_CPP = part_graph_integration_tests.cpp $(filter-out script_host_tests.cpp,$(SCRIPT_CPP)) \
                ../src/render/part_store.cpp ../src/render/matrix_math.cpp \
                ../src/render/raster_mesh.cpp ../src/lod_select.cpp \
                ../src/sector_grid.cpp ../src/animation/animation_asset_store.cpp
GRAPH_INT_C   = $(COMMON_MSL_C)

EXAMPLE_TARGET = build/example_world
# example_world.cpp, gallery_bake_tests.cpp and lighting_garden_tests.cpp (all
# built from this list -- see the *_CPP = ... $(EXAMPLE_CPP) targets below)
# call matter::load_world_definition directly, and script_host.h's inline
# ScriptHost::eval_world_definition() calls it too; script/world_definition_loader.cpp
# supplies it (same missing-source pattern as TILESETMEADOWMANIFEST_CPP above).
EXAMPLE_CPP = example_world.cpp ../src/part_graph.cpp ../src/bake_trace.cpp \
              ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp ../src/dsl_bindings.cpp \
              ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/script_rng_binding.cpp \
              ../src/script/world_definition_loader.cpp \
              ../src/part_asset_v2.cpp ../src/lod_bake.cpp ../src/world_flatten.cpp ../src/part_flatten.cpp \
              ../src/retopo_blacklist.cpp \
              ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
              ../src/part_cluster.cpp \
              ../src/sector_grid.cpp ../src/lod_select.cpp \
              ../src/modifier_apply.cpp $(MSL)/mesh_smooth.cpp \
              $(TERRAIN_SRC) \
              $(PF_CPP) \
              $(COMMON_MSL_FULL_SRC)
EXAMPLE_C   = $(COMMON_MSL_C)

GALLERY_TARGET = build/gallery_bake_tests
GALLERY_CPP = gallery_bake_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))

TREEBAKE_TARGET = build/tree_bake_check
TREEBAKE_CPP = tree_bake_check.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))

ROCKPROF_TARGET = build/rock_bake_profile
ROCKPROF_CPP = rock_bake_profile.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))

GRASSLOD_TARGET = build/grass_lod_tests
GRASSLOD_CPP = grass_lod_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))

STRESSFOREST_TARGET = build/stress_forest_tests
STRESSFOREST_CPP = stress_forest_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))

LIGHTING_GARDEN_TARGET = build/lighting_garden_tests
LIGHTING_GARDEN_CPP = lighting_garden_tests.cpp $(filter-out example_world.cpp, $(EXAMPLE_CPP))
LIGHTING_GARDEN_C = $(COMMON_MSL_C)

# ===== Task 9: live-edit production seams ==================================
LIVEPROD_TARGET = build/live_edit_prod_tests
LIVEPROD_CPP = live_edit_prod_tests.cpp \
               ../src/live_edit_prod.cpp \
               ../src/part_graph_snapshot.cpp \
               $(filter-out example_world.cpp, $(EXAMPLE_CPP))
LIVEPROD_C   = $(COMMON_MSL_C)

TILESETBAKE_TARGET = build/tileset_bake_tests
TILESETBAKE_CPP = tileset_bake_tests.cpp ../src/part_graph.cpp ../src/bake_trace.cpp \
                  ../src/tileset_bake.cpp ../src/tileset_settle.cpp \
                  ../src/tileset_collider.cpp ../src/tileset_part_collider.cpp \
                  ../src/tileset_phase.cpp \
                  ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp \
                  ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
                  ../src/dsl_bindings.cpp ../src/csg_lowering.cpp \
                  ../src/module_resolver.cpp ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
                  ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
                  ../src/modifier_apply.cpp ../src/retopo_blacklist.cpp $(MSL)/mesh_smooth.cpp \
                  $(TERRAIN_SRC) \
                  $(PF_CPP) \
                  $(COMMON_MSL_FULL_SRC)
TILESETBAKE_C   = $(COMMON_MSL_C)

TILESETPARAMS_TARGET = build/tileset_params_tests
TILESETPARAMS_CPP = tileset_params_tests.cpp $(filter-out tileset_bake_tests.cpp,$(TILESETBAKE_CPP))
TILESETPARAMS_C = $(TILESETBAKE_C)

# Bake Lab task 5.4 — settle_bench headless benchmark CLI. Pulls in
# build_settle_plan's dependencies (same as TILESETBAKE_CPP: full script host +
# part_graph + tileset_bake/settle/collider) plus tileset_metrics.cpp for the
# pose-delta compare gate.
SETTLEBENCH_TARGET = build/settle_bench
SETTLEBENCH_CPP = settle_bench.cpp $(filter-out tileset_bake_tests.cpp,$(TILESETBAKE_CPP)) \
                  ../src/tileset_metrics.cpp
SETTLEBENCH_C = $(TILESETBAKE_C)

# ===== flavor qjs (script_host_tests + tileset_dsl_tests + terrainnoise) ===
# NOTE: script_host_tests originally compiled with CFLAGS + INCLUDE_PATHS +
# QJS_INC (no -DMATTER_HAVE_SCRIPT_HOST). That maps to `qjs`, not `sh`.
TILESETDSL_TARGET = build/tileset_dsl_tests
TILESETDSL_CPP = tileset_dsl_tests.cpp $(filter-out script_host_tests.cpp, $(SCRIPT_CPP))

# Phase C Task 1: terrain noise contract (qjs flavor — module_resolver + QJS only).
TERRAINNOISE_TARGET = build/terrain_noise_tests
TERRAINNOISE_CPP = terrain_noise_tests.cpp ../src/module_resolver.cpp

# Phase 4 Task 1: hermetic World JavaScript statics contract.
WORLD_DEFINITION_TARGET = build/world_definition_tests
WORLD_DEFINITION_CPP = world_definition_tests.cpp \
                       ../src/script/world_definition_loader.cpp \
                       ../src/module_resolver.cpp

# Phase 4 Task 13: PhysicsPlayground world and closure.
PHYSICS_PLAYGROUND_TARGET = build/physics_playground_tests
PHYSICS_PLAYGROUND_CPP    = physics_playground_tests.cpp \
                            ../src/script/world_definition_loader.cpp \
                            ../src/module_resolver.cpp

# Phase C Task 4 — eval_world: World builder DSL + ScriptHost::eval_world (sh flavor).
EVALWORLD_TARGET = build/eval_world_tests
EVALWORLD_CPP = eval_world_tests.cpp ../src/part_graph.cpp ../src/bake_trace.cpp \
                ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
                ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
                ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
                ../src/modifier_apply.cpp ../src/retopo_blacklist.cpp $(MSL)/mesh_smooth.cpp \
                $(TERRAIN_SRC) \
                ../src/pf_bindings.cpp \
                ../../libs/ParticleFlowLib/src/pf_math.cpp ../../libs/ParticleFlowLib/src/pf_sim.cpp \
                ../../libs/ParticleFlowLib/src/pf_fields.cpp ../../libs/ParticleFlowLib/src/pf_path_recorder.cpp \
                $(COMMON_MSL_FULL_SRC)
EVALWORLD_C = $(COMMON_MSL_C)

# Phase C Task 1: fold cache in ScriptHost (sh flavor).
FOLDCACHE_TARGET = build/fold_cache_tests
FOLDCACHE_CPP = fold_cache_tests.cpp ../src/part_graph.cpp ../src/bake_trace.cpp \
                ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
                ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
                ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
                ../src/modifier_apply.cpp ../src/retopo_blacklist.cpp $(MSL)/mesh_smooth.cpp \
                $(TERRAIN_SRC) \
                ../src/pf_bindings.cpp \
                ../../libs/ParticleFlowLib/src/pf_math.cpp ../../libs/ParticleFlowLib/src/pf_sim.cpp \
                ../../libs/ParticleFlowLib/src/pf_fields.cpp ../../libs/ParticleFlowLib/src/pf_path_recorder.cpp \
                $(COMMON_MSL_FULL_SRC)
FOLDCACHE_C = $(COMMON_MSL_C)

# ===== flavor sp2 (SP2_SCRIPT_HOST) =====================================
SHLIB_TARGET = build/shared_lib_tests
SHLIB_CPP = shared_lib_tests.cpp ../src/module_resolver.cpp ../src/script_rng_binding.cpp \
            ../src/bake_trace.cpp \
            ../src/part_graph.cpp \
            ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp ../src/dsl_bindings.cpp ../src/csg_lowering.cpp \
            ../src/part_asset_v2.cpp ../src/lod_bake.cpp ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
            ../src/modifier_apply.cpp ../src/retopo_blacklist.cpp $(MSL)/mesh_smooth.cpp \
            $(TERRAIN_SRC) \
            $(PF_CPP) \
            $(COMMON_MSL_FULL_SRC)
SHLIB_C   = $(COMMON_MSL_C)

# ===== flavor vl (viewer_logic) =========================================
VIEWER_LOGIC_TARGET = build/viewer_logic_tests
# viewer_logic exercises the bake host through ScriptHost, so it owns the
# complete animation-authoring closure (including Ozz's bake-time compiler),
# not merely the Viewer runtime adapter. Keep this list explicit and guard it
# below: a missing TU otherwise appears only as a platform-specific link error.
VIEWER_LOGIC_ANIMATION_BAKE_CPP = \
              ../src/dsl_animation.cpp \
              ../src/animation/animation_ir.cpp \
              ../src/animation/animation_validate.cpp \
              ../src/animation/animation_binding_bake.cpp \
              ../src/animation/animation_runtime_asset.cpp \
              ../src/animation/anim_asset.cpp \
              ../src/animation/anim_bundle.cpp \
              ../src/animation/animation_asset_store.cpp \
              ../src/animation/animation_controllers.cpp \
              ../src/animation/animation_targets.cpp \
              ../src/animation/animation_evaluator.cpp \
              ../src/animation/animation_budget.cpp \
              ../src/animation/ozz_adapter.cpp \
              ../src/animation/ozz_bake.cpp \
              ../src/render/indexed_part_geometry.cpp
VIEWER_LOGIC_ANIMATION_REQUIRED = dsl_animation.cpp animation_ir.cpp \
              animation_validate.cpp animation_binding_bake.cpp \
              animation_runtime_asset.cpp anim_asset.cpp anim_bundle.cpp \
              animation_asset_store.cpp animation_controllers.cpp animation_targets.cpp \
              animation_evaluator.cpp animation_budget.cpp \
              ozz_adapter.cpp ozz_bake.cpp indexed_part_geometry.cpp
# $(sort) is load-bearing: EXAMPLE_CPP also picks up ANIMATION_BAKE_HOST_CPP via
# the += block below, so the two lists overlap. obj_list does not dedupe, and a
# repeated source makes ld see the same object twice ("multiple definition").
VIEWER_PIPELINE_CPP = $(sort $(filter-out example_world.cpp, $(EXAMPLE_CPP)) \
              $(VIEWER_LOGIC_ANIMATION_BAKE_CPP))
# NOTE: do not add ../src/script/world_definition_loader.cpp to the explicit
# list below -- it now arrives transitively via $(VIEWER_PIPELINE_CPP), which
# pulls in EXAMPLE_CPP (world_definition_loader.cpp landed there so
# example_world/gallery_bake_tests/lighting_garden_tests could link). obj_list
# (:261) does not dedupe, so listing it in both places causes ld to see the
# same object twice -> "multiple definition of matter::load_world_definition".
VIEWER_LOGIC_CPP = viewer_logic_tests.cpp \
              ../../MatterEditor/src/camera_controller.cpp \
              ../../MatterEditor/src/streaming_anchor_controller.cpp \
              ../src/async_bake.cpp \
              ../src/render/matrix_math.cpp ../src/render/frame_matrices.cpp \
              ../src/render/world_state.cpp \
              ../src/provider/resolvers.cpp \
              ../src/render/part_store.cpp \
              ../src/provider/local_provider.cpp \
              ../src/render/world_composer.cpp \
              ../src/render/raster_mesh.cpp \
              ../src/render/tileset_provider.cpp \
              ../src/tileset_phase.cpp \
              ../src/tileset_settle.cpp ../src/tileset_gtex.cpp \
              ../src/tileset_bake.cpp ../src/tileset_collider.cpp \
              ../src/tileset_part_collider.cpp ../src/tileset_torus_bvh.cpp \
              ../src/world_lights.cpp \
              ../src/world_tracer.cpp \
              ../src/shader_source.cpp \
              $(VIEWER_PIPELINE_CPP)

# ===== flavor retopo (retopo_integration_tests) ========================
# The test calls script_host::ScriptHost::bake_source(), which requires the
# full script-host + DSL + modifier_apply source set.  Build with the retopo
# flavor (-DMATTER_HAVE_AUTOREMESHER) so modifier_apply dispatches retopo.
# mesh_retopo.cpp is the only autoremesher-specific addition over MODBAKE_CPP.
RETOPO_INT_TARGET = build/retopo_integration_tests
RETOPO_INT_CPP = retopo_integration_tests.cpp ../src/bake_trace.cpp \
                 ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp \
                 ../src/triangle_emit.cpp ../src/polygon_triangulate.cpp \
                 ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/module_resolver.cpp \
                 ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
                 ../src/part_flatten.cpp ../src/part_cluster.cpp \
                 ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
                 ../src/modifier_apply.cpp ../src/retopo_blacklist.cpp \
                 $(MSL)/mesh_retopo.cpp \
                 $(MSL)/mesh_smooth.cpp \
                 $(TERRAIN_SRC) \
                 $(PF_CPP) \
                 $(COMMON_MSL_FULL_SRC)
RETOPO_INT_C   = $(COMMON_MSL_C)
# --allow-multiple-definition: raylib bundles stb_image_write.h in rtextures.c
# and autoremesher_core's geogram bundles its own copy in
# image_serializer_stb.o. Both archives get pulled in here.
RETOPO_INT_LDFLAGS = $(LDFLAGS) \
                     -L$(AR_CORE_PATH) -lautoremesher_core \
                     -L$(TBB_BUILD_DIR) -ltbb \
                     -Wl,-rpath,'$$ORIGIN/../../third_party/autoremesher_core/thirdparty/tbb/build/$(TBB_BUILD_SUBDIR)' \
                     -Wl,--allow-multiple-definition

# ===== flavor gpu (GPU tests) ==========================================
# Phase 5a (tech-debt.md §6) deleted the GL renderer/raster-composer/GpuCuller
# path outright (src/render/renderer.*, raster_composer.*, gpu_culler.*).
# FLAVOR_gpu_FLAGS now defines MATTER_VULKAN_ONLY so matter_engine.cpp's
# facade compiles without those types (mirrors MatterEngine3/Makefile's fix
# and the already-tested Windows/Vulkan editor.exe build). gpu_cull_tests.cpp
# and release_part_tests.cpp tested those deleted classes directly and were
# retired with them (see MatterEngine3/tests/Makefile git history).
GPU_RENDER_CPP = \
    ../src/async_bake.cpp \
    ../src/animation/animation_budget.cpp \
    ../src/render/vk_animation_skinning.cpp \
    ../src/bake_trace.cpp \
    ../src/render/matrix_math.cpp ../src/render/frame_matrices.cpp \
    ../src/render/raster_mesh.cpp ../src/render/part_store.cpp \
    ../src/render/world_composer.cpp ../src/render/world_state.cpp \
    ../src/render/tileset_provider.cpp \
    ../src/provider/local_provider.cpp ../src/provider/resolvers.cpp \
    ../src/matter_engine.cpp \
    ../src/event/event_hub.cpp \
    ../src/event/subscription.cpp \
    ../src/event/dispatch_context.cpp \
    ../src/scene/scene_service.cpp \
    ../src/scene/scene_change_tracker.cpp \
    ../src/live_edit.cpp \
    ../src/live_edit_prod.cpp \
    ../src/inotify_watcher.cpp \
    ../src/part_graph_snapshot.cpp \
    ../src/refine_controller.cpp \
    ../src/resolve_cache.cpp \
    ../src/sector_streamer.cpp

GPU_PIPELINE_CPP = \
    ../src/part_graph.cpp \
    ../src/script_host.cpp ../src/script/world_definition_loader.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp \
    ../src/dsl_bindings.cpp ../src/csg_lowering.cpp ../src/module_resolver.cpp \
    ../src/script_rng_binding.cpp \
    ../src/polygon_triangulate.cpp \
    ../src/part_asset_v2.cpp ../src/lod_bake.cpp \
    ../src/world_flatten.cpp ../src/part_flatten.cpp ../src/triangle_emit.cpp \
    ../src/part_cluster.cpp \
    ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
    ../src/tileset_torus_bvh.cpp \
    ../src/tileset_gtex.cpp \
    ../src/sector_grid.cpp ../src/lod_select.cpp \
    ../src/world_lights.cpp \
    ../src/world_tracer.cpp \
    ../src/shader_source.cpp \
    ../src/tileset_phase.cpp \
    ../src/tileset_bake.cpp ../src/tileset_settle.cpp \
    ../src/tileset_collider.cpp ../src/tileset_part_collider.cpp \
    ../src/modifier_apply.cpp ../src/retopo_blacklist.cpp $(MSL)/mesh_smooth.cpp \
    $(TERRAIN_SRC) \
    $(PF_CPP)

GPU_MSL_CPP = \
    $(MSL)/cluster.cpp $(MSL)/cell.cpp \
    $(MSL)/mesh_simplifier.cpp $(MSL)/mesh_indexed.cpp $(MSL)/mesh_transform.cpp \
    $(MSL)/blas_manager.cpp $(SQL)/src/bvh.cpp $(SQL)/src/bvh_analyzer.cpp \
    $(MSL)/mesh_worker_pool.cpp $(MSL)/mesh_build_utils.cpp \
    $(MSL)/meshing_algorithm.cpp $(MSL)/marching_cubes_algorithm.cpp \
    $(MSL)/oriented_cube_algorithm.cpp $(MSL)/vertex_ao.cpp \
    $(MSL)/occupancy.cpp $(MSL)/tlas_manager.cpp $(MSL)/part_asset.cpp \
    $(MSL)/lattice.cpp $(MSL)/particle_culling.cpp \
    $(GPU_AR_CORE_CPP)

GPU_MSL_C   = $(COMMON_MSL_C)
GPU_ANIMATION_RUNTIME_CPP = \
    $(filter-out animation_simulation_tests.cpp ../src/ecs/simulation_control.cpp,$(ANIMATION_SIMULATION_CPP)) \
    ../src/dsl_animation.cpp \
    ../src/animation/animation_runtime_asset.cpp \
    ../src/animation/animation_binding_bake.cpp \
    ../src/animation/animation_validate.cpp \
    ../src/animation/anim_asset.cpp \
    ../src/animation/anim_bundle.cpp \
    ../src/render/indexed_part_geometry.cpp
GPU_ALL_CPP = $(GPU_PIPELINE_CPP) $(GPU_RENDER_CPP) $(GPU_MSL_CPP) \
              $(GPU_ANIMATION_RUNTIME_CPP) \
              ../src/render/vk_animation_bounds.cpp \
              ../src/ecs/scene_registry.cpp \
              ../src/animation/animation_systems.cpp ../src/ecs/ecs_runtime.cpp ../src/ecs/physics_context.cpp \
              ../src/ecs/physics_shapes.cpp ../src/ecs/physics_systems.cpp \
              ../src/ecs/streaming_systems.cpp ../src/ecs/transform_system.cpp \
              ../src/streaming/sector_streaming_coordinator.cpp

# ---------------------------------------------------------------------------
# Bake-host closure fan-out.
#
# Every list below compiles script_host.cpp and/or part_graph.cpp, which now
# reference the animation bake host (authored-rig compilation, .part/.anim
# sibling publication, committed-bundle load). Appended with += rather than
# spliced into each multi-line list so the closure has exactly one definition;
# the per-flavor unions below are $(sort ...)ed, which also dedupes.
# SCRIPT_CPP already carries the closure inline and is deliberately absent.
# ---------------------------------------------------------------------------
TERRAINVERB_CPP  += $(ANIMATION_BAKE_HOST_CPP)
SECTORBAKE_CPP   += $(ANIMATION_BAKE_HOST_CPP)
EXAMPLE_CPP      += $(ANIMATION_BAKE_HOST_CPP)
TILESETBAKE_CPP  += $(ANIMATION_BAKE_HOST_CPP)
EVALWORLD_CPP    += $(ANIMATION_BAKE_HOST_CPP)
FOLDCACHE_CPP    += $(ANIMATION_BAKE_HOST_CPP)
SHLIB_CPP        += $(ANIMATION_BAKE_HOST_CPP)
RETOPO_INT_CPP   += $(ANIMATION_BAKE_HOST_CPP)
GPU_PIPELINE_CPP += $(ANIMATION_BAKE_HOST_CPP)

# ---------------------------------------------------------------------------
# Enumerate the source-per-flavor sets and instantiate object rules.
# ---------------------------------------------------------------------------

# Collect the sources compiled under each flavor.
def_CPP_SRCS := $(sort \
    $(ECS_CPP) \
    $(ANIMATION_STORE_CPP) \
    $(ANIMATION_BUDGET_CPP) \
    $(PHYSICS_CPP) \
    $(ANIMATION_SYSTEMS_CPP) \
    $(ANIMATION_SIMULATION_CPP) \
    $(SCENE_REGISTRY_CPP) \
    $(ENTITY_RECIPE_CPP) \
    $(DYNAMIC_SLOTS_CPP) \
    $(DYNAMIC_BRIDGE_CPP) \
    $(ANIMATION_RIGID_CPP) \
    $(ANIMATION_SKIN_CPP) \
    $(EDITOR_MODEL_CPP) \
    $(PROPERTIES_REGISTRY_CPP) \
    $(SIMULATION_CONTROL_CPP) \
    $(SPECIALIZED_EDITORS_CPP) \
    $(ECS_ENTITY_BRIDGE_CPP) \
    $(PARTV2_CPP) \
    $(GRAPH_SOURCES) \
    $(TRIVAR_CPP) \
    $(COMP_CPP) \
    $(FLATTEN_CPP) \
    $(TILESETTORUSBVH_CPP) \
    $(ISO_CPP) \
    $(DSLDET_CPP) \
    $(DEV_SOURCES) \
    $(POLYTRI_CPP) \
    $(SHADER_SOURCE_CPP) \
    $(ASYNCQ_CPP) \
    $(BAKETRACE_CPP) \
    $(TILESETGTEX_CPP) \
    $(TILESETVKREPACK_CPP) \
    $(TILESETVKAO_CPP) \
    $(TILESETVKHORIZON_CPP) \
    $(EMITTERGATHER_CPP) \
    $(REFINECTL_CPP) \
    $(SECTORSTREAM_CPP) \
    $(SECTORCOORD_CPP) \
    $(RESOLVECACHE_CPP) \
    $(MODAPPLY_CPP) \
    $(TERRAINFIELD_CPP) \
    $(TERRAINMESH_CPP) \
    $(PARTSTORE_CPP))

qjs_CPP_SRCS := $(sort \
    $(SCRIPT_CPP) \
    $(ANIMATION_DSL_RIG_CPP) \
    $(ANIMATION_DSL_MOTION_CPP) \
    $(ANIMATION_DIAGNOSTICS_CPP) \
    $(MODBAKE_CPP) \
    $(TILESETDSL_CPP) \
    $(TERRAINNOISE_CPP) \
    $(WORLD_DEFINITION_CPP) \
    $(PHYSICS_PLAYGROUND_CPP) \
    $(TILESETMEADOWMANIFEST_CPP))

sh_CPP_SRCS := $(sort \
    $(EVALWORLD_CPP) \
    $(TERRAINVERB_CPP) \
    $(SECTORBAKE_CPP) \
    $(FOLDCACHE_CPP) \
    $(GRAPH_INT_CPP) \
    $(ANIMATION_B_ACCEPTANCE_CPP) \
    $(ANIMATION_C4_ACCEPTANCE_CPP) \
    $(EXAMPLE_CPP) \
    $(GALLERY_CPP) \
    $(TREEBAKE_CPP) \
    $(ROCKPROF_CPP) \
    $(GRASSLOD_CPP) \
    $(STRESSFOREST_CPP) \
    $(LIGHTING_GARDEN_CPP) \
    $(TILESETBAKE_CPP) \
    $(TILESETPARAMS_CPP) \
    $(SETTLEBENCH_CPP) \
    $(LIVEPROD_CPP))
sh_test_CPP_SRCS := $(sort $(GRAPH_INT_CPP))
partstore_test_CPP_SRCS := $(sort $(PARTSTORE_CPP))

sp2_CPP_SRCS := $(sort $(SHLIB_CPP))
vl_CPP_SRCS  := $(sort $(VIEWER_LOGIC_CPP))
retopo_CPP_SRCS := $(sort $(RETOPO_INT_CPP))
gpu_CPP_SRCS := $(sort \
    tileset_provider_tests.cpp \
    api_tests.cpp \
    async_bake_tests.cpp \
    demand_bake_tests.cpp \
    transient_tests.cpp \
    vk_scene_renderer_tests.cpp \
    vk_animation_skinning_tests.cpp \
    vk_animation_skin_compute_tests.cpp \
    vk_animation_bounds_tests.cpp \
    refine_loop_tests.cpp \
    world_stream_tests.cpp \
    $(GPU_ALL_CPP))

# C-source flavors. Union across binaries that use them.
mslc_C_SRCS := $(sort $(COMMON_MSL_C))
qjsc_C_SRCS := $(sort $(QJS_C))
flecsc_C_SRCS := $(sort $(ECS_C) $(ANIMATION_RIGID_C))
physicsc_C_SRCS := $(sort $(PHYSICS_C))

# Object lists per flavor.
def_CPP_OBJS    := $(call obj_list,def,$(def_CPP_SRCS))
qjs_CPP_OBJS    := $(call obj_list,qjs,$(qjs_CPP_SRCS))
sh_CPP_OBJS     := $(call obj_list,sh,$(sh_CPP_SRCS))
sh_test_CPP_OBJS := $(call obj_list,sh_test,$(sh_test_CPP_SRCS))
partstore_test_CPP_OBJS := $(call obj_list,partstore_test,$(partstore_test_CPP_SRCS))
sp2_CPP_OBJS    := $(call obj_list,sp2,$(sp2_CPP_SRCS))
vl_CPP_OBJS     := $(call obj_list,vl,$(vl_CPP_SRCS))
retopo_CPP_OBJS := $(call obj_list,retopo,$(retopo_CPP_SRCS))
gpu_CPP_OBJS    := $(call obj_list,gpu,$(gpu_CPP_SRCS))
mslc_C_OBJS     := $(call obj_list,mslc,$(mslc_C_SRCS))
qjsc_C_OBJS     := $(call obj_list,qjsc,$(qjsc_C_SRCS))
flecsc_C_OBJS   := $(call obj_list,flecsc,$(flecsc_C_SRCS))
physicsc_C_OBJS := $(call obj_list,physicsc,$(physicsc_C_SRCS))

ALL_OBJS := $(def_CPP_OBJS) $(qjs_CPP_OBJS) $(sh_CPP_OBJS) $(sh_test_CPP_OBJS) $(partstore_test_CPP_OBJS) $(sp2_CPP_OBJS) \
            $(vl_CPP_OBJS) $(retopo_CPP_OBJS) $(gpu_CPP_OBJS) \
            $(mslc_C_OBJS) $(qjsc_C_OBJS) $(flecsc_C_OBJS) $(physicsc_C_OBJS)

# Instantiate per-source compile rules.
$(foreach s,$(def_CPP_SRCS),$(eval $(call objrule_cpp,def,$(s))))
$(foreach s,$(qjs_CPP_SRCS),$(eval $(call objrule_cpp,qjs,$(s))))
$(foreach s,$(sh_CPP_SRCS),$(eval $(call objrule_cpp,sh,$(s))))
$(foreach s,$(sh_test_CPP_SRCS),$(eval $(call objrule_cpp,sh_test,$(s))))
$(foreach s,$(partstore_test_CPP_SRCS),$(eval $(call objrule_cpp,partstore_test,$(s))))
$(foreach s,$(sp2_CPP_SRCS),$(eval $(call objrule_cpp,sp2,$(s))))
$(foreach s,$(vl_CPP_SRCS),$(eval $(call objrule_cpp,vl,$(s))))
$(foreach s,$(retopo_CPP_SRCS),$(eval $(call objrule_cpp,retopo,$(s))))
$(foreach s,$(gpu_CPP_SRCS),$(eval $(call objrule_cpp,gpu,$(s))))
$(foreach s,$(mslc_C_SRCS),$(eval $(call objrule_c,mslc,$(s))))
$(foreach s,$(qjsc_C_SRCS),$(eval $(call objrule_c,qjsc,$(s))))
$(foreach s,$(flecsc_C_SRCS),$(eval $(call objrule_c,flecsc,$(s))))
$(foreach s,$(physicsc_C_SRCS),$(eval $(call objrule_c,physicsc,$(s))))

# Load dep files.
-include $(wildcard $(OBJDIR)/*/*.d)

# ---------------------------------------------------------------------------
# Convenience: derived object-list helpers per binary (link-time inputs).
# ---------------------------------------------------------------------------
# E6 fallout (docs/event-system.md S I.11): physics_context.o now references
# matter::evt::* (event_hub_->emit(events::PhysStep) in PhysicsContext::pull),
# pulling in undefined dispatch_stack()/emit_depth() (dispatch_context.cpp),
# fail_fast() (subscription.cpp), and Hub::get_or_create_lane_channel /
# push_trace_record_locked (event_hub.cpp). Every def-flavor target that links
# physics_context.cpp must therefore also link the event impl trio. Append as a
# link-only object list (NOT via the *_CPP source vars) so these sources do not
# enter the def_CPP_SRCS union — their def obj rules already exist unconditionally
# via the scene_tracker block below, so a second rule-gen (and its "overriding
# recipe" warning) is avoided. Mirrors run-physicsevents / run-scenetracker /
# the gpu targets, which link the same trio.
EVENT_IMPL_DEF_OBJS = $(call obj_list,def,\
    ../src/event/event_hub.cpp \
    ../src/event/subscription.cpp \
    ../src/event/dispatch_context.cpp)

ECS_OBJS = $(call obj_list,def,$(ECS_CPP)) $(call obj_list,flecsc,$(ECS_C)) $(EVENT_IMPL_DEF_OBJS)
PHYSICS_OBJS = $(call obj_list,def,$(PHYSICS_CPP)) $(call obj_list,physicsc,$(PHYSICS_C)) $(EVENT_IMPL_DEF_OBJS)
# The animation suites below reach physics_context.cpp through the same ECS
# runtime closure (ANIMATION_ECS_RUNTIME_REQUIRED / ANIMATION_STORE_CPP), so
# they need the E6 event trio for exactly the reason described above.
ANIMATION_SYSTEMS_OBJS = $(call obj_list,def,$(ANIMATION_SYSTEMS_CPP)) $(call obj_list,flecsc,$(FLECS_C)) $(EVENT_IMPL_DEF_OBJS)
ANIMATION_SIMULATION_OBJS = $(call obj_list,def,$(ANIMATION_SIMULATION_CPP)) $(call obj_list,flecsc,$(FLECS_C)) $(EVENT_IMPL_DEF_OBJS)
ANIMATION_C4_ACCEPTANCE_OBJS = $(call obj_list,sh,$(ANIMATION_C4_ACCEPTANCE_CPP)) $(call obj_list,mslc,$(ANIMATION_C4_ACCEPTANCE_C)) $(call obj_list,flecsc,$(ANIMATION_C4_ACCEPTANCE_FLECS_C)) $(qjsc_C_OBJS)
ANIMATION_B_ACCEPTANCE_OBJS = $(call obj_list,sh,$(ANIMATION_B_ACCEPTANCE_CPP)) $(call obj_list,mslc,$(ANIMATION_B_ACCEPTANCE_C)) $(call obj_list,flecsc,$(ANIMATION_B_ACCEPTANCE_FLECS_C)) $(qjsc_C_OBJS)
ANIMATION_STORE_OBJS = $(call obj_list,def,$(ANIMATION_STORE_CPP)) $(call obj_list,flecsc,$(FLECS_C)) $(EVENT_IMPL_DEF_OBJS)
ANIMATION_BUDGET_OBJS = $(call obj_list,def,$(ANIMATION_BUDGET_CPP))
SCENE_REGISTRY_OBJS = $(call obj_list,def,$(SCENE_REGISTRY_CPP)) $(call obj_list,flecsc,$(SCENE_REGISTRY_C)) $(EVENT_IMPL_DEF_OBJS)
ENTITY_RECIPE_OBJS = $(call obj_list,def,$(ENTITY_RECIPE_CPP)) $(call obj_list,flecsc,$(ENTITY_RECIPE_C)) $(EVENT_IMPL_DEF_OBJS)
DYNAMIC_SLOTS_OBJS = $(call obj_list,def,$(DYNAMIC_SLOTS_CPP))
DYNAMIC_BRIDGE_OBJS = $(call obj_list,def,$(DYNAMIC_BRIDGE_CPP)) $(call obj_list,flecsc,$(DYNAMIC_BRIDGE_C)) $(EVENT_IMPL_DEF_OBJS)
ANIMATION_RIGID_OBJS = $(call obj_list,def,$(ANIMATION_RIGID_CPP)) $(call obj_list,flecsc,$(ANIMATION_RIGID_C))
ANIMATION_SKIN_OBJS = $(call obj_list,def,$(ANIMATION_SKIN_CPP)) $(call obj_list,flecsc,$(ANIMATION_SKIN_C))
EDITOR_MODEL_OBJS = $(call obj_list,def,$(EDITOR_MODEL_CPP))
PROPERTIES_REGISTRY_OBJS = $(call obj_list,def,$(PROPERTIES_REGISTRY_CPP)) $(call obj_list,flecsc,$(PROPERTIES_REGISTRY_C)) $(EVENT_IMPL_DEF_OBJS)
SIMULATION_CONTROL_OBJS = $(call obj_list,def,$(SIMULATION_CONTROL_CPP)) $(call obj_list,flecsc,$(SIMULATION_CONTROL_C)) $(EVENT_IMPL_DEF_OBJS)
SPECIALIZED_EDITORS_OBJS = $(call obj_list,def,$(SPECIALIZED_EDITORS_CPP)) $(call obj_list,flecsc,$(SPECIALIZED_EDITORS_C))
ECS_ENTITY_BRIDGE_OBJS = $(call obj_list,def,$(ECS_ENTITY_BRIDGE_CPP)) $(call obj_list,flecsc,$(ECS_ENTITY_BRIDGE_C)) $(EVENT_IMPL_DEF_OBJS)
PARTV2_OBJS = $(call obj_list,def,$(PARTV2_CPP)) $(call obj_list,mslc,$(PARTV2_C))
GRAPH_OBJS  = $(call obj_list,def,$(GRAPH_SOURCES)) $(call obj_list,mslc,$(GRAPH_C))
TRIVAR_OBJS = $(call obj_list,def,$(TRIVAR_CPP)) $(call obj_list,mslc,$(TRIVAR_C))
COMP_OBJS   = $(call obj_list,def,$(COMP_CPP)) $(call obj_list,mslc,$(COMP_C))
FLATTEN_OBJS = $(call obj_list,def,$(FLATTEN_CPP)) $(call obj_list,mslc,$(FLATTEN_C))
TILESETTORUSBVH_OBJS = $(call obj_list,def,$(TILESETTORUSBVH_CPP)) $(call obj_list,mslc,$(TILESETTORUSBVH_C))
TILESETMEADOWMANIFEST_OBJS = $(call obj_list,qjs,$(TILESETMEADOWMANIFEST_CPP)) $(call obj_list,mslc,$(TILESETMEADOWMANIFEST_C)) $(qjsc_C_OBJS)
ISO_OBJS = $(call obj_list,def,$(ISO_CPP)) $(call obj_list,mslc,$(ISO_C))
DSLDET_OBJS = $(call obj_list,def,$(DSLDET_CPP)) $(call obj_list,mslc,$(DSLDET_C))
DEV_OBJS = $(call obj_list,def,$(DEV_SOURCES))
POLYTRI_OBJS = $(call obj_list,def,$(POLYTRI_CPP))
SHADER_SOURCE_OBJS = $(call obj_list,def,$(SHADER_SOURCE_CPP))
ASYNCQ_OBJS = $(call obj_list,def,$(ASYNCQ_CPP))
BAKETRACE_OBJS = $(call obj_list,def,$(BAKETRACE_CPP))
TILESETGTEX_OBJS = $(call obj_list,def,$(TILESETGTEX_CPP))
TILESETVKREPACK_OBJS = $(call obj_list,def,$(TILESETVKREPACK_CPP))
TILESETVKAO_OBJS = $(call obj_list,def,$(TILESETVKAO_CPP))
TILESETVKHORIZON_OBJS = $(call obj_list,def,$(TILESETVKHORIZON_CPP))
EMITTERGATHER_OBJS = $(call obj_list,def,$(EMITTERGATHER_CPP))
REFINECTL_OBJS = $(call obj_list,def,$(REFINECTL_CPP))
SECTORSTREAM_OBJS = $(call obj_list,def,$(SECTORSTREAM_CPP))
SECTORCOORD_OBJS = $(call obj_list,def,$(SECTORCOORD_CPP)) $(call obj_list,flecsc,$(SECTORCOORD_C))
RESOLVECACHE_OBJS = $(call obj_list,def,$(RESOLVECACHE_CPP)) $(call obj_list,mslc,$(RESOLVECACHE_C))
MODAPPLY_OBJS = $(call obj_list,def,$(MODAPPLY_CPP))
TERRAINFIELD_OBJS = $(call obj_list,def,$(TERRAINFIELD_CPP))
TERRAINMESH_OBJS  = $(call obj_list,def,$(TERRAINMESH_CPP))
TERRAINVERB_OBJS  = $(call obj_list,sh,$(TERRAINVERB_CPP)) $(call obj_list,mslc,$(TERRAINVERB_C)) $(qjsc_C_OBJS)
SECTORBAKE_OBJS   = $(call obj_list,sh,$(SECTORBAKE_CPP)) $(call obj_list,mslc,$(SECTORBAKE_C)) $(qjsc_C_OBJS)
PARTSTORE_OBJS = $(call obj_list,partstore_test,$(PARTSTORE_CPP)) $(call obj_list,mslc,$(PARTSTORE_C))

SCRIPT_OBJS = $(call obj_list,qjs,$(SCRIPT_CPP)) $(call obj_list,mslc,$(SCRIPT_C)) $(qjsc_C_OBJS)
MODBAKE_OBJS = $(call obj_list,qjs,$(MODBAKE_CPP)) $(call obj_list,mslc,$(MODBAKE_C)) $(qjsc_C_OBJS)
ANIMATION_DIAGNOSTICS_OBJS = $(call obj_list,qjs,$(ANIMATION_DIAGNOSTICS_CPP)) $(call obj_list,mslc,$(ANIMATION_DIAGNOSTICS_C)) $(qjsc_C_OBJS)
TILESETDSL_OBJS = $(call obj_list,qjs,$(TILESETDSL_CPP)) $(call obj_list,mslc,$(SCRIPT_C)) $(qjsc_C_OBJS)
TERRAINNOISE_OBJS = $(call obj_list,qjs,$(TERRAINNOISE_CPP)) $(qjsc_C_OBJS)
WORLD_DEFINITION_OBJS = $(call obj_list,qjs,$(WORLD_DEFINITION_CPP)) $(qjsc_C_OBJS)
PHYSICS_PLAYGROUND_OBJS = $(call obj_list,qjs,$(PHYSICS_PLAYGROUND_CPP)) $(qjsc_C_OBJS)
EVALWORLD_OBJS = $(call obj_list,sh,$(EVALWORLD_CPP)) $(call obj_list,mslc,$(EVALWORLD_C)) $(qjsc_C_OBJS)
FOLDCACHE_OBJS = $(call obj_list,sh,$(FOLDCACHE_CPP)) $(call obj_list,mslc,$(FOLDCACHE_C)) $(qjsc_C_OBJS)

GRAPH_INT_OBJS = $(call obj_list,sh_test,$(GRAPH_INT_CPP)) $(call obj_list,mslc,$(GRAPH_INT_C)) $(qjsc_C_OBJS)
EXAMPLE_OBJS = $(call obj_list,sh,$(EXAMPLE_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
GALLERY_OBJS = $(call obj_list,sh,$(GALLERY_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
TREEBAKE_OBJS = $(call obj_list,sh,$(TREEBAKE_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
ROCKPROF_OBJS = $(call obj_list,sh,$(ROCKPROF_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
GRASSLOD_OBJS = $(call obj_list,sh,$(GRASSLOD_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
STRESSFOREST_OBJS = $(call obj_list,sh,$(STRESSFOREST_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS)
LIGHTING_GARDEN_OBJS = $(call obj_list,sh,$(LIGHTING_GARDEN_CPP)) \
                       $(call obj_list,mslc,$(LIGHTING_GARDEN_C)) $(qjsc_C_OBJS)
TILESETBAKE_OBJS = $(call obj_list,sh,$(TILESETBAKE_CPP)) $(call obj_list,mslc,$(TILESETBAKE_C)) $(qjsc_C_OBJS)
TILESETPARAMS_OBJS = $(call obj_list,sh,$(TILESETPARAMS_CPP)) $(call obj_list,mslc,$(TILESETPARAMS_C)) $(qjsc_C_OBJS)
SETTLEBENCH_OBJS = $(call obj_list,sh,$(SETTLEBENCH_CPP)) $(call obj_list,mslc,$(SETTLEBENCH_C)) $(qjsc_C_OBJS)
LIVEPROD_OBJS    = $(call obj_list,sh,$(LIVEPROD_CPP)) $(call obj_list,mslc,$(LIVEPROD_C)) $(qjsc_C_OBJS)

SHLIB_OBJS = $(call obj_list,sp2,$(SHLIB_CPP)) $(call obj_list,mslc,$(SHLIB_C)) $(qjsc_C_OBJS)
VIEWER_LOGIC_OBJS = $(call obj_list,vl,$(VIEWER_LOGIC_CPP)) $(call obj_list,mslc,$(EXAMPLE_C)) $(qjsc_C_OBJS) $(flecsc_C_OBJS)
RETOPO_INT_OBJS = $(call obj_list,retopo,$(RETOPO_INT_CPP)) $(call obj_list,mslc,$(RETOPO_INT_C)) $(qjsc_C_OBJS)

# GPU binaries reuse the same gpu-flavor object set. Each binary is:
# <driver>.o + $(GPU_ALL_CPP) objects + mslc + qjsc.
GPU_SHARED_OBJS := $(call obj_list,gpu,$(sort $(GPU_ALL_CPP))) $(call obj_list,mslc,$(GPU_MSL_C)) $(qjsc_C_OBJS) $(flecsc_C_OBJS)

# ---------------------------------------------------------------------------
# .PHONY targets: preserve every name build-all.sh and other scripts rely on.
# ---------------------------------------------------------------------------
.PHONY: clean run-animation-diagnostics run-matrix run-animation-ozz run-animation-ozz-adapter run-animation-asset run-animation-dsl-rig run-animation-dsl-motion run-animation-binding run-animation-bake run-animation-evaluator run-animation-budget run-animation-systems run-animation-simulation run-animation-phase-b-acceptance run-animation-phase-c-acceptance run-animation-controller run-animation-ik run-animation-rigid run-tilesetslicer run-ecs run-physics run-scene-registry run-entity-recipes run-dynamic-slots run-partv2 run-partstore run-script run-foldcache run-evalworld run-world-definition run-physics-playground run-terrainmesh run-terrainverb run-sectorbake run-sectorstream run-sectorcoord run-graph run-graph-integration run-trivar run-shlib run-comp run-dev run-live run-example run-gallery run-viewer-logic check-viewer-logic-animation-sources run-iso run-polytri run-treebake run-meadow run-meadow-check run-lighting-garden run-flatten run-retopo-integration run-grasslod run-stressforest run-tilesetphysics run-tilesetcore run-tilesetplacement run-tilesetdsl run-tilesetbake run-tilesetparams run-tilesetgtex run-shader-source run-asyncq gpu-tests tileset-gpu-tests tileset-provider-tests tileset-load-tests api-tests run-tilesetgpu run-tilesetseam run-tilesetprovider run-tilesetload run-asyncbake async-bake-tests run-liveprod run-terrainnoise release-part-tests run-releasepart run-release-part vk-scene-renderer-tests run-vk-scene-renderer vk-animation-skinning-tests run-vk-animation-skinning vk-animation-bounds-tests run-vk-animation-bounds run-gpucull demand-bake-tests run-demandbake run-refinectl refine-loop-tests run-resolvecache run-modapply run-modbake run-terrainfield world-stream-tests run-worldstream run-dynamic-bridge run-editor-model run-properties-registry run-simulation-control run-specialized-editors run-ecs-entity-bridge run-emittergather run-baketrace run-dsldet run-tilesetvkrepack run-tilesetvkao run-tilesettorusbvh run-tilesetmeadowmanifest run-api-tests run-refineloop resolve-cache-tests run-settlebench

# ---------------------------------------------------------------------------
# Binary link rules (link-only; every .o was built above).
# ---------------------------------------------------------------------------

$(MATRIX_TARGET): $(MATRIX_CPP) $(MATRIX_HEADERS) | build
	$(CC) $(MATRIX_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS)

run-matrix: $(MATRIX_TARGET)
	./$(MATRIX_TARGET)

$(ANIMATION_OZZ_TARGET): $(ANIMATION_OZZ_CPP) $(OZZ_OFFLINE_LIBS) | build
	$(CC) $(ANIMATION_OZZ_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(OZZ_OFFLINE_LIBS)

run-animation-ozz: $(ANIMATION_OZZ_TARGET)
	./$(ANIMATION_OZZ_TARGET)

$(ANIMATION_OZZ_ADAPTER_TARGET): $(ANIMATION_OZZ_ADAPTER_CPP) $(OZZ_OFFLINE_LIBS) | build
	$(CC) $(ANIMATION_OZZ_ADAPTER_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(OZZ_OFFLINE_LIBS)

run-animation-ozz-adapter: $(ANIMATION_OZZ_ADAPTER_TARGET)
	./$(ANIMATION_OZZ_ADAPTER_TARGET)

$(ANIMATION_OZZ_RUNTIME_LINK_TARGET): $(ANIMATION_OZZ_RUNTIME_LINK_CPP) $(OZZ_RUNTIME_LIBS) | build
	$(CC) $(ANIMATION_OZZ_RUNTIME_LINK_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(OZZ_RUNTIME_LIBS)

run-animation-ozz-runtime-link: $(ANIMATION_OZZ_RUNTIME_LINK_TARGET)
	./$(ANIMATION_OZZ_RUNTIME_LINK_TARGET)

$(ANIMATION_IR_TARGET): $(ANIMATION_IR_CPP) | build
	$(CC) $(ANIMATION_IR_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS)

run-animation-ir: $(ANIMATION_IR_TARGET)
	./$(ANIMATION_IR_TARGET)

$(ANIMATION_ASSET_TARGET): $(ANIMATION_ASSET_CPP) | build
	$(CC) $(ANIMATION_ASSET_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-animation-asset: $(ANIMATION_ASSET_TARGET)
	./$(ANIMATION_ASSET_TARGET)

.PHONY: run-animation-runtime-asset
$(ANIMATION_RUNTIME_ASSET_TARGET): $(ANIMATION_RUNTIME_ASSET_CPP) $(OZZ_OFFLINE_LIBS) | build
	$(CC) $(ANIMATION_RUNTIME_ASSET_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(OZZ_OFFLINE_LIBS) $(LDFLAGS) $(LDLIBS)
run-animation-runtime-asset: $(ANIMATION_RUNTIME_ASSET_TARGET)
	./$(ANIMATION_RUNTIME_ASSET_TARGET)

$(ANIMATION_DSL_RIG_TARGET): check-animation-dsl-rig-sources $(ANIMATION_DSL_RIG_OBJS) | build
	$(CC) $(ANIMATION_DSL_RIG_OBJS) -o $@ $(FLAVOR_qjs_FLAGS) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-animation-dsl-rig: $(ANIMATION_DSL_RIG_TARGET)
	./$(ANIMATION_DSL_RIG_TARGET)

$(ANIMATION_DSL_MOTION_TARGET): $(ANIMATION_DSL_MOTION_OBJS) | build
	$(CC) $(ANIMATION_DSL_MOTION_OBJS) -o $@ $(FLAVOR_qjs_FLAGS) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)
run-animation-dsl-motion: $(ANIMATION_DSL_MOTION_TARGET)
	./$(ANIMATION_DSL_MOTION_TARGET)

$(ANIMATION_BINDING_TARGET): $(ANIMATION_BINDING_CPP) | build
	$(CC) $(ANIMATION_BINDING_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-animation-binding: $(ANIMATION_BINDING_TARGET)
	./$(ANIMATION_BINDING_TARGET)

$(ANIMATION_STORE_TARGET): $(ANIMATION_STORE_OBJS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) | build
	$(CC) $(ANIMATION_STORE_OBJS) -o $@ $(FLAVOR_def_FLAGS) $(LDFLAGS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) $(FLECS_PLATFORM_LIBS)

run-animation-store: $(ANIMATION_STORE_TARGET)
	./$(ANIMATION_STORE_TARGET)

$(ANIMATION_EVALUATOR_TARGET): $(ANIMATION_EVALUATOR_CPP) $(OZZ_OFFLINE_LIBS) | build
	$(CC) $(ANIMATION_EVALUATOR_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(OZZ_OFFLINE_LIBS)

run-animation-evaluator: $(ANIMATION_EVALUATOR_TARGET)
	./$(ANIMATION_EVALUATOR_TARGET)

$(ANIMATION_BUDGET_TARGET): $(ANIMATION_BUDGET_OBJS) | build
	$(CC) $(ANIMATION_BUDGET_OBJS) -o $@ $(FLAVOR_def_FLAGS)

run-animation-budget: $(ANIMATION_BUDGET_TARGET)
	./$(ANIMATION_BUDGET_TARGET)

$(ANIMATION_SYSTEMS_TARGET): $(ANIMATION_SYSTEMS_OBJS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) | build
	$(CC) $(ANIMATION_SYSTEMS_OBJS) -o $@ $(FLAVOR_def_FLAGS) $(LDFLAGS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) $(FLECS_PLATFORM_LIBS)

run-animation-systems: $(ANIMATION_SYSTEMS_TARGET)
	./$(ANIMATION_SYSTEMS_TARGET)

$(ANIMATION_SIMULATION_TARGET): $(ANIMATION_SIMULATION_OBJS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) | build
	$(CC) $(ANIMATION_SIMULATION_OBJS) -o $@ $(FLAVOR_def_FLAGS) $(LDFLAGS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) $(FLECS_PLATFORM_LIBS)

run-animation-simulation: $(ANIMATION_SIMULATION_TARGET)
	./$(ANIMATION_SIMULATION_TARGET)

$(ANIMATION_C4_ACCEPTANCE_TARGET): $(ANIMATION_C4_ACCEPTANCE_OBJS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) | build
	$(CC) $(ANIMATION_C4_ACCEPTANCE_OBJS) -o $@ $(FLAVOR_sh_FLAGS) $(LDFLAGS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) $(LDLIBS) $(FLECS_PLATFORM_LIBS)

run-animation-phase-c-acceptance: $(ANIMATION_C4_ACCEPTANCE_TARGET)
	./$(ANIMATION_C4_ACCEPTANCE_TARGET)

$(ANIMATION_B_ACCEPTANCE_TARGET): $(ANIMATION_B_ACCEPTANCE_OBJS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) | build
	$(CC) $(ANIMATION_B_ACCEPTANCE_OBJS) -o $@ $(FLAVOR_sh_FLAGS) $(LDFLAGS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) $(LDLIBS) $(FLECS_PLATFORM_LIBS)

run-animation-phase-b-acceptance: $(ANIMATION_B_ACCEPTANCE_TARGET)
	./$(ANIMATION_B_ACCEPTANCE_TARGET)

$(ANIMATION_CONTROLLER_TARGET): $(ANIMATION_CONTROLLER_CPP) $(OZZ_OFFLINE_LIBS) | build
	$(CC) $(ANIMATION_CONTROLLER_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(OZZ_OFFLINE_LIBS)
run-animation-controller: $(ANIMATION_CONTROLLER_TARGET)
	./$(ANIMATION_CONTROLLER_TARGET)

$(ANIMATION_IK_TARGET): $(ANIMATION_IK_CPP) $(OZZ_OFFLINE_LIBS) | build
	$(CC) $(ANIMATION_IK_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(OZZ_OFFLINE_LIBS)
run-animation-ik: $(ANIMATION_IK_TARGET)
	./$(ANIMATION_IK_TARGET)

.PHONY: run-animation-ir

$(TILESETSLICER_TARGET): $(TILESETSLICER_CPP) $(TILESETSLICER_HEADERS) | build
	$(CC) $(TILESETSLICER_CPP) -o $@ $(CFLAGS) $(INCLUDE_PATHS)

run-tilesetslicer: $(TILESETSLICER_TARGET)
	./$(TILESETSLICER_TARGET)

.PHONY: check-ecs-animation-sources check-physics-animation-sources check-simulation-control-animation-sources
define check_animation_ecs_runtime_closure
@missing=""; \
for source in $(ANIMATION_ECS_RUNTIME_REQUIRED); do \
	case " $($(1)) " in *"$$source "*) ;; *) missing="$$missing $$source" ;; esac; \
done; \
if test -n "$$missing"; then \
	echo "ERROR: $(2) animation closure omits:$$missing"; exit 1; \
fi; \
duplicates="$$(printf '%s\\n' $($(1)) | sort | uniq -d)"; \
if test -n "$$duplicates"; then \
	echo "ERROR: $(2) animation closure has duplicate entries:"; \
	printf '%s\\n' "$$duplicates"; exit 1; \
fi
endef

check-ecs-animation-sources:
	$(call check_animation_ecs_runtime_closure,ECS_CPP,ecs)

check-physics-animation-sources:
	$(call check_animation_ecs_runtime_closure,PHYSICS_CPP,physics)

check-simulation-control-animation-sources:
	$(call check_animation_ecs_runtime_closure,SIMULATION_CONTROL_CPP,simulation control)

$(ECS_TARGET): check-ecs-animation-sources $(ECS_OBJS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS)
	$(CC) $(ECS_OBJS) -o $@ $(FLAVOR_def_FLAGS) $(LDFLAGS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) $(FLECS_PLATFORM_LIBS)

run-ecs: $(ECS_TARGET)
	./$(ECS_TARGET)

$(PHYSICS_TARGET): check-physics-animation-sources $(PHYSICS_OBJS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS)
	$(CC) $(PHYSICS_OBJS) -o $@ $(FLAVOR_def_FLAGS) $(LDFLAGS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) $(FLECS_PLATFORM_LIBS)

run-physics: $(PHYSICS_TARGET)
	./$(PHYSICS_TARGET)

# Phase 4 Task 4 — Scene registry (reflected components, validation, instantiation).
# Pure CPU test: no GL, no raylib — only box3d + flecs system deps.
$(SCENE_REGISTRY_TARGET): $(SCENE_REGISTRY_OBJS) $(BOX3D_LIB)
	$(CC) $(SCENE_REGISTRY_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) -lws2_32 -ldbghelp -lwinmm

run-scene-registry: $(SCENE_REGISTRY_TARGET)
	./$(SCENE_REGISTRY_TARGET)

# Phase 4 Task 5 — Recipe normalization + transactional bootstrap.
# Pure CPU test: no GL, no raylib — only box3d + flecs system deps.
$(ENTITY_RECIPE_TARGET): $(ENTITY_RECIPE_OBJS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS)
	$(CC) $(ENTITY_RECIPE_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) -lws2_32 -ldbghelp -lwinmm

run-entity-recipes: $(ENTITY_RECIPE_TARGET)
	./$(ENTITY_RECIPE_TARGET)

# Phase 4 Task 6 — CPU stable dynamic instance slots.
# Pure CPU test: no GL, no raylib, no Flecs/physics/ECS deps.
$(DYNAMIC_SLOTS_TARGET): $(DYNAMIC_SLOTS_OBJS)
	$(CC) $(DYNAMIC_SLOTS_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread

run-dynamic-slots: $(DYNAMIC_SLOTS_TARGET)
	./$(DYNAMIC_SLOTS_TARGET)

# Phase 4 Task 8 — ECS dynamic scene bridge (reconciles SceneEntityId +
# WorldTransform + PartInstance against DynamicInstanceSlots each frame).
# Pure CPU test: no GL, no raylib — only box3d + flecs system deps.
.PHONY: check-dynamic-bridge-animation-sources
check-dynamic-bridge-animation-sources:
	@for source in $(DYNAMIC_BRIDGE_RENDER_REQUIRED); do \
		case " $(DYNAMIC_BRIDGE_CPP) " in *"$$source "*) ;; *) \
			echo "ERROR: dynamic bridge animation closure omits $$source"; exit 1 ;; \
		esac; \
	done

$(DYNAMIC_BRIDGE_TARGET): check-dynamic-bridge-animation-sources $(DYNAMIC_BRIDGE_OBJS) $(BOX3D_LIB)
	$(CC) $(DYNAMIC_BRIDGE_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) -lws2_32 -ldbghelp -lwinmm

run-dynamic-bridge: $(DYNAMIC_BRIDGE_TARGET)
	./$(DYNAMIC_BRIDGE_TARGET)

$(ANIMATION_RIGID_TARGET): $(ANIMATION_RIGID_OBJS) | build
	$(CC) $(ANIMATION_RIGID_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread $(OZZ_OFFLINE_LIBS) $(FLECS_PLATFORM_LIBS)

run-animation-rigid: $(ANIMATION_RIGID_TARGET)
	./$(ANIMATION_RIGID_TARGET)

$(ANIMATION_SKIN_TARGET): $(ANIMATION_SKIN_OBJS) | build
	$(CC) $(ANIMATION_SKIN_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread $(OZZ_OFFLINE_LIBS) $(FLECS_PLATFORM_LIBS)

run-animation-skin: $(ANIMATION_SKIN_TARGET)
	./$(ANIMATION_SKIN_TARGET)

# Phase 4 Task 9 — Editor model (pure CPU, no Flecs/GL runtime).
$(EDITOR_MODEL_TARGET): $(EDITOR_MODEL_OBJS)
	$(CC) $(EDITOR_MODEL_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread

run-editor-model: $(EDITOR_MODEL_TARGET)
	./$(EDITOR_MODEL_TARGET)

# Phase 4 Task 10 — Properties registry (pure CPU, needs scene_registry).
$(PROPERTIES_REGISTRY_TARGET): $(PROPERTIES_REGISTRY_OBJS) $(BOX3D_LIB)
	$(CC) $(PROPERTIES_REGISTRY_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) -lws2_32 -ldbghelp -lwinmm

run-properties-registry: $(PROPERTIES_REGISTRY_TARGET)
	./$(PROPERTIES_REGISTRY_TARGET)

# Phase 4 Task 11 — Simulation control.
$(SIMULATION_CONTROL_TARGET): check-simulation-control-animation-sources $(SIMULATION_CONTROL_OBJS) $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS)
	$(CC) $(SIMULATION_CONTROL_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) -lws2_32 -ldbghelp -lwinmm

run-simulation-control: $(SIMULATION_CONTROL_TARGET)
	./$(SIMULATION_CONTROL_TARGET)

# Phase 4 Task 12 — Specialized editors (pure CPU, needs scene_registry).
$(SPECIALIZED_EDITORS_TARGET): $(SPECIALIZED_EDITORS_OBJS)
	$(CC) $(SPECIALIZED_EDITORS_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread -lws2_32 -ldbghelp -lwinmm

run-specialized-editors: $(SPECIALIZED_EDITORS_TARGET)
	./$(SPECIALIZED_EDITORS_TARGET)

# Phase 5 — entity -> ECS bridge (Ready world-state commands bootstrap
# Flecs entities via scene::bootstrap_transactional).
$(ECS_ENTITY_BRIDGE_TARGET): $(ECS_ENTITY_BRIDGE_OBJS) $(BOX3D_LIB)
	$(CC) $(ECS_ENTITY_BRIDGE_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS) -lws2_32 -ldbghelp -lwinmm

run-ecs-entity-bridge: $(ECS_ENTITY_BRIDGE_TARGET)
	./$(ECS_ENTITY_BRIDGE_TARGET)

# Part Artifact v2 round-trip + resolved-hash + format-guard tests.
$(PARTV2_TARGET): $(PARTV2_OBJS)
	$(CC) $(PARTV2_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-partv2: $(PARTV2_TARGET)
	./$(PARTV2_TARGET)

$(LIGHTING_GARDEN_TARGET): $(LIGHTING_GARDEN_OBJS)
	$(CC) $(LIGHTING_GARDEN_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDLIBS) $(LDFLAGS) $(OZZ_OFFLINE_LIBS)

run-lighting-garden: $(LIGHTING_GARDEN_TARGET)
	./$(LIGHTING_GARDEN_TARGET)

# Script host (QuickJS-ng) + DSL/CSG bake tests.
$(SCRIPT_TARGET): $(SCRIPT_OBJS)
	$(CC) $(SCRIPT_OBJS) -o $@ \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-script: $(SCRIPT_TARGET)
	./$(SCRIPT_TARGET)

# Phase C Task 1: fold cache in ScriptHost + resolve_hash hit/miss counting.
$(FOLDCACHE_TARGET): $(FOLDCACHE_OBJS)
	$(CC) $(FOLDCACHE_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-foldcache: $(FOLDCACHE_TARGET)
	./$(FOLDCACHE_TARGET)

# Phase C Task 4 — eval_world: World builder DSL + ScriptHost::eval_world.
$(EVALWORLD_TARGET): $(EVALWORLD_OBJS)
	$(CC) $(EVALWORLD_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-evalworld: $(EVALWORLD_TARGET)
	./$(EVALWORLD_TARGET)

$(WORLD_DEFINITION_TARGET): $(WORLD_DEFINITION_OBJS)
	$(CC) $(WORLD_DEFINITION_OBJS) -o $@ $(FLAVOR_qjs_FLAGS) -lm

run-world-definition: $(WORLD_DEFINITION_TARGET)
	./$(WORLD_DEFINITION_TARGET)

$(PHYSICS_PLAYGROUND_TARGET): $(PHYSICS_PLAYGROUND_OBJS)
	$(CC) $(PHYSICS_PLAYGROUND_OBJS) -o $@ $(FLAVOR_qjs_FLAGS) -lm

run-physics-playground: $(PHYSICS_PLAYGROUND_TARGET)
	./$(PHYSICS_PLAYGROUND_TARGET)

# Phase C Task 5 — terrain mesher (pure CPU, no JS, no GL).
$(TERRAINMESH_TARGET): $(TERRAINMESH_OBJS)
	$(CC) $(TERRAINMESH_OBJS) -o $@ $(CFLAGS) -I../src -lm

run-terrainmesh: $(TERRAINMESH_TARGET)
	./$(TERRAINMESH_TARGET)

# Phase C Task 5 — terrainVolume verb through ScriptHost::bake_source.
$(TERRAINVERB_TARGET): $(TERRAINVERB_OBJS)
	$(CC) $(TERRAINVERB_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-terrainverb: $(TERRAINVERB_TARGET)
	./$(TERRAINVERB_TARGET)

# Phase C Task 7 — WorldSector.js + world query verbs.
$(SECTORBAKE_TARGET): $(SECTORBAKE_OBJS)
	$(CC) $(SECTORBAKE_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-sectorbake: $(SECTORBAKE_TARGET)
	./$(SECTORBAKE_TARGET)

# Phase 1 typed iso-primitives + ordered CSG.
$(ISO_TARGET): $(ISO_OBJS)
	$(CC) $(ISO_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-iso: $(ISO_TARGET)
	./$(ISO_TARGET)

# Phase 3 (mathlib-and-raylib-removal) determinism harness.
$(DSLDET_TARGET): $(DSLDET_OBJS)
	$(CC) $(DSLDET_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-dsldet: $(DSLDET_TARGET)
	./$(DSLDET_TARGET)

# SP-3 end-to-end install through the REAL FileModuleResolver/HostBaker adapters.
$(GRAPH_INT_TARGET): $(GRAPH_INT_OBJS)
	$(CC) $(GRAPH_INT_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-graph-integration: $(GRAPH_INT_TARGET)
	./$(GRAPH_INT_TARGET)

.PHONY: run-animation-provider
run-animation-provider: $(GRAPH_INT_TARGET)
	./$(GRAPH_INT_TARGET) --animation-provider-only

# A8's end-to-end bake/cache contract: static lane remains unchanged while an
# ANLK Part is accepted only with a coherent PART/MANM/MACM sibling generation.
.PHONY: run-animation-bake
run-animation-bake: $(GRAPH_INT_TARGET)
	./$(GRAPH_INT_TARGET) --animation-provider-only

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
	      $(CFLAGS) -DSP2_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

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
	      $(CFLAGS) -DMATTER_HAVE_AUTOREMESHER $(INCLUDE_PATHS) -I$(AR_CORE_PATH)/include $(RETOPO_INT_LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

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
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-example: $(EXAMPLE_TARGET)
	./$(EXAMPLE_TARGET)

# Headless bake smoke test for the primitive test world.
$(GALLERY_TARGET): $(GALLERY_OBJS)
	$(CC) $(GALLERY_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-gallery: $(GALLERY_TARGET)
	./$(GALLERY_TARGET)

# Headless bake check for the world_demo Tree.
$(TREEBAKE_TARGET): $(TREEBAKE_OBJS)
	$(CC) $(TREEBAKE_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-treebake: $(TREEBAKE_TARGET)
	./$(TREEBAKE_TARGET)

# Rock bake profiling harness (cold bake per variant, wall time).
$(ROCKPROF_TARGET): $(ROCKPROF_OBJS)
	$(CC) $(ROCKPROF_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-rock-profile: $(ROCKPROF_TARGET)
	./$(ROCKPROF_TARGET)

# --- Viewer GL-free logic suite.
../shaders_gen/embedded_shaders.h:
	$(MAKE) -C .. shaders_gen/embedded_shaders.h

check-viewer-logic-animation-sources:
	@for source in $(VIEWER_LOGIC_ANIMATION_REQUIRED); do \
		case " $(VIEWER_LOGIC_CPP) " in *"/$$source "*) ;; *) \
			echo "ERROR: viewer logic bake closure omits $$source"; exit 1 ;; esac; \
	done

$(VIEWER_LOGIC_TARGET): check-viewer-logic-animation-sources $(VIEWER_LOGIC_OBJS) $(OZZ_OFFLINE_LIBS) | ../shaders_gen/embedded_shaders.h
	$(CC) $(VIEWER_LOGIC_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB -pthread $(INCLUDE_PATHS) \
	      -I$(BOX3D_DIR)/include $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(BOX3D_DIR)/libbox3d.a $(OZZ_OFFLINE_LIBS)

run-viewer-logic: $(VIEWER_LOGIC_TARGET)
	./$(VIEWER_LOGIC_TARGET)

# Grass.js prefix-subset triangle-budget LOD contract (Stage 3).
$(GRASSLOD_TARGET): $(GRASSLOD_OBJS)
	$(CC) $(GRASSLOD_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-grasslod: $(GRASSLOD_TARGET)
	./$(GRASSLOD_TARGET)

# Stage-4 stress fixture scatter determinism.
$(STRESSFOREST_TARGET): $(STRESSFOREST_OBJS)
	$(CC) $(STRESSFOREST_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-stressforest: $(STRESSFOREST_TARGET)
	./$(STRESSFOREST_TARGET)

# Task 9: live-edit production seam tests (ProdGraphResolver/Baker/Flattener).
$(LIVEPROD_TARGET): $(LIVEPROD_OBJS)
	$(CC) $(LIVEPROD_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-liveprod: $(LIVEPROD_TARGET)
	./$(LIVEPROD_TARGET)

# Alias: run-live = dev_live_edit regression (SP-5 seam interfaces, fake graph).
run-live: $(DEV_TARGET)
	./$(DEV_TARGET)

# Box3d physics smoke test.
build/tileset_physics_tests: tileset_physics_tests.cpp ../src/tileset_settle.cpp ../src/tileset_collider.cpp $(BOX3D_DIR)/libbox3d.a | build
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include -I../src -I$(BOX3D_DIR)/include \
	    -o $@ tileset_physics_tests.cpp ../src/tileset_settle.cpp ../src/tileset_collider.cpp \
	    $(BOX3D_DIR)/libbox3d.a -lm

$(BOX3D_LIB):
	$(MAKE) -C $(BOX3D_DIR)

run-tilesetphysics: build/tileset_physics_tests
	./build/tileset_physics_tests

build/tileset_core_tests: tileset_core_tests.cpp ../src/tileset_layout.cpp ../src/tileset_collider.cpp | build
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include -I../src \
	    -o $@ tileset_core_tests.cpp ../src/tileset_layout.cpp ../src/tileset_collider.cpp

run-tilesetcore: build/tileset_core_tests
	./build/tileset_core_tests

build/tileset_placement_tests: tileset_placement_tests.cpp ../src/tileset_placement.cpp | build
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include -I../src \
	    -o $@ tileset_placement_tests.cpp ../src/tileset_placement.cpp

run-tilesetplacement: build/tileset_placement_tests
	./build/tileset_placement_tests

# Tileset DSL root.
$(TILESETDSL_TARGET): $(TILESETDSL_OBJS)
	$(CC) $(TILESETDSL_OBJS) -o $@ \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-tilesetdsl: $(TILESETDSL_TARGET)
	./$(TILESETDSL_TARGET)

# Phase C Task 1: terrain noise contract.
$(TERRAINNOISE_TARGET): $(TERRAINNOISE_OBJS)
	$(CC) $(TERRAINNOISE_OBJS) -o $@ \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)

run-terrainnoise: $(TERRAINNOISE_TARGET)
	./$(TERRAINNOISE_TARGET)

# Tileset bake bridge.
$(TILESETBAKE_TARGET): $(TILESETBAKE_OBJS) $(BOX3D_DIR)/libbox3d.a
	$(CC) $(TILESETBAKE_OBJS) \
	      -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST \
	      $(INCLUDE_PATHS) -I$(BOX3D_DIR)/include \
	      $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(BOX3D_DIR)/libbox3d.a -lm $(OZZ_OFFLINE_LIBS)

run-tilesetbake: $(TILESETBAKE_TARGET)
	./$(TILESETBAKE_TARGET)

$(TILESETPARAMS_TARGET): $(TILESETPARAMS_OBJS) $(BOX3D_DIR)/libbox3d.a
	$(CC) $(TILESETPARAMS_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a

run-tilesetparams: $(TILESETPARAMS_TARGET)
	./$(TILESETPARAMS_TARGET)

# Bake Lab task 5.4 — settle_bench headless benchmark CLI.
$(SETTLEBENCH_TARGET): $(SETTLEBENCH_OBJS) $(BOX3D_DIR)/libbox3d.a
	$(CC) $(SETTLEBENCH_OBJS) -o $@ \
	      $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a

run-settlebench: $(SETTLEBENCH_TARGET)
	./$(SETTLEBENCH_TARGET) --spec synthetic

# tileset .gtex format round-trip.
$(TILESETGTEX_TARGET): $(TILESETGTEX_OBJS)
	$(CC) $(TILESETGTEX_OBJS) -o $@ $(CFLAGS) -I../include -I../../third_party/raylib/src

run-tilesetgtex: $(TILESETGTEX_TARGET)
	./$(TILESETGTEX_TARGET)

# Vulkan .gtex bake (V1) repack — headless, no engine/Vulkan link.
$(TILESETVKREPACK_TARGET): $(TILESETVKREPACK_OBJS)
	$(CC) $(TILESETVKREPACK_OBJS) -o $@ $(CFLAGS)

run-tilesetvkrepack: $(TILESETVKREPACK_TARGET)
	./$(TILESETVKREPACK_TARGET)

# Vulkan .gtex bake (V2) AO seam-invariance — headless, no engine/Vulkan link.
$(TILESETVKAO_TARGET): $(TILESETVKAO_OBJS)
	$(CC) $(TILESETVKAO_OBJS) -o $@ $(CFLAGS)

run-tilesetvkao: $(TILESETVKAO_TARGET)
	./$(TILESETVKAO_TARGET)

# Vulkan .gtex bake (V3) horizon pass -- headless, no engine/Vulkan link.
$(TILESETVKHORIZON_TARGET): $(TILESETVKHORIZON_OBJS)
	$(CC) $(TILESETVKHORIZON_OBJS) -o $@ $(CFLAGS)

run-tilesetvkhorizon: $(TILESETVKHORIZON_TARGET)
	./$(TILESETVKHORIZON_TARGET)

# tileset_torus_bvh.
$(TILESETTORUSBVH_TARGET): $(TILESETTORUSBVH_OBJS)
	$(CC) $(TILESETTORUSBVH_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-tilesettorusbvh: $(TILESETTORUSBVH_TARGET)
	./$(TILESETTORUSBVH_TARGET)

# Meadow world-definition tileset-flag test.
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

$(BAKETRACE_TARGET): $(BAKETRACE_OBJS)
	$(CC) $(BAKETRACE_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) -lpthread

run-baketrace: $(BAKETRACE_TARGET)
	./$(BAKETRACE_TARGET)

# Phase C Task 4 — RefineController tile pairing + priority/eviction (pure CPU, def flavor).
$(REFINECTL_TARGET): $(REFINECTL_OBJS)
	$(CC) $(REFINECTL_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS)

run-refinectl: $(REFINECTL_TARGET)
	./$(REFINECTL_TARGET)

# Phase C Task 8 — SectorStreamer (pure CPU, no JS, no GL).
$(SECTORSTREAM_TARGET): $(SECTORSTREAM_OBJS)
	$(CC) $(SECTORSTREAM_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS)

run-sectorstream: $(SECTORSTREAM_TARGET)
	./$(SECTORSTREAM_TARGET)

$(SECTORCOORD_TARGET): $(SECTORCOORD_OBJS)
	$(CC) $(SECTORCOORD_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS)

run-sectorcoord: $(SECTORCOORD_TARGET)
	./$(SECTORCOORD_TARGET)

# modifier_apply stack runner (simplify/smooth/retopo, failure=skip, blacklist chunk hash).
$(MODAPPLY_TARGET): $(MODAPPLY_OBJS)
	$(CC) $(MODAPPLY_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-modapply: $(MODAPPLY_TARGET)
	./$(MODAPPLY_TARGET)

# Phase C Task 3 — terrain field program (pure CPU, no JS, no GL).
$(TERRAINFIELD_TARGET): $(TERRAINFIELD_OBJS)
	$(CC) $(TERRAINFIELD_OBJS) -o $@ $(CFLAGS) -I../src -lm

run-terrainfield: $(TERRAINFIELD_TARGET)
	./$(TERRAINFIELD_TARGET)

# Lightweight part_store loader: segmented v6 flat + instance refs.
$(PARTSTORE_TARGET): $(PARTSTORE_OBJS)
	$(CC) $(PARTSTORE_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-partstore: $(PARTSTORE_TARGET)
	./$(PARTSTORE_TARGET)

# Volumetrics Task 3 — CPU emitter gather (distance filter, 256 cap).
$(EMITTERGATHER_TARGET): $(EMITTERGATHER_OBJS)
	$(CC) $(EMITTERGATHER_OBJS) -o $@ $(CFLAGS) $(INCLUDE_PATHS) -lm

run-emittergather: $(EMITTERGATHER_TARGET)
	./$(EMITTERGATHER_TARGET)

# Modifier-region bake plumbing (Task 4): full bake through ScriptHost, region
# meshes welded across cells and stack-processed.
$(MODBAKE_TARGET): $(MODBAKE_OBJS)
	$(CC) $(MODBAKE_OBJS) -o $@ \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

$(ANIMATION_DIAGNOSTICS_TARGET): $(ANIMATION_DIAGNOSTICS_OBJS) | build
	$(CC) $(ANIMATION_DIAGNOSTICS_OBJS) -o $@ \
	      $(CFLAGS) $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS) $(OZZ_OFFLINE_LIBS)

run-animation-diagnostics: $(ANIMATION_DIAGNOSTICS_TARGET)
	./$(ANIMATION_DIAGNOSTICS_TARGET)

run-modbake: $(MODBAKE_TARGET)
	./$(MODBAKE_TARGET)

# ---------------------------------------------------------------------------
# GPU tests (require GL 4.6 + GALLIUM_DRIVER=d3d12 on WSLg).
# All share the same $(GPU_SHARED_OBJS) — one flavor `gpu` for the C++ pipeline
# TUs, `mslc` for MSL C sources, `qjsc` for QuickJS C sources.
# ---------------------------------------------------------------------------

# gpu-tests (gpu_cull_tests.cpp, GL 4.6 parity tests for the GpuCuller compute
# path) retired in Phase 5a (tech-debt.md §6): its own source directly tested
# viewer::GpuCuller/viewer::RasterComposer, both deleted outright along with
# src/render/renderer.*, raster_composer.*, gpu_culler.*. See run-gpucull's
# former location below for the target-name history.

# Retired in V5 (docs/vulkan-rt-gtex-bake.md §I.8 "GPU test targets"), together
# with the GL ray-tracing .gtex bake they exercised:
#   tileset_gpu_tests  (atlas well-formedness)
#   tileset_seam_tests (boundary-strip byte-equality)
#   tileset_load_tests (LocalProvider cold-cache bake through a GL harness)
# The first two assertions live on in the Vulkan bake's CPU harnesses
# (run-tilesetvkrepack / run-tilesetvkao / run-tilesetvkhorizon) and the
# device-side gates under run-vk-scene-renderer.

tileset-provider-tests: $(call obj_of,gpu,tileset_provider_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h build
	$(CC) $(call obj_of,gpu,tileset_provider_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o build/tileset_provider_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-tilesetprovider: tileset-provider-tests
	GALLIUM_DRIVER=d3d12 ./build/tileset_provider_tests

api-tests: $(call obj_of,gpu,api_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h build
	$(CC) $(call obj_of,gpu,api_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o build/api_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-api-tests: api-tests
	cd ../../MatterEditor && GALLIUM_DRIVER=d3d12 ../MatterEngine3/tests/build/api_tests

# Phase C Task 9 — world-kind session end-to-end test (GPU suite).
world-stream-tests: $(call obj_of,gpu,world_stream_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h build
	$(CC) $(call obj_of,gpu,world_stream_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o build/world_stream_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-worldstream: world-stream-tests
	cd ../../MatterEditor && GALLIUM_DRIVER=d3d12 MATTER_STREAM_RINGS="32:3,80:2" ../MatterEngine3/tests/build/world_stream_tests

# Phase B Task 6 — async bake worker command loop tests.
async-bake-tests: $(call obj_of,gpu,async_bake_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h build
	$(CC) $(call obj_of,gpu,async_bake_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o build/async_bake_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-asyncbake: async-bake-tests
	./build/async_bake_tests

# Phase C Task 13 — demand-driven bake primitives (BakePolicy + bake_plan + ensure_*).
demand-bake-tests: $(call obj_of,gpu,demand_bake_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h build
	$(CC) $(call obj_of,gpu,demand_bake_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o build/demand_bake_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-demandbake: demand-bake-tests
	./build/demand_bake_tests

# Phase C Task 2 — transient artifact routing (tmpfs scratch dir).
# Tests: bake transient modules to scratch, release_transient unlink,
# PartStore scratch-aware get_or_load, persistent modules still use cache.
transient-tests: $(call obj_of,gpu,transient_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h build
	$(CC) $(call obj_of,gpu,transient_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o build/transient_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-transient: transient-tests
	./build/transient_tests

# release-part-tests (release_part_tests.cpp, PartStore::release +
# GpuCuller::release_part) retired in Phase 5a (tech-debt.md §6): its own
# source directly tested viewer::GpuCuller, deleted outright along with
# src/render/renderer.*, raster_composer.*, gpu_culler.*.

# Task 3 — indexed VkScenePart assembly (CPU-only; no GL/Vulkan runtime needed).
# Compiles under the gpu flavor (needs Vulkan headers) but links only std libs.
vk-animation-skinning-tests: $(call obj_of,gpu,vk_animation_skinning_tests.cpp) $(call obj_of,gpu,../src/render/vk_animation_skinning.cpp) $(call obj_of,gpu,../src/animation/animation_budget.cpp) | ../shaders_gen/embedded_shaders.h build
	$(CC) $(call obj_of,gpu,vk_animation_skinning_tests.cpp) $(call obj_of,gpu,../src/render/vk_animation_skinning.cpp) $(call obj_of,gpu,../src/animation/animation_budget.cpp) \
	      -o build/vk_animation_skinning_tests $(FLAVOR_gpu_FLAGS) -lm -lpthread

run-vk-animation-skinning: vk-animation-skinning-tests
	./build/vk_animation_skinning_tests

vk-animation-skin-compute-tests: $(call obj_of,gpu,vk_animation_skin_compute_tests.cpp) $(call obj_of,gpu,../src/render/vk_animation_skinning.cpp) $(call obj_of,gpu,../src/animation/animation_budget.cpp) | ../shaders_gen/embedded_shaders.h build
	$(CC) $(call obj_of,gpu,vk_animation_skin_compute_tests.cpp) $(call obj_of,gpu,../src/render/vk_animation_skinning.cpp) $(call obj_of,gpu,../src/animation/animation_budget.cpp) \
	      -o build/vk_animation_skin_compute_tests $(FLAVOR_gpu_FLAGS) -lm -lpthread

run-vk-animation-skin-compute: vk-animation-skin-compute-tests
	./build/vk_animation_skin_compute_tests

vk-animation-bounds-tests: $(call obj_of,gpu,vk_animation_bounds_tests.cpp) $(call obj_of,gpu,../src/render/vk_animation_bounds.cpp) $(call obj_of,gpu,../src/render/vk_animation_skinning.cpp) $(call obj_of,gpu,../src/animation/animation_budget.cpp) | ../shaders_gen/embedded_shaders.h build
	$(CC) $(call obj_of,gpu,vk_animation_bounds_tests.cpp) $(call obj_of,gpu,../src/render/vk_animation_bounds.cpp) $(call obj_of,gpu,../src/render/vk_animation_skinning.cpp) $(call obj_of,gpu,../src/animation/animation_budget.cpp) \
	      -o build/vk_animation_bounds_tests $(FLAVOR_gpu_FLAGS) -lm -lpthread

run-vk-animation-bounds: vk-animation-bounds-tests
	./build/vk_animation_bounds_tests

../shaders_gen/embedded_spirv.h: ../shaders_vk/cull.comp
	$(MAKE) -C .. shaders_gen/embedded_spirv.h

# vk_scene_renderer.cpp now reaches the animation bounds/skinning
# implementations, so this target links their objects too (upstream links only
# the test TU because those sources do not exist there).
vk-scene-renderer-tests: $(call obj_of,gpu,vk_scene_renderer_tests.cpp) $(call obj_of,gpu,../src/render/vk_animation_bounds.cpp) $(call obj_of,gpu,../src/render/vk_animation_skinning.cpp) $(call obj_of,gpu,../src/animation/animation_budget.cpp) ../shaders_gen/embedded_spirv.h | ../shaders_gen/embedded_shaders.h build
	$(CC) $(call obj_of,gpu,vk_scene_renderer_tests.cpp) $(call obj_of,gpu,../src/render/vk_animation_bounds.cpp) $(call obj_of,gpu,../src/render/vk_animation_skinning.cpp) $(call obj_of,gpu,../src/animation/animation_budget.cpp) \
	      -o build/vk_scene_renderer_tests $(FLAVOR_gpu_FLAGS) -lm -lpthread

run-vk-scene-renderer: vk-scene-renderer-tests
	./build/vk_scene_renderer_tests

# run-gpucull (gpu_cull_tests.cpp GL cull-parity suite) retired alongside
# gpu-tests in Phase 5a — see the note near the old gpu-tests location above.

# Phase C Task 6 — camera-driven refine loop (headless, allow_gl_lt_46=true).
refine-loop-tests: $(call obj_of,gpu,refine_loop_tests.cpp) $(GPU_SHARED_OBJS) | ../shaders_gen/embedded_shaders.h build
	$(CC) $(call obj_of,gpu,refine_loop_tests.cpp) $(GPU_SHARED_OBJS) \
	      -o build/refine_loop_tests $(FLAVOR_gpu_FLAGS) $(LDFLAGS) $(LDLIBS) \
	      $(BOX3D_DIR)/libbox3d.a $(GPU_AR_CORE_LDFLAGS)

run-refineloop: refine-loop-tests
	GALLIUM_DRIVER=d3d12 ./build/refine_loop_tests

# Phase C Task 17 — resolve/manifest cache round-trip (headless, no GL needed).
resolve-cache-tests: $(call obj_list,def,$(RESOLVECACHE_CPP)) $(call obj_list,mslc,$(RESOLVECACHE_C)) | build
	$(CC) $(call obj_list,def,$(RESOLVECACHE_CPP)) $(call obj_list,mslc,$(RESOLVECACHE_C)) \
	      -o build/resolve_cache_tests $(FLAVOR_def_FLAGS) $(LDFLAGS) $(LDLIBS)

run-resolvecache: resolve-cache-tests
	./build/resolve_cache_tests

# The Vulkan smoke executable is a Windows artifact built with the standalone
# vendored GLFW archive owned by MatterEditor's cross-build rules.
.PHONY: vulkan-smoke
vulkan-smoke:
	$(MAKE) -C ../../MatterEditor vulkan-smoke

# ---------------------------------------------------------------------------
# Clean: every object (build/obj/) and every binary (build/*) now lives under
# build/, so removing the whole tree is sufficient. The clean-* sub-targets
# below (chained on as extra prerequisites) are redundant with this but kept
# so `make clean-eventhub` etc. still work standalone.
# ---------------------------------------------------------------------------
clean:
	rm -rf build

# ---------------------------------------------------------------------------
# Bake Lab task 5.3: pose-delta metric unit tests (settle-tick-optimizer.md
# §II.3). Self-contained append-only block: separate .PHONY line and a clean
# hook so the shared .PHONY/clean lines above stay untouched.
# ---------------------------------------------------------------------------
build/tileset_metrics_tests: tileset_metrics_tests.cpp ../src/tileset_metrics.cpp ../src/tileset_metrics.h | build
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include -I../src \
	    -o $@ tileset_metrics_tests.cpp ../src/tileset_metrics.cpp -lm

run-tilesetmetrics: build/tileset_metrics_tests
	./build/tileset_metrics_tests

.PHONY: run-tilesetmetrics clean-tilesetmetrics
clean: clean-tilesetmetrics
clean-tilesetmetrics:
	rm -f build/tileset_metrics_tests

# ---------------------------------------------------------------------------
# E1a — evt::Channel<T> transport primitive tests (event-system.md §II.1 E1).
# Self-contained append-only block, same pattern as tileset_metrics_tests
# above: separate .PHONY line and clean hook so the shared .PHONY/clean lines
# stay untouched. Header-only template (MatterEngine3/include/matter/event/
# channel.h) — no engine .cpp deps, no GL/raylib. -lpthread for the
# std::thread producer/consumer tests.
# ---------------------------------------------------------------------------
build/event_channel_tests: event_channel_tests.cpp ../include/matter/event/channel.h check.h | build
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include \
	    -o $@ event_channel_tests.cpp -lpthread

run-eventchannel: build/event_channel_tests
	./build/event_channel_tests

.PHONY: run-eventchannel clean-eventchannel
clean: clean-eventchannel
clean-eventchannel:
	rm -f build/event_channel_tests

# ---------------------------------------------------------------------------
# E1b -- evt::Hub tests (event-system.md S II.1 E1, S II.4 items 1 and 7).
# Links the Hub's own implementation objects directly (small TU count);
# mirrors event_channel_tests' pattern -- self-contained, no engine .cpp
# deps beyond matter/event/*, no GL/raylib. -lpthread for the cross-thread
# lifetime/race tests. -Isrc is needed because event_hub.h reaches
# matter/event/detail/dispatch_context.h under include/, but
# dispatch_context.cpp/subscription.cpp/event_hub.cpp themselves live
# under src/event and are compiled directly here (not via the kernel lib)
# so this target builds standalone, like event_channel_tests above.
# ---------------------------------------------------------------------------
EVENTHUB_SRC = event_hub_tests.cpp \
               ../src/event/event_hub.cpp \
               ../src/event/subscription.cpp \
               ../src/event/dispatch_context.cpp
EVENTHUB_HEADERS = check.h ../include/matter/event/channel.h \
                   ../include/matter/event/subscription.h \
                   ../include/matter/event/event_name.h \
                   ../include/matter/event/registration_error.h \
                   ../include/matter/event/event_hub.h \
                   ../include/matter/event/detail/dispatch_context.h

build/event_hub_tests: $(EVENTHUB_SRC) $(EVENTHUB_HEADERS) | build
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include -I../src \
	    -o $@ $(EVENTHUB_SRC) -lpthread

run-eventhub: build/event_hub_tests
	./build/event_hub_tests

.PHONY: run-eventhub clean-eventhub
clean: clean-eventhub
clean-eventhub:
	rm -f build/event_hub_tests

# ---------------------------------------------------------------------------
# I.11 -- error-sink migration tests (event-system.md S I.11 Notification-sink
# row, S I.6 immediate delivery). Proves the two hub-backed sink adapters
# (live_edit::HubErrorSink, scene::make_hub_error_sink) emit error.live_edit /
# error.part_instance{,_clear} with the right fields. Self-contained like
# event_hub_tests above: the adapters are header-only, so this compiles only
# the event impl objects (no kernel lib, no flecs, no GL). -Isrc reaches
# live_edit_error_hub.h + ecs/bridge_error_hub.h.
# ---------------------------------------------------------------------------
ERROREVENTS_SRC = error_events_tests.cpp \
                  ../src/event/event_hub.cpp \
                  ../src/event/subscription.cpp \
                  ../src/event/dispatch_context.cpp
ERROREVENTS_HEADERS = check.h ../include/matter/event/event_hub.h \
                      ../include/matter/event/event_name.h \
                      ../include/matter/events/error_events.h \
                      ../src/live_edit_error_hub.h \
                      ../src/live_edit_interfaces.h \
                      ../src/ecs/bridge_error_hub.h \
                      ../src/ecs/bridge_error_sink.h

build/error_events_tests: $(ERROREVENTS_SRC) $(ERROREVENTS_HEADERS) | build
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include -I../src \
	    -o $@ $(ERROREVENTS_SRC) -lpthread

run-errorevents: build/error_events_tests
	./build/error_events_tests

.PHONY: run-errorevents clean-errorevents
clean: clean-errorevents
clean-errorevents:
	rm -f build/error_events_tests

# ---------------------------------------------------------------------------
# E4a -- evt::CommandRegistry tests (event-system.md S I.10, S II.4 items 2
# and 5). Same self-contained pattern as event_hub_tests above: compiles the
# command layer's own implementation objects directly (command.cpp plus the
# Hub/subscription objects it depends on for the cmd.* notifications), no
# engine .cpp deps beyond matter/event/*, no GL/raylib. -Isrc is needed for
# matter/event/detail/dispatch_context.h; -lpthread for the cross-thread
# dispatch/ticket/wait tests.
# ---------------------------------------------------------------------------
EVENTCMD_SRC = event_command_tests.cpp \
               ../src/event/command.cpp \
               ../src/event/event_hub.cpp \
               ../src/event/subscription.cpp \
               ../src/event/dispatch_context.cpp
EVENTCMD_HEADERS = check.h ../include/matter/event/channel.h \
                   ../include/matter/event/subscription.h \
                   ../include/matter/event/event_name.h \
                   ../include/matter/event/registration_error.h \
                   ../include/matter/event/event_hub.h \
                   ../include/matter/event/command.h \
                   ../include/matter/event/detail/dispatch_context.h

build/event_command_tests: $(EVENTCMD_SRC) $(EVENTCMD_HEADERS) | build
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include -I../src \
	    -o $@ $(EVENTCMD_SRC) -lpthread

run-eventcommand: build/event_command_tests
	./build/event_command_tests

.PHONY: run-eventcommand clean-eventcommand
clean: clean-eventcommand
clean-eventcommand:
	rm -f build/event_command_tests

# ---------------------------------------------------------------------------
# E5a -- evt::Property<T> + evt::PropertyScheduler tests (event-system.md
# S I.9, S II.1 E5). Same self-contained pattern as event_hub_tests above:
# the scheduler's only impl object is property.cpp, and it depends on the
# swappable fail_fast seam (subscription.cpp / registration_error). No engine
# .cpp deps beyond matter/event/*, no GL/raylib. -Isrc is unnecessary here
# (property.h reaches only subscription.h + registration_error.h under
# include/), but kept for parity. -lpthread for the affinity off-thread test.
# ---------------------------------------------------------------------------
EVENTPROP_SRC = event_property_tests.cpp \
                ../src/event/property.cpp \
                ../src/event/subscription.cpp \
                ../src/event/dispatch_context.cpp
EVENTPROP_HEADERS = check.h ../include/matter/event/property.h \
                    ../include/matter/event/subscription.h \
                    ../include/matter/event/registration_error.h \
                    ../include/matter/event/detail/dispatch_context.h

build/event_property_tests: $(EVENTPROP_SRC) $(EVENTPROP_HEADERS) | build
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include -I../src \
	    -o $@ $(EVENTPROP_SRC) -lpthread

run-eventproperty: build/event_property_tests
	./build/event_property_tests

.PHONY: run-eventproperty clean-eventproperty
clean: clean-eventproperty
clean-eventproperty:
	rm -f build/event_property_tests

# ---------------------------------------------------------------------------
# E5b -- scene::SceneChangeTracker + scene::SceneService tests
# (event-system.md S I.14 / S II.1 E5). Headless, Windows-runnable: flecs +
# the event hub, no GL/raylib/box3d. Object-based (not the single-g++-command
# pattern) because flecs.c must compile via the flecsc (gcc-as-C) flavor, like
# the run-ecs-entity-bridge target. Links the Windows flecs runtime libs
# (ws2_32/dbghelp/winmm) the scene-registry target already uses.
# ---------------------------------------------------------------------------
SCENETRACKER_TARGET = build/scene_tracker_tests
SCENETRACKER_CPP    = scene_tracker_tests.cpp \
                      ../src/scene/scene_service.cpp \
                      ../src/scene/scene_change_tracker.cpp \
                      ../src/event/event_hub.cpp \
                      ../src/event/subscription.cpp \
                      ../src/event/dispatch_context.cpp
SCENETRACKER_C      = $(FLECS_C)
SCENETRACKER_OBJS   = $(call obj_list,def,$(SCENETRACKER_CPP)) $(call obj_list,flecsc,$(SCENETRACKER_C))

# Generate the per-source compile rules for these sources (they are not in the
# line-833 def_CPP_SRCS union, so without this the generic $(OBJDIR)/% mkdir
# rule would wrongly create the .o paths as directories). flecs.c already has
# its rule via flecsc_C_SRCS. Append-only; none of these sources collide with
# def_CPP_SRCS entries.
$(foreach s,$(SCENETRACKER_CPP),$(eval $(call objrule_cpp,def,$(s))))

$(SCENETRACKER_TARGET): $(SCENETRACKER_OBJS)
	$(CC) $(SCENETRACKER_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread -lws2_32 -ldbghelp -lwinmm

run-scenetracker: $(SCENETRACKER_TARGET)
	./$(SCENETRACKER_TARGET)

.PHONY: run-scenetracker clean-scenetracker
clean: clean-scenetracker
clean-scenetracker:
	rm -f build/scene_tracker_tests

# ---------------------------------------------------------------------------
# E6 -- physics contact/sensor events as flecs entity events (event-system.md
# S I.4 / S I.11 PhysicsEvents row, S II.1 E6). The consuming proof: a flecs
# observer receives the per-entity gameplay events the pull stage now emits,
# and the session hub trace records the aggregate phys.step. Headless,
# Windows-runnable (physics engine + flecs + box3d + the event hub, no
# GL/raylib) -- links like the physics target ($(LDFLAGS) $(BOX3D_LIB)).
# The physics engine sources already carry def rules (via PHYSICS_CPP in the
# line-833 union) and the event sources carry def rules (via the scene_tracker
# target above); only physicsevents_tests.cpp needs a fresh def rule. flecs.c
# reuses the physicsc flavor object the physics target builds.
# ---------------------------------------------------------------------------
PHYSICSEVENTS_TARGET = build/physicsevents_tests
PHYSICSEVENTS_CPP    = physicsevents_tests.cpp ../src/ecs/scene_registry.cpp \
                       ../src/ecs/ecs_runtime.cpp ../src/ecs/physics_context.cpp \
                       ../src/ecs/physics_shapes.cpp ../src/ecs/physics_systems.cpp \
                       ../src/ecs/streaming_systems.cpp ../src/ecs/transform_system.cpp \
                       ../src/streaming/sector_streaming_coordinator.cpp \
                       ../src/sector_streamer.cpp \
                       ../src/event/event_hub.cpp ../src/event/subscription.cpp \
                       ../src/event/dispatch_context.cpp
PHYSICSEVENTS_C      = $(FLECS_C)
PHYSICSEVENTS_OBJS   = $(call obj_list,def,$(PHYSICSEVENTS_CPP)) $(call obj_list,physicsc,$(PHYSICSEVENTS_C))

$(eval $(call objrule_cpp,def,physicsevents_tests.cpp))

# Links like the scene-registry target: box3d + the Windows flecs runtime libs
# (ws2_32/dbghelp/winmm) flecs.c pulls in for its HTTP/OS layer, so the binary
# actually runs headless on Windows (not just compiles).
$(PHYSICSEVENTS_TARGET): $(PHYSICSEVENTS_OBJS) $(BOX3D_LIB)
	$(CC) $(PHYSICSEVENTS_OBJS) -o $@ $(FLAVOR_def_FLAGS) -lm -lpthread $(BOX3D_LIB) -lws2_32 -ldbghelp -lwinmm

run-physicsevents: $(PHYSICSEVENTS_TARGET)
	./$(PHYSICSEVENTS_TARGET)

.PHONY: run-physicsevents clean-physicsevents
clean: clean-physicsevents
clean-physicsevents:
	rm -f build/physicsevents_tests
