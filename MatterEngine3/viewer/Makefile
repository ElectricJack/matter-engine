# MatterEngine3 World Viewer (GL build). Reuses the example_world pipeline source
# set (SP-2 host + QuickJS + SP-4 compose + MSL backend) plus raylib GL and Dear
# ImGui. NOT part of the headless build-all test sweep (needs a GL context).

# Per-object parallel build: each TU compiles to its own .o under build/<platform>/
# so `make -j` fans them across cores; header changes are tracked via -MMD -MP
# generated .d files (so incremental rebuilds are correct, not silently stale).
# Parallel by default; override with e.g. `make -j1 windows` to serialize.
JOBS ?= $(shell nproc)
MAKEFLAGS += -j$(JOBS)

CC = g++
CFLAGS = -std=c++17 -Wall -Wno-missing-braces -Wno-unused-variable \
         -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33

RAYLIB_PATH = ../../Libraries/raylib/src
IMGUI_PATH  = ../../Libraries/imgui
QJS_DIR     = ../../Libraries/quickjs-ng
QJS_INC     = -I$(QJS_DIR)

INCLUDE_PATHS = -I../include -I../../MatterSurfaceLib/include -I$(RAYLIB_PATH) \
                -I../viewer -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends \
                -I$(RAYLIB_PATH)/external/glfw/include

LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11
LDLIBS  = ../../Libraries/raylib/build/linux/libraylib.a

# Viewer units (GL-free logic + GL renderer/ui/main).
VIEWER_SRC = main.cpp renderer.cpp ui.cpp \
             world_state.cpp part_store.cpp resolvers.cpp \
             world_composer.cpp local_provider.cpp \
             raster_mesh.cpp raster_composer.cpp probe_texture.cpp

# ME3 pipeline + MSL backend C++ sources (verbatim from tests/Makefile EXAMPLE_CPP,
# minus example_world.cpp). Same dir depth, so the ../ paths are unchanged.
PIPELINE_CPP = ../src/part_graph.cpp \
              ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/dsl_bindings.cpp \
              ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/script_rng_binding.cpp \
              ../src/polygon_triangulate.cpp \
              ../src/part_asset_v2.cpp ../src/lod_bake.cpp ../src/world_flatten.cpp ../src/part_flatten.cpp ../src/triangle_emit.cpp \
              ../src/part_cluster.cpp \
              ../src/sector_grid.cpp ../src/lod_select.cpp \
              ../src/world_lights.cpp ../src/probe_volume.cpp \
              ../src/world_tracer.cpp ../src/probe_bake.cpp \
              ../../MatterSurfaceLib/src/cluster.cpp ../../MatterSurfaceLib/src/cell.cpp ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
              ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/bvh_analyzer.cpp \
              ../../MatterSurfaceLib/src/mesh_worker_pool.cpp ../../MatterSurfaceLib/src/mesh_build_utils.cpp \
              ../../MatterSurfaceLib/src/meshing_algorithm.cpp ../../MatterSurfaceLib/src/marching_cubes_algorithm.cpp \
              ../../MatterSurfaceLib/src/oriented_cube_algorithm.cpp ../../MatterSurfaceLib/src/vertex_ao.cpp \
              ../../MatterSurfaceLib/src/occupancy.cpp ../../MatterSurfaceLib/src/tlas_manager.cpp ../../MatterSurfaceLib/src/part_asset.cpp \
              ../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp

# C sources (gcc-compiled to keep extern "C" symbols unmangled) + QuickJS C.
PIPELINE_C = ../../MatterSurfaceLib/src/surface.c ../../MatterSurfaceLib/src/open_particle_surface.c \
             ../../MatterSurfaceLib/src/spatial_hash.c ../../MatterSurfaceLib/src/object_allocator.c \
             ../../MatterSurfaceLib/src/material_registry.c ../../MatterSurfaceLib/src/fat_primitive.c
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
CXX_FLAGS_BUILD = $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) -MMD -MP
CPP_NAMES = $(notdir $(VIEWER_SRC) $(PIPELINE_CPP) $(IMGUI_SRC))
QJS_NAMES = $(notdir $(QJS_C))
PIPC_NAMES = $(notdir $(PIPELINE_C))

vpath %.cpp . ../src ../../MatterSurfaceLib/src $(IMGUI_PATH) $(IMGUI_PATH)/backends
vpath %.c ../../MatterSurfaceLib/src $(QJS_DIR)

# Linux object sets
L_DIR = build/linux
L_CPP_OBJ  = $(addprefix $(L_DIR)/,$(CPP_NAMES:.cpp=.o))
L_QJS_OBJ  = $(addprefix $(L_DIR)/,$(QJS_NAMES:.c=.o))
L_PIPC_OBJ = $(addprefix $(L_DIR)/,$(PIPC_NAMES:.c=.o))
L_ALL_OBJ  = $(L_CPP_OBJ) $(L_QJS_OBJ) $(L_PIPC_OBJ)

# Windows object sets
W_DIR = build/windows
W_CPP_OBJ  = $(addprefix $(W_DIR)/,$(CPP_NAMES:.cpp=.o))
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
$(L_PIPC_OBJ): $(L_DIR)/%.o: ../../MatterSurfaceLib/src/%.c | $(L_DIR)
	gcc -c $< -o $@ -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS) -MMD -MP

viewer: shaders $(L_ALL_OBJ)
	$(CC) $(L_ALL_OBJ) -o viewer $(CFLAGS) $(LDFLAGS) $(LDLIBS)

-include $(L_ALL_OBJ:.o=.d)

# Shader path resolves relative to the binary; symlink MSL's shaders/ dir.
# The viewer loads the include-flattened raytrace_tlas_blas_processed.fs, so it
# MUST be regenerated from MSL's .glsl sources first — a stale processed file
# silently mismatches the C++ TLAS texture layout (tiled reads vs single-row).
shaders: regen-processed-shader
	@[ -e shaders ] || ln -s ../../MatterSurfaceLib/shaders shaders

# Rebuild MSL's shader preprocessor and re-flatten the processed fragment shader
# so edits to bvh_tlas_common.glsl / materials.glsl / lighting.glsl take effect.
MSL_SHADERS = ../../MatterSurfaceLib/shaders
regen-processed-shader:
	@g++ -O2 -std=c++14 -o /tmp/me3_shader_preprocessor \
	    ../../MatterSurfaceLib/src/shader_preprocessor.cpp
	@/tmp/me3_shader_preprocessor $(MSL_SHADERS)/raytrace_tlas_blas.fs \
	    $(MSL_SHADERS)/raytrace_tlas_blas_processed.fs

# ---- Native Windows .exe (MinGW cross-compile from WSL/Linux) ----
# Mirrors MatterSurfaceLib's windows-native target: MinGW toolchain, the
# prebuilt windows-native raylib, and Win32 GL/GDI libs, statically linked so
# the .exe is self-contained. Run as MatterEngine3\viewer\viewer.exe on Windows.
WIN_CXX     = x86_64-w64-mingw32-g++-posix
WIN_CC      = x86_64-w64-mingw32-gcc
WIN_RAYLIB  = ../../Libraries/raylib/build/windows-native/libraylib.a
# Win32 import libs raylib depends on; MUST come AFTER libraylib.a on the link
# line so the static linker can resolve raylib's __imp_* references.
WIN_LIBS    = -lopengl32 -lgdi32 -lwinmm

# ---- Windows compile rules
$(W_CPP_OBJ): $(W_DIR)/%.o: %.cpp | $(W_DIR)
	$(WIN_CXX) -c $< -o $@ $(CXX_FLAGS_BUILD)
$(W_QJS_OBJ): $(W_DIR)/%.o: $(QJS_DIR)/%.c | $(W_DIR)
	$(WIN_CC) -c $< -o $@ -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC) -MMD -MP
$(W_PIPC_OBJ): $(W_DIR)/%.o: ../../MatterSurfaceLib/src/%.c | $(W_DIR)
	$(WIN_CC) -c $< -o $@ -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS) -MMD -MP

windows: win-shaders $(W_ALL_OBJ)
	$(WIN_CXX) $(W_ALL_OBJ) -o viewer.exe $(CFLAGS) -static $(WIN_RAYLIB) $(WIN_LIBS)

-include $(W_ALL_OBJ:.o=.d)

# A Windows .exe can't follow a Linux symlink, so copy MSL's shaders/ next to it.
# Regenerate the processed shader first (see `regen-processed-shader`) so the copy
# is current — otherwise the .exe ships a stale, layout-mismatched shader.
win-shaders: regen-processed-shader
	@rm -rf shaders && cp -r ../../MatterSurfaceLib/shaders shaders

clean:
	rm -rf viewer viewer.exe shaders build

.PHONY: viewer windows clean shaders win-shaders regen-processed-shader
