# Relocated production stages; logical SPIR-V names and options are unchanged.
set(matter_graphics_native_ray_stages
    rt_lighting.rgen
    rt_lighting_culled.rgen
    rt_lighting_culled_audit.rgen
    rt_primary_adaptive.rgen
    rt_primary_adaptive_culled.rgen
    rt_primary_adaptive_culled_audit.rgen
    rt_primary_fixed.rgen
    rt_radiance.rmiss
    rt_shadow.rchit
    rt_shadow.rgen
    rt_shadow.rmiss
    rt_surface.rahit
    rt_surface.rchit
    rt_surface_test.rgen
    rt_visibility.rahit
    rt_visibility.rchit
    rt_visibility.rmiss
    gi_temporal.comp
    gi_atrous.comp
    primary_light_cull.comp
    sparse_voxel_shadow.comp
)
set(matter_graphics_native_ray_overrides)
foreach(stage IN LISTS matter_graphics_native_ray_stages)
    list(APPEND matter_graphics_native_ray_overrides
        "${stage}|${CMAKE_CURRENT_LIST_DIR}/../shaders/${stage}")
endforeach()
