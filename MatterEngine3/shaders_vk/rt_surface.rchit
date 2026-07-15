#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

#define RT_SURFACE_HIT_SHADER 1
#include "rt_surface_common.glsl"

layout(location = 1) rayPayloadInEXT RtSurfacePayload surface_payload;
hitAttributeEXT vec2 hit_barycentrics;

void main() {
    surface_payload.surface = load_rt_surface(hit_barycentrics);
    surface_payload.part_slot = gl_InstanceCustomIndexEXT;
    surface_payload.primitive = gl_PrimitiveID;
}
