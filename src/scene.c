#include "scene.h"
#include <stdlib.h>
#include <math.h>

// Generate triangles for a box (12 triangles, 2 per face)
int generate_box_triangles(Triangle* triangles, Vec3 center, Vec3 size, int material_id) {
    Vec3 half_size = vec3_mul(size, 0.5f);
    Vec3 vertices[8];
    
    // Generate 8 vertices of the box
    vertices[0] = vec3_add(center, (Vec3){-half_size.x, -half_size.y, -half_size.z}); // min
    vertices[1] = vec3_add(center, (Vec3){ half_size.x, -half_size.y, -half_size.z});
    vertices[2] = vec3_add(center, (Vec3){ half_size.x,  half_size.y, -half_size.z});
    vertices[3] = vec3_add(center, (Vec3){-half_size.x,  half_size.y, -half_size.z});
    vertices[4] = vec3_add(center, (Vec3){-half_size.x, -half_size.y,  half_size.z});
    vertices[5] = vec3_add(center, (Vec3){ half_size.x, -half_size.y,  half_size.z});
    vertices[6] = vec3_add(center, (Vec3){ half_size.x,  half_size.y,  half_size.z}); // max
    vertices[7] = vec3_add(center, (Vec3){-half_size.x,  half_size.y,  half_size.z});
    
    int tri_idx = 0;
    
    // Front face (z+)
    triangles[tri_idx] = (Triangle){vertices[4], vertices[5], vertices[6], {0, 0, 1}, material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[4], vertices[6], vertices[7], {0, 0, 1}, material_id};
    tri_idx++;
    
    // Back face (z-)
    triangles[tri_idx] = (Triangle){vertices[0], vertices[2], vertices[1], {0, 0, -1}, material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[0], vertices[3], vertices[2], {0, 0, -1}, material_id};
    tri_idx++;
    
    // Right face (x+)
    triangles[tri_idx] = (Triangle){vertices[1], vertices[2], vertices[6], {1, 0, 0}, material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[1], vertices[6], vertices[5], {1, 0, 0}, material_id};
    tri_idx++;
    
    // Left face (x-)
    triangles[tri_idx] = (Triangle){vertices[0], vertices[7], vertices[3], {-1, 0, 0}, material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[0], vertices[4], vertices[7], {-1, 0, 0}, material_id};
    tri_idx++;
    
    // Top face (y+)
    triangles[tri_idx] = (Triangle){vertices[3], vertices[7], vertices[6], {0, 1, 0}, material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[3], vertices[6], vertices[2], {0, 1, 0}, material_id};
    tri_idx++;
    
    // Bottom face (y-)
    triangles[tri_idx] = (Triangle){vertices[0], vertices[1], vertices[5], {0, -1, 0}, material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[0], vertices[5], vertices[4], {0, -1, 0}, material_id};
    tri_idx++;
    
    return tri_idx;
}

// Generate triangles for a sphere using icosphere subdivision
int generate_sphere_triangles(Triangle* triangles, Vec3 center, float radius, int material_id, int subdivisions) {
    // For simplicity, create a low-poly sphere using octahedron subdivision
    // This creates 8 triangles for subdivisions=0
    (void)subdivisions; // Currently unused, but reserved for future subdivision implementation
    
    Vec3 vertices[6];
    vertices[0] = vec3_add(center, vec3_mul((Vec3){1, 0, 0}, radius));   // +X
    vertices[1] = vec3_add(center, vec3_mul((Vec3){-1, 0, 0}, radius));  // -X
    vertices[2] = vec3_add(center, vec3_mul((Vec3){0, 1, 0}, radius));   // +Y
    vertices[3] = vec3_add(center, vec3_mul((Vec3){0, -1, 0}, radius));  // -Y
    vertices[4] = vec3_add(center, vec3_mul((Vec3){0, 0, 1}, radius));   // +Z
    vertices[5] = vec3_add(center, vec3_mul((Vec3){0, 0, -1}, radius));  // -Z
    
    int tri_idx = 0;
    
    // 8 triangular faces of an octahedron
    triangles[tri_idx] = (Triangle){vertices[0], vertices[2], vertices[4], vec3_normalize(vec3_cross(vec3_sub(vertices[2], vertices[0]), vec3_sub(vertices[4], vertices[0]))), material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[0], vertices[4], vertices[3], vec3_normalize(vec3_cross(vec3_sub(vertices[4], vertices[0]), vec3_sub(vertices[3], vertices[0]))), material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[0], vertices[3], vertices[5], vec3_normalize(vec3_cross(vec3_sub(vertices[3], vertices[0]), vec3_sub(vertices[5], vertices[0]))), material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[0], vertices[5], vertices[2], vec3_normalize(vec3_cross(vec3_sub(vertices[5], vertices[0]), vec3_sub(vertices[2], vertices[0]))), material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[1], vertices[4], vertices[2], vec3_normalize(vec3_cross(vec3_sub(vertices[4], vertices[1]), vec3_sub(vertices[2], vertices[1]))), material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[1], vertices[3], vertices[4], vec3_normalize(vec3_cross(vec3_sub(vertices[3], vertices[1]), vec3_sub(vertices[4], vertices[1]))), material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[1], vertices[5], vertices[3], vec3_normalize(vec3_cross(vec3_sub(vertices[5], vertices[1]), vec3_sub(vertices[3], vertices[1]))), material_id};
    tri_idx++;
    triangles[tri_idx] = (Triangle){vertices[1], vertices[2], vertices[5], vec3_normalize(vec3_cross(vec3_sub(vertices[2], vertices[1]), vec3_sub(vertices[5], vertices[1]))), material_id};
    tri_idx++;
    
    return tri_idx;
}

Scene* scene_create_test_scene(void) {
    Scene* scene = malloc(sizeof(Scene));
    if (!scene) return NULL;
    
    // Calculate total triangles needed: 3 boxes (12 each) + 2 spheres (8 each)
    int total_triangles = 3 * 12 + 2 * 8;
    scene->triangles = malloc(total_triangles * sizeof(Triangle));
    if (!scene->triangles) {
        free(scene);
        return NULL;
    }
    
    int tri_count = 0;
    
    // Add 3 boxes with different materials
    tri_count += generate_box_triangles(&scene->triangles[tri_count], (Vec3){-2, 0, 0}, (Vec3){1, 1, 1},      0); // Red material
    tri_count += generate_box_triangles(&scene->triangles[tri_count], (Vec3){2, 0, 0},  (Vec3){2, 2, 2},      1);  // Blue material
    tri_count += generate_box_triangles(&scene->triangles[tri_count], (Vec3){0, -2, 0}, (Vec3){20, 0.1f, 20}, 2); // Ground plane (green)
    
    // Add 2 spheres
    tri_count += generate_sphere_triangles(&scene->triangles[tri_count], (Vec3){0, 1, 1}, 0.5f, 3, 0); // Yellow sphere
    tri_count += generate_sphere_triangles(&scene->triangles[tri_count], (Vec3){0, 1, -1}, 0.3f, 4, 0); // Magenta sphere
    
    scene->triangle_count = tri_count;
    
    // Build BVH
    scene->bvh = bvh_create(scene->triangles, scene->triangle_count, 4);
    if (!scene->bvh) {
        free(scene->triangles);
        free(scene);
        return NULL;
    }
    
    return scene;
}

void scene_destroy(Scene* scene) {
    if (scene) {
        if (scene->bvh) bvh_destroy(scene->bvh);
        free(scene->triangles);
        free(scene);
    }
}