#version 460
#extension GL_GOOGLE_include_directive : require

#include "material_common.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tint;
layout(location = 3) in vec4 in_surface;
layout(location = 4) in uint in_material_index;
// The C2 skin raster specialization supplies this attribute from
// animation_skin.comp's previous output. The default static specialization
// keeps the legacy five-attribute contract and uses in_position below.
#ifdef MATTER_SKINNED_VERTEX_INPUT
layout(location = 5) in vec3 in_previous_position;
#else
// Warp field (VT Phase 2): warped ground coordinate + frozen frame,
// terrain-sector vertices only (zeros elsewhere; su == 0 means "no warp").
// The skinned specialization's VkSkinVertex carries no warp data — animated
// props never get a field — so these attributes exist only in the static
// vertex layout; the skinned variant emits the neutral varyings below.
layout(location = 6) in vec2 in_warp_uv;
layout(location = 7) in uvec2 in_warp_frame;  // x: oct tangent f16x2, y: (su, sv) f16x2
#endif

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec4 out_tint;
layout(location = 2) out vec4 out_surface;
layout(location = 3) out vec3 out_velocity_valid;
layout(location = 4) flat out uint out_material_index;
layout(location = 5) flat out uint out_instance_token;
layout(location = 6) flat out uint out_material_valid;
layout(location = 7) out vec3 out_world_pos;
// WP-E (chart-space VT): pass the draw record's transported vt slot through
// flat; gbuffer.frag branches on it. 0 = no VT (legacy path).
layout(location = 8) flat out uint out_vt_slot;
// Warp field (VT Phase 2): xy = warped ground uv (world-anchored metres),
// z = su, w = sv (the frame's gradient magnitudes; su == 0 => no warp).
layout(location = 9) out vec4 out_warp_uv_scales;
// The frame tangent (world space, unit-ish; re-orthogonalized against the
// interpolated normal in the fragment shader).
layout(location = 10) out vec3 out_warp_tangent;
// LOD debug view: the rung cull.comp selected for this draw (or the direct
// override below). Location 11 and NOT 9 -- 9/10 are the warp field on this
// base; the branch this was ported from had no warp field and used 9, and
// copying that number silently aliases the ground's warped uv.
layout(location = 11) flat out uint out_selected_lod;

layout(set = 0, binding = 0, std140) uniform FrameConstants {
    mat4 world_to_clip;
    mat4 previous_world_to_clip;
    vec4 frustum_planes[6];
    vec4 camera_eye_pixel_budget;
    uvec4 counts;
    uvec4 capacities;
    uvec4 temporal;
} frame;

struct DrawTransform {
    mat4 current;
    mat4 previous;
    uint history_valid;
    uint instance_token;
    uint vt_slot;
    uint selected_lod;
};

// Shared with gbuffer.frag. Direct (non-indirect) draws have no cull-written
// transform tail, so they carry their own selected rung in the first two
// words. `wireframe_enabled` is reserved: this base has no wireframe path, and
// nothing reads the word.
layout(push_constant) uniform RasterDebugPushConstants {
    uint direct_lod;
    uint direct_lod_valid;
    uint lod_tint_enabled;
    uint wireframe_enabled;
} debug_push;

layout(set = 1, binding = 3, std430) readonly buffer DrawTransforms {
    DrawTransform transforms[];
};

#ifndef MATTER_SKINNED_VERTEX_INPUT
// Octahedral decode, the exact inverse of warp_field.cpp's oct_encode.
vec3 warp_oct_decode(vec2 e) {
    vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
        v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0,
                                        v.y >= 0.0 ? 1.0 : -1.0);
    float len = length(v);
    return len > 1e-6 ? v / len : vec3(1.0, 0.0, 0.0);
}
#endif

void main() {
    // gl_InstanceIndex already includes VkDrawIndirectCommand::firstInstance.
    // Adding gl_BaseInstance would index the Task 7 transform array twice.
    DrawTransform draw = transforms[gl_InstanceIndex];
    mat4 model = draw.current;
    vec4 world = model * vec4(in_position, 1.0);
    vec4 current_clip = frame.world_to_clip * world;
    vec3 previous_local_position = in_position;
#ifdef MATTER_SKINNED_VERTEX_INPUT
    previous_local_position = in_previous_position;
#endif
    vec4 previous_clip = frame.previous_world_to_clip *
                         draw.previous * vec4(previous_local_position, 1.0);
    gl_Position = current_clip;
    bool valid = frame.temporal.y == 0u && draw.history_valid != 0u &&
                 current_clip.w != 0.0 && previous_clip.w != 0.0;
    out_velocity_valid = vec3(
        valid ? (current_clip.xy / current_clip.w -
                 previous_clip.xy / previous_clip.w) * 0.5 *
                vec2(float(frame.temporal.z), -float(frame.temporal.w))
              : vec2(0.0),
        valid ? 1.0 : 0.0);
    out_normal = normalize(mat3(model) * in_normal);
    out_tint = in_tint;
    out_surface = in_surface;
    out_material_index = in_material_index;
    out_instance_token = draw.instance_token;
    out_material_valid = in_material_index < frame.counts.z ? 1u : 0u;
    out_world_pos = world.xyz;
    out_vt_slot = draw.vt_slot;
    out_selected_lod = debug_push.direct_lod_valid != 0u
                           ? debug_push.direct_lod
                           : draw.selected_lod;
#ifdef MATTER_SKINNED_VERTEX_INPUT
    // Animated props carry no warp field; su == 0 selects the world-XZ
    // fallback in gbuffer.frag.
    out_warp_uv_scales = vec4(0.0);
    out_warp_tangent = vec3(1.0, 0.0, 0.0);
#else
    vec2 warp_scales = unpackHalf2x16(in_warp_frame.y);
    out_warp_uv_scales = vec4(in_warp_uv, warp_scales);
    // Terrain draws are translations, but rotate through the model matrix
    // anyway so an instanced/rotated placement keeps a consistent frame.
    out_warp_tangent =
        mat3(model) * warp_oct_decode(unpackHalf2x16(in_warp_frame.x));
#endif
}
