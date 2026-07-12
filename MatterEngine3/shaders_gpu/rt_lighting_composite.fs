#version 460
in vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_albedo;    // G-buffer albedo (full res, RGBA8)
uniform sampler2D u_lighting;  // RT lighting output (half res, RGBA16F, bilinear)

void main() {
    // The lighting buffer already contains albedo-modulated radiance from the raygen.
    // We just need tonemapping and gamma correction.
    vec3 lit = texture(u_lighting, v_uv).rgb;

    // Reinhard tonemapping.
    vec3 mapped = lit / (lit + vec3(1.0));
    // Gamma correction.
    mapped = pow(mapped, vec3(1.0 / 2.2));

    frag_color = vec4(mapped, 1.0);
}
