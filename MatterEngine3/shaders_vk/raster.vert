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
// Impostor parallax: xy = the cell-uv displacement per unit of stored depth,
// z = the card's world-space depth extent (for the conservative depth write).
// Zero for every non-impostor draw.
layout(location = 14) out vec4 out_impostor_parallax;
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
    // Intra-cell impostor parallax toggle. Declared in BOTH gbuffer.frag and
    // raster.vert and in RasterDebugPushConstants (vk_scene_renderer.h); the
    // three must stay identical.
    uint impostor_parallax_enabled;
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
    vec4 impostor_parallax = vec4(0.0);
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

        // THE CARD IS BUILT IN OBJECT SPACE, then transformed by `model`.
        //
        // It used to be built in world space and sized by
        //     half_extent * length(model[0].xyz)
        // -- column 0, the X scale, under an explicit uniform-scale
        // assumption. That assumption is false for the content impostors
        // exist to serve: alpine trees are placed with
        // scale(s, s * heightScale, s) (WorldSector.js), so a tree 1.4x
        // taller than base drew its MESH 1.4x tall and its IMPOSTOR at 1.0x.
        // The tree visibly shrank at the handover.
        //
        // Building in object space removes the assumption rather than
        // patching it: the card is half_extent-sized in the object's own
        // frame, and `model` then stretches it by exactly the matrix that
        // stretches the mesh it replaces -- so the two agree by construction
        // for ANY linear placement, not just the ones someone remembered.
        // Non-uniform scale shears the card off perpendicular-to-eye, which
        // is correct: the geometry it stands in for is sheared identically.
        //
        // world -> object for a DIRECTION needs inverse(mat3(model)), and
        // transpose is the inverse only for an orthonormal basis. With
        // model = R * S the columns carry the scales, so peel them off:
        // inverse(R * S) = S^-1 * transpose(R). The old code used transpose
        // alone, which under scale(s, s*h, s) left the elevation off by h^2
        // and picked the wrong ring for exactly the trees it mis-sized.
        const vec3 col_len = vec3(length(model[0].xyz), length(model[1].xyz),
                                  length(model[2].xyz));
        const vec3 inv_col = vec3(col_len.x > 1e-8 ? 1.0 / col_len.x : 0.0,
                                  col_len.y > 1e-8 ? 1.0 / col_len.y : 0.0,
                                  col_len.z > 1e-8 ? 1.0 / col_len.z : 0.0);
        const mat3 rot = mat3(model[0].xyz * inv_col.x,
                              model[1].xyz * inv_col.y,
                              model[2].xyz * inv_col.z);
        // `v * m` is transpose(m) * v in GLSL; the component-wise inv_col
        // that follows is the S^-1.
        const vec3 eye_obj = (to_eye * rot) * inv_col;
        const float eye_obj_len = length(eye_obj);
        const vec3 eye_obj_dir = eye_obj_len > 1e-5 ? eye_obj / eye_obj_len
                                                    : vec3(0.0, 0.0, 1.0);

        // SPHERICAL billboarding: the card faces the eye outright, tilting as
        // the camera rises. It was cylindrical when the atlas held one equator
        // ring -- a tilting card would have drawn the equator's image where
        // the top belonged -- and the two changed together (impostor_bake.h,
        // "ELEVATION ROWS"). A vertical card seen from 60 degrees up is
        // foreshortened to half the height of the mesh it replaced; this is
        // the half of that fix that lives in the geometry.
        //
        // The frame is built exactly as the bake builds its own -- right from
        // cross(up, view_z), up from cross(view_z, right) -- so the card's
        // frame and the cell's frame differ only by the view quantisation,
        // which is the same approximation the azimuth ring always made. It is
        // built around OBJECT up, because that is the axis the bake's rings
        // were swept about.
        const vec3 up_obj = vec3(0.0, 1.0, 0.0);
        // Degenerate when the eye is straight overhead: cross(up, eye)
        // vanishes and the card would collapse. The bake never hits this (its
        // top ring is 60 degrees, where |cross| is still 0.5) but the eye is
        // not quantised, so the runtime must handle it. Fall back to a fixed
        // azimuth -- at straight-down the choice is arbitrary by definition,
        // and an arbitrary card beats a NaN one.
        vec3 right_axis = cross(up_obj, eye_obj_dir);
        const float right_len = length(right_axis);
        right_axis = right_len > 1e-4 ? right_axis / right_len
                                      : vec3(1.0, 0.0, 0.0);
        const vec3 card_up = cross(eye_obj_dir, right_axis);
        // THE CARD SITS AT THE OBJECT'S NEAR BOUND, not through its centre.
        //
        // gbuffer.frag declares `layout(depth_less)`, so a fragment may only
        // push its depth AWAY from the camera. The baked depth is measured
        // from the near bound, so a card there can only ever push backward and
        // the write is legal by construction. A card through the centre would
        // need to pull the front half forward, which that declaration forbids
        // -- and relaxing it would cost early-Z for every surface in the frame.
        //
        // Moving the card toward the eye would also make it subtend a larger
        // angle, so the quad is scaled by (D - r) / D to land on exactly the
        // same pixels it covered before. That is exact for a perspective
        // projection: a quad at D-r of half-size h(D-r)/D subtends what one of
        // half-size h subtends at D.
        // World length of the push. The card moves along eye_obj_dir by
        // half_extent in OBJECT space, so the distance it actually travels is
        // the model's scale along that direction -- not the largest column.
        // Those differ by heightScale for a tree, and using the column would
        // over-shrink every tall one.
        const float push_world =
            length(mat3(model) * eye_obj_dir) * half_extent;
        const float eye_dist = length(to_eye);
        const float near_shrink =
            eye_dist > 1e-4 ? clamp((eye_dist - push_world) / eye_dist,
                                    0.05, 1.0)
                            : 1.0;
        const vec3 corner_local = center_local + eye_obj_dir * half_extent +
                                  (right_axis * sign_xy.x +
                                   card_up * sign_xy.y) *
                                      (half_extent * near_shrink);
        world = model * vec4(corner_local, 1.0);

        // Which baked view. The ring was baked in OBJECT space, so the eye
        // direction had to come back through the placement before its azimuth
        // meant anything -- otherwise a rotated placement shows the wrong face
        // of the rock. eye_obj_dir above is that direction.
        //
        // The exact inverse of the bake's
        //   view_z = (sin(a)*cos(e), sin(e), cos(a)*cos(e))
        // atan2(x,z) ignores y, so the azimuth is unaffected by elevation.
        float azimuth = atan(eye_obj_dir.x, eye_obj_dir.z);
        const float kTwoPi = 6.28318530717958647692;
        if (azimuth < 0.0) azimuth += kTwoPi;
        const uint az_i = uint(floor(azimuth / kTwoPi * float(IMPOSTOR_AZIMUTHS)
                                     + 0.5)) % IMPOSTOR_AZIMUTHS;
        // NEAREST ring. Below the equator and above the top ring both clamp:
        // the rings cover 0..60 degrees and a camera outside that range gets
        // the closest one it has, which is the documented limit rather than a
        // wrap into the wrong hemisphere.
        const float elevation = asin(clamp(eye_obj_dir.y, -1.0, 1.0));
        const float ring = floor(elevation / float(IMPOSTOR_ELEV_STEP) + 0.5);
        const uint el_i = uint(clamp(ring, 0.0, float(IMPOSTOR_ELEVATIONS - 1u)));
        impostor_view = float(el_i * IMPOSTOR_AZIMUTHS + az_i);   // view_index()
        // ---- the BAKED view's frame, rebuilt exactly as the bake built it --
        //
        // The card faces the EYE; the cell was rendered from the nearest
        // QUANTISED direction. Those differ by up to half an azimuth step
        // (11.25 degrees), and that difference is the entire source of
        // parallax -- a camera-facing card sampled in its own frame has none
        // by construction, which is why rotating around a tree used to snap
        // between cells instead of sliding.
        //
        // So the cell uv is no longer the corner's sign pair. It is the
        // corner PROJECTED INTO THE BAKED FRAME, which is the same mapping
        // impostor_bake.cpp used on the geometry:
        //   px = (nx * 0.5 + 0.5) * C,  py = (0.5 - ny * 0.5) * C
        // That also absorbs the near-bound shift above for free -- the card
        // moved along the eye, and this projection reports where that lands
        // in the baked image.
        const float baked_az = float(az_i) * (kTwoPi / float(IMPOSTOR_AZIMUTHS));
        const float baked_el = float(el_i) * float(IMPOSTOR_ELEV_STEP);
        const float baked_ce = cos(baked_el);
        const vec3 view_b = vec3(sin(baked_az) * baked_ce, sin(baked_el),
                                 cos(baked_az) * baked_ce);
        vec3 right_b = cross(vec3(0.0, 1.0, 0.0), view_b);
        const float right_b_len = length(right_b);
        right_b = right_b_len > 1e-4 ? right_b / right_b_len
                                     : vec3(1.0, 0.0, 0.0);
        const vec3 up_b = cross(view_b, right_b);

        const float inv_h = half_extent > 1e-6 ? 1.0 / half_extent : 0.0;
        const vec3 rel = corner_local - center_local;
        impostor_uv = vec2(dot(rel, right_b) * inv_h * 0.5 + 0.5,
                           0.5 - dot(rel, up_b) * inv_h * 0.5);

        // A surface t behind the card along the EYE ray lands at
        //   project_baked(card_point - eye * t)
        // and t = d * 2 * half_extent for stored depth d, so the uv shift is
        // exactly parallax_dir * d with no further scaling. The y term flips
        // because the mapping above flips v.
        impostor_parallax = vec4(-dot(eye_obj_dir, right_b),
                                 dot(eye_obj_dir, up_b),
                                 2.0 * push_world, 0.0);
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
    out_impostor_parallax = impostor_parallax;
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
