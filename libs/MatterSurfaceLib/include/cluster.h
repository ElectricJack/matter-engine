#ifndef CLUSTER_H
#define CLUSTER_H

#include "raylib.h"
#include "particle.h"
#include "vertex_ao.h"  // AoGrid, AoParams, Occupancy
#include <vector>
#include <cstdint>
#include <memory>
#include <unordered_set>

// Forward declarations
struct Cell;
struct SpatialHash;
struct SurfaceScratch;
class BLASManager;
class TLASManager;
class CellVisitor;
class CellRenderVisitor;
class MeshWorkerPool;

// Static particle structure for matter representation
struct StaticParticle {
    Vector3 position;       // Position in local cluster space
    float radius;          // Particle radius
    uint32_t materialId;   // Material identifier
    Vector4 tint;          // RGBA tint; a = blend strength. (1,1,1,0) = no tint.
    float detail_size;     // tier-0 spacing / 2^tier; 0 => fall back to tier 0

    StaticParticle(const Vector3& pos = {0,0,0}, float r = 1.0f, uint32_t mat = 0,
                   const Vector4& t = {1.0f, 1.0f, 1.0f, 0.0f}, float ds = 0.0f)
        : position(pos), radius(r), materialId(mat), tint(t), detail_size(ds) {}
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
    uint32_t add_particle(const Vector3& local_position, float radius, uint32_t material_id, const Vector4& tint);
    uint32_t add_particle(const Vector3& local_position, float radius, uint32_t material_id,
                          const Vector4& tint, float detail_size);
    bool remove_particle(uint32_t particle_id);
    bool update_particle_position(uint32_t particle_id, const Vector3& new_local_position);
    // Drop all additive particles (cells/BLAS are reclaimed by the next
    // force_rebuild_all_cells). Used to re-emit the scene with new parameters.
    void clear_particles();
    
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
    
    // Cell sizing
    void set_smallest_cell_size(float size) { smallest_cell_size_ = size; }
    float get_smallest_cell_size() const { return smallest_cell_size_; }
    
    // Single-resolution rebuild of every cell (used after a full scene change).
    void force_rebuild_all_cells();

    // Skip-meshing: cells whose packed integer coordinate is in this set are
    // created/tracked but never meshed (they hold no mesh, register no BLAS).
    // Coordinates use the same floor(local/cell_size) basis as get_cell_coordinates.
    void set_no_mesh_cells(const std::vector<Vector3>& coords);
    void clear_no_mesh_cells() { no_mesh_cells_.clear(); }

    // Subtractive carve particles (smooth-CSG). Distributed per-cell by the same
    // intersects_sphere halo as additive particles so the carved field stays
    // continuous across cell boundaries.
    void set_carve_particles(const std::vector<Particle>& carve) { carve_particles_ = carve; }
    void clear_carve_particles() { carve_particles_.clear(); }

    // Mesh simplification (uniform across cells; per-cell distance LOD can drive
    // this later without changing the simplifier).
    void set_simplification_ratio(float ratio) {
        if (ratio < 0.05f) ratio = 0.05f;
        if (ratio > 1.0f)  ratio = 1.0f;
        simplification_ratio_ = ratio;
    }
    float get_simplification_ratio() const { return simplification_ratio_; }

    // Lattice tier-0 spacing S; cells use it to recover the finest tier present
    // from each particle's detail_size when choosing mesh resolution.
    void set_base_detail_size(float s) { base_detail_size_ = s; }
    float get_base_detail_size() const { return base_detail_size_; }
    // Upper bound on per-cell divisionPow (2^pow grid).
    void set_max_division_pow(int p) { max_division_pow_ = p; }
    int get_max_division_pow() const { return max_division_pow_; }

    // Number of CPU mesh worker threads. Resizing is only applied between
    // rebuilds (call from the UI before the next rebuild_dirty_cells).
    void set_mesh_worker_count(int n);
    int  get_mesh_worker_count() const;

    // Enables post-meshing per-vertex AO baking against `occ` (borrowed, must
    // outlive the cluster). Pass occ=nullptr to disable. `grid` maps cluster-local
    // positions to occupancy slots; see AoGrid.
    void set_ao_baker(const Occupancy* occ, AoGrid grid, AoParams params);

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
    float simplification_ratio_ = 1.0f; // 1.0 = no simplification
    float base_detail_size_ = 0.0f;   // lattice tier-0 spacing S (0 => disabled)
    int   max_division_pow_ = 6;      // resolution ceiling (64^3)
    SpatialHash* cell_spatial_hash_;
    // Persistent worker pool for per-cell CPU meshing. Owns one SurfaceScratch
    // per worker thread (replaces the former single per-cluster scratch). Sized
    // to hardware concurrency at construction; resized between rebuilds via the
    // ImGui worker slider.
    std::unique_ptr<MeshWorkerPool> mesh_pool_;
    std::vector<std::unique_ptr<Cell>> cells_;
    std::unordered_set<uint64_t> no_mesh_cells_;  // packed integer cell coords
    std::vector<Particle> carve_particles_;

    // Post-meshing per-vertex AO bake config (disabled while ao_occ_ is null).
    const Occupancy* ao_occ_ = nullptr;
    AoGrid    ao_grid_{};
    AoParams  ao_params_{};

    // Helper methods
    Vector3 get_cell_coordinates(const Vector3& local_position) const;
    Cell* find_or_create_cell(const Vector3& cell_coords);
    void clear_all_cells();

    // Finest detail_size across all particles (seeded with base_detail_size_).
    // Drives a single uniform mesh resolution for every meshed cell.
    float compute_finest_detail() const;
};

#endif // CLUSTER_H