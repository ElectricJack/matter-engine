#ifndef MATTER_VT_MATERIAL_DOMAIN_GLSL
#define MATTER_VT_MATERIAL_DOMAIN_GLSL
#include "vt_common.glsl"
#include "vt_visible_input.glsl"

// Two complete addresses: private receiver coverage/geometry and independently
// resident material. Never replace the receiver slot with a material slot: a
// receiver page can span several module pages, each with a different fallback.
struct VtMaterialBinding {
    VtAddress receiver;
    VtAddress material;
    bool uses_module;
};

// Coordinates are unwrapped normalized module coordinates. The caller maps
// position AND derivatives from the receiver's physical surface frame before
// calling. Normal-frame conversion and displacement datum remain the mapping
// owner's responsibility; this function addresses pixels, not geometry.
//
// The expected generation belongs to an immutable published receiver binding.
// This check detects stale/incomplete bindings; it does not replace retaining
// the module, its inputs and its page table through the GPU reader horizon.
// If a binding is unavailable, all material channels keep the complete receiver
// fallback together. No mixture of module color and receiver height is allowed.
VtMaterialBinding vt_resolve_material_domain(VtAddress receiver,
    uint module_slot, uint generation, vec2 module_uv, vec2 duv_dx, vec2 duv_dy) {
    VtMaterialBinding result;
    result.receiver = receiver;
    result.material = receiver;
    result.uses_module = false;
    if (!receiver.valid || module_slot == 0u || module_slot > uint(vt_variants.length()) ||
        any(isnan(module_uv)) || any(isinf(module_uv)) ||
        any(isnan(duv_dx)) || any(isinf(duv_dx)) ||
        any(isnan(duv_dy)) || any(isinf(duv_dy))) return result;
    VtVariantRecord record = vt_variants[module_slot - 1u];
    if (record.generation != generation || (record.flags & 1u) == 0u ||
        record.atlas_w == 0u || record.atlas_h == 0u ||
        record.mip_count == 0u || record.mip_count > VT_MAX_MIPS) return result;
    float lod = vt_desired_mip(module_slot, duv_dx, duv_dy);
    VtAddress material = vt_resolve_periodic(module_slot, module_uv, lod);
    if (!material.valid) return result;
    result.material = material;
    result.uses_module = true;
    return result;
}

vec4 vt_material_channel(VtMaterialBinding binding, int channel) {
    if (channel == VT_CHANNEL_AUX) return vt_sample_channel(binding.receiver, channel);
    vec4 value=vt_sample_channel(binding.material, channel);
    if(binding.uses_module && channel==VT_CHANNEL_ORM)value.r*=vt_sample_occlusion(binding.receiver);
    return value;
}

float vt_material_height_m(VtMaterialBinding binding) {
    return vt_sample_height_m(binding.material);
}

// Keep both requests available. The runtime must schedule module demand as well
// as receiver coverage, including the module coordinates visited by POM.
uvec4 vt_material_feedback(VtMaterialBinding binding) {
    return binding.uses_module ? vt_feedback_request(binding.material) : binding.material.module_request;
}
uvec4 vt_material_visible_feedback(VtMaterialBinding binding) {
    return vt_pack_visible_feedback(vt_feedback_request(binding.receiver),
                                    vt_material_feedback(binding));
}
#endif
