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

// Global-illumination feature flags (set from the ImGui lighting panel). Lowering
// these trades softer fill light for deeper contrast and fewer secondary rays.
uniform float giStrength;      // 0 = skip the indirect bounce ray entirely; 1 = full
uniform float shadowStrength;  // 0 = no shadow darkening; 1 = fully black shadows
uniform int   aoEnabled;       // 0 = skip AO rays (return unoccluded)

// Enhanced lighting for better shadow visibility
vec3 lightPos   = vec3(3.0, 8.0, 2.0);            // Lower sun position for better shadows
vec3 lightColor = vec3(4.0, 3.8, 3.5);          // Brighter direct lighting
vec3 ambient    = vec3(0.34, 0.34, 0.33);          // Brighter neutral fill so light stone/marble reads

// Light cache for performance optimization
struct LightCache {
    vec3 indirectLight;
    float ambientOcclusion;
    bool valid;
};

// Simple spatial hashing for light cache (crude approximation)
uint getGridHash(vec3 pos) {
    ivec3 gridPos = ivec3(floor(pos * 2.0)); // 0.5 unit grid cells
    return uint(gridPos.x * 73 + gridPos.y * 137 + gridPos.z * 281);
}

// Sky texture (placeholder - could be added as uniform)
uniform sampler2D skyTexture;


// Include comprehensive material properties system
#include "materials.glsl"

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

#include "lighting.glsl"

// Raytracing with shadows and reflections (performance balanced)
vec3 trace(vec3 rayOrigin, vec3 rayDirection, inout uint seed) {
    vec3 rayPos = rayOrigin;
    vec3 rayDir = rayDirection;
    vec3 color = vec3(0.0);
    vec3 attenuation = vec3(1.0);
    
    // Atmospheric parameters - much more transparent haze
    float fogDensity = 0.00005;
    vec3  fogColor   = vec3(0.8, 0.8, 0.9);
    int   MAX_DEPTH  = 2;

    for (int rayDepth = 0; rayDepth < MAX_DEPTH; rayDepth++) { // Limited to 2 bounces for performance
        HitResult hit = intersectScene(rayPos, rayDir);
        
        if (!hit.hit) {
            // Hit sky
            vec3 skyColor = sampleSky(rayDir);
            
            // Enhanced sun disk with realistic intensity
            vec3 sunDir = normalize(lightPos);
            float sunDot = max(0.0, dot(rayDir, sunDir));
            
            // Large soft sun disk
            float sunIntensity = pow(sunDot, 128.0);
            vec3 sunColor = vec3(1.2, 1.0, 0.8) * sunIntensity * 2.0;
            
            // Bright core
            float sunCore = pow(sunDot, 512.0);
            sunColor += vec3(1.5, 1.3, 1.0) * sunCore * 3.0;
            
            color += attenuation * (skyColor + sunColor);
            break;
        }

        // Imposters carry real albedo (tint/tintAlpha) and a world-space normal,
        // so they fall through to the standard PBR lighting path below.

        //return vec3(1.0,0.0,0.0);

        // Get intersection details
        vec3 hitPos = rayPos + rayDir * hit.t;
        vec3 normal = normalize(hit.normal);
        
        // Add fog based on distance
        float distance = hit.t;
        float fogFactor = 1.0 - exp(-fogDensity * distance * distance);
        
        // Get material properties from the material system
        MaterialProperties matProps = getMaterialProperties(hit.material);
        vec3 albedo = mix(matProps.albedo, hit.tint, hit.tintAlpha);
        float roughness = matProps.roughness;
        float metallic = matProps.metallic;
        bool isMirror = (metallic > 0.5 && roughness < 0.3);
        
        // Add emission contribution
        if (matProps.emission > 0.0) {
            vec3 emissionColor = matProps.albedo * matProps.emission;
            color += attenuation * emissionColor;
        }
        
        // Handle different material types based on properties
        if (matProps.translucency > 0.0 && rayDepth < MAX_DEPTH-1) {
            // Translucent material - handle both reflection and transmission
            vec3 directLight = calculatePBR(hitPos, normal, rayDir, albedo, roughness, metallic, hit.ao, rayDepth == 0, seed);
            
            // Determine if ray is entering or exiting the material
            bool entering = dot(rayDir, normal) < 0.0;
            vec3 n = entering ? normal : -normal;
            float eta = entering ? (1.0 / matProps.ior) : matProps.ior;
            
            // Calculate Fresnel reflectance
            float fresnelReflectance = fresnel(rayDir, n, matProps.ior);
            
            // Calculate reflection direction for glass surfaces
            vec3 reflectedDir = roughnessReflect(rayDir, n, roughness, seed);
            
            // Refract the ray
            vec3 refractedDir = refract(rayDir, n, eta);
            
            if (length(refractedDir) > 0.0) {
                // Both reflection and refraction are possible
                float transmittance = (1.0 - fresnelReflectance) * matProps.translucency;
                float reflectance = fresnelReflectance;
                
                // Add surface lighting contribution
                color += attenuation * directLight * (1.0 - transmittance - reflectance * 0.1);
                
                // Sample reflection contribution by casting a reflection ray
                // We'll accumulate this immediately rather than recursing
                if (reflectance > 0.01 && rayDepth < MAX_DEPTH-2) {
                    vec3 reflectionPos = hitPos + n * 0.001;
                    HitResult reflectionHit = intersectScene(reflectionPos, reflectedDir);
                    
                    if (reflectionHit.hit) {
                        // Get material properties for reflected surface (works for imposters too:
                        // imposter sets tintAlpha=1 so mix() returns the baked albedo directly).
                        MaterialProperties reflMatProps = getMaterialProperties(reflectionHit.material);
                        vec3 reflAlbedo = mix(reflMatProps.albedo, reflectionHit.tint, reflectionHit.tintAlpha);
                        vec3 reflNormal = reflectionHit.normal;

                        // Calculate direct lighting on reflected surface
                        vec3 reflectedLight = calculatePBR(reflectionHit.position, reflNormal, reflectedDir,
                                                         reflAlbedo, reflMatProps.roughness, reflMatProps.metallic, reflectionHit.ao, false, seed);

                        // Add reflection contribution with proper energy conservation
                        color += attenuation * reflectedLight * albedo * reflectance;
                    } else {
                        // Reflection hits sky
                        vec3 skyReflection = sampleSky(reflectedDir);
                        color += attenuation * skyReflection * reflectance;
                    }
                }
                
                // Continue with refracted ray for transmission
                rayDir = refractedDir;
                rayPos = hitPos - n * 0.001; // Offset in direction of refraction
                attenuation *= albedo * transmittance; // Tint transmitted light
                
                // Beer's law absorption for colored glass
                if (!entering && matProps.translucency > 0.5) {
                    // Inside glass - apply absorption based on distance traveled
                    // This creates the colored glass effect
                    float glassThickness = 0.1; // Approximate thickness
                    vec3 absorption = exp(-glassThickness * (1.0 - albedo) * 2.0);
                    attenuation *= absorption;
                }
            } else {
                // Total internal reflection - treat as perfect mirror
                rayDir = reflectedDir;
                rayPos = hitPos + n * 0.001;
                attenuation *= albedo * 0.95; // Slight energy loss
            }
        } else if (isMirror && rayDepth < MAX_DEPTH-1) {
            // Reflective material
            vec3 directLight = calculatePBR(hitPos, normal, rayDir, albedo, roughness, metallic, hit.ao, rayDepth == 0, seed);
            
            // Calculate Fresnel for reflection
            float NdotV = max(0.0, dot(normal, -rayDir));
            vec3 F0 = mix(vec3(0.04), albedo, metallic);
            vec3 fresnelVec = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);
            float fresnelStrength = (fresnelVec.r + fresnelVec.g + fresnelVec.b) / 3.0;
            
            // Energy conservation: balance direct and reflected light
            float reflectionWeight = fresnelStrength * (1.0 - roughness);
            
            // Add direct lighting with proper energy conservation
            color += attenuation * directLight * (1.0 - reflectionWeight * 0.7);
            
            // Continue ray for reflection with roughness-based perturbation
            vec3 reflectedDir = roughnessReflect(rayDir, normal, roughness, seed);
            rayDir = reflectedDir;
            rayPos = hitPos + normal * 0.001;
            attenuation *= fresnelVec * (1.0 - roughness) * 0.6;
        } else {
            // Opaque material - calculate full PBR lighting and terminate
            vec3 materialColor = calculatePBR(hitPos, normal, rayDir, albedo, roughness, metallic, hit.ao, rayDepth == 0, seed);
            
            // Apply atmospheric fog
            materialColor = mix(materialColor, fogColor, fogFactor);
            
            color += attenuation * materialColor;
            break;
        }
        
        // Early termination if attenuation is too low
        if (length(attenuation) < 0.01) {
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
    
    // Debug mode: visualize triangle test counts
    if (debugTriangleTests == 1) {
        HitResult debugHit = intersectScene(cameraPos, rayDir);
        vec3 debugColor = triangleTestCountToColor(debugHit.triangleTests);
        finalColor = vec4(debugColor, 1.0);
        return;
    }
    
    // Trace the ray
    vec3 color = trace(cameraPos, rayDir, seed);
    
    // Tone mapping in linear space FIRST, then gamma. (Doing gamma before
    // Reinhard crushes the whole range into a muddy mid-gray band.)
    color = color / (color + vec3(1.0)); // Simple Reinhard tone mapping
    color = pow(color, vec3(1.0/2.2));   // Gamma correction

    // Add subtle color grading
    color = pow(color, vec3(1.05)); // Slight contrast adjustment

    finalColor = vec4(color, 1.0);
}
