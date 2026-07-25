#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <vector>
#include <set>
#include <map>
#include <cmath>

extern "C" {
    #include "raylib.h"
}

#include "../include/cluster.h"
#include "../include/cell.h"
#include "../include/blas_manager.hpp"
#include "../include/tlas_manager.hpp"

// Test utilities
bool vectors_equal(const Vector3& a, const Vector3& b, float epsilon = 1e-6f) {
    return (fabs(a.x - b.x) < epsilon && 
            fabs(a.y - b.y) < epsilon && 
            fabs(a.z - b.z) < epsilon);
}

// Test to verify cell coordinate hashing produces unique, non-overlapping cells
bool test_cell_coordinate_uniqueness() {
    printf("=== Testing Cell Coordinate Uniqueness ===\n");
    
    BLASManager blas_manager;
    TLASManager tlas_manager(1000); // Pass capacity
    Cluster cluster(0, blas_manager, tlas_manager, 1.0f);
    
    // Add particles in a grid pattern
    float spacing = 0.5f;
    int grid_size = 5;
    
    printf("Adding %d particles in a %dx%dx%d grid...\n", 
           grid_size * grid_size * grid_size, grid_size, grid_size, grid_size);
    
    for (int x = 0; x < grid_size; x++) {
        for (int y = 0; y < grid_size; y++) {
            for (int z = 0; z < grid_size; z++) {
                Vector3 pos = {x * spacing, y * spacing, z * spacing};
                cluster.add_particle(pos, 0.2f, 0);
            }
        }
    }
    
    // Rebuild cells
    cluster.rebuild_dirty_cells();
    
    // Get all cells
    Vector3 min_bound = {-1.0f, -1.0f, -1.0f};
    Vector3 max_bound = {grid_size * spacing + 1.0f, grid_size * spacing + 1.0f, grid_size * spacing + 1.0f};
    auto cells = cluster.get_cells_in_region(min_bound, max_bound);
    
    printf("Found %zu cells\n", cells.size());
    
    // Test 1: Check for coordinate uniqueness
    std::map<std::tuple<float, float, float, int>, int> coordinate_count;
    
    for (const auto* cell : cells) {
        auto key = std::make_tuple(cell->coordinates.x, cell->coordinates.y, cell->coordinates.z, cell->size_power);
        coordinate_count[key]++;
    }
    
    bool unique_coordinates = true;
    for (const auto& entry : coordinate_count) {
        if (entry.second > 1) {
            auto [x, y, z, power] = entry.first;
            printf("ERROR: Cell coordinate (%.0f,%.0f,%.0f,power=%d) appears %d times!\n", 
                   x, y, z, power, entry.second);
            unique_coordinates = false;
        }
    }
    
    // Test 2: Check for overlapping bounds
    bool no_overlaps = true;
    for (size_t i = 0; i < cells.size(); i++) {
        for (size_t j = i + 1; j < cells.size(); j++) {
            const Cell* cell1 = cells[i];
            const Cell* cell2 = cells[j];
            
            // Check if bounding boxes overlap
            bool overlaps = !(cell1->max_bound.x <= cell2->min_bound.x || 
                             cell2->max_bound.x <= cell1->min_bound.x ||
                             cell1->max_bound.y <= cell2->min_bound.y || 
                             cell2->max_bound.y <= cell1->min_bound.y ||
                             cell1->max_bound.z <= cell2->min_bound.z || 
                             cell2->max_bound.z <= cell1->min_bound.z);
            
            if (overlaps) {
                printf("ERROR: Cell (%.0f,%.0f,%.0f,size=%.1f) overlaps with Cell (%.0f,%.0f,%.0f,size=%.1f)!\n",
                       cell1->coordinates.x, cell1->coordinates.y, cell1->coordinates.z, cell1->actual_size,
                       cell2->coordinates.x, cell2->coordinates.y, cell2->coordinates.z, cell2->actual_size);
                printf("  Cell1 bounds: (%.2f,%.2f,%.2f) to (%.2f,%.2f,%.2f)\n",
                       cell1->min_bound.x, cell1->min_bound.y, cell1->min_bound.z,
                       cell1->max_bound.x, cell1->max_bound.y, cell1->max_bound.z);
                printf("  Cell2 bounds: (%.2f,%.2f,%.2f) to (%.2f,%.2f,%.2f)\n",
                       cell2->min_bound.x, cell2->min_bound.y, cell2->min_bound.z,
                       cell2->max_bound.x, cell2->max_bound.y, cell2->max_bound.z);
                no_overlaps = false;
            }
        }
    }
    
    bool success = unique_coordinates && no_overlaps;
    printf("Cell Coordinate Uniqueness Test: %s\n", success ? "PASSED" : "FAILED");
    
    return success;
}

// Test to verify each cell generates exactly one mesh per material
bool test_one_mesh_per_cell_per_material() {
    printf("\n=== Testing One Mesh Per Cell Per Material ===\n");
    
    BLASManager blas_manager;
    TLASManager tlas_manager(1000);
    Cluster cluster(1, blas_manager, tlas_manager, 1.0f);
    
    // Add particles with different materials in specific patterns
    // Material 0 particles
    cluster.add_particle({0.5f, 0.5f, 0.5f}, 0.3f, 0);
    cluster.add_particle({1.5f, 0.5f, 0.5f}, 0.3f, 0);
    
    // Material 1 particles  
    cluster.add_particle({0.5f, 1.5f, 0.5f}, 0.3f, 1);
    cluster.add_particle({1.5f, 1.5f, 0.5f}, 0.3f, 1);
    
    // Material 2 particles (mixed with others)
    cluster.add_particle({0.7f, 0.7f, 0.5f}, 0.3f, 2);
    cluster.add_particle({1.3f, 1.3f, 0.5f}, 0.3f, 2);
    
    cluster.rebuild_dirty_cells();
    
    // Get all cells
    Vector3 min_bound = {-1.0f, -1.0f, -1.0f};
    Vector3 max_bound = {3.0f, 3.0f, 3.0f};
    auto cells = cluster.get_cells_in_region(min_bound, max_bound);
    
    printf("Found %zu cells after adding particles with materials 0, 1, 2\n", cells.size());
    
    bool success = true;
    int total_meshes = 0;
    
    for (const auto* cell : cells) {
        if (!cell->has_meshes) {
            continue;
        }
        
        printf("Cell (%.0f,%.0f,%.0f,size=%.1f):\n", 
               cell->coordinates.x, cell->coordinates.y, cell->coordinates.z, cell->actual_size);
        
        // Check that each material has exactly one mesh
        const auto& material_meshes = cell->material_meshes;
        const auto& material_blas = cell->get_material_blas();
        const auto& material_particles = cell->material_particle_indices;
        
        printf("  Has %zu material meshes, %zu BLAS entries, %zu particle groups\n",
               material_meshes.size(), material_blas.size(), material_particles.size());
        
        // Verify mesh count matches particle material count
        if (material_meshes.size() != material_particles.size()) {
            printf("  ERROR: Mesh count (%zu) doesn't match material particle groups (%zu)\n",
                   material_meshes.size(), material_particles.size());
            success = false;
        }
        
        // Verify BLAS count matches mesh count
        if (material_blas.size() != material_meshes.size()) {
            printf("  ERROR: BLAS count (%zu) doesn't match mesh count (%zu)\n",
                   material_blas.size(), material_meshes.size());
            success = false;
        }
        
        // Check each material
        for (const auto& particle_entry : material_particles) {
            uint32_t material_id = particle_entry.first;
            const auto& particle_indices = particle_entry.second;
            
            printf("    Material %u: %zu particles", material_id, particle_indices.size());
            
            // Check if this material has exactly one mesh
            auto mesh_it = material_meshes.find(material_id);
            if (mesh_it == material_meshes.end()) {
                printf(" - ERROR: Missing mesh!\n");
                success = false;
            } else {
                const Mesh& mesh = mesh_it->second;
                printf(" - Mesh: %d vertices, %d triangles", mesh.vertexCount, mesh.triangleCount);
                total_meshes++;
            }
            
            // Check if this material has exactly one BLAS
            auto blas_it = material_blas.find(material_id);
            if (blas_it == material_blas.end()) {
                printf(" - ERROR: Missing BLAS!\n");
                success = false;
            } else {
                printf(" - BLAS: handle %u", blas_it->second);
            }
            
            printf("\n");
        }
    }
    
    printf("Total meshes generated: %d\n", total_meshes);
    printf("One Mesh Per Cell Per Material Test: %s\n", success ? "PASSED" : "FAILED");
    
    return success;
}

// Test to verify particles are assigned to correct cells based on position
bool test_particle_cell_assignment() {
    printf("\n=== Testing Particle Cell Assignment ===\n");
    
    BLASManager blas_manager;
    TLASManager tlas_manager(1000);
    Cluster cluster(2, blas_manager, tlas_manager, 2.0f); // 2.0 unit cell size
    
    // Add particles at known positions
    struct TestParticle {
        Vector3 position;
        Vector3 expected_cell_coords;
        float radius;
        uint32_t material;
    };
    
    std::vector<TestParticle> test_particles = {
        {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, 0.5f, 0},  // Should be in cell (0,0,0)
        {{3.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, 0.5f, 0},  // Should be in cell (1,0,0)
        {{1.0f, 3.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, 0.5f, 0},  // Should be in cell (0,1,0)
        {{3.0f, 3.0f, 3.0f}, {1.0f, 1.0f, 1.0f}, 0.5f, 1},  // Should be in cell (1,1,1)
        {{-1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f}, 0.5f, 1}, // Should be in cell (-1,-1,-1)
    };
    
    printf("Adding %zu test particles at specific positions...\n", test_particles.size());
    
    for (const auto& tp : test_particles) {
        cluster.add_particle(tp.position, tp.radius, tp.material);
        printf("  Particle at (%.1f,%.1f,%.1f) should create cell (%.0f,%.0f,%.0f)\n",
               tp.position.x, tp.position.y, tp.position.z,
               tp.expected_cell_coords.x, tp.expected_cell_coords.y, tp.expected_cell_coords.z);
    }
    
    cluster.rebuild_dirty_cells();
    
    // Get all cells
    Vector3 min_bound = {-3.0f, -3.0f, -3.0f};
    Vector3 max_bound = {5.0f, 5.0f, 5.0f};
    auto cells = cluster.get_cells_in_region(min_bound, max_bound);
    
    printf("Generated %zu cells\n", cells.size());
    
    bool success = true;
    
    // Verify each expected cell exists and has the right particles
    for (const auto& tp : test_particles) {
        bool found_cell = false;
        
        for (const auto* cell : cells) {
            if (vectors_equal(cell->coordinates, tp.expected_cell_coords) && cell->size_power == 0) {
                found_cell = true;
                
                printf("  Found expected cell (%.0f,%.0f,%.0f) with size %.1f\n",
                       cell->coordinates.x, cell->coordinates.y, cell->coordinates.z, cell->actual_size);
                
                // Verify particle is in this cell
                bool particle_in_cell = cell->contains_point(tp.position);
                if (!particle_in_cell) {
                    printf("    ERROR: Particle at (%.1f,%.1f,%.1f) is not contained in its expected cell!\n",
                           tp.position.x, tp.position.y, tp.position.z);
                    printf("    Cell bounds: (%.2f,%.2f,%.2f) to (%.2f,%.2f,%.2f)\n",
                           cell->min_bound.x, cell->min_bound.y, cell->min_bound.z,
                           cell->max_bound.x, cell->max_bound.y, cell->max_bound.z);
                    success = false;
                }
                
                // Verify cell has particles with the correct material
                const auto& material_particles = cell->material_particle_indices;
                auto material_it = material_particles.find(tp.material);
                if (material_it == material_particles.end() || material_it->second.empty()) {
                    printf("    ERROR: Cell doesn't contain particles with material %u!\n", tp.material);
                    success = false;
                }
                
                break;
            }
        }
        
        if (!found_cell) {
            printf("  ERROR: Expected cell (%.0f,%.0f,%.0f) was not created!\n",
                   tp.expected_cell_coords.x, tp.expected_cell_coords.y, tp.expected_cell_coords.z);
            success = false;
        }
    }
    
    printf("Particle Cell Assignment Test: %s\n", success ? "PASSED" : "FAILED");
    
    return success;
}

// Run all tests
int main() {
    printf("=== Cell System Tests ===\n\n");
    
    // Initialize Raylib (required for Vector3 operations)
    InitWindow(1, 1, "Test Window");
    SetWindowState(FLAG_WINDOW_HIDDEN);
    
    bool all_passed = true;
    
    all_passed &= test_cell_coordinate_uniqueness();
    all_passed &= test_one_mesh_per_cell_per_material();  
    all_passed &= test_particle_cell_assignment();
    
    CloseWindow();
    
    printf("\n=== Test Summary ===\n");
    printf("Overall Result: %s\n", all_passed ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    
    return all_passed ? 0 : 1;
}