#version 460
#extension GL_GOOGLE_include_directive : require

#include "material_common.glsl"
#include "impostor_common.glsl"

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
// M2.5 impostor: the instance's object->world rotation, as its first two basis
// columns (the third is their cross product). The atlas stores OBJECT-space
// normals -- one atlas serves every placement of the part -- so the fragment
// stage needs the rotation to put them in world space, and the fragment stage
// has no model matrix. Written for every draw (two vec3 interpolants); read
// only when the impostor branch runs.
layout(location = 12) flat out vec3 out_model_basis_x;
layout(location = 13) flat out vec3 out_model_basis_y;

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

// M2.5 impostor marker. The terminal rung of an eligible part's ladder is two
// triangles whose TriEx carries this sentinel in uv.x; build_raster_mesh_data
// forwards uv into surface.xy, so the billboard needs NO new vertex attribute,
// no stride change and no second pipeline -- it is drawn by the same indirect
// draw as every mesh rung, selected by the same lod::select_rep walk.
//
// The rest of the rung's vertex payload is repurposed, and only here:
//   surface.y : corner ordinal 0..3, counter-clockwise from (-1,-1)
//   surface.z : the quad's half extent in part-local metres (the AO channel;
//               an impostor's occlusion comes from the atlas, so it is free)
//   tint.rg   : the atlas slot, 8 bits each, patched by the renderer when the
//               part's atlas is uploaded to a layer pair
//   tint.b    : this impostor's ordinal within the part (pre-patch identity)

void main() {
    // gl_InstanceIndex already includes VkDrawIndirectCommand::firstInstance.
    // Adding gl_BaseInstance would index the Task 7 transform array twice.
    DrawTransform draw = transforms[gl_InstanceIndex];
    mat4 model = draw.current;
    const bool is_impostor = in_surface.x > kImpostorMarker;
    vec4 world = model * vec4(in_position, 1.0);
    vec2 impostor_uv = vec2(0.0);
    float impostor_view = 0.0;
    if (is_impostor) {
        const float half_extent = in_surface.z;
        const uint corner = uint(in_surface.y + 0.5);
        // 0 -> (-1,-1)  1 -> (+1,-1)  2 -> (+1,+1)  3 -> (-1,+1)
        const vec2 sign_xy = vec2((corner == 1u || corner == 2u) ? 1.0 : -1.0,
                                  (corner >= 2u) ? 1.0 : -1.0);
        // Every corner can recover the quad's centre, which is what lets the
        // billboard exist without a second per-vertex channel for it.
        const vec3 center_local =
            in_position - vec3(sign_xy.x * half_extent,
                               sign_xy.y * half_extent, 0.0);
        const vec3 center_world = (model * vec4(center_local, 1.0)).xyz;
        const vec3 to_eye = frame.camera_eye_pixel_budget.xyz - center_world;

        // CYLINDRICAL billboarding: world up is preserved and only the
        // azimuth turns. That is not a simplification of a spherical card --
        // it is the same parameterisation the atlas was baked in (one azimuth
        // ring at the object's equator), so the quad's frame and the cell's
        // frame are the same frame by construction. A spherical card would
        // need elevation rows the atlas does not have.
        vec3 horizon = vec3(to_eye.x, 0.0, to_eye.z);
        const float horizon_len = length(horizon);
        horizon = horizon_len > 1e-5 ? horizon / horizon_len : vec3(0.0, 0.0, 1.0);
        const vec3 up_world = vec3(0.0, 1.0, 0.0);
        const vec3 right_world = normalize(cross(up_world, horizon));

        // Model scale: column length of the linear part. Uniform-scale
        // assumption, the same one part_flatten::transform_uniform_scale makes.
        const float model_scale = length(model[0].xyz);
        const float half_world = half_extent * model_scale;
        const vec3 world_corner = center_world +
                                  right_world * (sign_xy.x * half_world) +
                                  up_world * (sign_xy.y * half_world);
        // The billboard's world position is BUILT rather than transformed:
        // it is the only vertex in this shader whose placement depends on the
        // camera, so it cannot come out of the model matrix.
        world = vec4(world_corner, 1.0);

        // Which baked view. The ring was baked in OBJECT space, so the eye
        // direction has to come back through the model rotation before its
        // azimuth means anything -- otherwise a rotated placement shows the
        // wrong face of the rock.
        const vec3 eye_obj = to_eye * mat3(model);   // = transpose(mat3) * to_eye
        float azimuth = atan(eye_obj.x, eye_obj.z);  // bake: view_z = (sin a, 0, cos a)
        const float kTwoPi = 6.28318530717958647692;
        if (azimuth < 0.0) azimuth += kTwoPi;
        impostor_view = float(uint(floor(azimuth / kTwoPi * float(IMPOSTOR_VIEWS)
                                         + 0.5)) % uint(IMPOSTOR_VIEWS));
        // Cell-local uv, matching the bake's pixel mapping exactly:
        //   px = (nx * 0.5 + 0.5) * C,  py = (0.5 - ny * 0.5) * C
        impostor_uv = vec2(sign_xy.x * 0.5 + 0.5, 0.5 - sign_xy.y * 0.5);
    }
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
    // The impostor's atlas normals are OBJECT space; the fragment stage has no
    // model matrix, so hand it the rotation. Two columns, third by cross.
    out_model_basis_x = normalize(model[0].xyz);
    out_model_basis_y = normalize(model[1].xyz);
    // The impostor repurposes both interpolants: the fragment stage needs the
    // cell uv and which view was chosen, and neither exists per-vertex before
    // the camera is known. Every other draw passes them through untouched.
    out_tint = is_impostor
                   ? vec4(in_tint.x * 255.0, in_tint.y * 255.0, impostor_view, 0.0)
                   : in_tint;
    out_surface = is_impostor
                      ? vec4(in_surface.x, impostor_uv, 1.0)
                      : in_surface;
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
