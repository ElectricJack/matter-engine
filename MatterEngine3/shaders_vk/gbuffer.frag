#version 460

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec4 in_albedo;
layout(location = 2) in vec4 in_orm;
layout(location = 3) in vec3 in_velocity_valid;

layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_orm;
layout(location = 3) out vec2 out_velocity;

void main() {
    out_albedo = in_albedo;
    out_normal = vec4(normalize(in_normal), in_orm.w);
    // ORM alpha is reserved/opaque (1.0); emission lives in normal alpha.
    out_orm = vec4(in_orm.xyz, 1.0);
    out_velocity = in_velocity_valid.z > 0.5
                       ? in_velocity_valid.xy
                       : vec2(0.0);
}
