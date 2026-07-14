#version 460

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_hdr;

layout(set = 0, binding = 0) uniform sampler2D albedo_texture;
layout(set = 0, binding = 1) uniform sampler2D normal_texture;
layout(set = 0, binding = 2) uniform sampler2D orm_texture;

void main() {
    vec4 albedo = texture(albedo_texture, in_uv);
    vec3 normal_sample = texture(normal_texture, in_uv).xyz;
    float normal_length_squared = dot(normal_sample, normal_sample);
    vec3 normal = normal_length_squared > 1e-20
                    ? normal_sample * inversesqrt(normal_length_squared)
                    : vec3(0.0);
    vec3 orm = texture(orm_texture, in_uv).xyz;
    float direct = max(normal.y, 0.0);
    float roughness_response = mix(1.0, 0.7, orm.y);
    vec3 linear_hdr = albedo.rgb * (0.2 + 0.8 * direct) * roughness_response;
    out_hdr = vec4(linear_hdr, 1.0);
}
