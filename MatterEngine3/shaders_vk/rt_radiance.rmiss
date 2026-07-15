#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

#include "rt_surface_common.glsl"

layout(location = 1) rayPayloadInEXT RtSurface surface_payload;

void main() { surface_payload = invalid_rt_surface(); }
