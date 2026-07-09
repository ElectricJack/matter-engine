#include "bvh.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

// Matrix operations implementation

Matrix4x4 matrix_identity() {
    Matrix4x4 m;
    memset(m.m, 0, sizeof(m.m));
    m.m[0] = 1.0f;   // [0,0]
    m.m[5] = 1.0f;   // [1,1]
    m.m[10] = 1.0f;  // [2,2]
    m.m[15] = 1.0f;  // [3,3]
    return m;
}

Matrix4x4 matrix_multiply(const Matrix4x4* a, const Matrix4x4* b) {
    Matrix4x4 result;
    
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            result.m[row * 4 + col] = 
                a->m[row * 4 + 0] * b->m[0 * 4 + col] +
                a->m[row * 4 + 1] * b->m[1 * 4 + col] +
                a->m[row * 4 + 2] * b->m[2 * 4 + col] +
                a->m[row * 4 + 3] * b->m[3 * 4 + col];
        }
    }
    
    return result;
}

Matrix4x4 matrix_inverse(const Matrix4x4* m) {
    /*
     * True 4x4 inverse via cofactor/adjugate method.
     *
     * The matrix is stored column-major: m[col*4 + row].
     * We label elements a00..a33 in row-major notation for clarity, then
     * read them out of the column-major array accordingly.
     *
     * Reference: raylib MatrixInvert (zlib licence), transcribed without
     * any raylib dependency.
     */
    const float* a = m->m; /* shorthand */

    /* Read rows from column-major storage: a[col*4 + row] */
    float a00 = a[0],  a01 = a[4],  a02 = a[8],  a03 = a[12];
    float a10 = a[1],  a11 = a[5],  a12 = a[9],  a13 = a[13];
    float a20 = a[2],  a21 = a[6],  a22 = a[10], a23 = a[14];
    float a30 = a[3],  a31 = a[7],  a32 = a[11], a33 = a[15];

    /* 2x2 minors needed for cofactors */
    float b00 = a00*a11 - a01*a10;
    float b01 = a00*a12 - a02*a10;
    float b02 = a00*a13 - a03*a10;
    float b03 = a01*a12 - a02*a11;
    float b04 = a01*a13 - a03*a11;
    float b05 = a02*a13 - a03*a12;
    float b06 = a20*a31 - a21*a30;
    float b07 = a20*a32 - a22*a30;
    float b08 = a20*a33 - a23*a30;
    float b09 = a21*a32 - a22*a31;
    float b10 = a21*a33 - a23*a31;
    float b11 = a22*a33 - a23*a32;

    /* Determinant */
    float det = b00*b11 - b01*b10 + b02*b09 + b03*b08 - b04*b07 + b05*b06;

    /* Guard near-zero determinant: return identity to avoid NaN/Inf */
    if (fabsf(det) < 1e-6f) {
        return matrix_identity();
    }

    float inv_det = 1.0f / det;

    /* Adjugate (transposed cofactor matrix), stored column-major */
    Matrix4x4 result;
    float* r = result.m;

    r[0]  = ( a11*b11 - a12*b10 + a13*b09) * inv_det;
    r[1]  = (-a10*b11 + a12*b08 - a13*b07) * inv_det;
    r[2]  = ( a10*b10 - a11*b08 + a13*b06) * inv_det;
    r[3]  = (-a10*b09 + a11*b07 - a12*b06) * inv_det;

    r[4]  = (-a01*b11 + a02*b10 - a03*b09) * inv_det;
    r[5]  = ( a00*b11 - a02*b08 + a03*b07) * inv_det;
    r[6]  = (-a00*b10 + a01*b08 - a03*b06) * inv_det;
    r[7]  = ( a00*b09 - a01*b07 + a02*b06) * inv_det;

    r[8]  = ( a31*b05 - a32*b04 + a33*b03) * inv_det;
    r[9]  = (-a30*b05 + a32*b02 - a33*b01) * inv_det;
    r[10] = ( a30*b04 - a31*b02 + a33*b00) * inv_det;
    r[11] = (-a30*b03 + a31*b01 - a32*b00) * inv_det;

    r[12] = (-a21*b05 + a22*b04 - a23*b03) * inv_det;
    r[13] = ( a20*b05 - a22*b02 + a23*b01) * inv_det;
    r[14] = (-a20*b04 + a21*b02 - a23*b00) * inv_det;
    r[15] = ( a20*b03 - a21*b01 + a22*b00) * inv_det;

    return result;
}

Vec3 matrix_transform_point(const Matrix4x4* m, Vec3 p) {
    Vec3 result;
    result.x = m->m[0] * p.x + m->m[4] * p.y + m->m[8] * p.z + m->m[12];
    result.y = m->m[1] * p.x + m->m[5] * p.y + m->m[9] * p.z + m->m[13];
    result.z = m->m[2] * p.x + m->m[6] * p.y + m->m[10] * p.z + m->m[14];
    return result;
}

Vec3 matrix_transform_vector(const Matrix4x4* m, Vec3 v) {
    Vec3 result;
    result.x = m->m[0] * v.x + m->m[4] * v.y + m->m[8] * v.z;
    result.y = m->m[1] * v.x + m->m[5] * v.y + m->m[9] * v.z;
    result.z = m->m[2] * v.x + m->m[6] * v.y + m->m[10] * v.z;
    return result;
}

// Advanced matrix creation functions

Matrix4x4 matrix_translation(float x, float y, float z) {
    Matrix4x4 m = matrix_identity();
    m.m[12] = x; // Translation X
    m.m[13] = y; // Translation Y
    m.m[14] = z; // Translation Z
    return m;
}

Matrix4x4 matrix_scale(float sx, float sy, float sz) {
    Matrix4x4 m = matrix_identity();
    m.m[0] = sx;  // Scale X
    m.m[5] = sy;  // Scale Y
    m.m[10] = sz; // Scale Z
    return m;
}

Matrix4x4 matrix_rotation_x(float angle_radians) {
    Matrix4x4 m = matrix_identity();
    float c = cosf(angle_radians);
    float s = sinf(angle_radians);
    
    m.m[5] = c;   // [1,1]
    m.m[6] = s;   // [1,2]
    m.m[9] = -s;  // [2,1]
    m.m[10] = c;  // [2,2]
    
    return m;
}

Matrix4x4 matrix_rotation_y(float angle_radians) {
    Matrix4x4 m = matrix_identity();
    float c = cosf(angle_radians);
    float s = sinf(angle_radians);
    
    m.m[0] = c;   // [0,0]
    m.m[2] = -s;  // [0,2]
    m.m[8] = s;   // [2,0]
    m.m[10] = c;  // [2,2]
    
    return m;
}

Matrix4x4 matrix_rotation_z(float angle_radians) {
    Matrix4x4 m = matrix_identity();
    float c = cosf(angle_radians);
    float s = sinf(angle_radians);
    
    m.m[0] = c;   // [0,0]
    m.m[1] = s;   // [0,1]
    m.m[4] = -s;  // [1,0]
    m.m[5] = c;   // [1,1]
    
    return m;
}

Matrix4x4 matrix_rotation_axis(Vec3 axis, float angle_radians) {
    // Normalize the axis
    float len = sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (len < 1e-6f) return matrix_identity();
    
    axis.x /= len;
    axis.y /= len;
    axis.z /= len;
    
    float c = cosf(angle_radians);
    float s = sinf(angle_radians);
    float one_minus_c = 1.0f - c;
    
    Matrix4x4 m = matrix_identity();
    
    // Rodrigues' rotation formula
    m.m[0] = c + axis.x * axis.x * one_minus_c;
    m.m[1] = axis.x * axis.y * one_minus_c + axis.z * s;
    m.m[2] = axis.x * axis.z * one_minus_c - axis.y * s;
    
    m.m[4] = axis.y * axis.x * one_minus_c - axis.z * s;
    m.m[5] = c + axis.y * axis.y * one_minus_c;
    m.m[6] = axis.y * axis.z * one_minus_c + axis.x * s;
    
    m.m[8] = axis.z * axis.x * one_minus_c + axis.y * s;
    m.m[9] = axis.z * axis.y * one_minus_c - axis.x * s;
    m.m[10] = c + axis.z * axis.z * one_minus_c;
    
    return m;
}

// GPU data preparation functions

void prepare_gpu_triangles(Triangle* triangles, int count, GPUTriangle* gpu_triangles) {
    for (int i = 0; i < count; i++) {
        Triangle* tri = &triangles[i];
        GPUTriangle* gpu_tri = &gpu_triangles[i];
        
        gpu_tri->v0x = tri->v0.x;
        gpu_tri->v0y = tri->v0.y;
        gpu_tri->v0z = tri->v0.z;
        gpu_tri->dummy1 = 0.0f;
        
        gpu_tri->v1x = tri->v1.x;
        gpu_tri->v1y = tri->v1.y;
        gpu_tri->v1z = tri->v1.z;
        gpu_tri->dummy2 = 0.0f;
        
        gpu_tri->v2x = tri->v2.x;
        gpu_tri->v2y = tri->v2.y;
        gpu_tri->v2z = tri->v2.z;
        gpu_tri->dummy3 = 0.0f;
        
        gpu_tri->n0x = tri->n0.x;
        gpu_tri->n0y = tri->n0.y;
        gpu_tri->n0z = tri->n0.z;
        gpu_tri->dummy4 = 0.0f;
        
        gpu_tri->n1x = tri->n1.x;
        gpu_tri->n1y = tri->n1.y;
        gpu_tri->n1z = tri->n1.z;
        gpu_tri->dummy5 = 0.0f;
        
        gpu_tri->n2x = tri->n2.x;
        gpu_tri->n2y = tri->n2.y;
        gpu_tri->n2z = tri->n2.z;
        gpu_tri->dummy6 = 0.0f;
        
        gpu_tri->cx = tri->centroid.x;
        gpu_tri->cy = tri->centroid.y;
        gpu_tri->cz = tri->centroid.z;
        gpu_tri->dummy7 = 0.0f;
    }
}

void prepare_gpu_blas_nodes(BVHNode* nodes, int count, GPUBVHNode* gpu_nodes) {
    for (int i = 0; i < count; i++) {
        BVHNode* node = &nodes[i];
        GPUBVHNode* gpu_node = &gpu_nodes[i];
        
        gpu_node->minx = node->aabb_min.x;
        gpu_node->miny = node->aabb_min.y;
        gpu_node->minz = node->aabb_min.z;
        gpu_node->left_first = (int)node->left_first;
        
        gpu_node->maxx = node->aabb_max.x;
        gpu_node->maxy = node->aabb_max.y;
        gpu_node->maxz = node->aabb_max.z;
        gpu_node->tri_count = (int)node->tri_count;
    }
}

void prepare_gpu_instances(BVHInstance* instances, int count, GPUBVHInstance* gpu_instances) {
    for (int i = 0; i < count; i++) {
        BVHInstance* inst = &instances[i];
        GPUBVHInstance* gpu_inst = &gpu_instances[i];
        
        // Copy transform matrix
        memcpy(gpu_inst->transform, inst->transform.m, sizeof(float) * 16);
        memcpy(gpu_inst->inv_transform, inst->inv_transform.m, sizeof(float) * 16);
        
        gpu_inst->blas_start_index = inst->blas_start_index;
        gpu_inst->instance_id = inst->instance_id;
        
        // Clear padding
        memset(gpu_inst->padding, 0, sizeof(gpu_inst->padding));
    }
}

void prepare_gpu_tlas_nodes(TLASNode* nodes, int count, GPUTLASNode* gpu_nodes) {
    for (int i = 0; i < count; i++) {
        TLASNode* node = &nodes[i];
        GPUTLASNode* gpu_node = &gpu_nodes[i];
        
        gpu_node->minx = node->aabb_min.x;
        gpu_node->miny = node->aabb_min.y;
        gpu_node->minz = node->aabb_min.z;
        gpu_node->left_right = node->left_right;
        
        gpu_node->maxx = node->aabb_max.x;
        gpu_node->maxy = node->aabb_max.y;
        gpu_node->maxz = node->aabb_max.z;
        gpu_node->blas_index = node->blas_index;
    }
}

// BLAS operations

BLAS* blas_create(Triangle* triangles, int triangle_count, int max_triangles_per_leaf) {
    if (triangle_count == 0) return NULL;
    
    BLAS* blas = malloc(sizeof(BLAS));
    if (!blas) return NULL;
    
    blas->triangles = triangles;
    blas->triangle_count = triangle_count;
    blas->max_triangles_per_leaf = max_triangles_per_leaf;
    
    // Allocate triangle indices
    blas->triangle_indices = malloc(triangle_count * sizeof(int));
    if (!blas->triangle_indices) {
        free(blas);
        return NULL;
    }
    
    // Initialize indices
    for (int i = 0; i < triangle_count; i++) {
        blas->triangle_indices[i] = i;
    }
    
    // Allocate nodes (worst case: 2 * triangle_count - 1)
    int max_nodes = 2 * triangle_count;
    blas->nodes = malloc(max_nodes * sizeof(BVHNode));
    if (!blas->nodes) {
        free(blas->triangle_indices);
        free(blas);
        return NULL;
    }
    
    blas->node_count = 0;
    
    return blas;
}

void blas_destroy(BLAS* blas) {
    if (blas) {
        free(blas->triangle_indices);
        free(blas->nodes);
        free(blas);
    }
}

// Helper functions for BLAS building

AABB aabb_from_triangle(Triangle triangle) {
    AABB aabb;
    aabb.min.x = fminf(fminf(triangle.v0.x, triangle.v1.x), triangle.v2.x);
    aabb.min.y = fminf(fminf(triangle.v0.y, triangle.v1.y), triangle.v2.y);
    aabb.min.z = fminf(fminf(triangle.v0.z, triangle.v1.z), triangle.v2.z);
    aabb.max.x = fmaxf(fmaxf(triangle.v0.x, triangle.v1.x), triangle.v2.x);
    aabb.max.y = fmaxf(fmaxf(triangle.v0.y, triangle.v1.y), triangle.v2.y);
    aabb.max.z = fmaxf(fmaxf(triangle.v0.z, triangle.v1.z), triangle.v2.z);
    return aabb;
}

static AABB compute_triangle_aabb(Triangle* tri) {
    return aabb_from_triangle(*tri);
}

AABB aabb_union(AABB a, AABB b) {
    AABB result;
    result.min.x = fminf(a.min.x, b.min.x);
    result.min.y = fminf(a.min.y, b.min.y);
    result.min.z = fminf(a.min.z, b.min.z);
    result.max.x = fmaxf(a.max.x, b.max.x);
    result.max.y = fmaxf(a.max.y, b.max.y);
    result.max.z = fmaxf(a.max.z, b.max.z);
    return result;
}

float aabb_surface_area(AABB aabb) {
    Vec3 extent = {
        aabb.max.x - aabb.min.x,
        aabb.max.y - aabb.min.y,
        aabb.max.z - aabb.min.z
    };
    return 2.0f * (extent.x * extent.y + extent.y * extent.z + extent.z * extent.x);
}

static AABB compute_bounding_box(Triangle* triangles, int* indices, int count) {
    if (count == 0) {
        AABB empty = {{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
        return empty;
    }
    
    AABB aabb = compute_triangle_aabb(&triangles[indices[0]]);
    for (int i = 1; i < count; i++) {
        AABB tri_aabb = compute_triangle_aabb(&triangles[indices[i]]);
        aabb = aabb_union(aabb, tri_aabb);
    }
    return aabb;
}

static int partition_triangles(Triangle* triangles, int* indices, int count, int axis, float split_pos) {
    int left = 0, right = count - 1;
    
    while (left <= right) {
        // Use pre-computed centroid
        Triangle* tri = &triangles[indices[left]];
        float pos = (axis == 0) ? tri->centroid.x : (axis == 1) ? tri->centroid.y : tri->centroid.z;
        
        if (pos < split_pos) {
            left++;
        } else {
            // Swap with right
            int temp = indices[left];
            indices[left] = indices[right];
            indices[right] = temp;
            right--;
        }
    }
    
    return left;
}

static float evaluate_sah(Triangle* triangles, int* indices, int count, int axis, float split_pos) {
    // Count triangles on each side and compute their AABBs
    int left_count = 0;
    AABB left_aabb = {{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
    AABB right_aabb = {{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
    
    for (int i = 0; i < count; i++) {
        Triangle* tri = &triangles[indices[i]];
        float pos = (axis == 0) ? tri->centroid.x : (axis == 1) ? tri->centroid.y : tri->centroid.z;
        
        AABB tri_aabb = compute_triangle_aabb(tri);
        
        if (pos < split_pos) {
            left_count++;
            if (left_count == 1) {
                left_aabb = tri_aabb;
            } else {
                left_aabb = aabb_union(left_aabb, tri_aabb);
            }
        } else {
            if (count - left_count == 1) {
                right_aabb = tri_aabb;
            } else {
                right_aabb = aabb_union(right_aabb, tri_aabb);
            }
        }
    }
    
    int right_count = count - left_count;
    
    // Avoid degenerate splits
    if (left_count == 0 || right_count == 0) {
        return FLT_MAX;
    }
    
    // Calculate SAH cost
    float left_area = aabb_surface_area(left_aabb);
    float right_area = aabb_surface_area(right_aabb);
    
    return left_count * left_area + right_count * right_area;
}

static void subdivide_blas(BLAS* blas, uint32_t node_idx, int* indices, int count, uint32_t* next_node_idx) {
    BVHNode* node = &blas->nodes[node_idx];
    
    // Compute bounding box for this node
    AABB node_aabb = compute_bounding_box(blas->triangles, indices, count);
    node->aabb_min = node_aabb.min;
    node->aabb_max = node_aabb.max;
    
    // Check if we should make this a leaf
    if (count <= blas->max_triangles_per_leaf) {
        // Create leaf node
        node->left_first = indices - blas->triangle_indices;
        node->tri_count = count;
        return;
    }
    
    // Find best split using SAH
    int best_axis = -1;
    float best_split_pos = 0.0f;
    float best_cost = FLT_MAX;
    
    for (int axis = 0; axis < 3; axis++) {
        // Calculate centroid bounds for this axis
        float min_centroid = FLT_MAX;
        float max_centroid = -FLT_MAX;
        
        for (int i = 0; i < count; i++) {
            Triangle* tri = &blas->triangles[indices[i]];
            float centroid_pos = (axis == 0) ? tri->centroid.x : (axis == 1) ? tri->centroid.y : tri->centroid.z;
            min_centroid = fminf(min_centroid, centroid_pos);
            max_centroid = fmaxf(max_centroid, centroid_pos);
        }
        
        if (max_centroid - min_centroid < 1e-6f) continue; // Skip if no spread on this axis
        
        // Test multiple split positions
        const int num_splits = 8;
        for (int i = 1; i < num_splits; i++) {
            float t = (float)i / (float)num_splits;
            float split_pos = min_centroid + t * (max_centroid - min_centroid);
            
            float cost = evaluate_sah(blas->triangles, indices, count, axis, split_pos);
            
            if (cost < best_cost) {
                best_cost = cost;
                best_axis = axis;
                best_split_pos = split_pos;
            }
        }
    }
    
    // If no good split found, make leaf
    if (best_axis == -1 || best_cost >= count * aabb_surface_area(node_aabb)) {
        // Create leaf node
        node->left_first = indices - blas->triangle_indices;
        node->tri_count = count;
        return;
    }
    
    // Partition triangles
    int left_count = partition_triangles(blas->triangles, indices, count, best_axis, best_split_pos);
    
    // Ensure we don't create degenerate splits
    if (left_count == 0 || left_count == count) {
        // Create leaf node as fallback
        node->left_first = indices - blas->triangle_indices;
        node->tri_count = count;
        return;
    }
    
    // Create internal node
    uint32_t left_child_idx = (*next_node_idx)++;
    uint32_t right_child_idx = (*next_node_idx)++;
    
    node->left_first = left_child_idx;
    node->tri_count = 0; // Internal node
    
    // Recursively build children
    subdivide_blas(blas, left_child_idx, indices, left_count, next_node_idx);
    subdivide_blas(blas, right_child_idx, indices + left_count, count - left_count, next_node_idx);
}

void blas_build(BLAS* blas) {
    if (!blas || blas->triangle_count == 0) return;
    
    // Ensure triangles have pre-computed centroids
    for (int i = 0; i < blas->triangle_count; i++) {
        Triangle* tri = &blas->triangles[i];
        tri->centroid.x = (tri->v0.x + tri->v1.x + tri->v2.x) / 3.0f;
        tri->centroid.y = (tri->v0.y + tri->v1.y + tri->v2.y) / 3.0f;
        tri->centroid.z = (tri->v0.z + tri->v1.z + tri->v2.z) / 3.0f;
    }
    
    // Start building from root
    uint32_t next_node_idx = 1;
    subdivide_blas(blas, 0, blas->triangle_indices, blas->triangle_count, &next_node_idx);
    blas->node_count = next_node_idx;
    
    printf("BLAS built: %d nodes for %d triangles\n", blas->node_count, blas->triangle_count);
}

// BVH instance operations

BVHInstance* bvh_instance_create(BLAS* blas, uint32_t instance_id) {
    if (!blas) return NULL;
    
    BVHInstance* instance = malloc(sizeof(BVHInstance));
    if (!instance) return NULL;
    
    instance->blas = blas;
    instance->instance_id = instance_id;
    instance->transform = matrix_identity();
    instance->inv_transform = matrix_identity();
    instance->blas_start_index = 0; // Will be set when building combined BLAS array
    
    // Initialize world bounds to invalid state
    instance->world_bounds.min = (Vec3){FLT_MAX, FLT_MAX, FLT_MAX};
    instance->world_bounds.max = (Vec3){-FLT_MAX, -FLT_MAX, -FLT_MAX};
    
    return instance;
}

void bvh_instance_destroy(BVHInstance* instance) {
    if (instance) {
        free(instance);
    }
}

void bvh_instance_set_transform(BVHInstance* instance, const Matrix4x4* transform) {
    if (!instance || !transform) return;
    
    instance->transform = *transform;
    instance->inv_transform = matrix_inverse(transform);
    
    // Recalculate world bounds
    // For now, use simple AABB transformation (8 corner points)
    if (instance->blas && instance->blas->node_count > 0) {
        BVHNode* root = &instance->blas->nodes[0];
        
        // Transform all 8 corners of the AABB
        Vec3 corners[8];
        corners[0] = (Vec3){root->aabb_min.x, root->aabb_min.y, root->aabb_min.z};
        corners[1] = (Vec3){root->aabb_max.x, root->aabb_min.y, root->aabb_min.z};
        corners[2] = (Vec3){root->aabb_min.x, root->aabb_max.y, root->aabb_min.z};
        corners[3] = (Vec3){root->aabb_max.x, root->aabb_max.y, root->aabb_min.z};
        corners[4] = (Vec3){root->aabb_min.x, root->aabb_min.y, root->aabb_max.z};
        corners[5] = (Vec3){root->aabb_max.x, root->aabb_min.y, root->aabb_max.z};
        corners[6] = (Vec3){root->aabb_min.x, root->aabb_max.y, root->aabb_max.z};
        corners[7] = (Vec3){root->aabb_max.x, root->aabb_max.y, root->aabb_max.z};
        
        // Transform first corner to initialize bounds
        instance->world_bounds.min = matrix_transform_point(transform, corners[0]);
        instance->world_bounds.max = instance->world_bounds.min;
        
        // Transform remaining corners and expand bounds
        for (int i = 1; i < 8; i++) {
            Vec3 transformed = matrix_transform_point(transform, corners[i]);
            
            if (transformed.x < instance->world_bounds.min.x) instance->world_bounds.min.x = transformed.x;
            if (transformed.y < instance->world_bounds.min.y) instance->world_bounds.min.y = transformed.y;
            if (transformed.z < instance->world_bounds.min.z) instance->world_bounds.min.z = transformed.z;
            if (transformed.x > instance->world_bounds.max.x) instance->world_bounds.max.x = transformed.x;
            if (transformed.y > instance->world_bounds.max.y) instance->world_bounds.max.y = transformed.y;
            if (transformed.z > instance->world_bounds.max.z) instance->world_bounds.max.z = transformed.z;
        }
    }
}

// TLAS operations

TLAS* tlas_create(int max_instances) {
    TLAS* tlas = malloc(sizeof(TLAS));
    if (!tlas) return NULL;
    
    tlas->instances = malloc(max_instances * sizeof(BVHInstance));
    if (!tlas->instances) {
        free(tlas);
        return NULL;
    }
    
    tlas->nodes = malloc(max_instances * 2 * sizeof(TLASNode));  // Worst case nodes
    if (!tlas->nodes) {
        free(tlas->instances);
        free(tlas);
        return NULL;
    }
    
    tlas->max_instances = max_instances;
    tlas->instance_count = 0;
    tlas->node_count = 0;
    
    return tlas;
}

void tlas_destroy(TLAS* tlas) {
    if (tlas) {
        free(tlas->instances);
        free(tlas->nodes);
        free(tlas);
    }
}

void tlas_add_instance(TLAS* tlas, BVHInstance* instance) {
    if (!tlas || !instance || tlas->instance_count >= tlas->max_instances) return;
    
    tlas->instances[tlas->instance_count] = *instance;
    tlas->instance_count++;
}

// Helper functions for TLAS building

static AABB compute_instance_bounds(BVHInstance* instances, int* indices, int count) {
    if (count == 0) {
        AABB empty = {{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
        return empty;
    }
    
    AABB aabb = instances[indices[0]].world_bounds;
    for (int i = 1; i < count; i++) {
        aabb = aabb_union(aabb, instances[indices[i]].world_bounds);
    }
    return aabb;
}

static Vec3 compute_instance_centroid(BVHInstance* instance) {
    Vec3 centroid;
    centroid.x = (instance->world_bounds.min.x + instance->world_bounds.max.x) * 0.5f;
    centroid.y = (instance->world_bounds.min.y + instance->world_bounds.max.y) * 0.5f;
    centroid.z = (instance->world_bounds.min.z + instance->world_bounds.max.z) * 0.5f;
    return centroid;
}

static int partition_instances(BVHInstance* instances, int* indices, int count, int axis, float split_pos) {
    int left = 0, right = count - 1;
    
    while (left <= right) {
        Vec3 centroid = compute_instance_centroid(&instances[indices[left]]);
        float pos = (axis == 0) ? centroid.x : (axis == 1) ? centroid.y : centroid.z;
        
        if (pos < split_pos) {
            left++;
        } else {
            // Swap with right
            int temp = indices[left];
            indices[left] = indices[right];
            indices[right] = temp;
            right--;
        }
    }
    
    return left;
}

static float evaluate_tlas_sah(BVHInstance* instances, int* indices, int count, int axis, float split_pos) {
    // Count instances on each side and compute their AABBs
    int left_count = 0;
    AABB left_aabb = {{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
    AABB right_aabb = {{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
    
    for (int i = 0; i < count; i++) {
        Vec3 centroid = compute_instance_centroid(&instances[indices[i]]);
        float pos = (axis == 0) ? centroid.x : (axis == 1) ? centroid.y : centroid.z;
        
        AABB inst_aabb = instances[indices[i]].world_bounds;
        
        if (pos < split_pos) {
            left_count++;
            if (left_count == 1) {
                left_aabb = inst_aabb;
            } else {
                left_aabb = aabb_union(left_aabb, inst_aabb);
            }
        } else {
            if (count - left_count == 1) {
                right_aabb = inst_aabb;
            } else {
                right_aabb = aabb_union(right_aabb, inst_aabb);
            }
        }
    }
    
    int right_count = count - left_count;
    
    // Avoid degenerate splits
    if (left_count == 0 || right_count == 0) {
        return FLT_MAX;
    }
    
    // Calculate SAH cost (simplified for instances)
    float left_area = aabb_surface_area(left_aabb);
    float right_area = aabb_surface_area(right_aabb);
    
    return left_count * left_area + right_count * right_area;
}

static void subdivide_tlas(TLAS* tlas, uint32_t node_idx, int* indices, int count, uint32_t* next_node_idx) {
    TLASNode* node = &tlas->nodes[node_idx];
    
    // Compute bounding box for this node
    AABB node_aabb = compute_instance_bounds(tlas->instances, indices, count);
    node->aabb_min = node_aabb.min;
    node->aabb_max = node_aabb.max;
    
    // Check if we should make this a leaf (TLAS leaves typically contain 1 instance)
    if (count == 1) {
        // Create leaf node pointing to this instance
        node->left_right = 0; // 0 indicates leaf
        node->blas_index = indices[0]; // Index of the instance
        return;
    }
    
    // Find best split using SAH
    int best_axis = -1;
    float best_split_pos = 0.0f;
    float best_cost = FLT_MAX;
    
    for (int axis = 0; axis < 3; axis++) {
        // Calculate centroid bounds for this axis
        float min_centroid = FLT_MAX;
        float max_centroid = -FLT_MAX;
        
        for (int i = 0; i < count; i++) {
            Vec3 centroid = compute_instance_centroid(&tlas->instances[indices[i]]);
            float centroid_pos = (axis == 0) ? centroid.x : (axis == 1) ? centroid.y : centroid.z;
            min_centroid = fminf(min_centroid, centroid_pos);
            max_centroid = fmaxf(max_centroid, centroid_pos);
        }
        
        if (max_centroid - min_centroid < 1e-6f) continue; // Skip if no spread on this axis
        
        // Test multiple split positions
        const int num_splits = 4; // Fewer splits for TLAS (typically has fewer instances)
        for (int i = 1; i < num_splits; i++) {
            float t = (float)i / (float)num_splits;
            float split_pos = min_centroid + t * (max_centroid - min_centroid);
            
            float cost = evaluate_tlas_sah(tlas->instances, indices, count, axis, split_pos);
            
            if (cost < best_cost) {
                best_cost = cost;
                best_axis = axis;
                best_split_pos = split_pos;
            }
        }
    }
    
    // If no good split found, make leaf with first instance (fallback)
    if (best_axis == -1) {
        node->left_right = 0; // Leaf node
        node->blas_index = indices[0]; // First instance
        return;
    }
    
    // Partition instances
    int left_count = partition_instances(tlas->instances, indices, count, best_axis, best_split_pos);
    
    // Ensure we don't create degenerate splits
    if (left_count == 0 || left_count == count) {
        // Create leaf node as fallback
        node->left_right = 0; // Leaf node
        node->blas_index = indices[0]; // First instance
        return;
    }
    
    // Create internal node
    uint32_t left_child_idx = (*next_node_idx)++;
    uint32_t right_child_idx = (*next_node_idx)++;
    
    // Pack left and right child indices into single 32-bit value
    node->left_right = (right_child_idx << 16) | left_child_idx;
    node->blas_index = 0; // Not used for internal nodes
    
    // Recursively build children
    subdivide_tlas(tlas, left_child_idx, indices, left_count, next_node_idx);
    subdivide_tlas(tlas, right_child_idx, indices + left_count, count - left_count, next_node_idx);
}

void tlas_build(TLAS* tlas) {
    if (!tlas || tlas->instance_count == 0) return;
    
    // Create instance indices array for building
    int* instance_indices = malloc(tlas->instance_count * sizeof(int));
    if (!instance_indices) return;
    
    // Initialize indices
    for (int i = 0; i < tlas->instance_count; i++) {
        instance_indices[i] = i;
    }
    
    // Start building from root
    uint32_t next_node_idx = 1;
    subdivide_tlas(tlas, 0, instance_indices, tlas->instance_count, &next_node_idx);
    tlas->node_count = next_node_idx;
    
    free(instance_indices);
    
    printf("TLAS built: %d nodes for %d instances\n", tlas->node_count, tlas->instance_count);
}

// Vector operations
Vec3 vec3_add(Vec3 a, Vec3 b) {
    return (Vec3){a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 vec3_sub(Vec3 a, Vec3 b) {
    return (Vec3){a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 vec3_mul(Vec3 v, float s) {
    return (Vec3){v.x * s, v.y * s, v.z * s};
}

Vec3 vec3_cross(Vec3 a, Vec3 b) {
    return (Vec3){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

float vec3_dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 vec3_normalize(Vec3 v) {
    float len = sqrt(vec3_dot(v, v));
    if (len > 0.0f) {
        return vec3_mul(v, 1.0f / len);
    }
    return (Vec3){0, 0, 0};
}

// AABB ray intersection (implementation for legacy compatibility)
bool aabb_ray_intersect(AABB aabb, Vec3 ray_origin, Vec3 ray_direction, float max_t) {
    float inv_dir_x = 1.0f / ray_direction.x;
    float inv_dir_y = 1.0f / ray_direction.y;
    float inv_dir_z = 1.0f / ray_direction.z;
    
    float t1 = (aabb.min.x - ray_origin.x) * inv_dir_x;
    float t2 = (aabb.max.x - ray_origin.x) * inv_dir_x;
    float t3 = (aabb.min.y - ray_origin.y) * inv_dir_y;
    float t4 = (aabb.max.y - ray_origin.y) * inv_dir_y;
    float t5 = (aabb.min.z - ray_origin.z) * inv_dir_z;
    float t6 = (aabb.max.z - ray_origin.z) * inv_dir_z;
    
    float tmin = fmaxf(fmaxf(fminf(t1, t2), fminf(t3, t4)), fminf(t5, t6));
    float tmax = fminf(fminf(fmaxf(t1, t2), fmaxf(t3, t4)), fmaxf(t5, t6));
    
    return tmax >= 0 && tmin <= tmax && tmin <= max_t;
}

// Ray-triangle intersection using Möller-Trumbore algorithm
bool ray_triangle_intersect(Vec3 ray_origin, Vec3 ray_direction, Triangle triangle,
                           float* t, float* u, float* v) {
    const float EPSILON = 0.0000001f;
    
    Vec3 edge1 = vec3_sub(triangle.v1, triangle.v0);
    Vec3 edge2 = vec3_sub(triangle.v2, triangle.v0);
    Vec3 h = vec3_cross(ray_direction, edge2);
    float a = vec3_dot(edge1, h);
    
    if (a > -EPSILON && a < EPSILON) return false;
    
    float f = 1.0f / a;
    Vec3 s = vec3_sub(ray_origin, triangle.v0);
    *u = f * vec3_dot(s, h);
    
    if (*u < 0.0f || *u > 1.0f) return false;
    
    Vec3 q = vec3_cross(s, edge1);
    *v = f * vec3_dot(ray_direction, q);
    
    if (*v < 0.0f || *u + *v > 1.0f) return false;
    
    *t = f * vec3_dot(edge2, q);
    
    return *t > EPSILON;
}

// Legacy compatibility function - delegates to new BLAS implementation
BVH* bvh_create(Triangle* triangles, int triangle_count, int max_triangles_per_leaf) {
    return blas_create(triangles, triangle_count, max_triangles_per_leaf);
}

void bvh_destroy(BVH* bvh) {
    blas_destroy(bvh);  // Delegate to BLAS destroy
}

void bvh_flatten_for_gpu(BVH* bvh, BVHNode** node_buffer, int** index_buffer, 
                         int* node_count, int* index_count) {
    if (!bvh) return;
    
    *node_buffer = malloc(bvh->node_count * sizeof(BVHNode));
    *index_buffer = malloc(bvh->triangle_count * sizeof(int));
    
    if (*node_buffer && *index_buffer) {
        memcpy(*node_buffer, bvh->nodes, bvh->node_count * sizeof(BVHNode));
        memcpy(*index_buffer, bvh->triangle_indices, bvh->triangle_count * sizeof(int));
        *node_count = bvh->node_count;
        *index_count = bvh->triangle_count;
    }
}