#version 460

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_hdr;

layout(set = 0, binding = 0) uniform sampler2D albedo_texture;
layout(set = 0, binding = 1) uniform sampler2D normal_texture;
layout(set = 0, binding = 2) uniform sampler2D orm_texture;

layout(push_constant) uniform SceneLighting {
    vec3 sun_direction;
    float sun_intensity;
    vec3 sun_color;
    float pad0;
    vec3 sky_color;
    float pad1;
} lighting;

void main() {
    vec4 albedo = texture(albedo_texture, in_uv);
    vec3 normal_sample = texture(normal_texture, in_uv).xyz;
    float normal_length_squared = dot(normal_sample, normal_sample);
    vec3 normal = normal_length_squared > 1e-20
                    ? normal_sample * inversesqrt(normal_length_squared)
                    : vec3(0.0);
    vec3 orm = texture(orm_texture, in_uv).xyz;
    vec3 to_sun = normalize(-lighting.sun_direction);
    float direct = max(dot(normal, to_sun), 0.0);
    float roughness = orm.x;
    float metallic = orm.y;
    float ao = orm.z;
    vec3 diffuse = albedo.rgb * (1.0 - metallic);
    vec3 ambient = diffuse * lighting.sky_color * ao;
    vec3 sun = diffuse * lighting.sun_color * direct * lighting.sun_intensity;
    vec3 emission = albedo.rgb * albedo.a;
    vec3 linear_hdr = ambient + sun * mix(1.0, 0.65, roughness) + emission;
    out_hdr = vec4(linear_hdr, 1.0);
}
