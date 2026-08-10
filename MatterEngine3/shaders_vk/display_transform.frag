#version 460
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler2D linear_hdr_texture;
layout(push_constant) uniform DisplaySettings {
    float exposure_ev;
    // Debug passthrough. > 0.5 writes the composite's output straight to the
    // swapchain, bypassing exposure and the ACES curve. It exists for debug
    // views that ENCODE data in their colour rather than displaying it --
    // composite.frag's debug_view 3.0 packs linear depth across R/G/B, and
    // ACES at 8 bits is not invertible well enough to recover it (at the
    // default -2 EV the whole [0,1] range lands in [0, 0.374] and the slope
    // near black is ~0.05, i.e. one code step is 7% of the range).
    float passthrough;
    // Cancel the hardware OETF when the swapchain is an _SRGB format, so a
    // passthrough byte reads back as the byte the shader intended. Set from
    // VkSceneRenderer::display_pipeline_format_.
    float srgb_output;
} display;

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92,
               pow((c + 0.055) / 1.055, vec3(2.4)),
               step(vec3(0.04045), c));
}

vec3 linear_to_srgb(vec3 c) {
    return mix(c * 12.92,
               1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               step(vec3(0.0031308), c));
}

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
    if (display.passthrough > 0.5) {
        vec3 encoded = clamp(hdr, 0.0, 1.0);
        if (display.srgb_output > 0.5) encoded = srgb_to_linear(encoded);
        out_color = vec4(encoded, 1.0);
        return;
    }
    const int ranks[64] = int[64](37,12,54,1,46,27,61,8,18,43,5,58,31,50,14,40,63,22,35,10,48,3,56,29,16,45,7,60,25,52,11,38,33,0,47,20,57,15,42,30,9,53,24,62,4,36,19,51,41,13,55,28,59,6,44,21,26,49,2,39,17,34,23,32);
    ivec2 pixel = ivec2(floor(gl_FragCoord.xy));
    int rank = ranks[(pixel.y & 7) * 8 + (pixel.x & 7)];
    float d = ((float(rank) - 31.5) / 31.5) * (0.5 / 255.0);
    vec3 code = linear_to_srgb(aces_sdr(hdr));
    vec3 code_dithered = clamp(code + vec3(d), 0.0, 1.0);
    out_color = vec4(display.srgb_output > 0.5
                         ? srgb_to_linear(code_dithered)
                         : code_dithered,
                     1.0);
}
