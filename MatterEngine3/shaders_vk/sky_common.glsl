// Shared procedural sky — single source of truth for background, GI bounces,
// and ambient irradiance.  Include from composite.frag and rt_lighting.rgen.
// Caller provides sky_color and sun parameters; no push-constant dependency.

vec3 procedural_sky(vec3 direction, vec3 sky_color) {
    float height = direction.y;
    vec3 zenith   = vec3(0.25, 0.5, 1.0);
    vec3 horizon  = vec3(0.9, 0.7, 0.5);
    vec3 ground   = vec3(0.3, 0.25, 0.2);

    vec3 color;
    if (height > 0.0) {
        float t = smoothstep(0.0, 0.6, height);
        color = mix(horizon, zenith, t);
        float scattering = exp(-height * 3.0);
        color = mix(color, vec3(1.0, 0.8, 0.6), scattering * 0.15);
        float cloud = sin(direction.x * 3.0) * sin(direction.z * 2.0) * 0.1;
        float cf = smoothstep(0.2, 0.8, height) * max(0.0, cloud);
        color = mix(color, vec3(1.0, 1.0, 0.95), cf * 0.1);
    } else {
        float depth = clamp(-height * 2.0, 0.0, 1.0);
        color = mix(horizon * 0.4, ground, depth);
    }
    color *= sky_color / vec3(0.38, 0.43, 0.52);
    return color;
}

vec3 sky_with_sun(vec3 direction, vec3 sky_color,
                  vec3 to_sun, vec3 sun_color, float sun_intensity) {
    vec3 sky = procedural_sky(direction, sky_color);
    float sun_disk = smoothstep(0.99975, 0.99995,
                                dot(direction, normalize(to_sun)));
    return sky + sun_color * sun_intensity * sun_disk;
}

vec3 sky_irradiance(vec3 normal, vec3 sky_color) {
    float t = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    return sky_color * mix(0.2, 1.0, t);
}
