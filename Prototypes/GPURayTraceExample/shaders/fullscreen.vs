#version 330 core

// Raylib standard vertex attributes
in vec3 vertexPosition;    // vertex position (required)
in vec2 vertexTexCoord;    // vertex texture coordinates (required)

// Output to fragment shader
out vec2 fragTexCoord;

void main() {
    // Pass texture coordinates to fragment shader
    fragTexCoord = vertexTexCoord;
    
    // Transform vertex position to screen space 
    gl_Position = vec4(vertexPosition, 1.0);
}