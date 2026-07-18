# Makefile for MatterEngine3 (script host + voxel-CSG bake library).
#
# MatterEngine3 is a library sub-project: it has no application `main` of its own,
# so `make` builds a static archive (libmatter_engine3.a) that bundles the full
# engine — script host, DSL/CSG bake pipeline, part-graph, LOD flatten/select,
# render/provider/facade, MatterSurfaceLib backend, and vendored QuickJS-ng.
# MatterViewer links this single archive plus raylib and (optionally) autoremesher.
#
# MatterEngine3 consumes the MatterSurfaceLib prototype READ-ONLY:
#   new headers via -Iinclude, consumed prototype headers via
#   -I../MatterSurfaceLib/include. QuickJS-ng is vendored under
#   ../Libraries/quickjs-ng and compiled with gcc so its extern "C" symbols stay
#   unmangled. Per CLAUDE.md memory: after struct/header changes, prefer
#   `make clean && make` to avoid stale objects.
#
# GRAPHICS_API rationale (I1): the old MatterEngine3/viewer/Makefile always used
# GRAPHICS_API_OPENGL_43 for the render sources (renderer.cpp, raster_composer.cpp,
# etc.) — confirmed by `git show 75d8e7f:MatterViewer/Makefile | grep GRAPHICS_API`.
# GL 4.3 is required for compute shaders (cull.comp, hiz_downsample.comp, etc.)
# used by GpuCuller and the tileset bake pipeline. OPENGL_33 would silently omit
# gl*Compute* API surface from rlgl.h, breaking any compute-shader dispatch.
# Keeping OPENGL_43 here matches the viewer and GPU tests; MatterEngine3/tests/Makefile
# now also uses OPENGL_43 in its GPU-test CFLAGS so the same render sources see
# the same GL define everywhere they are compiled.
#
# Caller can inject extra flags via EXTRA_CFLAGS / EXTRA_INCLUDE:
#   make -C MatterEngine3 EXTRA_CFLAGS="-DMATTER_HAVE_AUTOREMESHER ..." \
#        EXTRA_INCLUDE="-I../Libraries/autoremesher_core/include"
# MatterViewer/Makefile does this when libautoremesher_core.a is present so that
# part_flatten.cpp compiles with the retopo hook enabled.
#
# When MATTER_HAVE_AUTOREMESHER is passed via EXTRA_CFLAGS, the caller must also
# set EXTRA_RETOPO_CPP to point at MatterSurfaceLib/src/mesh_retopo.cpp so the
# retopo implementation object is included in the archive.

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

PLATFORM = PLATFORM_DESKTOP
GRAPHICS = GRAPHICS_API_OPENGL_43

ifeq ($(UNAME_S),Darwin)
    ifeq ($(UNAME_M),arm64)
        MACOS_VERSION = 11.0
    else
        MACOS_VERSION = 10.15
    endif
endif

CC      = g++
CFLAGS  = -std=c++17 -Wall -Wno-missing-braces -Wno-unused-variable -O2 -D$(PLATFORM) -D$(GRAPHICS) \
          -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB $(EXTRA_CFLAGS)
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -mmacosx-version-min=$(MACOS_VERSION)
endif

RAYLIB_PATH = ../Libraries/raylib/src
MSL_DIR     = ../MatterSurfaceLib
PFL_DIR     = ../ParticleFlowLib
FLECS_DIR   = ../Libraries/flecs

# Include directories: NEW MatterEngine3 headers, consumed MatterSurfaceLib
# headers, vendored QuickJS-ng, then raylib (for the BLAS/Tri/TriEx types).
QJS_DIR       = ../Libraries/quickjs-ng
BOX3D_DIR     = ../Libraries/box3d
BOX3D_LIB     = $(BOX3D_DIR)/libbox3d.a
INCLUDE_PATHS = -Iinclude -Isrc -Isrc/render -Isrc/provider \
                -I$(MSL_DIR)/include -I$(MSL_DIR)/src \
                -I$(QJS_DIR) -I$(RAYLIB_PATH) -I$(BOX3D_DIR)/include \
                -I$(PFL_DIR)/include \
                -I$(FLECS_DIR) \
                -I../Libraries/Vulkan-Headers/include $(EXTRA_INCLUDE)

# Vendored QuickJS-ng engine. v0.10.0 needs xsum.c (xsum_small_* symbols) beyond
# the core four. Compiled via gcc (C) so the embedding symbols stay unmangled.
QJS_C   = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c \
          $(QJS_DIR)/libunicode.c $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c
QJS_OBJ = quickjs.o libregexp.o libunicode.o cutils.o xsum.o

# Flecs is C17 and must never be compiled by the C++ rule below.
FLECS_C   = $(FLECS_DIR)/flecs.c
FLECS_OBJ = flecs.o

# MatterEngine3 C++ sources: full pipeline — bake/script/DSL/CSG, part-graph,
# LOD flatten/select, tileset pipeline, render, provider, and facade.
ME3_CPP = src/script_host.cpp src/dsl_state.cpp src/dsl_bindings.cpp \
          src/dsl_triangle.cpp src/triangle_emit.cpp src/polygon_triangulate.cpp \
          src/csg_lowering.cpp src/module_resolver.cpp src/script_rng_binding.cpp \
          src/part_asset_v2.cpp src/part_graph.cpp src/part_graph_snapshot.cpp \
          src/live_edit.cpp src/live_edit_prod.cpp src/inotify_watcher.cpp \
          src/lod_bake.cpp src/world_flatten.cpp src/part_flatten.cpp \
          src/retopo_blacklist.cpp \
          src/modifier_apply.cpp \
          src/sector_grid.cpp src/lod_select.cpp \
          src/world_lights.cpp \
          src/world_tracer.cpp \
          src/part_cluster.cpp \
          src/tileset_layout.cpp src/tileset_collider.cpp src/tileset_settle.cpp \
          src/tileset_placement.cpp src/tileset_part_collider.cpp src/tileset_bake.cpp \
          src/tileset_phase.cpp src/tileset_phase_gpu.cpp \
          src/tileset_gtex.cpp src/tileset_torus_bvh.cpp \
          src/tileset_bake_primary.cpp src/tileset_bake_ao.cpp src/tileset_bake_gpu.cpp \
          src/shader_source.cpp \
          src/render/renderer.cpp src/render/raster_composer.cpp src/render/raster_mesh.cpp \
          src/render/matrix_math.cpp src/render/frame_matrices.cpp src/render/vk_temporal.cpp src/render/vk_gi_math.cpp src/render/vk_lighting_controls.cpp \
          src/render/part_store.cpp src/render/world_composer.cpp src/render/world_state.cpp \
          src/render/gpu_culler.cpp \
          src/render/tileset_provider.cpp src/render/tileset_gl_ctx.cpp \
          src/provider/local_provider.cpp src/provider/resolvers.cpp \
          src/ecs/ecs_runtime.cpp src/ecs/physics_context.cpp src/ecs/physics_shapes.cpp src/ecs/physics_systems.cpp src/ecs/transform_system.cpp \
          src/matter_engine.cpp \
          src/refine_controller.cpp \
          src/async_bake.cpp \
          src/resolve_cache.cpp \
          src/terrain_field.cpp \
          src/terrain_mesher.cpp \
          src/sector_streamer.cpp

# MatterSurfaceLib C++ backend sources compiled into the archive so MatterViewer
# need not compile them directly (no libmsl.a). Same set as the old viewer's MSL_CPP
# minus mesh_retopo.cpp (handled separately via EXTRA_RETOPO_CPP when AR is present).
MSL_CPP = $(MSL_DIR)/src/cluster.cpp $(MSL_DIR)/src/cell.cpp \
          $(MSL_DIR)/src/mesh_simplifier.cpp $(MSL_DIR)/src/mesh_indexed.cpp \
          $(MSL_DIR)/src/mesh_smooth.cpp \
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
          $(EXTRA_RETOPO_CPP)

# MSL C sources: compiled via gcc to keep extern "C" symbols unmangled.
MSL_C   = $(MSL_DIR)/src/surface.c $(MSL_DIR)/src/open_particle_surface.c \
          $(MSL_DIR)/src/spatial_hash.c $(MSL_DIR)/src/object_allocator.c \
          $(MSL_DIR)/src/material_registry.c $(MSL_DIR)/src/fat_primitive.c
MSL_C_OBJ = surface.o open_particle_surface.o spatial_hash.o \
            object_allocator.o material_registry.o fat_primitive.o

ME3_OBJ = script_host.o dsl_state.o dsl_bindings.o \
          pf_math.o pf_sim.o pf_fields.o pf_path_recorder.o pf_bindings.o \
          dsl_triangle.o triangle_emit.o polygon_triangulate.o \
          csg_lowering.o module_resolver.o script_rng_binding.o \
          part_asset_v2.o part_graph.o part_graph_snapshot.o \
          live_edit.o live_edit_prod.o inotify_watcher.o \
          lod_bake.o world_flatten.o part_flatten.o \
          retopo_blacklist.o \
          modifier_apply.o \
          sector_grid.o lod_select.o \
          world_lights.o \
          world_tracer.o \
          part_cluster.o \
          tileset_layout.o tileset_collider.o \
          tileset_settle.o tileset_placement.o tileset_part_collider.o tileset_bake.o \
          tileset_phase.o tileset_phase_gpu.o \
          tileset_gtex.o tileset_torus_bvh.o tileset_bake_primary.o \
          tileset_bake_ao.o tileset_bake_gpu.o shader_source.o \
          renderer.o raster_composer.o raster_mesh.o matrix_math.o frame_matrices.o vk_temporal.o vk_gi_math.o vk_lighting_controls.o \
          part_store.o world_composer.o world_state.o \
          gpu_culler.o tileset_provider.o tileset_gl_ctx.o \
          local_provider.o resolvers.o \
          ecs_runtime.o physics_context.o physics_shapes.o physics_systems.o transform_system.o \
          matter_engine.o \
          refine_controller.o \
          async_bake.o \
          resolve_cache.o \
          terrain_field.o \
          terrain_mesher.o \
          sector_streamer.o

# MSL C++ object names (basenames; same flat-object model as ME3_OBJ).
MSL_OBJ = cluster.o cell.o mesh_simplifier.o mesh_indexed.o mesh_smooth.o mesh_transform.o \
          blas_manager.o bvh.o bvh_analyzer.o \
          mesh_worker_pool.o mesh_build_utils.o meshing_algorithm.o \
          marching_cubes_algorithm.o oriented_cube_algorithm.o vertex_ao.o \
          occupancy.o tlas_manager.o part_asset.o \
          lattice.o particle_culling.o $(if $(EXTRA_RETOPO_CPP),mesh_retopo.o)

LIB = libmatter_engine3.a

# Shader embedding codegen (Stage 1a).
# Base shader list from the brief table + glsl includes found in .fs/.comp sources.
SHADER_LOGICAL := shaders/raster.vs shaders/raster.fs shaders/tileset_sampling.glsl \
    shaders/raytrace_tlas_blas_processed.fs \
    shaders/materials.glsl shaders/bvh_tlas_common.glsl shaders/lighting.glsl \
    shaders_gpu/cull.comp shaders_gpu/hiz_downsample.comp \
    shaders_gpu/raster_gpu_driven.vs shaders_gpu/tileset_bake_primary.comp \
    shaders_gpu/tileset_bake_ao.comp
# SHADER_FILES resolves logical paths into real filesystem paths under MatterEngine3/:
# shaders/ is a symlink -> ../MatterSurfaceLib/shaders; shaders_gpu/ is a real dir.
SHADER_FILES := $(SHADER_LOGICAL)

.PHONY: all clean test vulkan-spirv

# all MUST be the first target so it is the default goal.
all: $(LIB)

shaders_gen/embedded_shaders.h: tools/embed_shaders.py $(SHADER_FILES)
	mkdir -p shaders_gen
	python3 tools/embed_shaders.py $@ . $(SHADER_LOGICAL)

GLSLC ?= glslc
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

build/shaders_vk/%.spv: shaders_vk/%
	@mkdir -p $(dir $@)
	$(GLSLC) --target-env=vulkan1.3 -O -Ishaders_vk $< -o $@

build/shaders_vk/raster.vert.spv build/shaders_vk/gbuffer.frag.spv: \
    shaders_vk/material_common.glsl

build/shaders_vk/rt_surface.rchit.spv build/shaders_vk/rt_visibility.rahit.spv \
    build/shaders_vk/rt_radiance.rmiss.spv \
    build/shaders_vk/rt_lighting.rgen.spv \
    build/shaders_vk/rt_surface_test.rgen.spv: \
    shaders_vk/rt_surface_common.glsl

build/shaders_vk/gi_temporal.comp.spv build/shaders_vk/gi_atrous.comp.spv: \
    shaders_vk/gi_common.glsl

shaders_gen/embedded_spirv.h: $(VK_SPV) tools/embed_spirv.py
	python3 tools/embed_spirv.py $@ $(VK_SPV)

vulkan-spirv: shaders_gen/embedded_spirv.h

# Per-file pattern rules enable safe parallel compilation with `make -j`.
# vpath directs Make to search source directories by basename.
vpath %.cpp src src/render src/provider src/ecs $(MSL_DIR)/src $(PFL_DIR)/src
vpath %.c $(QJS_DIR) $(MSL_DIR)/src $(FLECS_DIR)

# Flecs C source (gcc C17, never g++).
$(FLECS_OBJ): $(FLECS_C)
	gcc -c $< -o $@ -std=c17 -O2 -I$(FLECS_DIR)

# QuickJS C sources (gcc, extern "C" safe).
%.o: $(QJS_DIR)/%.c
	gcc -c $< -o $@ -O2 -DCONFIG_VERSION='"0.10.0"' -I$(QJS_DIR)

# MSL C sources (gcc, extern "C" safe).
%.o: $(MSL_DIR)/src/%.c
	gcc -c $< -o $@ -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)

# EXTRA_CFLAGS changes are invisible to make's dependency graph (objects look
# up-to-date even though they were compiled with different defines, e.g.
# MATTER_HAVE_AUTOREMESHER). Every C++ object depends on .lib_flags, which is
# rewritten only when the flag set differs — so a flag change forces a full
# recompile while same-flag builds stay incremental.
.PHONY: FORCE
FORCE:
.lib_flags: FORCE
	@[ "`cat $@ 2>/dev/null`" = '$(EXTRA_CFLAGS)' ] || printf '%s' '$(EXTRA_CFLAGS)' > $@

# ME3 + MSL C++ sources (each source file gets its own .o via pattern rule).
# ME3 objects depend on the generated shader header.
$(ME3_OBJ): shaders_gen/embedded_shaders.h
$(ME3_OBJ) $(MSL_OBJ): .lib_flags
%.o: %.cpp
	$(CC) -c $< -o $@ $(CFLAGS) $(INCLUDE_PATHS)

$(BOX3D_LIB):
	$(MAKE) -C $(BOX3D_DIR)

$(LIB): $(QJS_OBJ) $(ME3_OBJ) $(MSL_OBJ) $(MSL_C_OBJ) $(FLECS_OBJ) $(BOX3D_LIB)
	ar rcs $(LIB) $(QJS_OBJ) $(ME3_OBJ) $(MSL_OBJ) $(MSL_C_OBJ) $(FLECS_OBJ)

# Delegate to the test suite (headless, GL-free host + raylib-linked BLAS path).
test:
	$(MAKE) -C tests run-ecs
	$(MAKE) -C tests run-physics
	$(MAKE) -C tests run-partv2
	$(MAKE) -C tests run-script

clean:
	rm -f $(LIB) $(QJS_OBJ) $(ME3_OBJ) $(MSL_OBJ) $(MSL_C_OBJ) $(FLECS_OBJ) .lib_flags
	$(MAKE) -C tests clean
