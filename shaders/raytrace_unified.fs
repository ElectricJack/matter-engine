#version 330 core

in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;

// Camera uniforms
uniform vec3 cameraPos;
uniform vec3 cameraTarget;
uniform vec3 cameraUp;
uniform float cameraFovy;
uniform vec2 screenSize;

// Scene data uniforms
uniform int nodeCount;
uniform int triangleCount;
uniform sampler2D trianglesTexture;  // Triangle data packed in texture
uniform sampler2D bvhNodesTexture;   // BVH nodes packed in texture (optional)

// Control uniforms
uniform int debugMode;           // 0=ray dirs, 1=uv coords, 2=ray dirs alt, 3=hit testing, 4=distance, 5=full raytracing, 6=BVH traversal
uniform int intersectionMode;    // 0=brute force, 1=BVH traversal

// Unified hit result structure
struct HitResult {
    bool hit;
    float t;
    vec3 position;
    vec3 normal;
    int material;
};

// Triangle structure
struct Triangle {
    vec3 v0, v1, v2;
    vec3 normal;
    int materialId;
};

// BVH node structure (for BVH mode)
struct BVHNode {
    vec3 aabbMin;
    vec3 aabbMax;
    bool isLeaf;
    int childOrTriangleStart;
    int triangleCount;
};

// Decode triangle from texture
Triangle decodeTriangle(int triangleIndex) {
    Triangle tri;
    
    // Each triangle uses 4 texels
    // Texel 0: v0.xyz + materialId
    // Texel 1: v1.xyz + padding
    // Texel 2: v2.xyz + padding
    // Texel 3: normal.xyz + padding
    
    float triTexCoord = (float(triangleIndex) + 0.5) / float(triangleCount);
    
    vec4 data0 = texture(trianglesTexture, vec2(triTexCoord, 0.125));
    vec4 data1 = texture(trianglesTexture, vec2(triTexCoord, 0.375));
    vec4 data2 = texture(trianglesTexture, vec2(triTexCoord, 0.625));
    vec4 data3 = texture(trianglesTexture, vec2(triTexCoord, 0.875));
    
    tri.v0 = data0.xyz;
    tri.materialId = int(data0.w);
    tri.v1 = data1.xyz;
    tri.v2 = data2.xyz;
    tri.normal = data3.xyz;
    
    return tri;
}

// Decode BVH node from texture (for BVH mode)
BVHNode decodeBVHNode(int nodeIndex) {
    BVHNode node;
    
    float nodeTexCoord = (float(nodeIndex) + 0.5) / float(nodeCount);
    
    vec4 data0 = texture(bvhNodesTexture, vec2(nodeTexCoord, 0.125)); // Row 0
    vec4 data1 = texture(bvhNodesTexture, vec2(nodeTexCoord, 0.375)); // Row 1
    vec4 data2 = texture(bvhNodesTexture, vec2(nodeTexCoord, 0.625)); // Row 2
    
    node.aabbMin = data0.xyz;
    node.isLeaf = data0.w > 0.5;
    node.aabbMax = data1.xyz;
    node.childOrTriangleStart = int(data1.w);
    node.triangleCount = int(data2.x);
    
    return node;
}

// Material colors (unified)
vec3 getMaterialColor(int materialId) {
    if (materialId == 0) return vec3(1.0, 0.2, 0.2); // Red
    if (materialId == 1) return vec3(0.2, 0.2, 1.0); // Blue
    if (materialId == 2) return vec3(0.2, 1.0, 0.2); // Green
    if (materialId == 3) return vec3(1.0, 1.0, 0.2); // Yellow
    if (materialId == 4) return vec3(1.0, 0.2, 1.0); // Magenta
    return vec3(0.8, 0.8, 0.8); // Default gray
}

// Material properties (unified)
void getMaterialProperties(int materialId, out vec3 color, out float reflectance) {
    if (materialId == 0) { color = vec3(1.0, 0.2, 0.2); reflectance = 0.3; }      // Red
    else if (materialId == 1) { color = vec3(0.2, 0.2, 1.0); reflectance = 0.3; } // Blue
    else if (materialId == 2) { color = vec3(0.2, 1.0, 0.2); reflectance = 0.1; } // Green
    else if (materialId == 3) { color = vec3(1.0, 1.0, 0.2); reflectance = 0.2; } // Yellow
    else if (materialId == 4) { color = vec3(1.0, 0.2, 1.0); reflectance = 0.4; } // Magenta
    else { color = vec3(0.8); reflectance = 0.1; }                                // Default
}

// Ray-AABB intersection (unified)
bool rayAABBIntersect(vec3 rayOrigin, vec3 rayDir, vec3 aabbMin, vec3 aabbMax, float maxT) {
    vec3 invDir = 1.0 / (rayDir + vec3(1e-8)); // Avoid division by zero
    vec3 t1 = (aabbMin - rayOrigin) * invDir;
    vec3 t2 = (aabbMax - rayOrigin) * invDir;
    
    vec3 tmin = min(t1, t2);
    vec3 tmax = max(t1, t2);
    
    float tminMax = max(max(tmin.x, tmin.y), tmin.z);
    float tmaxMin = min(min(tmax.x, tmax.y), tmax.z);
    
    return tmaxMin >= 0.0 && tminMax <= tmaxMin && tminMax <= maxT;
}

// Ray-triangle intersection (unified)
bool rayTriangleIntersect(vec3 rayOrigin, vec3 rayDir, vec3 v0, vec3 v1, vec3 v2, out float t) {
    const float EPSILON = 0.000001;
    
    vec3 edge1 = v1 - v0;
    vec3 edge2 = v2 - v0;
    vec3 h = cross(rayDir, edge2);
    float a = dot(edge1, h);
    
    if (a > -EPSILON && a < EPSILON) return false;
    
    float f = 1.0 / a;
    vec3 s = rayOrigin - v0;
    float u = f * dot(s, h);
    
    if (u < 0.0 || u > 1.0) return false;
    
    vec3 q = cross(s, edge1);
    float v = f * dot(rayDir, q);
    
    if (v < 0.0 || u + v > 1.0) return false;
    
    t = f * dot(edge2, q);
    return t > EPSILON;
}

// Brute force triangle intersection
HitResult intersectBruteForce(vec3 rayOrigin, vec3 rayDir) {
    HitResult closest;
    closest.hit = false;
    closest.t = 1000000.0;
    closest.material = -1;
    
    // Test against all triangles
    for (int i = 0; i < triangleCount; i++) {
        Triangle tri = decodeTriangle(i);
        
        float t;
        if (rayTriangleIntersect(rayOrigin, rayDir, tri.v0, tri.v1, tri.v2, t) && t < closest.t) {
            closest.hit = true;
            closest.t = t;
            closest.position = rayOrigin + rayDir * t;
            closest.normal = tri.normal;
            closest.material = tri.materialId;
        }
    }
    
    return closest;
}

// BVH traversal intersection
HitResult intersectBVH(vec3 rayOrigin, vec3 rayDir) {
    HitResult result;
    result.hit = false;
    result.t = 1000000.0;
    result.material = -1;
    
    // Stack for iterative traversal (limited to 32 levels)
    int stack[32];
    int stackPtr = 0;
    
    // Start with root node
    stack[stackPtr++] = 0;
    
    while (stackPtr > 0) {
        int nodeIndex = stack[--stackPtr];
        BVHNode node = decodeBVHNode(nodeIndex);
        
        // Test ray against node AABB
        if (!rayAABBIntersect(rayOrigin, rayDir, node.aabbMin, node.aabbMax, result.t)) {
            continue;
        }
        
        if (node.isLeaf) {
            // Test against triangles in leaf
            for (int i = 0; i < node.triangleCount; i++) {
                int triIndex = node.childOrTriangleStart + i;
                Triangle tri = decodeTriangle(triIndex);
                
                float t;
                if (rayTriangleIntersect(rayOrigin, rayDir, tri.v0, tri.v1, tri.v2, t) && t < result.t) {
                    result.hit = true;
                    result.t = t;
                    result.position = rayOrigin + rayDir * t;
                    result.normal = tri.normal;
                    result.material = tri.materialId;
                }
            }
        } else {
            // Add children to stack (right child first for left-first traversal)
            if (stackPtr < 30) { // Leave room for both children
                stack[stackPtr++] = node.childOrTriangleStart + 1; // Right child
                stack[stackPtr++] = node.childOrTriangleStart;     // Left child
            }
        }
    }
    
    return result;
}

// Unified intersection interface
HitResult intersectScene(vec3 rayOrigin, vec3 rayDir) {
    if (intersectionMode == 0) {
        return intersectBruteForce(rayOrigin, rayDir);
    } else {
        return intersectBVH(rayOrigin, rayDir);
    }
}

// Unified path tracing (shared shading logic)
vec3 pathTrace(vec3 rayOrigin, vec3 rayDir) {
    vec3 throughput = vec3(1.0);
    vec3 accum = vec3(0.0);
    vec3 currentRayOrigin = rayOrigin;
    vec3 currentRayDir = rayDir;
    
    for (int bounce = 0; bounce < 3; bounce++) {
        HitResult hit = intersectScene(currentRayOrigin, currentRayDir);
        
        if (!hit.hit) {
            // Sky color
            float skyT = max(0.0, currentRayDir.y * 0.5 + 0.5);
            vec3 skyColor = mix(vec3(0.5, 0.7, 1.0), vec3(1.0, 1.0, 1.0), skyT);
            accum += throughput * skyColor;
            break;
        }
        
        // Get material properties
        vec3 materialColor;
        float reflectance;
        getMaterialProperties(hit.material, materialColor, reflectance);
        
        // Simple lighting
        vec3 lightDir = normalize(vec3(1.0, 1.0, -1.0));
        float ndotl = max(0.0, dot(hit.normal, lightDir));
        
        // Shadow ray
        vec3 shadowRayOrigin = hit.position + hit.normal * 0.001;
        HitResult shadowHit = intersectScene(shadowRayOrigin, lightDir);
        float shadow = shadowHit.hit ? 0.3 : 1.0;
        
        // Direct lighting
        vec3 diffuse = materialColor * ndotl * shadow;
        vec3 ambient = materialColor * 0.2;
        vec3 directLight = ambient + diffuse;
        
        accum += throughput * directLight * (1.0 - reflectance);
        
        // Reflection
        if (reflectance > 0.05 && bounce < 2) {
            currentRayDir = reflect(currentRayDir, hit.normal);
            currentRayOrigin = hit.position + hit.normal * 0.001;
            throughput *= reflectance;
        } else {
            break;
        }
    }
    
    return accum;
}

void main() {
    // Use gl_FragCoord for reliable screen-space coordinates
    vec2 screenUV = gl_FragCoord.xy / screenSize;
    vec2 uv = screenUV * 2.0 - 1.0;
    uv.x *= screenSize.x / screenSize.y;
    
    // Debug Mode 1: UV Coordinates visualization
    if (debugMode == 1) {
        vec3 debugColor = vec3(screenUV.y, screenUV.x, 0.0);
        finalColor = vec4(debugColor, 1.0);
        return;
    }
    
    // Compute camera basis
    vec3 forward = normalize(cameraTarget - cameraPos);
    vec3 right = normalize(cross(forward, cameraUp));
    vec3 up = cross(right, forward);
    
    // Compute ray direction
    float fovScale = tan(radians(cameraFovy) * 0.5);
    vec3 rayDir = normalize(uv.x * right * fovScale + uv.y * up * fovScale + forward);
    
    // Debug Mode 2: Ray direction visualization
    if (debugMode == 2) {
        vec3 debugColor = rayDir * 0.5 + 0.5;
        finalColor = vec4(debugColor, 1.0);
        return;
    }
    
    // Unified intersection for all other modes
    HitResult hit = intersectScene(cameraPos, rayDir);
    
    // Debug Mode 3: Hit testing
    if (debugMode == 3) {
        if (hit.hit) {
            // Color based on material
            if (hit.material == 0) finalColor = vec4(1.0, 0.2, 0.2, 1.0); // Red
            else if (hit.material == 1) finalColor = vec4(0.2, 0.2, 1.0, 1.0); // Blue
            else if (hit.material == 2) finalColor = vec4(0.2, 1.0, 0.2, 1.0); // Green
            else if (hit.material == 3) finalColor = vec4(1.0, 1.0, 0.2, 1.0); // Yellow
            else if (hit.material == 4) finalColor = vec4(1.0, 0.2, 1.0, 1.0); // Magenta
            else finalColor = vec4(1.0, 1.0, 1.0, 1.0); // White fallback
        } else {
            // Sky gradient
            float skyT = max(0.0, rayDir.y * 0.5 + 0.5);
            finalColor = vec4(mix(vec3(0.5, 0.7, 1.0), vec3(1.0, 1.0, 1.0), skyT), 1.0);
        }
        return;
    }
    
    // Debug Mode 4: Distance visualization
    if (debugMode == 4) {
        if (hit.hit) {
            float normalized_distance = clamp(hit.t / 10.0, 0.0, 1.0);
            float brightness = 1.0 - normalized_distance;
            finalColor = vec4(vec3(brightness), 1.0);
        } else {
            finalColor = vec4(0.0, 0.0, 0.0, 1.0);
        }
        return;
    }
    
    // Debug Mode 5 & 6: Full raytracing (same implementation for both)
    if (debugMode == 5 || debugMode == 6) {
        vec3 color = pathTrace(cameraPos, rayDir);
        
        // Gamma correction
        color = pow(color, vec3(1.0/2.2));
        finalColor = vec4(color, 1.0);
        return;
    }
    
    // Debug Mode 0: Ray direction as color (default)
    vec3 color = rayDir * 0.5 + 0.5;
    finalColor = vec4(color, 1.0);
}