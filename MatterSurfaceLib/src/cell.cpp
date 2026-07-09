#include "../include/cell.h"
#include "../include/cluster.h"
#include "../include/blas_manager.hpp"
#include "../include/bvh_analyzer.h"
#include "../include/cell_visitor.h"
#include "material_registry.h"
#include "mesh_simplifier.hpp"
#include "../include/mesh_worker_pool.h"
#include "mesh_build_utils.h"
#include "meshing_algorithm.h"
#include <utility>   // std::move
extern "C" {
#include "../include/spatial_hash.h"  // sh_query_radius_nearest for per-triangle material/tint
}
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

// Forward declarations we need for surface mesh generation.
// Bounds/Particle and GenerateMesh/ComputeSurfaceNormals now live in cell.h
// (the single source), so they are not re-declared here.
extern "C" {
    // Raymath and raylib functions we need
    Vector3 Vector3Add(Vector3 v1, Vector3 v2);
    Vector3 Vector3Subtract(Vector3 v1, Vector3 v2);
    Vector3 Vector3Scale(Vector3 v, float scalar);
    Vector3 Vector3Transform(Vector3 v, Matrix mat);
    float Vector3Length(Vector3 v);
    float Vector3DotProduct(Vector3 v1, Vector3 v2);
    Material LoadMaterialDefault(void);
    void DrawMesh(Mesh mesh, Material material, Matrix transform);
    void DrawLine3D(Vector3 startPos, Vector3 endPos, Color color);
    Matrix MatrixIdentity(void);
    Matrix MatrixTranslate(float x, float y, float z);
    void DrawCubeWires(Vector3 position, float width, float height, float length, Color color);
    void DrawSphere(Vector3 centerPos, float radius, Color color);
    void RL_FREE(void *ptr);
}

int choose_division_pow(float detail_size_min, float base_detail, int base_pow, int max_pow) {
    int tier = 0;
    if (detail_size_min > 0.0f && base_detail > 0.0f && detail_size_min < base_detail) {
        tier = (int)lroundf(log2f(base_detail / detail_size_min));
        if (tier < 0) tier = 0;
    }
    int pow = base_pow + tier;
    if (pow < base_pow) pow = base_pow;
    if (pow > max_pow)  pow = max_pow;
    return pow;
}

Cell::Cell(const Vector3& coords, int size_pow, float smallest_cell_size)
    : coordinates(coords),
      size_power(size_pow),
      actual_size(smallest_cell_size * (1 << size_pow)),
      has_meshes(false),
      is_dirty(true),
      mesh_version(0) {
    
    calculate_bounds(smallest_cell_size);
}

Cell::~Cell() {
    clear_meshes();
}

void Cell::calculate_bounds(float smallest_cell_size) {
    // Cell coordinates use the cluster's CORNER convention: a point at local
    // position p belongs to cell floor(p / size), so cell C spans
    // [C*size, (C+1)*size] and is centered at (C+0.5)*size. This must match
    // Cluster::get_cell_coordinates and the cell spatial-hash key; otherwise
    // the mesh-generation box is shifted half a cell and spheres render as
    // partial blobs.
    center = Vector3{
        (coordinates.x + 0.5f) * actual_size,
        (coordinates.y + 0.5f) * actual_size,
        (coordinates.z + 0.5f) * actual_size
    };
    
    Vector3 half_size = Vector3{actual_size * 0.5f, actual_size * 0.5f, actual_size * 0.5f};
    min_bound = Vector3Subtract(center, half_size);
    max_bound = Vector3Add(center, half_size);
}

bool Cell::contains_point(const Vector3& local_point) const {
    return (local_point.x >= min_bound.x && local_point.x <= max_bound.x &&
            local_point.y >= min_bound.y && local_point.y <= max_bound.y &&
            local_point.z >= min_bound.z && local_point.z <= max_bound.z);
}

bool Cell::intersects_sphere(const Vector3& sphere_center, float radius) const {
    // Find closest point on the cell's bounding box to the sphere center
    Vector3 closest = {
        fmaxf(min_bound.x, fminf(sphere_center.x, max_bound.x)),
        fmaxf(min_bound.y, fminf(sphere_center.y, max_bound.y)),
        fmaxf(min_bound.z, fminf(sphere_center.z, max_bound.z))
    };
    
    // Calculate distance from sphere center to closest point
    Vector3 diff = Vector3Subtract(sphere_center, closest);
    float distance_squared = Vector3DotProduct(diff, diff);
    
    return distance_squared <= (radius * radius);
}

void Cell::add_particle_index(uint32_t particle_index, uint32_t material_id) {
    // Bucket by merge group, not raw material id, so shades that share a group
    // feed one SDF field and blend into a single mesh.
    uint32_t group = (uint32_t)MaterialMergeGroup((int)material_id);
    auto& material_particles = material_particle_indices[group];
    // Check if already exists
    if (std::find(material_particles.begin(), material_particles.end(), particle_index) == material_particles.end()) {
        material_particles.push_back(particle_index);
        is_dirty = true;
    }
}

void Cell::add_particle_index_unchecked(uint32_t particle_index, uint32_t material_id) {
    // Fast path used in rebuild_dirty_cells after clear_particle_indices(): the
    // caller iterates each particle at most once per cell, so uniqueness is
    // guaranteed — skip the O(n) std::find.
    uint32_t group = (uint32_t)MaterialMergeGroup((int)material_id);
    material_particle_indices[group].push_back(particle_index);
    is_dirty = true;
}

void Cell::remove_particle_index(uint32_t particle_index, uint32_t material_id) {
    uint32_t group = (uint32_t)MaterialMergeGroup((int)material_id);
    auto material_it = material_particle_indices.find(group);
    if (material_it != material_particle_indices.end()) {
        auto& material_particles = material_it->second;
        auto it = std::find(material_particles.begin(), material_particles.end(), particle_index);
        if (it != material_particles.end()) {
            material_particles.erase(it);
            is_dirty = true;
            
            // Clean up empty material entries
            if (material_particles.empty()) {
                material_particle_indices.erase(material_it);
            }
        }
    }
}

void Cell::clear_particle_indices() {
    material_particle_indices.clear();
    is_dirty = true;
}

void Cell::rebuild_meshes(const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager,
                          SurfaceScratch* scratch,
                          float simplification_ratio, float base_detail, int max_pow, float uniform_detail,
                          const Particle* carveParticles, int carveCount) {
    clear_meshes(&blas_manager);

    if (material_particle_indices.empty()) {
        return;
    }

    CellMeshResult result = build_cell_meshes(cluster_particles, scratch, simplification_ratio,
                                              base_detail, max_pow, uniform_detail,
                                              carveParticles, carveCount);
    commit_cell_meshes(result, blas_manager);
}


// --- Feature-degradation tunables, expressed in voxels of the current cell ---
// Marching cubes only emits a surface where a grid sample lands inside an SDF
// well, so a feature must span ~1 voxel to be sampled at all. A coarser cell
// (smaller divisionPow) has a larger voxel, so a fixed-size particle's
// size-in-voxels (rv = radius/voxel) shrinks, walking it through three regimes:
//   rv >= kFeatureVisVoxels    : kept at its true radius (fully resolvable).
//   rv in [kCull, kVis)        : lifted to kFeatureVisVoxels voxels so it stays
//                                samplable -- a slight enlargement -- and the
//                                smooth-min union blends it into larger neighbors.
//   rv <  kFeatureCullVoxels   : dropped entirely. If it sat next to a bigger
//                                particle the blend already absorbed it, so no
//                                hole appears; isolated tiny features just vanish.
// Net effect as the voxel grows: slightly larger -> metaball blend -> gone.
static constexpr float kFeatureVisVoxels  = 1.0f;
static constexpr float kFeatureCullVoxels = 0.6f;

// Smooth-min fillet width as a fraction of voxel size. A coarser cell has a
// larger voxel, so the metaball blend strengthens; at the finest resolution it
// is near-sharp. Kept small: the field query now returns the true nearest
// neighbors (sh_query_radius_nearest), so the smooth-min sums the full local
// cluster. A wide fillet then fills the valleys between particles and flattens
// the surface (especially dense tier-1 sub-particles); a narrow one keeps each
// particle's bump defined while staying watertight.
static constexpr float kBlendVoxels = 0.15f;

std::vector<Particle> build_clip_particles(
    uint32_t group_id,
    const std::map<uint32_t, std::vector<uint32_t>>& buckets,
    const std::vector<StaticParticle>& cluster_particles,
    bool group_transparent,
    float cull_radius, float vis_radius) {
    std::vector<Particle> clip;
    for (const auto& other : buckets) {
        if (other.first == group_id || other.second.empty()) continue;

        // A merge group is one optical class, so a representative particle's
        // material decides transparency for the whole foreign group.
        uint32_t rep_idx = other.second.front();
        if (rep_idx >= cluster_particles.size()) continue;
        int rep_mat = static_cast<int>(cluster_particles[rep_idx].materialId);
        bool other_transparent = MaterialIsTransparent(rep_mat) != 0;

        // Transparency gate: only carve when at least one side is transparent.
        // opaque<->opaque overlap is hidden, so leave it uncarved.
        if (!(group_transparent || other_transparent)) continue;

        // Add the foreign group's particles using the SAME LOD taper/cull as the
        // group's own particles, so the carve locus matches the meshed field.
        for (uint32_t idx : other.second) {
            if (idx >= cluster_particles.size()) continue;
            const StaticParticle& sp = cluster_particles[idx];
            if (sp.radius < cull_radius) continue;
            float r_eff = (sp.radius < vis_radius) ? vis_radius : sp.radius;

            Particle cp;
            cp.position = sp.position;
            cp.radius = r_eff;
            cp.materialId = static_cast<int>(sp.materialId); // unused by carve math; set for consistency
            clip.push_back(cp);
        }
    }
    return clip;
}

GroupMeshResult Cell::build_group_mesh(uint32_t group_id, const std::vector<StaticParticle>& cluster_particles,
                                       SurfaceScratch* scratch,
                                       float simplification_ratio, float base_detail, int max_pow, float uniform_detail,
                                       const Particle* carveParticles, int carveCount,
                                       const FieldStages* stages, const FatPrim* fat, int fatCount,
                                       const int* clusterStage) const {
    GroupMeshResult result;
    result.group_id = group_id;

    auto group_it = material_particle_indices.find(group_id);
    if (group_it == material_particle_indices.end() || group_it->second.empty()) {
        // Fat-prim-only groups (e.g. capsule line() strokes in a voxel session)
        // have no additive particle indices but still need to be meshed.  Skip
        // only when there are also no fat prims — the combined empty check at
        // line "if (particles.empty() && cell_fat.empty())" below handles the
        // true-empty case once fat prims have been collected.
        if (fatCount == 0) return result;
    }

    // particle_indices is empty for fat-prim-only groups; the loops below are
    // no-ops in that case, which is correct (fat prims provide all the geometry).
    static const std::vector<uint32_t> kEmptyIndices;
    const auto& particle_indices =
        (group_it != material_particle_indices.end()) ? group_it->second : kEmptyIndices;

    Bounds bounds;
    bounds.center = center;
    bounds.size = Vector3{actual_size, actual_size, actual_size};

    float detail_min;
    if (uniform_detail > 0.0f) {
        detail_min = uniform_detail;
    } else {
        detail_min = base_detail;
        for (uint32_t idx : particle_indices) {
            if (idx >= cluster_particles.size()) continue;
            float ds = cluster_particles[idx].detail_size;
            if (ds > 0.0f && ds < detail_min) detail_min = ds;
        }
    }
    bounds.divisionPow = choose_division_pow(detail_min, base_detail, 4, max_pow);
    int gridSize = 1 << bounds.divisionPow;
    float voxel = actual_size / (float)(gridSize - 1);
    float blend_voxels = kBlendVoxels;
    if (const char* e = getenv("MSL_BLEND_VOXELS")) { float v = (float)atof(e); if (v >= 0.0f) blend_voxels = v; }
    float blend_width = blend_voxels * voxel;
    float carve_blend = blend_width;
    if (const char* e = getenv("MSL_CARVE_BLEND")) { float v = (float)atof(e); if (v > 0.0f) carve_blend = v; }
    float cull_radius = kFeatureCullVoxels * voxel;
    float vis_radius  = kFeatureVisVoxels  * voxel;

    std::vector<Particle> particles;
    std::vector<float4> particle_tints;
    // Local CSG stage index parallel to `particles` (only populated when the
    // caller supplied a clusterStage map; drives the ordered-CSG field eval).
    std::vector<int> particle_stage;
    particles.reserve(particle_indices.size());
    particle_tints.reserve(particle_indices.size());
    if (clusterStage) particle_stage.reserve(particle_indices.size());
    float max_radius = 0.0f;
    for (uint32_t idx : particle_indices) {
        if (idx >= cluster_particles.size()) continue;
        const StaticParticle& sp = cluster_particles[idx];
        if (sp.radius < cull_radius) continue;
        float r_eff = (sp.radius < vis_radius) ? vis_radius : sp.radius;

        Particle surface_particle;
        surface_particle.position = sp.position;
        surface_particle.radius = r_eff;
        surface_particle.materialId = static_cast<int>(sp.materialId);
        particles.push_back(surface_particle);
        particle_tints.push_back(make_float4(sp.tint.x, sp.tint.y, sp.tint.z, sp.tint.w));
        if (clusterStage) particle_stage.push_back(clusterStage[idx]);
        if (r_eff > max_radius) max_radius = r_eff;
    }

    // Typed iso-primitives (Phase 1): keep only fat prims whose bounding sphere
    // overlaps this cell box (same halo test the sphere path uses). The cell's
    // sampling grid already spans the full cell box, so an overlapping fat prim
    // is sampled where it enters the cell; ones outside the cell are dropped to
    // keep the per-cell linear scan small. Cluster-level cell creation (caller)
    // is responsible for ensuring a cell exists wherever a fat prim has surface.
    std::vector<FatPrim> cell_fat;
    if (fat && fatCount > 0) {
        cell_fat.reserve(fatCount);
        for (int i = 0; i < fatCount; ++i) {
            if (intersects_sphere(fat[i].center, fat[i].boundRadius * 1.5f))
                cell_fat.push_back(fat[i]);
        }
    }

    // A group can be sphere-only, mixed, or fat(box)-only. Only bail when this
    // cell has neither spheres nor overlapping fat prims to sample.
    if (particles.empty() && cell_fat.empty()) {
        return result;
    }

    bool group_transparent = particles.empty()
        ? (MaterialIsTransparent(cell_fat[0].materialId) != 0)
        : (MaterialIsTransparent(particles[0].materialId) != 0);
    std::vector<Particle> clip = build_clip_particles(
        group_id, material_particle_indices, cluster_particles,
        group_transparent, cull_radius, vis_radius);
    Particle* clipPtr = clip.empty() ? NULL : clip.data();
    int clipCount = static_cast<int>(clip.size());
    const FatPrim* fatPtr = cell_fat.empty() ? nullptr : cell_fat.data();
    int cellFatCount = static_cast<int>(cell_fat.size());

    // A fat-only group has no spheres, so max_radius is still 0 and the field
    // query / hash cell size (r*2.5 + blend*4) would collapse to 0. Seed it from
    // the largest fat bounding radius so the grid sampling has a sane scale.
    if (particles.empty() && cellFatCount > 0) {
        for (int i = 0; i < cellFatCount; ++i)
            if (cell_fat[i].boundRadius > max_radius) max_radius = cell_fat[i].boundRadius;
    }

    // Build a per-cell FieldStages. The staged field eval recovers a hashed
    // particle's stage by pointer arithmetic against `_particleBase`, so it MUST
    // point at this cell's local `particles` array (the one the mesher hashes),
    // with `particleStage` parallel to it. The ordered stage-op list itself is
    // shared (borrowed from the caller's global FieldStages).
    FieldStages cellStages;
    const FieldStages* stagesPtr = nullptr;
    if (stages && (stages->stageCount > 1 || cellFatCount > 0)) {
        cellStages.stageOp = stages->stageOp;
        cellStages.stageCount = stages->stageCount;
        // particleStage may be empty for a fat-only group (no spheres to tag);
        // the staged eval then leaves every sphere at stage 0 (there are none).
        cellStages.particleStage = particle_stage.empty() ? nullptr : particle_stage.data();
        cellStages._particleBase = particles.empty() ? nullptr : particles.data();
        cellStages._particleCount = (long)particles.size();
        stagesPtr = &cellStages;
    }

    // Pack the resolved group + meshing parameters and dispatch to the algorithm
    // selected by the group's representative material. Materials in a merge group
    // are required to agree on algorithm, so particles[0] is authoritative. The
    // context's references/pointers borrow these locals and stay valid for the
    // duration of the generate() call (it runs synchronously on this worker).
    MeshContext ctx{
        particles,
        particle_tints,
        max_radius,
        bounds,
        CellBounds{ min_bound, max_bound },
        voxel,
        blend_width,
        clipPtr, clipCount,
        carveParticles, carveCount,
        carve_blend,
        simplification_ratio,
        scratch,
        group_id,
        stagesPtr,
        fatPtr, cellFatCount
    };

    int repMat = particles.empty() ? cell_fat[0].materialId : particles[0].materialId;
    MeshAlgorithm algo = (MeshAlgorithm)MaterialMeshingAlgorithm(repMat);
    return GetMeshingAlgorithm(algo).generate(ctx);
}

void Cell::commit_group_mesh(GroupMeshResult& result, BLASManager& blas_manager) {
    if (result.mesh.vertexCount <= 0) {
        return;
    }
    uint32_t group_id = result.group_id;

    material_meshes[group_id] = result.mesh;
    UploadMesh(&material_meshes[group_id], false);

    try {
        std::vector<Tri>& triangles = result.triangles;
        std::vector<TriEx>& triangle_normals = result.triangle_normals;

        if (!triangles.empty()) {
            material_blas[group_id] = blas_manager.register_triangles(triangles, triangle_normals);

            BVH* bvh = blas_manager.get_bvh(material_blas[group_id]);
            BvhMesh* mesh_ptr = blas_manager.get_mesh(material_blas[group_id]);
            if (bvh && mesh_ptr) {
                // B11 fix: use a stable name (no triangle count) so the registry
                // key is consistent across rebuilds; unregister the old entry first
                // so we don't accumulate dangling pointers.
                std::string analysis_name = "Cell(" + std::to_string((int)coordinates.x) + "," +
                                           std::to_string((int)coordinates.y) + "," +
                                           std::to_string((int)coordinates.z) + ")_Mat" +
                                           std::to_string(group_id);
                BVHReportManager::UnregisterBVH(analysis_name); // remove stale entry
                BVHReportManager::RegisterBVH(analysis_name, bvh, mesh_ptr);
                // Perf: UpdateAnalysis runs a full BVH quality pass — skip it on
                // every commit unless the caller opts in via MSL_BVH_ANALYSIS=1.
                static const bool bvh_analysis_enabled = (getenv("MSL_BVH_ANALYSIS") != nullptr);
                if (bvh_analysis_enabled) {
                    BVHReportManager::UpdateAnalysis(analysis_name);
                }
            }
        } else {
            material_blas[group_id] = 0;
        }
    } catch (const std::exception& e) {
        printf("Error registering mesh with BLAS manager: %s\n", e.what());
        material_blas[group_id] = 0;
    } catch (...) {
        printf("Unknown error registering mesh with BLAS manager\n");
        material_blas[group_id] = 0;
    }
}

CellMeshResult Cell::build_cell_meshes(const std::vector<StaticParticle>& cluster_particles, SurfaceScratch* scratch,
                                       float simplification_ratio, float base_detail, int max_pow, float uniform_detail,
                                       const Particle* carveParticles, int carveCount,
                                       const FieldStages* stages, const FatPrim* fat, int fatCount,
                                       const int* clusterStage) const {
    CellMeshResult cell_result;
    for (const auto& group_entry : material_particle_indices) {
        uint32_t group_id = group_entry.first;
        GroupMeshResult gr = build_group_mesh(group_id, cluster_particles, scratch, simplification_ratio,
                                              base_detail, max_pow, uniform_detail, carveParticles, carveCount,
                                              stages, fat, fatCount, clusterStage);
        if (gr.mesh.vertexCount > 0) {
            cell_result.groups.push_back(std::move(gr));
        }
    }
    return cell_result;
}

void Cell::commit_cell_meshes(CellMeshResult& result, BLASManager& blas_manager) {
    for (auto& gr : result.groups) {
        commit_group_mesh(gr, blas_manager);
    }
    has_meshes = !material_meshes.empty();
}

void Cell::clear_meshes(BLASManager* blas_manager) {
    // B11 fix: unregister BVH analyzer entries before releasing the BLAS so the
    // registry never holds dangling raw pointers.
    for (const auto& blas_entry : material_blas) {
        std::string analysis_name = "Cell(" + std::to_string((int)coordinates.x) + "," +
                                   std::to_string((int)coordinates.y) + "," +
                                   std::to_string((int)coordinates.z) + ")_Mat" +
                                   std::to_string(blas_entry.first);
        BVHReportManager::UnregisterBVH(analysis_name);
    }

    // Release this cell's BLAS references so the manager can reclaim entries
    // that no live cell still points at (prevents unbounded GPU accumulation).
    if (blas_manager) {
        for (const auto& blas_entry : material_blas) {
            blas_manager->release_blas(blas_entry.second);
        }
    }

    // Properly free both GPU and CPU mesh resources
    for (auto& mesh_entry : material_meshes) {
        Mesh& mesh = mesh_entry.second;

        // First, unload GPU resources (VAO, VBOs, etc.)
        // This is critical - without this, old mesh data stays on GPU!
        UnloadMesh(mesh);

        // Note: UnloadMesh() already frees the CPU memory (vertices, normals, etc.)
        // so we don't need to manually call RL_FREE() anymore
    }

    material_meshes.clear();
    material_blas.clear();
    has_meshes = false;
}

void Cell::accept(CellVisitor& visitor) const {
    visitor.visit_cell(*this);
}

void Cell::accept_transformed(CellRenderVisitor& visitor, const Matrix& transform) const {
    visitor.visit_cell_transformed(*this, transform);
}

float Cell::get_diagonal_length() const {
    Vector3 size = Vector3Subtract(max_bound, min_bound);
    return Vector3Length(size);
}