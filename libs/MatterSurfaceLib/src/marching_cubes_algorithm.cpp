#include "marching_cubes_algorithm.h"
#include "mesh_build_utils.h"
#include "mesh_simplifier.hpp"   // simplify_mesh, SimplifyOptions
#include <cstdio>
#include <utility>               // std::move
extern "C" {
#include "spatial_hash.h"        // sh_query_radius_nearest
}

GroupMeshResult MarchingCubesAlgorithm::generate(const MeshContext& ctx) const {
    GroupMeshResult result;
    result.group_id = ctx.group_id;

    // A fat-primitive-only group (e.g. a pure box part) has no spheres but still
    // has surface to mesh, so only bail when there is nothing at all to sample.
    if (ctx.particles.empty() && ctx.fat_count == 0) return result;

    Particle* particles = ctx.particles.empty()
        ? nullptr : const_cast<Particle*>(ctx.particles.data());
    int particleCount = (int)ctx.particles.size();
    Particle* clip = const_cast<Particle*>(ctx.clip);
    Particle* carve = const_cast<Particle*>(ctx.carve);

    // Ordered-CSG / typed iso-primitive path: when the context carries an ordered
    // stage list or any fat primitive, drive the staged field eval. Otherwise the
    // legacy single-union mesher runs (byte-identical hot path). GenerateMeshStaged
    // internally falls back to the legacy path when field_needs_staging is false.
    const bool staged = (ctx.fat_count > 0) ||
                        (ctx.stages != nullptr && ctx.stages->stageCount > 1);

    Mesh mesh = staged
        ? GenerateMeshStaged(ctx.scratch, particles, ctx.max_radius, particleCount,
                             ctx.bounds, ctx.blend_width, ctx.stages, ctx.fat, ctx.fat_count,
                             clip, ctx.clip_count, carve, ctx.carve_count, ctx.carve_blend)
        : GenerateMeshWithScratch(ctx.scratch, particles, ctx.max_radius, particleCount,
                             ctx.bounds, ctx.blend_width, clip, ctx.clip_count,
                             carve, ctx.carve_count, ctx.carve_blend);

    if (ctx.simplification_ratio < 1.0f && mesh.vertexCount > 0 && mesh.triangleCount > 0) {
        SimplifyOptions so;
        so.target_ratio = ctx.simplification_ratio;
        so.lock_boundary = true;
        CellBounds cb = ctx.cell_bounds;
        Mesh simplified = simplify_mesh(mesh, so, &cb);
        if (simplified.vertexCount > 0 && simplified.triangleCount > 0) {
            ComputeSurfaceNormalsWithScratch(ctx.scratch, &simplified, particles, ctx.max_radius,
                                  particleCount, ctx.blend_width, clip, ctx.clip_count,
                                  carve, ctx.carve_count, ctx.carve_blend);
            unload_cpu_mesh(mesh);
            mesh = simplified;
        } else {
            unload_cpu_mesh(simplified);
        }
    }

    if (mesh.vertexCount <= 0) return result;

    std::vector<TriEx> triangle_normals;
    std::vector<Tri> triangles = convert_mesh_to_triangles(mesh, &triangle_normals);

    SpatialHash* tri_hash = SurfaceScratchHash(ctx.scratch);
    float tri_search = ctx.max_radius * 2.5f + ctx.blend_width * 4.0f;
    for (size_t t = 0; t < triangle_normals.size() && t < triangles.size(); ++t) {
        const float3& c = triangles[t].centroid;
        if (particleCount > 0) {
            int bestIdx = 0;
            Particle* nearest = NULL;
            int nfound = tri_hash
                ? sh_query_radius_nearest(tri_hash, c.x, c.y, c.z, tri_search, (void**)&nearest, 1)
                : 0;
            if (nfound > 0 && nearest) {
                bestIdx = (int)(nearest - particles);
            }
            triangle_normals[t].materialId = particles[bestIdx].materialId;
            triangle_normals[t].tint = ctx.particle_tints[bestIdx];
        } else if (ctx.fat_count > 0) {
            // Fat-primitive-only group: tag each triangle from the nearest fat
            // primitive (by bounding-sphere center) since there are no spheres.
            int bestIdx = 0; float bestD2 = 1e30f;
            for (int j = 0; j < ctx.fat_count; ++j) {
                float dx = c.x - ctx.fat[j].center.x;
                float dy = c.y - ctx.fat[j].center.y;
                float dz = c.z - ctx.fat[j].center.z;
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < bestD2) { bestD2 = d2; bestIdx = j; }
            }
            triangle_normals[t].materialId = ctx.fat[bestIdx].materialId;
            const Vector4& ft = ctx.fat[bestIdx].tint;
            triangle_normals[t].tint = make_float4(ft.x, ft.y, ft.z, ft.w);
        }
    }

    result.mesh = mesh;
    result.triangles = std::move(triangles);
    result.triangle_normals = std::move(triangle_normals);
    return result;
}
