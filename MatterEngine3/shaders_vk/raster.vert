#version 460

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_albedo;
layout(location = 3) in vec4 in_orm;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec4 out_albedo;
layout(location = 2) out vec4 out_orm;

layout(set = 0, binding = 0, std140) uniform FrameConstants {
    mat4 world_to_clip;
    vec4 frustum_planes[6];
    vec4 camera_eye_pixel_budget;
    uvec4 counts;
    uvec4 capacities;
} frame;

layout(set = 1, binding = 3, std430) readonly buffer DrawTransforms {
    mat4 transforms[];
};

void main() {
    // gl_InstanceIndex already includes VkDrawIndirectCommand::firstInstance.
    // Adding gl_BaseInstance would index the Task 7 transform array twice.
    mat4 model = transforms[gl_InstanceIndex];
    vec4 world = model * vec4(in_position, 1.0);
    gl_Position = frame.world_to_clip * world;
    out_normal = normalize(mat3(model) * in_normal);
    out_albedo = in_albedo;
    out_orm = in_orm;
}
