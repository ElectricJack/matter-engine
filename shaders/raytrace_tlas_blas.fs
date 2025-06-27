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

// Lighting uniforms (ported from raytracer.cl)
vec3 lightPos = vec3(3.0, 10.0, 2.0);
vec3 lightColor = vec3(150.0, 150.0, 120.0);
vec3 ambient = vec3(0.2, 0.2, 0.4);

// Sky texture (placeholder - could be added as uniform)
uniform sampler2D skyTexture;


// Include the enhanced BVH common code (ported from bvh_article)
#include "bvh_tlas_common.glsl"

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

// Enhanced sky sampling with atmospheric effects
vec3 sampleSky(vec3 direction) {
    float height = direction.y;
    
    // Horizon and zenith colors
    vec3 horizonColor = vec3(0.8, 0.7, 0.6);  // Warm horizon
    vec3 zenithColor = vec3(0.2, 0.5, 0.9);   // Blue zenith
    vec3 nadirColor = vec3(0.1, 0.2, 0.3);    // Dark below horizon
    
    // Smooth gradient based on height
    vec3 skyColor;
    if (height > 0.0) {
        // Above horizon - interpolate from horizon to zenith
        float t = pow(height, 0.7); // Non-linear for more natural gradient
        skyColor = mix(horizonColor, zenithColor, t);
    } else {
        // Below horizon - dark ground color
        float t = clamp(-height * 3.0, 0.0, 1.0);
        skyColor = mix(horizonColor * 0.3, nadirColor, t);
    }
    
    // Add atmospheric haze near horizon
    float hazeFactor = exp(-abs(height) * 3.0);
    skyColor = mix(skyColor, vec3(0.9, 0.8, 0.7), hazeFactor * 0.3);
    
    return skyColor;
}

// Optimized shadow calculation
float calculateShadow(vec3 hitPos, vec3 lightDir, float lightDist) {
    // Only test shadows for reasonable distances to avoid performance issues
    if (lightDist > 100.0) return 1.0;
    
    // Offset start position to avoid self-intersection
    vec3 shadowRayOrigin = hitPos + lightDir * 0.01;
    
    // Test shadow ray
    HitResult shadowHit = intersectScene(shadowRayOrigin, lightDir);
    
    if (shadowHit.hit && shadowHit.t < lightDist - 0.01) {
        // In shadow - but add some ambient occlusion softening
        return 0.2; // Soft shadows
    }
    
    return 1.0; // Fully lit
}

// Enhanced lighting calculation with shadows and multiple light contributions
vec3 calculateLighting(vec3 hitPos, vec3 normal, vec3 viewDir, vec3 albedo, float roughness, float metallic) {
    vec3 totalLight = vec3(0.0);
    
    // Primary sun light
    vec3 lightVec = lightPos - hitPos;
    float lightDist = length(lightVec);
    vec3 lightDir = lightVec / lightDist;
    
    // Shadow test
    float shadow = calculateShadow(hitPos, lightDir, lightDist);
    
    if (shadow > 0.0) {
        // Diffuse lighting (Lambert)
        float NdotL = max(0.0, dot(normal, lightDir));
        
        // Specular lighting (Blinn-Phong approximation)
        vec3 halfVec = normalize(lightDir - viewDir);
        float NdotH = max(0.0, dot(normal, halfVec));
        float specPower = mix(32.0, 256.0, 1.0 - roughness);
        float specular = pow(NdotH, specPower) * (1.0 - roughness);
        
        // Light attenuation
        float lightFalloff = 1.0 / (1.0 + lightDist * lightDist * 0.01);
        
        // Combine diffuse and specular
        vec3 diffuseContrib = albedo * NdotL;
        vec3 specularContrib = mix(vec3(0.04), albedo, metallic) * specular;
        
        totalLight += (diffuseContrib + specularContrib) * lightColor * lightFalloff * shadow;
    }
    
    // Ambient lighting with subtle directional bias
    vec3 ambientContrib = albedo * ambient * (0.5 + 0.5 * dot(normal, vec3(0.0, 1.0, 0.0)));
    totalLight += ambientContrib;
    
    // Sky lighting (simple approximation)
    float skyContrib = max(0.0, dot(normal, vec3(0.0, 1.0, 0.0))) * 0.3;
    totalLight += albedo * vec3(0.4, 0.6, 0.8) * skyContrib;
    
    return totalLight;
}

// Raytracing with shadows and reflections (performance balanced)
vec3 trace(vec3 rayOrigin, vec3 rayDirection, uint seed) {
    vec3 rayPos = rayOrigin;
    vec3 rayDir = rayDirection;
    vec3 color = vec3(0.0);
    vec3 attenuation = vec3(1.0);
    
    // Atmospheric parameters
    float fogDensity = 0.03;
    vec3 fogColor = vec3(0.7, 0.8, 0.9);
    
    for (int rayDepth = 0; rayDepth < 3; rayDepth++) { // Reduced from 4 to 3 bounces
        HitResult hit = intersectScene(rayPos, rayDir);
        
        if (!hit.hit) {
            // Hit sky
            vec3 skyColor = sampleSky(rayDir);
            
            // Enhanced sky with sun disk
            vec3 sunDir = normalize(lightPos);
            float sunDot = max(0.0, dot(rayDir, sunDir));
            float sunIntensity = pow(sunDot, 256.0);
            vec3 sunColor = vec3(1.0, 0.9, 0.7) * sunIntensity * 1.5;
            
            color += attenuation * (skyColor + sunColor);
            break;
        }
        
        // Get intersection details
        vec3 hitPos = rayPos + rayDir * hit.t;
        vec3 normal = normalize(hit.normal);
        
        // Add fog based on distance
        float distance = hit.t;
        float fogFactor = 1.0 - exp(-fogDensity * distance * distance);
        
        // Material properties
        vec3 albedo;
        float roughness, metallic;
        bool isMirror = false;
        
        int matId = hit.material;
        if (matId == 0) { // Red metal
            albedo = vec3(0.8, 0.1, 0.1);
            roughness = 0.1;
            metallic = 0.9;
            isMirror = true;
        } else if (matId == 1) { // Blue ceramic
            albedo = vec3(0.1, 0.3, 0.8);
            roughness = 0.8;
            metallic = 0.0;
        } else if (matId == 2) { // Green plastic
            albedo = vec3(0.2, 0.7, 0.2);
            roughness = 0.6;
            metallic = 0.1;
        } else if (matId == 3) { // Gold
            albedo = vec3(1.0, 0.8, 0.2);
            roughness = 0.1;
            metallic = 1.0;
            isMirror = true;
        } else { // Default gray
            albedo = vec3(0.6);
            roughness = 0.5;
            metallic = 0.0;
        }
        
        if (isMirror && rayDepth < 2) { // Limit reflection depth
            // Calculate lighting for the surface
            vec3 directLight = calculateLighting(hitPos, normal, rayDir, albedo, roughness, metallic);
            
            // Calculate reflection
            float fresnel = pow(1.0 - max(0.0, -dot(normal, rayDir)), 2.0);
            fresnel = mix(0.04, 1.0, fresnel);
            
            if (rayDepth == 0) {
                // Primary reflection - mix direct lighting with reflection
                vec3 reflectedDir = rayDir - 2.0 * normal * dot(normal, rayDir);
                rayDir = reflectedDir;
                rayPos = hitPos + normal * 0.001;
                attenuation *= albedo * fresnel * 0.8;
                
                // Add some direct lighting to first bounce
                color += attenuation * directLight * (1.0 - fresnel) * 0.3;
            } else {
                // Secondary reflection - simplified
                rayDir = rayDir - 2.0 * normal * dot(normal, rayDir);
                rayPos = hitPos + normal * 0.001;
                attenuation *= albedo * 0.6;
            }
        } else {
            // Diffuse material - calculate full lighting
            vec3 materialColor = calculateLighting(hitPos, normal, rayDir, albedo, roughness, metallic);
            
            // Apply fog
            materialColor = mix(materialColor, fogColor, fogFactor);
            
            color += attenuation * materialColor;
            break;
        }
    }
    
    return color;
}


void main() {
    // Use gl_FragCoord for reliable screen-space coordinates
    vec2 screenUV = gl_FragCoord.xy / screenSize;
    
    // Initialize RNG seed (similar to raytracer.cl)
    uint seed = WangHash(uint(gl_FragCoord.x + gl_FragCoord.y * screenSize.x) * 17u + 1u);
    
    // Compute camera basis
    vec3 forward = normalize(cameraTarget - cameraPos);
    vec3 right = normalize(cross(forward, cameraUp));
    vec3 up = cross(right, forward);
    
    // Single sample for performance (no anti-aliasing for now)
    vec2 uv = (gl_FragCoord.xy / screenSize) * 2.0 - 1.0;
    uv.x *= screenSize.x / screenSize.y;
    
    // Compute ray direction
    float fovScale = tan(radians(cameraFovy) * 0.5);
    vec3 rayDir = normalize(uv.x * right * fovScale + uv.y * up * fovScale + forward);
    
    // Trace the ray
    vec3 color = trace(cameraPos, rayDir, seed);
    
    // Gamma correction
    color = pow(color, vec3(1.0/2.2));
    
    // Tone mapping for better color reproduction
    color = color / (color + vec3(1.0)); // Simple Reinhard tone mapping
    
    // Add subtle color grading
    color = pow(color, vec3(0.95)); // Slight contrast adjustment
    
    finalColor = vec4(color, 1.0);
}