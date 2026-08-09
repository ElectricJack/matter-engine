# MatterEditor — world editor application (Vulkan build).
#
# STALE HEADER NOTE (kept for history): this comment used to describe a Linux
# GL target that compiled main.cpp + ui.cpp + imgui and linked
# libmatter_engine3.a. That target and its sources (main_linux.cpp,
# ui_linux.cpp) were deleted outright in Phase 5a (tech-debt.md §6 / GL-path
# deletion) -- see docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md.
#
# Windows: MinGW-hosted native compile (WIN_CXX runs ON Windows via MSYS2
#   UCRT64, it is not a Linux->Windows cross-compiler) that does direct-source
#   compilation of all engine+MSL+QJS units with updated ../MatterEngine3/src/...
#   paths. (Pragmatic: no mingw .a packaging this phase. Private ME3 src/
#   includes are scoped to the Windows variable set below.) This is the only
#   target verified in this repository's normal Windows dev environment.
#
# Linux (Phase 5b, ADDED, UNVERIFIED): the `linux` target below mirrors the
# Windows target's shape almost exactly -- same APP_SRC/WIN_ME3_CPP/WIN_MSL_CPP/
# WIN_PIPELINE_C/IMGUI_SRC_WIN direct-source-compile lists (they were never
# actually Windows-specific, just never built anywhere else), a Vulkan-only
# GLFW build against the X11 backend instead of Win32, and
# -DMATTER_HAVE_STREAMLINE=0 hardcoded (Streamline/DLSS is Windows-only).
# There is no Linux machine, toolchain, or Vulkan loader anywhere in the
# environment this was written in, so this target has never been compiled or
# run. See MatterEditor/README.md for the full list of what is and is not
# verified, and the "Linux Vulkan target" comment block near APP_SRC/GLFW_LINUX_SRC
# below for per-line justification.
#
# Per-object parallel build: each TU compiles to its own .o under build/<platform>/
# so `make -j` fans them across cores; header changes are tracked via -MMD -MP
# generated .d files (so incremental rebuilds are correct, not silently stale).
JOBS ?= $(shell nproc)
MAKEFLAGS += -j$(JOBS)

CC = g++
CFLAGS = -std=c++17 -O2 -Wall -Wno-missing-braces -Wno-unused-variable \
         -DPLATFORM_DESKTOP

RAYLIB_PATH = ../third_party/raylib/src
IMGUI_PATH  = ../third_party/imgui
# Official ImGuizmo upstream commit: dc25afb98bc3ebe00dfc9a23ba7235fead2ccb1d
IMGUIZMO_PATH = ../third_party/ImGuizmo
QJS_DIR     = ../third_party/quickjs-ng
QJS_INC     = -I$(QJS_DIR)
BOX3D_DIR   = ../third_party/box3d
# Vendored CPU BC1-7 block encoders (MIT / public domain -- see
# third_party/bc7enc/README.md). Reached only through
# MatterEngine3/src/render/bc_encode.{h,cpp}.
BC7ENC_DIR  = ../third_party/bc7enc
BOX3D_LIB   = $(BOX3D_DIR)/libbox3d.a
WIN_BOX3D_LIB = $(BOX3D_DIR)/build-mingw/libbox3d.a
ME3_DIR     = ../MatterEngine3
MSL_DIR     = ../libs/MatterSurfaceLib
PROFILE_DIR = ../libs/ProfileLib
FLECS_DIR   = ../third_party/flecs
# Shared source-of-truth libraries (compiled from source, not copied).
SQL_DIR     = ../libs/SpatialQueryLib
MEMLIB_DIR  = ../libs/MemoryLib
MATHLIB_DIR = ../libs/MathLib
# Ozz-animation. The editor bakes as well as runs -- a game ships the whole
# engine and editor, so there is no separate runtime binary to keep the offline
# builders out of. It therefore links OZZ_OFFLINE_LIBS (offline + runtime); see
# check-animation-production-sources, which asserts the bake unit stays in.
OZZ_DIR     = ../third_party/ozz-animation
OZZ_BUILD_DIR = $(OZZ_DIR)/build/matter
OZZ_INCLUDE = $(OZZ_DIR)/include
OZZ_BASE_LIB = $(OZZ_BUILD_DIR)/src/base/libozz_base.a
OZZ_ANIMATION_LIB = $(OZZ_BUILD_DIR)/src/animation/runtime/libozz_animation.a
OZZ_OFFLINE_LIB = $(OZZ_BUILD_DIR)/src/animation/offline/libozz_animation_offline.a
OZZ_RUNTIME_LIBS = $(OZZ_ANIMATION_LIB) $(OZZ_BASE_LIB)
OZZ_OFFLINE_LIBS = $(OZZ_OFFLINE_LIB) $(OZZ_RUNTIME_LIBS)

# Linux app compile: only public engine API + imgui + raylib headers visible.
# Private ME3 src/ headers are NOT exposed here; they are internal to the library.
# -I$(ME3_DIR)/src: world_session.h currently includes the private
# render/vk_gi_contract.h; drop this once the public API stops leaking it.
INCLUDE_PATHS = -I$(ME3_DIR)/include \
                -I$(ME3_DIR)/src \
                -I$(MSL_DIR)/include \
                -I$(SQL_DIR)/include -I$(MEMLIB_DIR)/include \
                -I$(MATHLIB_DIR)/include \
                -I$(PROFILE_DIR)/include \
                -I$(RAYLIB_PATH) \
                -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends \
                -I$(IMGUIZMO_PATH) \
                -I$(RAYLIB_PATH)/external/glfw/include \
                -I$(BOX3D_DIR)/include \
                -I$(FLECS_DIR) \
                -I$(OZZ_INCLUDE) \
                -I$(BC7ENC_DIR) \
                -I../third_party/Vulkan-Headers/include

# Windows direct-source build also needs private ME3 src/ headers since it
# compiles all engine sources directly (no .a). $(VULKAN_INCLUDE) comes first
# so the MSYS2 Vulkan headers win over the vendored copy.
WIN_INCLUDE_PATHS = -I$(VULKAN_INCLUDE) \
                    $(INCLUDE_PATHS) \
                    -I$(ME3_DIR) \
                    -I$(ME3_DIR)/src \
                    -I$(ME3_DIR)/src/render \
                     -I$(ME3_DIR)/src/provider \
                     -I$(MSL_DIR)/src \
                     -I$(FLECS_DIR) \
                     -I../libs/ParticleFlowLib/include

LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11
LDLIBS  = ../third_party/raylib/build/linux/libraylib.a $(BOX3D_LIB) $(OZZ_OFFLINE_LIBS)

HAVE_STREAMLINE ?= 0
# Point this at a locally installed, licensed Streamline SDK.  Nothing from the
# SDK is checked into this repository; HAVE_STREAMLINE=0 is the default.
STREAMLINE_PATH ?=
# The SDK ships two DLL sets: <path>/bin/x64 (16 release-only DLLs) and
# <path>/bin/x64/development (18 -- adds sl.imgui + sl.nvperf). development is
# what tools/check_vulkan_toolchain.sh, build-dlss.sh, and every historical
# build here actually use, so it is the default; override STREAMLINE_DLL_DIR
# to ship the smaller release set instead.
STREAMLINE_DLL_DIR ?= $(STREAMLINE_PATH)/bin/x64/development
# ---- autoremesher_core: the retopo modifier backend ----
# libautoremesher_core.a is self-contained on every platform — its TBB
# replacement (third_party/autoremesher_core/thirdparty/tbb_shim/) is
# header-only — so linking it is just the archive: no companion runtime
# library, no rpath, and nothing platform-conditional.
#
# The archive is a prerequisite of the viewer targets, so a fresh checkout
# builds it automatically. It is vendored code that does not change between
# builds, so the rule has no prerequisites of its own and fires only when the
# archive is missing; after that it costs nothing. Pass RETOPO=0 to build the
# editor without retopo — schemas whose modifier stacks opt into it then hit
# the warn-and-continue path in MatterEngine3/src/modifier_apply.cpp.
RETOPO ?= 1
AR_CORE_PATH := ../third_party/autoremesher_core
AR_CORE_LIB  := $(AR_CORE_PATH)/libautoremesher_core.a

ifeq ($(RETOPO),1)
  HAVE_AUTOREMESHER := 1
  # -DMATTER_HAVE_AUTOREMESHER enables the retopo branch in modifier_apply.cpp
  # and the retopo() glue in MatterSurfaceLib's mesh_retopo.cpp.
  AR_CORE_CFLAGS := -DMATTER_HAVE_AUTOREMESHER -I$(AR_CORE_PATH)/include
  # --allow-multiple-definition: raylib bundles stb_image_write.h in
  # rtextures.c and autoremesher_core's geogram bundles its own copy in
  # image_serializer_stb.o. Where both archives land in one link, the stb
  # sources are byte-equivalent vendor copies, so either definition is safe.
  AR_CORE_LDFLAGS := -L$(AR_CORE_PATH) -lautoremesher_core \
                     -Wl,--allow-multiple-definition
  AR_CORE_DEP     := ar-core
  AR_CORE_MSL_CPP := $(MSL_DIR)/src/mesh_retopo.cpp
else
  HAVE_AUTOREMESHER :=
  AR_CORE_CFLAGS :=
  AR_CORE_LDFLAGS :=
  AR_CORE_DEP :=
  AR_CORE_MSL_CPP :=
endif

# Delegate to autoremesher_core's own Makefile on EVERY build rather than
# treating the archive as an up-to-date file target.
#
# This used to be `$(AR_CORE_LIB):` with no prerequisites, which meant make ran
# the recipe only when the archive was MISSING. Edit a vendored source and the
# stale archive still satisfied the dependency, so `make -C MatterEditor
# windows` reported success while linking the old code. That is exactly what
# happened merging the retopo ASSERT_THROW fix: remesh.cpp updated, remesh.o
# stayed a day old, the editor linked clean, and the crash it fixed would have
# reproduced -- with a fresh editor.exe timestamp and a zero exit code arguing
# nothing was wrong.
#
# Listing the archive's sources as prerequisites here would mean globbing the
# whole vendored geogram tree and duplicating knowledge that already lives one
# directory down. The sub-make has correct per-object rules; let it decide.
# Cost when nothing changed is one recursive make that stats its objects.
.PHONY: ar-core
ar-core:
	@$(MAKE) -C $(AR_CORE_PATH)

# ---- Linux viewer: GL path RETIRED in Phase 5a, Vulkan target ADDED in 5b ----
# main_linux.cpp, ui_linux.cpp and ui_linux.h (the GL/raylib entry point + imgui
# OpenGL3 backend glue) were deleted outright in Phase 5a -- not migrated.
# Phase 5b (docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md)
# added the `linux` target (see the "Linux Vulkan target (Phase 5b)" section
# below, after win-shaders-gpu): it compiles this same APP_SRC (already
# raylib/GL-free) with -DMATTER_VULKAN_ONLY -DMATTER_VULKAN_VIEWER, a
# Vulkan-only GLFW build against X11 (mirroring glfw_vulkan_only_context.c,
# see src/glfw_vulkan_only_context_x11.c), and links -lvulkan + -lX11 instead
# of -lGL. `windows` remains the only target actually built/run/verified in
# this repository's normal dev environment -- see MatterEditor/README.md for
# what `linux` has and has not been checked against (no Linux machine
# available where it was written).
APP_SRC = src/main.cpp src/ui.cpp src/ui_lighting_controls.cpp src/animation_debug_overlay.cpp src/animation_panel.cpp src/animation_panel_model.cpp src/session_binding.cpp src/scene_model_adapter.cpp src/bake_lab.cpp src/bake_lab_timeline.cpp src/event_inspector.cpp src/asset_browser.cpp src/part_workbench.cpp src/part_workbench_iso.cpp src/reveal_part.cpp src/lod_inspector.cpp src/camera_controller.cpp src/camera_focus.cpp src/camera_orbit.cpp src/streaming_anchor_controller.cpp src/selection_set.cpp src/editor_model.cpp src/scene_tree_panel.cpp src/selection_bounds.cpp src/viewport_pick.cpp src/selection_outline.cpp src/toolbar_panel.cpp src/console_panel.cpp src/console_log.cpp src/image_preview.cpp src/issue_reporter.cpp src/issue_reporter_panel.cpp src/shot_replay.cpp src/properties_registry.cpp src/properties_panel.cpp src/specialized_editors.cpp src/property_editor.cpp src/editor_props.cpp src/gizmo.cpp src/os_open.cpp

# Windows direct-source compile: ME3 pipeline + MSL + QJS (mirrors old viewer).
# These are ONLY used by the Windows target; the Linux target links libmatter_engine3.a.
WIN_ME3_CPP = $(ME3_DIR)/src/part_graph.cpp \
              $(ME3_DIR)/src/animation/ozz_adapter.cpp \
              $(ME3_DIR)/src/animation/ozz_bake.cpp \
              $(ME3_DIR)/src/animation/animation_ir.cpp \
              $(ME3_DIR)/src/animation/animation_validate.cpp \
              $(ME3_DIR)/src/animation/animation_binding_bake.cpp \
              $(ME3_DIR)/src/animation/animation_runtime_asset.cpp \
              $(ME3_DIR)/src/animation/anim_asset.cpp \
              $(ME3_DIR)/src/animation/anim_bundle.cpp \
              $(ME3_DIR)/src/animation/animation_asset_store.cpp \
              $(ME3_DIR)/src/animation/animation_store.cpp \
              $(ME3_DIR)/src/animation/animation_budget.cpp \
              $(ME3_DIR)/src/animation/animation_evaluator.cpp \
              $(ME3_DIR)/src/animation/animation_systems.cpp \
              $(ME3_DIR)/src/animation/animation_world_queries.cpp \
              $(ME3_DIR)/src/animation/animation_controllers.cpp \
              $(ME3_DIR)/src/animation/animation_targets.cpp \
              $(ME3_DIR)/src/script_host.cpp $(ME3_DIR)/src/dsl_state.cpp \
              $(ME3_DIR)/src/dsl_triangle.cpp $(ME3_DIR)/src/dsl_bindings.cpp \
              $(ME3_DIR)/src/dsl_animation.cpp \
              $(ME3_DIR)/src/csg_lowering.cpp $(ME3_DIR)/src/module_resolver.cpp \
              $(ME3_DIR)/src/script/world_definition_loader.cpp \
              $(ME3_DIR)/src/script_rng_binding.cpp \
              $(ME3_DIR)/src/polygon_triangulate.cpp \
              $(ME3_DIR)/src/part_asset_v2.cpp $(ME3_DIR)/src/lod_bake.cpp \
              $(ME3_DIR)/src/warp_field.cpp \
              $(ME3_DIR)/src/world_flatten.cpp $(ME3_DIR)/src/part_flatten.cpp \
              $(ME3_DIR)/src/impostor_bake.cpp \
              $(ME3_DIR)/src/triangle_emit.cpp \
              $(ME3_DIR)/src/part_cluster.cpp \
              $(ME3_DIR)/src/tileset_placement.cpp $(ME3_DIR)/src/tileset_layout.cpp \
              $(ME3_DIR)/src/tileset_torus_bvh.cpp \
              $(ME3_DIR)/src/tileset_gtex.cpp \
              $(ME3_DIR)/src/sector_grid.cpp $(ME3_DIR)/src/lod_select.cpp \
              $(ME3_DIR)/src/world_lights.cpp \
              $(ME3_DIR)/src/world_tracer.cpp \
              $(ME3_DIR)/src/shader_source.cpp \
              $(ME3_DIR)/src/tileset_phase.cpp \
              $(ME3_DIR)/src/tileset_bake.cpp $(ME3_DIR)/src/tileset_settle.cpp \
              $(ME3_DIR)/src/tileset_collider.cpp $(ME3_DIR)/src/tileset_part_collider.cpp \
              \
              $(ME3_DIR)/src/render/raster_mesh.cpp $(ME3_DIR)/src/render/indexed_part_geometry.cpp $(ME3_DIR)/src/render/part_store.cpp \
              $(ME3_DIR)/src/render/world_state.cpp \
              $(ME3_DIR)/src/render/lod_trace.cpp \
              $(ME3_DIR)/src/render/streamline_bridge.cpp \
              $(ME3_DIR)/src/render/vk_context.cpp $(ME3_DIR)/src/render/vk_resources.cpp \
              $(ME3_DIR)/src/render/vk_pipeline.cpp $(ME3_DIR)/src/render/vk_scene_renderer.cpp \
              $(ME3_DIR)/src/render/vk_animation_skinning.cpp $(ME3_DIR)/src/render/vk_animation_bounds.cpp \
              $(ME3_DIR)/src/render/animation_skin_bridge.cpp \
              $(ME3_DIR)/src/render/tileset_slicer.cpp \
              $(ME3_DIR)/src/render/bc_encode.cpp \
              $(ME3_DIR)/src/render/vt_compositor.cpp \
              $(ME3_DIR)/src/render/vt_enrich.cpp \
              $(ME3_DIR)/src/render/vt_residency.cpp \
              $(ME3_DIR)/src/render/vt_stub_filler.cpp \
              $(BC7ENC_DIR)/bc7enc.cpp $(BC7ENC_DIR)/rgbcx.cpp \
              $(ME3_DIR)/src/render/tileset_bake_vk.cpp \
              $(ME3_DIR)/src/render/vk_volumetrics.cpp $(ME3_DIR)/src/render/vk_atmosphere.cpp $(ME3_DIR)/src/render/vk_emitter_gather.cpp \
              $(ME3_DIR)/src/render/vk_instance_cache.cpp \
              $(ME3_DIR)/src/render/vulkan_only_compat.cpp \
              $(ME3_DIR)/src/render/frame_matrices.cpp $(ME3_DIR)/src/render/matrix_math.cpp \
              $(ME3_DIR)/src/render/vk_temporal.cpp $(ME3_DIR)/src/render/vk_gi_math.cpp \
              $(ME3_DIR)/src/render/vk_lighting_controls.cpp \
              $(ME3_DIR)/src/provider/local_provider.cpp $(ME3_DIR)/src/provider/resolvers.cpp \
              $(ME3_DIR)/src/modifier_apply.cpp $(ME3_DIR)/src/retopo_blacklist.cpp \
              $(ME3_DIR)/src/async_bake.cpp \
              $(ME3_DIR)/src/live_edit.cpp \
              $(ME3_DIR)/src/live_edit_prod.cpp \
              $(ME3_DIR)/src/inotify_watcher.cpp \
              $(ME3_DIR)/src/part_graph_snapshot.cpp \
              $(ME3_DIR)/src/refine_controller.cpp \
              $(ME3_DIR)/src/resolve_cache.cpp \
              $(ME3_DIR)/src/terrain_field.cpp \
              $(ME3_DIR)/src/terrain_mesher.cpp \
              $(ME3_DIR)/src/sector_streamer.cpp \
              $(ME3_DIR)/src/ecs/ecs_runtime.cpp \
              $(ME3_DIR)/src/ecs/physics_context.cpp \
              $(ME3_DIR)/src/ecs/physics_shapes.cpp \
              $(ME3_DIR)/src/ecs/physics_systems.cpp \
              $(ME3_DIR)/src/ecs/streaming_systems.cpp \
              $(ME3_DIR)/src/ecs/transform_system.cpp \
              $(ME3_DIR)/src/ecs/scene_registry.cpp \
              $(ME3_DIR)/src/ecs/simulation_control.cpp \
              $(ME3_DIR)/src/ecs/dynamic_scene_bridge.cpp \
              $(ME3_DIR)/src/render/animation_rigid_bridge.cpp \
              $(ME3_DIR)/src/render/dynamic_instance_slots.cpp \
              $(ME3_DIR)/src/streaming/sector_streaming_coordinator.cpp \
              $(ME3_DIR)/src/matter_engine.cpp \
              $(ME3_DIR)/src/event/event_hub.cpp \
              $(ME3_DIR)/src/event/subscription.cpp \
              $(ME3_DIR)/src/event/dispatch_context.cpp \
              $(ME3_DIR)/src/event/command.cpp \
              $(ME3_DIR)/src/event/property.cpp \
              $(ME3_DIR)/src/util/json_doc.cpp \
              $(ME3_DIR)/src/props/props.cpp \
              $(ME3_DIR)/src/props/props_file.cpp \
              $(ME3_DIR)/src/props/draw_overrides.cpp \
              $(ME3_DIR)/src/scene/scene_service.cpp \
              $(ME3_DIR)/src/scene/scene_change_tracker.cpp \
              $(ME3_DIR)/src/bake_trace.cpp \
              $(ME3_DIR)/src/pf_bindings.cpp \
              ../libs/ParticleFlowLib/src/pf_math.cpp \
              ../libs/ParticleFlowLib/src/pf_sim.cpp \
              ../libs/ParticleFlowLib/src/pf_fields.cpp \
              ../libs/ParticleFlowLib/src/pf_path_recorder.cpp

WIN_MSL_CPP = $(MSL_DIR)/src/cluster.cpp $(MSL_DIR)/src/cell.cpp \
              $(MSL_DIR)/src/mesh_simplifier.cpp $(MSL_DIR)/src/mesh_indexed.cpp \
              $(MSL_DIR)/src/mesh_smooth.cpp $(MSL_DIR)/src/mesh_transform.cpp \
              $(MSL_DIR)/src/blas_manager.cpp $(SQL_DIR)/src/bvh.cpp \
              $(SQL_DIR)/src/bvh_analyzer.cpp \
              $(MSL_DIR)/src/mesh_worker_pool.cpp $(MSL_DIR)/src/mesh_build_utils.cpp \
              $(MSL_DIR)/src/meshing_algorithm.cpp \
              $(MSL_DIR)/src/marching_cubes_algorithm.cpp \
              $(MSL_DIR)/src/oriented_cube_algorithm.cpp \
              $(MSL_DIR)/src/vertex_ao.cpp \
              $(MSL_DIR)/src/occupancy.cpp $(MSL_DIR)/src/tlas_manager.cpp \
              $(MSL_DIR)/src/part_asset.cpp \
              $(MSL_DIR)/src/lattice.cpp $(MSL_DIR)/src/particle_culling.cpp \
              ../libs/MeshChartingLib/src/mesh_charting.cpp \
              $(AR_CORE_MSL_CPP)

WIN_PIPELINE_C = $(MSL_DIR)/src/surface.c \
                 $(SQL_DIR)/src/spatial_hash.c $(MEMLIB_DIR)/src/mem_pool.c \
                 $(MSL_DIR)/src/material_registry.c $(MSL_DIR)/src/fat_primitive.c

QJS_C   = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c $(QJS_DIR)/libunicode.c \
          $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c

FLECS_C = $(FLECS_DIR)/flecs.c

IMGUI_CORE_SRC = $(IMGUI_PATH)/imgui.cpp $(IMGUI_PATH)/imgui_draw.cpp \
            $(IMGUI_PATH)/imgui_tables.cpp $(IMGUI_PATH)/imgui_widgets.cpp \
            $(IMGUI_PATH)/backends/imgui_impl_glfw.cpp
IMGUIZMO_SRC = $(IMGUIZMO_PATH)/ImGuizmo.cpp
# IMGUI_SRC_LINUX (imgui_impl_opengl3.cpp backend) retired with the Linux GL
# viewer above. Phase 5b's `linux` target below reuses IMGUI_SRC_WIN as-is
# (the Vulkan+GLFW imgui backend, not actually Windows-specific despite the
# name) rather than reintroducing an OpenGL3 one -- see LINUX_ALL_CPP_SRC.
IMGUI_SRC_WIN = $(IMGUI_CORE_SRC) $(IMGUIZMO_SRC) \
            $(IMGUI_PATH)/backends/imgui_impl_vulkan.cpp

# ---- Per-object lists (one .o per TU, in a platform build dir, so `make -j`
# can compile them concurrently). Basenames are unique across all source dirs,
# so flattening with $(notdir ...) is collision-free. vpath (below) lets the
# pattern rules find each source by basename.
#
# Windows: full direct-source compile (APP_SRC + WIN_ME3_CPP + WIN_MSL_CPP + IMGUI_SRC).
# Windows is the release build path for now: -DNDEBUG disables assert().
CXX_FLAGS_BUILD_WIN_COMMON = $(CFLAGS) -DNDEBUG -DMATTER_HAVE_SCRIPT_HOST -DMATTER_VULKAN_VIEWER -DMATTER_VULKAN_ONLY $(WIN_INCLUDE_PATHS) -I$(BOX3D_DIR)/include $(QJS_INC) $(AR_CORE_CFLAGS) -ffunction-sections -fdata-sections -MMD -MP
CXX_FLAGS_BUILD_WIN_COMMON += -DMATTER_HAVE_STREAMLINE=$(HAVE_STREAMLINE)
ifeq ($(HAVE_STREAMLINE),1)
  CXX_FLAGS_BUILD_WIN_COMMON += -I"$(STREAMLINE_PATH)/include"
endif

# Windows object set: full direct-source compile.
WIN_ALL_CPP_SRC = $(APP_SRC) $(WIN_ME3_CPP) $(WIN_MSL_CPP) $(IMGUI_SRC_WIN) $(PROFILE_DIR)/src/profile.cpp
CPP_NAMES_WIN_ALL = $(notdir $(WIN_ALL_CPP_SRC))
QJS_NAMES = $(notdir $(QJS_C))
PIPC_NAMES_WIN = $(notdir $(WIN_PIPELINE_C))
FLECS_NAME = $(notdir $(FLECS_C))

vpath %.cpp src $(PROFILE_DIR)/src $(ME3_DIR)/src $(ME3_DIR)/src/animation $(ME3_DIR)/src/render $(ME3_DIR)/src/provider $(ME3_DIR)/src/ecs \
             $(ME3_DIR)/src/streaming $(ME3_DIR)/src/script $(ME3_DIR)/src/event \
             $(ME3_DIR)/src/scene $(ME3_DIR)/src/util $(ME3_DIR)/src/props \
             $(MSL_DIR)/src $(SQL_DIR)/src $(IMGUI_PATH) $(IMGUI_PATH)/backends \
             $(IMGUIZMO_PATH) \
             ../libs/ParticleFlowLib/src \
             ../libs/MeshChartingLib/src \
             $(BC7ENC_DIR)
vpath %.c $(MSL_DIR)/src $(SQL_DIR)/src $(MEMLIB_DIR)/src $(QJS_DIR) $(FLECS_DIR)

# `windows` is the only build target until Phase 5b adds a Linux Vulkan one
# (see the Linux-viewer comment near APP_SRC above).
.DEFAULT_GOAL := windows

# L_DIR is kept as a bare path constant: `dist`'s non-MSYS2 branch (below)
# still names it as where a Linux binary would land. There is no rule that
# builds anything into it right now -- see Phase 5b note above.
L_DIR = build/linux

# Windows object sets: full direct-source compile including engine+MSL.
# Objects/.d files live under an obj/ subdir so build/windows/ only ever holds
# editor.exe (+ the non-object build artifacts below).
W_DIR = build/windows
W_OBJ_DIR = $(W_DIR)/obj
W_CPP_OBJ  = $(addprefix $(W_OBJ_DIR)/,$(CPP_NAMES_WIN_ALL:.cpp=.o))
W_QJS_OBJ  = $(addprefix $(W_OBJ_DIR)/,$(QJS_NAMES:.c=.o))
W_PIPC_OBJ = $(addprefix $(W_OBJ_DIR)/,$(PIPC_NAMES_WIN:.c=.o))
W_FLECS_OBJ = $(W_OBJ_DIR)/$(FLECS_NAME:.c=.o)
# Vendored BC encoders as PREBUILT objects (they are already in $(W_CPP_OBJ)
# via WIN_ME3_CPP; this name exists so the vulkan_smoke_tests.exe recipe --
# which compiles its whole source list in one -Wall -Wextra -Werror g++ call
# -- can LINK them instead of compiling them. Unmodified upstream sources do
# not survive -Werror -Wextra, and patching them would break the "vendored
# verbatim" contract in third_party/bc7enc/README.md.)
W_BC7ENC_OBJ = $(W_OBJ_DIR)/bc7enc.o $(W_OBJ_DIR)/rgbcx.o
W_ALL_OBJ  = $(W_CPP_OBJ) $(W_QJS_OBJ) $(W_PIPC_OBJ) $(W_FLECS_OBJ)

$(W_DIR) $(W_OBJ_DIR):
	mkdir -p $@

# ---- Library prerequisite: build libmatter_engine3.a. Only the (retired)
# Linux target ever linked this archive directly -- the Windows target
# recompiles ME3/MSL sources itself (WIN_ME3_CPP/WIN_MSL_CPP above) -- but
# build-engine-lib is kept as the one place that knows how to invoke the
# library sub-make with the right autoremesher flags, for whatever Phase 5b's
# Linux Vulkan target ends up needing.
# When retopo is enabled, pass the flag + AR include path so that
# modifier_apply.cpp and mesh_retopo.cpp inside the library see the right
# defines. EXTRA_RETOPO_CPP tells the library Makefile to also compile
# mesh_retopo.cpp.
# The library Makefile stamps its EXTRA_CFLAGS (.lib_flags) and self-forces a
# full recompile only when the flag set changes, so this stays incremental.
.PHONY: build-engine-lib
build-engine-lib:
ifneq ($(HAVE_AUTOREMESHER),)
	$(MAKE) -C $(ME3_DIR) \
	    EXTRA_CFLAGS="$(AR_CORE_CFLAGS)" \
	    EXTRA_INCLUDE="-I$(AR_CORE_PATH)/include" \
	    EXTRA_RETOPO_CPP="$(MSL_DIR)/src/mesh_retopo.cpp"
else
	$(MAKE) -C $(ME3_DIR)
endif

# The editor links the engine archive, so test the actual archive boundary
# rather than only direct-compiled adapter sources: a real clip must compile
# to serialized Ozz bytes through the archive, which proves the offline
# builders are present and reachable.
ANIMATION_ARCHIVE_LINK_TARGET = build/test_animation_bake_archive_link
$(ANIMATION_ARCHIVE_LINK_TARGET): tests/test_animation_bake_archive_link.cpp build-engine-lib
	@mkdir -p $(dir $@)
	$(CC) $< $(ME3_DIR)/build/libmatter_engine3.a -o $@ \
	    $(CXX_FLAGS_BUILD_COMMON) $(LDFLAGS) $(LDLIBS) $(AR_CORE_LDFLAGS)

.PHONY: check-animation-archive-link
check-animation-archive-link: $(ANIMATION_ARCHIVE_LINK_TARGET)
	./$(ANIMATION_ARCHIVE_LINK_TARGET)

# Generate embedded shader header before compiling any TU that includes it.
# Needed by the Windows direct-source build (shader_source.cpp).
.PHONY: embedded-shaders
embedded-shaders:
	$(MAKE) -C $(ME3_DIR) shaders_gen/embedded_shaders.h

# Shader path resolves relative to the binary; symlink MSL's shaders/ dir.
# The viewer loads the include-flattened raytrace_tlas_blas_processed.fs, so it
# MUST be regenerated from MSL's .glsl sources first — a stale processed file
# silently mismatches the C++ TLAS texture layout (tiled reads vs single-row).
shaders: regen-processed-shader
	@[ -e shaders ] || ln -s $(MSL_DIR)/shaders shaders
	@missing=""; \
	 for f in shaders/raster.vs shaders/raster.fs shaders/raytrace_tlas_blas_processed.fs; do \
	     [ -e "$$f" ] || missing="$$missing $$f"; \
	 done; \
	 if [ -n "$$missing" ]; then \
	     echo "ERROR: required shader files missing:$$missing"; \
	     exit 1; \
	 fi

# Rebuild MSL's shader preprocessor and re-flatten the processed fragment shader
# so edits to bvh_tlas_common.glsl / materials.glsl / lighting.glsl take effect.
MSL_SHADERS = $(MSL_DIR)/shaders
regen-processed-shader:
	@g++ -O2 -std=c++14 -o /tmp/me3_shader_preprocessor \
	    $(MSL_DIR)/src/shader_preprocessor.cpp
	@/tmp/me3_shader_preprocessor $(MSL_SHADERS)/raytrace_tlas_blas.fs \
	    $(MSL_SHADERS)/raytrace_tlas_blas_processed.fs

# ---- MinGW cross-compile of box3d (Linux → Windows) ----
# box3d/src/timer.c includes <Windows.h> (capital W) which is case-sensitive on
# Linux. We create a local wrapper header and compile timer.c separately.
# Run `make mingw-box3d` once; the resulting build-mingw/libbox3d.a is committed.
MINGW_CC      = x86_64-w64-mingw32-gcc
MINGW_CASE_FIX_DIR = /tmp/box3d_mingw_case_fix

mingw-box3d:
	mkdir -p $(MINGW_CASE_FIX_DIR)
	echo '#include <windows.h>' > $(MINGW_CASE_FIX_DIR)/Windows.h
	mkdir -p $(BOX3D_DIR)/build-mingw
	cd $(BOX3D_DIR) && $(MINGW_CC) -std=c17 -O2 -ffp-contract=off -Iinclude -Isrc -Wall -Wno-unused-parameter -Wno-unused-value -c src/*.c
	cd $(BOX3D_DIR) && $(MINGW_CC) -std=c17 -O2 -ffp-contract=off -Iinclude -Isrc -I$(MINGW_CASE_FIX_DIR) -c src/timer.c -o timer_win.o
	cd $(BOX3D_DIR) && ar rcs build-mingw/libbox3d.a *.o timer_win.o && rm -f *.o timer_win.o

# ---- Native Windows .exe (MinGW cross-compile from WSL/Linux) ----
# Mirrors MatterSurfaceLib's windows-native target: MinGW toolchain, the
# prebuilt windows-native raylib, and Win32 GL/GDI libs, statically linked so
# the .exe is self-contained.
WIN_CXX     ?= /ucrt64/bin/g++
WIN_CC      = x86_64-w64-mingw32-gcc
WIN_AR      ?= /ucrt64/bin/ar
GLSLC ?= /ucrt64/bin/glslc
VULKAN_INCLUDE ?= /ucrt64/include
VULKAN_LIB_DIR ?= /ucrt64/lib
VULKAN_RUNTIME_DIR ?= $(shell cygpath -m /ucrt64/bin)
WIN_FEATURES = build/windows/build_features.txt
# Win32 import libs the Windows/Vulkan build depends on (box3d + pthread).
# There is no libraylib.a on this link line -- the Vulkan build does not link
# raylib at all (see the module header comment); this list only needs to
# precede $(WIN_SYSTEM_LIBS) below on the link line.
WIN_LIBS    = $(WIN_BOX3D_LIB) $(OZZ_OFFLINE_LIBS) -lwinpthread
WIN_SYSTEM_LIBS = -lgdi32 -lwinmm -luser32 -lshell32 -lws2_32 -ldbghelp

.PHONY: vulkan-preflight
vulkan-preflight:
	@WIN_CXX="$(WIN_CXX)" VULKAN_INCLUDE="$(VULKAN_INCLUDE)" \
	 VULKAN_LIB_DIR="$(VULKAN_LIB_DIR)" GLSLC="$(GLSLC)" \
	 HAVE_STREAMLINE="$(HAVE_STREAMLINE)" STREAMLINE_PATH="$(STREAMLINE_PATH)" \
	 STREAMLINE_DLL_DIR="$(STREAMLINE_DLL_DIR)" sh tools/check_vulkan_toolchain.sh

$(WIN_FEATURES): vulkan-preflight
	@mkdir -p $(dir $@)
	@printf 'VULKAN=1\nOPENGL=0\nSTREAMLINE=$(HAVE_STREAMLINE)\n' > $@

# Standalone GLFW archive for Vulkan smoke tests.  Keep this separate from
# raylib's GLFW objects so the existing OpenGL viewer path is unchanged.
GLFW_SRC_DIR = ../third_party/raylib/src/external/glfw/src
GLFW_WIN_SRC = context.c egl_context.c init.c input.c monitor.c null_init.c \
               null_joystick.c null_monitor.c null_window.c osmesa_context.c \
               platform.c vulkan.c win32_init.c win32_joystick.c \
               win32_module.c win32_monitor.c win32_thread.c win32_time.c \
               win32_window.c window.c
# Kept under obj/glfw/ alongside every other Windows object, consistent with
# the rest of the obj/ split above.
GLFW_WIN_OBJ = $(addprefix $(W_OBJ_DIR)/glfw/,$(GLFW_WIN_SRC:.c=.o))
GLFW_WIN_OBJ += $(W_OBJ_DIR)/glfw/glfw_vulkan_only_context.o
GLFW_WIN_LIB = build/windows/libglfw3.a

$(W_OBJ_DIR)/glfw/%.o: $(GLFW_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -c $< -o $@ -D_GLFW_WIN32 \
	    -I../third_party/raylib/src/external/glfw/include

$(W_OBJ_DIR)/glfw/glfw_vulkan_only_context.o: src/glfw_vulkan_only_context.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -c $< -o $@ -D_GLFW_WIN32 \
	    -I$(GLFW_SRC_DIR) -I../third_party/raylib/src/external/glfw/include

$(GLFW_WIN_LIB): $(GLFW_WIN_OBJ)
	$(WIN_AR) rcs $@ $^

VK_SPV = build/shaders_vk/transform_probe.comp.spv \
         build/shaders_vk/atmosphere_transmittance.comp.spv \
         build/shaders_vk/atmosphere_multiscatter.comp.spv \
         build/shaders_vk/atmosphere_sky_view.comp.spv \
         build/shaders_vk/atmosphere_irradiance.comp.spv \
         build/shaders_vk/animation_skin.comp.spv \
         build/shaders_vk/cull.comp.spv \
         build/shaders_vk/raster.vert.spv \
         build/shaders_vk/raster_skin.vert.spv \
         build/shaders_vk/gbuffer.frag.spv \
         build/shaders_vk/composite.vert.spv \
         build/shaders_vk/composite.frag.spv \
         build/shaders_vk/display_transform.frag.spv \
         build/shaders_vk/rt_shadow.rgen.spv \
         build/shaders_vk/rt_shadow.rmiss.spv \
         build/shaders_vk/rt_shadow.rchit.spv \
         build/shaders_vk/rt_visibility.rmiss.spv \
         build/shaders_vk/rt_visibility.rchit.spv \
         build/shaders_vk/rt_visibility.rahit.spv \
         build/shaders_vk/rt_radiance.rmiss.spv \
         build/shaders_vk/rt_lighting.rgen.spv \
         build/shaders_vk/gi_temporal.comp.spv \
         build/shaders_vk/gi_atrous.comp.spv \
         build/shaders_vk/rt_surface.rchit.spv \
         build/shaders_vk/rt_surface.rahit.spv \
         build/shaders_vk/rt_surface_test.rgen.spv \
         build/shaders_vk/vol_density.comp.spv \
         build/shaders_vk/vol_scatter.comp.spv \
         build/shaders_vk/vol_integrate.comp.spv \
         build/shaders_vk/tileset_bake_primary.comp.spv \
         build/shaders_vk/tileset_bake_ao.comp.spv \
         build/shaders_vk/vt_composite.comp.spv \
         build/shaders_vk/vt_bc_encode.comp.spv \
         build/shaders_vk/vt_enrich_ao.comp.spv
VK_EMBEDDED_HEADER = $(ME3_DIR)/shaders_gen/embedded_spirv.h

build/shaders_vk/%.spv: $(ME3_DIR)/shaders_vk/%
	@mkdir -p $(dir $@)
	$(GLSLC) --target-env=vulkan1.3 -O -I$(ME3_DIR)/shaders_vk $< -o $@

build/shaders_vk/raster.vert.spv build/shaders_vk/gbuffer.frag.spv: \
    $(ME3_DIR)/shaders_vk/material_common.glsl

build/shaders_vk/atmosphere_transmittance.comp.spv \
    build/shaders_vk/atmosphere_multiscatter.comp.spv \
    build/shaders_vk/atmosphere_sky_view.comp.spv \
    build/shaders_vk/atmosphere_irradiance.comp.spv: \
    $(ME3_DIR)/shaders_vk/atmosphere_common.glsl

# C2 uses the same source and descriptor ABI as raster.vert; a compile-time
# specialization adds location 5 for the previous skinned position.
build/shaders_vk/raster_skin.vert.spv: $(ME3_DIR)/shaders_vk/raster.vert \
    $(ME3_DIR)/shaders_vk/material_common.glsl
	@mkdir -p $(dir $@)
	$(GLSLC) --target-env=vulkan1.3 -O -I$(ME3_DIR)/shaders_vk \
	    -DMATTER_SKINNED_VERTEX_INPUT $< -o $@

build/shaders_vk/gbuffer.frag.spv: $(ME3_DIR)/shaders_vk/tileset_common.glsl

build/shaders_vk/rt_surface.rchit.spv build/shaders_vk/rt_surface.rahit.spv \
    build/shaders_vk/rt_visibility.rahit.spv \
    build/shaders_vk/rt_radiance.rmiss.spv \
    build/shaders_vk/rt_lighting.rgen.spv \
    build/shaders_vk/rt_surface_test.rgen.spv: \
    $(ME3_DIR)/shaders_vk/rt_surface_common.glsl \
    $(ME3_DIR)/shaders_vk/tileset_common.glsl

build/shaders_vk/composite.frag.spv build/shaders_vk/rt_lighting.rgen.spv: \
    $(ME3_DIR)/shaders_vk/sky_common.glsl

build/shaders_vk/gi_temporal.comp.spv build/shaders_vk/gi_atrous.comp.spv: \
    $(ME3_DIR)/shaders_vk/gi_common.glsl

# WP-D (chart VT): tileset_common.glsl includes wang_common.glsl; every
# tileset_common consumer depends on it, and vt_composite includes it directly.
build/shaders_vk/gbuffer.frag.spv build/shaders_vk/rt_shadow.rgen.spv \
    build/shaders_vk/rt_surface.rchit.spv \
    build/shaders_vk/rt_surface.rahit.spv \
    build/shaders_vk/rt_visibility.rahit.spv \
    build/shaders_vk/rt_radiance.rmiss.spv \
    build/shaders_vk/rt_lighting.rgen.spv \
    build/shaders_vk/rt_surface_test.rgen.spv \
    build/shaders_vk/vt_composite.comp.spv: \
    $(ME3_DIR)/shaders_vk/wang_common.glsl

# WP-H (chart VT): the shared chart-resolve includes the page passes consume.
build/shaders_vk/vt_composite.comp.spv \
    build/shaders_vk/vt_enrich_ao.comp.spv: \
    $(ME3_DIR)/shaders_vk/vt_chart_types.glsl \
    $(ME3_DIR)/shaders_vk/vt_chart_resolve.glsl

# P2 (texel-rate tape): the GLSL noise twin + tape interpreter include.
build/shaders_vk/vt_composite.comp.spv: \
    $(ME3_DIR)/shaders_vk/vt_surface_tape.glsl

# WP-E (chart VT): vt_common.glsl is the shared VT sampling helper (an include,
# not a stage), so it has no .spv -- but its consumers must rebuild when it
# changes. WP-G added the RT stages: rt_surface_common.glsl now includes
# vt_common.glsl, so every shader that includes IT is a consumer too.
build/shaders_vk/gbuffer.frag.spv build/shaders_vk/rt_surface.rchit.spv \
    build/shaders_vk/rt_surface.rahit.spv \
    build/shaders_vk/rt_visibility.rahit.spv \
    build/shaders_vk/rt_radiance.rmiss.spv \
    build/shaders_vk/rt_lighting.rgen.spv \
    build/shaders_vk/rt_surface_test.rgen.spv: \
    $(ME3_DIR)/shaders_vk/vt_common.glsl

$(VK_EMBEDDED_HEADER): $(VK_SPV) $(ME3_DIR)/tools/embed_spirv.py
	python3 $(ME3_DIR)/tools/embed_spirv.py $@ $(VK_SPV)

build/windows/vulkan_smoke_tests.exe: \
        $(ME3_DIR)/tests/vulkan_smoke_tests.cpp \
        src/ui_lighting_controls.cpp src/ui.h \
        $(ME3_DIR)/include/matter/vulkan_device.h \
        $(ME3_DIR)/src/render/gpu_matrix_pack.h \
        $(ME3_DIR)/src/render/vk_draw_command.h \
        $(ME3_DIR)/src/render/frame_matrices.h \
        $(ME3_DIR)/src/render/matrix_math.h \
        $(ME3_DIR)/src/render/vk_resources.h \
        $(ME3_DIR)/src/render/vk_pipeline.h \
        $(ME3_DIR)/src/render/vk_scene_renderer.h \
        $(ME3_DIR)/src/render/vk_volumetrics.h \
        $(ME3_DIR)/src/render/vk_atmosphere.h \
        $(ME3_DIR)/src/render/vk_emitter_gather.h \
        $(ME3_DIR)/src/render/vk_instance_cache.h \
        $(ME3_DIR)/src/render/vk_instance_cache.cpp \
        $(ME3_DIR)/src/render/vk_lighting_controls.h \
        $(ME3_DIR)/src/render/streamline_bridge.h \
        $(ME3_DIR)/src/render/streamline_bridge.cpp \
        $(ME3_DIR)/src/render/vk_context.cpp \
        $(ME3_DIR)/src/render/vk_resources.cpp \
        $(ME3_DIR)/src/render/vk_pipeline.cpp \
        $(ME3_DIR)/src/render/vk_scene_renderer.cpp \
        $(ME3_DIR)/src/render/vk_animation_skinning.cpp \
        $(ME3_DIR)/src/render/vk_animation_bounds.cpp \
        $(ME3_DIR)/src/animation/animation_budget.cpp \
        $(ME3_DIR)/src/render/tileset_slicer.cpp \
        $(ME3_DIR)/src/render/bc_encode.cpp \
        $(ME3_DIR)/src/render/vt_residency.cpp \
        $(ME3_DIR)/include/matter/vt_budgets.h \
        $(ME3_DIR)/src/props/props.cpp \
        $(ME3_DIR)/src/util/json_doc.cpp \
        $(ME3_DIR)/src/render/vt_stub_filler.cpp \
        $(ME3_DIR)/src/render/vt_compositor.cpp \
        $(ME3_DIR)/src/render/vt_enrich.cpp \
        $(ME3_DIR)/src/render/vt_surface_tape.h \
        $(ME3_DIR)/src/terrain_field.cpp \
        $(ME3_DIR)/tests/vulkan_smoke_tileset_bake_stub.cpp \
        $(ME3_DIR)/src/tileset_gtex.cpp \
        $(ME3_DIR)/src/render/vk_volumetrics.cpp \
        $(ME3_DIR)/src/render/vk_atmosphere.cpp \
        $(ME3_DIR)/src/render/vk_emitter_gather.cpp \
        $(ME3_DIR)/src/render/raster_mesh.cpp $(ME3_DIR)/src/render/indexed_part_geometry.cpp \
        $(ME3_DIR)/src/render/frame_matrices.cpp \
        $(ME3_DIR)/src/render/vk_temporal.cpp \
        $(ME3_DIR)/src/render/vk_gi_math.cpp \
        $(ME3_DIR)/src/render/vk_lighting_controls.cpp \
        $(ME3_DIR)/src/render/matrix_math.cpp \
        $(ME3_DIR)/src/render/lod_trace.cpp \
        $(VK_EMBEDDED_HEADER) $(GLFW_WIN_LIB) $(W_FLECS_OBJ) $(W_BC7ENC_OBJ)
	@mkdir -p $(dir $@)
	$(WIN_CXX) $(ME3_DIR)/tests/vulkan_smoke_tests.cpp src/ui_lighting_controls.cpp \
	    $(ME3_DIR)/src/render/streamline_bridge.cpp \
	    $(ME3_DIR)/src/render/vk_context.cpp \
	    $(ME3_DIR)/src/render/vk_resources.cpp \
	    $(ME3_DIR)/src/render/vk_pipeline.cpp \
	    $(ME3_DIR)/src/render/vk_scene_renderer.cpp \
	    $(ME3_DIR)/src/render/vk_animation_skinning.cpp \
	    $(ME3_DIR)/src/render/vk_animation_bounds.cpp \
	    $(ME3_DIR)/src/animation/animation_budget.cpp \
	    $(ME3_DIR)/src/render/tileset_slicer.cpp \
	    $(ME3_DIR)/src/render/bc_encode.cpp \
	    $(ME3_DIR)/src/render/vt_residency.cpp \
	    $(ME3_DIR)/src/props/props.cpp \
	    $(ME3_DIR)/src/util/json_doc.cpp \
	    $(ME3_DIR)/src/render/vt_stub_filler.cpp \
	    $(ME3_DIR)/src/render/vt_compositor.cpp \
	    $(ME3_DIR)/src/render/vt_enrich.cpp \
	    $(ME3_DIR)/src/terrain_field.cpp \
	    $(ME3_DIR)/tests/vulkan_smoke_tileset_bake_stub.cpp \
	    $(ME3_DIR)/src/tileset_gtex.cpp \
	    $(ME3_DIR)/src/render/vk_volumetrics.cpp \
	    $(ME3_DIR)/src/render/vk_atmosphere.cpp \
	    $(ME3_DIR)/src/render/vk_emitter_gather.cpp \
	    $(ME3_DIR)/src/render/raster_mesh.cpp $(ME3_DIR)/src/render/indexed_part_geometry.cpp \
	    $(ME3_DIR)/src/render/vk_instance_cache.cpp \
	    $(ME3_DIR)/src/render/frame_matrices.cpp \
	    $(ME3_DIR)/src/render/vk_temporal.cpp \
	    $(ME3_DIR)/src/render/vk_gi_math.cpp \
	    $(ME3_DIR)/src/render/vk_lighting_controls.cpp \
	    $(ME3_DIR)/src/render/matrix_math.cpp \
	    $(ME3_DIR)/src/render/lod_trace.cpp \
	    $(PROFILE_DIR)/src/profile.cpp \
	    $(GLFW_WIN_LIB) $(W_FLECS_OBJ) $(W_BC7ENC_OBJ) -o $@ \
	    -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers \
	    -ffunction-sections -Wl,--gc-sections \
	    -static \
	    -DVK_USE_PLATFORM_WIN32_KHR \
	    -DMATTER_VK_TEST_FAULT_INJECTION \
	    -DMATTER_HAVE_STREAMLINE=$(HAVE_STREAMLINE) \
	    -DMATTER_VK_TEST_LAYER_PATH='"$(VULKAN_RUNTIME_DIR)"' \
	    -I$(ME3_DIR) -I$(ME3_DIR)/include -I$(ME3_DIR)/src \
	    -I$(PROFILE_DIR)/include \
	    -I$(BC7ENC_DIR) \
	    -I../third_party/raylib/src \
	    -I../libs/MatterSurfaceLib/include \
	    -I../libs/SpatialQueryLib/include \
	    -I../libs/MemoryLib/include \
	    -I../libs/MathLib/include \
	    -I$(ME3_DIR)/tests \
	    -I../third_party/raylib/src/external/glfw/include \
	    -I$(OZZ_INCLUDE) \
	    -I$(FLECS_DIR) \
	    $(if $(filter 1,$(HAVE_STREAMLINE)),-I"$(STREAMLINE_PATH)/include") \
	    -I$(VULKAN_INCLUDE) -L$(VULKAN_LIB_DIR) -Wl,-Bdynamic -lvulkan-1 \
	    $(WIN_SYSTEM_LIBS) -Wl,-Bstatic

.PHONY: vulkan-smoke
vulkan-smoke: vulkan-preflight $(GLFW_WIN_LIB) \
              build/windows/vulkan_smoke_tests.exe \
              build/windows/vulkan_compat_tests.exe
	/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe \
	    -NoProfile -ExecutionPolicy Bypass -File \
	    tools/smoke_vulkan_faults.ps1 \
	    -TestPath build/windows/vulkan_smoke_tests.exe \
	    -TimeoutMilliseconds 30000

build/windows/vulkan_compat_tests.exe: \
        $(ME3_DIR)/tests/vulkan_compat_tests.cpp \
        $(ME3_DIR)/src/render/vulkan_only_compat.cpp
	@mkdir -p $(dir $@)
	$(WIN_CXX) $^ -o $@ -std=c++17 -Wall -Wextra -Werror -static \
	    -DMATTER_VULKAN_COMPAT_TESTING \
	    -I$(ME3_DIR)/include -I$(ME3_DIR)/src \
	    -I../third_party/raylib/src

# ---- WP-D: standalone GPU test for the tier-1 VT page compositor ----------
# Same device-bootstrap pattern as vulkan_smoke_tests.exe (hidden GLFW window
# + VulkanDevice with validation), but a deliberately minimal TU list: the
# compositor is standalone by contract, so the exe proves it links against
# nothing beyond the device layer.
build/windows/vt_compositor_tests.exe: \
        $(ME3_DIR)/tests/vt_compositor_tests.cpp \
        $(ME3_DIR)/src/render/vt_compositor.cpp \
        $(ME3_DIR)/src/render/vt_compositor.h \
        $(ME3_DIR)/src/render/vt_types.h \
        $(ME3_DIR)/src/render/vt_surface_tape.h \
        $(ME3_DIR)/src/render/chart_atlas.h \
        $(ME3_DIR)/src/terrain_field.cpp \
        $(ME3_DIR)/src/terrain_field.h \
        $(ME3_DIR)/src/render/vk_context.cpp \
        $(ME3_DIR)/src/render/vk_resources.cpp \
        $(ME3_DIR)/src/render/streamline_bridge.cpp \
        $(VK_EMBEDDED_HEADER) $(GLFW_WIN_LIB) $(W_FLECS_OBJ)
	@mkdir -p $(dir $@)
	$(WIN_CXX) $(ME3_DIR)/tests/vt_compositor_tests.cpp \
	    $(ME3_DIR)/src/render/vt_compositor.cpp \
	    $(ME3_DIR)/src/terrain_field.cpp \
	    $(ME3_DIR)/src/render/vk_context.cpp \
	    $(ME3_DIR)/src/render/vk_resources.cpp \
	    $(ME3_DIR)/src/render/streamline_bridge.cpp \
	    $(GLFW_WIN_LIB) $(W_FLECS_OBJ) -o $@ \
	    -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers \
	    -Wno-unused-parameter \
	    -ffunction-sections -Wl,--gc-sections \
	    -static \
	    -DVK_USE_PLATFORM_WIN32_KHR \
	    -DMATTER_HAVE_STREAMLINE=$(HAVE_STREAMLINE) \
	    -DMATTER_VK_TEST_LAYER_PATH='"$(VULKAN_RUNTIME_DIR)"' \
	    -I$(ME3_DIR) -I$(ME3_DIR)/include -I$(ME3_DIR)/src \
	    -I$(ME3_DIR)/src/render \
	    -I$(ME3_DIR)/tests \
	    -I$(FLECS_DIR) \
	    -I../third_party/raylib/src \
	    -I../libs/MatterSurfaceLib/include \
	    -I../libs/SpatialQueryLib/include \
	    -I../libs/MemoryLib/include \
	    -I../libs/MathLib/include \
	    -I../third_party/raylib/src/external/glfw/include \
	    $(if $(filter 1,$(HAVE_STREAMLINE)),-I"$(STREAMLINE_PATH)/include") \
	    -I$(VULKAN_INCLUDE) -L$(VULKAN_LIB_DIR) -Wl,-Bdynamic -lvulkan-1 \
	    $(WIN_SYSTEM_LIBS) -Wl,-Bstatic

.PHONY: vt-compositor-tests
vt-compositor-tests: vulkan-preflight $(GLFW_WIN_LIB) \
                     build/windows/vt_compositor_tests.exe
	cd build/windows && ./vt_compositor_tests.exe

# ---- Windows compile rules
$(W_CPP_OBJ): $(W_OBJ_DIR)/%.o: %.cpp | $(W_OBJ_DIR)
	$(WIN_CXX) -c $< -o $@ $(CXX_FLAGS_BUILD_WIN_COMMON)
$(W_QJS_OBJ): $(W_OBJ_DIR)/%.o: $(QJS_DIR)/%.c | $(W_OBJ_DIR)
	$(WIN_CC) -c $< -o $@ -O2 -DNDEBUG -DCONFIG_VERSION='"0.10.0"' $(QJS_INC) -ffunction-sections -fdata-sections -MMD -MP
# Pipeline C objects. Most come from MatterSurfaceLib, but spatial_hash and
# mem_pool are compiled from their source-of-truth libraries, so they cannot
# share the MSL static pattern rule and get their own explicit rules below.
WIN_PIPC_COMPILE = $(WIN_CC) -c $< -o $@ -O2 -DNDEBUG -DPLATFORM_DESKTOP $(WIN_INCLUDE_PATHS) -ffunction-sections -fdata-sections -MMD -MP
W_PIPC_MSL_OBJ = $(filter-out $(W_OBJ_DIR)/spatial_hash.o $(W_OBJ_DIR)/mem_pool.o,$(W_PIPC_OBJ))

$(W_PIPC_MSL_OBJ): $(W_OBJ_DIR)/%.o: $(MSL_DIR)/src/%.c | $(W_OBJ_DIR)
	$(WIN_PIPC_COMPILE)
$(W_OBJ_DIR)/spatial_hash.o: $(SQL_DIR)/src/spatial_hash.c | $(W_OBJ_DIR)
	$(WIN_PIPC_COMPILE)
$(W_OBJ_DIR)/mem_pool.o: $(MEMLIB_DIR)/src/mem_pool.c | $(W_OBJ_DIR)
	$(WIN_PIPC_COMPILE)
$(W_FLECS_OBJ): $(W_OBJ_DIR)/%.o: $(FLECS_DIR)/%.c | $(W_OBJ_DIR)
	$(WIN_CC) -c $< -o $@ -std=c17 -O2 -I$(FLECS_DIR) -DNDEBUG -ffunction-sections -fdata-sections -MMD -MP

# Order-only: the header must exist before shader_source.cpp compiles, but its
# regeneration must not force object rebuilds (embedded-shaders is phony).
$(W_CPP_OBJ): | embedded-shaders

# MATTER_HAVE_STREAMLINE is a compile-time gate, but make only tracks source
# timestamps, so flipping HAVE_STREAMLINE between invocations used to reuse the
# stale streamline_bridge.o and silently ship the wrong branch. The stamp below
# records the last-built value and is rewritten (bumping its mtime) only when
# the value differs, forcing exactly the TU that reads the define to recompile.
W_STREAMLINE_STAMP = $(W_OBJ_DIR)/.streamline_flag
.PHONY: FORCE
FORCE:
$(W_STREAMLINE_STAMP): FORCE | $(W_OBJ_DIR)
	@[ "$$(cat $@ 2>/dev/null)" = "$(HAVE_STREAMLINE)" ] || echo "$(HAVE_STREAMLINE)" > $@
$(W_OBJ_DIR)/streamline_bridge.o: $(W_STREAMLINE_STAMP)

windows: $(WIN_FEATURES) $(VK_EMBEDDED_HEADER) $(GLFW_WIN_LIB) $(AR_CORE_DEP) $(W_ALL_OBJ)
	$(WIN_CXX) $(W_ALL_OBJ) -o $(W_DIR)/editor.exe $(CFLAGS) -static -s -Wl,--gc-sections $(WIN_LIBS) $(GLFW_WIN_LIB) $(AR_CORE_LDFLAGS) $(WIN_SYSTEM_LIBS) -L$(VULKAN_LIB_DIR) -Wl,-Bdynamic -lvulkan-1 -Wl,-Bstatic
	@if [ "$(HAVE_STREAMLINE)" = "1" ]; then cp -f "$(STREAMLINE_DLL_DIR)"/*.dll $(W_DIR)/; fi
	@imports="$$($(WIN_CXX) -print-prog-name=objdump -p $(W_DIR)/editor.exe)"; \
	 for forbidden in opengl32 ChoosePixelFormat SetPixelFormat SwapBuffers cuGraphicsGL nvcuda cuDevice; do \
	   echo "$$imports" | grep -qi "$$forbidden" && { echo "ERROR: forbidden PE import/symbol $$forbidden"; exit 1; } || true; \
	 done

.PHONY: vulkan-viewer-gate
vulkan-viewer-gate:
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/check_vulkan_viewer.ps1

.PHONY: vulkan-instancing-perf
vulkan-instancing-perf: windows
	VK_LAYER_PATH="$(VULKAN_RUNTIME_DIR)" PATH="$(VULKAN_RUNTIME_DIR);$$PATH" \
	    powershell.exe -NoProfile -ExecutionPolicy Bypass -File \
	    tools/perf_vulkan_instancing.ps1

# Windows .d files contain D:/-style paths that break GNU make on Linux/WSL
# ("multiple target patterns"), so only include them under MSYS2.
ifdef MSYSTEM
-include $(W_ALL_OBJ:.o=.d)
endif

# win-shaders: removed — all runtime shaders are now embedded at build time via
# shaders_gen/embedded_shaders.h (Task 9). A Windows .exe no longer needs shader
# files on disk; the embedded fallback in shader_text() covers every load.
# The old `rm -rf shaders && cp -r` recipe also destroyed the Linux symlink, so
# removing it keeps the shaders/ symlink intact after a cross-compile.

# win-shaders-gpu lived here and re-created a MatterEditor/shaders_gpu symlink
# into MatterEngine3/shaders_gpu/. Both are gone: that directory held only GL
# sources for the deleted GpuCuller/raster-composer path. Vulkan shaders live in
# MatterEngine3/shaders_vk/ and reach the binary as embedded SPIR-V
# ($(VK_EMBEDDED_HEADER)), so no symlink is needed to resolve them at runtime.

# =============================================================================
# ---- Linux Vulkan target (Phase 5b) ----------------------------------------
# =============================================================================
# EVERYTHING IN THIS SECTION IS UNVERIFIED. It was written on Windows with no
# Linux machine, toolchain, Vulkan loader, or X11/GLFW dev headers available
# to compile or link against -- see MatterEditor/README.md for the full list
# of what is/isn't checked. It is deliberately isolated from the `windows`
# target: separate variables (LINUX_*, L_*), a separate object directory
# (build/linux/obj/, vs build/windows/obj/), and a separate link line. Nothing
# in this section is referenced by any `windows`-target rule above, and
# nothing above this point was changed to make room for it.
#
# Shape: identical TU list to the Windows target (APP_SRC + WIN_ME3_CPP +
# WIN_MSL_CPP + WIN_PIPELINE_C + QJS_C + FLECS_C + IMGUI_SRC_WIN), direct-
# source compiled exactly like Windows does -- NOT linked against
# libmatter_engine3.a the way the old (Phase-5a-deleted) GL Linux target was.
# That old shape doesn't fit any more: MatterEngine3/Makefile's own ME3_CPP
# list does not compile the vk_*.cpp render sources at all (see the comment
# above CFLAGS in that Makefile) -- they only exist as direct-compile lists
# here, in WIN_ME3_CPP. The "WIN_" prefix on those shared variables is
# historical, not a statement that the sources are Windows-specific; per the
# plan (docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md
# Phase 5 table) only vk_context.cpp has meaningful platform gating, and this
# landing fixed the one spot in it that would not even compile on Linux (see
# the Phase 5b comments in MatterEngine3/src/render/vk_context.cpp).
#
# Deliberately X11-only, not Wayland: matches the dependency set the old
# (Phase-5a-deleted) GL Linux target already had (LDFLAGS there was
# -lGL -lm -lpthread -ldl -lrt -lX11, no Wayland libs), keeps this port
# bounded, and avoids a wayland-scanner/wayland-protocols build-tool
# dependency that would add more unverifiable surface. GLFW 3.4 supports
# building both backends with runtime auto-selection (platform.c), so adding
# _GLFW_WAYLAND later is additive, not a rewrite -- flagged as a known gap in
# the README rather than attempted here.
LINUX_CXX ?= g++
LINUX_CC  ?= gcc
LINUX_AR  ?= ar

# HAVE_STREAMLINE is deliberately NOT threaded through to this target the way
# it is for `windows` (that one reads the shared HAVE_STREAMLINE/STREAMLINE_PATH/
# STREAMLINE_DLL_DIR variables). Streamline is a Windows-only SDK -- DLSS-G
# needs to hook a swapchain that only exists via Win32/DXGI -- so MATTER_HAVE_
# STREAMLINE=0 is hardcoded below and this target never reads STREAMLINE_PATH
# or STREAMLINE_DLL_DIR at all. That makes "accidentally build Linux with
# Streamline on" structurally unreachable through this target's own flags,
# rather than relying on a caller remembering to pass HAVE_STREAMLINE=0.
# (streamline_bridge.cpp's own #if MATTER_HAVE_STREAMLINE guard around
# `#include <windows.h>` is the belt to this suspenders -- see that file.)
#
# VULKAN_INCLUDE / VULKAN_LIB_DIR / GLSLC are the SAME shared variables the
# `windows` target uses (default /ucrt64/... paths, defined near WIN_CXX
# above) -- deliberately NOT given fresh Linux-flavored defaults here, because
# any guessed default (/usr/include vs /usr/include/vulkan, distro-dependent
# multiarch lib dirs, glslc vs glslc-14, ...) would be silently wrong on some
# distros and there is no way to verify a guess on this machine. A real Linux
# build MUST override them explicitly, e.g.:
#   make -C MatterEditor linux \
#        VULKAN_INCLUDE=/usr/include VULKAN_LIB_DIR=/usr/lib/x86_64-linux-gnu \
#        GLSLC=/usr/bin/glslc
# Leaving the ucrt64 defaults in place when this reuses $(WIN_INCLUDE_PATHS)
# below is harmless either way: gcc/g++ silently ignore a -I directory that
# does not exist, so an un-overridden /ucrt64/include on a real Linux box just
# means the vendored ../third_party/Vulkan-Headers/include copy (also already
# in $(WIN_INCLUDE_PATHS), further down the search order) wins instead.
CXX_FLAGS_BUILD_LINUX_COMMON = $(CFLAGS) -DNDEBUG -DMATTER_HAVE_SCRIPT_HOST \
    -DMATTER_VULKAN_VIEWER -DMATTER_VULKAN_ONLY -DMATTER_HAVE_STREAMLINE=0 \
    $(WIN_INCLUDE_PATHS) -I$(BOX3D_DIR)/include $(QJS_INC) $(AR_CORE_CFLAGS) \
    -ffunction-sections -fdata-sections -MMD -MP

# Reusing $(WIN_ALL_CPP_SRC) verbatim rather than re-listing APP_SRC/WIN_ME3_CPP/
# WIN_MSL_CPP/IMGUI_SRC_WIN a second time: it is already exactly the TU set
# this target needs (see the module-header comment above), and referencing it
# here cannot change what the `windows` target does with it.
LINUX_ALL_CPP_SRC = $(WIN_ALL_CPP_SRC)
CPP_NAMES_LINUX_ALL = $(notdir $(LINUX_ALL_CPP_SRC))

L_OBJ_DIR = $(L_DIR)/obj
L_CPP_OBJ   = $(addprefix $(L_OBJ_DIR)/,$(CPP_NAMES_LINUX_ALL:.cpp=.o))
L_QJS_OBJ   = $(addprefix $(L_OBJ_DIR)/,$(QJS_NAMES:.c=.o))
L_PIPC_OBJ  = $(addprefix $(L_OBJ_DIR)/,$(PIPC_NAMES_WIN:.c=.o))
L_FLECS_OBJ = $(L_OBJ_DIR)/$(FLECS_NAME:.c=.o)
L_ALL_OBJ   = $(L_CPP_OBJ) $(L_QJS_OBJ) $(L_PIPC_OBJ) $(L_FLECS_OBJ)

$(L_DIR) $(L_OBJ_DIR):
	mkdir -p $@

# ---- Vulkan-only GLFW for X11 --------------------------------------------
# Mirrors GLFW_WIN_SRC's shape: the common/null/egl/osmesa files plus the X11
# backend instead of win32, minus glx_context.c (the real GLX/libGL
# implementation -- excluded on purpose, same reasoning as wgl_context.c being
# excluded from GLFW_WIN_SRC) plus the fail-closed stub replacement for it
# (src/glfw_vulkan_only_context_x11.c, mirroring src/glfw_vulkan_only_context.c).
#
# posix_module.c / posix_poll.c / posix_thread.c / posix_time.c and
# linux_joystick.c are the non-Windows counterparts to win32_module.c/
# win32_thread.c/win32_time.c that GLFW_WIN_SRC lists -- confirmed by reading
# third_party/raylib/src/external/glfw/src/platform.h's GLFW_BUILD_POSIX_MODULE
# / GLFW_BUILD_POSIX_POLL / linux_joystick.h #include guards (_GLFW_X11 or
# _GLFW_WAYLAND, combined with __linux__, pulls in linux_joystick.h).
#
# UNVERIFIED: which X11/Vulkan dev packages a given distro needs
# (libx11-dev/libxrandr-dev/libxinerama-dev/libxcursor-dev/libxi-dev/
# libxext-dev cover the headers GLFW's own CMakeLists.txt checks for when
# building the X11 backend; GLFW dlopen()s libXrandr/libXinerama/libXcursor/
# libXi/libX11 itself at runtime rather than link-time per x11_init.c, but
# -lX11 below is still required because x11_window.c calls Xlib functions
# like XCreateWindow directly, not through a dlopen'd pointer) -- there is no
# Linux package manager here to confirm any of this against a real install.
GLFW_LINUX_SRC = context.c egl_context.c init.c input.c monitor.c null_init.c \
                 null_joystick.c null_monitor.c null_window.c osmesa_context.c \
                 platform.c vulkan.c window.c \
                 x11_init.c x11_monitor.c x11_window.c xkb_unicode.c \
                 linux_joystick.c \
                 posix_module.c posix_poll.c posix_thread.c posix_time.c
GLFW_LINUX_OBJ = $(addprefix $(L_OBJ_DIR)/glfw/,$(GLFW_LINUX_SRC:.c=.o))
GLFW_LINUX_OBJ += $(L_OBJ_DIR)/glfw/glfw_vulkan_only_context_x11.o
GLFW_LINUX_LIB = $(L_DIR)/libglfw3.a

$(L_OBJ_DIR)/glfw/%.o: $(GLFW_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(LINUX_CC) -c $< -o $@ -D_GLFW_X11 \
	    -I../third_party/raylib/src/external/glfw/include

$(L_OBJ_DIR)/glfw/glfw_vulkan_only_context_x11.o: src/glfw_vulkan_only_context_x11.c
	@mkdir -p $(dir $@)
	$(LINUX_CC) -c $< -o $@ -D_GLFW_X11 \
	    -I$(GLFW_SRC_DIR) -I../third_party/raylib/src/external/glfw/include

$(GLFW_LINUX_LIB): $(GLFW_LINUX_OBJ)
	$(LINUX_AR) rcs $@ $^

# ---- Linux compile rules (mirrors the Windows "compile rules" section) ---
$(L_CPP_OBJ): $(L_OBJ_DIR)/%.o: %.cpp | $(L_OBJ_DIR)
	$(LINUX_CXX) -c $< -o $@ $(CXX_FLAGS_BUILD_LINUX_COMMON)
$(L_QJS_OBJ): $(L_OBJ_DIR)/%.o: $(QJS_DIR)/%.c | $(L_OBJ_DIR)
	$(LINUX_CC) -c $< -o $@ -O2 -DNDEBUG -DCONFIG_VERSION='"0.10.0"' $(QJS_INC) -ffunction-sections -fdata-sections -MMD -MP
LINUX_PIPC_COMPILE = $(LINUX_CC) -c $< -o $@ -O2 -DNDEBUG -DPLATFORM_DESKTOP $(WIN_INCLUDE_PATHS) -ffunction-sections -fdata-sections -MMD -MP
L_PIPC_MSL_OBJ = $(filter-out $(L_OBJ_DIR)/spatial_hash.o $(L_OBJ_DIR)/mem_pool.o,$(L_PIPC_OBJ))
$(L_PIPC_MSL_OBJ): $(L_OBJ_DIR)/%.o: $(MSL_DIR)/src/%.c | $(L_OBJ_DIR)
	$(LINUX_PIPC_COMPILE)
$(L_OBJ_DIR)/spatial_hash.o: $(SQL_DIR)/src/spatial_hash.c | $(L_OBJ_DIR)
	$(LINUX_PIPC_COMPILE)
$(L_OBJ_DIR)/mem_pool.o: $(MEMLIB_DIR)/src/mem_pool.c | $(L_OBJ_DIR)
	$(LINUX_PIPC_COMPILE)
$(L_FLECS_OBJ): $(L_OBJ_DIR)/%.o: $(FLECS_DIR)/%.c | $(L_OBJ_DIR)
	$(LINUX_CC) -c $< -o $@ -std=c17 -O2 -I$(FLECS_DIR) -DNDEBUG -ffunction-sections -fdata-sections -MMD -MP

# Order-only, same reasoning as the Windows equivalent: the embedded-shaders
# header must exist before shader_source.cpp compiles, but its regeneration
# (a phony target) must not force object rebuilds every invocation.
$(L_CPP_OBJ): | embedded-shaders

# linux-shaders-gpu was the Linux twin of win-shaders-gpu and went with it; see
# the note there.

# box3d: unlike WIN_BOX3D_LIB (a committed mingw cross-build with no build
# rule of its own -- see mingw-box3d above), $(BOX3D_LIB) already has a real
# rule ($(MAKE) -C $(BOX3D_DIR), defined above near WIN_LIBS) and a committed
# third_party/box3d/libbox3d.a exists in the repo. UNVERIFIED whether that
# committed archive is ABI-compatible with whatever toolchain a real Linux
# build uses; if not, `make -C ../third_party/box3d` (the existing rule this
# target depends on) rebuilds it natively.
#
# Link libs: -lX11 for the GLFW X11 backend (see GLFW_LINUX_SRC comment
# above); -ldl for GLFW's runtime dlopen() of libXrandr/libXinerama/libXcursor/
# libXi/libX11 itself; -lpthread for box3d/QuickJS threads (mirrors WIN_LIBS'
# -lwinpthread); -lm and -lrt match the old (Phase-5a-deleted) GL Linux
# target's LDFLAGS verbatim (GLFW's own CMakeLists.txt links both on
# UNIX-and-not-APPLE, see third_party/raylib/src/external/glfw/src/CMakeLists.txt).
# No raylib anywhere on this line, same as `windows` -- confirmed by grepping
# this target's full source list the same way the module header's
# raylib-dependency analysis did for Windows.
LINUX_SYSTEM_LIBS = -lX11 -ldl -lpthread -lm -lrt

# Not a hard requirement of `linux` (VK_EMBEDDED_HEADER/VK_SPV are listed as a
# prerequisite below anyway), but documented since it trips up a first build:
# $(VK_EMBEDDED_HEADER)'s recipe (defined above, shared with `windows`) invokes
# $(GLSLC) -- override that too (see the VULKAN_INCLUDE/VULKAN_LIB_DIR/GLSLC
# note earlier in this section) if it is not already an X11/Linux glslc.
linux: $(VK_EMBEDDED_HEADER) $(GLFW_LINUX_LIB) $(BOX3D_LIB) $(AR_CORE_DEP) $(L_ALL_OBJ)
	$(LINUX_CXX) $(L_ALL_OBJ) -o $(L_DIR)/editor $(CFLAGS) -Wl,--gc-sections \
	    $(BOX3D_LIB) $(GLFW_LINUX_LIB) $(AR_CORE_LDFLAGS) $(LINUX_SYSTEM_LIBS) \
	    -L$(VULKAN_LIB_DIR) -lvulkan

# Linux .d files use plain Unix paths, not the D:/-style paths that make the
# Windows .d include conditional on MSYSTEM above -- safe to include
# unconditionally. $(L_ALL_OBJ) is only defined by this point in the file (the
# Linux section above), which is why this line lives here and not next to the
# Windows one. -include silently skips files that do not exist yet (e.g. on
# Windows, where nothing ever builds build/linux/obj/*.d), so this is a no-op
# there, not an error.
-include $(L_ALL_OBJ:.o=.d)

# clean: remove build artifacts and generated binaries.
# shaders/ is a committed git symlink — do NOT remove it here,
# or `git status` will show them as deleted. The build recreates them via the
# shaders rule anyway.
clean:
	rm -rf build

# ---- dist: self-contained, zippable folder ----
# Shaders (both GL and Vulkan) are embedded into the executable at build time
# (see shader_text() in MatterEngine3/src/shader_source.cpp and the win-shaders
# removal note above) so they are NEVER copied here — only the exe + project
# content, which lives outside the build tree at ../projects/.
#
# Platform is inferred the same way the rest of this Makefile does (MSYSTEM is
# set inside an MSYS2 shell): under MSYS2 we package the Windows exe, otherwise
# the Linux binary. Either way `dist` depends on the real build so it compiles
# first if needed.
ifdef MSYSTEM
DIST_BUILD_TARGET = windows
DIST_BIN_DIR = $(W_DIR)
DIST_BIN = editor.exe
else
# Phase 5b: DIST_BUILD_TARGET now names the real `linux` target added below
# (see "Linux Vulkan target (Phase 5b)" near win-shaders-gpu), which places
# its binary at $(L_DIR)/editor -- so the specific "No rule to make target
# 'editor'" failure this comment used to document is gone. UNVERIFIED/STILL
# BROKEN for an unrelated, pre-existing reason this phase does not fix: the
# `dist` recipe below unconditionally requires STREAMLINE_PATH and copies
# *.dll files (see the "dist REQUIRES the Streamline SDK" comment below).
# Streamline is Windows-only and there is no DLSS on Linux (see the `linux`
# target's own HAVE_STREAMLINE note), so `dist` on a real Linux shell will
# still hard-fail at the STREAMLINE_PATH check before it ever reaches
# DIST_BUILD_TARGET. Making `dist` produce a sensible Linux package (no
# Streamline requirement, no .dll copy step) is out of scope here.
DIST_BUILD_TARGET = linux
DIST_BIN_DIR = $(L_DIR)
DIST_BIN = editor
endif

PROJECT ?= world_demo
DIST_ROOT = build/dist/$(PROJECT)

# dist REQUIRES the Streamline SDK. A shipped build is expected to have DLSS /
# Reflex available, so this deliberately fails rather than quietly producing a
# package without them.
#
# The build is re-invoked with HAVE_STREAMLINE=1 rather than merely copying the
# DLLs alongside whatever binary already exists: MATTER_HAVE_STREAMLINE is a
# COMPILE-time define (see CXX_FLAGS_BUILD_WIN_COMMON), so a binary built with
# HAVE_STREAMLINE=0 ignores the DLLs even when they sit next to it. Shipping
# them beside such a binary would look correct and silently do nothing.
#
# Nothing from the SDK is checked into this repository; point STREAMLINE_PATH at
# a locally installed, licensed copy. You are responsible for the redistribution
# terms of anything this stages into the package.
.PHONY: dist
dist:
	@[ -n "$(STREAMLINE_PATH)" ] || ( \
	    echo "ERROR: dist requires the Streamline SDK."; \
	    echo "  Set STREAMLINE_PATH to a local, licensed SDK install, e.g."; \
	    echo "    make dist STREAMLINE_PATH=/c/sdk/streamline"; \
	    echo "  (override the DLL dir with STREAMLINE_DLL_DIR if it is not <path>/bin/x64/development)"; \
	    exit 1)
	@[ -d "$(STREAMLINE_DLL_DIR)" ] || ( \
	    echo "ERROR: STREAMLINE_DLL_DIR not found: $(STREAMLINE_DLL_DIR)"; \
	    exit 1)
	@ls "$(STREAMLINE_DLL_DIR)"/*.dll >/dev/null 2>&1 || ( \
	    echo "ERROR: no .dll files in $(STREAMLINE_DLL_DIR)"; \
	    exit 1)
	@[ -d ../projects/$(PROJECT) ] || (echo "ERROR: ../projects/$(PROJECT) not found" && exit 1)
	@$(MAKE) $(DIST_BUILD_TARGET) HAVE_STREAMLINE=1 \
	    STREAMLINE_PATH="$(STREAMLINE_PATH)" STREAMLINE_DLL_DIR="$(STREAMLINE_DLL_DIR)"
	@rm -rf $(DIST_ROOT)
	@mkdir -p $(DIST_ROOT)/projects
	@cp $(DIST_BIN_DIR)/$(DIST_BIN) $(DIST_ROOT)/$(DIST_BIN)
	@cp -f "$(STREAMLINE_DLL_DIR)"/*.dll $(DIST_ROOT)/
	@tar -C ../projects --exclude=.cache --exclude=backup --exclude='*.bak' \
	    -cf - $(PROJECT) | tar -C $(DIST_ROOT)/projects -xf -
	@echo "dist ready: $(DIST_ROOT)"
	@echo "  binary   : $(DIST_BIN) (built with MATTER_HAVE_STREAMLINE=1)"
	@echo "  streamline: $$(ls $(DIST_ROOT)/*.dll 2>/dev/null | wc -l) dll(s)"
	@echo "  project  : $(PROJECT) (.cache excluded; it is regenerable)"
	@echo "Launch $(DIST_BIN) from inside $(DIST_ROOT)/ (asset lookup is cwd-relative)."

# SelectionSet headless test (no GL/raylib/imgui deps)
build/test_selection_set.exe: tests/test_selection_set.cpp src/selection_set.cpp src/selection_set.h
	@mkdir -p $(dir $@)
	$(WIN_CXX) tests/test_selection_set.cpp src/selection_set.cpp -o $@ \
	    -std=c++17 -Wall -Wextra -Isrc

.PHONY: run-test-selection-set
run-test-selection-set: build/test_selection_set.exe
	./build/test_selection_set.exe

# Camera orbit / move / turn math (issue a4203d22 parts 1-2). Pure CPU: no
# ImGui context, no window, no session -- camera_orbit.cpp exists precisely
# so this math is reachable without any of them. camera_controller.cpp comes
# along for apply_camera_input (what the Move/Turn buttons call); it drags in
# GLFW only because CameraController::update shares its TU, hence the
# $(GLFW_WIN_LIB) on the link line -- nothing here touches a window.
build/test_camera_orbit.exe: tests/test_camera_orbit.cpp \
        src/camera_orbit.cpp src/camera_orbit.h \
        src/camera_controller.cpp src/camera_controller.h $(GLFW_WIN_LIB)
	@mkdir -p $(dir $@)
	$(WIN_CXX) tests/test_camera_orbit.cpp src/camera_orbit.cpp \
	    src/camera_controller.cpp -o $@ \
	    -std=c++17 -Wall -Wextra -Isrc -I$(ME3_DIR)/include \
	    $(GLFW_WIN_LIB) -lgdi32 -luser32 -lshell32

.PHONY: run-test-camera-orbit
run-test-camera-orbit: build/test_camera_orbit.exe
	./build/test_camera_orbit.exe

# Asset-browser action headless test (issue editor-workbench-actions-noop):
# Reveal's module->baked-root selection, camera focus over baked-root bounds,
# and the Workbench isolation world text (the unexpanded root). Pure CPU; no
# GL/ImGui. flecs.o comes in through camera_focus.h's properties_panel.h ->
# matter/ecs.h chain — header-inline flecs, not a real dependency (same
# situation and fix as build/test_issue_reporter.exe below).
build/test_workbench_actions.exe: tests/test_workbench_actions.cpp \
        src/reveal_part.cpp src/reveal_part.h \
        src/camera_focus.cpp src/camera_focus.h \
        src/part_workbench_iso.cpp src/part_workbench_iso.h \
        src/selection_set.cpp src/selection_set.h build/windows/obj/flecs.o
	@mkdir -p $(dir $@)
	$(WIN_CXX) tests/test_workbench_actions.cpp src/reveal_part.cpp \
	    src/camera_focus.cpp src/part_workbench_iso.cpp src/selection_set.cpp \
	    build/windows/obj/flecs.o \
	    -o $@ -std=c++17 -Wall -Isrc \
	    -I$(ME3_DIR)/include -I$(ME3_DIR)/src -I$(FLECS_DIR) -lws2_32 -ldbghelp

.PHONY: run-test-workbench-actions
run-test-workbench-actions: build/test_workbench_actions.exe
	./build/test_workbench_actions.exe

# Asset-browser required-child ImGui identity regression. Pure CPU: the row
# identity helper is ImGui-free, while draw_object_row uses the same helper to
# scope each repeated "Go" control.
build/test_asset_browser_ids.exe: tests/test_asset_browser_ids.cpp \
        src/asset_browser_ids.h
	@mkdir -p $(dir $@)
	$(WIN_CXX) tests/test_asset_browser_ids.cpp -o $@ \
	    -std=c++17 -Wall -Wextra -Isrc

.PHONY: run-test-asset-browser-ids
run-test-asset-browser-ids: build/test_asset_browser_ids.exe
	./build/test_asset_browser_ids.exe

# Animation panel model headless test: animation_panel_model.cpp holds the
# ImGui-free presentation logic (animation_panel.cpp does the drawing), so this
# links with no ImGui and no engine archive -- only the public debug headers.
build/test_animation_panel_model.exe: tests/test_animation_panel_model.cpp \
    src/animation_panel_model.cpp src/animation_panel_model.h
	@mkdir -p $(dir $@)
	$(WIN_CXX) tests/test_animation_panel_model.cpp src/animation_panel_model.cpp -o $@ \
	    -std=c++17 -Wall -Wextra -Isrc -I$(ME3_DIR)/include -I$(MATHLIB_DIR)/include

.PHONY: run-test-animation-panel-model
run-test-animation-panel-model: build/test_animation_panel_model.exe
	./build/test_animation_panel_model.exe

# ConsoleLog headless test: console_log.cpp holds the ImGui-free ring-buffer
# logic (draw_console_contents, which needs ImGui, lives in console_panel.cpp
# instead), so this links without any ImGui dependency.
build/test_console_log.exe: tests/test_console_log.cpp src/console_log.cpp src/console_panel.h
	@mkdir -p $(dir $@)
	$(WIN_CXX) tests/test_console_log.cpp src/console_log.cpp -o $@ \
	    -std=c++17 -Wall -Wextra -Isrc -lpthread

.PHONY: run-test-console-log
run-test-console-log: build/test_console_log.exe
	./build/test_console_log.exe

# Issue reporter headless test: covers issue_reporter.cpp (crop math + the
# on-disk report schema). The dialog half needs ImGui and is not covered.
# flecs.o comes in through ui.h's editor_model.h/scene_tree_panel.h chain --
# header-inline flecs, not a real dependency of the writer. issue_reporter.cpp
# deliberately avoids matter::dlss_mode_name so this does not have to link
# streamline_bridge.o and, through it, the whole Vulkan loader.
build/test_issue_reporter.exe: tests/test_issue_reporter.cpp src/issue_reporter.cpp \
                               src/console_log.cpp src/issue_reporter.h \
                               $(ME3_DIR)/src/props/props.cpp \
                               $(ME3_DIR)/src/util/json_doc.cpp \
                               $(PROFILE_DIR)/src/profile.cpp \
                               build/windows/obj/flecs.o
	@mkdir -p $(dir $@)
	$(WIN_CXX) tests/test_issue_reporter.cpp src/issue_reporter.cpp \
	    src/console_log.cpp $(ME3_DIR)/src/props/props.cpp \
	    $(ME3_DIR)/src/util/json_doc.cpp $(PROFILE_DIR)/src/profile.cpp \
	    build/windows/obj/flecs.o -o $@ \
	    -std=c++17 -Wall -Isrc -I../MatterEngine3/include -I../MatterEngine3/src \
	    -I../libs/MatterSurfaceLib/include -I../libs/SpatialQueryLib/include \
	    -I../libs/MemoryLib/include -I../libs/MathLib/include \
	    -I$(PROFILE_DIR)/include \
	    -I../third_party/imgui -I../third_party/ImGuizmo \
	    -I../third_party/Vulkan-Headers/include -I../third_party/flecs \
	    -I../libs/ParticleFlowLib/include -I../third_party/box3d/include \
	    -lpthread -lws2_32 -ldbghelp

.PHONY: run-test-issue-reporter
run-test-issue-reporter: build/test_issue_reporter.exe
	./build/test_issue_reporter.exe

.PHONY: editor windows clean dist shaders  regen-processed-shader mingw-box3d embedded-shaders build-engine-lib vulkan-instancing-perf linux check-animation-production-sources

# The direct-source builds (windows/linux) are production. Keep their TU list
# aligned with the archive's runtime implementation units without dragging in
# Ozz's bake-only compiler: animation schema verbs are present, while raw-clip
# compilation is routed to the bake host rather than the editor runtime.
check-animation-production-sources:
	@for source in dsl_animation.cpp animation_ir.cpp animation_validate.cpp animation_binding_bake.cpp animation_runtime_asset.cpp anim_asset.cpp anim_bundle.cpp animation_asset_store.cpp animation_store.cpp animation_evaluator.cpp animation_systems.cpp animation_world_queries.cpp animation_controllers.cpp animation_targets.cpp animation_rigid_bridge.cpp animation_skin_bridge.cpp; do \
		case " $(WIN_ME3_CPP) " in *"/$$source "*) ;; *) \
			echo "ERROR: production build omits $$source"; exit 1 ;; esac; \
	done
	@case " $(WIN_ME3_CPP) " in *"/ozz_bake.cpp "*) ;; *) \
		echo "ERROR: editor must compile ozz_bake.cpp -- it bakes as well as runs"; exit 1 ;; esac
	@case " $(WIN_LIBS) " in *"$(OZZ_OFFLINE_LIB) "*) ;; *) \
		echo "ERROR: editor must link the Ozz offline archive"; exit 1 ;; esac
