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

// --- Imposter: voxel box volume ---
uniform sampler3D imposterColorVolume;   // RGB albedo, A coverage
uniform sampler3D imposterNormalVolume;  // RG octahedral
uniform vec3 imposterBoxMin;
uniform vec3 imposterBoxExt;

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

// Octahedral normal decode (matches host oct_decode in src/voxel_imposter.cpp).
vec3 octDecode(vec2 e){
    vec2 f = e*2.0-1.0;
    vec3 n = vec3(f.x, f.y, 1.0-abs(f.x)-abs(f.y));
    float t = max(-n.z,0.0);
    n.x += n.x>=0.0 ? -t : t;
    n.y += n.y>=0.0 ? -t : t;
    return normalize(n);
}
// Voxel DDA march through imposterColorVolume.
// o and d are in unit-box space [0,1]^3 (invTransform already handles world->box).
// Coverage threshold: alpha > 0.5 == occupied.
bool voxelMarch(vec3 o, vec3 d, out ivec3 hitVox, out float tBox){
    ivec3 dim = textureSize(imposterColorVolume, 0);
    vec3 fdim = vec3(dim);
    // Clip the local ray to the unit box [0,1]^3 and advance the origin to the
    // entry point. The camera origin is always outside the box (the proxy is a
    // unit-cube BLAS), so starting the DDA at the clamped origin would snap
    // grazing rays onto a boundary voxel row and produce a false slab.
    vec3 invD = 1.0 / d;
    vec3 ta = (vec3(0.0) - o) * invD;
    vec3 tb = (vec3(1.0) - o) * invD;
    vec3 tlo = min(ta, tb);
    vec3 thi = max(ta, tb);
    float tEnterBox = max(max(tlo.x, tlo.y), tlo.z);
    float tExitBox  = min(min(thi.x, thi.y), thi.z);
    if (tExitBox < max(tEnterBox, 0.0)) return false; // ray misses the box
    float tStart = max(tEnterBox, 0.0);
    vec3 oStart = o + d * tStart;
    ivec3 v = clamp(ivec3(floor(oStart * fdim)), ivec3(0), dim - 1);
    vec3 stepV = sign(d);
    vec3 tDelta, tMax;
    for (int a = 0; a < 3; ++a) {
        if (abs(d[a]) < 1e-12) { tDelta[a] = 1e30; tMax[a] = 1e30; }
        else {
            float cs = 1.0 / fdim[a];
            float bnd = (float(v[a]) + (d[a] > 0.0 ? 1.0 : 0.0)) * cs;
            tMax[a] = (bnd - oStart[a]) / d[a];
            tDelta[a] = cs / abs(d[a]);
        }
    }
    // Check the entry voxel (tBox measured from the original origin o).
    if (texelFetch(imposterColorVolume, v, 0).a > 0.5) { hitVox = v; tBox = tStart; return true; }
    int budget = (dim.x + dim.y + dim.z) * 2;
    for (int i = 0; i < budget; ++i) {
        int axis = (tMax.x < tMax.y) ? (tMax.x < tMax.z ? 0 : 2) : (tMax.y < tMax.z ? 1 : 2);
        v[axis] += int(stepV[axis]);
        if (v[axis] < 0 || v[axis] >= dim[axis]) return false;
        float tEnter = tStart + tMax[axis];
        tMax[axis] += tDelta[axis];
        if (texelFetch(imposterColorVolume, v, 0).a > 0.5) { hitVox = v; tBox = tEnter; return true; }
    }
    return false;
}

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

    vec4 data0 = texture(tlasNodesTexture, tiledTexel(tlasNodesTexture, nodeIndex, 0, 3));  // aabbMin + leftChild
    vec4 data1 = texture(tlasNodesTexture, tiledTexel(tlasNodesTexture, nodeIndex, 1, 3));  // aabbMax + BLAS
    vec4 data2 = texture(tlasNodesTexture, tiledTexel(tlasNodesTexture, nodeIndex, 2, 3));  // rightChild in .w

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

    // Load transform matrix (4 rows, stored as individual floats)
    vec4 row0 = texture(instancesTexture, tiledTexel(instancesTexture, instanceIndex, 0, 9));
    vec4 row1 = texture(instancesTexture, tiledTexel(instancesTexture, instanceIndex, 1, 9));
    vec4 row2 = texture(instancesTexture, tiledTexel(instancesTexture, instanceIndex, 2, 9));
    vec4 row3 = texture(instancesTexture, tiledTexel(instancesTexture, instanceIndex, 3, 9));

    // Store as individual floats in row-major order
    inst.transform[0] = row0.x; inst.transform[1] = row0.y; inst.transform[2] = row0.z; inst.transform[3] = row0.w;
    inst.transform[4] = row1.x; inst.transform[5] = row1.y; inst.transform[6] = row1.z; inst.transform[7] = row1.w;
    inst.transform[8] = row2.x; inst.transform[9] = row2.y; inst.transform[10] = row2.z; inst.transform[11] = row2.w;
    inst.transform[12] = row3.x; inst.transform[13] = row3.y; inst.transform[14] = row3.z; inst.transform[15] = row3.w;

    // Load inverse transform matrix (4 rows)
    vec4 invRow0 = texture(instancesTexture, tiledTexel(instancesTexture, instanceIndex, 4, 9));
    vec4 invRow1 = texture(instancesTexture, tiledTexel(instancesTexture, instanceIndex, 5, 9));
    vec4 invRow2 = texture(instancesTexture, tiledTexel(instancesTexture, instanceIndex, 6, 9));
    vec4 invRow3 = texture(instancesTexture, tiledTexel(instancesTexture, instanceIndex, 7, 9));

    // Store as individual floats in row-major order  
    inst.invTransform[0] = invRow0.x; inst.invTransform[1] = invRow0.y; inst.invTransform[2] = invRow0.z; inst.invTransform[3] = invRow0.w;
    inst.invTransform[4] = invRow1.x; inst.invTransform[5] = invRow1.y; inst.invTransform[6] = invRow1.z; inst.invTransform[7] = invRow1.w;
    inst.invTransform[8] = invRow2.x; inst.invTransform[9] = invRow2.y; inst.invTransform[10] = invRow2.z; inst.invTransform[11] = invRow2.w;
    inst.invTransform[12] = invRow3.x; inst.invTransform[13] = invRow3.y; inst.invTransform[14] = invRow3.z; inst.invTransform[15] = invRow3.w;
    
    // Load metadata (currently using blas_start_index as blasIndex for compatibility)
    vec4 metadata = texture(instancesTexture, tiledTexel(instancesTexture, instanceIndex, 8, 9));
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
            // Voxel box imposter path (v2): DDA through the 3-D voxel volume.
            // invTransform maps world -> unit-cube [0,1]^3 (the instance is built as
            // a unit-cube BLAS scaled/translated to the bounding box in world space).
            vec3 oLocal = transformPosition(rayOrigin, inst.invTransform);
            vec3 dLocal = transformVector(rayDir, inst.invTransform);
            ivec3 hv; float tBox;
            if (voxelMarch(oLocal, dLocal, hv, tBox)) {
                vec4 colorSample  = texelFetch(imposterColorVolume,  hv, 0);
                vec3 albedo = colorSample.rgb;
                vec2 ne = texelFetch(imposterNormalVolume, hv, 0).rg;
                vec3 nLocal = octDecode(ne);
                // Reconstruct world-space hit position from the local-space march distance.
                vec3 pLocal = oLocal + dLocal * tBox;
                vec3 pWorld = transformPosition(pLocal, inst.transform);
                result.hit      = true;
                result.position = pWorld;
                result.t        = length(pWorld - rayOrigin);
                result.normal   = transformNormal(nLocal, inst.invTransform);
                result.tint     = albedo;
                result.tintAlpha = 1.0;
                result.material  = 0;
                result.isImposter = true;
            } else {
                // Miss (porosity): ray passes through.
                result.hit        = false;
                result.material   = -1;
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
