#version 460

layout(push_constant) uniform PC { mat4 world_to_clip; };

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 color;

layout(location = 0) out vec4 v_color;

void main() {
    gl_Position = world_to_clip * vec4(position, 1.0);
    v_color = color;
}
