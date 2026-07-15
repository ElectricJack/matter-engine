#version 460
#extension GL_EXT_ray_tracing : require

struct VisibilityPayload {
    vec3 visibility;
    uint layers;
};

layout(location = 0) rayPayloadInEXT VisibilityPayload payload;

void main() { payload.visibility = vec3(0.0); }
