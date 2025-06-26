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
// === BEGIN INCLUDE: blas_tlas_common.glsl ===
// BLAS/TLAS Common Shader Code
// This file contains all the BVH traversal logic and data structures

// TLAS/BLAS data uniforms
uniform int triangleCount;     // Total number of triangles
uniform int blasNodeCount;     // Total number of BLAS nodes
uniform int tlasNodeCount;     // Number of TLAS nodes
uniform int instanceCount;     // Number of instances

uniform sampler2D trianglesTexture;    // All triangle data (14x4)
uniform sampler2D blasNodesTexture;    // All BLAS nodes (10x3) 
uniform sampler2D tlasNodesTexture;    // TLAS nodes (11x3)
uniform sampler2D instancesTexture;    // Instance transforms (6x8)

// Control uniforms
uniform int intersectionMode;    // 0=brute force, 1=TLAS/BLAS traversal

// Structures for TLAS/BLAS system

struct Triangle {
    vec3 v0, v1, v2;
    vec3 normal;
    int  materialId;
};

struct BLASNode {
    vec3 aabbMin;
    vec3 aabbMax;
    int  leftFirst;    // Left child index (internal) or first triangle (leaf)
    int  triCount;     // Triangle count (>0 for leaf, 0 for internal)
};

struct TLASNode {
    vec3 aabbMin;
    vec3 aabbMax;
    int  leftRight;    // Packed left (16-bit) and right (16-bit) child indices, 0 for leaf
    int  blasIndex;    // BLAS index for leaf nodes, 0 for internal nodes
};

struct Instance {
    mat4 transform;
    mat4 invTransform;
    int  blasStartIndex;  // Starting index in the combined BLAS nodes array
};

struct HitResult {
    bool  hit;
    float t;
    vec3  position;
    vec3  normal;
    int   material;
    int   instanceId;
};

// Decode triangle from texture
Triangle decodeTriangle(int triangleIndex) {
    Triangle tri;
    
    // Each triangle uses 4 texels (rows)
    float triTexCoord = (float(triangleIndex) + 0.5) / float(triangleCount);
    
    vec4 data0 = texture(trianglesTexture, vec2(triTexCoord, 0.125));  // v0 + materialId
    vec4 data1 = texture(trianglesTexture, vec2(triTexCoord, 0.375));  // v1
    vec4 data2 = texture(trianglesTexture, vec2(triTexCoord, 0.625));  // v2
    vec4 data3 = texture(trianglesTexture, vec2(triTexCoord, 0.875));  // normal
    
    tri.v0 = data0.xyz;
    tri.materialId = int(data0.w);
    tri.v1 = data1.xyz;
    tri.v2 = data2.xyz;
    tri.normal = data3.xyz;
    
    return tri;
}

// Decode BLAS node from texture
BLASNode decodeBLASNode(int nodeIndex) {
    BLASNode node;
    
    float nodeTexCoord = (float(nodeIndex) + 0.5) / float(blasNodeCount);
    
    vec4 data0 = texture(blasNodesTexture, vec2(nodeTexCoord, 0.125));  // aabbMin + leftFirst
    vec4 data1 = texture(blasNodesTexture, vec2(nodeTexCoord, 0.375));  // aabbMax + triCount
    
    node.aabbMin = data0.xyz;
    node.leftFirst = int(data0.w);
    node.aabbMax = data1.xyz;
    node.triCount = int(data1.w);
    
    return node;
}

// Decode TLAS node from texture
TLASNode decodeTLASNode(int nodeIndex) {
    TLASNode node;
    
    float nodeTexCoord = (float(nodeIndex) + 0.5) / float(tlasNodeCount);
    
    vec4 data0 = texture(tlasNodesTexture, vec2(nodeTexCoord, 0.125));  // aabbMin + leftRight
    vec4 data1 = texture(tlasNodesTexture, vec2(nodeTexCoord, 0.375));  // aabbMax + blasIndex
    
    node.aabbMin = data0.xyz;
    node.leftRight = int(data0.w);
    node.aabbMax = data1.xyz;
    node.blasIndex = int(data1.w);
    
    return node;
}

// Decode instance from texture
Instance decodeInstance(int instanceIndex) {
    Instance inst;
    
    float instTexCoord = (float(instanceIndex) + 0.5) / float(instanceCount);
    
    // Load transform matrix (4 rows) - 9 total rows, so each row is 1/9 = 0.1111
    vec4 row0 = texture(instancesTexture, vec2(instTexCoord, 0.0556));  // Row 0 (0.5/9)
    vec4 row1 = texture(instancesTexture, vec2(instTexCoord, 0.1667));  // Row 1 (1.5/9)
    vec4 row2 = texture(instancesTexture, vec2(instTexCoord, 0.2778));  // Row 2 (2.5/9)
    vec4 row3 = texture(instancesTexture, vec2(instTexCoord, 0.3889));  // Row 3 (3.5/9)
    
    inst.transform = mat4(row0, row1, row2, row3);
    
    // Load inverse transform matrix (4 rows)
    vec4 invRow0 = texture(instancesTexture, vec2(instTexCoord, 0.5000)); // Row 4 (4.5/9)
    vec4 invRow1 = texture(instancesTexture, vec2(instTexCoord, 0.6111)); // Row 5 (5.5/9)
    vec4 invRow2 = texture(instancesTexture, vec2(instTexCoord, 0.7222)); // Row 6 (6.5/9)
    vec4 invRow3 = texture(instancesTexture, vec2(instTexCoord, 0.8333)); // Row 7 (7.5/9)
    
    inst.invTransform = mat4(invRow0, invRow1, invRow2, invRow3);
    
    // Load metadata (BLAS start index + instance ID)
    vec4 metadata = texture(instancesTexture, vec2(instTexCoord, 0.9444)); // Row 8 (8.5/9)
    inst.blasStartIndex = int(metadata.x);  // Read BLAS start index from texture
    
    return inst;
}

// Ray-AABB intersection
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

// Ray-triangle intersection using Möller-Trumbore algorithm
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

// Transform ray to instance space
void transformRay(vec3 rayOrigin, vec3 rayDir, mat4 invTransform, out vec3 localOrigin, out vec3 localDir) {
    vec4 localOrigin4 = invTransform * vec4(rayOrigin, 1.0);
    vec4 localDir4 = invTransform * vec4(rayDir, 0.0);
    
    localOrigin = localOrigin4.xyz;
    localDir = normalize(localDir4.xyz);
}

// Transform normal from instance space to world space
vec3 transformNormal(vec3 normal, mat4 transform) {
    // Use transpose of inverse for normal transformation (simplified for demo)
    vec4 worldNormal4 = transpose(transform) * vec4(normal, 0.0);
    return normalize(worldNormal4.xyz);
}

// BLAS traversal for a specific instance
HitResult intersectBLAS(vec3 localRayOrigin, vec3 localRayDir, int instanceIndex, Instance inst, float maxT) {
    HitResult result;
    result.hit = false;
    result.t = maxT;
    result.instanceId = instanceIndex;
    
    // Stack for iterative BLAS traversal
    int stack[32];
    int stackPtr = 0;
    
    // Start with BLAS root node (instance's BLAS start index)
    stack[stackPtr++] = inst.blasStartIndex;
    
    while (stackPtr > 0) {
        int nodeIndex = stack[--stackPtr];
        BLASNode node = decodeBLASNode(nodeIndex);
        
        // Test ray against BLAS node AABB
        if (!rayAABBIntersect(localRayOrigin, localRayDir, node.aabbMin, node.aabbMax, result.t)) {
            continue;
        }
        
        if (node.triCount > 0) {
            // Leaf node - test triangles
            for (int i = 0; i < node.triCount; i++) {
                int triIndex = node.leftFirst + i;
                Triangle tri = decodeTriangle(triIndex);
                
                float t;
                if (rayTriangleIntersect(localRayOrigin, localRayDir, tri.v0, tri.v1, tri.v2, t) && t < result.t) {
                    result.hit = true;
                    result.t = t;
                    result.position = localRayOrigin + localRayDir * t;
                    result.normal = tri.normal;
                    result.material = tri.materialId;
                }
            }
        } else {
            // Internal node - add children to stack
            if (stackPtr < 30) { // Leave room for both children
                stack[stackPtr++] = node.leftFirst + 1; // Right child
                stack[stackPtr++] = node.leftFirst;     // Left child
            }
        }
    }
    
    return result;
}

// TLAS/BLAS traversal
HitResult intersectTLAS(vec3 rayOrigin, vec3 rayDir) {
    HitResult closest;
    closest.hit = false;
    closest.t = 1000000.0;
    closest.material = -1;
    closest.instanceId = -1;
    
    // Stack for iterative TLAS traversal
    int stack[32];
    int stackPtr = 0;
    
    // Start with TLAS root node
    stack[stackPtr++] = 0;
    
    while (stackPtr > 0) {
        int nodeIndex = stack[--stackPtr];
        TLASNode node = decodeTLASNode(nodeIndex);
        
        // Test ray against TLAS node AABB
        if (!rayAABBIntersect(rayOrigin, rayDir, node.aabbMin, node.aabbMax, closest.t)) {
            continue;
        }
        
        if (node.leftRight == 0) {
            // Leaf node - intersect with instance
            int instanceIndex = node.blasIndex;
            Instance inst = decodeInstance(instanceIndex);
            
            // Transform ray to instance space
            vec3 localRayOrigin, localRayDir;
            transformRay(rayOrigin, rayDir, inst.invTransform, localRayOrigin, localRayDir);
            
            // Intersect with BLAS in instance space
            HitResult blasHit = intersectBLAS(localRayOrigin, localRayDir, instanceIndex, inst, closest.t);
            
            if (blasHit.hit && blasHit.t < closest.t) {
                // Transform hit point and normal back to world space
                vec4 worldPos4 = inst.transform * vec4(blasHit.position, 1.0);
                vec3 worldNormal = transformNormal(blasHit.normal, inst.invTransform);
                
                closest.hit = true;
                closest.t = blasHit.t;
                closest.position = worldPos4.xyz;
                closest.normal = worldNormal;
                closest.material = blasHit.material;
                closest.instanceId = instanceIndex;
            }
        } else {
            // Internal node - extract and add children to stack
            int leftChild = node.leftRight & 0xFFFF;           // Lower 16 bits
            int rightChild = (node.leftRight >> 16) & 0xFFFF; // Upper 16 bits
            
            if (stackPtr < 30) { // Leave room for both children
                stack[stackPtr++] = rightChild; // Right child first
                stack[stackPtr++] = leftChild;  // Left child second (processed first)
            }
        }
    }
    
    return closest;
}

// Brute force triangle intersection (for comparison)
HitResult intersectBruteForce(vec3 rayOrigin, vec3 rayDir) {
    HitResult closest;
    closest.hit = false;
    closest.t = 1000000.0;
    closest.material = -1;
    closest.instanceId = -1;
    
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
            closest.instanceId = 0; // Assume first instance for brute force
        }
    }
    
    return closest;
}

// Unified intersection interface
HitResult intersectScene(vec3 rayOrigin, vec3 rayDir) {
    if (intersectionMode == 0) {
        return intersectBruteForce(rayOrigin, rayDir);
    } else {
        return intersectTLAS(rayOrigin, rayDir);
    }
}
// === END INCLUDE: blas_tlas_common.glsl ===

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
