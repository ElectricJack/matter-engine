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
    mat4 transform;
    mat4 invTransform;
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
    
    // Load transform matrix (4 rows)
    vec4 row0 = texture(instancesTexture, vec2(instTexCoord, 0.0556));
    vec4 row1 = texture(instancesTexture, vec2(instTexCoord, 0.1667));
    vec4 row2 = texture(instancesTexture, vec2(instTexCoord, 0.2778));
    vec4 row3 = texture(instancesTexture, vec2(instTexCoord, 0.3889));
    
    inst.transform = mat4(row0, row1, row2, row3);
    
    // Load inverse transform matrix (4 rows)
    vec4 invRow0 = texture(instancesTexture, vec2(instTexCoord, 0.5000));
    vec4 invRow1 = texture(instancesTexture, vec2(instTexCoord, 0.6111));
    vec4 invRow2 = texture(instancesTexture, vec2(instTexCoord, 0.7222));
    vec4 invRow3 = texture(instancesTexture, vec2(instTexCoord, 0.8333));
    
    inst.invTransform = mat4(invRow0, invRow1, invRow2, invRow3);
    
    // Load metadata (currently using blas_start_index as blasIndex for compatibility)
    vec4 metadata = texture(instancesTexture, vec2(instTexCoord, 0.9444));
    inst.blasIndex = uint(metadata.x); // Actually blas_start_index for now
    inst.materialId = uint(metadata.y); // Will be 0 for now
    
    return inst;
}

// Transform ray to local space
void transformRay(vec3 rayOrigin, vec3 rayDir, mat4 invTransform, out vec3 localOrigin, out vec3 localDir)
{
    vec4 localOrigin4 = invTransform * vec4(rayOrigin, 1.0);
    vec4 localDir4 = invTransform * vec4(rayDir, 0.0);
    
    localOrigin = localOrigin4.xyz;
    localDir = normalize(localDir4.xyz);
}

// Transform normal from local to world space
vec3 transformNormal(vec3 normal, mat4 transform)
{
    vec4 worldNormal4 = transpose(transform) * vec4(normal, 0.0);
    return normalize(worldNormal4.xyz);
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

// TLAS traversal (ported from TLAS::Intersect)
void TLASIntersect(inout Ray ray)
{
    // Stack for iterative TLAS traversal
    int stack[32];
    int stackPtr = 0;
    
    // Start with TLAS root node
    int nodeIdx = 0;
    
    while (true)
    {
        TLASNode node = decodeTLASNode(nodeIdx);
        
        // Test ray against TLAS node AABB
        if (IntersectAABB(ray, node.aabbMin, node.aabbMax) == 1e30)
        {
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
            continue;
        }
        
        if (node.leftRight == 0u) // Leaf node
        {
            // Intersect with instance
            uint instanceIndex = node.BLAS;
            BVHInstance inst = decodeInstance(int(instanceIndex));
            
            // Transform ray to instance space
            vec3 localRayOrigin, localRayDir;
            transformRay(ray.O, ray.D, inst.invTransform, localRayOrigin, localRayDir);
            
            // Create local ray
            Ray localRay;
            localRay.O = localRayOrigin;
            localRay.D = localRayDir;
            localRay.rD = vec3(1.0) / localRayDir;
            localRay.hit = ray.hit;
            
            // Intersect with BLAS
            BVHIntersect(localRay, instanceIndex, inst.blasIndex);
            
            // Update global ray if we found a closer intersection
            if (localRay.hit.t < ray.hit.t)
            {
                ray.hit = localRay.hit;
            }
            
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
        }
        else // Interior node
        {
            // Extract left and right child indices
            uint leftChild = node.leftRight & 0xFFFFu;
            uint rightChild = (node.leftRight >> 16) & 0xFFFFu;
            
            // Add children to stack (right first, so left is processed first)
            if (stackPtr < 30)
            {
                stack[stackPtr++] = int(rightChild);
                stack[stackPtr++] = int(leftChild);
            }
            
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
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