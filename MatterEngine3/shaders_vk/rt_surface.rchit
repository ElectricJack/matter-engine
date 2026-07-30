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
    // WP-G ray cone: propagate the incoming cone to the hit point. The cone
    // is not widened by surface CURVATURE here (Akenine-Moller's beta term) --
    // that needs a per-vertex curvature estimate the render vertex does not
    // carry; the raygen's per-bounce roughness widening is the approximation
    // that stands in for it. Documented in rt_lighting.rgen's cone model.
    surface_payload.surface.cone_width =
        max(surface_payload.cone_width +
                surface_payload.cone_spread * gl_HitTEXT,
            0.0);
}
