// libs/MatterSurfaceLib/src/cluster.cpp
//
// A Cluster owns one rigid body of "matter": a flat array of StaticParticles in
// cluster-LOCAL space, a uniform grid of Cells built lazily over them, and the
// pipeline that turns dirty cells into meshes, BLASes and TLAS instances.
//
// Structure
// - `particles_` is append-only during authoring; a particle's index is its
//   identity and is what Cells store in `material_particle_indices`.
// - `cells_` owns every Cell; `cell_spatial_hash_` is a lookup index into it,
//   keyed by cell CENTRE, and holds raw `Cell*` borrowed from `cells_`. Cells
//   are created on demand and never removed, so those pointers stay valid for
//   the cluster's lifetime.
// - `no_mesh_cells_` is the set of packed integer coordinates known to be
//   fully interior: those cells are cleared instead of meshed.
// - `carve_particles_` are subtractive (smooth CSG); they are distributed to
//   cells with the same halo test as additive particles so the carved field
//   stays continuous across cell boundaries.
//
// Threading
// - `mesh_pool_` is a persistent MeshWorkerPool, sized to hardware
//   concurrency - 1 at construction so the main thread stays responsive, with
//   one `SurfaceScratch` per worker. Only the middle phase of
//   `rebuild_dirty_cells` runs on it; everything else -- particle gathering,
//   BLAS release, mesh commit, TLAS rebuild -- is main-thread work.
// - Cluster itself has no locking and is not safe to mutate concurrently.
//
// Coordinates and units: positions are cluster-local metres. `position_` /
// `rotation_` place the cluster in the world; `local_to_world` applies them.
// `add_to_tlas` bakes that same placement into every instance transform.
#include "../include/cluster.h"
#include "../include/cell.h"
#include "../include/tlas_manager.hpp"
#include "../include/cell_visitor.h"
#include "../include/occupancy.h"  // pack_slot, SlotCoord
#include "../include/vertex_ao.h"  // bake_vertex_ao, AoGrid, AoParams
#include "../include/mesh_worker_pool.h"   // MeshWorkerPool + SurfaceScratch
extern "C" {
#include "spatial_hash.h"
}
#include <cmath>
#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <algorithm>
#include <thread>    // std::thread::hardware_concurrency
#include <chrono>
#include <memory>


// Cluster's own Vector3/Vector4/Quaternion/Matrix math moved off raylib's
// Vector3Add/Subtract/DotProduct/Length, QuaternionIdentity/Invert,
// Vector3RotateByQuaternion and MatrixTranslate onto MathLib's mm::
// equivalents in Phase 4 (Step 4) of
// docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md.
// mm::quat_invert()/rotate() are ported element-for-element from
// vulkan_only_compat.cpp's QuaternionInvert/Vector3RotateByQuaternion (the
// CPU-only replacements this MATTER_VULKAN_ONLY build actually links against,
// not raymath.h's canonical formulas -- see matter_math.h's comment above
// quat_invert() for why that distinction matters for bit-identical output).

Cluster::Cluster(uint32_t cluster_id, BLASManager& blas_manager, TLASManager& tlas_manager, float smallest_cell_size)
    : cluster_id_(cluster_id),
      position_({0.0f, 0.0f, 0.0f}),
      rotation_(mm::quat_identity()),
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

mm::Vec3 Cluster::local_to_world(const mm::Vec3& local_pos) const {
    mm::Vec3 rotated = mm::rotate(local_pos, rotation_);
    return mm::add(position_, rotated);
}

uint32_t Cluster::add_particle(const mm::Vec3& local_position, float radius, uint32_t material_id) {
    uint32_t particle_id = next_particle_id_++;
    
    // Add particle to storage
    particles_.emplace_back(local_position, radius, material_id);
    
    // Mark cells dirty around this particle
    mark_cells_dirty_around_particle(local_position, radius);
    
    printf("Added particle %u to cluster %u at (%.2f, %.2f, %.2f)\n",
           particle_id, cluster_id_, local_position.x, local_position.y, local_position.z);

    return particle_id;
}

uint32_t Cluster::add_particle(const mm::Vec3& local_position, float radius, uint32_t material_id, const mm::Vec4& tint) {
    uint32_t particle_id = next_particle_id_++;
    particles_.emplace_back(local_position, radius, material_id, tint);
    mark_cells_dirty_around_particle(local_position, radius);
    return particle_id;
}

uint32_t Cluster::add_particle(const mm::Vec3& local_position, float radius, uint32_t material_id,
                               const mm::Vec4& tint, float detail_size) {
    uint32_t particle_id = next_particle_id_++;
    particles_.emplace_back(local_position, radius, material_id, tint, detail_size);
    mark_cells_dirty_around_particle(local_position, radius);
    return particle_id;
}

// Mark every cell within a particle's influence as needing a rebuild. Despite
// the name it also CREATES those cells if they don't exist yet -- this is the
// only thing that grows the cell grid, so a particle added outside the current
// extent immediately extends it.
//
// The influence radius is a conservative 2x the particle radius (the smooth-min
// blend reaches beyond the particle's own surface), and the loop walks the
// whole integer cell box that covers it, so cost grows cubically with radius /
// cell size.
void Cluster::mark_cells_dirty_around_particle(const mm::Vec3& local_position, float radius) {
    // Calculate the range of cell coordinates that might be affected
    float influence_radius = radius * 2.0f; // Conservative estimate
    float cell_size = smallest_cell_size_;

    // Calculate cell coordinate range for the current LOD level only
    mm::Vec3 min_cell = {
        floorf((local_position.x - influence_radius) / cell_size),
        floorf((local_position.y - influence_radius) / cell_size),
        floorf((local_position.z - influence_radius) / cell_size)
    };
    mm::Vec3 max_cell = {
        floorf((local_position.x + influence_radius) / cell_size),
        floorf((local_position.y + influence_radius) / cell_size),
        floorf((local_position.z + influence_radius) / cell_size)
    };

    // Mark cells in this range as dirty
    for (int x = (int)min_cell.x; x <= (int)max_cell.x; ++x) {
        for (int y = (int)min_cell.y; y <= (int)max_cell.y; ++y) {
            for (int z = (int)min_cell.z; z <= (int)max_cell.z; ++z) {
                mm::Vec3 cell_coords = {(float)x, (float)y, (float)z};
                Cell* cell = find_or_create_cell(cell_coords);
                if (cell) {
                    cell->is_dirty = true;
                }
            }
        }
    }
}

mm::Vec3 Cluster::get_cell_coordinates(const mm::Vec3& local_position) const {
    float cell_size = smallest_cell_size_;
    return mm::Vec3{
        floorf(local_position.x / cell_size),
        floorf(local_position.y / cell_size),
        floorf(local_position.z / cell_size)
    };
}

// Look a cell up by its integer coordinates, creating it if absent. Never
// returns null in practice: allocation failure would throw rather than return.
//
// The lookup is a spatial-hash point query at the cell's exact centre with a
// tolerance of 10% of a cell, which relies on every cell in the hash having
// been inserted at that same derived centre -- keep this in step with
// Cell::calculate_bounds' corner convention.
//
// Cells are only ever added here; nothing removes them, so `cells_` and the
// hash grow monotonically over a cluster's lifetime.
Cell* Cluster::find_or_create_cell(const mm::Vec3& cell_coords) {
    float cell_size = smallest_cell_size_;

    // Calculate cell center for spatial hash lookup
    mm::Vec3 cell_center = {
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

// Re-mesh every dirty cell. Main thread; blocks until the whole batch is done.
//
// Three phases, in order, with the parallel one sandwiched between two serial
// ones so that all GL/BLAS/TLAS mutation stays on this thread:
//   1. PRE (serial)   -- pick one uniform resolution for the batch, build
//                        transient spatial hashes over the additive and carve
//                        particles, then per dirty cell: re-bucket its
//                        particles, release the previous build's BLAS, clear
//                        the dirty flag, bump `mesh_version`, and queue a
//                        CellJob carrying that cell's carve subset.
//   2. MESH (parallel)-- MeshWorkerPool builds each job's CellMeshResult with
//                        its own SurfaceScratch; `particles_` is read-only.
//   3. DRAIN (serial) -- in fixed job order so results are deterministic: bake
//                        per-vertex AO if an occupancy grid is set, then commit
//                        each result (upload + BLAS registration).
// Finally, if anything changed at all -- including cells that were only
// CLEARED -- the TLAS is torn down and rebuilt in full.
//
// The transient hashes exist to avoid an O(dirty x total particles) scan and
// are destroyed before the parallel phase. The per-cell query buffer is sized
// to the worst case (every particle in one box), so a candidate set is never
// truncated. Interior cells listed in `no_mesh_cells_` are cleared rather than
// meshed.
//
// One uniform resolution is deliberate: marching-cubes grids only stay
// watertight between same-resolution neighbours, so the globally finest detail
// wins for every cell in the batch.
void Cluster::rebuild_dirty_cells() {
    // One resolution for every meshed cell: derived from the globally finest
    // detail so neighboring marching-cubes grids align and stay watertight.
    float uniform_detail = compute_finest_detail();

    auto t_start = std::chrono::steady_clock::now();

    // Build a transient particle spatial hash to avoid O(dirty × total) scans.
    // Cell size = smallest_cell_size_ so each cell center query touches ~3^3 buckets.
    // The hash maps particle position → particle index (cast via uintptr_t).
    // Also track max_particle_radius so we can expand the per-cell query box.
    float max_particle_radius = 0.0f;
    float max_carve_radius    = 0.0f;
    for (const auto& p : particles_) {
        if (p.radius > max_particle_radius) max_particle_radius = p.radius;
    }
    for (const auto& cp : carve_particles_) {
        if (cp.radius * 1.5f > max_carve_radius) max_carve_radius = cp.radius * 1.5f;
    }

    // Build additive particle hash (sh_query_box will over-approximate; we
    // refine with intersects_sphere on the candidate set).
    SpatialHash* particle_hash = nullptr;
    if (!particles_.empty()) {
        particle_hash = sh_create(smallest_cell_size_, (int)particles_.size());
        if (particle_hash) {
            for (uint32_t i = 0; i < (uint32_t)particles_.size(); ++i) {
                const mm::Vec3& pos = particles_[i].position;
                // Store index as pointer: (void*)(uintptr_t)(i+1) to distinguish 0 from null.
                sh_insert(particle_hash, pos.x, pos.y, pos.z, (void*)(uintptr_t)(i + 1));
            }
        }
    }

    // Build carve particle hash similarly.
    SpatialHash* carve_hash = nullptr;
    if (!carve_particles_.empty()) {
        carve_hash = sh_create(smallest_cell_size_, (int)carve_particles_.size());
        if (carve_hash) {
            for (uint32_t i = 0; i < (uint32_t)carve_particles_.size(); ++i) {
                // Particle::position is MtVec3 (Phase 4 Step 3), not raylib Vector3.
                const MtVec3& pos = carve_particles_[i].position;
                sh_insert(carve_hash, pos.x, pos.y, pos.z, (void*)(uintptr_t)(i + 1));
            }
        }
    }

    // Scratch buffer for sh_query_box results (reused per cell). Sized to the
    // WORST CASE -- every particle (or every carve particle) landing in one
    // cell's query box -- because sh_query_box bails at `maxResults` in
    // grid-scan order and returns an arbitrary subset with no way to tell that
    // it truncated. Each particle is inserted into the hash exactly once, so
    // this bound is exact. It also keeps the hashed path's result identical to
    // the scan-everything fallback below, which is the semantics the rest of
    // this function assumes. One allocation per rebuild, not per cell.
    const int kMaxQueryResults =
        (int)std::max(particles_.size(), carve_particles_.size());
    std::vector<void*> query_buf((size_t)std::max(kMaxQueryResults, 1));

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

        // Assign particles to this cell using the spatial hash:
        // query the expanded cell AABB then refine with intersects_sphere.
        // Use the unchecked variant: each candidate is unique in the hash,
        // so each matching particle is added at most once.
        cell->clear_particle_indices();

        if (particle_hash) {
            float qxmin = cell->min_bound.x - max_particle_radius;
            float qymin = cell->min_bound.y - max_particle_radius;
            float qzmin = cell->min_bound.z - max_particle_radius;
            float qxmax = cell->max_bound.x + max_particle_radius;
            float qymax = cell->max_bound.y + max_particle_radius;
            float qzmax = cell->max_bound.z + max_particle_radius;
            int found = sh_query_box(particle_hash, qxmin, qymin, qzmin,
                                     qxmax, qymax, qzmax,
                                     query_buf.data(), kMaxQueryResults);
            for (int qi = 0; qi < found; ++qi) {
                uint32_t i = (uint32_t)((uintptr_t)query_buf[qi] - 1);
                if (i >= (uint32_t)particles_.size()) continue;
                const StaticParticle& particle = particles_[i];
                if (cell->intersects_sphere(particle.position, particle.radius)) {
                    cell->add_particle_index_unchecked(i, particle.materialId);
                }
            }
        } else {
            // Fallback: no hash (e.g. OOM) — scan all particles.
            for (uint32_t i = 0; i < (uint32_t)particles_.size(); ++i) {
                const StaticParticle& particle = particles_[i];
                if (cell->intersects_sphere(particle.position, particle.radius)) {
                    cell->add_particle_index_unchecked(i, particle.materialId);
                }
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
        // Gather carve particles whose influence overlaps this cell using the
        // carve hash (mirrors the additive intersects_sphere halo; slack covers
        // the carve fillet reach).
        if (carve_hash) {
            float cxmin = cell->min_bound.x - max_carve_radius;
            float cymin = cell->min_bound.y - max_carve_radius;
            float czmin = cell->min_bound.z - max_carve_radius;
            float cxmax = cell->max_bound.x + max_carve_radius;
            float cymax = cell->max_bound.y + max_carve_radius;
            float czmax = cell->max_bound.z + max_carve_radius;
            int cfound = sh_query_box(carve_hash, cxmin, cymin, czmin,
                                      cxmax, cymax, czmax,
                                      query_buf.data(), kMaxQueryResults);
            for (int qi = 0; qi < cfound; ++qi) {
                uint32_t ci = (uint32_t)((uintptr_t)query_buf[qi] - 1);
                if (ci >= (uint32_t)carve_particles_.size()) continue;
                const Particle& cpart = carve_particles_[ci];
                // cpart.position is Particle's MtVec3 (Phase 4 Step 3); intersects_sphere
                // takes mm::Vec3 as of Phase 4 Step 4 -- cross via mm::from_c().
                if (cell->intersects_sphere(mm::from_c(cpart.position), cpart.radius * 1.5f))
                    job.carve.push_back(cpart);
            }
        } else {
            for (const Particle& cpart : carve_particles_) {
                if (cell->intersects_sphere(mm::from_c(cpart.position), cpart.radius * 1.5f))
                    job.carve.push_back(cpart);
            }
        }
        job.simplification_ratio = simplification_ratio_;
        job.base_detail = base_detail_size_;
        job.max_pow = max_division_pow_;
        job.uniform_detail = uniform_detail;
        jobs.push_back(std::move(job));
    }

    // Free transient hashes.
    if (particle_hash) sh_destroy(particle_hash);
    if (carve_hash)    sh_destroy(carve_hash);

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

    // PHASE 3 - DRAIN (serial, fixed job order): bake per-vertex AO, then commit
    // GL/BLAS deterministically.
    uint32_t committed_groups = 0;
    for (size_t i = 0; i < jobs.size(); ++i) {
        if (ao_occ_) {
            for (auto& g : results[i].groups) {
                bake_vertex_ao(g.triangles, g.triangle_normals, *ao_occ_, ao_grid_, ao_params_);
            }
        }
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

// Smallest positive `detail_size` over every particle, falling back to
// `base_detail_size_`. This is the single resolution the whole batch meshes at
// (see rebuild_dirty_cells). O(particles), rescanned on every rebuild.
float Cluster::compute_finest_detail() const {
    float finest = base_detail_size_;
    for (const auto& p : particles_) {
        if (p.detail_size > 0.0f && (finest <= 0.0f || p.detail_size < finest))
            finest = p.detail_size;
    }
    return finest;
}

// Cells whose bounds overlap the given cluster-local AABB. The broad phase is
// a radius query around the region's bounding sphere -- so it over-fetches for
// elongated regions -- refined by an exact box overlap test.
//
// The broad-phase result buffer is sized to the cell count, so the query
// cannot truncate (sh_query_radius bails at `maxResults` in grid-scan order
// and gives no way to detect that it did). Returned pointers are borrowed from
// `cells_` and stay valid as long as the cluster does.
std::vector<Cell*> Cluster::get_cells_in_region(const mm::Vec3& min_bound, const mm::Vec3& max_bound) {
    std::vector<Cell*> result;

    // Query spatial hash for cells in region
    mm::Vec3 region_center = {
        (min_bound.x + max_bound.x) * 0.5f,
        (min_bound.y + max_bound.y) * 0.5f,
        (min_bound.z + max_bound.z) * 0.5f
    };
    mm::Vec3 region_size = {
        max_bound.x - min_bound.x,
        max_bound.y - min_bound.y,
        max_bound.z - min_bound.z
    };
    float search_radius = mm::length(region_size) * 0.5f;
    
    // Every cell is inserted into the hash exactly once, so cells_.size() is an
    // exact upper bound on what the broad phase can return.
    if (cells_.empty()) return result;
    std::vector<void*> query_results(cells_.size());
    int found_count = sh_query_radius(cell_spatial_hash_,
                                     region_center.x, region_center.y, region_center.z,
                                     search_radius, query_results.data(),
                                     (int)query_results.size());

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

// Emit one TLAS instance per (cell, merge group) BLAS. `const` on the Cluster,
// but it mutates the shared TLASManager: it sets that manager's current
// transform and appends draw records, so it expects to be called right after
// `tlas_manager_.clear()` and before `tlas_manager_.build()`.
//
// The instance transform is the full cluster placement -- rotate by `rotation_`
// then translate by `position_`, the same composition `local_to_world()` applies
// to a point -- so ray-traced geometry lines up with the rasterised geometry
// even for a rotated cluster. There is no scale in a Cluster transform.
void Cluster::add_to_tlas() const {
    // Cluster placement is per-cluster, not per-cell, so build it once.
    const mm::Mat4 cluster_xform =
        mm::from_trs(position_, rotation_, mm::Vec3{1.0f, 1.0f, 1.0f});

    // Add all cell meshes to the TLAS for ray tracing
    for (const auto& cell : cells_) {
        if (cell->has_meshes) {
            const auto& material_blas = cell->get_material_blas();
            for (const auto& blas_entry : material_blas) {
                // The BLAS map key is a merge-GROUP id, not a shading material.
                uint32_t group_id = blas_entry.first;
                BLASHandle blas_handle = blas_entry.second;

                if (blas_handle > 0) {
                    tlas_manager_.load_matrix(cluster_xform);
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
