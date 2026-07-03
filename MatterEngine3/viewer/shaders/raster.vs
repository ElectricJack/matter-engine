#version 330
// Forward instanced vertex shader. raylib binds: vertexPosition/vertexNormal/
// vertexColor/vertexTexCoord + per-instance mat4 'instanceTransform' (divisor 1),
// and uploads 'mvp' = view*projection (model is identity for instanced draws).
in vec3 vertexPosition;
in vec2 vertexTexCoord;    // x = materialId (-1 sentinel), y = baked vertex AO
in vec3 vertexNormal;
in vec4 vertexColor;       // TriEx tint; a = blend strength vs material albedo
in mat4 instanceTransform;

uniform mat4 mvp;

out vec3 fragNormal;       // world space
out vec4 fragTint;
out vec2 fragMatAO;

void main() {
    mat4 model = instanceTransform;
    vec4 world = model * vec4(vertexPosition, 1.0);
    // inverse-transpose-free normal: assumes rigid+uniform-scale placements (true for
    // current worlds); revisit if non-uniform instance scales appear.
    fragNormal = normalize(mat3(model) * vertexNormal);
    fragTint   = vertexColor;
    fragMatAO  = vertexTexCoord;
    gl_Position = mvp * world;
}
