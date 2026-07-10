# ExplorerDemo — standalone demo app that consumes only the MatterEngine3 public API.
#
# Linux: compile main.cpp + camera_rig.cpp, link libmatter_engine3.a + raylib + GL.
# Windows: MinGW cross-compile; engine+MSL+QJS sources compiled directly (no .a),
#          mirroring MatterViewer/Makefile windows stanza. No imgui, no autoremesher.
#
# Engine include path (Linux): -I../MatterEngine3/include ONLY (no engine internals)
# Engine include path (Windows): also adds private ME3 src/ paths needed for direct
#   source compilation (same as MatterViewer's WIN_INCLUDE_PATHS).
#
# Run from ExplorerDemo/ directory so that cache/ resolves correctly.

JOBS ?= $(shell nproc)
MAKEFLAGS += -j$(JOBS)

CC = g++
CFLAGS = -std=c++17 -O2 -Wall -Wno-missing-braces -Wno-unused-variable \
         -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_43

RAYLIB_PATH = ../Libraries/raylib/src
ME3_DIR     = ../MatterEngine3
MSL_DIR     = ../MatterSurfaceLib
QJS_DIR     = ../Libraries/quickjs-ng
BOX3D_DIR   = ../Libraries/box3d

# Linux include: only the public engine API.
INCLUDE_PATHS = -I$(ME3_DIR)/include \
                -I$(RAYLIB_PATH)

# Windows include: also expose ME3/MSL private src/ headers for direct compilation.
WIN_INCLUDE_PATHS = $(INCLUDE_PATHS) \
                    -I$(ME3_DIR)/src \
                    -I$(ME3_DIR)/src/render \
                    -I$(ME3_DIR)/src/provider \
                    -I$(MSL_DIR)/include \
                    -I$(MSL_DIR)/src \
                    -I$(QJS_DIR) \
                    -I$(BOX3D_DIR)/include

LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11
LDLIBS  = ../Libraries/raylib/build/linux/libraylib.a \
          $(BOX3D_DIR)/libbox3d.a

# ---- Linux: main.cpp + camera_rig.cpp only; links libmatter_engine3.a ----

BUILD_DIR = build/linux

APP_SRC   = main.cpp camera_rig.cpp
CPP_NAMES = $(notdir $(APP_SRC))

$(BUILD_DIR):
	mkdir -p $@

CPP_OBJ = $(addprefix $(BUILD_DIR)/,$(CPP_NAMES:.cpp=.o))

$(CPP_OBJ): $(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CC) -c $< -o $@ $(CFLAGS) $(INCLUDE_PATHS) -MMD -MP

.PHONY: build-engine-lib
build-engine-lib:
	$(MAKE) -C $(ME3_DIR)

# Generate embedded shader header (required by shader_source.cpp in Windows build).
# The Linux build gets it via the library sub-make above.
.PHONY: embedded-shaders
embedded-shaders:
	$(MAKE) -C $(ME3_DIR) shaders_gen/embedded_shaders.h

.PHONY: shaders shaders_gpu_link
shaders:
	@[ -e shaders ] || ln -s ../MatterSurfaceLib/shaders shaders

shaders_gpu_link:
	@[ -e shaders_gpu ] || ln -s $(ME3_DIR)/shaders_gpu shaders_gpu

.DEFAULT_GOAL := explorer

explorer: build-engine-lib shaders shaders_gpu_link $(CPP_OBJ)
	$(CC) $(CPP_OBJ) $(ME3_DIR)/libmatter_engine3.a -o explorer \
	      $(CFLAGS) $(LDFLAGS) $(LDLIBS)

-include $(CPP_OBJ:.o=.d)

# ---- Windows direct-source compile (MinGW cross-compile from WSL/Linux) ----
# Mirrors MatterViewer/Makefile windows stanza:
#   - x86_64-w64-mingw32-g++-posix for C++, -gcc for C
#   - Direct source compile of all engine+MSL+QJS units (no .a for windows)
#   - WIN_ME3_CPP mirrors the viewer's list (retopo_blacklist.cpp added;
#     inotify_watcher.cpp included — guarded by #ifdef __linux__, empty TU on Windows)
#   - No imgui, no autoremesher, no MATTER_HAVE_AUTOREMESHER define
#   - box3d from build-mingw/libbox3d.a (settle path needs box3d symbols)
#   - Static link: -static after object list, WIN32 import libs after raylib

WIN_CXX    = x86_64-w64-mingw32-g++-posix
WIN_CC     = x86_64-w64-mingw32-gcc
WIN_RAYLIB = ../Libraries/raylib/build/windows-native/libraylib.a
WIN_LIBS   = -lopengl32 -lgdi32 -lwinmm $(BOX3D_DIR)/build-mingw/libbox3d.a -lwinpthread

# Engine C++ sources for Windows direct compile.
# Matches MatterEngine3/Makefile ME3_CPP list, plus retopo_blacklist.cpp,
# inotify_watcher.cpp (empty on Windows via #ifdef __linux__ guard).
# Does NOT include MATTER_HAVE_AUTOREMESHER extras (mesh_retopo.cpp, etc.).
WIN_ME3_CPP = $(ME3_DIR)/src/part_graph.cpp \
              $(ME3_DIR)/src/script_host.cpp $(ME3_DIR)/src/dsl_state.cpp \
              $(ME3_DIR)/src/dsl_triangle.cpp $(ME3_DIR)/src/dsl_bindings.cpp \
              $(ME3_DIR)/src/csg_lowering.cpp $(ME3_DIR)/src/module_resolver.cpp \
              $(ME3_DIR)/src/script_rng_binding.cpp \
              $(ME3_DIR)/src/polygon_triangulate.cpp \
              $(ME3_DIR)/src/part_asset_v2.cpp $(ME3_DIR)/src/lod_bake.cpp \
              $(ME3_DIR)/src/world_flatten.cpp $(ME3_DIR)/src/part_flatten.cpp \
              $(ME3_DIR)/src/retopo_blacklist.cpp \
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
              $(ME3_DIR)/src/async_bake.cpp \
              $(ME3_DIR)/src/live_edit.cpp \
              $(ME3_DIR)/src/live_edit_prod.cpp \
              $(ME3_DIR)/src/inotify_watcher.cpp \
              $(ME3_DIR)/src/part_graph_snapshot.cpp \
              $(ME3_DIR)/src/refine_controller.cpp \
              $(ME3_DIR)/src/matter_engine.cpp

WIN_MSL_CPP = $(MSL_DIR)/src/cluster.cpp $(MSL_DIR)/src/cell.cpp \
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
              $(MSL_DIR)/src/lattice.cpp $(MSL_DIR)/src/particle_culling.cpp

WIN_PIPELINE_C = $(MSL_DIR)/src/surface.c $(MSL_DIR)/src/open_particle_surface.c \
                 $(MSL_DIR)/src/spatial_hash.c $(MSL_DIR)/src/object_allocator.c \
                 $(MSL_DIR)/src/material_registry.c $(MSL_DIR)/src/fat_primitive.c

QJS_C = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c $(QJS_DIR)/libunicode.c \
        $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c

# Application sources (no imgui for ExplorerDemo).
WIN_APP_SRC = main.cpp camera_rig.cpp

# Per-object lists for Windows build.
W_DIR = build/windows

WIN_ALL_CPP_SRC  = $(WIN_APP_SRC) $(WIN_ME3_CPP) $(WIN_MSL_CPP)
CPP_NAMES_WIN    = $(notdir $(WIN_ALL_CPP_SRC))
QJS_NAMES        = $(notdir $(QJS_C))
PIPC_NAMES_WIN   = $(notdir $(WIN_PIPELINE_C))

W_CPP_OBJ  = $(addprefix $(W_DIR)/,$(CPP_NAMES_WIN:.cpp=.o))
W_QJS_OBJ  = $(addprefix $(W_DIR)/,$(QJS_NAMES:.c=.o))
W_PIPC_OBJ = $(addprefix $(W_DIR)/,$(PIPC_NAMES_WIN:.c=.o))
W_ALL_OBJ  = $(W_CPP_OBJ) $(W_QJS_OBJ) $(W_PIPC_OBJ)

# vpath lets the pattern rules find sources by basename.
vpath %.cpp . $(ME3_DIR)/src $(ME3_DIR)/src/render $(ME3_DIR)/src/provider $(MSL_DIR)/src
vpath %.c $(MSL_DIR)/src $(QJS_DIR)

$(W_DIR):
	mkdir -p $@

# Common Windows compile flags (matches MatterViewer CXX_FLAGS_BUILD_WIN_COMMON,
# minus MATTER_HAVE_AUTOREMESHER and imgui paths).
WIN_CXXFLAGS = $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB \
               $(WIN_INCLUDE_PATHS) -MMD -MP

# Windows C++ compile rule (app + ME3 + MSL C++ sources).
$(W_CPP_OBJ): $(W_DIR)/%.o: %.cpp | $(W_DIR)
	$(WIN_CXX) -c $< -o $@ $(WIN_CXXFLAGS)

# Order-only: embedded_shaders.h must exist before shader_source.cpp compiles.
$(W_CPP_OBJ): | embedded-shaders

# Windows QuickJS C compile rule.
$(W_QJS_OBJ): $(W_DIR)/%.o: $(QJS_DIR)/%.c | $(W_DIR)
	$(WIN_CC) -c $< -o $@ -O2 -DCONFIG_VERSION='"0.10.0"' -I$(QJS_DIR) -MMD -MP

# Windows MSL pipeline C compile rule.
$(W_PIPC_OBJ): $(W_DIR)/%.o: $(MSL_DIR)/src/%.c | $(W_DIR)
	$(WIN_CC) -c $< -o $@ -O2 -DPLATFORM_DESKTOP $(WIN_INCLUDE_PATHS) -MMD -MP

# win-shaders-gpu: ensure the shaders_gpu/ symlink resolves (runtime compute shaders).
.PHONY: win-shaders-gpu
win-shaders-gpu:
	@[ -d shaders_gpu ] || ln -s $(ME3_DIR)/shaders_gpu shaders_gpu
	@[ -d $(ME3_DIR)/shaders_gpu ] || (echo "ERROR: MatterEngine3/shaders_gpu/ missing" && exit 1)

windows: win-shaders-gpu $(W_ALL_OBJ)
	$(WIN_CXX) $(W_ALL_OBJ) -o explorer.exe $(CFLAGS) -static $(WIN_RAYLIB) $(WIN_LIBS)

-include $(W_ALL_OBJ:.o=.d)

clean:
	rm -rf explorer explorer.exe build shaders shaders_gpu

.PHONY: explorer windows clean shaders shaders_gpu_link win-shaders-gpu embedded-shaders build-engine-lib
