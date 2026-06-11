#ifndef BVH_H
#define BVH_H

#include <stdbool.h>
#include <stdint.h>

// Aligned vector type for better performance
typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    Vec3 min, max;
} AABB;

// Triangle structure optimized for GPU with pre-computed centroid
typedef struct {
    Vec3 v0, v1, v2;      // Triangle vertices
    Vec3 centroid;        // Pre-computed centroid for faster BVH building
    Vec3 normal;          // Surface normal
    int  material_id;      // Material identifier
} Triangle;

// GPU-compatible triangle structure (32-byte aligned)
typedef struct {
    float v0x, v0y, v0z, dummy1;
    float v1x, v1y, v1z, dummy2;
    float v2x, v2y, v2z, dummy3;
    float  cx,  cy,  cz, dummy4;
} GPUTriangle;

// Additional triangle data for shading
typedef struct {
    Vec3  n0, n1, n2;      // Per-vertex normals (for smooth shading)
    float u0, v0;         // UV coordinates vertex 0
    float u1, v1;         // UV coordinates vertex 1
    float u2, v2;         // UV coordinates vertex 2
} TriangleEx;

// Optimized BVH node structure (32 bytes, SIMD-friendly)
typedef struct {
    Vec3     aabb_min;    // Minimum bounds
    uint32_t left_first;  // Left child index (internal) or first triangle (leaf)
    Vec3     aabb_max;    // Maximum bounds  
    uint32_t tri_count;   // Triangle count (>0 for leaf, 0 for internal)
} BVHNode;

// GPU-compatible BVH node structure
typedef struct {
    float minx, miny, minz;
    int   left_first;
    float maxx, maxy, maxz;
    int   tri_count;
} GPUBVHNode;

// Matrix for transformations (4x4)
typedef struct {
    float m[16];  // Column-major layout for GPU compatibility
} Matrix4x4;

// Bottom-Level Acceleration Structure (BLAS) - single mesh BVH
typedef struct {
    Triangle* triangles;
    int*      triangle_indices;
    BVHNode*  nodes;
    int       triangle_count;
    int       node_count;
    int       max_triangles_per_leaf;
} BLAS;

// BVH Instance - represents a transformed BLAS in world space
typedef struct {
    BLAS*     blas;             // Reference to bottom-level BVH
    Matrix4x4 transform;        // Instance-to-world transform
    Matrix4x4 inv_transform;    // World-to-instance transform (for ray transformation)
    AABB      world_bounds;     // AABB in world space (after transformation)
    uint32_t  instance_id;      // Unique instance identifier
    uint32_t  blas_start_index; // Starting index of this BLAS in the combined BLAS array
} BVHInstance;

// GPU-compatible instance structure  
typedef struct {
    float    transform[16];     // 4x4 transform matrix
    float    inv_transform[16]; // 4x4 inverse transform matrix
    uint32_t blas_start_index;  // Starting index in combined BLAS array
    uint32_t instance_id;       // Instance identifier
    uint32_t padding[6];        // Padding to maintain alignment
} GPUBVHInstance;

// Top-Level Acceleration Structure (TLAS) node
typedef struct {
    Vec3 aabb_min;           // Minimum bounds
    uint32_t left_right;     // Packed left (16-bit) and right (16-bit) child indices
    Vec3 aabb_max;           // Maximum bounds
    uint32_t blas_index;     // BLAS index (for leaf nodes), 0 for internal nodes
} TLASNode;

// GPU-compatible TLAS node structure
typedef struct {
    float minx, miny, minz;
    uint32_t left_right;     // 2x16 bits for left/right children
    float maxx, maxy, maxz;
    uint32_t blas_index;
} GPUTLASNode;

// Top-Level Acceleration Structure - manages BVH instances
typedef struct {
    BVHInstance* instances;  // Array of BVH instances
    TLASNode* nodes;         // TLAS nodes
    int instance_count;      // Number of instances
    int node_count;          // Number of TLAS nodes used
    int max_instances;       // Maximum instances supported
} TLAS;

// Function declarations

// Matrix operations
Matrix4x4 matrix_identity();
Matrix4x4 matrix_multiply(const Matrix4x4* a, const Matrix4x4* b);
Matrix4x4 matrix_inverse(const Matrix4x4* m);
Vec3 matrix_transform_point(const Matrix4x4* m, Vec3 p);
Vec3 matrix_transform_vector(const Matrix4x4* m, Vec3 v);

// Advanced matrix creation
Matrix4x4 matrix_translation(float x, float y, float z);
Matrix4x4 matrix_scale(float sx, float sy, float sz);
Matrix4x4 matrix_rotation_x(float angle_radians);
Matrix4x4 matrix_rotation_y(float angle_radians);
Matrix4x4 matrix_rotation_z(float angle_radians);
Matrix4x4 matrix_rotation_axis(Vec3 axis, float angle_radians);

// BLAS operations  
BLAS* blas_create(Triangle* triangles, int triangle_count, int max_triangles_per_leaf);
void blas_destroy(BLAS* blas);
void blas_build(BLAS* blas);

// BVH instance operations
BVHInstance* bvh_instance_create(BLAS* blas, uint32_t instance_id);
void bvh_instance_destroy(BVHInstance* instance);
void bvh_instance_set_transform(BVHInstance* instance, const Matrix4x4* transform);

// TLAS operations
TLAS* tlas_create(int max_instances);
void tlas_destroy(TLAS* tlas);
void tlas_add_instance(TLAS* tlas, BVHInstance* instance);
void tlas_build(TLAS* tlas);

// GPU data preparation
void prepare_gpu_triangles(Triangle* triangles, int count, GPUTriangle* gpu_triangles);
void prepare_gpu_blas_nodes(BVHNode* nodes, int count, GPUBVHNode* gpu_nodes);
void prepare_gpu_instances(BVHInstance* instances, int count, GPUBVHInstance* gpu_instances);
void prepare_gpu_tlas_nodes(TLASNode* nodes, int count, GPUTLASNode* gpu_nodes);

// Legacy compatibility (to be removed)
typedef BLAS BVH;
BVH* bvh_create(Triangle* triangles, int triangle_count, int max_triangles_per_leaf);
void bvh_destroy(BVH* bvh);

// Flatten BVH for GPU usage
void bvh_flatten_for_gpu(BVH* bvh, BVHNode** node_buffer, int** index_buffer, 
                         int* node_count, int* index_count);

// Ray-triangle intersection
bool ray_triangle_intersect(Vec3 ray_origin, Vec3 ray_direction, Triangle triangle,
                           float* t, float* u, float* v);

// AABB operations
AABB aabb_from_triangle(Triangle triangle);
AABB aabb_union(AABB a, AABB b);
bool aabb_ray_intersect(AABB aabb, Vec3 ray_origin, Vec3 ray_direction, float max_t);
float aabb_surface_area(AABB aabb);

// Vector operations
Vec3 vec3_add(Vec3 a, Vec3 b);
Vec3 vec3_sub(Vec3 a, Vec3 b);
Vec3 vec3_mul(Vec3 v, float s);
Vec3 vec3_cross(Vec3 a, Vec3 b);
float vec3_dot(Vec3 a, Vec3 b);
Vec3 vec3_normalize(Vec3 v);

#endif // BVH_H