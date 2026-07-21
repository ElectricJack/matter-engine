# MatterViewer — world viewer application (GL build).
#
# Linux: compiles ONLY main.cpp + ui.cpp + imgui; links libmatter_engine3.a
#   (which contains the full engine: render/provider/facade, bake pipeline,
#   MSL backend, QuickJS-ng). Add raylib, box3d, and (optionally) autoremesher.
#   Engine include path is -I../MatterEngine3/include ONLY — private ME3 src/
#   headers are not visible to app code. When libautoremesher_core.a is present,
#   the library is rebuilt with MATTER_HAVE_AUTOREMESHER so part_flatten.cpp
#   compiles the retopo hook path.
#
# Windows: MinGW cross-compile that does direct-source compilation of all
#   engine+MSL+QJS units with updated ../MatterEngine3/src/... paths.
#   (Pragmatic: no mingw .a packaging this phase. Private ME3 src/ includes are
#   scoped to the Windows variable set below.)
#
# Per-object parallel build: each TU compiles to its own .o under build/<platform>/
# so `make -j` fans them across cores; header changes are tracked via -MMD -MP
# generated .d files (so incremental rebuilds are correct, not silently stale).
JOBS ?= $(shell nproc)
MAKEFLAGS += -j$(JOBS)

CC = g++
CFLAGS = -std=c++17 -O2 -Wall -Wno-missing-braces -Wno-unused-variable \
         -DPLATFORM_DESKTOP

RAYLIB_PATH = ../Libraries/raylib/src
IMGUI_PATH  = ../Libraries/imgui
# Official ImGuizmo upstream commit: dc25afb98bc3ebe00dfc9a23ba7235fead2ccb1d
IMGUIZMO_PATH = ../Libraries/ImGuizmo
QJS_DIR     = ../Libraries/quickjs-ng
QJS_INC     = -I$(QJS_DIR)
BOX3D_DIR   = ../Libraries/box3d
BOX3D_LIB   = $(BOX3D_DIR)/libbox3d.a
WIN_BOX3D_LIB = $(BOX3D_DIR)/build-mingw/libbox3d.a
ME3_DIR     = ../MatterEngine3
MSL_DIR     = ../MatterSurfaceLib
FLECS_DIR   = ../Libraries/flecs

# Linux app compile: only public engine API + imgui + raylib headers visible.
# Private ME3 src/ headers are NOT exposed here; they are internal to the library.
# -I$(ME3_DIR)/src: world_session.h currently includes the private
# render/vk_gi_contract.h; drop this once the public API stops leaking it.
INCLUDE_PATHS = -I$(ME3_DIR)/include \
                -I$(ME3_DIR)/src \
                -I$(MSL_DIR)/include \
                -I$(RAYLIB_PATH) \
                -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends \
                -I$(IMGUIZMO_PATH) \
                -I$(RAYLIB_PATH)/external/glfw/include \
                -I$(BOX3D_DIR)/include \
                -I$(FLECS_DIR) \
                -I../Libraries/Vulkan-Headers/include

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
                     -I../ParticleFlowLib/include

LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11
LDLIBS  = ../Libraries/raylib/build/linux/libraylib.a $(BOX3D_LIB)

HAVE_STREAMLINE ?= 0
# Point this at a locally installed, licensed Streamline SDK.  Nothing from the
# SDK is checked into this repository; HAVE_STREAMLINE=0 is the default.
STREAMLINE_PATH ?=
STREAMLINE_DLL_DIR ?= $(STREAMLINE_PATH)/bin/x64
# ---- Phase 5 autoremesher_core (Task 15 wiring) ----
# The vendored autoremesher_core static lib + its TBB dependency. Linux-only:
# the library was built for Linux in Task 4 (Libraries/autoremesher_core/) and
# the geogram/TBB build under mingw isn't part of this landing. Windows viewer
# builds WITHOUT -DMATTER_HAVE_AUTOREMESHER, so schemas that opt into retopo
# transparently fall through to the streaming unretopo'd flatten path (same
# behavior guarantee as tests/Makefile RETOPO_INT_TARGET's design).
#
# Only fold retopo into the build if the static lib and TBB build dir are
# actually present — build-all.sh produces both. Otherwise skip retopo entirely
# and the viewer builds as before (schemas without retopo unaffected; schemas
# WITH retopo hit the #else warn-and-continue path in part_flatten.cpp).
AR_CORE_PATH := ../Libraries/autoremesher_core
AR_CORE_LIB  := $(AR_CORE_PATH)/libautoremesher_core.a
TBB_BUILD_DIR := $(firstword $(wildcard $(AR_CORE_PATH)/thirdparty/tbb/build/linux_*_release))
TBB_BUILD_SUBDIR := $(patsubst $(AR_CORE_PATH)/thirdparty/tbb/build/%,%,$(TBB_BUILD_DIR))
HAVE_AUTOREMESHER := $(and $(wildcard $(AR_CORE_LIB)),$(TBB_BUILD_DIR))

ifneq ($(HAVE_AUTOREMESHER),)
  # -DMATTER_HAVE_AUTOREMESHER enables the retopo hook in part_flatten.cpp (inside
  # the library) and the retopo() glue in mesh_retopo.cpp (compiled into the library
  # via EXTRA_RETOPO_CPP). The library is rebuilt with these flags as a prerequisite
  # of the viewer target, so part_flatten.o in the archive sees the flag.
  AR_CORE_CFLAGS := -DMATTER_HAVE_AUTOREMESHER -I$(AR_CORE_PATH)/include
  # --allow-multiple-definition: raylib bundles stb_image_write.h in rtextures.c
  # and autoremesher_core's geogram bundles its own copy in
  # image_serializer_stb.o. Both archives get pulled in here — raylib because
  # the viewer draws GL, autoremesher_core because part_flatten's hook
  # references autoremesher::retopo. The stb sources are byte-equivalent vendor
  # copies, so picking either definition is safe. This mirrors tests/Makefile
  # RETOPO_INT_TARGET's link workaround verbatim.
  AR_CORE_LDFLAGS := -L$(AR_CORE_PATH) -lautoremesher_core \
                     -L$(TBB_BUILD_DIR) -ltbb \
                     -Wl,-rpath,'$$ORIGIN/../Libraries/autoremesher_core/thirdparty/tbb/build/$(TBB_BUILD_SUBDIR)' \
                     -Wl,--allow-multiple-definition
else
  AR_CORE_CFLAGS :=
  AR_CORE_LDFLAGS :=
endif

# ---- Linux viewer: preserved legacy GL entry/UI + imgui; links libmatter_engine3.a ----
# libmatter_engine3.a contains the full engine: ME3 pipeline, MSL backend, QJS.
# The library is (re)built as a prerequisite of the viewer target so it picks up
# MATTER_HAVE_AUTOREMESHER when autoremesher is present.

# Task 9 switches the Windows application to Vulkan. Keep the existing Linux
# application/backend as an explicit compatibility path until the later engine
# OpenGL-removal task can migrate or retire it without breaking the default target.
LINUX_APP_SRC = main_linux.cpp ui_linux.cpp camera_controller.cpp camera_focus.cpp streaming_anchor_controller.cpp selection_set.cpp editor_model.cpp scene_tree_panel.cpp selection_bounds.cpp viewport_pick.cpp selection_outline.cpp toolbar_panel.cpp console_panel.cpp console_log.cpp properties_registry.cpp properties_panel.cpp specialized_editors.cpp gizmo.cpp os_open.cpp
APP_SRC = main.cpp ui.cpp camera_controller.cpp camera_focus.cpp streaming_anchor_controller.cpp selection_set.cpp editor_model.cpp scene_tree_panel.cpp selection_bounds.cpp viewport_pick.cpp selection_outline.cpp toolbar_panel.cpp console_panel.cpp console_log.cpp properties_registry.cpp properties_panel.cpp specialized_editors.cpp gizmo.cpp os_open.cpp

# Windows direct-source compile: ME3 pipeline + MSL + QJS (mirrors old viewer).
# These are ONLY used by the Windows target; the Linux target links libmatter_engine3.a.
WIN_ME3_CPP = $(ME3_DIR)/src/part_graph.cpp \
              $(ME3_DIR)/src/script_host.cpp $(ME3_DIR)/src/dsl_state.cpp \
              $(ME3_DIR)/src/dsl_triangle.cpp $(ME3_DIR)/src/dsl_bindings.cpp \
              $(ME3_DIR)/src/csg_lowering.cpp $(ME3_DIR)/src/module_resolver.cpp \
              $(ME3_DIR)/src/script/world_definition_loader.cpp \
              $(ME3_DIR)/src/script_rng_binding.cpp \
              $(ME3_DIR)/src/polygon_triangulate.cpp \
              $(ME3_DIR)/src/part_asset_v2.cpp $(ME3_DIR)/src/lod_bake.cpp \
              $(ME3_DIR)/src/world_flatten.cpp $(ME3_DIR)/src/part_flatten.cpp \
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
              $(ME3_DIR)/src/render/raster_mesh.cpp $(ME3_DIR)/src/render/part_store.cpp \
              $(ME3_DIR)/src/render/world_state.cpp \
              $(ME3_DIR)/src/render/streamline_bridge.cpp \
              $(ME3_DIR)/src/render/vk_context.cpp $(ME3_DIR)/src/render/vk_resources.cpp \
              $(ME3_DIR)/src/render/vk_pipeline.cpp $(ME3_DIR)/src/render/vk_scene_renderer.cpp \
              $(ME3_DIR)/src/render/vk_volumetrics.cpp $(ME3_DIR)/src/render/vk_emitter_gather.cpp \
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
              $(ME3_DIR)/src/render/dynamic_instance_slots.cpp \
              $(ME3_DIR)/src/streaming/sector_streaming_coordinator.cpp \
              $(ME3_DIR)/src/matter_engine.cpp \
              $(ME3_DIR)/src/pf_bindings.cpp \
              ../ParticleFlowLib/src/pf_math.cpp \
              ../ParticleFlowLib/src/pf_sim.cpp \
              ../ParticleFlowLib/src/pf_fields.cpp \
              ../ParticleFlowLib/src/pf_path_recorder.cpp

WIN_MSL_CPP = $(MSL_DIR)/src/cluster.cpp $(MSL_DIR)/src/cell.cpp \
              $(MSL_DIR)/src/mesh_simplifier.cpp $(MSL_DIR)/src/mesh_indexed.cpp \
              $(MSL_DIR)/src/mesh_smooth.cpp $(MSL_DIR)/src/mesh_transform.cpp \
              $(MSL_DIR)/src/blas_manager.cpp $(MSL_DIR)/src/bvh.cpp \
              $(MSL_DIR)/src/bvh_analyzer.cpp \
              $(MSL_DIR)/src/mesh_worker_pool.cpp $(MSL_DIR)/src/mesh_build_utils.cpp \
              $(MSL_DIR)/src/meshing_algorithm.cpp \
              $(MSL_DIR)/src/marching_cubes_algorithm.cpp \
              $(MSL_DIR)/src/oriented_cube_algorithm.cpp \
              $(MSL_DIR)/src/vertex_ao.cpp \
              $(MSL_DIR)/src/occupancy.cpp $(MSL_DIR)/src/tlas_manager.cpp \
              $(MSL_DIR)/src/part_asset.cpp \
              $(MSL_DIR)/src/lattice.cpp $(MSL_DIR)/src/particle_culling.cpp

WIN_PIPELINE_C = $(MSL_DIR)/src/surface.c $(MSL_DIR)/src/open_particle_surface.c \
                 $(MSL_DIR)/src/spatial_hash.c $(MSL_DIR)/src/object_allocator.c \
                 $(MSL_DIR)/src/material_registry.c $(MSL_DIR)/src/fat_primitive.c

QJS_C   = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c $(QJS_DIR)/libunicode.c \
          $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c

FLECS_C = $(FLECS_DIR)/flecs.c

IMGUI_CORE_SRC = $(IMGUI_PATH)/imgui.cpp $(IMGUI_PATH)/imgui_draw.cpp \
            $(IMGUI_PATH)/imgui_tables.cpp $(IMGUI_PATH)/imgui_widgets.cpp \
            $(IMGUI_PATH)/backends/imgui_impl_glfw.cpp
IMGUIZMO_SRC = $(IMGUIZMO_PATH)/ImGuizmo.cpp
IMGUI_SRC_LINUX = $(IMGUI_CORE_SRC) $(IMGUIZMO_SRC) \
            $(IMGUI_PATH)/backends/imgui_impl_opengl3.cpp
IMGUI_SRC_WIN = $(IMGUI_CORE_SRC) $(IMGUIZMO_SRC) \
            $(IMGUI_PATH)/backends/imgui_impl_vulkan.cpp

# ---- Per-object lists (one .o per TU, in a platform build dir, so `make -j`
# can compile them concurrently). Basenames are unique across all source dirs,
# so flattening with $(notdir ...) is collision-free. vpath (below) lets the
# pattern rules find each source by basename.
#
# Linux: only APP_SRC + IMGUI_SRC are compiled as objects; engine+MSL+QJS come
# from libmatter_engine3.a on the link line.
#
# Windows: full direct-source compile (APP_SRC + WIN_ME3_CPP + WIN_MSL_CPP + IMGUI_SRC).
CXX_FLAGS_BUILD_COMMON = $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB $(INCLUDE_PATHS) -I$(BOX3D_DIR)/include $(QJS_INC) -MMD -MP
# Windows is the release build path for now: -DNDEBUG disables assert().
CXX_FLAGS_BUILD_WIN_COMMON = $(CFLAGS) -DNDEBUG -DMATTER_HAVE_SCRIPT_HOST -DMATTER_VULKAN_VIEWER -DMATTER_VULKAN_ONLY $(WIN_INCLUDE_PATHS) -I$(BOX3D_DIR)/include $(QJS_INC) -ffunction-sections -fdata-sections -MMD -MP
CXX_FLAGS_BUILD_WIN_COMMON += -DMATTER_HAVE_STREAMLINE=$(HAVE_STREAMLINE)
ifeq ($(HAVE_STREAMLINE),1)
  CXX_FLAGS_BUILD_WIN_COMMON += -I"$(STREAMLINE_PATH)/include"
endif

# Linux object set: ONLY app + imgui.
L_CPP_SRC = $(LINUX_APP_SRC) $(IMGUI_SRC_LINUX)
CPP_NAMES = $(notdir $(L_CPP_SRC))

# Windows object set: full direct-source compile.
WIN_ALL_CPP_SRC = $(APP_SRC) $(WIN_ME3_CPP) $(WIN_MSL_CPP) $(IMGUI_SRC_WIN)
CPP_NAMES_WIN_ALL = $(notdir $(WIN_ALL_CPP_SRC))
QJS_NAMES = $(notdir $(QJS_C))
PIPC_NAMES_WIN = $(notdir $(WIN_PIPELINE_C))
FLECS_NAME = $(notdir $(FLECS_C))

vpath %.cpp . $(ME3_DIR)/src $(ME3_DIR)/src/render $(ME3_DIR)/src/provider $(ME3_DIR)/src/ecs \
             $(ME3_DIR)/src/streaming $(ME3_DIR)/src/script \
             $(MSL_DIR)/src $(IMGUI_PATH) $(IMGUI_PATH)/backends \
             $(IMGUIZMO_PATH) \
             ../ParticleFlowLib/src
vpath %.c $(MSL_DIR)/src $(QJS_DIR) $(FLECS_DIR)

.DEFAULT_GOAL := viewer

# Linux object sets: ONLY app + imgui objects.
L_DIR = build/linux
L_CPP_OBJ  = $(addprefix $(L_DIR)/,$(CPP_NAMES:.cpp=.o))
L_ALL_OBJ  = $(L_CPP_OBJ)

# Windows object sets: full direct-source compile including engine+MSL.
W_DIR = build/windows
W_CPP_OBJ  = $(addprefix $(W_DIR)/,$(CPP_NAMES_WIN_ALL:.cpp=.o))
W_QJS_OBJ  = $(addprefix $(W_DIR)/,$(QJS_NAMES:.c=.o))
W_PIPC_OBJ = $(addprefix $(W_DIR)/,$(PIPC_NAMES_WIN:.c=.o))
W_FLECS_OBJ = $(W_DIR)/$(FLECS_NAME:.c=.o)
W_ALL_OBJ  = $(W_CPP_OBJ) $(W_QJS_OBJ) $(W_PIPC_OBJ) $(W_FLECS_OBJ)

$(L_DIR) $(W_DIR):
	mkdir -p $@

# ---- Linux compile rules (app + imgui only)
$(L_CPP_OBJ): $(L_DIR)/%.o: %.cpp | $(L_DIR)
	$(CC) -c $< -o $@ $(CXX_FLAGS_BUILD_COMMON)

# ---- Library prerequisite: build libmatter_engine3.a before linking the viewer.
# When autoremesher is present, pass the flag + AR include path so that
# part_flatten.cpp and mesh_retopo.cpp inside the library see the right defines.
# EXTRA_RETOPO_CPP tells the library Makefile to also compile mesh_retopo.cpp.
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

# Generate embedded shader header before compiling any TU that includes it.
# Needed by the Windows direct-source build (shader_source.cpp); the Linux
# viewer gets it implicitly — the library sub-make generates the header before
# its own objects. Keeping it off the viewer prereq list also avoids two
# parallel recursive makes into ME3_DIR racing under -j.
.PHONY: embedded-shaders
embedded-shaders:
	$(MAKE) -C $(ME3_DIR) shaders_gen/embedded_shaders.h

viewer: build-engine-lib shaders shaders_gpu_link $(L_ALL_OBJ)
	$(CC) $(L_ALL_OBJ) $(ME3_DIR)/libmatter_engine3.a -o viewer \
	      $(CFLAGS) $(LDFLAGS) $(LDLIBS) $(AR_CORE_LDFLAGS)

-include $(L_ALL_OBJ:.o=.d)

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

# shaders_gpu/ is a real directory committed to the repo; it lives under MatterEngine3/.
# Create a symlink here so the binary can resolve shaders_gpu/cull.comp etc.
shaders_gpu_link:
	@[ -e shaders_gpu ] || ln -s $(ME3_DIR)/shaders_gpu shaders_gpu
	@[ -d shaders_gpu ] || (echo "ERROR: shaders_gpu/ symlink target missing" && exit 1)

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
# Win32 import libs raylib depends on; MUST come AFTER libraylib.a on the link
# line so the static linker can resolve raylib's __imp_* references.
WIN_LIBS    = $(WIN_BOX3D_LIB) -lwinpthread
WIN_SYSTEM_LIBS = -lgdi32 -lwinmm -luser32 -lshell32 -lws2_32 -ldbghelp

.PHONY: vulkan-preflight
vulkan-preflight:
	@WIN_CXX="$(WIN_CXX)" VULKAN_INCLUDE="$(VULKAN_INCLUDE)" \
	 VULKAN_LIB_DIR="$(VULKAN_LIB_DIR)" GLSLC="$(GLSLC)" \
	 HAVE_STREAMLINE="$(HAVE_STREAMLINE)" STREAMLINE_PATH="$(STREAMLINE_PATH)" \
	 STREAMLINE_DLL_DIR="$(STREAMLINE_DLL_DIR)" sh tools/check_vulkan_toolchain.sh

$(WIN_FEATURES): vulkan-preflight
	@mkdir -p $(dir $@)
	@printf 'VULKAN=1\nOPENGL=0\n' > $@

# Standalone GLFW archive for Vulkan smoke tests.  Keep this separate from
# raylib's GLFW objects so the existing OpenGL viewer path is unchanged.
GLFW_SRC_DIR = ../Libraries/raylib/src/external/glfw/src
GLFW_WIN_SRC = context.c egl_context.c init.c input.c monitor.c null_init.c \
               null_joystick.c null_monitor.c null_window.c osmesa_context.c \
               platform.c vulkan.c win32_init.c win32_joystick.c \
               win32_module.c win32_monitor.c win32_thread.c win32_time.c \
               win32_window.c window.c
GLFW_WIN_OBJ = $(addprefix build/windows/glfw/,$(GLFW_WIN_SRC:.c=.o))
GLFW_WIN_OBJ += build/windows/glfw/glfw_vulkan_only_context.o
GLFW_WIN_LIB = build/windows/libglfw3.a

build/windows/glfw/%.o: $(GLFW_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -c $< -o $@ -D_GLFW_WIN32 \
	    -I../Libraries/raylib/src/external/glfw/include

build/windows/glfw/glfw_vulkan_only_context.o: glfw_vulkan_only_context.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -c $< -o $@ -D_GLFW_WIN32 \
	    -I$(GLFW_SRC_DIR) -I../Libraries/raylib/src/external/glfw/include

$(GLFW_WIN_LIB): $(GLFW_WIN_OBJ)
	$(WIN_AR) rcs $@ $^

VK_SPV = build/shaders_vk/transform_probe.comp.spv \
         build/shaders_vk/cull.comp.spv \
         build/shaders_vk/raster.vert.spv \
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
         build/shaders_vk/rt_surface_test.rgen.spv
VK_EMBEDDED_HEADER = $(ME3_DIR)/shaders_gen/embedded_spirv.h

build/shaders_vk/%.spv: $(ME3_DIR)/shaders_vk/%
	@mkdir -p $(dir $@)
	$(GLSLC) --target-env=vulkan1.3 -O -I$(ME3_DIR)/shaders_vk $< -o $@

build/shaders_vk/raster.vert.spv build/shaders_vk/gbuffer.frag.spv: \
    $(ME3_DIR)/shaders_vk/material_common.glsl

build/shaders_vk/rt_surface.rchit.spv build/shaders_vk/rt_visibility.rahit.spv \
    build/shaders_vk/rt_radiance.rmiss.spv \
    build/shaders_vk/rt_lighting.rgen.spv \
    build/shaders_vk/rt_surface_test.rgen.spv: \
    $(ME3_DIR)/shaders_vk/rt_surface_common.glsl

build/shaders_vk/composite.frag.spv build/shaders_vk/rt_lighting.rgen.spv: \
    $(ME3_DIR)/shaders_vk/sky_common.glsl

build/shaders_vk/gi_temporal.comp.spv build/shaders_vk/gi_atrous.comp.spv: \
    $(ME3_DIR)/shaders_vk/gi_common.glsl

$(VK_EMBEDDED_HEADER): $(VK_SPV) $(ME3_DIR)/tools/embed_spirv.py
	python3 $(ME3_DIR)/tools/embed_spirv.py $@ $(VK_SPV)

build/windows/vulkan_smoke_tests.exe: \
        $(ME3_DIR)/tests/vulkan_smoke_tests.cpp \
        ui.cpp ui.h streaming_anchor_controller.cpp \
        streaming_anchor_controller.h $(IMGUI_SRC_WIN) \
        $(ME3_DIR)/include/matter/vulkan_device.h \
        $(ME3_DIR)/src/render/gpu_matrix_pack.h \
        $(ME3_DIR)/src/render/vk_draw_command.h \
        $(ME3_DIR)/src/render/frame_matrices.h \
        $(ME3_DIR)/src/render/matrix_math.h \
        $(ME3_DIR)/src/render/vk_resources.h \
        $(ME3_DIR)/src/render/vk_pipeline.h \
        $(ME3_DIR)/src/render/vk_scene_renderer.h \
        $(ME3_DIR)/src/render/vk_volumetrics.h \
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
        $(ME3_DIR)/src/render/vk_volumetrics.cpp \
        $(ME3_DIR)/src/render/vk_emitter_gather.cpp \
        $(ME3_DIR)/src/render/raster_mesh.cpp \
        $(ME3_DIR)/src/render/frame_matrices.cpp \
        $(ME3_DIR)/src/render/vk_temporal.cpp \
        $(ME3_DIR)/src/render/vk_gi_math.cpp \
        $(ME3_DIR)/src/render/vk_lighting_controls.cpp \
        $(ME3_DIR)/src/render/matrix_math.cpp \
        $(VK_EMBEDDED_HEADER) $(GLFW_WIN_LIB) $(W_FLECS_OBJ)
	@mkdir -p $(dir $@)
	$(WIN_CXX) $(ME3_DIR)/tests/vulkan_smoke_tests.cpp ui.cpp \
	    streaming_anchor_controller.cpp \
	    $(IMGUI_SRC_WIN) \
	    $(ME3_DIR)/src/render/streamline_bridge.cpp \
	    $(ME3_DIR)/src/render/vk_context.cpp \
	    $(ME3_DIR)/src/render/vk_resources.cpp \
	    $(ME3_DIR)/src/render/vk_pipeline.cpp \
	    $(ME3_DIR)/src/render/vk_scene_renderer.cpp \
	    $(ME3_DIR)/src/render/vk_volumetrics.cpp \
	    $(ME3_DIR)/src/render/vk_emitter_gather.cpp \
	    $(ME3_DIR)/src/render/raster_mesh.cpp \
	    $(ME3_DIR)/src/render/vk_instance_cache.cpp \
	    $(ME3_DIR)/src/render/frame_matrices.cpp \
	    $(ME3_DIR)/src/render/vk_temporal.cpp \
	    $(ME3_DIR)/src/render/vk_gi_math.cpp \
	    $(ME3_DIR)/src/render/vk_lighting_controls.cpp \
	    $(ME3_DIR)/src/render/matrix_math.cpp \
	    $(GLFW_WIN_LIB) $(W_FLECS_OBJ) -o $@ \
	    -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers \
	    -ffunction-sections -Wl,--gc-sections \
	    -static \
	    -DVK_USE_PLATFORM_WIN32_KHR \
	    -DMATTER_VK_TEST_FAULT_INJECTION \
	    -DMATTER_HAVE_STREAMLINE=$(HAVE_STREAMLINE) \
	    -DMATTER_VK_TEST_LAYER_PATH='"$(VULKAN_RUNTIME_DIR)"' \
	    -I$(ME3_DIR) -I$(ME3_DIR)/include -I$(ME3_DIR)/src \
	    -I../Libraries/raylib/src \
	    -I../MatterSurfaceLib/include \
	    -I$(ME3_DIR)/tests \
	    -I../Libraries/raylib/src/external/glfw/include \
	    -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends -I$(IMGUIZMO_PATH) \
	    -I$(FLECS_DIR) \
	    $(if $(filter 1,$(HAVE_STREAMLINE)),-I"$(STREAMLINE_PATH)/include") \
	    -I$(VULKAN_INCLUDE) -L$(VULKAN_LIB_DIR) -Wl,-Bdynamic -lvulkan-1 \
	    -lgdi32 -luser32 -lshell32 -Wl,-Bstatic

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
	    -I../Libraries/raylib/src

# ---- Windows compile rules
$(W_CPP_OBJ): $(W_DIR)/%.o: %.cpp | $(W_DIR)
	$(WIN_CXX) -c $< -o $@ $(CXX_FLAGS_BUILD_WIN_COMMON)
$(W_QJS_OBJ): $(W_DIR)/%.o: $(QJS_DIR)/%.c | $(W_DIR)
	$(WIN_CC) -c $< -o $@ -O2 -DNDEBUG -DCONFIG_VERSION='"0.10.0"' $(QJS_INC) -ffunction-sections -fdata-sections -MMD -MP
$(W_PIPC_OBJ): $(W_DIR)/%.o: $(MSL_DIR)/src/%.c | $(W_DIR)
	$(WIN_CC) -c $< -o $@ -O2 -DNDEBUG -DPLATFORM_DESKTOP $(WIN_INCLUDE_PATHS) -ffunction-sections -fdata-sections -MMD -MP
$(W_FLECS_OBJ): $(W_DIR)/%.o: $(FLECS_DIR)/%.c | $(W_DIR)
	$(WIN_CC) -c $< -o $@ -std=c17 -O2 -I$(FLECS_DIR) -DNDEBUG -ffunction-sections -fdata-sections -MMD -MP

# Order-only: the header must exist before shader_source.cpp compiles, but its
# regeneration must not force object rebuilds (embedded-shaders is phony).
$(W_CPP_OBJ): | embedded-shaders

windows: $(WIN_FEATURES) $(VK_EMBEDDED_HEADER) $(GLFW_WIN_LIB) win-shaders-gpu $(W_ALL_OBJ)
	$(WIN_CXX) $(W_ALL_OBJ) -o viewer.exe $(CFLAGS) -static -s -Wl,--gc-sections $(WIN_LIBS) $(GLFW_WIN_LIB) $(WIN_SYSTEM_LIBS) -L$(VULKAN_LIB_DIR) -Wl,-Bdynamic -lvulkan-1 -Wl,-Bstatic
	@if [ "$(HAVE_STREAMLINE)" = "1" ]; then cp -f "$(STREAMLINE_DLL_DIR)"/*.dll .; fi
	@imports="$$($(WIN_CXX) -print-prog-name=objdump -p viewer.exe)"; \
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

# shaders_gpu/ is a committed git symlink in MatterViewer/ pointing to
# MatterEngine3/shaders_gpu/. The symlink is navigable from WSL for the Linux
# viewer; re-create it here in case it was missing for some other reason.
win-shaders-gpu:
	@[ -d shaders_gpu ] || ln -s $(ME3_DIR)/shaders_gpu shaders_gpu
	@[ -d $(ME3_DIR)/shaders_gpu ] || (echo "ERROR: MatterEngine3/shaders_gpu/ missing" && exit 1)

# clean: remove build artifacts and generated binaries.
# shaders and shaders_gpu are committed git symlinks — do NOT remove them here,
# or `git status` will show them as deleted. The build recreates them via the
# shaders / shaders_gpu_link / win-shaders-gpu rules anyway.
clean:
	rm -rf viewer viewer.exe build

# SelectionSet headless test (no GL/raylib/imgui deps)
build/test_selection_set.exe: tests/test_selection_set.cpp selection_set.cpp selection_set.h
	@mkdir -p $(dir $@)
	$(WIN_CXX) tests/test_selection_set.cpp selection_set.cpp -o $@ \
	    -std=c++17 -Wall -Wextra -I.

.PHONY: run-test-selection-set
run-test-selection-set: build/test_selection_set.exe
	./build/test_selection_set.exe

# ConsoleLog headless test: console_log.cpp holds the ImGui-free ring-buffer
# logic (draw_console_contents, which needs ImGui, lives in console_panel.cpp
# instead), so this links without any ImGui dependency.
build/test_console_log.exe: tests/test_console_log.cpp console_log.cpp console_panel.h
	@mkdir -p $(dir $@)
	$(WIN_CXX) tests/test_console_log.cpp console_log.cpp -o $@ \
	    -std=c++17 -Wall -Wextra -I. -lpthread

.PHONY: run-test-console-log
run-test-console-log: build/test_console_log.exe
	./build/test_console_log.exe

.PHONY: viewer windows clean shaders shaders_gpu_link win-shaders-gpu regen-processed-shader mingw-box3d embedded-shaders build-engine-lib vulkan-instancing-perf
