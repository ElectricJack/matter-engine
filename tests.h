#ifndef TESTS_H
#define TESTS_H

#include "surface.h"

// Main test function that runs all tests
bool run_all_tests(void);

// Individual test functions
bool test_duplicate_vertices(const Mesh* mesh);
bool test_degenerate_triangles(const Mesh* mesh);
bool test_watertight_mesh(const Mesh* mesh);
bool test_consistent_triangle_orientation(const Mesh* mesh);

// Unit tests for surface.c internal functions
bool test_vertex_hash_table(void);
bool test_edge_tracking(void);
bool test_grid_edge_indexing(void);
bool test_mesh_generation_simple(void);

// Test case functions
bool test_mesh_case(Mesh mesh, const char* test_name);
Mesh create_two_spheres_mesh(void);
Mesh create_single_sphere_mesh(void);
Mesh create_cube_sphere_intersection_mesh(void);
Mesh create_single_cube_mesh(void);

// Timeout-protected test functions
bool run_test_with_timeout(bool (*test_function)(), const char* test_name);
bool test_mesh_case_with_timeout(Mesh mesh, const char* name);
Mesh create_two_spheres_mesh_safe(void);
Mesh create_single_sphere_mesh_safe(void);
Mesh create_cube_sphere_intersection_mesh_safe(void);
Mesh create_single_cube_mesh_safe(void);

// Utility functions
void print_test_report(const char* test_name, bool success, const char* failure_details);
float get_vertex_distance(const Mesh* mesh, int index1, int index2);
bool are_vertices_equal(const Mesh* mesh, int index1, int index2, float epsilon);
bool is_edge_shared(const Mesh* mesh, int edge1_v1, int edge1_v2, int edge2_v1, int edge2_v2);
int find_edge_sharing_count(const Mesh* mesh, int triIndex);

// Constants for tests
#define VERTEX_EPSILON 0.0001f  // Threshold for considering vertices equal
#define TEST_PASSED true
#define TEST_FAILED false

#endif // TESTS_H