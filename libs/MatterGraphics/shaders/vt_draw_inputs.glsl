#ifndef MATTER_VT_DRAW_INPUTS_GLSL
#define MATTER_VT_DRAW_INPUTS_GLSL
// Matches VkSceneRenderer::VtDrawInputGpu; slots in these immutable material
// rows already address the corresponding source bank in tileset_common.glsl.
struct VtDrawInputGpu {
    uvec4 meta;
    VT_DRAW_MATERIAL_TYPE materials[256];
};
layout(set = VT_SET, binding = VT_DRAW_INPUT_BINDING, std430)
readonly buffer VtDrawInputs { VtDrawInputGpu vt_draw_inputs[8]; };

bool vt_draw_material(uint snapshot, uint material_index, out VT_DRAW_MATERIAL_TYPE material) {
    if (snapshot >= 8u || material_index >= vt_draw_inputs[snapshot].meta.x) return false;
    material = vt_draw_inputs[snapshot].materials[material_index];
    return true;
}
#endif
