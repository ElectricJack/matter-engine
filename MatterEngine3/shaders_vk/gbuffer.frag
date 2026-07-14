#version 460

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec4 in_albedo;
layout(location = 2) in vec4 in_orm;

layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_orm;

void main() {
    out_albedo = in_albedo;
    out_normal = vec4(normalize(in_normal), 1.0);
    out_orm = in_orm;
}
