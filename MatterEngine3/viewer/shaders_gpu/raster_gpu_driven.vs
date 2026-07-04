#version 460
// GPU-driven variant of raster.vs: per-instance transform comes from the
// DrawXforms SSBO (written by cull.comp) indexed by gl_BaseInstance +
// gl_InstanceID instead of a divisor-1 vertex attribute.
layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;

layout(std430, binding = 3) readonly buffer DrawXforms { mat4 xforms[]; };

uniform mat4 mvp;

out vec3 fragNormal;
out vec4 fragTint;
out vec2 fragMatAO;
out vec3 fragWorldPos;

void main() {
    mat4 model = xforms[gl_BaseInstance + gl_InstanceID];
    vec4 world = model * vec4(vertexPosition, 1.0);
    // inverse-transpose-free normal: assumes rigid+uniform-scale placements (true for
    // current worlds); revisit if non-uniform instance scales appear.
    fragNormal   = normalize(mat3(model) * vertexNormal);
    fragTint     = vertexColor;
    fragMatAO    = vertexTexCoord;
    fragWorldPos = world.xyz;
    gl_Position  = mvp * world;
}
