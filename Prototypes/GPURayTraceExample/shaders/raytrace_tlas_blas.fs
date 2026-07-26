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

// Enhanced lighting for better shadow visibility
vec3 lightPos   = vec3(3.0, 8.0, 2.0);            // Lower sun position for better shadows
vec3 lightColor = vec3(4.0, 3.8, 3.5);          // Brighter direct lighting
vec3 ambient    = vec3(0.1, 0.15, 0.2);            // Darker ambient for contrast

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

// Realistic sunny day sky with horizon
vec3 sampleSky(vec3 direction) {
    float height = direction.y;
    
    // Sunny day colors
    vec3 zenithColor = vec3(0.25, 0.5, 1.0);      // Bright blue zenith
    vec3 horizonColor = vec3(0.9, 0.7, 0.5);      // Warm yellow-orange horizon
    vec3 groundColor = vec3(0.3, 0.25, 0.2);      // Brown earth
    
    vec3 skyColor;
    
    if (height > 0.0) {
        // Above horizon - sky
        float t = smoothstep(0.0, 0.6, height);
        skyColor = mix(horizonColor, zenithColor, t);
        
        // Add atmospheric scattering near horizon - more subtle
        float scattering = exp(-height * 3.0);
        skyColor = mix(skyColor, vec3(1.0, 0.8, 0.6), scattering * 0.15);
        
        // Add subtle clouds - less prominent
        float cloudNoise = sin(direction.x * 3.0) * sin(direction.z * 2.0) * 0.1;
        float cloudFactor = smoothstep(0.2, 0.8, height) * max(0.0, cloudNoise);
        skyColor = mix(skyColor, vec3(1.0, 1.0, 0.95), cloudFactor * 0.1);
    } else {
        // Below horizon - ground
        float depth = clamp(-height * 2.0, 0.0, 1.0);
        skyColor = mix(horizonColor * 0.4, groundColor, depth);
    }
    
    return skyColor;
}

// Refraction calculation using Snell's law
vec3 refract(vec3 incident, vec3 normal, float eta) {
    float cosI = -dot(normal, incident);
    float sinT2 = eta * eta * (1.0 - cosI * cosI);
    if (sinT2 > 1.0) {
        return vec3(0.0); // Total internal reflection
    }
    float cosT = sqrt(1.0 - sinT2);
    return eta * incident + (eta * cosI - cosT) * normal;
}

// Fresnel reflectance calculation (Schlick approximation)
float fresnel(vec3 incident, vec3 normal, float ior) {
    float cosI = abs(dot(incident, normal));
    float r0 = (1.0 - ior) / (1.0 + ior);
    r0 = r0 * r0;
    return r0 + (1.0 - r0) * pow(1.0 - cosI, 5.0);
}

// Generate random direction in hemisphere (cosine-weighted sampling)
vec3 sampleHemisphere(vec3 normal, inout uint seed) {
    float u1 = RandomFloat(seed);
    float u2 = RandomFloat(seed);
    
    // Cosine-weighted hemisphere sampling
    float cosTheta = sqrt(u1);
    float sinTheta = sqrt(1.0 - u1);
    float phi = 2.0 * 3.14159 * u2;
    
    vec3 w = normal;
    vec3 u = normalize(cross((abs(w.x) > 0.1 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)), w));
    vec3 v = cross(w, u);
    
    return normalize(u * cos(phi) * sinTheta + v * sin(phi) * sinTheta + w * cosTheta);
}

// Sample GGX/Trowbridge-Reitz microfacet distribution for realistic roughness
vec3 sampleGGX(vec3 normal, float roughness, inout uint seed) {
    float u1 = RandomFloat(seed);
    float u2 = RandomFloat(seed);
    
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    
    // Importance sample GGX distribution
    float cosTheta = sqrt((1.0 - u1) / (1.0 + (alpha2 - 1.0) * u1));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float phi = 2.0 * 3.14159 * u2;
    
    // Convert to world coordinates
    vec3 w = normal;
    vec3 u = normalize(cross((abs(w.x) > 0.1 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)), w));
    vec3 v = cross(w, u);
    
    return normalize(u * cos(phi) * sinTheta + v * sin(phi) * sinTheta + w * cosTheta);
}

// Generate roughness-perturbed reflection direction using microfacet model
vec3 roughnessReflect(vec3 incident, vec3 normal, float roughness, inout uint seed) {
    if (roughness < 0.001) {
        // Perfect mirror - no perturbation
        return incident - 2.0 * normal * dot(normal, incident);
    }
    
    // Sample a microfacet normal using GGX distribution
    vec3 halfVector = sampleGGX(normal, roughness, seed);
    
    // Reflect around the sampled microfacet normal
    vec3 reflectedDir = incident - 2.0 * halfVector * dot(halfVector, incident);
    
    // Ensure the reflection is in the correct hemisphere
    if (dot(reflectedDir, normal) < 0.0) {
        // Fallback to perfect reflection if sampling resulted in invalid direction
        reflectedDir = incident - 2.0 * normal * dot(normal, incident);
    }
    
    return normalize(reflectedDir);
}

// Simple shadow calculation
float calculateShadow(vec3 hitPos, vec3 lightDir, float lightDist, vec3 normal) {
    // Adaptive bias based on surface orientation relative to light
    float bias = max(0.002, 0.01 * (1.0 - abs(dot(normal, lightDir))));
    vec3 shadowRayOrigin = hitPos + normal * bias;
    
    // Test shadow ray
    HitResult shadowHit = intersectScene(shadowRayOrigin, lightDir);
    
    if (shadowHit.hit && shadowHit.t < lightDist - bias) {
        return 0.3; // In shadow
    }
    
    return 1.0; // Fully lit
}

// Efficient indirect lighting approximation with adaptive sampling
vec3 calculateIndirectLighting(vec3 hitPos, vec3 normal, vec3 albedo, inout uint seed) {
    vec3 indirectLight = vec3(0.0);
    
    // Use screen-space hash to reduce samples for similar positions
    uint positionHash = getGridHash(hitPos);
    int sampleCount = 1; // Trimmed: single indirect sample to bound secondary-ray cost
    
    for (int i = 0; i < sampleCount; i++) {
        // Sample hemisphere around normal with importance sampling toward sky
        vec3 sampleDir;
        if (i == 0) {
            // First sample biased toward sky for efficient sky lighting
            float skyBias = 0.7;
            vec3 biasedNormal = normalize(normal + vec3(0.0, skyBias, 0.0));
            sampleDir = sampleHemisphere(biasedNormal, seed);
        } else {
            // Standard hemisphere sampling
            sampleDir = sampleHemisphere(normal, seed);
        }
        
        // Cast indirect ray with limited distance
        float maxDistance = 4.0; // Slightly reduced for performance
        HitResult indirectHit = intersectScene(hitPos + normal * 0.001, sampleDir);
        
        if (indirectHit.hit && indirectHit.t < maxDistance) {
            // Check if hit surface is emissive
            MaterialProperties hitMat = getMaterialProperties(indirectHit.material);
            if (hitMat.emission > 0.0) {
                // Calculate contribution from emissive surface
                vec3 emissionColor = hitMat.albedo * hitMat.emission;
                float distance = indirectHit.t;
                float falloff = 1.0 / (1.0 + distance * distance * 0.15);
                float cosTheta = max(0.0, dot(normal, sampleDir));
                
                // Enhanced emissive contribution
                indirectLight += emissionColor * falloff * cosTheta * 1.5;
            } else {
                // Non-emissive surface - add color bleeding
                vec3 bouncedLight = hitMat.albedo * 0.15; // Increased bounce contribution
                float distance = indirectHit.t;
                float falloff = 1.0 / (1.0 + distance * distance * 0.25);
                float cosTheta = max(0.0, dot(normal, sampleDir));
                
                indirectLight += bouncedLight * falloff * cosTheta * 0.4;
            }
        } else {
            // Ray escaped - add sky contribution as indirect light
            vec3 skyColor = sampleSky(sampleDir);
            float cosTheta = max(0.0, dot(normal, sampleDir));
            indirectLight += skyColor * cosTheta * 0.25;
        }
    }
    
    // Average the samples and apply albedo modulation
    indirectLight /= float(sampleCount);
    return indirectLight * albedo; // Removed extra scaling for more pronounced effect
}

// Fast ambient occlusion approximation with adaptive sampling
float calculateAmbientOcclusion(vec3 hitPos, vec3 normal, inout uint seed) {
    // Use spatial hashing to reduce AO samples in some areas
    uint posHash = getGridHash(hitPos);
    if (posHash % 4u != 0u) {
        // Skip AO calculation for 75% of pixels, return reasonable default
        return 0.8; // Slightly occluded default
    }
    
    float occlusion = 0.0;
    const int AO_SAMPLES = 2; // Trimmed: fewer AO rays to bound secondary-ray cost
    const float AO_RADIUS = 0.4; // Slightly smaller radius
    
    for (int i = 0; i < AO_SAMPLES; i++) {
        // Sample hemisphere with shorter rays for AO
        vec3 sampleDir = sampleHemisphere(normal, seed);
        
        // Cast short ray for occlusion test
        HitResult aoHit = intersectScene(hitPos + normal * 0.002, sampleDir);
        
        if (aoHit.hit && aoHit.t < AO_RADIUS) {
            // Surface is occluded
            float occlusionFactor = 1.0 - (aoHit.t / AO_RADIUS);
            occlusion += occlusionFactor * occlusionFactor; // Non-linear falloff
        }
    }
    
    // Convert occlusion to accessibility
    occlusion /= float(AO_SAMPLES);
    return 1.0 - clamp(occlusion * 0.6, 0.0, 0.4); // Limit AO strength
}

// Enhanced PBR lighting with indirect illumination
vec3 calculatePBR(vec3 hitPos, vec3 normal, vec3 viewDir, vec3 albedo, float roughness, float metallic, inout uint seed) {
    vec3 totalLight = vec3(0.0);
    
    // Primary sun light
    vec3 lightVec = lightPos - hitPos;
    float lightDist = length(lightVec);
    vec3 lightDir = lightVec / lightDist;
    
    // Shadow test
    float shadow = calculateShadow(hitPos, lightDir, lightDist, normal);
    
    if (shadow > 0.0) {
        float NdotL = max(0.0, dot(normal, lightDir));
        float NdotV = max(0.0, dot(normal, -viewDir));
        vec3 halfVec = normalize(lightDir - viewDir);
        float NdotH = max(0.0, dot(normal, halfVec));
        float VdotH = max(0.0, dot(-viewDir, halfVec));
        
        // PBR calculations
        // F0 (base reflectivity)
        vec3 F0 = mix(vec3(0.04), albedo, metallic);
        
        // Fresnel (Schlick approximation)
        vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
        
        // Distribution (GGX/Trowbridge-Reitz)
        float alpha = roughness * roughness;
        float alpha2 = alpha * alpha;
        float denom = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
        float D = alpha2 / (3.14159 * denom * denom);
        
        // Geometry (Smith method)
        float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
        float G1L = NdotL / (NdotL * (1.0 - k) + k);
        float G1V = NdotV / (NdotV * (1.0 - k) + k);
        float G = G1L * G1V;
        
        // BRDF
        vec3 numerator = D * G * F;
        float denominator = 4.0 * NdotV * NdotL + 0.001; // Prevent division by zero
        vec3 specular = numerator / denominator;
        
        // Energy conservation
        vec3 kS = F; // Reflected light
        vec3 kD = vec3(1.0) - kS; // Refracted light
        kD *= 1.0 - metallic; // Metallics don't have diffuse
        
        // Light attenuation
        float lightFalloff = 1.0 / (1.0 + lightDist * lightDist * 0.005);
        
        // Combine diffuse and specular
        vec3 diffuse = kD * albedo / 3.14159; // Lambert BRDF
        totalLight += (diffuse + specular) * lightColor * NdotL * lightFalloff * shadow;
    }
    
    // Calculate ambient occlusion
    float ao = calculateAmbientOcclusion(hitPos, normal, seed);
    
    // Add indirect lighting from emissive surfaces
    vec3 indirectContribution = calculateIndirectLighting(hitPos, normal, albedo, seed);
    totalLight += indirectContribution * ao;
    
    // Enhanced ambient lighting with AO
    vec3 upVector = vec3(0.0, 1.0, 0.0);
    float skyFactor = max(0.0, dot(normal, upVector));
    
    // Sample sky for ambient
    vec3 skyAmbient = sampleSky(normal) * 0.2; // Reduced since we have indirect lighting
    
    // Ground bounce lighting
    vec3 downVector = vec3(0.0, -1.0, 0.0);
    float groundFactor = max(0.0, dot(normal, downVector));
    vec3 groundAmbient = vec3(0.1, 0.08, 0.06) * groundFactor * 0.15;
    
    // Combine ambient terms
    vec3 ambientColor = (skyAmbient * skyFactor + groundAmbient + ambient * 0.3) * ao;
    
    // Apply ambient with material properties
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 ambientFresnel = F0 + (1.0 - F0) * pow(1.0 - max(0.0, dot(normal, -viewDir)), 5.0);
    vec3 ambientKS = ambientFresnel;
    vec3 ambientKD = (1.0 - ambientKS) * (1.0 - metallic);
    
    totalLight += ambientKD * albedo * ambientColor;
    
    return totalLight;
}

// Raytracing with shadows and reflections (performance balanced)
vec3 trace(vec3 rayOrigin, vec3 rayDirection, inout uint seed) {
    vec3 rayPos = rayOrigin;
    vec3 rayDir = rayDirection;
    vec3 color = vec3(0.0);
    vec3 attenuation = vec3(1.0);
    
    // Atmospheric parameters - much more transparent haze
    float fogDensity = 0.00005;
    vec3  fogColor   = vec3(0.8, 0.8, 0.9);
    int   MAX_DEPTH  = 3;
    
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
        
        // Get intersection details
        vec3 hitPos = rayPos + rayDir * hit.t;
        vec3 normal = normalize(hit.normal);
        
        // Debug mode: Show normals as colors
        if (debugMode == 1) {
            // Show interpolated normals as RGB colors (map from [-1,1] to [0,1])
            color += attenuation * (normal * 0.5 + 0.5);
            break;
        } else if (debugMode == 2) {
            // Show face normals as RGB colors (temporarily disabled - requires instPrim field)
            // Note: Face normal visualization disabled due to HitResult struct compatibility
            color += attenuation * (normal * 0.5 + 0.5);  // Fall back to interpolated normals
            break;
        }
        
        // Add fog based on distance
        float distance = hit.t;
        float fogFactor = 1.0 - exp(-fogDensity * distance * distance);
        
        // Get material properties from the material system
        MaterialProperties matProps = getMaterialProperties(hit.material);
        vec3 albedo = matProps.albedo;
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
            vec3 directLight = calculatePBR(hitPos, normal, rayDir, albedo, roughness, metallic, seed);
            
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
                        // Get material properties for reflected surface
                        MaterialProperties reflMatProps = getMaterialProperties(reflectionHit.material);
                        vec3 reflAlbedo = reflMatProps.albedo;
                        vec3 reflNormal = reflectionHit.normal;
                        
                        // Calculate direct lighting on reflected surface
                        vec3 reflectedLight = calculatePBR(reflectionHit.position, reflNormal, reflectedDir, 
                                                         reflAlbedo, reflMatProps.roughness, reflMatProps.metallic, seed);
                        
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
            vec3 directLight = calculatePBR(hitPos, normal, rayDir, albedo, roughness, metallic, seed);
            
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
            vec3 materialColor = calculatePBR(hitPos, normal, rayDir, albedo, roughness, metallic, seed);
            
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
    
    // Trace the ray
    vec3 color = trace(cameraPos, rayDir, seed);
    
    // Gamma correction
    color = pow(color, vec3(1.0/2.2));
    
    // Tone mapping for better color reproduction
    color = color / (color + vec3(1.0)); // Simple Reinhard tone mapping
    
    // Add subtle color grading
    color = pow(color, vec3(1.05)); // Slight contrast adjustment
    
    finalColor = vec4(color, 1.0);
}