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
// === BEGIN INCLUDE: bvh_tlas_common.glsl ===
// Enhanced BVH traversal based on proven bvh_article implementation
// Data structures and algorithms ported from OpenCL version

// TLAS/BLAS data uniforms
uniform int triangleCount;     // Total number of triangles
uniform int blasNodeCount;     // Total number of BLAS nodes
uniform int tlasNodeCount;     // Number of TLAS nodes
uniform int instanceCount;     // Number of instances

uniform sampler2D trianglesTexture;    // All triangle data
uniform sampler2D blasNodesTexture;    // All BLAS nodes
uniform sampler2D tlasNodesTexture;    // TLAS nodes
uniform sampler2D instancesTexture;    // Instance transforms

// Control uniforms
uniform int intersectionMode;    // 0=brute force, 1=TLAS/BLAS traversal

// Ray tracing structures
struct Intersection
{
    float t;         // intersection distance along ray
    float u, v;      // barycentric coordinates of the intersection
    uint instPrim;   // instance index (12 bit) and primitive index (20 bit)
};

struct Ray
{
    vec3 O, D, rD;   // origin, direction, reciprocal direction
    Intersection hit;
};

struct Triangle
{
    vec3 v0, v1, v2; // triangle vertices
    vec3 center;     // for BVH construction (renamed from centroid - reserved keyword)
};

struct BVHNode
{
    vec3 aabbMin;
    uint leftFirst;  // left child index or first triangle index
    vec3 aabbMax;
    uint triCount;   // triangle count (0 for interior nodes)
};

struct TLASNode  
{
    vec3 aabbMin;
    uint leftRight;  // packed left/right child indices (16 bits each)
    vec3 aabbMax;
    uint BLAS;       // BLAS index for leaf nodes
};

struct BVHInstance
{
    // Transform matrix as 16 individual floats (row-major like reference)
    float transform[16];
    float invTransform[16];
    uint blasIndex;
    uint materialId;
};

struct HitResult
{
    bool hit;
    float t;
    vec3 position;
    vec3 normal;
    int material;
    int instanceId;
};

// Random number generation (ported from tools.cl)
uint WangHash(uint s) 
{
    s = (s ^ 61u) ^ (s >> 16);
    s *= 9u;
    s = s ^ (s >> 4);
    s *= 0x27d4eb2du;
    s = s ^ (s >> 15);
    return s;
}

uint RandomInt(inout uint s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

float RandomFloat(inout uint s)
{
    return float(RandomInt(s)) * 2.3283064365387e-10; // = 1 / (2^32-1)
}

// Triangle intersection using Moeller-Trumbore (from kernels.cl)
void IntersectTri(inout Ray ray, Triangle tri, uint instPrim)
{
    vec3 edge1 = tri.v1 - tri.v0;
    vec3 edge2 = tri.v2 - tri.v0;
    vec3 h = cross(ray.D, edge2);
    float a = dot(edge1, h);
    
    if (a > -0.00001 && a < 0.00001) return; // ray parallel to triangle
    
    float f = 1.0 / a;
    vec3 s = ray.O - tri.v0;
    float u = f * dot(s, h);
    
    if (u < 0.0 || u > 1.0) return;
    
    vec3 q = cross(s, edge1);
    float v = f * dot(ray.D, q);
    
    if (v < 0.0 || u + v > 1.0) return;
    
    float t = f * dot(edge2, q);
    
    if (t > 0.0001 && t < ray.hit.t)
    {
        ray.hit.t = t;
        ray.hit.u = u;
        ray.hit.v = v;
        ray.hit.instPrim = instPrim;
    }
}

// AABB intersection (from kernels.cl)
float IntersectAABB(Ray ray, vec3 aabbMin, vec3 aabbMax)
{
    float tx1 = (aabbMin.x - ray.O.x) * ray.rD.x;
    float tx2 = (aabbMax.x - ray.O.x) * ray.rD.x;
    float tmin = min(tx1, tx2);
    float tmax = max(tx1, tx2);
    
    float ty1 = (aabbMin.y - ray.O.y) * ray.rD.y;
    float ty2 = (aabbMax.y - ray.O.y) * ray.rD.y;
    tmin = max(tmin, min(ty1, ty2));
    tmax = min(tmax, max(ty1, ty2));
    
    float tz1 = (aabbMin.z - ray.O.z) * ray.rD.z;
    float tz2 = (aabbMax.z - ray.O.z) * ray.rD.z;
    tmin = max(tmin, min(tz1, tz2));
    tmax = min(tmax, max(tz1, tz2));
    
    if (tmax >= tmin && tmin < ray.hit.t && tmax > 0.0) 
        return tmin;
    else 
        return 1e30;
}

// Decode triangle from texture (optimized layout)
Triangle decodeTriangle(int triangleIndex)
{
    Triangle tri;
    
    float triTexCoord = (float(triangleIndex) + 0.5) / float(triangleCount);
    
    vec4 data0 = texture(trianglesTexture, vec2(triTexCoord, 0.125));  // v0 + materialId
    vec4 data1 = texture(trianglesTexture, vec2(triTexCoord, 0.375));  // v1
    vec4 data2 = texture(trianglesTexture, vec2(triTexCoord, 0.625));  // v2
    vec4 data3 = texture(trianglesTexture, vec2(triTexCoord, 0.875));  // centroid + normal
    
    tri.v0 = data0.xyz;
    tri.v1 = data1.xyz;
    tri.v2 = data2.xyz;
    tri.center = data3.xyz;
    
    return tri;
}

// Decode BVH node from texture (matches BVHNode struct)
BVHNode decodeBVHNode(int nodeIndex)
{
    BVHNode node;
    
    float nodeTexCoord = (float(nodeIndex) + 0.5) / float(blasNodeCount);
    
    vec4 data0 = texture(blasNodesTexture, vec2(nodeTexCoord, 0.125));  // aabbMin + leftFirst
    vec4 data1 = texture(blasNodesTexture, vec2(nodeTexCoord, 0.375));  // aabbMax + triCount
    
    node.aabbMin = data0.xyz;
    node.leftFirst = uint(data0.w);
    node.aabbMax = data1.xyz;
    node.triCount = uint(data1.w);
    
    return node;
}

// Decode TLAS node from texture (matches TLASNode struct)
TLASNode decodeTLASNode(int nodeIndex)
{
    TLASNode node;
    
    float nodeTexCoord = (float(nodeIndex) + 0.5) / float(tlasNodeCount);
    
    vec4 data0 = texture(tlasNodesTexture, vec2(nodeTexCoord, 0.125));  // aabbMin + leftRight
    vec4 data1 = texture(tlasNodesTexture, vec2(nodeTexCoord, 0.375));  // aabbMax + BLAS
    
    node.aabbMin = data0.xyz;
    node.leftRight = uint(data0.w);
    node.aabbMax = data1.xyz;
    node.BLAS = uint(data1.w);
    
    return node;
}

// Decode instance from texture (matches BVHInstance struct)
BVHInstance decodeInstance(int instanceIndex)
{
    BVHInstance inst;
    
    float instTexCoord = (float(instanceIndex) + 0.5) / float(instanceCount);
    
    // Load transform matrix (4 rows, stored as individual floats)
    vec4 row0 = texture(instancesTexture, vec2(instTexCoord, 0.0556));
    vec4 row1 = texture(instancesTexture, vec2(instTexCoord, 0.1667));
    vec4 row2 = texture(instancesTexture, vec2(instTexCoord, 0.2778));
    vec4 row3 = texture(instancesTexture, vec2(instTexCoord, 0.3889));
    
    // Store as individual floats in row-major order
    inst.transform[0] = row0.x; inst.transform[1] = row0.y; inst.transform[2] = row0.z; inst.transform[3] = row0.w;
    inst.transform[4] = row1.x; inst.transform[5] = row1.y; inst.transform[6] = row1.z; inst.transform[7] = row1.w;
    inst.transform[8] = row2.x; inst.transform[9] = row2.y; inst.transform[10] = row2.z; inst.transform[11] = row2.w;
    inst.transform[12] = row3.x; inst.transform[13] = row3.y; inst.transform[14] = row3.z; inst.transform[15] = row3.w;
    
    // Load inverse transform matrix (4 rows)
    vec4 invRow0 = texture(instancesTexture, vec2(instTexCoord, 0.5000));
    vec4 invRow1 = texture(instancesTexture, vec2(instTexCoord, 0.6111));
    vec4 invRow2 = texture(instancesTexture, vec2(instTexCoord, 0.7222));
    vec4 invRow3 = texture(instancesTexture, vec2(instTexCoord, 0.8333));
    
    // Store as individual floats in row-major order  
    inst.invTransform[0] = invRow0.x; inst.invTransform[1] = invRow0.y; inst.invTransform[2] = invRow0.z; inst.invTransform[3] = invRow0.w;
    inst.invTransform[4] = invRow1.x; inst.invTransform[5] = invRow1.y; inst.invTransform[6] = invRow1.z; inst.invTransform[7] = invRow1.w;
    inst.invTransform[8] = invRow2.x; inst.invTransform[9] = invRow2.y; inst.invTransform[10] = invRow2.z; inst.invTransform[11] = invRow2.w;
    inst.invTransform[12] = invRow3.x; inst.invTransform[13] = invRow3.y; inst.invTransform[14] = invRow3.z; inst.invTransform[15] = invRow3.w;
    
    // Load metadata (currently using blas_start_index as blasIndex for compatibility)
    vec4 metadata = texture(instancesTexture, vec2(instTexCoord, 0.9444));
    inst.blasIndex = uint(metadata.x); // Actually blas_start_index for now
    inst.materialId = uint(metadata.y); // Will be 0 for now
    
    return inst;
}

// Transform vector (ported from tools.cl)
vec3 transformVector(vec3 v, float T[16])
{
    return vec3(
        dot(vec3(T[0], T[1], T[2]), v),
        dot(vec3(T[4], T[5], T[6]), v),
        dot(vec3(T[8], T[9], T[10]), v)
    );
}

// Transform position (ported from tools.cl)
vec3 transformPosition(vec3 v, float T[16])
{
    return vec3(
        dot(vec3(T[0], T[1], T[2]), v) + T[3],
        dot(vec3(T[4], T[5], T[6]), v) + T[7],
        dot(vec3(T[8], T[9], T[10]), v) + T[11]
    );
}

// Transform ray to local space (ported from tools.cl)
void transformRay(inout Ray ray, float invTransform[16])
{
    // Transform direction and origin using reference functions
    ray.D = transformVector(ray.D, invTransform);
    ray.O = transformPosition(ray.O, invTransform);
    
    // Update reciprocals after transformation (crucial!)
    ray.rD = vec3(1.0) / ray.D;
}

// Transform normal from local to world space
vec3 transformNormal(vec3 normal, float transform[16])
{
    // For normals, use transpose of transform (inverse of inverse = original)
    return normalize(vec3(
        dot(vec3(transform[0], transform[4], transform[8]), normal),
        dot(vec3(transform[1], transform[5], transform[9]), normal),
        dot(vec3(transform[2], transform[6], transform[10]), normal)
    ));
}

// BVH traversal (ported from BVH::Intersect in bvh.cpp)
void BVHIntersect(inout Ray ray, uint instanceIdx, uint blasOffset)
{
    // Stack for iterative BVH traversal
    int stack[32];
    int stackPtr = 0;
    
    // Start with root node
    int nodeIdx = int(blasOffset);
    
    while (true)
    {
        BVHNode node = decodeBVHNode(nodeIdx);
        
        if (node.triCount > 0u) // Leaf node
        {
            // Test all triangles in this leaf
            for (uint i = 0u; i < node.triCount; i++)
            {
                // Since triangles are now stored in BVH order, direct indexing works
                uint triIdx = node.leftFirst + i;
                Triangle tri = decodeTriangle(int(triIdx));
                uint instPrim = (instanceIdx << 20u) + triIdx;
                IntersectTri(ray, tri, instPrim);
            }
            
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
            continue;
        }
        
        // Interior node - test both children
        int leftChild = int(node.leftFirst);
        int rightChild = leftChild + 1;
        
        BVHNode leftNode = decodeBVHNode(leftChild);
        BVHNode rightNode = decodeBVHNode(rightChild);
        
        float dist1 = IntersectAABB(ray, leftNode.aabbMin, leftNode.aabbMax);
        float dist2 = IntersectAABB(ray, rightNode.aabbMin, rightNode.aabbMax);
        
        // Sort by distance to process nearest first
        if (dist1 > dist2)
        {
            float tmpDist = dist1; dist1 = dist2; dist2 = tmpDist;
            int tmpIdx = leftChild; leftChild = rightChild; rightChild = tmpIdx;
        }
        
        if (dist1 == 1e30)
        {
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
        }
        else
        {
            nodeIdx = leftChild;
            if (dist2 != 1e30 && stackPtr < 31)
                stack[stackPtr++] = rightChild;
        }
    }
}

// Instance intersection (ported from tools.cl)
void instanceIntersect(inout Ray ray, BVHInstance inst, uint blasIdx)
{
    // Backup ray
    Ray backup = ray;
    
    // Transform ray to instance space
    transformRay(ray, inst.invTransform);
    
    // Intersect with BLAS
    BVHIntersect(ray, blasIdx, inst.blasIndex);
    
    // Restore ray but keep intersection
    backup.hit = ray.hit;
    ray = backup;
}

// TLAS traversal (ported from tools.cl)
void TLASIntersect(inout Ray ray)
{
    // Initialize reciprocals for TLAS traversal
    ray.rD = vec3(1.0) / ray.D;
    
    // Stack for iterative TLAS traversal
    int stack[32];
    int stackPtr = 0;
    
    // Start with TLAS root node
    int nodeIdx = 0;
    
    while (true)
    {
        TLASNode node = decodeTLASNode(nodeIdx);
        
        if (node.leftRight == 0u) // Leaf node
        {
            // Current node is a leaf: intersect instance
            BVHInstance inst = decodeInstance(int(node.BLAS));
            instanceIntersect(ray, inst, node.BLAS);
            
            // Pop a node from the stack; terminate if none left
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
            continue;
        }
        
        // Current node is an interior node: visit child nodes, ordered
        uint leftChild = node.leftRight & 0xFFFFu;
        uint rightChild = (node.leftRight >> 16) & 0xFFFFu;
        
        TLASNode child1 = decodeTLASNode(int(leftChild));
        TLASNode child2 = decodeTLASNode(int(rightChild));
        
        float dist1 = IntersectAABB(ray, child1.aabbMin, child1.aabbMax);
        float dist2 = IntersectAABB(ray, child2.aabbMin, child2.aabbMax);
        
        // Sort by distance to process nearest first
        if (dist1 > dist2)
        {
            float tmpDist = dist1; dist1 = dist2; dist2 = tmpDist;
            uint tmpIdx = leftChild; leftChild = rightChild; rightChild = tmpIdx;
        }
        
        if (dist1 == 1e30)
        {
            // Missed both child nodes; pop a node from the stack
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
        }
        else
        {
            // Visit near node; push the far node if the ray intersects it
            nodeIdx = int(leftChild);
            if (dist2 != 1e30 && stackPtr < 31)
                stack[stackPtr++] = int(rightChild);
        }
    }
}

// Main intersection interface
HitResult intersectScene(vec3 rayOrigin, vec3 rayDir)
{
    Ray ray;
    ray.O = rayOrigin;
    ray.D = rayDir;
    ray.rD = vec3(1.0) / rayDir;
    ray.hit.t = 1e30;
    ray.hit.u = 0.0;
    ray.hit.v = 0.0;
    ray.hit.instPrim = 0u;
    
    // Perform TLAS traversal
    TLASIntersect(ray);
    
    HitResult result;
    result.hit = (ray.hit.t < 1e30);
    result.t = ray.hit.t;
    
    if (result.hit)
    {
        result.position = rayOrigin + rayDir * ray.hit.t;
        
        // Extract triangle and instance indices
        uint triIdx = ray.hit.instPrim & 0xFFFFFu;
        uint instIdx = ray.hit.instPrim >> 20;
        
        // Get triangle data for normal calculation
        Triangle tri = decodeTriangle(int(triIdx));
        vec3 normal = normalize(cross(tri.v1 - tri.v0, tri.v2 - tri.v0));
        
        // Transform normal to world space
        BVHInstance inst = decodeInstance(int(instIdx));
        result.normal = transformNormal(normal, inst.invTransform);
        result.material = int(inst.materialId);
        result.instanceId = int(instIdx);
    }
    else
    {
        result.position = vec3(0.0);
        result.normal = vec3(0.0);
        result.material = -1;
        result.instanceId = -1;
    }
    
    return result;
}
// === END INCLUDE: bvh_tlas_common.glsl ===

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

// Sky sampling (ported from raytracer.cl)
vec3 sampleSky(vec3 direction) {
    // Procedural sky for now (could use skyTexture in the future)
    float phi = atan(direction.z, direction.x);
    float theta = acos(direction.y);
    
    // Simple gradient sky
    float skyFactor = (direction.y + 1.0) * 0.5;
    vec3 skyColor = mix(vec3(0.2, 0.4, 0.8), vec3(0.8, 0.9, 1.0), skyFactor);
    
    // Add sun effect
    vec3 sunDir = normalize(vec3(0.5, 0.7, 0.3));
    float sunDot = max(0.0, dot(direction, sunDir));
    float sun = pow(sunDot, 256.0);
    skyColor += vec3(1.0, 0.9, 0.7) * sun;
    
    return skyColor * 0.65;
}

// Advanced raytracing with reflections (ported from raytracer.cl)
vec3 trace(vec3 rayOrigin, vec3 rayDirection, uint seed) {
    vec3 rayPos = rayOrigin;
    vec3 rayDir = rayDirection;
    vec3 color = vec3(0.0);
    vec3 attenuation = vec3(1.0);
    
    for (int rayDepth = 0; rayDepth < 4; rayDepth++) {
        HitResult hit = intersectScene(rayPos, rayDir);
        
        if (!hit.hit) {
            // Hit sky
            color += attenuation * sampleSky(rayDir);
            break;
        }
        
        // DEBUG: If we found any hit, show a bright color
        if (rayDepth == 0 && gl_FragCoord.x < 100.0 && gl_FragCoord.y < 100.0) {
            return vec3(0.0, 1.0, 0.0); // Bright green for hits in top-left corner
        }
        
        // Get triangle for normal calculation and material properties
        vec3 hitPos = rayPos + rayDir * hit.t;
        vec3 normal = hit.normal;
        
        // Material properties based on instance ID (simulate material variation)
        bool isMirror = (hit.instanceId * 17) % 2 == 1;
        
        if (isMirror) {
            // Mirror reflection
            if (rayDepth == 1) {
                // Quick sky sample for depth 1 reflections
                vec3 reflectedDir = rayDir - 2.0 * normal * dot(normal, rayDir);
                color += attenuation * sampleSky(reflectedDir);
                break;
            }
            
            // Calculate reflection ray
            rayDir = rayDir - 2.0 * normal * dot(normal, rayDir);
            rayPos = hitPos + rayDir * 0.005; // Offset to avoid self-intersection
            
            // Slight attenuation for reflections
            attenuation *= 0.9;
        } else {
            // Diffuse material - calculate lighting
            vec3 lightVec = lightPos - hitPos;
            float lightDist = length(lightVec);
            vec3 lightDir = lightVec / lightDist;
            
            // Base material color
            vec3 albedo;
            if (hit.material == 0) albedo = vec3(0.8, 0.2, 0.2);      // Red
            else if (hit.material == 1) albedo = vec3(0.2, 0.2, 0.8); // Blue
            else if (hit.material == 2) albedo = vec3(0.2, 0.8, 0.2); // Green
            else if (hit.material == 3) albedo = vec3(0.8, 0.8, 0.2); // Yellow
            else if (hit.material == 4) albedo = vec3(0.8, 0.2, 0.8); // Magenta
            else albedo = vec3(0.5);                                   // Gray
            
            // Lighting calculation
            float lightFalloff = 1.0 / (lightDist * lightDist);
            float diffuse = max(0.0, dot(normal, lightDir));
            
            color += attenuation * albedo * (ambient + diffuse * lightColor * lightFalloff);
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
    
    // Multiple samples for antialiasing (like raytracer.cl)
    vec3 color = vec3(0.0);
    for (int i = 0; i < 2; i++) {
        // Add random offset for antialiasing
        vec2 pixelOffset = vec2(RandomFloat(seed), RandomFloat(seed));
        vec2 uv = ((gl_FragCoord.xy + pixelOffset) / screenSize) * 2.0 - 1.0;
        uv.x *= screenSize.x / screenSize.y;
        
        // Compute ray direction
        float fovScale = tan(radians(cameraFovy) * 0.5);
        vec3 rayDir = normalize(uv.x * right * fovScale + uv.y * up * fovScale + forward);
        
        // Trace the ray
        color += trace(cameraPos, rayDir, seed);
    }
    
    // Average the samples
    color *= 0.5;
    
    // Gamma correction
    color = pow(color, vec3(1.0/2.2));
    
    // DEBUG: Add a test pattern to verify shader is working
    vec2 testUV = gl_FragCoord.xy / screenSize;
    if (testUV.x < 0.1 && testUV.y < 0.1) {
        color = vec3(1.0, 0.0, 0.0); // Red square in top-left corner
    }
    
    finalColor = vec4(color, 1.0);
}
