#ifndef CELL_H
#define CELL_H

#include "raylib.h"
#include <vector>
#include <cstdint>
#include <map>

// Forward declarations
struct StaticParticle;
class BLASManager;
class CellVisitor;
class CellRenderVisitor;
typedef uint32_t BLASHandle;

struct Cell {
    // Cell identification and spatial properties
    Vector3 coordinates;        // Integer coordinates in cluster space (stored as floats for convenience)
    int size_power;            // Cell size = smallest_cell_size * (2^size_power)
    float actual_size;         // Computed actual size of the cell
    Vector3 center;            // Center position in cluster local space
    Vector3 min_bound;         // Minimum bound in cluster local space
    Vector3 max_bound;         // Maximum bound in cluster local space
    
    // Material-based mesh data
    std::map<uint32_t, Mesh> material_meshes;  // One mesh per material in this cell
    std::map<uint32_t, BLASHandle> material_blas; // One BLAS per material mesh
    bool has_meshes;           // Whether any meshes have been generated
    bool is_dirty;             // Whether cell needs mesh rebuilding
    uint32_t mesh_version;     // Version number for cache invalidation
    
    // Particle references grouped by material
    std::map<uint32_t, std::vector<uint32_t>> material_particle_indices;
    
    // Construction and lifecycle
    Cell(const Vector3& coords, int size_pow, float smallest_cell_size);
    ~Cell();
    
    // Mesh management
    void rebuild_meshes(const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager);
    void clear_meshes();
    bool contains_point(const Vector3& local_point) const;
    bool intersects_sphere(const Vector3& center, float radius) const;
    
    // Particle management
    void add_particle_index(uint32_t particle_index, uint32_t material_id);
    void remove_particle_index(uint32_t particle_index, uint32_t material_id);
    void clear_particle_indices();
    
    // Visitor pattern support
    void accept(CellVisitor& visitor) const;
    void accept_transformed(CellRenderVisitor& visitor, const Matrix& transform) const;
    
    // BLAS access
    const std::map<uint32_t, BLASHandle>& get_material_blas() const { return material_blas; }
    
    // Utilities
    float get_diagonal_length() const;
    Vector3 get_size() const { return Vector3{actual_size, actual_size, actual_size}; }
    
private:
    void calculate_bounds(float smallest_cell_size);
    void generate_mesh_for_material(uint32_t material_id, const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager);
};

#endif // CELL_H