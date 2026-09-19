#ifndef MATTER_RT_PRIMARY_INPUTS_GLSL
#define MATTER_RT_PRIMARY_INPUTS_GLSL
#include "vt_visible_input.glsl"
// Primary raygen reads the exact G-buffer source pixel, also for scaled GI.
layout(set = 0, binding = 29, rgba32ui)
uniform readonly uimage2D primary_visible_inputs;
bool rt_primary_composed_height(ivec2 pixel) {
    return (imageLoad(primary_visible_inputs, pixel).w & VT_VISIBLE_COMPOSED_HEIGHT) != 0u;
}
bool rt_primary_material(ivec2 source_pixel, uint material_index,
                         out RtMaterialGpu material) {
    uint snapshot = vt_visible_input_snapshot(
        imageLoad(primary_visible_inputs, source_pixel).w);
    if (vt_draw_material(snapshot, material_index, material)) return true;
    if (material_index >= rt_materials.length()) return false;
    material = rt_materials[material_index];
    return true;
}
#endif
