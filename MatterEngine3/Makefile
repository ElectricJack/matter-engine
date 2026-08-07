# Makefile for MatterEngine3 (script host + voxel-CSG bake library).
#
# MatterEngine3 is a library sub-project: it has no application `main` of its own,
# so `make` builds a static archive (libmatter_engine3.a) that bundles the full
# engine — script host, DSL/CSG bake pipeline, part-graph, LOD flatten/select,
# render/provider/facade, MatterSurfaceLib backend, and vendored QuickJS-ng.
# MatterEditor links this single archive plus raylib and autoremesher.
#
# MatterEngine3 consumes the MatterSurfaceLib prototype READ-ONLY:
#   new headers via -Iinclude, consumed prototype headers via
#   -I../libs/MatterSurfaceLib/include. QuickJS-ng is vendored under
#   ../third_party/quickjs-ng and compiled with gcc so its extern "C" symbols stay
#   unmangled. Per CLAUDE.md memory: after struct/header changes, prefer
#   `make clean && make` to avoid stale objects.
#
# GRAPHICS_API rationale (I1): the old MatterEngine3/viewer/Makefile always used
# GRAPHICS_API_OPENGL_43 for the render sources — confirmed by
# `git show 75d8e7f:MatterViewer/Makefile | grep GRAPHICS_API`. GL 4.3 is
# required for compute shaders (cull.comp, hiz_downsample.comp, etc.) used by
# the tileset bake pipeline. OPENGL_33 would silently omit gl*Compute* API
# surface from rlgl.h, breaking any compute-shader dispatch. Keeping OPENGL_43
# here matches the GPU tests; MatterEngine3/tests/Makefile now also uses
# OPENGL_43 in its GPU-test CFLAGS so the same render sources see the same GL
# define everywhere they are compiled.
#
# Phase 5a (tech-debt.md §6) deleted the GL renderer/raster-composer/GpuCuller
# path (src/render/renderer.*, raster_composer.*, gpu_culler.*) outright, and
# this build now always defines MATTER_VULKAN_ONLY (see CFLAGS below) so
# matter_engine.cpp's facade no longer references those types. GRAPHICS_API_
# OPENGL_43 remains for the pieces that still are GL (tileset_provider.cpp's
# atlas compute path, raylib POD types Phase 4 has not yet migrated).
#
# Caller can inject extra flags via EXTRA_CFLAGS / EXTRA_INCLUDE:
#   make -C MatterEngine3 EXTRA_CFLAGS="-DMATTER_HAVE_AUTOREMESHER ..." \
#        EXTRA_INCLUDE="-I../third_party/autoremesher_core/include"
# MatterEditor/Makefile does this whenever retopo is enabled, so that
# modifier_apply.cpp compiles with its retopo branch live. (The retopo hook
# used to sit in part_flatten.cpp; ea579ba6 moved it to the modifier-region
# path in modifier_apply.cpp.)
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
NM      = nm
# MATTER_VULKAN_ONLY (Phase 5a, GL-path deletion): matter_engine.cpp's facade has
# carried #ifndef MATTER_VULKAN_ONLY / #ifdef MATTER_VULKAN_VIEWER branches since
# before this phase -- the Windows/Vulkan editor.exe build has always defined
# MATTER_VULKAN_ONLY (MatterEditor/Makefile's CXX_FLAGS_BUILD_WIN_COMMON) and
# compiled out the #ifndef blocks that reference viewer::Renderer/GpuCuller/
# RasterComposer. Those classes are now deleted outright (src/render/renderer.*,
# raster_composer.*, gpu_culler.* -- tech-debt.md §6), so this plain archive
# build must define the same macro or matter_engine.cpp fails to compile.
# MATTER_VULKAN_VIEWER is deliberately NOT defined here: this build doesn't
# compile the vk_*.cpp sources (those live only in MatterEditor/Makefile's
# WIN_ME3_CPP), so matter_engine.cpp falls through to its existing
# "#ifndef MATTER_VULKAN_VIEWER" stubs (already-tested headless/no-viewer path).
CFLAGS  = -std=c++17 -Wall -Wno-missing-braces -Wno-unused-variable -O2 -D$(PLATFORM) -D$(GRAPHICS) \
          -DMATTER_HAVE_SCRIPT_HOST -DTILESET_GTEX_USE_RAYLIB_STB -DMATTER_VULKAN_ONLY $(EXTRA_CFLAGS)
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -mmacosx-version-min=$(MACOS_VERSION)
endif

RAYLIB_PATH = ../third_party/raylib/src
MSL_DIR     = ../libs/MatterSurfaceLib
PFL_DIR     = ../libs/ParticleFlowLib
FLECS_DIR   = ../third_party/flecs
# Shared source-of-truth libraries (compiled from source, not copied).
SQL_DIR     = ../libs/SpatialQueryLib
MEMLIB_DIR  = ../libs/MemoryLib
MATHLIB_DIR = ../libs/MathLib
MCL_DIR     = ../libs/MeshChartingLib
PROFILE_DIR = ../libs/ProfileLib
OZZ_DIR     = ../third_party/ozz-animation
OZZ_BUILD_DIR = $(OZZ_DIR)/build/matter
OZZ_INCLUDE = $(OZZ_DIR)/include
OZZ_BASE_LIB = $(OZZ_BUILD_DIR)/src/base/libozz_base.a
OZZ_ANIMATION_LIB = $(OZZ_BUILD_DIR)/src/animation/runtime/libozz_animation.a
OZZ_OFFLINE_LIB = $(OZZ_BUILD_DIR)/src/animation/offline/libozz_animation_offline.a
OZZ_RUNTIME_LIBS = $(OZZ_ANIMATION_LIB) $(OZZ_BASE_LIB)
OZZ_OFFLINE_LIBS = $(OZZ_OFFLINE_LIB) $(OZZ_RUNTIME_LIBS)

# Include directories: NEW MatterEngine3 headers, consumed MatterSurfaceLib
# headers, vendored QuickJS-ng, then raylib (for the BLAS/Tri/TriEx types).
QJS_DIR       = ../third_party/quickjs-ng
# Vendored CPU BC1-7 block encoders (MIT / public domain -- see
# third_party/bc7enc/README.md). Consumed only through
# src/render/bc_encode.{h,cpp}; nothing else includes these headers.
BC7ENC_DIR    = ../third_party/bc7enc
BOX3D_DIR     = ../third_party/box3d
BOX3D_LIB     = $(BOX3D_DIR)/libbox3d.a
INCLUDE_PATHS = -Iinclude -Isrc -Isrc/render -Isrc/provider \
                -I$(MSL_DIR)/include -I$(MSL_DIR)/src \
                -I$(SQL_DIR)/include -I$(MEMLIB_DIR)/include \
                -I$(MATHLIB_DIR)/include \
                -I$(PROFILE_DIR)/include \
                -I$(QJS_DIR) -I$(RAYLIB_PATH) -I$(BOX3D_DIR)/include \
                -I$(BC7ENC_DIR) \
                -I$(PFL_DIR)/include \
                -I$(FLECS_DIR) \
                -I$(OZZ_INCLUDE) \
                -I../third_party/Vulkan-Headers/include $(EXTRA_INCLUDE)

# Vendored QuickJS-ng engine. v0.10.0 needs xsum.c (xsum_small_* symbols) beyond
# the core four. Compiled via gcc (C) so the embedding symbols stay unmangled.
QJS_C   = $(QJS_DIR)/quickjs.c $(QJS_DIR)/libregexp.c \
          $(QJS_DIR)/libunicode.c $(QJS_DIR)/cutils.c $(QJS_DIR)/xsum.c
QJS_OBJ = $(addprefix $(OBJDIR)/,quickjs.o libregexp.o libunicode.o cutils.o xsum.o)

# Flecs is C17 and must never be compiled by the C++ rule below.
FLECS_C   = $(FLECS_DIR)/flecs.c
FLECS_OBJ = $(addprefix $(OBJDIR)/,flecs.o)

# MatterEngine3 C++ sources: full pipeline — bake/script/DSL/CSG, part-graph,
# LOD flatten/select, tileset pipeline, render, provider, and facade.
#
# ME3_OBJ derives from this list (below), so this list alone decides what the
# archive contains. Not every .cpp under src/ belongs here: the vk_*/vt_* render
# sources are deliberately absent because this build doesn't define
# MATTER_VULKAN_VIEWER, and MatterEditor compiles them itself via WIN_ME3_CPP.
# That exception is what makes an accidental omission hard to see -- an absent
# source looks like one more deliberate editor-only source. So absences here are
# NOT self-evident; check that the editor really does supply the symbol before
# concluding one is intentional. pf_bindings, warp_field and the ParticleFlowLib
# sources were each absent for that reason and left the archive unlinkable.
ME3_CPP = src/script_host.cpp src/dsl_state.cpp src/dsl_bindings.cpp src/dsl_animation.cpp \
          $(PFL_DIR)/src/pf_math.cpp $(PFL_DIR)/src/pf_sim.cpp \
          $(PFL_DIR)/src/pf_fields.cpp $(PFL_DIR)/src/pf_path_recorder.cpp \
          src/pf_bindings.cpp \
          src/dsl_triangle.cpp src/triangle_emit.cpp src/polygon_triangulate.cpp \
          src/csg_lowering.cpp src/module_resolver.cpp src/script/world_definition_loader.cpp src/script_rng_binding.cpp \
          src/part_asset_v2.cpp src/part_graph.cpp src/part_graph_snapshot.cpp \
          src/live_edit.cpp src/live_edit_prod.cpp src/inotify_watcher.cpp \
          src/lod_bake.cpp src/world_flatten.cpp src/part_flatten.cpp \
          src/warp_field.cpp \
          src/impostor_bake.cpp \
          src/retopo_blacklist.cpp \
          src/modifier_apply.cpp \
          src/sector_grid.cpp src/lod_select.cpp \
          src/world_lights.cpp \
          src/world_tracer.cpp \
          src/part_cluster.cpp \
          src/tileset_layout.cpp src/tileset_collider.cpp src/tileset_settle.cpp \
          src/tileset_placement.cpp src/tileset_part_collider.cpp src/tileset_bake.cpp \
          src/tileset_phase.cpp \
          src/tileset_metrics.cpp \
          src/tileset_gtex.cpp src/tileset_torus_bvh.cpp \
          src/shader_source.cpp \
          src/render/raster_mesh.cpp src/render/indexed_part_geometry.cpp \
          src/render/matrix_math.cpp src/render/frame_matrices.cpp src/render/vk_temporal.cpp src/render/vk_gi_math.cpp src/render/vk_lighting_controls.cpp \
          src/render/part_store.cpp src/render/world_state.cpp \
          src/render/lod_trace.cpp \
          src/render/tileset_slicer.cpp src/render/bc_encode.cpp \
          src/provider/local_provider.cpp src/provider/resolvers.cpp \
          src/ecs/ecs_runtime.cpp src/ecs/simulation_control.cpp src/ecs/physics_context.cpp src/ecs/physics_shapes.cpp src/ecs/physics_systems.cpp src/ecs/streaming_systems.cpp src/ecs/transform_system.cpp \
          src/scene/scene_service.cpp src/scene/scene_change_tracker.cpp \
          src/matter_engine.cpp \
          src/refine_controller.cpp \
          src/async_bake.cpp \
          src/bake_trace.cpp \
          src/resolve_cache.cpp \
          src/terrain_field.cpp \
          src/terrain_mesher.cpp \
          src/sector_streamer.cpp \
          src/animation/ozz_adapter.cpp \
          src/animation/ozz_bake.cpp \
          src/animation/animation_ir.cpp src/animation/animation_validate.cpp src/animation/animation_binding_bake.cpp src/animation/animation_budget.cpp \
          src/animation/anim_asset.cpp src/animation/anim_bundle.cpp src/animation/animation_asset_store.cpp src/animation/animation_runtime_asset.cpp src/animation/animation_store.cpp src/animation/animation_evaluator.cpp src/animation/animation_systems.cpp src/animation/animation_world_queries.cpp src/animation/animation_controllers.cpp src/animation/animation_targets.cpp \
          src/render/animation_rigid_bridge.cpp src/render/animation_skin_bridge.cpp src/render/vk_animation_skinning.cpp src/render/vk_animation_bounds.cpp \
          src/streaming/sector_streaming_coordinator.cpp \
          src/event/dispatch_context.cpp src/event/subscription.cpp src/event/event_hub.cpp src/event/command.cpp \
          src/event/property.cpp \
          src/util/json_doc.cpp \
          src/props/props.cpp src/props/props_file.cpp \
          src/props/draw_overrides.cpp

# MatterSurfaceLib C++ backend sources compiled into the archive so MatterEditor
# need not compile them directly (no libmsl.a). Same set as the old viewer's MSL_CPP
# minus mesh_retopo.cpp (handled separately via EXTRA_RETOPO_CPP when AR is present).
MSL_CPP = $(MSL_DIR)/src/cluster.cpp $(MSL_DIR)/src/cell.cpp \
          $(MSL_DIR)/src/mesh_simplifier.cpp $(MSL_DIR)/src/mesh_indexed.cpp \
          $(MSL_DIR)/src/mesh_smooth.cpp \
          $(MSL_DIR)/src/mesh_transform.cpp \
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
          $(MCL_DIR)/src/mesh_charting.cpp \
          $(EXTRA_RETOPO_CPP)

# MSL C sources: compiled via gcc to keep extern "C" symbols unmangled.
# spatial_hash.c and mem_pool.c are compiled from their source-of-truth libs
# (SpatialQueryLib / MemoryLib) rather than from copies under MatterSurfaceLib.
MSL_C   = $(MSL_DIR)/src/surface.c \
          $(SQL_DIR)/src/spatial_hash.c $(MEMLIB_DIR)/src/mem_pool.c \
          $(MSL_DIR)/src/material_registry.c $(MSL_DIR)/src/fat_primitive.c
MSL_C_OBJ = $(addprefix $(OBJDIR)/,surface.o spatial_hash.o \
            mem_pool.o material_registry.o fat_primitive.o)

# ME3_OBJ is DERIVED from ME3_CPP -- never hand-maintained.
#
# It used to be a second, parallel list of .o names, and keeping the two in sync
# by hand failed twice in one migration: part_imposter_representation.o and then
# impostor_bake.o were each in ME3_CPP but missing here, so the archive silently
# shipped with the symbol unresolved. The editor still linked, because it
# compiles those sources a second time itself -- which is exactly what makes the
# omission invisible until something links against the archive alone.
#
# vpath (above) already resolves every source directory, so the mapping is total.
# It is unambiguous only while ME3_CPP basenames stay unique -- two same-named
# sources in different directories collapse onto one .o here and one of them
# silently never compiles, which is the one way this derivation can still lose a
# source. check-me3-source-basenames (below) holds that assumption to account.
# Adding a source to ME3_CPP is otherwise sufficient.
ME3_OBJ = $(addprefix $(OBJDIR)/,$(notdir $(ME3_CPP:.cpp=.o)))

# Vendored BC encoder objects. Explicit rules (below) rather than vpath: these
# are third-party sources that must never be picked up by the generic %.cpp
# rule's source search.
BC7ENC_OBJ = $(addprefix $(OBJDIR)/,bc7enc.o rgbcx.o)

# ProfileLib (libs/ProfileLib): the shared engine profiler. Explicit rule +
# archive member, same self-contained model as mem_pool.o / spatial_hash.o.
PROFILE_OBJ = $(OBJDIR)/profile.o

# MSL C++ object names (basenames; same flat-object model as ME3_OBJ).
MSL_OBJ = $(addprefix $(OBJDIR)/,cluster.o cell.o mesh_simplifier.o mesh_indexed.o mesh_smooth.o mesh_transform.o \
          blas_manager.o bvh.o bvh_analyzer.o \
          mesh_worker_pool.o mesh_build_utils.o meshing_algorithm.o \
          marching_cubes_algorithm.o oriented_cube_algorithm.o vertex_ao.o \
          occupancy.o tlas_manager.o part_asset.o \
          lattice.o particle_culling.o mesh_charting.o \
          $(if $(EXTRA_RETOPO_CPP),mesh_retopo.o))

# Build layout: <project>/build/obj/ for objects+deps, <project>/build/ for
# the archive. Keep the project root source-only.
OBJDIR = build/obj
LIB = build/libmatter_engine3.a
# Flag-set stamp file also counts as build output; keep it out of the root too.
LIB_FLAGS_STAMP = build/.lib_flags

# Shader embedding codegen (Stage 1a).
# Base shader list from the brief table + glsl includes found in .fs/.comp sources.
# A shaders_gpu/ directory used to sit beside shaders/ holding cull.comp,
# hiz_downsample.comp and raster_gpu_driven.vs for the GL GpuCuller /
# raster-composer path. That path was deleted in Phase 5a (tech-debt.md §6),
# which left the sources unread; the directory and its MatterEditor junction
# were retired outright once the last of them went.
# NOTE, still worth stating because the names collide: build/shaders_vk/cull.comp.spv
# below (VK_SPV) is a compiled Vulkan SPIR-V binary built from the LIVE
# shaders_vk/cull.comp source. That is the shipping culler and must not be
# confused with the retired GL shaders_gpu/cull.comp it was named after.
SHADER_LOGICAL := shaders/raster.vs shaders/raster.fs shaders/tileset_sampling.glsl \
    shaders/raytrace_tlas_blas_processed.fs \
    shaders/materials.glsl shaders/bvh_tlas_common.glsl shaders/lighting.glsl
# SHADER_FILES resolves logical paths into real filesystem paths under MatterEngine3/:
# shaders/ is a symlink -> ../libs/MatterSurfaceLib/shaders.
SHADER_FILES := $(SHADER_LOGICAL)

.PHONY: all clean test vulkan-spirv check-me3-source-basenames check-animation-production-sources check-animation-runtime-boundary

# all MUST be the first target so it is the default goal.
all: $(LIB)

$(OBJDIR) build:
	@mkdir -p $@

# ME3_OBJ derives from ME3_CPP by $(notdir), so two ME3_CPP sources sharing a
# basename collapse onto one object: one of them is never compiled, and `ar` is
# handed the same member name twice without complaint. That is the single way
# the derivation can still lose a source, and it is silent -- so assert the
# uniqueness the derivation assumes rather than leaving it to review.
# $(LIB) depends on this order-only (it is phony; a normal prerequisite would
# leave the archive perpetually out of date and re-run `ar` every build).
check-me3-source-basenames:
	@dups=`printf '%s\n' $(notdir $(ME3_CPP)) | sort | uniq -d`; \
	if [ -n "$$dups" ]; then \
		echo "ERROR: ME3_CPP sources share a basename, so they collide on one .o:"; \
		printf '  %s\n' $$dups; \
		echo "       rename one, or the archive silently ships only the other."; \
		exit 1; \
	fi

# Keep the archive's explicit object list in sync with the animation sources
# consumed by the bake/runtime boundary.  The archive is flat, so a source
# listed in ME3_CPP but omitted here silently disappears from production links.
# ME3_OBJ entries carry the $(OBJDIR)/ prefix, so match on "/<object> " rather
# than " <object> " -- a bare-name match would silently never fire.
ANIMATION_PRODUCTION_OBJ = dsl_animation.o ozz_adapter.o ozz_bake.o animation_ir.o animation_budget.o \
                           animation_validate.o animation_binding_bake.o anim_asset.o anim_bundle.o animation_asset_store.o animation_runtime_asset.o animation_store.o animation_evaluator.o animation_systems.o animation_world_queries.o animation_controllers.o animation_targets.o animation_rigid_bridge.o animation_skin_bridge.o
check-animation-production-sources:
	@for object in $(ANIMATION_PRODUCTION_OBJ); do \
		case " $(ME3_OBJ) " in *"/$$object "*) ;; *) \
			echo "ERROR: production archive omits $$object"; exit 1 ;; esac; \
	done

# The runtime adapter must remain linkable against only Ozz runtime archives.
# Offline compilation lives in ozz_bake.o, which is retained in this archive
# for the bake host but is never a Viewer runtime dependency.
check-animation-runtime-boundary: $(OBJDIR)/ozz_adapter.o
	@if $(NM) -C $(OBJDIR)/ozz_adapter.o | grep -q 'ozz::animation::offline::'; then \
		echo "ERROR: runtime adapter references Ozz offline symbols"; exit 1; \
	fi

shaders_gen/embedded_shaders.h: tools/embed_shaders.py $(SHADER_FILES)
	mkdir -p shaders_gen
	python3 tools/embed_shaders.py $@ . $(SHADER_LOGICAL)

GLSLC ?= glslc
VK_SPV = build/shaders_vk/transform_probe.comp.spv \
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

build/shaders_vk/%.spv: shaders_vk/%
	@mkdir -p $(dir $@)
	$(GLSLC) --target-env=vulkan1.3 -O -Ishaders_vk $< -o $@

build/shaders_vk/raster.vert.spv build/shaders_vk/gbuffer.frag.spv: \
    shaders_vk/material_common.glsl

# C2 uses the same source and descriptor ABI as raster.vert, but a compile
# time specialization adds location 5 for the previous skinned position.
build/shaders_vk/raster_skin.vert.spv: shaders_vk/raster.vert shaders_vk/material_common.glsl
	@mkdir -p $(dir $@)
	$(GLSLC) --target-env=vulkan1.3 -O -Ishaders_vk -DMATTER_SKINNED_VERTEX_INPUT $< -o $@

build/shaders_vk/gbuffer.frag.spv: shaders_vk/tileset_common.glsl

build/shaders_vk/rt_surface.rchit.spv build/shaders_vk/rt_surface.rahit.spv \
    build/shaders_vk/rt_visibility.rahit.spv \
    build/shaders_vk/rt_radiance.rmiss.spv \
    build/shaders_vk/rt_lighting.rgen.spv \
    build/shaders_vk/rt_surface_test.rgen.spv: \
    shaders_vk/rt_surface_common.glsl shaders_vk/tileset_common.glsl

# Phase 2 (horizon-map lighting): rt_shadow.rgen now includes
# tileset_common.glsl directly (not via rt_surface_common.glsl -- it does
# not otherwise use that file) to reach bindings 15/16 for the horizon test.
build/shaders_vk/rt_shadow.rgen.spv: shaders_vk/tileset_common.glsl

build/shaders_vk/composite.frag.spv build/shaders_vk/rt_lighting.rgen.spv: \
    shaders_vk/sky_common.glsl

build/shaders_vk/gi_temporal.comp.spv build/shaders_vk/gi_atrous.comp.spv: \
    shaders_vk/gi_common.glsl

build/shaders_vk/vol_density.comp.spv build/shaders_vk/vol_scatter.comp.spv \
    build/shaders_vk/vol_integrate.comp.spv \
    build/shaders_vk/composite.frag.spv: \
    shaders_vk/vol_common.glsl

# WP-D (chart VT): the Wang cell machinery moved into wang_common.glsl, which
# tileset_common.glsl now includes; every tileset_common consumer therefore
# also depends on it, and the VT compositor includes it directly.
build/shaders_vk/gbuffer.frag.spv build/shaders_vk/rt_shadow.rgen.spv \
    build/shaders_vk/rt_surface.rchit.spv \
    build/shaders_vk/rt_surface.rahit.spv \
    build/shaders_vk/rt_visibility.rahit.spv \
    build/shaders_vk/rt_radiance.rmiss.spv \
    build/shaders_vk/rt_lighting.rgen.spv \
    build/shaders_vk/rt_surface_test.rgen.spv \
    build/shaders_vk/vt_composite.comp.spv: shaders_vk/wang_common.glsl

# WP-H (chart VT): vt_chart_types.glsl (the struct mirrors) and
# vt_chart_resolve.glsl (the page-texel -> owning-triangle resolve) are
# includes, not stages. BOTH page passes consume them, which is the point: the
# tier-2 enrichment multiplies occlusion into pages the tier-1 compositor baked,
# so the two must resolve every texel identically or the occlusion lands on the
# wrong surface.
build/shaders_vk/vt_composite.comp.spv \
    build/shaders_vk/vt_enrich_ao.comp.spv: \
    shaders_vk/vt_chart_types.glsl shaders_vk/vt_chart_resolve.glsl

# P2 (texel-rate tape): the GLSL noise twin + tape interpreter include.
build/shaders_vk/vt_composite.comp.spv: shaders_vk/vt_surface_tape.glsl

# WP-E (chart VT): vt_common.glsl is the shared VT sampling helper. It is an
# include, not a stage, so it has no .spv of its own -- but every consumer must
# rebuild when it changes. WP-G added the RT stages: rt_surface_common.glsl now
# includes vt_common.glsl, so every shader that includes IT is a consumer too.
build/shaders_vk/gbuffer.frag.spv build/shaders_vk/rt_surface.rchit.spv \
    build/shaders_vk/rt_surface.rahit.spv \
    build/shaders_vk/rt_visibility.rahit.spv \
    build/shaders_vk/rt_radiance.rmiss.spv \
    build/shaders_vk/rt_lighting.rgen.spv \
    build/shaders_vk/rt_surface_test.rgen.spv: shaders_vk/vt_common.glsl

# M2.5 impostors: impostor_common.glsl is the ONE place the view grid is
# written down, and BOTH stages that touch a billboard read it -- raster.vert
# to pick the cell, gbuffer.frag to sample it. It had no rule here, so the
# elevation-ring change (2026-08-05) edited the grid from 4x4 to 8x8, rebuilt
# raster.vert because that file changed too, and left gbuffer.frag.spv holding
# the OLD divisor. That mis-samples every impostor by a quarter cell and looks
# like a rendering bug, not a stale build. Same failure the vt_common.glsl and
# vt_chart_types.glsl rules above exist to prevent.
#
# M6.5 added three more consumers. The header now also carries the identity
# attachment's impostor BIT and the mask helpers, and the deferred passes that
# read that attachment must agree with gbuffer.frag on where the bit is:
# rt_shadow.rgen acts on it, composite.frag and rt_lighting.rgen mask it off.
# A stale .spv here is the same silent-wrong-pixel class as the grid divisor
# above -- an unmasked reader just fails its bounds test and drops a material
# feature, with nothing on screen that says "stale build".
build/shaders_vk/raster.vert.spv build/shaders_vk/raster_skin.vert.spv \
    build/shaders_vk/gbuffer.frag.spv \
    build/shaders_vk/rt_shadow.rgen.spv \
    build/shaders_vk/rt_lighting.rgen.spv \
    build/shaders_vk/composite.frag.spv: shaders_vk/impostor_common.glsl

shaders_gen/embedded_spirv.h: $(VK_SPV) tools/embed_spirv.py
	python3 tools/embed_spirv.py $@ $(VK_SPV)

vulkan-spirv: shaders_gen/embedded_spirv.h

# Per-file pattern rules enable safe parallel compilation with `make -j`.
# vpath directs Make to search source directories by basename.
vpath %.cpp src src/animation src/render src/provider src/script src/ecs src/scene src/streaming src/event src/util src/props $(MSL_DIR)/src $(SQL_DIR)/src $(PFL_DIR)/src $(MCL_DIR)/src
vpath %.c $(QJS_DIR) $(MSL_DIR)/src $(FLECS_DIR)

# Flecs C source (gcc C17, never g++).
$(FLECS_OBJ): $(FLECS_C) | $(OBJDIR)
	gcc -c $< -o $@ -std=c17 -O2 -I$(FLECS_DIR)

# QuickJS C sources (gcc, extern "C" safe).
$(OBJDIR)/%.o: $(QJS_DIR)/%.c | $(OBJDIR)
	gcc -c $< -o $@ -O2 -DCONFIG_VERSION='"0.10.0"' -I$(QJS_DIR)

# MSL C sources (gcc, extern "C" safe).
$(OBJDIR)/%.o: $(MSL_DIR)/src/%.c | $(OBJDIR)
	gcc -c $< -o $@ -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)

# Shared library C sources, compiled from their source-of-truth directories.
# These are explicit (not pattern) rules and the source dirs are deliberately
# kept off vpath: SpatialQueryLib/src/bvh.c would otherwise shadow
# SpatialQueryLib/src/bvh.cpp when make resolves bvh.o.
$(OBJDIR)/spatial_hash.o: $(SQL_DIR)/src/spatial_hash.c | $(OBJDIR)
	gcc -c $< -o $@ -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)

$(OBJDIR)/mem_pool.o: $(MEMLIB_DIR)/src/mem_pool.c | $(OBJDIR)
	gcc -c $< -o $@ -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)

$(OBJDIR)/profile.o: $(PROFILE_DIR)/src/profile.cpp | $(OBJDIR)
	$(CC) -c $< -o $@ $(CFLAGS) $(INCLUDE_PATHS)

# Vendored BC encoders. Always -O2 (an -O0 bc7enc is ~30x slower, which would
# show up directly as world-connect stall time) and -w: these are unmodified
# upstream sources, so their warnings are not this project's to fix or to read
# past. Explicit rules, and $(BC7ENC_DIR) is deliberately off vpath.
$(OBJDIR)/bc7enc.o: $(BC7ENC_DIR)/bc7enc.cpp | $(OBJDIR)
	$(CC) -c $< -o $@ -std=c++17 -O2 -w -I$(BC7ENC_DIR)

$(OBJDIR)/rgbcx.o: $(BC7ENC_DIR)/rgbcx.cpp | $(OBJDIR)
	$(CC) -c $< -o $@ -std=c++17 -O2 -w -I$(BC7ENC_DIR)

# EXTRA_CFLAGS changes are invisible to make's dependency graph (objects look
# up-to-date even though they were compiled with different defines, e.g.
# MATTER_HAVE_AUTOREMESHER). Every C++ object depends on this stamp file,
# which is rewritten only when the flag set differs — so a flag change forces
# a full recompile while same-flag builds stay incremental.
.PHONY: FORCE
FORCE:
$(LIB_FLAGS_STAMP): FORCE | build
	@[ "`cat $@ 2>/dev/null`" = '$(EXTRA_CFLAGS)' ] || printf '%s' '$(EXTRA_CFLAGS)' > $@

# ME3 + MSL C++ sources (each source file gets its own .o via pattern rule).
# ME3 objects depend on the generated shader header.
$(ME3_OBJ): shaders_gen/embedded_shaders.h
$(ME3_OBJ) $(MSL_OBJ): $(LIB_FLAGS_STAMP)
$(OBJDIR)/%.o: %.cpp | $(OBJDIR)
	$(CC) -c $< -o $@ $(CFLAGS) $(INCLUDE_PATHS)

$(BOX3D_LIB):
	$(MAKE) -C $(BOX3D_DIR)

$(LIB): $(QJS_OBJ) $(ME3_OBJ) $(MSL_OBJ) $(MSL_C_OBJ) $(FLECS_OBJ) $(BC7ENC_OBJ) $(PROFILE_OBJ) $(BOX3D_LIB) | check-me3-source-basenames
# `ar rcs` REPLACES the members it is given and leaves every other member in
# place, so an object whose source was deleted (and whose name was dropped from
# ME3_OBJ) keeps being served out of a stale archive forever -- linking dead
# code that no longer exists in the tree, with no build error to notice. This
# bit the far-field-impostor work twice (a deleted impostor_root_ledger.o, then
# M0's world_composer.o / tileset_provider.o). Rebuild the archive from scratch.
	rm -f $(LIB)
	ar rcs $(LIB) $(QJS_OBJ) $(ME3_OBJ) $(MSL_OBJ) $(MSL_C_OBJ) $(FLECS_OBJ) $(BC7ENC_OBJ) $(PROFILE_OBJ)

# Delegate to the test suite (headless, GL-free host + raylib-linked BLAS path).
test:
	$(MAKE) -C tests run-ecs
	$(MAKE) -C tests run-physics
	$(MAKE) -C tests run-partv2
	$(MAKE) -C tests run-script

clean:
	rm -rf build
	$(MAKE) -C tests clean
