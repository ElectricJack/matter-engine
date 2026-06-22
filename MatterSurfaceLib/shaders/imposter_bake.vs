#version 330 core
in vec3 vertexPosition;   // cage world-space (part-space) position
in vec3 vertexNormal;     // cage outward normal
in vec2 vertexTexCoord;   // cage atlas UV

out vec3 cageWorldPos;
out vec3 cageNormal;

void main() {
    cageWorldPos = vertexPosition;
    cageNormal   = vertexNormal;
    // Rasterize directly in UV space: UV [0,1] -> NDC [-1,1]. The atlas uses a
    // v-down convention (matching the CPU displacement/coverage write), so invert
    // v here: with the readback's ImageFlipVertical this lands each cell's
    // right-triangle in the same texels the CPU disp/coverage occupy.
    gl_Position = vec4(vertexTexCoord.x * 2.0 - 1.0,
                       (1.0 - vertexTexCoord.y) * 2.0 - 1.0, 0.0, 1.0);
}
