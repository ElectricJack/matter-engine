#include "../include/cluster.h"
#include "../include/cell.h"
#include "../include/tlas_manager.hpp"
#include "../include/cell_visitor.h"
#include "../include/occupancy.h"  // pack_slot, SlotCoord
#include "../include/mesh_worker_pool.h"   // MeshWorkerPool + SurfaceScratch
extern "C" {
#include "../include/spatial_hash.h"
}
#include <cmath>
#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <algorithm>
#include <thread>    // std::thread::hardware_concurrency
#include <chrono>
#include <memory>


// Raymath function prototypes we need (without including the problematic header)
extern "C" {
    Quaternion QuaternionIdentity(void);
    Vector3 Vector3RotateByQuaternion(Vector3 v, Quaternion q);
    Quaternion QuaternionInvert(Quaternion q);
    Vector3 Vector3Add(Vector3 v1, Vector3 v2);
    Vector3 Vector3Subtract(Vector3 v1, Vector3 v2);
    Vector3 Vector3Scale(Vector3 v, float scalar);
    float Vector3Length(Vector3 v);
    float Vector3DotProduct(Vector3 v1, Vector3 v2);
    Matrix MatrixTranslate(float x, float y, float z);
}

Cluster::Cluster(uint32_t cluster_id, BLASManager& blas_manager, TLASManager& tlas_manager, float smallest_cell_size)
    : cluster_id_(cluster_id),
      position_({0.0f, 0.0f, 0.0f}),
      rotation_(QuaternionIdentity()),
      blas_manager_(blas_manager),
      tlas_manager_(tlas_manager),
      next_particle_id_(0),
      smallest_cell_size_(smallest_cell_size) {
    
    // Initialize spatial hash for cell management
    // Use cell size as spatial hash cell size for efficient cell lookup
    cell_spatial_hash_ = sh_create(smallest_cell_size, 1000);
    if (!cell_spatial_hash_) {
        printf("Warning: Failed to initialize spatial hash for cluster %u\n", cluster_id_);
    }

    // Persistent CPU mesh worker pool: default to all-but-one hardware thread so
    // the main thread (GL/BLAS/TLAS commit) stays responsive. One SurfaceScratch
    // per worker makes the mesh build re-entrant.
    unsigned hw = std::thread::hardware_concurrency();
    int default_workers = (hw > 1u) ? (int)(hw - 1u) : 1;
    mesh_pool_ = std::make_unique<MeshWorkerPool>(default_workers);

    printf("Created cluster %u with smallest cell size %.2f\n", cluster_id_, smallest_cell_size_);
}

Cluster::~Cluster() {
    // Clear all cells (this will free their meshes)
    cells_.clear();
    
    // Cleanup spatial hash
    if (cell_spatial_hash_) {
        sh_destroy(cell_spatial_hash_);
    }

    // Worker pool joins its threads and frees their scratches in its destructor.
    mesh_pool_.reset();

    printf("Destroyed cluster %u\n", cluster_id_);
}

Vector3 Cluster::local_to_world(const Vector3& local_pos) const {
    Vector3 rotated = Vector3RotateByQuaternion(local_pos, rotation_);
    return Vector3Add(position_, rotated);
}

Vector3 Cluster::world_to_local(const Vector3& world_pos) const {
    Vector3 relative = Vector3Subtract(world_pos, position_);
    return Vector3RotateByQuaternion(relative, QuaternionInvert(rotation_));
}

uint32_t Cluster::add_particle(const Vector3& local_position, float radius, uint32_t material_id) {
    uint32_t particle_id = next_particle_id_++;
    
    // Add particle to storage
    particles_.emplace_back(local_position, radius, material_id);
    
    // Mark cells dirty around this particle
    mark_cells_dirty_around_particle(local_position, radius);
    
    printf("Added particle %u to cluster %u at (%.2f, %.2f, %.2f)\n",
           particle_id, cluster_id_, local_position.x, local_position.y, local_position.z);

    return particle_id;
}

uint32_t Cluster::add_particle(const Vector3& local_position, float radius, uint32_t material_id, const Vector4& tint) {
    uint32_t particle_id = next_particle_id_++;
    particles_.emplace_back(local_position, radius, material_id, tint);
    mark_cells_dirty_around_particle(local_position, radius);
    return particle_id;
}

uint32_t Cluster::add_particle(const Vector3& local_position, float radius, uint32_t material_id,
                               const Vector4& tint, float detail_size) {
    uint32_t particle_id = next_particle_id_++;
    particles_.emplace_back(local_position, radius, material_id, tint, detail_size);
    mark_cells_dirty_around_particle(local_position, radius);
    return particle_id;
}

bool Cluster::remove_particle(uint32_t particle_id) {
    if (particle_id >= particles_.size()) {
        return false;
    }
    
    // Mark cells dirty around the particle being removed
    const StaticParticle& particle = particles_[particle_id];
    mark_cells_dirty_around_particle(particle.position, particle.radius);
    
    // Remove particle using swap-and-pop for efficiency
    if (particle_id != particles_.size() - 1) {
        particles_[particle_id] = particles_.back();
        
        // Update all cells that reference the moved particle
        for (auto& cell : cells_) {
            for (auto& material_entry : cell->material_particle_indices) {
                auto& indices = material_entry.second;
                for (auto& idx : indices) {
                    if (idx == particles_.size() - 1) {
                        idx = particle_id;
                    }
                }
            }
        }
    }
    
    particles_.pop_back();
    
    printf("Removed particle %u from cluster %u\n", particle_id, cluster_id_);
    return true;
}

bool Cluster::update_particle_position(uint32_t particle_id, const Vector3& new_local_position) {
    if (particle_id >= particles_.size()) {
        return false;
    }
    
    StaticParticle& particle = particles_[particle_id];
    
    // Mark cells dirty around old and new positions
    mark_cells_dirty_around_particle(particle.position, particle.radius);
    mark_cells_dirty_around_particle(new_local_position, particle.radius);
    
    // Update position
    particle.position = new_local_position;
    
    return true;
}

void Cluster::mark_cells_dirty_around_particle(const Vector3& local_position, float radius) {
    // Calculate the range of cell coordinates that might be affected
    float influence_radius = radius * 2.0f; // Conservative estimate
    float cell_size = smallest_cell_size_;
    
    // Calculate cell coordinate range for the current LOD level only
    Vector3 min_cell = {
        floorf((local_position.x - influence_radius) / cell_size),
        floorf((local_position.y - influence_radius) / cell_size),
        floorf((local_position.z - influence_radius) / cell_size)
    };
    Vector3 max_cell = {
        floorf((local_position.x + influence_radius) / cell_size),
        floorf((local_position.y + influence_radius) / cell_size),
        floorf((local_position.z + influence_radius) / cell_size)
    };
    
    // Mark cells in this range as dirty
    for (int x = (int)min_cell.x; x <= (int)max_cell.x; ++x) {
        for (int y = (int)min_cell.y; y <= (int)max_cell.y; ++y) {
            for (int z = (int)min_cell.z; z <= (int)max_cell.z; ++z) {
                Vector3 cell_coords = {(float)x, (float)y, (float)z};
                Cell* cell = find_or_create_cell(cell_coords);
                if (cell) {
                    cell->is_dirty = true;
                }
            }
        }
    }
}

Vector3 Cluster::get_cell_coordinates(const Vector3& local_position) const {
    float cell_size = smallest_cell_size_;
    return Vector3{
        floorf(local_position.x / cell_size),
        floorf(local_position.y / cell_size),
        floorf(local_position.z / cell_size)
    };
}

Cell* Cluster::find_or_create_cell(const Vector3& cell_coords) {
    float cell_size = smallest_cell_size_;
    
    // Calculate cell center for spatial hash lookup
    Vector3 cell_center = {
        (cell_coords.x + 0.5f) * cell_size,
        (cell_coords.y + 0.5f) * cell_size,
        (cell_coords.z + 0.5f) * cell_size
    };
    
    // Try to find existing cell
    void* result = sh_query_first(cell_spatial_hash_, cell_center.x, cell_center.y, cell_center.z, cell_size * 0.1f);
    if (result) {
        return static_cast<Cell*>(result);
    }
    
    // Create new cell
    auto new_cell = std::make_unique<Cell>(cell_coords, 0, smallest_cell_size_);
    Cell* cell_ptr = new_cell.get();
    
    // Insert into spatial hash
    sh_insert(cell_spatial_hash_, cell_center.x, cell_center.y, cell_center.z, cell_ptr);
    
    // Store cell
    cells_.push_back(std::move(new_cell));
    
    return cell_ptr;
}

void Cluster::set_no_mesh_cells(const std::vector<Vector3>& coords) {
    no_mesh_cells_.clear();
    no_mesh_cells_.reserve(coords.size());
    for (const Vector3& c : coords) {
        no_mesh_cells_.insert(pack_slot(SlotCoord{
            (int)lroundf(c.x), (int)lroundf(c.y), (int)lroundf(c.z)}));
    }
}

void Cluster::set_mesh_worker_count(int n) {
    if (mesh_pool_) mesh_pool_->resize(n);
}

int Cluster::get_mesh_worker_count() const {
    return mesh_pool_ ? mesh_pool_->size() : 0;
}

void Cluster::rebuild_dirty_cells() {
    // One resolution for every meshed cell: derived from the globally finest
    // detail so neighboring marching-cubes grids align and stay watertight.
    float uniform_detail = compute_finest_detail();

    auto t_start = std::chrono::steady_clock::now();

    // PHASE 1 - PRE (serial, main thread): per dirty non-interior cell, gather
    // particle indices + carve subset, release the old BLAS, and queue a CellJob.
    std::vector<CellJob> jobs;
    uint32_t processed = 0;   // non-interior dirty cells handled (gates TLAS rebuild)

    for (auto& cell : cells_) {
        if (!cell->is_dirty) continue;

        uint64_t key = pack_slot(SlotCoord{
            (int)lroundf(cell->coordinates.x),
            (int)lroundf(cell->coordinates.y),
            (int)lroundf(cell->coordinates.z)});
        if (no_mesh_cells_.find(key) != no_mesh_cells_.end()) {
            cell->clear_meshes(&blas_manager_);  // interior cell: never meshed
            cell->is_dirty = false;
            continue;
        }

        // Assign particles to this cell (read-only over particles_).
        cell->clear_particle_indices();
        for (uint32_t i = 0; i < particles_.size(); ++i) {
            const StaticParticle& particle = particles_[i];
            if (cell->intersects_sphere(particle.position, particle.radius)) {
                cell->add_particle_index(i, particle.materialId);
            }
        }

        // Release the previous build's BLAS on the main thread before re-meshing.
        cell->clear_meshes(&blas_manager_);
        cell->is_dirty = false;
        cell->mesh_version++;
        processed++;

        if (cell->material_particle_indices.empty()) {
            continue;  // nothing to mesh (already cleared)
        }

        CellJob job;
        job.cell = cell.get();
        // Gather carve particles whose influence overlaps this cell (mirrors the
        // additive intersects_sphere halo; slack covers the carve fillet reach).
        for (const Particle& cpart : carve_particles_) {
            if (cell->intersects_sphere(cpart.position, cpart.radius * 1.5f))
                job.carve.push_back(cpart);
        }
        job.simplification_ratio = simplification_ratio_;
        job.base_detail = base_detail_size_;
        job.max_pow = max_division_pow_;
        job.uniform_detail = uniform_detail;
        jobs.push_back(std::move(job));
    }

    // PHASE 2 - PARALLEL: build every job's cell mesh on the worker pool. Each
    // worker uses its own SurfaceScratch; particles_ is read-only here.
    std::vector<CellMeshResult> results;
    if (!jobs.empty()) {
        mesh_pool_->run(jobs, results,
            [this](const CellJob& job, SurfaceScratch* scratch, CellMeshResult& out) {
                const Particle* carvePtr = job.carve.empty() ? nullptr : job.carve.data();
                int carveCount = static_cast<int>(job.carve.size());
                out = job.cell->build_cell_meshes(particles_, scratch, job.simplification_ratio,
                                                  job.base_detail, job.max_pow, job.uniform_detail,
                                                  carvePtr, carveCount);
            });
    }

    // PHASE 3 - DRAIN (serial, fixed job order): commit GL/BLAS deterministically.
    uint32_t committed_groups = 0;
    for (size_t i = 0; i < jobs.size(); ++i) {
        jobs[i].cell->commit_cell_meshes(results[i], blas_manager_);
        committed_groups += static_cast<uint32_t>(results[i].groups.size());
    }

    // One TLAS rebuild if anything changed (meshed or cleared).
    if (processed > 0) {
        tlas_manager_.clear();
        add_to_tlas();
        tlas_manager_.build(blas_manager_);
    }

    auto t_end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    printf("REBUILD: %u cells processed, %zu meshed / %u groups, %.1f ms (%d workers)\n",
           processed, jobs.size(), committed_groups, ms, mesh_pool_->size());
}

float Cluster::compute_finest_detail() const {
    float finest = base_detail_size_;
    for (const auto& p : particles_) {
        if (p.detail_size > 0.0f && (finest <= 0.0f || p.detail_size < finest))
            finest = p.detail_size;
    }
    return finest;
}

std::vector<Cell*> Cluster::get_cells_in_region(const Vector3& min_bound, const Vector3& max_bound) {
    std::vector<Cell*> result;
    
    // Query spatial hash for cells in region
    Vector3 region_center = {
        (min_bound.x + max_bound.x) * 0.5f,
        (min_bound.y + max_bound.y) * 0.5f,
        (min_bound.z + max_bound.z) * 0.5f
    };
    Vector3 region_size = {
        max_bound.x - min_bound.x,
        max_bound.y - min_bound.y,
        max_bound.z - min_bound.z
    };
    float search_radius = Vector3Length(region_size) * 0.5f;
    
    void* query_results[1000];
    int found_count = sh_query_radius(cell_spatial_hash_, 
                                     region_center.x, region_center.y, region_center.z,
                                     search_radius, query_results, 1000);
    
    for (int i = 0; i < found_count; ++i) {
        Cell* cell = static_cast<Cell*>(query_results[i]);
        
        // Check if cell actually intersects the region
        if (cell->min_bound.x <= max_bound.x && cell->max_bound.x >= min_bound.x &&
            cell->min_bound.y <= max_bound.y && cell->max_bound.y >= min_bound.y &&
            cell->min_bound.z <= max_bound.z && cell->max_bound.z >= min_bound.z) {
            result.push_back(cell);
        }
    }
    
    return result;
}

void Cluster::accept(CellVisitor& visitor) const {
    visitor.visit_cluster(*this);
}

void Cluster::visit_cells(CellRenderVisitor& visitor) const {
    // Create transform matrix for cluster world position
    Matrix cluster_transform = MatrixTranslate(position_.x, position_.y, position_.z);
    
    uint32_t cells_with_meshes = 0;
    for (const auto& cell : cells_) {
        if (cell->has_meshes) {
            cells_with_meshes++;
            cell->accept_transformed(visitor, cluster_transform);
        }
    }
    
    // static int debug_counter = 0;
    // if (debug_counter++ % 60 == 0) { // Print every 60 frames
    //     printf("Cluster render: %u/%zu cells have meshes\n", cells_with_meshes, cells_.size());
    // }
}

void Cluster::visit_all_cells(CellVisitor& visitor) const {
    for (const auto& cell : cells_) {
        cell->accept(visitor);
    }
}

void Cluster::add_to_tlas() const {
    // Add all cell meshes to the TLAS for ray tracing
    for (const auto& cell : cells_) {
        if (cell->has_meshes) {
            const auto& material_blas = cell->get_material_blas();
            for (const auto& blas_entry : material_blas) {
                // The BLAS map key is a merge-GROUP id, not a shading material.
                uint32_t group_id = blas_entry.first;
                BLASHandle blas_handle = blas_entry.second;

                if (blas_handle > 0) {
                    // TODO: Apply cluster transform (position + rotation)
                    // For now, just add at identity transform
                    tlas_manager_.load_identity();
                    tlas_manager_.translate(position_.x, position_.y, position_.z);
                    //tlas_manager.rotate_quaternion(rotation_);
                    // The value packed as the instance material is a merge-group id,
                    // used only as a fallback because every real triangle carries its
                    // own per-triangle materialId.
                    tlas_manager_.draw(blas_handle, group_id);
                }
            }
        }
    }
}

uint32_t Cluster::get_cell_count() const {
    return static_cast<uint32_t>(cells_.size());
}

uint32_t Cluster::get_dirty_cell_count() const {
    uint32_t count = 0;
    for (const auto& cell : cells_) {
        if (cell->is_dirty) {
            count++;
        }
    }
    return count;
}

void Cluster::force_rebuild_all_cells() {
    printf("Cluster %u: Force rebuilding all cells\n", cluster_id_);

    // Clear all existing cells
    clear_all_cells();

    // Every cell mesh is regenerated below, so wipe the BLAS manager to reclaim
    // entries from the previous build (they otherwise leak: cell destructors run
    // without a manager handle, and re-meshing yields fresh content hashes that
    // dedup can't match).
    blas_manager_.clear();

    // Mark all particles as needing new cell assignment
    for (const auto& particle : particles_) {
        mark_cells_dirty_around_particle(particle.position, particle.radius);
    }
    
    // Rebuild all dirty cells
    rebuild_dirty_cells();
}

void Cluster::clear_particles() {
    particles_.clear();
    next_particle_id_ = 0;
}

void Cluster::clear_all_cells() {
    printf("Cluster %u: Clearing all %zu cells\n", cluster_id_, cells_.size());
    
    // Clear cells vector (this will call destructors and free mesh memory)
    cells_.clear();
    
    // Clear spatial hash
    if (cell_spatial_hash_) {
        // The spatial hash entries will be cleared when cells are destroyed
        // But we need to reset the spatial hash structure
        sh_destroy(cell_spatial_hash_);
        cell_spatial_hash_ = sh_create(smallest_cell_size_, 1000);
        
        if (!cell_spatial_hash_) {
            printf("Warning: Failed to recreate spatial hash for cluster %u after clearing cells\n", cluster_id_);
        }
    }
}