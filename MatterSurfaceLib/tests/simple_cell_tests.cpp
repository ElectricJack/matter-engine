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

// Test utilities
bool vectors_equal(const Vector3& a, const Vector3& b, float epsilon = 1e-6f) {
    return (fabs(a.x - b.x) < epsilon && 
            fabs(a.y - b.y) < epsilon && 
            fabs(a.z - b.z) < epsilon);
}

// Minimal mock for testing without BLAS manager
class MockBLASManager {
public:
    uint32_t register_triangles(const std::vector<void*>& triangles) {
        return ++last_handle_;
    }
    
private:
    uint32_t last_handle_ = 0;
};

// Minimal mock for testing without TLAS manager
class MockTLASManager {
public:
    void clear() {}
    void load_identity() {}
    void translate(float x, float y, float z) {}
    void draw(uint32_t blas_handle, uint32_t material_id) {}
    void build(MockBLASManager& blas_manager) {}
    int get_instance_count() const { return 0; }
    int get_node_count() const { return 0; }
};

// Test to verify cell coordinate hashing produces unique, non-overlapping cells
bool test_cell_coordinate_uniqueness() {
    printf("=== Testing Cell Coordinate Uniqueness ===\n");
    
    // Create cluster with specific cell size
    float cell_size = 2.0f;
    MockBLASManager blas_manager;
    MockTLASManager tlas_manager;
    Cluster cluster(0, blas_manager, tlas_manager, cell_size);
    
    // Add particles in a grid pattern that should create specific cells
    printf("Adding particles to test cell creation...\n");
    
    // These particles should create distinct, non-overlapping cells
    struct TestCase {
        Vector3 particle_pos;
        Vector3 expected_cell_coords;
    };
    
    std::vector<TestCase> test_cases = {
        {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}},    // Cell (0,0,0)
        {{3.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},    // Cell (1,0,0)
        {{1.0f, 3.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},    // Cell (0,1,0)
        {{1.0f, 1.0f, 3.0f}, {0.0f, 0.0f, 1.0f}},    // Cell (0,0,1)
        {{-1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f}}, // Cell (-1,-1,-1)
    };
    
    for (const auto& test_case : test_cases) {
        cluster.add_particle(test_case.particle_pos, 0.5f, 0);
        printf("  Added particle at (%.1f,%.1f,%.1f), expect cell (%.0f,%.0f,%.0f)\n",
               test_case.particle_pos.x, test_case.particle_pos.y, test_case.particle_pos.z,
               test_case.expected_cell_coords.x, test_case.expected_cell_coords.y, test_case.expected_cell_coords.z);
    }
    
    printf("Total particles: %u\n", cluster.get_particle_count());
    printf("Total cells before rebuild: %u\n", cluster.get_cell_count());
    printf("Dirty cells: %u\n", cluster.get_dirty_cell_count());
    
    // Get all cells
    Vector3 min_bound = {-3.0f, -3.0f, -3.0f};
    Vector3 max_bound = {5.0f, 5.0f, 5.0f};
    auto cells = cluster.get_cells_in_region(min_bound, max_bound);
    
    printf("Found %zu cells in region\n", cells.size());
    
    // Test 1: Check for coordinate uniqueness at the same LOD level
    std::map<std::tuple<float, float, float, int>, int> coordinate_count;
    
    for (const auto* cell : cells) {
        printf("  Cell: coords=(%.0f,%.0f,%.0f), size_power=%d, actual_size=%.1f\n",
               cell->coordinates.x, cell->coordinates.y, cell->coordinates.z, 
               cell->size_power, cell->actual_size);
        printf("    Bounds: (%.2f,%.2f,%.2f) to (%.2f,%.2f,%.2f)\n",
               cell->min_bound.x, cell->min_bound.y, cell->min_bound.z,
               cell->max_bound.x, cell->max_bound.y, cell->max_bound.z);
        
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
    
    // Test 2: Check for overlapping bounds at the same LOD level
    bool no_overlaps = true;
    for (size_t i = 0; i < cells.size(); i++) {
        for (size_t j = i + 1; j < cells.size(); j++) {
            const Cell* cell1 = cells[i];
            const Cell* cell2 = cells[j];
            
            // Only check cells at the same LOD level
            if (cell1->size_power != cell2->size_power) {
                continue;
            }
            
            // Check if bounding boxes overlap
            bool overlaps = !(cell1->max_bound.x <= cell2->min_bound.x || 
                             cell2->max_bound.x <= cell1->min_bound.x ||
                             cell1->max_bound.y <= cell2->min_bound.y || 
                             cell2->max_bound.y <= cell1->min_bound.y ||
                             cell1->max_bound.z <= cell2->min_bound.z || 
                             cell2->max_bound.z <= cell1->min_bound.z);
            
            if (overlaps) {
                printf("ERROR: Cell (%.0f,%.0f,%.0f,size=%.1f) overlaps with Cell (%.0f,%.0f,%.0f,size=%.1f) at same LOD!\n",
                       cell1->coordinates.x, cell1->coordinates.y, cell1->coordinates.z, cell1->actual_size,
                       cell2->coordinates.x, cell2->coordinates.y, cell2->coordinates.z, cell2->actual_size);
                no_overlaps = false;
            }
        }
    }
    
    // Test 3: Verify expected cells were created
    bool expected_cells_created = true;
    for (const auto& test_case : test_cases) {
        bool found = false;
        for (const auto* cell : cells) {
            if (vectors_equal(cell->coordinates, test_case.expected_cell_coords) && cell->size_power == 0) {
                found = true;
                
                // Verify the particle is actually contained in this cell
                if (!cell->contains_point(test_case.particle_pos)) {
                    printf("ERROR: Expected cell (%.0f,%.0f,%.0f) doesn't contain particle at (%.1f,%.1f,%.1f)\n",
                           test_case.expected_cell_coords.x, test_case.expected_cell_coords.y, test_case.expected_cell_coords.z,
                           test_case.particle_pos.x, test_case.particle_pos.y, test_case.particle_pos.z);
                    expected_cells_created = false;
                }
                break;
            }
        }
        
        if (!found) {
            printf("ERROR: Expected cell (%.0f,%.0f,%.0f) was not created for particle at (%.1f,%.1f,%.1f)\n",
                   test_case.expected_cell_coords.x, test_case.expected_cell_coords.y, test_case.expected_cell_coords.z,
                   test_case.particle_pos.x, test_case.particle_pos.y, test_case.particle_pos.z);
            expected_cells_created = false;
        }
    }
    
    bool success = unique_coordinates && no_overlaps && expected_cells_created;
    printf("Cell Coordinate Uniqueness Test: %s\n", success ? "PASSED" : "FAILED");
    
    return success;
}

// Test to check if multiple LOD levels create overlapping cells
bool test_multiple_lod_overlap() {
    printf("\n=== Testing Multiple LOD Overlap Issue ===\n");
    
    MockBLASManager blas_manager;
    MockTLASManager tlas_manager;
    Cluster cluster(1, blas_manager, tlas_manager, 1.0f);
    
    // Add a single particle that will trigger multiple LOD levels
    Vector3 particle_pos = {1.5f, 1.5f, 1.5f};
    cluster.add_particle(particle_pos, 1.0f, 0); // Large radius to trigger multiple LOD levels
    
    printf("Added particle at (%.1f,%.1f,%.1f) with radius 1.0\n", 
           particle_pos.x, particle_pos.y, particle_pos.z);
    
    // Get all cells created
    Vector3 min_bound = {-1.0f, -1.0f, -1.0f};
    Vector3 max_bound = {4.0f, 4.0f, 4.0f};
    auto cells = cluster.get_cells_in_region(min_bound, max_bound);
    
    printf("Created %zu cells for single particle\n", cells.size());
    
    // Group cells by LOD level
    std::map<int, std::vector<const Cell*>> cells_by_lod;
    for (const auto* cell : cells) {
        cells_by_lod[cell->size_power].push_back(cell);
        printf("  LOD %d: Cell (%.0f,%.0f,%.0f), size=%.1f\n", 
               cell->size_power, cell->coordinates.x, cell->coordinates.y, cell->coordinates.z, cell->actual_size);
    }
    
    printf("Cells created at %zu different LOD levels\n", cells_by_lod.size());
    
    // Check for overlaps between different LOD levels covering the same space
    bool has_overlaps = false;
    for (const auto& lod1_entry : cells_by_lod) {
        for (const auto& lod2_entry : cells_by_lod) {
            if (lod1_entry.first >= lod2_entry.first) continue; // Only check lower vs higher LOD
            
            for (const Cell* cell1 : lod1_entry.second) {
                for (const Cell* cell2 : lod2_entry.second) {
                    // Check if smaller cell is completely inside larger cell
                    bool completely_inside = (
                        cell2->min_bound.x >= cell1->min_bound.x && cell2->max_bound.x <= cell1->max_bound.x &&
                        cell2->min_bound.y >= cell1->min_bound.y && cell2->max_bound.y <= cell1->max_bound.y &&
                        cell2->min_bound.z >= cell1->min_bound.z && cell2->max_bound.z <= cell1->max_bound.z
                    );
                    
                    if (completely_inside) {
                        printf("OVERLAP: LOD%d cell (%.0f,%.0f,%.0f,size=%.1f) contains LOD%d cell (%.0f,%.0f,%.0f,size=%.1f)\n",
                               cell1->size_power, cell1->coordinates.x, cell1->coordinates.y, cell1->coordinates.z, cell1->actual_size,
                               cell2->size_power, cell2->coordinates.x, cell2->coordinates.y, cell2->coordinates.z, cell2->actual_size);
                        has_overlaps = true;
                    }
                }
            }
        }
    }
    
    // This test EXPECTS overlaps to demonstrate the current problem
    printf("Multiple LOD Overlap Test: %s (EXPECTED: multiple LOD levels should NOT create overlapping cells)\n", 
           has_overlaps ? "FOUND OVERLAPS (Problem Confirmed)" : "NO OVERLAPS");
    
    return !has_overlaps; // Return true if NO overlaps (which is what we want)
}

// Run all tests
int main() {
    printf("=== Simple Cell System Tests ===\n\n");
    
    // Initialize Raylib (required for Vector3 operations)
    InitWindow(1, 1, "Test Window");
    SetWindowState(FLAG_WINDOW_HIDDEN);
    
    bool all_passed = true;
    
    all_passed &= test_cell_coordinate_uniqueness();
    all_passed &= test_multiple_lod_overlap();
    
    CloseWindow();
    
    printf("\n=== Test Summary ===\n");
    printf("Overall Result: %s\n", all_passed ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    
    if (!all_passed) {
        printf("\nIdentified Issues:\n");
        printf("1. Multiple LOD levels are creating overlapping cells for the same particles\n");
        printf("2. This causes multiple surface meshes to be generated for the same space\n");
        printf("3. The fix should ensure only ONE LOD level is used per particle/region\n");
    }
    
    return all_passed ? 0 : 1;
}