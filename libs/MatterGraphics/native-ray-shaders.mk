# Included from MatterEngine3/Makefile; the same moved production entrypoints.
MATTER_GRAPHICS_NATIVE_RAY_STAGES := rt_lighting.rgen \
    rt_lighting_culled.rgen \
    rt_lighting_culled_audit.rgen \
    rt_primary_adaptive.rgen \
    rt_primary_adaptive_culled.rgen \
    rt_primary_adaptive_culled_audit.rgen \
    rt_primary_fixed.rgen \
    rt_radiance.rmiss \
    rt_shadow.rchit \
    rt_shadow.rgen \
    rt_shadow.rmiss \
    rt_surface.rahit \
    rt_surface.rchit \
    rt_surface_test.rgen \
    rt_visibility.rahit \
    rt_visibility.rchit \
    rt_visibility.rmiss \
    gi_temporal.comp \
    gi_atrous.comp \
    primary_light_cull.comp \
    sparse_voxel_shadow.comp

$(addprefix build/shaders_vk/,$(addsuffix .spv,$(MATTER_GRAPHICS_NATIVE_RAY_STAGES))): build/shaders_vk/%.spv: ../libs/MatterGraphics/shaders/%
	@mkdir -p $(dir $@)
	$(GLSLC) --target-env=vulkan1.3 -O -Ishaders_vk $< -o $@

# Make lacks shader depfiles; retain dependencies through compatibility includes.
$(VK_SPV): ../libs/MatterGraphics/shaders/rt_lighting_impl.glsl \
    ../libs/MatterGraphics/shaders/rt_primary_inputs.glsl \
    ../libs/MatterGraphics/shaders/rt_surface_common.glsl \
    ../libs/MatterGraphics/shaders/gi_common.glsl \
    ../libs/MatterGraphics/shaders/local_light_types.glsl \
    ../libs/MatterGraphics/shaders/local_lighting.glsl \
    ../libs/MatterGraphics/shaders/sparse_voxel_optics.glsl
