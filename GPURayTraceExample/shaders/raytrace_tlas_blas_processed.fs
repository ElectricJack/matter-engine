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
// === BEGIN INCLUDE: materials.glsl ===

// Material Properties System
// Comprehensive material definition with PBR, emission, translucency, and surface properties

struct MaterialProperties
{
    // PBR properties
    vec3 albedo;           // Base color/diffuse reflectance
    float roughness;       // Surface roughness (0 = mirror, 1 = completely rough)
    float metallic;        // Metallic factor (0 = dielectric, 1 = metallic)
    
    // Emission properties
    float emission;        // Emission strength (0 = no emission, >0 = emissive)
    
    // Translucency and refraction
    float translucency;    // Translucency factor (0 = opaque, 1 = fully translucent)
    float ior;             // Index of refraction (1.0 = air, 1.5 = glass, etc.)
    
    // Surface properties
    bool flatShading;      // true = flat shaded, false = smooth normals
};

// Material lookup table - defines all materials used in the scene
MaterialProperties getMaterialProperties(int materialId)
{
    MaterialProperties mat;
    
    // Initialize defaults
    mat.albedo = vec3(0.5, 0.5, 0.5);
    mat.roughness = 0.5;
    mat.metallic = 0.0;
    mat.emission = 0.0;
    mat.translucency = 0.0;
    mat.ior = 1.0;

    // If the materialId is greater than 1M, then that's a flag to enable
    // smooth shading. Otherwise it's flat shading.
    int smooth_normals_offset = 1000000;
    mat.flatShading = true;
    if (materialId >= smooth_normals_offset) {
        materialId -= smooth_normals_offset;
        mat.flatShading = false;
    }
    
    // Material definitions based on scene setup
    if (materialId == 0) {
        // Red semi-metallic with slight emission
        mat.albedo = vec3(0.8, 0.2, 0.2);
        mat.roughness = 0.2;
        mat.metallic = 0.6;
        mat.emission = 0.1;  // Slight red glow
        mat.translucency = 0.0;
        mat.ior = 1.0;
    } 
    else if (materialId == 1) {
        // Blue diffuse sphere with smooth normals
        mat.albedo = vec3(0.2, 0.3, 0.8);
        mat.roughness = 0.7;
        mat.metallic = 0.1;
        mat.emission = 0.0;
        mat.translucency = 0.0;
        mat.ior = 1.0;
    } 
    else if (materialId == 2) {
        // Green diffuse ground with flat shading
        mat.albedo = vec3(0.3, 0.7, 0.3);
        mat.roughness = 0.9;
        mat.metallic = 0.0;
        mat.emission = 0.0;
        mat.translucency = 0.0;
        mat.ior = 1.0;
    } 
    else if (materialId == 3) {
        // Yellow/Gold metallic sphere with smooth normals
        mat.albedo = vec3(0.8, 0.7, 0.3);
        mat.roughness = 0.05;
        mat.metallic = 1.0;
        mat.emission = 0.0;
        mat.translucency = 0.0;
        mat.ior = 1.0;
    } 
    else if (materialId == 4) {
        // White translucent glass with smooth normals
        mat.albedo = vec3(0.9, 0.9, 0.9);
        mat.roughness = 0.01;
        mat.metallic = 0.15;
        mat.emission = 0.0;
        mat.translucency = 0.5; // Highly translucent
        mat.ior = 1.5; // Glass IOR
    }
    else if (materialId == 5) {
        // Bright emissive light source
        mat.albedo = vec3(1.0, 0.9, 0.7); // Warm white light
        mat.roughness = 1.0;
        mat.metallic = 0.0;
        mat.emission = 5.0; // Strong emission
        mat.translucency = 0.0;
        mat.ior = 1.0;
    }
    else if (materialId == 6) {
        // Colored glass - green tinted
        mat.albedo = vec3(0.2, 0.9, 0.3);
        mat.roughness = 0.005;
        mat.metallic = 0.15;
        mat.emission = 0.0;
        mat.translucency = 0.5;
        mat.ior = 1.52; // Crown glass IOR
    }
    else if (materialId == 7) {
        // Water-like material
        mat.albedo = vec3(0.2, 0.4, 0.8);
        mat.roughness = 0.0;
        mat.metallic = 0.1;
        mat.emission = 0.0;
        mat.translucency = 1;
        mat.ior = 1.33; // Water IOR
    }
    else {
        // Default gray metallic material
        mat.albedo = vec3(0.6, 0.6, 0.6);
        mat.roughness = 0.1;
        mat.metallic = 0.8;
        mat.emission = 0.0;
        mat.translucency = 0.0;
        mat.ior = 1.0;
    }
    
    return mat;
}

// Utility function to check if a material is emissive
bool isMaterialEmissive(int materialId)
{
    MaterialProperties mat = getMaterialProperties(materialId);
    return mat.emission > 0.0;
}

// Utility function to check if a material is translucent
bool isMaterialTranslucent(int materialId)
{
    MaterialProperties mat = getMaterialProperties(materialId);
    return mat.translucency > 0.0;
}

// Utility function to get emission color
vec3 getMaterialEmission(int materialId)
{
    MaterialProperties mat = getMaterialProperties(materialId);
    return mat.albedo * mat.emission;
}
// === END INCLUDE: materials.glsl ===

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
uniform int debugMode;          // 0=normal rendering, 1=show normals, 2=show face normals

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
    vec3 n0, n1, n2; // per-vertex normals for interpolated shading
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

// Decode triangle from texture (6-row layout with per-vertex normals)
Triangle decodeTriangle(int triangleIndex)
{
    Triangle tri;
    
    float triTexCoord = (float(triangleIndex) + 0.5) / float(triangleCount);
    
    // 6 rows: v0, v1, v2, n0, n1, n2
    vec4 data0 = texture(trianglesTexture, vec2(triTexCoord, 0.0833));  // v0 (row 0: 1/12)
    vec4 data1 = texture(trianglesTexture, vec2(triTexCoord, 0.25));    // v1 (row 1: 3/12)
    vec4 data2 = texture(trianglesTexture, vec2(triTexCoord, 0.4167));  // v2 (row 2: 5/12)
    vec4 data3 = texture(trianglesTexture, vec2(triTexCoord, 0.5833));  // n0 (row 3: 7/12)
    vec4 data4 = texture(trianglesTexture, vec2(triTexCoord, 0.75));    // n1 (row 4: 9/12)
    vec4 data5 = texture(trianglesTexture, vec2(triTexCoord, 0.9167));  // n2 (row 5: 11/12)
    
    tri.v0 = data0.xyz;
    tri.v1 = data1.xyz;
    tri.v2 = data2.xyz;
    tri.n0 = data3.xyz;
    tri.n1 = data4.xyz;
    tri.n2 = data5.xyz;
    
    // Calculate centroid for BVH (not stored in texture anymore)
    tri.center = (tri.v0 + tri.v1 + tri.v2) / 3.0;
    
    return tri;
}

// Interpolate triangle normal using barycentric coordinates
vec3 interpolateNormal(Triangle tri, float u, float v)
{
    float w = 1.0 - u - v;
    return normalize(w * tri.n0 + u * tri.n1 + v * tri.n2);
}

// Get face normal from triangle vertices
vec3 getFaceNormal(Triangle tri)
{
    vec3 edge1 = tri.v1 - tri.v0;
    vec3 edge2 = tri.v2 - tri.v0;
    return normalize(cross(edge1, edge2));
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
        Triangle    tri  = decodeTriangle(int(triIdx));
        BVHInstance inst = decodeInstance(int(instIdx));
        
        // Get material properties to determine shading mode
        MaterialProperties matProps = getMaterialProperties(int(inst.materialId));
        
        vec3 normal;
        if (matProps.flatShading) {
            // Use face normal for flat shading
            normal = normalize(cross(tri.v1 - tri.v0, tri.v2 - tri.v0));
        } else {
            // Transform world hit position to local space
            vec3 localHitPos = transformPosition(result.position, inst.invTransform);
            // Smooth normal is the normalized position from sphere center (origin in local space)
            normal = normalize(localHitPos);
        }
        
        // Transform normal to world space
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

// Hit result structure for ray intersections
struct HitResult {
    bool hit;           // Whether an intersection occurred
    float t;            // Distance along ray
    vec3 position;      // Hit position in world space
    vec3 normal;        // Interpolated surface normal
    int material;       // Material ID for shading
    float u, v;         // Barycentric coordinates for interpolation
};

// Scene intersection function using TLAS/BLAS traversal
HitResult intersectScene(vec3 rayOrigin, vec3 rayDirection) {
    HitResult result;
    result.hit = false;
    result.t = 1e30;
    
    // Create ray for TLAS traversal
    Ray ray;
    ray.O = rayOrigin;
    ray.D = rayDirection;
    ray.rD = vec3(1.0) / rayDirection;
    ray.hit.t = 1e30;
    ray.hit.instPrim = 0u;
    
    // Perform TLAS intersection
    TLASIntersect(ray);
    
    if (ray.hit.t < 1e30) {
        result.hit = true;
        result.t = ray.hit.t;
        result.position = rayOrigin + rayDirection * ray.hit.t;
        result.u = ray.hit.u;
        result.v = ray.hit.v;
        
        // Extract triangle index from instPrim (lower 20 bits)
        uint triangleIndex = ray.hit.instPrim & 0xFFFFFu;
        
        // Decode triangle data including per-vertex normals
        Triangle tri = decodeTriangle(int(triangleIndex));
        
        // Calculate interpolated normal using barycentric coordinates
        result.normal = interpolateNormal(tri, ray.hit.u, ray.hit.v);
        
        // For now, material ID is 0 (can be extended later)
        result.material = 0;
    }
    
    return result;
}


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
    int sampleCount = (positionHash % 3u == 0u) ? 2 : 1; // Variable sampling
    
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
    const int AO_SAMPLES = 3; // Reduced samples for performance
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
            // Show face normals as RGB colors
            uint triangleIndex = hit.instPrim & 0xFFFFFu;
            Triangle tri = decodeTriangle(int(triangleIndex));
            vec3 faceNormal = getFaceNormal(tri);
            color += attenuation * (faceNormal * 0.5 + 0.5);
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
