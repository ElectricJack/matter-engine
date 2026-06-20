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
    ray.hit.primIdx = 0u;
    ray.hit.instIdx = 0u;
    ray.triangleTests = 0; // Initialize triangle test counter

    // Perform TLAS traversal
    TLASIntersect(ray);

    HitResult result;
    result.hit = (ray.hit.t < 1e30);
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
    }
    
    return result;
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