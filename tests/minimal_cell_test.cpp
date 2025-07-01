#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <map>

extern "C" {
    #include "raylib.h"
}

// Test the core logic directly without full cluster implementation

// Test cell coordinate calculation
bool test_cell_coordinate_calculation() {
    printf("=== Testing Cell Coordinate Calculation Logic ===\n");
    
    float smallest_cell_size = 2.0f;
    
    struct TestCase {
        Vector3 position;
        int lod_level;
        Vector3 expected_coords;
    };
    
    std::vector<TestCase> test_cases = {
        // LOD 0 (2.0 unit cells)
        {{1.0f, 1.0f, 1.0f}, 0, {0.0f, 0.0f, 0.0f}},
        {{3.0f, 1.0f, 1.0f}, 0, {1.0f, 0.0f, 0.0f}},
        {{-1.0f, -1.0f, -1.0f}, 0, {-1.0f, -1.0f, -1.0f}},
        
        // LOD 1 (4.0 unit cells)
        {{1.0f, 1.0f, 1.0f}, 1, {0.0f, 0.0f, 0.0f}},
        {{5.0f, 1.0f, 1.0f}, 1, {1.0f, 0.0f, 0.0f}},
        {{-1.0f, -1.0f, -1.0f}, 1, {-1.0f, -1.0f, -1.0f}},
        
        // LOD 2 (8.0 unit cells)
        {{1.0f, 1.0f, 1.0f}, 2, {0.0f, 0.0f, 0.0f}},
        {{9.0f, 1.0f, 1.0f}, 2, {1.0f, 0.0f, 0.0f}},
    };
    
    bool all_passed = true;
    
    for (const auto& test_case : test_cases) {
        float cell_size = smallest_cell_size * (1 << test_case.lod_level);
        Vector3 calculated_coords = {
            floorf(test_case.position.x / cell_size),
            floorf(test_case.position.y / cell_size),
            floorf(test_case.position.z / cell_size)
        };
        
        bool coords_match = (
            fabs(calculated_coords.x - test_case.expected_coords.x) < 1e-6f &&
            fabs(calculated_coords.y - test_case.expected_coords.y) < 1e-6f &&
            fabs(calculated_coords.z - test_case.expected_coords.z) < 1e-6f
        );
        
        printf("  Position (%.1f,%.1f,%.1f) LOD%d (cell_size=%.1f): coords=(%.0f,%.0f,%.0f), expected=(%.0f,%.0f,%.0f) %s\n",
               test_case.position.x, test_case.position.y, test_case.position.z,
               test_case.lod_level, cell_size,
               calculated_coords.x, calculated_coords.y, calculated_coords.z,
               test_case.expected_coords.x, test_case.expected_coords.y, test_case.expected_coords.z,
               coords_match ? "✓" : "✗");
        
        if (!coords_match) {
            all_passed = false;
        }
    }
    
    printf("Cell Coordinate Calculation Test: %s\n", all_passed ? "PASSED" : "FAILED");
    return all_passed;
}

// Test the multiple LOD problem - demonstrate that current logic creates overlapping cells
bool test_multiple_lod_problem() {
    printf("\n=== Testing Multiple LOD Problem ===\n");
    
    float smallest_cell_size = 2.0f;
    Vector3 particle_position = {1.0f, 1.0f, 1.0f};
    float particle_radius = 1.0f;
    float influence_radius = particle_radius * 2.0f; // This is what mark_cells_dirty_around_particle uses
    
    printf("Particle at (%.1f,%.1f,%.1f) with radius %.1f (influence_radius=%.1f)\n",
           particle_position.x, particle_position.y, particle_position.z, particle_radius, influence_radius);
    
    // Simulate what mark_cells_dirty_around_particle does
    struct CellInfo {
        Vector3 coords;
        int lod_level;
        float cell_size;
        Vector3 min_bound;
        Vector3 max_bound;
    };
    
    std::vector<CellInfo> created_cells;
    
    // Check multiple LOD levels (this is the current problematic behavior)
    for (int lod = 0; lod <= 4; ++lod) {
        float cell_size = smallest_cell_size * (1 << lod);
        
        // Calculate cell coordinate range
        Vector3 min_cell = {
            floorf((particle_position.x - influence_radius) / cell_size),
            floorf((particle_position.y - influence_radius) / cell_size),
            floorf((particle_position.z - influence_radius) / cell_size)
        };
        Vector3 max_cell = {
            floorf((particle_position.x + influence_radius) / cell_size),
            floorf((particle_position.y + influence_radius) / cell_size),
            floorf((particle_position.z + influence_radius) / cell_size)
        };
        
        printf("LOD %d (cell_size=%.1f): range (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f)\n",
               lod, cell_size, min_cell.x, min_cell.y, min_cell.z, max_cell.x, max_cell.y, max_cell.z);
        
        // Create cells in this range
        for (int x = (int)min_cell.x; x <= (int)max_cell.x; ++x) {
            for (int y = (int)min_cell.y; y <= (int)max_cell.y; ++y) {
                for (int z = (int)min_cell.z; z <= (int)max_cell.z; ++z) {
                    CellInfo cell;
                    cell.coords = {(float)x, (float)y, (float)z};
                    cell.lod_level = lod;
                    cell.cell_size = cell_size;
                    
                    // Calculate bounds
                    Vector3 center = {
                        (cell.coords.x + 0.5f) * cell_size,
                        (cell.coords.y + 0.5f) * cell_size,
                        (cell.coords.z + 0.5f) * cell_size
                    };
                    Vector3 half_size = {cell_size * 0.5f, cell_size * 0.5f, cell_size * 0.5f};
                    cell.min_bound = {center.x - half_size.x, center.y - half_size.y, center.z - half_size.z};
                    cell.max_bound = {center.x + half_size.x, center.y + half_size.y, center.z + half_size.z};
                    
                    created_cells.push_back(cell);
                    
                    printf("  Created cell LOD%d (%.0f,%.0f,%.0f) size=%.1f bounds=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n",
                           lod, cell.coords.x, cell.coords.y, cell.coords.z, cell_size,
                           cell.min_bound.x, cell.min_bound.y, cell.min_bound.z,
                           cell.max_bound.x, cell.max_bound.y, cell.max_bound.z);
                }
            }
        }
    }
    
    printf("Total cells created: %zu\n", created_cells.size());
    
    // Check for overlaps
    int overlap_count = 0;
    for (size_t i = 0; i < created_cells.size(); i++) {
        for (size_t j = i + 1; j < created_cells.size(); j++) {
            const CellInfo& cell1 = created_cells[i];
            const CellInfo& cell2 = created_cells[j];
            
            // Check if bounds overlap
            bool overlaps = !(cell1.max_bound.x <= cell2.min_bound.x || 
                             cell2.max_bound.x <= cell1.min_bound.x ||
                             cell1.max_bound.y <= cell2.min_bound.y || 
                             cell2.max_bound.y <= cell1.min_bound.y ||
                             cell1.max_bound.z <= cell2.min_bound.z || 
                             cell2.max_bound.z <= cell1.min_bound.z);
            
            if (overlaps) {
                overlap_count++;
                printf("OVERLAP #%d: LOD%d cell (%.0f,%.0f,%.0f) size=%.1f overlaps with LOD%d cell (%.0f,%.0f,%.0f) size=%.1f\n",
                       overlap_count,
                       cell1.lod_level, cell1.coords.x, cell1.coords.y, cell1.coords.z, cell1.cell_size,
                       cell2.lod_level, cell2.coords.x, cell2.coords.y, cell2.coords.z, cell2.cell_size);
            }
        }
    }
    
    printf("Total overlapping cell pairs: %d\n", overlap_count);
    printf("Multiple LOD Problem Test: %s (Current system creates %d overlapping cells)\n", 
           overlap_count > 0 ? "PROBLEM CONFIRMED" : "NO PROBLEM", overlap_count);
    
    // Return false if overlaps found (indicates the problem exists)
    return overlap_count == 0;
}

// Test what the corrected system should do
bool test_corrected_single_lod_approach() {
    printf("\n=== Testing Corrected Single LOD Approach ===\n");
    
    float smallest_cell_size = 2.0f;
    Vector3 particle_position = {1.0f, 1.0f, 1.0f};
    float particle_radius = 1.0f;
    
    printf("Particle at (%.1f,%.1f,%.1f) with radius %.1f\n",
           particle_position.x, particle_position.y, particle_position.z, particle_radius);
    
    // Calculate appropriate LOD level based on particle size
    // Use the smallest cell size that can contain the particle
    int appropriate_lod = 0;
    float cell_size = smallest_cell_size;
    
    // Find the smallest LOD where cell_size >= particle_radius * 2
    while (cell_size < particle_radius * 2.0f && appropriate_lod < 4) {
        appropriate_lod++;
        cell_size = smallest_cell_size * (1 << appropriate_lod);
    }
    
    printf("Appropriate LOD level: %d (cell_size=%.1f)\n", appropriate_lod, cell_size);
    
    // Calculate which single cell this particle should belong to
    Vector3 cell_coords = {
        floorf(particle_position.x / cell_size),
        floorf(particle_position.y / cell_size),
        floorf(particle_position.z / cell_size)
    };
    
    printf("Particle assigned to single cell: LOD%d (%.0f,%.0f,%.0f)\n",
           appropriate_lod, cell_coords.x, cell_coords.y, cell_coords.z);
    
    // Calculate cell bounds
    Vector3 center = {
        (cell_coords.x + 0.5f) * cell_size,
        (cell_coords.y + 0.5f) * cell_size,
        (cell_coords.z + 0.5f) * cell_size
    };
    Vector3 half_size = {cell_size * 0.5f, cell_size * 0.5f, cell_size * 0.5f};
    Vector3 min_bound = {center.x - half_size.x, center.y - half_size.y, center.z - half_size.z};
    Vector3 max_bound = {center.x + half_size.x, center.y + half_size.y, center.z + half_size.z};
    
    printf("Cell bounds: (%.1f,%.1f,%.1f) to (%.1f,%.1f,%.1f)\n",
           min_bound.x, min_bound.y, min_bound.z, max_bound.x, max_bound.y, max_bound.z);
    
    // Verify particle is contained
    bool contained = (particle_position.x >= min_bound.x && particle_position.x <= max_bound.x &&
                     particle_position.y >= min_bound.y && particle_position.y <= max_bound.y &&
                     particle_position.z >= min_bound.z && particle_position.z <= max_bound.z);
    
    printf("Particle contained in cell: %s\n", contained ? "✓" : "✗");
    printf("Corrected Single LOD Approach Test: %s\n", contained ? "PASSED" : "FAILED");
    
    return contained;
}

int main() {
    printf("=== Minimal Cell System Analysis ===\n\n");
    
    // Initialize Raylib minimally
    InitWindow(1, 1, "Test Window");
    SetWindowState(FLAG_WINDOW_HIDDEN);
    
    bool all_passed = true;
    
    all_passed &= test_cell_coordinate_calculation();
    all_passed &= test_multiple_lod_problem();
    all_passed &= test_corrected_single_lod_approach();
    
    CloseWindow();
    
    printf("\n=== Analysis Summary ===\n");
    printf("Issues Identified:\n");
    printf("1. ✓ Current system creates cells at multiple LOD levels for each particle\n");
    printf("2. ✓ This creates overlapping cells that cover the same space\n");
    printf("3. ✓ Each overlapping cell generates its own surface mesh\n");
    printf("4. ✓ Result: Multiple overlapping meshes for the same matter\n");
    printf("\nProposed Fix:\n");
    printf("1. Choose appropriate single LOD level per particle based on particle size\n");
    printf("2. Only create cells at that specific LOD level\n");
    printf("3. Ensure each particle belongs to exactly one cell\n");
    printf("4. Result: One mesh per spatial region, no overlaps\n");
    
    return 0;
}