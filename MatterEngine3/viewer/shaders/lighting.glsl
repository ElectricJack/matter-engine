// Shared PBR shading + sky + sampling helpers, extracted from
// raytrace_tlas_blas.fs so the imposter bake shader can reuse the exact
// same lighting. Relies on the includer having already declared:
//   - includes: materials.glsl, bvh_tlas_common.glsl (getMaterialProperties,
//     intersectScene, RandomFloat, HitResult, MaterialProperties, intersectionMode)
//   - inputs: getGridHash(), lightPos/lightColor/ambient,
//     giStrength/shadowStrength/aoEnabled

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

    // Any-hit test: stop at the first occluder between surface and light. Same
    // predicate as the old closest-hit test (hit && t < lightDist - bias), but
    // it skips the closest-hit decode shadow rays never use.
    if (shadowQuery(shadowRayOrigin, lightDir, lightDist - bias)) {
        return 1.0 - shadowStrength; // In shadow; higher shadowStrength = deeper
    }

    return 1.0; // Fully lit
}

// Efficient indirect lighting approximation with adaptive sampling
vec3 calculateIndirectLighting(vec3 hitPos, vec3 normal, vec3 albedo, inout uint seed) {
    vec3 indirectLight = vec3(0.0);
    if (giStrength <= 0.0) return indirectLight; // GI off: skip the secondary bounce ray

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
        
        // Cast indirect ray bounded to maxDistance: seeding the traversal cutoff
        // prunes the BVH past this range instead of tracing to infinity and
        // discarding far hits afterward.
        float maxDistance = 4.0;
        HitResult indirectHit = intersectScene(hitPos + normal * 0.001, sampleDir, maxDistance);

        if (indirectHit.hit) {
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
    return indirectLight * albedo * giStrength; // scaled by the GI feature flag
}

// Enhanced PBR lighting with indirect illumination
vec3 calculatePBR(vec3 hitPos, vec3 normal, vec3 viewDir, vec3 albedo, float roughness, float metallic, float bakedAO, bool doIndirect, inout uint seed) {
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
    
    // Baked ambient occlusion (precomputed per-vertex, interpolated at the hit).
    float ao = bakedAO;
    
    // Indirect lighting (a full bounce ray). Only fire it on the primary hit;
    // on reflected/secondary surfaces its contribution is barely visible and not
    // worth another scene traversal.
    vec3 indirectContribution = doIndirect ? calculateIndirectLighting(hitPos, normal, albedo, seed) : vec3(0.0);
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
