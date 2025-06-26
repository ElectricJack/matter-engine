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

// Control uniforms
uniform int debugMode;           // 0=ray dirs, 1=uv coords, 2=TLAS debug, 3=BLAS debug, 4=instance debug, 5=full raytracing

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
    // Use gl_FragCoord for reliable screen-space coordinates (like the working version)
    vec2 screenUV = gl_FragCoord.xy / screenSize;
    vec2 uv = screenUV * 2.0 - 1.0;
    uv.x *= screenSize.x / screenSize.y;
    
    // Debug Mode 1: UV Coordinates visualization
    if (debugMode == 1) {
        vec3 debugColor = vec3(screenUV.y, screenUV.x, 0.0);
        finalColor = vec4(debugColor, 1.0);
        return;
    }
    
    // Compute camera basis (like the working version)
    vec3 forward = normalize(cameraTarget - cameraPos);
    vec3 right = normalize(cross(forward, cameraUp));
    vec3 up = cross(right, forward);
    
    // Compute ray direction (like the working version)
    float fovScale = tan(radians(cameraFovy) * 0.5);
    vec3 rayDir = normalize(uv.x * right * fovScale + uv.y * up * fovScale + forward);
    
    // Debug Mode 2: TLAS debug visualization
    if (debugMode == 2) {
        HitResult hit = intersectTLAS(cameraPos, rayDir);
        if (hit.hit) {
            // Color code by instance ID
            float instanceNorm = float(hit.instanceId) / float(instanceCount);
            finalColor = vec4(instanceNorm, 1.0 - instanceNorm, 0.5, 1.0);
        } else {
            finalColor = vec4(0.1, 0.1, 0.1, 1.0); // Dark gray for no hit
        }
        return;
    }
    
    // Debug Mode 3: BLAS vs TLAS comparison
    if (debugMode == 3) {
        HitResult bruteHit = intersectBruteForce(cameraPos, rayDir);
        HitResult tlasHit = intersectTLAS(cameraPos, rayDir);
        
        if (bruteHit.hit && tlasHit.hit) {
            // Both hit - show green if same material, red if different
            if (bruteHit.material == tlasHit.material) {
                finalColor = vec4(0.0, 1.0, 0.0, 1.0); // Green - match
            } else {
                finalColor = vec4(1.0, 0.0, 0.0, 1.0); // Red - mismatch
            }
        } else if (bruteHit.hit && !tlasHit.hit) {
            finalColor = vec4(1.0, 1.0, 0.0, 1.0); // Yellow - TLAS missed
        } else if (!bruteHit.hit && tlasHit.hit) {
            finalColor = vec4(1.0, 0.0, 1.0, 1.0); // Magenta - TLAS false positive
        } else {
            finalColor = vec4(0.5, 0.7, 1.0, 1.0); // Blue - both miss (sky)
        }
        return;
    }
    
    // Debug Mode 4: Instance ID visualization
    if (debugMode == 4) {
        HitResult hit = intersectTLAS(cameraPos, rayDir);
        if (hit.hit) {
            // Get material color from material ID
            vec3 baseColor;
            if (hit.material == 0) baseColor = vec3(0.8, 0.2, 0.2);      // Red
            else if (hit.material == 1) baseColor = vec3(0.2, 0.2, 0.8); // Blue
            else if (hit.material == 2) baseColor = vec3(0.2, 0.8, 0.2); // Green
            else if (hit.material == 3) baseColor = vec3(0.8, 0.8, 0.2); // Yellow
            else if (hit.material == 4) baseColor = vec3(0.8, 0.2, 0.8); // Magenta
            else baseColor = vec3(0.5);                                   // Gray
            finalColor = vec4(baseColor, 1.0);
        } else {
            finalColor = vec4(0.1, 0.1, 0.1, 1.0); // Dark gray for no hit
        }
        return;
    }
    
    // Debug Mode 5: Full raytracing
    if (debugMode == 5) {
        HitResult hit = intersectScene(cameraPos, rayDir);
        vec3 color = shade(hit, rayDir);
        
        // Gamma correction
        color = pow(color, vec3(1.0/2.2));
        finalColor = vec4(color, 1.0);
        return;
    }
    
    // Debug Mode 0: Ray direction as color (default)
    vec3 color = rayDir * 0.5 + 0.5;
    finalColor = vec4(color, 1.0);
}