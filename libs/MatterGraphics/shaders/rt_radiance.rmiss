#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

#include "rt_surface_common.glsl"

layout(location = 1) rayPayloadInEXT RtSurfacePayload surface_payload;

void main() {
    // Environment radiance is evaluated in raygen from the miss direction;
    // keep the shared surface payload deterministically invalid here.
    surface_payload.surface = invalid_rt_surface();
    surface_payload.part_slot = 0xffffffffu;
    surface_payload.primitive = 0xffffffffu;
}
