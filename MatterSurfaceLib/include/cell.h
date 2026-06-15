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

// SurfaceLib's C-linkage surface API plus the Particle/Bounds plain-old-data
// types. surface.h now wraps its prototypes in its own `extern "C"` guard, so
// the symbols stay unmangled to match the C-compiled surface.c. Including it
// here (rather than re-mirroring the declarations) keeps a single source of
// truth that cell.cpp and the headless tests share.
#include "surface.h"

// Builds the transparency-gated foreign clip-particle set for meshing the merge
// group `group_id` in a cell. For every OTHER non-empty bucket, the carve is
// relevant iff this group is transparent OR the foreign group is transparent
// (opaque<->opaque pairs are skipped: harmless hidden overlap). Relevant foreign
// particles are added with the SAME LOD taper/cull as the group's own particles
// (skip radius < cull_radius; lift r_eff = max(radius, vis_radius)). GL-free, so
// both generate_mesh_for_group and the headless tests call the same code.
std::vector<Particle> build_clip_particles(
    uint32_t group_id,
    const std::map<uint32_t, std::vector<uint32_t>>& buckets,
    const std::vector<StaticParticle>& cluster_particles,
    bool group_transparent,
    float cull_radius, float vis_radius);

// Pick a marching-cubes divisionPow from the finest detail present in a cell.
// detail_size_min: smallest StaticParticle.detail_size among the cell's particles
// (<= 0 or >= base_detail => tier 0). base_detail: lattice tier-0 spacing S.
// tier = round(log2(base_detail / detail_size_min)); returns
// clamp(base_pow + max(0,tier), base_pow, max_pow). GL-free / pure.
int choose_division_pow(float detail_size_min, float base_detail, int base_pow, int max_pow);

struct Cell {
    // Cell identification and spatial properties
    Vector3 coordinates;        // Integer coordinates in cluster space (stored as floats for convenience)
    int size_power;            // Cell size = smallest_cell_size * (2^size_power)
    float actual_size;         // Computed actual size of the cell
    Vector3 center;            // Center position in cluster local space
    Vector3 min_bound;         // Minimum bound in cluster local space
    Vector3 max_bound;         // Maximum bound in cluster local space
    
    // Merge-group-based mesh data. The map key is a merge-group id (not a shading
    // material): shades of the same material merge into one group/mesh, while
    // distinct material types stay separate.
    std::map<uint32_t, Mesh> material_meshes;  // One mesh per merge group in this cell
    std::map<uint32_t, BLASHandle> material_blas; // One BLAS per merge-group mesh
    bool has_meshes;           // Whether any meshes have been generated
    bool is_dirty;             // Whether cell needs mesh rebuilding
    uint32_t mesh_version;     // Version number for cache invalidation

    // Particle references grouped by merge group (map key is a merge-group id)
    std::map<uint32_t, std::vector<uint32_t>> material_particle_indices;
    
    // Construction and lifecycle
    Cell(const Vector3& coords, int size_pow, float smallest_cell_size);
    ~Cell();
    
    // Mesh management
    void rebuild_meshes(const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager, float simplification_ratio = 1.0f);
    // Drops this cell's meshes. When blas_manager is provided, the cell's BLAS
    // references are released so stale entries don't accumulate on the GPU.
    void clear_meshes(BLASManager* blas_manager = nullptr);
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
    void generate_mesh_for_group(uint32_t group_id, const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager, float simplification_ratio);
};

#endif // CELL_H