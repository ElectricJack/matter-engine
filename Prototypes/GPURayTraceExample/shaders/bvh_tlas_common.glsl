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
    int bvhSteps = 0;

    while (true)
    {
        // Safety cap: bound worst-case traversal so a pathological ray can't hang the GPU (TDR)
        if (++bvhSteps > 4096) break;
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
    int tlasSteps = 0;

    while (true)
    {
        // Safety cap: bound worst-case traversal so a pathological ray can't hang the GPU (TDR)
        if (++tlasSteps > 512) break;
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
            // Smooth shading: interpolate the per-vertex normals by the hit's
            // barycentric coordinates (works for any mesh, not just spheres).
            normal = interpolateNormal(tri, ray.hit.u, ray.hit.v);
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