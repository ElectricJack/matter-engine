// gi_common.glsl -- shared pure helpers for GI compute shaders.
// No layout/binding declarations; include after #version and #extension lines.

float luminance(vec3 value) {
    return dot(value, vec3(0.2126, 0.7152, 0.0722));
}
