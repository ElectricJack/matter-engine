#ifndef CLUSTER_H
#define CLUSTER_H

#include "raylib.h"
#include <vector>
#include <cstdint>
#include <memory>

// Forward declarations
struct Cell;
struct SpatialHash;
class BLASManager;
class TLASManager;
class CellVisitor;
class CellRenderVisitor;

// Static particle structure for matter representation
struct StaticParticle {
    Vector3 position;       // Position in local cluster space
    float radius;          // Particle radius
    uint32_t materialId;   // Material identifier
    
    StaticParticle(const Vector3& pos = {0,0,0}, float r = 1.0f, uint32_t mat = 0)
        : position(pos), radius(r), materialId(mat) {}
};

class Cluster {
public:
    Cluster(uint32_t cluster_id, BLASManager& blas_manager, TLASManager& tlas_manager, float smallest_cell_size = 1.0f);
    ~Cluster();
    
    // Cluster management
    uint32_t get_id() const { return cluster_id_; }
    
    // Transform operations (position + rotation, no scale)
    Vector3 get_position() const { return position_; }
    Quaternion get_rotation() const { return rotation_; }
    void set_position(const Vector3& pos) { position_ = pos; }
    void set_rotation(const Quaternion& rot) { rotation_ = rot; }
    
    // Transform particles between local and world space
    Vector3 local_to_world(const Vector3& local_pos) const;
    Vector3 world_to_local(const Vector3& world_pos) const;
    
    // Particle management
    uint32_t add_particle(const Vector3& local_position, float radius = 1.0f, uint32_t material_id = 0);
    bool remove_particle(uint32_t particle_id);
    bool update_particle_position(uint32_t particle_id, const Vector3& new_local_position);
    
    // Get particles in local space
    const std::vector<StaticParticle>& get_particles() const { return particles_; }
    uint32_t get_particle_count() const { return static_cast<uint32_t>(particles_.size()); }
    
    // Cell management
    void mark_cells_dirty_around_particle(const Vector3& local_position, float radius);
    void rebuild_dirty_cells();
    std::vector<Cell*> get_cells_in_region(const Vector3& min_bound, const Vector3& max_bound);
    
    // Visitor pattern support
    void accept(CellVisitor& visitor) const;
    void visit_cells(CellRenderVisitor& visitor) const;
    void visit_all_cells(CellVisitor& visitor) const;  // Visit all cells regardless of mesh status
    
    // TLAS integration
    void add_to_tlas() const;
    
    // LOD configuration
    void set_smallest_cell_size(float size) { smallest_cell_size_ = size; }
    float get_smallest_cell_size() const { return smallest_cell_size_; }
    
    // LOD level management
    void set_lod_level(int lod_level, bool clear_blas = false);
    int get_lod_level() const { return current_lod_level_; }
    float get_current_cell_size() const { return smallest_cell_size_ * (1 << current_lod_level_); }
    void force_rebuild_all_cells();
    
    // Statistics
    uint32_t get_cell_count() const;
    uint32_t get_dirty_cell_count() const;

private:
    // Cluster identification and transform
    uint32_t cluster_id_;
    Vector3 position_;          // World position
    Quaternion rotation_;       // World rotation
    
    // Manager references (set at construction time)
    BLASManager& blas_manager_;
    TLASManager& tlas_manager_;
    
    // Particle storage
    std::vector<StaticParticle> particles_;
    uint32_t next_particle_id_;
    
    // Cell management
    float smallest_cell_size_;
    int current_lod_level_;           // Currently active LOD level (0 = finest detail)
    SpatialHash* cell_spatial_hash_;
    std::vector<std::unique_ptr<Cell>> cells_;
    
    // Helper methods
    Vector3 get_cell_coordinates(const Vector3& local_position) const;
    Cell* find_or_create_cell(const Vector3& cell_coords);
    void update_cell_meshes(Cell* cell);
    void clear_all_cells();
};

#endif // CLUSTER_H