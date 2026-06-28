# MatterEngine3 World Viewer (GL build). Reuses the example_world pipeline source
# set (SP-2 host + QuickJS + SP-4 compose + MSL backend) plus raylib GL and Dear
# ImGui. NOT part of the headless build-all test sweep (needs a GL context).

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
             world_composer.cpp local_provider.cpp

# ME3 pipeline + MSL backend C++ sources (verbatim from tests/Makefile EXAMPLE_CPP,
# minus example_world.cpp). Same dir depth, so the ../ paths are unchanged.
PIPELINE_CPP = ../src/part_graph.cpp \
              ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/dsl_bindings.cpp \
              ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/script_rng_binding.cpp \
              ../src/polygon_triangulate.cpp \
              ../src/part_asset_v2.cpp ../src/lod_bake.cpp ../src/world_flatten.cpp ../src/triangle_emit.cpp \
              ../src/sector_grid.cpp ../src/lod_select.cpp \
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
PIPELINE_C_OBJ = surface.o open_particle_surface.o spatial_hash.o object_allocator.o material_registry.o fat_primitive.o
QJS_C   = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c $(QJS_DIR)/libunicode.c \
          $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c
QJS_OBJ = quickjs.o libregexp.o libunicode.o cutils.o xsum.o

IMGUI_SRC = $(IMGUI_PATH)/imgui.cpp $(IMGUI_PATH)/imgui_draw.cpp \
            $(IMGUI_PATH)/imgui_tables.cpp $(IMGUI_PATH)/imgui_widgets.cpp \
            $(IMGUI_PATH)/backends/imgui_impl_glfw.cpp \
            $(IMGUI_PATH)/backends/imgui_impl_opengl3.cpp

viewer: shaders $(VIEWER_SRC) $(PIPELINE_CPP) $(PIPELINE_C) $(QJS_C) $(IMGUI_SRC)
	gcc -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	gcc -c $(PIPELINE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(VIEWER_SRC) $(PIPELINE_CPP) $(IMGUI_SRC) $(QJS_OBJ) $(PIPELINE_C_OBJ) \
	  -o viewer $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) $(LDFLAGS) $(LDLIBS)
	rm -f $(QJS_OBJ) $(PIPELINE_C_OBJ)

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

windows: win-shaders $(VIEWER_SRC) $(PIPELINE_CPP) $(PIPELINE_C) $(QJS_C) $(IMGUI_SRC)
	$(WIN_CC) -c $(QJS_C) -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC)
	$(WIN_CC) -c $(PIPELINE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(WIN_CXX) $(VIEWER_SRC) $(PIPELINE_CPP) $(IMGUI_SRC) $(QJS_OBJ) $(PIPELINE_C_OBJ) \
	  -o viewer.exe $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST $(INCLUDE_PATHS) $(QJS_INC) -static $(WIN_RAYLIB) $(WIN_LIBS)
	rm -f $(QJS_OBJ) $(PIPELINE_C_OBJ)

# A Windows .exe can't follow a Linux symlink, so copy MSL's shaders/ next to it.
# Regenerate the processed shader first (see `regen-processed-shader`) so the copy
# is current — otherwise the .exe ships a stale, layout-mismatched shader.
win-shaders: regen-processed-shader
	@rm -rf shaders && cp -r ../../MatterSurfaceLib/shaders shaders

clean:
	rm -rf viewer viewer.exe shaders

.PHONY: viewer windows clean shaders win-shaders regen-processed-shader
