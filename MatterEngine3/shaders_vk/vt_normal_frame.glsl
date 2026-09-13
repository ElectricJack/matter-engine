#ifndef VT_NORMAL_FRAME_GLSL
#define VT_NORMAL_FRAME_GLSL
// Chart-page normals live in this OBJECT-local frame. Rebuild around the
// interpolated local normal, decode, then apply the instance normal matrix.
// +Z is a deterministic, well-conditioned fallback near either X pole.
mat3 vt_normal_frame(vec3 normal_local) {
    vec3 n = normalize(normal_local);
    vec3 a = abs(n.x) > 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 t = normalize(a - n * dot(a, n));
    return mat3(t, cross(n, t), n);
}
vec3 vt_frame_encode(vec3 normal_detail, vec3 normal_base) {
    return transpose(vt_normal_frame(normal_base)) * normalize(normal_detail);
}
vec3 vt_frame_decode(vec3 normal_ts, vec3 normal_base) {
    return normalize(vt_normal_frame(normal_base) * normal_ts);
}
// Full inverse transpose preserves nonsingular nonuniform scale and reflection.
// A singular transform has no normal transform: retain a deterministic finite
// identity fallback instead of feeding NaNs through the G-buffer.
mat3 vt_instance_normal_matrix(mat3 model) {
    mat3 cof = mat3(cross(model[1], model[2]), cross(model[2], model[0]),
                   cross(model[0], model[1]));
    float det = dot(model[0], cof[0]);
    float scale = length(model[0]) * length(model[1]) * length(model[2]);
    if (scale <= 0.0 || abs(det) <= 1e-8 * scale) return mat3(1.0);
    return cof / det;
}
#endif
