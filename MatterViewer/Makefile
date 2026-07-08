# MatterViewer — world viewer application (GL build).
#
# Linux: compiles only main.cpp + ui.cpp + imgui; links libmatter_engine3.a
#   (which contains the render/provider/facade + bake pipeline + shader_source),
#   MatterSurfaceLib sources (MSL backend compiled directly — no libmsl.a yet),
#   QuickJS-ng, raylib, and box3d.
#
# Windows: MinGW cross-compile that still does direct-source compilation of all
#   engine+MSL+QJS units with updated ../MatterEngine3/src/... paths.
#
# Per-object parallel build: each TU compiles to its own .o under build/<platform>/
# so `make -j` fans them across cores; header changes are tracked via -MMD -MP
# generated .d files (so incremental rebuilds are correct, not silently stale).
JOBS ?= $(shell nproc)
MAKEFLAGS += -j$(JOBS)

CC = g++
CFLAGS = -std=c++17 -O2 -Wall -Wno-missing-braces -Wno-unused-variable \
         -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_43

RAYLIB_PATH = ../Libraries/raylib/src
IMGUI_PATH  = ../Libraries/imgui
QJS_DIR     = ../Libraries/quickjs-ng
QJS_INC     = -I$(QJS_DIR)
BOX3D_DIR   = ../Libraries/box3d
ME3_DIR     = ../MatterEngine3
MSL_DIR     = ../MatterSurfaceLib

INCLUDE_PATHS = -I$(ME3_DIR)/include \
                -I$(ME3_DIR)/src \
                -I$(ME3_DIR)/src/render \
                -I$(ME3_DIR)/src/provider \
                -I$(MSL_DIR)/include \
                -I$(RAYLIB_PATH) \
                -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends \
                -I$(RAYLIB_PATH)/external/glfw/include \
                -I$(BOX3D_DIR)/include

LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11
LDLIBS  = ../Libraries/raylib/build/linux/libraylib.a
BOX3D_LIB = $(BOX3D_DIR)/libbox3d.a
LDLIBS   += $(BOX3D_LIB)

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
  # -DMATTER_HAVE_AUTOREMESHER enables the retopo hook in part_flatten.cpp and
  # the retopo() / from_tri / to_tri glue in MSL. mesh_retopo.cpp is the MSL
  # unit that wraps autoremesher_core::remesh().
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
  AR_CORE_CPP := $(MSL_DIR)/src/mesh_retopo.cpp
else
  AR_CORE_CFLAGS :=
  AR_CORE_LDFLAGS :=
  AR_CORE_CPP :=
endif

# ---- Linux viewer: main.cpp + ui.cpp + imgui; links libmatter_engine3.a ----
# The .a contains: render/provider/facade + bake pipeline + shader_source.
# MSL backend and QuickJS are compiled directly here (no libmsl.a yet).

# Application entry points (GL + imgui; no engine logic).
APP_SRC = main.cpp ui.cpp

# MSL backend C++ sources (compiled directly; same set as old PIPELINE_CPP minus ME3 srcs).
MSL_CPP = $(MSL_DIR)/src/cluster.cpp $(MSL_DIR)/src/cell.cpp \
          $(MSL_DIR)/src/mesh_simplifier.cpp $(MSL_DIR)/src/mesh_indexed.cpp \
          $(MSL_DIR)/src/mesh_transform.cpp \
          $(MSL_DIR)/src/blas_manager.cpp $(MSL_DIR)/src/bvh.cpp \
          $(MSL_DIR)/src/bvh_analyzer.cpp \
          $(MSL_DIR)/src/mesh_worker_pool.cpp $(MSL_DIR)/src/mesh_build_utils.cpp \
          $(MSL_DIR)/src/meshing_algorithm.cpp \
          $(MSL_DIR)/src/marching_cubes_algorithm.cpp \
          $(MSL_DIR)/src/oriented_cube_algorithm.cpp \
          $(MSL_DIR)/src/vertex_ao.cpp \
          $(MSL_DIR)/src/occupancy.cpp $(MSL_DIR)/src/tlas_manager.cpp \
          $(MSL_DIR)/src/part_asset.cpp \
          $(MSL_DIR)/src/lattice.cpp $(MSL_DIR)/src/particle_culling.cpp \
          $(AR_CORE_CPP)

# ME3 pipeline C++ sources (compiled directly; the .a doesn't archive MSL/QJS deps).
ME3_CPP = $(ME3_DIR)/src/part_graph.cpp \
           $(ME3_DIR)/src/script_host.cpp $(ME3_DIR)/src/dsl_state.cpp \
           $(ME3_DIR)/src/dsl_triangle.cpp $(ME3_DIR)/src/dsl_bindings.cpp \
           $(ME3_DIR)/src/csg_lowering.cpp $(ME3_DIR)/src/module_resolver.cpp \
           $(ME3_DIR)/src/script_rng_binding.cpp \
           $(ME3_DIR)/src/polygon_triangulate.cpp \
           $(ME3_DIR)/src/part_asset_v2.cpp $(ME3_DIR)/src/lod_bake.cpp \
           $(ME3_DIR)/src/world_flatten.cpp $(ME3_DIR)/src/part_flatten.cpp \
           $(ME3_DIR)/src/triangle_emit.cpp \
           $(ME3_DIR)/src/part_cluster.cpp \
           $(ME3_DIR)/src/tileset_placement.cpp $(ME3_DIR)/src/tileset_layout.cpp \
           $(ME3_DIR)/src/tileset_torus_bvh.cpp \
           $(ME3_DIR)/src/tileset_gtex.cpp $(ME3_DIR)/src/tileset_bake_gpu.cpp \
           $(ME3_DIR)/src/sector_grid.cpp $(ME3_DIR)/src/lod_select.cpp \
           $(ME3_DIR)/src/world_lights.cpp $(ME3_DIR)/src/probe_volume.cpp \
           $(ME3_DIR)/src/world_tracer.cpp $(ME3_DIR)/src/probe_bake.cpp \
           $(ME3_DIR)/src/shader_source.cpp \
           $(ME3_DIR)/src/tileset_phase.cpp $(ME3_DIR)/src/tileset_phase_gpu.cpp \
           $(ME3_DIR)/src/tileset_bake.cpp $(ME3_DIR)/src/tileset_settle.cpp \
           $(ME3_DIR)/src/tileset_collider.cpp $(ME3_DIR)/src/tileset_part_collider.cpp \
           $(ME3_DIR)/src/tileset_bake_primary.cpp $(ME3_DIR)/src/tileset_bake_ao.cpp \
           $(ME3_DIR)/src/render/renderer.cpp $(ME3_DIR)/src/render/raster_composer.cpp \
           $(ME3_DIR)/src/render/raster_mesh.cpp $(ME3_DIR)/src/render/part_store.cpp \
           $(ME3_DIR)/src/render/world_composer.cpp $(ME3_DIR)/src/render/world_state.cpp \
           $(ME3_DIR)/src/render/gpu_culler.cpp $(ME3_DIR)/src/render/probe_texture.cpp \
           $(ME3_DIR)/src/render/tileset_provider.cpp $(ME3_DIR)/src/render/tileset_gl_ctx.cpp \
           $(ME3_DIR)/src/provider/local_provider.cpp $(ME3_DIR)/src/provider/resolvers.cpp \
           $(ME3_DIR)/src/matter_engine.cpp

# C sources (gcc-compiled to keep extern "C" symbols unmangled) + QuickJS C.
PIPELINE_C = $(MSL_DIR)/src/surface.c $(MSL_DIR)/src/open_particle_surface.c \
             $(MSL_DIR)/src/spatial_hash.c $(MSL_DIR)/src/object_allocator.c \
             $(MSL_DIR)/src/material_registry.c $(MSL_DIR)/src/fat_primitive.c
QJS_C   = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c $(QJS_DIR)/libunicode.c \
          $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c

IMGUI_SRC = $(IMGUI_PATH)/imgui.cpp $(IMGUI_PATH)/imgui_draw.cpp \
            $(IMGUI_PATH)/imgui_tables.cpp $(IMGUI_PATH)/imgui_widgets.cpp \
            $(IMGUI_PATH)/backends/imgui_impl_glfw.cpp \
            $(IMGUI_PATH)/backends/imgui_impl_opengl3.cpp

# ---- Per-object lists (one .o per TU, in a platform build dir, so `make -j`
# can compile them concurrently). Basenames are unique across all source dirs,
# so flattening with $(notdir ...) is collision-free. vpath (below) lets the
# pattern rules find each source by basename.
CXX_FLAGS_BUILD_COMMON = $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB $(INCLUDE_PATHS) -I$(BOX3D_DIR)/include $(QJS_INC) -MMD -MP
CXX_FLAGS_BUILD = $(CXX_FLAGS_BUILD_COMMON) $(AR_CORE_CFLAGS)
CXX_FLAGS_BUILD_WIN = $(CXX_FLAGS_BUILD_COMMON)

ALL_CPP_SRC = $(APP_SRC) $(ME3_CPP) $(MSL_CPP) $(IMGUI_SRC)
CPP_NAMES = $(notdir $(ALL_CPP_SRC))
QJS_NAMES = $(notdir $(QJS_C))
PIPC_NAMES = $(notdir $(PIPELINE_C))

# Windows CPP set: excludes mesh_retopo.cpp (Linux-only via AR_CORE_CPP). The
# empty AR_CORE_CPP on Windows would already make this a no-op — the filter is
# defensive: even if HAVE_AUTOREMESHER is set on the host (Linux build), the
# Windows target must not try to compile mesh_retopo.cpp (needs geogram TBB).
CPP_NAMES_WIN = $(filter-out mesh_retopo.cpp,$(CPP_NAMES))

vpath %.cpp . $(ME3_DIR)/src $(ME3_DIR)/src/render $(ME3_DIR)/src/provider \
             $(MSL_DIR)/src $(IMGUI_PATH) $(IMGUI_PATH)/backends
vpath %.c $(MSL_DIR)/src $(QJS_DIR)

# Linux object sets
L_DIR = build/linux
L_CPP_OBJ  = $(addprefix $(L_DIR)/,$(CPP_NAMES:.cpp=.o))
L_QJS_OBJ  = $(addprefix $(L_DIR)/,$(QJS_NAMES:.c=.o))
L_PIPC_OBJ = $(addprefix $(L_DIR)/,$(PIPC_NAMES:.c=.o))
L_ALL_OBJ  = $(L_CPP_OBJ) $(L_QJS_OBJ) $(L_PIPC_OBJ)

# Windows object sets. Windows uses CPP_NAMES_WIN which excludes mesh_retopo.cpp.
W_DIR = build/windows
W_CPP_OBJ  = $(addprefix $(W_DIR)/,$(CPP_NAMES_WIN:.cpp=.o))
W_QJS_OBJ  = $(addprefix $(W_DIR)/,$(QJS_NAMES:.c=.o))
W_PIPC_OBJ = $(addprefix $(W_DIR)/,$(PIPC_NAMES:.c=.o))
W_ALL_OBJ  = $(W_CPP_OBJ) $(W_QJS_OBJ) $(W_PIPC_OBJ)

$(L_DIR) $(W_DIR):
	mkdir -p $@

# ---- Linux compile rules
$(L_CPP_OBJ): $(L_DIR)/%.o: %.cpp | $(L_DIR)
	$(CC) -c $< -o $@ $(CXX_FLAGS_BUILD)
$(L_QJS_OBJ): $(L_DIR)/%.o: $(QJS_DIR)/%.c | $(L_DIR)
	gcc -c $< -o $@ -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC) -MMD -MP
$(L_PIPC_OBJ): $(L_DIR)/%.o: $(MSL_DIR)/src/%.c | $(L_DIR)
	gcc -c $< -o $@ -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS) -MMD -MP

# Generate embedded shader header before compiling any TU that includes it.
.PHONY: embedded-shaders
embedded-shaders:
	$(MAKE) -C $(ME3_DIR) shaders_gen/embedded_shaders.h

viewer: embedded-shaders shaders shaders_gpu_link $(L_ALL_OBJ)
	$(CC) $(L_ALL_OBJ) -o viewer $(CFLAGS) $(LDFLAGS) $(LDLIBS) $(AR_CORE_LDFLAGS)

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
WIN_CXX     = x86_64-w64-mingw32-g++-posix
WIN_CC      = x86_64-w64-mingw32-gcc
WIN_RAYLIB  = ../Libraries/raylib/build/windows-native/libraylib.a
# Win32 import libs raylib depends on; MUST come AFTER libraylib.a on the link
# line so the static linker can resolve raylib's __imp_* references.
WIN_LIBS    = -lopengl32 -lgdi32 -lwinmm $(BOX3D_DIR)/build-mingw/libbox3d.a

# ---- Windows compile rules
$(W_CPP_OBJ): $(W_DIR)/%.o: %.cpp | $(W_DIR)
	$(WIN_CXX) -c $< -o $@ $(CXX_FLAGS_BUILD_WIN)
$(W_QJS_OBJ): $(W_DIR)/%.o: $(QJS_DIR)/%.c | $(W_DIR)
	$(WIN_CC) -c $< -o $@ -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC) -MMD -MP
$(W_PIPC_OBJ): $(W_DIR)/%.o: $(MSL_DIR)/src/%.c | $(W_DIR)
	$(WIN_CC) -c $< -o $@ -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS) -MMD -MP

windows: win-shaders win-shaders-gpu $(W_ALL_OBJ)
	$(WIN_CXX) $(W_ALL_OBJ) -o viewer.exe $(CFLAGS) -static $(WIN_RAYLIB) $(WIN_LIBS)

-include $(W_ALL_OBJ:.o=.d)

# A Windows .exe can't follow a Linux symlink, so copy MSL's shaders/ next to it.
# Regenerate the processed shader first (see `regen-processed-shader`) so the copy
# is current — otherwise the .exe ships a stale, layout-mismatched shader.
win-shaders: regen-processed-shader
	@rm -rf shaders && cp -r $(MSL_DIR)/shaders shaders

# shaders_gpu/ symlink works for the Linux binary but not Windows .exe.
# Copy shaders_gpu/ from MatterEngine3 for the Windows build.
win-shaders-gpu:
	@[ -d shaders_gpu ] || ln -s $(ME3_DIR)/shaders_gpu shaders_gpu
	@[ -d $(ME3_DIR)/shaders_gpu ] || (echo "ERROR: MatterEngine3/shaders_gpu/ missing" && exit 1)

clean:
	rm -rf viewer viewer.exe shaders shaders_gpu build

.PHONY: viewer windows clean shaders shaders_gpu_link win-shaders win-shaders-gpu regen-processed-shader mingw-box3d embedded-shaders
