#version 460
#extension GL_GOOGLE_include_directive : require
#include "raster_common.glsl"
#include "impostor_sample.glsl"
layout(set=2,binding=0) uniform sampler2D base_texture;
layout(set=2,binding=1) uniform sampler2D mr_texture;
layout(set=2,binding=2) uniform sampler2D normal_texture;
layout(set=2,binding=3) uniform sampler2D emissive_texture;
layout(set=2,binding=4) uniform sampler2DArray atlas_texture;
layout(location=0) in vec3 normal_ws;
layout(location=1) in vec3 world_position;
layout(location=2) in vec4 tangent_ws;
layout(location=3) in vec2 tex_uv;
layout(location=4) in vec2 base_tex_uv;
layout(location=5) flat in uint draw_id;
layout(location=6) in vec4 surface;
layout(location=7) flat in vec4 tint;
layout(location=8) in vec4 parallax;
layout(location=9) flat in vec3 basis_x;
layout(location=10) flat in vec3 basis_y;
layout(location=0) out vec4 out_albedo;
layout(location=1) out vec4 out_normal;
layout(location=2) out vec4 out_orm;
layout(location=3) out vec4 out_emission;
layout(location=4) out uvec2 out_identity;
layout(depth_less) out float gl_FragDepth;
void main() {
    RasterDraw draw = draws[draw_id];
    MaterialGpu material = materials[draw.ids.x];
    RasterOptions opt = options[draw.ids.x];
    vec4 base = vec4(material.base_roughness.rgb,material.metal_opacity_spec_coat.y)*texture(base_texture,base_tex_uv);
    vec4 mr = texture(mr_texture,tex_uv);
    vec3 emission = material.emission_strength.rgb*material.emission_strength.w*texture(emissive_texture,tex_uv).rgb;
    vec3 dx = dFdx(world_position), dy = dFdy(world_position);
    vec2 du = dFdx(tex_uv), dv = dFdy(tex_uv);
    vec3 n = dot(normal_ws,normal_ws)>1e-16 ? normalize(normal_ws) : normalize(cross(dx,dy))*(gl_FrontFacing?-1:1);
    if (!gl_FrontFacing && opt.flags.y!=0u) n=-n;
    if (opt.flags.z!=0u) {
        vec3 t, b;
        vec3 projected=tangent_ws.xyz-n*dot(n,tangent_ws.xyz);
        if (dot(projected,projected)>1e-16) {
            t=normalize(projected);
            b=cross(n,t)*tangent_ws.w;
        } else {
            float det = du.x*dv.y-du.y*dv.x;
            if (abs(det)>1e-12) {
                vec3 candidate=(dx*dv.y-dy*du.y)/det;
                projected=candidate-n*dot(n,candidate);
                t=dot(projected,projected)>1e-16?normalize(projected):normalize(cross(abs(n.z)<.9?vec3(0,0,1):vec3(0,1,0),n));
                b=cross(n,t)*sign(det)*sign(dot(cross(dx,dy),n));
            } else {
                t=normalize(cross(abs(n.z)<.9?vec3(0,0,1):vec3(0,1,0),n));
                b=cross(n,t);
            }
        }
        vec3 mapped=texture(normal_texture,tex_uv).rgb*2-1;
        mapped.xy*=opt.sampling.x;
        vec3 candidate=t*mapped.x+b*mapped.y+n*mapped.z;
        if(dot(candidate,candidate)>1e-16) n=normalize(candidate);
    }
    float depth=gl_FragCoord.z;
    if (draw.ids.z!=0u) {
        // Descriptor contains this draw's one atlas pair; the shared helper
        // sees local slot zero while tint.z preserves the selected view.
        ImpostorSample sampled=sample_impostor(atlas_texture,material,
            vec4(0,0,tint.z,0),surface,parallax,basis_x,basis_y,world_position,
            camera.eye.xyz,camera.world_to_clip,draw.ids.w,0u);
        base=vec4(sampled.color,1); n=sampled.normal;
        if(sampled.depth>=0) depth=sampled.depth;
    } else if (opt.flags.x!=0u && base.a<opt.sampling.y) discard;
    out_albedo=vec4(base.rgb,opt.flags.x==0u?1:base.a);
    out_normal=vec4(normalize(n),0); // Explicit emission product owns emission.
    out_orm=vec4(material.base_roughness.w*mr.g,material.metal_opacity_spec_coat.x*mr.b,1,1);
    out_emission=vec4(emission,0);
    out_identity=uvec2(draw.ids.x|(draw.ids.z!=0u?IMPOSTOR_IDENTITY_BIT:0u),draw.ids.y);
    gl_FragDepth=depth;
}
