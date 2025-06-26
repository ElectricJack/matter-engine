#version 330 core

in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;

// Camera uniforms
uniform vec3  cameraPos;
uniform vec3  cameraTarget;
uniform vec3  cameraUp;
uniform float cameraFovy;
uniform vec2  screenSize;


// Include the shared BLAS/TLAS common code
#include "blas_tlas_common.glsl"

// Camera ray generation
vec3 computeCameraRay(vec2 uv) {
    // Aspect ratio and FOV calculations
    float aspect = screenSize.x / screenSize.y;
    float fovRadians = radians(cameraFovy);
    float halfHeight = tan(fovRadians * 0.5);
    float halfWidth = aspect * halfHeight;
    
    // Camera coordinate system
    vec3 forward = normalize(cameraTarget - cameraPos);
    vec3 right = normalize(cross(forward, cameraUp));
    vec3 up = cross(right, forward);
    
    // Convert screen coordinates to camera space
    vec3 rayDir = normalize(
        halfWidth * (uv.x * 2.0 - 1.0) * right +
        halfHeight * (uv.y * 2.0 - 1.0) * up +
        forward
    );
    
    return rayDir;
}

// Simple shading function
vec3 shade(HitResult hit, vec3 rayDir) {
    if (!hit.hit) {
        // Sky color
        float skyFactor = (rayDir.y + 1.0) * 0.5;
        return mix(vec3(0.2, 0.4, 0.8), vec3(0.8, 0.9, 1.0), skyFactor);
    }
    
    // Simple lighting based on material ID
    vec3 baseColor;
    if (hit.material == 0) baseColor = vec3(0.8, 0.2, 0.2);      // Red
    else if (hit.material == 1) baseColor = vec3(0.2, 0.2, 0.8); // Blue
    else if (hit.material == 2) baseColor = vec3(0.2, 0.8, 0.2); // Green
    else if (hit.material == 3) baseColor = vec3(0.8, 0.8, 0.2); // Yellow
    else if (hit.material == 4) baseColor = vec3(0.8, 0.2, 0.8); // Magenta
    else baseColor = vec3(0.5);                                   // Gray
    
    // Simple diffuse shading
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float diffuse = max(0.0, dot(hit.normal, lightDir));
    vec3 ambient = vec3(0.2);
    
    return baseColor * (ambient + diffuse * vec3(0.8));
}

void main() {
    // Use gl_FragCoord for reliable screen-space coordinates
    vec2 screenUV = gl_FragCoord.xy / screenSize;
    vec2 uv = screenUV * 2.0 - 1.0;
    uv.x *= screenSize.x / screenSize.y;
    
    // Compute camera basis
    vec3 forward = normalize(cameraTarget - cameraPos);
    vec3 right = normalize(cross(forward, cameraUp));
    vec3 up = cross(right, forward);
    
    // Compute ray direction
    float fovScale = tan(radians(cameraFovy) * 0.5);
    vec3 rayDir = normalize(uv.x * right * fovScale + uv.y * up * fovScale + forward);
    
    // Raytracing
    HitResult hit = intersectScene(cameraPos, rayDir);
    vec3 color = shade(hit, rayDir);
    
    // Gamma correction
    color = pow(color, vec3(1.0/2.2));
    finalColor = vec4(color, 1.0);
}