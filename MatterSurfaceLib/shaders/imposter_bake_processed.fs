#version 330 core
in vec3 cageWorldPos;
in vec3 cageNormal;
out vec4 fragColor;

uniform float maxDisp;       // shell thickness; ray marches at most this far inward
uniform int   debugAlbedo;   // !=0: bake raw albedo (no lighting) to isolate ray/material
uniform int   debugAO;       // !=0: bake hit.ao as grayscale to isolate the AO term

// --- Free symbols required by lighting.glsl (mirror raytrace_tlas_blas.fs) ---
uniform float giStrength;
uniform float shadowStrength;
uniform int   aoEnabled;

vec3 lightPos   = vec3(3.0, 8.0, 2.0);
vec3 lightColor = vec3(4.0, 3.8, 3.5);
vec3 ambient    = vec3(0.34, 0.34, 0.33);

uint getGridHash(vec3 pos) {
    ivec3 gridPos = ivec3(floor(pos * 2.0));
    return uint(gridPos.x * 73 + gridPos.y * 137 + gridPos.z * 281);
}

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

// Packed material table, uploaded from the CPU registry. 12 floats per material
// (see MATERIAL_FLOATS_PER_DEF / MaterialRegistryPackForGPU):
//   [0..2] albedo, [3] roughness, [4] metallic, [5] emission, [6] pad,
//   [7] translucency, [8] ior, [9] flatShading, [10] mergeGroup, [11] pad
#define MAX_MATERIALS 64
#define MATERIAL_FLOATS_PER_DEF 12
uniform float materialTable[MAX_MATERIALS * MATERIAL_FLOATS_PER_DEF];
uniform int materialCount;

// Material lookup table - data-driven via uniform array uploaded from CPU registry
MaterialProperties getMaterialProperties(int materialId)
{
    // Smooth-shading flag is now a table field; keep the legacy >=1M offset
    // working so existing callers that set it still smooth-shade.
    bool forceSmooth = false;
    int smooth_normals_offset = 1000000;
    if (materialId >= smooth_normals_offset) { materialId -= smooth_normals_offset; forceSmooth = true; }

    MaterialProperties mat;
    int id = materialId;
    if (id < 0 || id >= materialCount) {
        mat.albedo = vec3(0.6); mat.roughness = 0.1; mat.metallic = 0.8;
        mat.emission = 0.0; mat.translucency = 0.0; mat.ior = 1.0; mat.flatShading = true;
        return mat;
    }
    int b = id * MATERIAL_FLOATS_PER_DEF;
    mat.albedo = vec3(materialTable[b+0], materialTable[b+1], materialTable[b+2]);
    mat.roughness = materialTable[b+3];
    mat.metallic  = materialTable[b+4];
    mat.emission  = materialTable[b+5];
    mat.translucency = materialTable[b+7];
    mat.ior = materialTable[b+8];
    mat.flatShading = forceSmooth ? false : (materialTable[b+9] > 0.5);
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

// --- Imposter (v1: single bound atlas + global params) ---
uniform sampler2D imposterColorTex;   // baked radiance (rgb) + coverage (a)
uniform sampler2D imposterDispTex;    // scalar inward depth, normalized [0,1]
uniform int   imposterGrid;           // atlas cell grid (ceil(sqrt(cageTriCount)))
uniform int   imposterTriBase;        // global triangle index of the cage's first triangle
uniform float imposterMaxDisp;        // shell thickness (denormalizes displacement)
uniform vec2  imposterAtlasSize;      // (atlasW, atlasH) for padding math
uniform float imposterPad;            // gutter padding in texels (matches build_cage)
uniform int   imposterDbg;            // 0=normal, 1=color by cage triangle index (no relief)
uniform sampler2D imposterTriUvTex;   // RGBA32F: col=BVH triangle slot, row=corner, .xy=uv
uniform int   imposterTriCount;       // number of cage triangles (texture width)

// Control uniforms
uniform int intersectionMode;    // 0=brute force, 1=TLAS/BLAS traversal
uniform int debugTriangleTests;  // 0=normal rendering, 1=visualize triangle test counts

// Ray tracing structures
struct Intersection
{
    float t;         // intersection distance along ray
    float u, v;      // barycentric coordinates of the intersection
    uint primIdx;    // global triangle index (full 32 bits; was 20-bit packed)
    uint instIdx;    // instance index (full 32 bits; was 12-bit packed)
};

struct Ray
{
    vec3 O, D, rD;   // origin, direction, reciprocal direction
    Intersection hit;
    int triangleTests; // Debug: count triangle intersections tested
};

struct Triangle
{
    vec3 v0, v1, v2; vec3 n0, n1, n2; // triangle vertices + per-vertex shading normals
    vec3 ao;         // per-vertex baked AO (x=v0, y=v1, z=v2); default 1.0
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
    uint leftChild;  // left child node index (0 == leaf sentinel)
    vec3 aabbMax;
    uint BLAS;       // BLAS/instance index for leaf nodes
    uint rightChild; // right child node index
};

struct BVHInstance
{
    // Transform matrix as 16 individual floats (row-major like reference)
    float transform[16];
    float invTransform[16];
    uint blasIndex;
    uint materialId;
    bool isImposter;
};

struct HitResult
{
    bool hit;
    float t;
    vec3 position;
    vec3 normal;
    int material;
    int instanceId;
    int triangleTests; // Debug: number of triangle tests performed
    vec3 tint;       // per-triangle tint rgb (from spare .w of rows 1-3)
    float tintAlpha; // blend strength (from spare .w of row 4); 0 = no tint
    float ao;        // baked ambient occlusion at the hit, [0,1]; 1.0 = unoccluded
    bool isImposter;
    vec3 bakedColor; // valid when isImposter && hit (baked radiance to display)
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
void IntersectTri(inout Ray ray, Triangle tri, uint primIdx, uint instIdx)
{
    // Count this triangle test for debugging
    ray.triangleTests++;

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
        ray.hit.primIdx = primIdx;
        ray.hit.instIdx = instIdx;
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

// Tiled texture addressing. Data wider than the texture's tile width wraps into
// additional vertical tile rows so the texture width never exceeds
// GL_MAX_TEXTURE_SIZE. rowsPerElem is 6 for triangles, 3 for BVH nodes; the
// uploader (blas_manager.cpp) lays out texels with the matching convention.
vec2 tiledTexel(sampler2D tex, int index, int row, int rowsPerElem)
{
    ivec2 ts = textureSize(tex, 0);
    int tileW = ts.x;
    int tx = index % tileW;
    int ty = index / tileW;
    float fx = (float(tx) + 0.5) / float(ts.x);
    float fy = (float(ty * rowsPerElem + row) + 0.5) / float(ts.y);
    return vec2(fx, fy);
}

// Decode triangle from texture (optimized layout)
Triangle decodeTriangle(int triangleIndex)
{
    Triangle tri;

    vec4 data0 = texture(trianglesTexture, tiledTexel(trianglesTexture, triangleIndex, 0, 6));  // v0 (row 0)
    vec4 data1 = texture(trianglesTexture, tiledTexel(trianglesTexture, triangleIndex, 1, 6));  // v1 (row 1)
    vec4 data2 = texture(trianglesTexture, tiledTexel(trianglesTexture, triangleIndex, 2, 6));  // v2 (row 2)
    // Rows 3-5 (per-vertex normals) are sampled lazily at shade time, not during traversal

    tri.v0 = data0.xyz;
    tri.v1 = data1.xyz;
    tri.v2 = data2.xyz;
    tri.n0 = tri.n1 = tri.n2 = vec3(0.0);
    tri.ao = vec3(1.0); // baked AO sampled lazily at shade time, not during traversal

    return tri;
}

// Decode BVH node from texture (matches BVHNode struct)
BVHNode decodeBVHNode(int nodeIndex)
{
    BVHNode node;

    vec4 data0 = texture(blasNodesTexture, tiledTexel(blasNodesTexture, nodeIndex, 0, 3));  // aabbMin + leftFirst
    vec4 data1 = texture(blasNodesTexture, tiledTexel(blasNodesTexture, nodeIndex, 1, 3));  // aabbMax + triCount

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

    vec4 data0 = texture(tlasNodesTexture, vec2(nodeTexCoord, 0.125));    // aabbMin + leftChild
    vec4 data1 = texture(tlasNodesTexture, vec2(nodeTexCoord, 0.375));    // aabbMax + BLAS
    vec4 data2 = texture(tlasNodesTexture, vec2(nodeTexCoord, 0.8333));   // rightChild in .w

    node.aabbMin = data0.xyz;
    node.leftChild = uint(data0.w);
    node.aabbMax = data1.xyz;
    node.BLAS = uint(data1.w);
    node.rightChild = uint(data2.w);

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
    inst.isImposter = metadata.z > 0.5;

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
    // Stack for iterative BVH traversal. Size must exceed the builder's
    // MAX_DEPTH (40 in bvh.cpp) or deep trees overflow and corrupt traversal;
    // 64 matches the CPU reference BVH::Intersect stack.
    int stack[64];
    int stackPtr = 0;

    // Start with root node
    int nodeIdx = int(blasOffset); int bvhSteps = 0;
    
    while (true)
    {
        // A tree traversal visits each node at most once, so iterations can never
        // exceed the total node count unless the data is corrupt; guard against hangs.
        if (++bvhSteps > blasNodeCount) break; BVHNode node = decodeBVHNode(nodeIdx);
        
        if (node.triCount > 0u) // Leaf node
        {
            // Test all triangles in this leaf
            for (uint i = 0u; i < node.triCount; i++)
            {
                // Since triangles are now stored in BVH order, direct indexing works
                uint triIdx = node.leftFirst + i;
                Triangle tri = decodeTriangle(int(triIdx));
                IntersectTri(ray, tri, triIdx, instanceIdx);
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
            if (dist2 != 1e30 && stackPtr < 63)
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
    
    // Stack for iterative TLAS traversal (sized to match the CPU reference, 64).
    int stack[64];
    int stackPtr = 0;

    // Start with TLAS root node
    int nodeIdx = 0; int tlasSteps = 0;
    
    while (true)
    {
        // Each TLAS node is visited at most once in a valid tree traversal, so the
        // node count is a safe upper bound; exceeding it means corrupt data.
        if (++tlasSteps > tlasNodeCount) break; TLASNode node = decodeTLASNode(nodeIdx);
        
        if (node.leftChild == 0u) // Leaf node
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
        uint leftChild = node.leftChild;
        uint rightChild = node.rightChild;
        
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
            if (dist2 != 1e30 && stackPtr < 63)
                stack[stackPtr++] = int(rightChild);
        }
    }
}

// ---- Any-hit shadow query -------------------------------------------------
// Shadow rays only need to know whether *any* occluder lies between the surface
// and the light, so this path early-outs on the first triangle hit within
// (eps, maxDist) and skips the whole HitResult decode (normals/tint/AO/
// barycentrics) that intersectScene performs for the closest hit. The ray's
// hit.t is seeded to maxDist so AABB tests also prune nodes beyond the light.
// t is preserved across the affine instance transform (O and D scale together),
// so a world-space maxDist is a valid cutoff in instance space too.
bool shadowIntersectTri(Ray ray, Triangle tri, float maxDist)
{
    vec3 edge1 = tri.v1 - tri.v0;
    vec3 edge2 = tri.v2 - tri.v0;
    vec3 h = cross(ray.D, edge2);
    float a = dot(edge1, h);
    if (a > -0.00001 && a < 0.00001) return false; // ray parallel to triangle
    float f = 1.0 / a;
    vec3 s = ray.O - tri.v0;
    float u = f * dot(s, h);
    if (u < 0.0 || u > 1.0) return false;
    vec3 q = cross(s, edge1);
    float v = f * dot(ray.D, q);
    if (v < 0.0 || u + v > 1.0) return false;
    float t = f * dot(edge2, q);
    return (t > 0.0001 && t < maxDist);
}

bool shadowBVHIntersect(Ray ray, uint blasOffset, float maxDist)
{
    int stack[64];
    int stackPtr = 0;
    int nodeIdx = int(blasOffset); int bvhSteps = 0;
    while (true)
    {
        if (++bvhSteps > blasNodeCount) break; BVHNode node = decodeBVHNode(nodeIdx);

        if (node.triCount > 0u) // Leaf node
        {
            for (uint i = 0u; i < node.triCount; i++)
            {
                uint triIdx = node.leftFirst + i;
                Triangle tri = decodeTriangle(int(triIdx));
                if (shadowIntersectTri(ray, tri, maxDist)) return true;
            }
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
            continue;
        }

        int leftChild = int(node.leftFirst);
        int rightChild = leftChild + 1;
        BVHNode leftNode = decodeBVHNode(leftChild);
        BVHNode rightNode = decodeBVHNode(rightChild);
        float dist1 = IntersectAABB(ray, leftNode.aabbMin, leftNode.aabbMax);
        float dist2 = IntersectAABB(ray, rightNode.aabbMin, rightNode.aabbMax);
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
            if (dist2 != 1e30 && stackPtr < 63)
                stack[stackPtr++] = rightChild;
        }
    }
    return false;
}

bool shadowInstanceIntersect(Ray ray, BVHInstance inst, float maxDist)
{
    transformRay(ray, inst.invTransform);   // ray is a local copy; TLAS ray unaffected
    return shadowBVHIntersect(ray, inst.blasIndex, maxDist);
}

bool shadowQuery(vec3 origin, vec3 dir, float maxDist)
{
    Ray ray;
    ray.O = origin;
    ray.D = dir;
    ray.rD = vec3(1.0) / dir;
    ray.hit.t = maxDist; // AABB tests prune anything past the light
    ray.hit.u = 0.0;
    ray.hit.v = 0.0;
    ray.hit.primIdx = 0u;
    ray.hit.instIdx = 0u;
    ray.triangleTests = 0;

    int stack[64];
    int stackPtr = 0;
    int nodeIdx = 0; int tlasSteps = 0;
    while (true)
    {
        if (++tlasSteps > tlasNodeCount) break; TLASNode node = decodeTLASNode(nodeIdx);

        if (node.leftChild == 0u) // Leaf node
        {
            BVHInstance inst = decodeInstance(int(node.BLAS));
            if (shadowInstanceIntersect(ray, inst, maxDist)) return true;
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
            continue;
        }

        uint leftChild = node.leftChild;
        uint rightChild = node.rightChild;
        TLASNode child1 = decodeTLASNode(int(leftChild));
        TLASNode child2 = decodeTLASNode(int(rightChild));
        float dist1 = IntersectAABB(ray, child1.aabbMin, child1.aabbMax);
        float dist2 = IntersectAABB(ray, child2.aabbMin, child2.aabbMax);
        if (dist1 > dist2)
        {
            float tmpDist = dist1; dist1 = dist2; dist2 = tmpDist;
            uint tmpIdx = leftChild; leftChild = rightChild; rightChild = tmpIdx;
        }
        if (dist1 == 1e30)
        {
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
        }
        else
        {
            nodeIdx = int(leftChild);
            if (dist2 != 1e30 && stackPtr < 63)
                stack[stackPtr++] = int(rightChild);
        }
    }
    return false;
}

// Relief-march from the cage entry into the displacement shell. Returns true and
// the hit UV when a covered crossing is found within maxDisp; false = pass through.
// hitS returns the arc length along (normalized) rayDir from entryPos to the
// crossing, so the caller can place the hit at the true displaced surface.
bool reliefMarch(vec3 entryPos, vec3 rayDir,
                 vec3 v0, vec3 v1, vec3 v2,
                 vec2 uv0, vec2 uv1, vec2 uv2,
                 vec3 cageN, out vec2 hitUV, out float hitS) {
    hitS = 0.0;
    vec3 dpdu, dpdv;
    {
        vec3 e1 = v1 - v0, e2 = v2 - v0;
        vec2 d1 = uv1 - uv0, d2 = uv2 - uv0;
        float det = d1.x * d2.y - d2.x * d1.y;
        if (abs(det) < 1e-12) return false;
        float r = 1.0 / det;
        dpdu = r * ( d2.y * e1 - d1.y * e2);
        dpdv = r * (-d2.x * e1 + d1.x * e2);
    }
    // Barycentric entry UV.
    vec3 e1 = v1 - v0, e2 = v2 - v0, ep = entryPos - v0;
    float d00=dot(e1,e1), d01=dot(e1,e2), d11=dot(e2,e2), d20=dot(ep,e1), d21=dot(ep,e2);
    float den = d00*d11 - d01*d01;
    if (abs(den) < 1e-12) return false; // degenerate cage triangle -> avoid NaN
    float bv = (d11*d20 - d01*d21) / den;
    float bw = (d00*d21 - d01*d20) / den;
    float bu = 1.0 - bv - bw;
    vec2 entryUV = bu*uv0 + bv*uv1 + bw*uv2;

    // Chart bounds = this triangle's own UV bbox. Marching UV must stay within it,
    // or we'd sample a neighbor chart's heightfield (cross-cell bleed / seams).
    vec2 cellLo = min(uv0, min(uv1, uv2)) - vec2(0.002);
    vec2 cellHi = max(uv0, max(uv1, uv2)) + vec2(0.002);

    // World ray -> (du, dv, dn) rates. dn<0 = going inward (below cage along N).
    mat3 M = mat3(dpdu, dpdv, cageN);
    if (abs(determinant(M)) < 1e-9) return false; // near-singular frame -> inverse undefined
    vec3 duvn = inverse(M) * rayDir;
    float inward = -duvn.z;
    if (inward <= 1e-5) return false; // ray not entering the shell

    const int LIN = 128;
    const int BIN = 8;
    float sMax = imposterMaxDisp / inward;   // arc length to reach full depth
    float ds = sMax / float(LIN);
    float prevS = 0.0; bool have_prev = false;
    for (int i = 1; i <= LIN; ++i) {
        float s = ds * float(i);
        vec2 uvc = entryUV + duvn.xy * s;
        if (uvc.x < cellLo.x - 0.002 || uvc.x > cellHi.x + 0.002 ||
            uvc.y < cellLo.y - 0.002 || uvc.y > cellHi.y + 0.002) break;
        float pen = inward * s;                       // penetration below cage
        float d = texture(imposterDispTex, uvc).r * imposterMaxDisp;
        float cov = texture(imposterColorTex, uvc).a;
        float diff = pen - d;                          // >0 once we pass the surface
        if (cov > 0.5 && diff >= 0.0) {
            // Binary refine between prevS and s.
            float lo = have_prev ? prevS : 0.0, hi = s;
            for (int b = 0; b < BIN; ++b) {
                float mid = 0.5*(lo+hi);
                vec2 um = entryUV + duvn.xy*mid;
                float dm = texture(imposterDispTex, um).r * imposterMaxDisp;
                if (inward*mid - dm >= 0.0) hi = mid; else lo = mid;
            }
            hitUV = entryUV + duvn.xy*hi;
            hitS = hi;
            return texture(imposterColorTex, hitUV).a > 0.5;
        }
        prevS = s; have_prev = true;
    }
    return false; // reached maxDisp without a covered crossing -> pass through
}

// Main intersection interface. maxT bounds the search: seeding the ray's hit.t
// lets AABB/triangle tests prune anything past maxT, so a short maxT (e.g. a
// bounded GI ray) walks far less of the BVH. Pass 1e30 for an unbounded search.
HitResult intersectScene(vec3 rayOrigin, vec3 rayDir, float maxT)
{
    Ray ray;
    ray.O = rayOrigin;
    ray.D = rayDir;
    ray.rD = vec3(1.0) / rayDir;
    ray.hit.t = maxT;
    ray.hit.u = 0.0;
    ray.hit.v = 0.0;
    ray.hit.primIdx = 0u;
    ray.hit.instIdx = 0u;
    ray.triangleTests = 0; // Initialize triangle test counter

    // Perform TLAS traversal
    TLASIntersect(ray);

    HitResult result;
    result.hit = (ray.hit.t < maxT);
    result.t = ray.hit.t;
    result.triangleTests = ray.triangleTests; // Copy debug counter

    if (result.hit)
    {
        result.position = rayOrigin + rayDir * ray.hit.t;
        
        // Extract triangle and instance indices
        uint triIdx = ray.hit.primIdx;
        uint instIdx = ray.hit.instIdx;
        
        // Get triangle data for normal calculation
        Triangle    tri  = decodeTriangle(int(triIdx));
        BVHInstance inst = decodeInstance(int(instIdx));

        // Per-triangle material packed in row-0 .w (see blas_manager.cpp packing).
        // >=0 overrides the instance material; -1 falls back to the instance default.
        // NOTE: the instance materialId is a merge-GROUP id (see Cluster::add_to_tlas),
        // so it is only a fallback; every real triangle carries its own materialId here.
        float triMatF = texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 0, 6)).w;
        int triMat = int(triMatF);
        int effectiveMat = (triMat >= 0) ? triMat : int(inst.materialId);

        // Per-triangle tint packed in the spare .w of rows 1-4 (see blas_manager.cpp).
        result.tint = vec3(
            texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 1, 6)).w,
            texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 2, 6)).w,
            texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 3, 6)).w);
        result.tintAlpha = texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 4, 6)).w;

        // Material properties (incl. flatShading) sourced from the per-triangle material.
        MaterialProperties matProps = getMaterialProperties(effectiveMat);

        // Barycentrics at the hit (shared by baked AO and smooth-normal shading).
        vec3 localHitPos = transformPosition(result.position, inst.invTransform);
        vec3 e1 = tri.v1 - tri.v0, e2 = tri.v2 - tri.v0, ep = localHitPos - tri.v0;
        float d00 = dot(e1, e1), d01 = dot(e1, e2), d11 = dot(e2, e2);
        float d20 = dot(ep, e1), d21 = dot(ep, e2);
        float den = d00 * d11 - d01 * d01;
        float bv = 0.0, bw = 0.0, bu = 1.0;
        if (abs(den) >= 1e-12) {
            bv = (d11 * d20 - d01 * d21) / den;
            bw = (d00 * d21 - d01 * d20) / den;
            bu = 1.0 - bv - bw;
        }

        // Baked AO: unpack 3x8-bit from row 5 .w and interpolate. bits==0 means the
        // slot was never written (old texture / disabled) -> treat as unoccluded.
        float aoPacked = texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 5, 6)).w;
        uint aoBits = floatBitsToUint(aoPacked);
        vec3 aoV = (aoBits == 0u) ? vec3(1.0)
                 : vec3(float(aoBits & 0xFFu), float((aoBits >> 8) & 0xFFu), float((aoBits >> 16) & 0xFFu)) / 255.0;
        result.ao = (aoEnabled != 0) ? clamp(bu * aoV.x + bv * aoV.y + bw * aoV.z, 0.0, 1.0) : 1.0;

        vec3 normal;
        if (matProps.flatShading) {
            // Use face normal for flat shading
            normal = normalize(cross(e1, e2));
        } else {
            // Smooth normal: lazily sample the per-vertex normals (rows 3-5) and interpolate by barycentrics at the hit
            vec3 N0 = texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 3, 6)).xyz;
            vec3 N1 = texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 4, 6)).xyz;
            vec3 N2 = texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 5, 6)).xyz;
            if (abs(den) < 1e-12) normal = normalize(cross(e1, e2));
            else normal = normalize(bu * N0 + bv * N1 + bw * N2);
        }

        // Transform normal to world space
        result.normal = transformNormal(normal, inst.invTransform);
        result.material = effectiveMat;
        result.instanceId = int(instIdx);
        result.isImposter = inst.isImposter;
        result.bakedColor = vec3(0.0);
        if (inst.isImposter) {
            int localTri = int(triIdx) - imposterTriBase;
            if (imposterDbg != 0) {
                // Debug: paint each cage triangle a distinct hue, skip relief, so the
                // raw cage-triangle hit layout is visible on screen.
                float h = float(localTri) / 12.0;
                result.bakedColor = vec3(fract(h*3.0), fract(h*5.0+0.33), fract(h*7.0+0.66));
                return result;
            }
            // Cage verts in world space.
            vec3 w0 = transformPosition(tri.v0, inst.transform);
            vec3 w1 = transformPosition(tri.v1, inst.transform);
            vec3 w2 = transformPosition(tri.v2, inst.transform);
            vec3 cageN = normalize(result.normal);
            // Per-corner baked UVs, fetched in BVH-triangle order -> reorder-safe and
            // geometry-agnostic (same path for cube and fitted cages).
            vec2 uv0 = texelFetch(imposterTriUvTex, ivec2(localTri, 0), 0).xy;
            vec2 uv1 = texelFetch(imposterTriUvTex, ivec2(localTri, 1), 0).xy;
            vec2 uv2 = texelFetch(imposterTriUvTex, ivec2(localTri, 2), 0).xy;
            vec3 ndir = normalize(rayDir);
            vec2 hitUV; float hitS;
            if (reliefMarch(result.position, ndir, w0, w1, w2, uv0, uv1, uv2, cageN, hitUV, hitS)) {
                // Advance from the cage plane to the true displaced surface so depth
                // and lighting use the real hit, and neighbouring cage triangles'
                // relief surfaces line up instead of painting flat at the cage.
                result.position += ndir * hitS;
                result.t += hitS / length(rayDir);
                result.bakedColor = texture(imposterColorTex, hitUV).rgb;
            } else {
                result.hit = false;          // coverage miss: ray passes through
                result.material = -1;
                result.instanceId = -1;
            }
        }
    }
    else
    {
        result.position = vec3(0.0);
        result.normal = vec3(0.0);
        result.material = -1;
        result.instanceId = -1;
        result.tint = vec3(0.0);
        result.tintAlpha = 0.0;
        result.ao = 1.0;
        result.isImposter = false;
        result.bakedColor = vec3(0.0);
    }
    
    return result;
}

// Unbounded closest-hit search (the common case).
HitResult intersectScene(vec3 rayOrigin, vec3 rayDir)
{
    return intersectScene(rayOrigin, rayDir, 1e30);
}

// Debug visualization: convert triangle test count to color
vec3 triangleTestCountToColor(int testCount)
{
    if (testCount == 0) {
        return vec3(0.0, 0.0, 0.2); // Dark blue for rays that hit nothing
    }
    
    // Color scale: Green (few tests) -> Yellow -> Red (many tests)
    float normalizedCount = float(testCount) / 50.0; // Adjust scale as needed
    normalizedCount = clamp(normalizedCount, 0.0, 1.0);
    
    if (normalizedCount < 0.5) {
        // Green to Yellow
        float t = normalizedCount * 2.0;
        return mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), t);
    } else {
        // Yellow to Red
        float t = (normalizedCount - 0.5) * 2.0;
        return mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), t);
    }
}
// === END INCLUDE: bvh_tlas_common.glsl ===
// === BEGIN INCLUDE: lighting.glsl ===
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
// === END INCLUDE: lighting.glsl ===

void main() {
    vec3 n = normalize(cageNormal);
    vec3 origin = cageWorldPos;
    vec3 dir = -n;                       // inward
    HitResult hit = intersectScene(origin, dir, maxDisp * 1.5);
    if (!hit.hit) { fragColor = vec4(0.0, 0.0, 0.0, 0.0); return; }

    vec3 hitPos = origin + dir * hit.t;
    vec3 hn = normalize(hit.normal);
    MaterialProperties matProps = getMaterialProperties(hit.material);
    vec3 albedo = mix(matProps.albedo, hit.tint, hit.tintAlpha);

    if (debugAlbedo != 0) { fragColor = vec4(albedo, 1.0); return; }
    if (debugAO != 0) { fragColor = vec4(vec3(hit.ao), 1.0); return; }

    // Fixed outward view (no live camera): bake radiance as seen from outside the cage.
    vec3 viewDir = dir;
    uint seed = uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u + 1u;
    vec3 radiance = calculatePBR(hitPos, hn, viewDir, albedo, matProps.roughness,
                                 matProps.metallic, hit.ao, true, seed);
    if (matProps.emission > 0.0) radiance += matProps.albedo * matProps.emission;
    fragColor = vec4(radiance, 1.0);
}
