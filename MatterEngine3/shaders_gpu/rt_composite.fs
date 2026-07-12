#version 460
in vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;
uniform sampler2D u_shadow;
uniform float     u_shadow_strength;

void main() {
    vec3 scene  = texture(u_scene, v_uv).rgb;
    float shadow = texture(u_shadow, v_uv).r;
    float factor = mix(1.0, shadow, u_shadow_strength);
    frag_color = vec4(scene * factor, 1.0);
}
