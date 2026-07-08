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
CFLAGS = -std=c++17 -O2 -Wall -Wno-missing-braces -Wno-unused-variable \
         -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_43

RAYLIB_PATH = ../../Libraries/raylib/src
IMGUI_PATH  = ../../Libraries/imgui
QJS_DIR     = ../../Libraries/quickjs-ng
QJS_INC     = -I$(QJS_DIR)

INCLUDE_PATHS = -I../include -I../../MatterSurfaceLib/include -I$(RAYLIB_PATH) \
                -I../viewer -I$(IMGUI_PATH) -I$(IMGUI_PATH)/backends \
                -I$(RAYLIB_PATH)/external/glfw/include

LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11
LDLIBS  = ../../Libraries/raylib/build/linux/libraylib.a
BOX3D_LIB = ../../Libraries/box3d/libbox3d.a
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
AR_CORE_PATH := ../../Libraries/autoremesher_core
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
                     -Wl,-rpath,'$$ORIGIN/../../Libraries/autoremesher_core/thirdparty/tbb/build/$(TBB_BUILD_SUBDIR)' \
                     -Wl,--allow-multiple-definition
  AR_CORE_CPP := ../../MatterSurfaceLib/src/mesh_retopo.cpp
else
  AR_CORE_CFLAGS :=
  AR_CORE_LDFLAGS :=
  AR_CORE_CPP :=
endif

# Test-only sources excluded from VIEWER_SRC so `make viewer` doesn't link them.
TEST_ONLY_SRC = gpu_cull_tests.cpp

# Viewer units (GL-free logic + GL renderer/ui/main).
VIEWER_SRC = main.cpp renderer.cpp ui.cpp \
             world_state.cpp part_store.cpp resolvers.cpp \
             world_composer.cpp local_provider.cpp \
             raster_mesh.cpp raster_composer.cpp probe_texture.cpp \
             gpu_culler.cpp tileset_gl_ctx.cpp tileset_bake_primary.cpp \
             tileset_bake_ao.cpp tileset_provider.cpp

# ME3 pipeline + MSL backend C++ sources (verbatim from tests/Makefile EXAMPLE_CPP,
# minus example_world.cpp). Same dir depth, so the ../ paths are unchanged.
PIPELINE_CPP = ../src/part_graph.cpp \
              ../src/script_host.cpp ../src/dsl_state.cpp ../src/dsl_triangle.cpp ../src/dsl_bindings.cpp \
              ../src/csg_lowering.cpp ../src/module_resolver.cpp ../src/script_rng_binding.cpp \
              ../src/polygon_triangulate.cpp \
              ../src/part_asset_v2.cpp ../src/lod_bake.cpp ../src/world_flatten.cpp ../src/part_flatten.cpp ../src/triangle_emit.cpp \
              ../src/part_cluster.cpp \
              ../src/tileset_placement.cpp ../src/tileset_layout.cpp \
              ../src/tileset_torus_bvh.cpp \
              ../src/tileset_gtex.cpp ../src/tileset_bake_gpu.cpp \
              ../src/sector_grid.cpp ../src/lod_select.cpp \
              ../src/world_lights.cpp ../src/probe_volume.cpp \
              ../src/world_tracer.cpp ../src/probe_bake.cpp \
              ../../MatterSurfaceLib/src/cluster.cpp ../../MatterSurfaceLib/src/cell.cpp ../../MatterSurfaceLib/src/mesh_simplifier.cpp \
              ../../MatterSurfaceLib/src/mesh_indexed.cpp ../../MatterSurfaceLib/src/mesh_transform.cpp \
              ../../MatterSurfaceLib/src/blas_manager.cpp ../../MatterSurfaceLib/src/bvh.cpp ../../MatterSurfaceLib/src/bvh_analyzer.cpp \
              ../../MatterSurfaceLib/src/mesh_worker_pool.cpp ../../MatterSurfaceLib/src/mesh_build_utils.cpp \
              ../../MatterSurfaceLib/src/meshing_algorithm.cpp ../../MatterSurfaceLib/src/marching_cubes_algorithm.cpp \
              ../../MatterSurfaceLib/src/oriented_cube_algorithm.cpp ../../MatterSurfaceLib/src/vertex_ao.cpp \
              ../../MatterSurfaceLib/src/occupancy.cpp ../../MatterSurfaceLib/src/tlas_manager.cpp ../../MatterSurfaceLib/src/part_asset.cpp \
              ../src/tileset_phase.cpp ../src/tileset_phase_gpu.cpp \
              ../src/tileset_bake.cpp ../src/tileset_settle.cpp \
              ../src/tileset_collider.cpp ../src/tileset_part_collider.cpp \
              ../../MatterSurfaceLib/src/lattice.cpp ../../MatterSurfaceLib/src/particle_culling.cpp \
              $(AR_CORE_CPP)

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
# Linux gets -DMATTER_HAVE_AUTOREMESHER + autoremesher_core include path when
# HAVE_AUTOREMESHER is set (see AR_CORE_CFLAGS above). Windows never opts in
# (autoremesher_core hasn't been cross-built for mingw); schemas with the
# opt-in fall through to the streaming path with a one-line warn on Windows.
CXX_FLAGS_BUILD_COMMON = $(CFLAGS) -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB $(INCLUDE_PATHS) -I../../Libraries/box3d/include $(QJS_INC) -MMD -MP
CXX_FLAGS_BUILD = $(CXX_FLAGS_BUILD_COMMON) $(AR_CORE_CFLAGS)
CXX_FLAGS_BUILD_WIN = $(CXX_FLAGS_BUILD_COMMON)
CPP_NAMES = $(notdir $(VIEWER_SRC) $(PIPELINE_CPP) $(IMGUI_SRC))
QJS_NAMES = $(notdir $(QJS_C))
PIPC_NAMES = $(notdir $(PIPELINE_C))
# Windows CPP set: excludes mesh_retopo.cpp (Linux-only via AR_CORE_CPP). The
# empty AR_CORE_CPP on Windows would already make this a no-op — the filter is
# defensive: even if HAVE_AUTOREMESHER is set on the host (Linux build), the
# Windows target must not try to compile mesh_retopo.cpp (needs geogram TBB).
CPP_NAMES_WIN = $(filter-out mesh_retopo.cpp,$(CPP_NAMES))

vpath %.cpp . ../src ../../MatterSurfaceLib/src $(IMGUI_PATH) $(IMGUI_PATH)/backends
vpath %.c ../../MatterSurfaceLib/src $(QJS_DIR)

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

# Test-only objects (compiled with same flags, but NOT part of L_ALL_OBJ so they
# don't pollute the viewer link).
TEST_CPP_NAMES = $(notdir $(TEST_ONLY_SRC))
L_TEST_OBJ = $(addprefix $(L_DIR)/,$(TEST_CPP_NAMES:.cpp=.o))

# ---- Linux compile rules
$(L_CPP_OBJ): $(L_DIR)/%.o: %.cpp | $(L_DIR)
	$(CC) -c $< -o $@ $(CXX_FLAGS_BUILD)
$(L_TEST_OBJ): $(L_DIR)/%.o: %.cpp | $(L_DIR)
	$(CC) -c $< -o $@ $(CXX_FLAGS_BUILD)
$(L_QJS_OBJ): $(L_DIR)/%.o: $(QJS_DIR)/%.c | $(L_DIR)
	gcc -c $< -o $@ -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC) -MMD -MP
$(L_PIPC_OBJ): $(L_DIR)/%.o: ../../MatterSurfaceLib/src/%.c | $(L_DIR)
	gcc -c $< -o $@ -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS) -MMD -MP

viewer: shaders shaders_gpu_link $(L_ALL_OBJ)
	$(CC) $(L_ALL_OBJ) -o viewer $(CFLAGS) $(LDFLAGS) $(LDLIBS) $(AR_CORE_LDFLAGS)

# GPU cull parity test binary: same objects as viewer, minus main.o, plus gpu_cull_tests.o.
GPU_TEST_OBJ = $(filter-out $(L_DIR)/main.o,$(L_ALL_OBJ)) $(L_TEST_OBJ)
gpu-tests: shaders shaders_gpu_link $(GPU_TEST_OBJ)
	$(CC) $(GPU_TEST_OBJ) -o gpu_tests $(CFLAGS) $(LDFLAGS) $(LDLIBS) $(AR_CORE_LDFLAGS)

# tileset headless GPU tests (Task 3..7). Same object set as gpu-tests + the
# tileset_gpu_tests.cpp driver, minus main.o and gpu_cull_tests.o.
L_TSET_GPU_TEST_OBJ = $(L_DIR)/tileset_gpu_tests.o
$(L_TSET_GPU_TEST_OBJ): tileset_gpu_tests.cpp | $(L_DIR)
	$(CC) -c $< -o $@ $(CXX_FLAGS_BUILD)

TILESET_GPU_TEST_OBJ = $(filter-out $(L_DIR)/main.o $(L_DIR)/gpu_cull_tests.o,$(L_ALL_OBJ)) $(L_TSET_GPU_TEST_OBJ)
tileset-gpu-tests: shaders shaders_gpu_link $(TILESET_GPU_TEST_OBJ)
	$(CC) $(TILESET_GPU_TEST_OBJ) -o tileset_gpu_tests $(CFLAGS) $(LDFLAGS) $(AR_CORE_LDFLAGS) $(LDLIBS)

run-tilesetgpu: tileset-gpu-tests
	./tileset_gpu_tests

# tileset seam-invariance tests (Task 7). Same object set as tileset-gpu-tests
# + tileset_seam_tests.cpp driver, minus main.o and gpu_cull_tests.o.
L_TSET_SEAM_OBJ = $(L_DIR)/tileset_seam_tests.o
$(L_TSET_SEAM_OBJ): tileset_seam_tests.cpp | $(L_DIR)
	$(CC) -c $< -o $@ $(CXX_FLAGS_BUILD)

TILESET_SEAM_TEST_OBJ = $(filter-out $(L_DIR)/main.o $(L_DIR)/gpu_cull_tests.o,$(L_ALL_OBJ)) $(L_TSET_SEAM_OBJ)
tileset-seam-tests: shaders shaders_gpu_link $(TILESET_SEAM_TEST_OBJ)
	$(CC) $(TILESET_SEAM_TEST_OBJ) -o tileset_seam_tests $(CFLAGS) $(LDFLAGS) $(AR_CORE_LDFLAGS) $(LDLIBS)

run-tilesetseam: tileset-seam-tests
	./tileset_seam_tests

# tileset_provider headless tests (Phase 4 Task 1). Same object set as
# tileset-gpu-tests + the tileset_provider_tests.cpp driver, minus main.o.
L_TSET_PROV_TEST_OBJ = $(L_DIR)/tileset_provider_tests.o
$(L_TSET_PROV_TEST_OBJ): tileset_provider_tests.cpp | $(L_DIR)
	$(CC) -c $< -o $@ $(CXX_FLAGS_BUILD)

TILESET_PROV_TEST_OBJ = $(filter-out $(L_DIR)/main.o $(L_DIR)/gpu_cull_tests.o,$(L_ALL_OBJ)) $(L_TSET_PROV_TEST_OBJ)
tileset-provider-tests: shaders shaders_gpu_link $(TILESET_PROV_TEST_OBJ)
	$(CC) $(TILESET_PROV_TEST_OBJ) -o tileset_provider_tests $(CFLAGS) $(LDFLAGS) $(AR_CORE_LDFLAGS) $(LDLIBS)

run-tilesetprovider: tileset-provider-tests
	./tileset_provider_tests

# tileset load integration tests (Phase 4 Task 4). LocalProvider connect() path:
# world.manifest tileset root -> run_tileset_phase(opts) -> load_slot.
L_TSET_LOAD_TEST_OBJ = $(L_DIR)/tileset_load_tests.o
$(L_TSET_LOAD_TEST_OBJ): tileset_load_tests.cpp | $(L_DIR)
	$(CC) -c $< -o $@ $(CXX_FLAGS_BUILD)

TILESET_LOAD_TEST_OBJ = $(filter-out $(L_DIR)/main.o $(L_DIR)/gpu_cull_tests.o,$(L_ALL_OBJ)) $(L_TSET_LOAD_TEST_OBJ)
tileset-load-tests: shaders shaders_gpu_link $(TILESET_LOAD_TEST_OBJ)
	$(CC) $(TILESET_LOAD_TEST_OBJ) -o tileset_load_tests $(CFLAGS) $(LDFLAGS) $(AR_CORE_LDFLAGS) $(LDLIBS)

run-tilesetload: tileset-load-tests
	./tileset_load_tests

-include $(L_ALL_OBJ:.o=.d)

# Shader path resolves relative to the binary; symlink MSL's shaders/ dir.
# The viewer loads the include-flattened raytrace_tlas_blas_processed.fs, so it
# MUST be regenerated from MSL's .glsl sources first — a stale processed file
# silently mismatches the C++ TLAS texture layout (tiled reads vs single-row).
shaders: regen-processed-shader
	@[ -e shaders ] || ln -s ../../MatterSurfaceLib/shaders shaders
	@missing=""; \
	 for f in shaders/raster.vs shaders/raster.fs shaders/raytrace_tlas_blas_processed.fs; do \
	     [ -e "$$f" ] || missing="$$missing $$f"; \
	 done; \
	 if [ -n "$$missing" ]; then \
	     echo "ERROR: required shader files missing:$$missing"; \
	     exit 1; \
	 fi

# shaders_gpu/ is a real directory committed to the repo; it lives next to the
# binary on Linux (no copy needed, the binary resolves relative paths from its
# own directory).  Just ensure it exists (it always does after checkout).
shaders_gpu_link:
	@[ -d shaders_gpu ] || (echo "ERROR: shaders_gpu/ directory missing" && exit 1)

# Rebuild MSL's shader preprocessor and re-flatten the processed fragment shader
# so edits to bvh_tlas_common.glsl / materials.glsl / lighting.glsl take effect.
MSL_SHADERS = ../../MatterSurfaceLib/shaders
regen-processed-shader:
	@g++ -O2 -std=c++14 -o /tmp/me3_shader_preprocessor \
	    ../../MatterSurfaceLib/src/shader_preprocessor.cpp
	@/tmp/me3_shader_preprocessor $(MSL_SHADERS)/raytrace_tlas_blas.fs \
	    $(MSL_SHADERS)/raytrace_tlas_blas_processed.fs

# ---- MinGW cross-compile of box3d (Linux → Windows) ----
# box3d/src/timer.c includes <Windows.h> (capital W) which is case-sensitive on
# Linux. We create a local wrapper header and compile timer.c separately.
# Run `make mingw-box3d` once; the resulting build-mingw/libbox3d.a is committed.
BOX3D_DIR     = ../../Libraries/box3d
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
# the .exe is self-contained. Run as MatterEngine3\viewer\viewer.exe on Windows.
WIN_CXX     = x86_64-w64-mingw32-g++-posix
WIN_CC      = x86_64-w64-mingw32-gcc
WIN_RAYLIB  = ../../Libraries/raylib/build/windows-native/libraylib.a
# Win32 import libs raylib depends on; MUST come AFTER libraylib.a on the link
# line so the static linker can resolve raylib's __imp_* references.
WIN_LIBS    = -lopengl32 -lgdi32 -lwinmm ../../Libraries/box3d/build-mingw/libbox3d.a

# ---- Windows compile rules
$(W_CPP_OBJ): $(W_DIR)/%.o: %.cpp | $(W_DIR)
	$(WIN_CXX) -c $< -o $@ $(CXX_FLAGS_BUILD_WIN)
$(W_QJS_OBJ): $(W_DIR)/%.o: $(QJS_DIR)/%.c | $(W_DIR)
	$(WIN_CC) -c $< -o $@ -O2 -DCONFIG_VERSION='"0.10.0"' $(QJS_INC) -MMD -MP
$(W_PIPC_OBJ): $(W_DIR)/%.o: ../../MatterSurfaceLib/src/%.c | $(W_DIR)
	$(WIN_CC) -c $< -o $@ -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS) -MMD -MP

windows: win-shaders win-shaders-gpu $(W_ALL_OBJ)
	$(WIN_CXX) $(W_ALL_OBJ) -o viewer.exe $(CFLAGS) -static $(WIN_RAYLIB) $(WIN_LIBS)

-include $(W_ALL_OBJ:.o=.d)

# A Windows .exe can't follow a Linux symlink, so copy MSL's shaders/ next to it.
# Regenerate the processed shader first (see `regen-processed-shader`) so the copy
# is current — otherwise the .exe ships a stale, layout-mismatched shader.
win-shaders: regen-processed-shader
	@rm -rf shaders && cp -r ../../MatterSurfaceLib/shaders shaders

# shaders_gpu/ is a real directory committed to the repo; it lives next to viewer.exe
# already (viewer.exe is output to the same viewer/ dir, same as on Linux).
# No copy needed: the .exe resolves "shaders_gpu/cull.comp" relative to cwd, which is
# this directory when launched from MatterEngine3\viewer\ on Windows.
win-shaders-gpu:
	@[ -d shaders_gpu ] || (echo "ERROR: shaders_gpu/ directory missing" && exit 1)

clean:
	rm -rf viewer viewer.exe gpu_tests tileset_gpu_tests tileset_seam_tests tileset_provider_tests tileset_load_tests shaders build

.PHONY: viewer windows gpu-tests tileset-gpu-tests run-tilesetgpu tileset-seam-tests run-tilesetseam tileset-provider-tests run-tilesetprovider tileset-load-tests run-tilesetload clean shaders shaders_gpu_link win-shaders win-shaders-gpu regen-processed-shader mingw-box3d
