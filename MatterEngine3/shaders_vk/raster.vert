#version 460

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_albedo;
layout(location = 3) in vec4 in_orm;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec4 out_albedo;
layout(location = 2) out vec4 out_orm;
layout(location = 3) out vec3 out_velocity_valid;

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
    uint pad0;
    uint pad1;
    uint pad2;
};

layout(set = 1, binding = 3, std430) readonly buffer DrawTransforms {
    DrawTransform transforms[];
};

void main() {
    // gl_InstanceIndex already includes VkDrawIndirectCommand::firstInstance.
    // Adding gl_BaseInstance would index the Task 7 transform array twice.
    DrawTransform draw = transforms[gl_InstanceIndex];
    mat4 model = draw.current;
    vec4 world = model * vec4(in_position, 1.0);
    vec4 current_clip = frame.world_to_clip * world;
    vec4 previous_clip = frame.previous_world_to_clip *
                         draw.previous * vec4(in_position, 1.0);
    gl_Position = current_clip;
    bool valid = frame.temporal.y == 0u && draw.history_valid != 0u &&
                 current_clip.w != 0.0 && previous_clip.w != 0.0;
    out_velocity_valid = vec3(
        valid ? (current_clip.xy / current_clip.w -
                 previous_clip.xy / previous_clip.w) * 0.5 *
                vec2(frame.temporal.zw)
              : vec2(0.0),
        valid ? 1.0 : 0.0);
    out_normal = normalize(mat3(model) * in_normal);
    out_albedo = in_albedo;
    out_orm = in_orm;
}
