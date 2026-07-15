#version 460
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler2D linear_hdr_texture;
layout(push_constant) uniform DisplaySettings { float exposure_ev; } display;

vec3 aces_sdr(vec3 value) {
    vec3 x = max(value * exp2(clamp(display.exposure_ev, -6.0, 6.0)), vec3(0.0));
    vec3 numerator = x * (2.51 * x + 0.03);
    vec3 denominator = x * (2.43 * x + 0.59) + 0.14;
    return clamp(numerator / max(denominator, vec3(1e-6)), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(linear_hdr_texture, in_uv).rgb;
    hdr = vec3(isnan(hdr.x) || isinf(hdr.x) ? 0.0 : hdr.x,
               isnan(hdr.y) || isinf(hdr.y) ? 0.0 : hdr.y,
               isnan(hdr.z) || isinf(hdr.z) ? 0.0 : hdr.z);
    out_color = vec4(aces_sdr(hdr), 1.0);
}
