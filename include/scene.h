#ifndef SCENE_H
#define SCENE_H

#include "bvh.h"

typedef struct {
    Triangle* triangles;
    int triangle_count;
    BVH* bvh;
} Scene;

// Create a test scene with boxes and spheres
Scene* scene_create_test_scene(void);

// Destroy scene and free memory
void scene_destroy(Scene* scene);

// Generate triangles for a box at given position and size
int generate_box_triangles(Triangle* triangles, Vec3 center, Vec3 size, int material_id);

// Generate triangles for a sphere at given position and radius
int generate_sphere_triangles(Triangle* triangles, Vec3 center, float radius, int material_id, int subdivisions);

#endif // SCENE_H