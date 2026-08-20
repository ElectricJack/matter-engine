#ifndef CLUSTER_H
#define CLUSTER_H

// ---------------------------------------------------------------------------
// libs/MatterSurfaceLib/include/cluster.h
// ---------------------------------------------------------------------------
// A Cluster is one rigidly-transformed body of matter: a flat list of
// StaticParticles in local space, subdivided into Cells (cell.h) that each
// mesh their own share of the implicit surface.
//
// Responsibilities
//   - Particle storage and ids (add_particle overloads).
//   - Cell creation and lookup, through a SpatialHash keyed on cell
//     coordinates; a particle is distributed into every cell its radius
//     touches, so neighbouring cells overlap and the field stays continuous.
//   - Driving rebuilds: mark_cells_dirty_around_particle() then
//     rebuild_dirty_cells(), which fans the CPU half of meshing out over the
//     MeshWorkerPool and commits the results.
//   - Meshing policy shared by all its cells: simplification ratio, lattice
//     tier-0 spacing, division-pow ceiling, carve particles, AO bake config.
//
// Ownership and lifetime
//   Constructed with references to a BLASManager and a TLASManager, both of
//   which must outlive the Cluster -- cells register BLAS entries with the
//   former and add_to_tlas() writes instances into the latter. The Cluster
//   owns its Cells, its SpatialHash and its MeshWorkerPool.
//
// Space and units
//   Particle positions, cell bounds and everything the meshers see are
//   CLUSTER-LOCAL. `position_` / `rotation_` place the cluster in the world;
//   local_to_world() is the only conversion. There is no scale.
//
// Threading
//   The Cluster object itself is single-threaded: only the per-cell CPU mesh
//   build is parallel, and that happens inside rebuild_dirty_cells() on the
//   pool's workers against per-worker SurfaceScratch. Do not call other
//   Cluster methods concurrently with a rebuild.
//
// Gotchas
//   - Committing a cell's mesh needs a graphics context, so MatterEngine3's
//     headless bake does not drive Cluster at all; script_host.cpp includes
//     this header only for StaticParticle. Treat Cluster as the interactive /
//     prototype path.
//   - add_to_tlas() is const but mutates the TLASManager, and it currently
//     applies only the cluster's translation (see the TODO in cluster.cpp) --
//     rotation_ is not baked into the instance transform.
//   - `no_mesh_cells_` is a memo of cells known to produce no geometry, keyed
//     by packed integer cell coordinates; clear it when the field changes
//     underneath it.
// ---------------------------------------------------------------------------

// Phase 4 (Step 4) of docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md:
// this header used to include raylib.h for Vector3/Vector4/Quaternion. It is
// C++-only (no C consumer), so it uses matter_math.h's mm::Vec3/mm::Vec4/
// mm::Quat instead.
#include "matter_math.h"
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
//
// The authored unit of matter: a sphere in cluster-local space carrying the
// material and per-instance tint that the mesher tags onto the triangles it
// produces. "Static" means it does not move once placed -- these are geometry
// inputs, not simulated particles.
//
// POD, copied by value into the Cluster's `particles_` vector; cells refer to
// entries by index, so the vector must not be reallocated while a mesh build
// is reading it. MatterEngine3 consumes this type directly (script_host.cpp)
// even though it does not drive Cluster.
struct StaticParticle {
    mm::Vec3 position;      // Position in local cluster space
    float radius;          // Particle radius
    uint32_t materialId;   // Material identifier
    mm::Vec4 tint;          // RGBA tint; a = blend strength. (1,1,1,0) = no tint.
    float detail_size;     // tier-0 spacing / 2^tier; 0 => fall back to tier 0

    StaticParticle(const mm::Vec3& pos = {0,0,0}, float r = 1.0f, uint32_t mat = 0,
                   const mm::Vec4& t = {1.0f, 1.0f, 1.0f, 0.0f}, float ds = 0.0f)
        : position(pos), radius(r), materialId(mat), tint(t), detail_size(ds) {}
};

class Cluster {
public:
    Cluster(uint32_t cluster_id, BLASManager& blas_manager, TLASManager& tlas_manager, float smallest_cell_size = 1.0f);
    ~Cluster();
    
    // Cluster management
    uint32_t get_id() const { return cluster_id_; }
    
    // Transform operations (position + rotation, no scale)
    mm::Vec3 get_position() const { return position_; }
    mm::Quat get_rotation() const { return rotation_; }
    void set_position(const mm::Vec3& pos) { position_ = pos; }
    void set_rotation(const mm::Quat& rot) { rotation_ = rot; }

    // Transform particles between local and world space
    // Applies rotation_ then position_. There is no scale and no inverse
    // helper -- everything else in this class (particles, cell bounds, meshing)
    // stays in cluster-local space.
    mm::Vec3 local_to_world(const mm::Vec3& local_pos) const;

    // Particle management
    uint32_t add_particle(const mm::Vec3& local_position, float radius = 1.0f, uint32_t material_id = 0);
    uint32_t add_particle(const mm::Vec3& local_position, float radius, uint32_t material_id, const mm::Vec4& tint);
    uint32_t add_particle(const mm::Vec3& local_position, float radius, uint32_t material_id,
                          const mm::Vec4& tint, float detail_size);
    
    // Get particles in local space
    const std::vector<StaticParticle>& get_particles() const { return particles_; }
    uint32_t get_particle_count() const { return static_cast<uint32_t>(particles_.size()); }
    
    // Cell management
    // mark_cells_dirty_around_particle(): flags every cell the sphere touches,
    // creating cells as needed. Call it for each particle whose influence
    // changed, then rebuild once.
    //
    // rebuild_dirty_cells(): the expensive one. Re-buckets particles, runs the
    // CPU mesh build for every dirty cell across the MeshWorkerPool, then
    // commits the results on the calling thread (mesh upload plus BLAS
    // registration), so it must be called from the thread that owns the
    // graphics context. Cost scales with dirty cells x particles, not with the
    // number of marks.
    void mark_cells_dirty_around_particle(const mm::Vec3& local_position, float radius);
    void rebuild_dirty_cells();
    std::vector<Cell*> get_cells_in_region(const mm::Vec3& min_bound, const mm::Vec3& max_bound);
    
    // Visitor pattern support
    void accept(CellVisitor& visitor) const;
    
    // TLAS integration
    // Walks every meshed cell and emits one TLAS instance per merge-group BLAS.
    // `const` on the Cluster only -- it mutates the referenced TLASManager, and
    // it appends rather than replacing, so calling it twice duplicates every
    // instance. The instance material it packs is a merge-group id used purely
    // as a fallback; real triangles carry their own per-triangle materialId.
    // Only the cluster's translation is applied (rotation_ is not).
    void add_to_tlas() const;
    
    // Cell sizing
    void set_smallest_cell_size(float size) { smallest_cell_size_ = size; }
    float get_smallest_cell_size() const { return smallest_cell_size_; }
    

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



    // Statistics
    uint32_t get_cell_count() const;
    uint32_t get_dirty_cell_count() const;

private:
    // Cluster identification and transform
    uint32_t cluster_id_;
    mm::Vec3 position_;         // World position
    mm::Quat rotation_;         // World rotation
    
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
    mm::Vec3 get_cell_coordinates(const mm::Vec3& local_position) const;
    Cell* find_or_create_cell(const mm::Vec3& cell_coords);

    // Finest detail_size across all particles (seeded with base_detail_size_).
    // Drives a single uniform mesh resolution for every meshed cell.
    float compute_finest_detail() const;
};

#endif // CLUSTER_H