#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

#define RT_SURFACE_HIT_SHADER 1
#include "rt_surface_common.glsl"

layout(location = 1) rayPayloadInEXT RtSurface surface_payload;
hitAttributeEXT vec2 hit_barycentrics;

void main() { surface_payload = load_rt_surface(hit_barycentrics); }
