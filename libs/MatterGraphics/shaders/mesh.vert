#version 460
#extension GL_GOOGLE_include_directive : require
#include "raster_common.glsl"
#include "impostor_raster.glsl"
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec4 tangent;
layout(location=3) in vec2 uv;
layout(location=4) in vec2 base_uv;
layout(location=0) out vec3 normal_ws;
layout(location=1) out vec3 world_position;
layout(location=2) out vec4 tangent_ws;
layout(location=3) out vec2 tex_uv;
layout(location=4) out vec2 base_tex_uv;
layout(location=5) flat out uint draw_id;
layout(location=6) out vec4 surface;
layout(location=7) flat out vec4 tint;
layout(location=8) out vec4 parallax;
layout(location=9) flat out vec3 basis_x;
layout(location=10) flat out vec3 basis_y;
void main() {
    RasterDraw draw = draws[raster_push.draw_index];
    draw_id = raster_push.draw_index;
    vec4 world = draw.model * vec4(position,1);
    surface = vec4(0); tint = vec4(0); parallax = vec4(0);
    basis_x = normalize(draw.model[0].xyz);
    basis_y = normalize(draw.model[1].xyz);
    if (draw.ids.z != 0u) {
        uint corner = uint(gl_VertexIndex);
        vec2 signs = vec2((corner==1u || corner==2u)?1:-1,corner>=2u?1:-1);
        vec3 local = draw.card.xyz + vec3(signs*draw.card.w,0);
        ImpostorPlacement card = place_impostor(draw.model,local,
            vec4(1e30,float(corner),draw.card.w,1),camera.eye.xyz);
        world = card.world;
        surface = vec4(1e30,card.uv,1);
        uint slot = draw.ids.z-1u;
        tint = vec4(float(slot&255u),float(slot>>8u),card.view,0);
        parallax = card.parallax;
    }
    // Portable transforms use inverse transpose; legacy raster.vert is intact.
    normal_ws = mat3(draw.normal_matrix)*normal;
    tangent_ws = vec4(mat3(draw.model)*tangent.xyz,
                      tangent.w*sign(determinant(mat3(draw.model))));
    world_position = world.xyz;
    tex_uv = uv; base_tex_uv = base_uv;
    gl_Position = camera.world_to_clip*world;
}
