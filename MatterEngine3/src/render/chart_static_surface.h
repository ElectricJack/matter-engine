#pragma once

#include "vk_scene_renderer.h"
#include "vt_snapshot.h"
#include "../lod_bake.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>

namespace viewer {

// Runtime geometry with one cluster and one complete rung (terrain joining
// strips, for example). Reuse the ordinary chart builder and preserve every
// triangle, normal and material. Split corners because chart cuts need distinct
// UVs even when the caller shares a position. Failure leaves the part unchanged.
inline bool chart_static_surface(VkScenePart& part, float texels_per_meter,
                                 std::string& error) {
    error.clear();
    if (part.clusters.size() != 1 || part.clusters[0].lods.size() != 1 ||
        part.clusters[0].lods[0].first_index != 0 ||
        part.clusters[0].lods[0].index_count != part.indices.size() ||
        part.indices.empty() || part.indices.size() % 3 != 0 ||
        part.indices.size() > std::numeric_limits<uint32_t>::max() ||
        part.indices.size() / 3 > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        !part.lod_charts.empty() || !part.lod_chart_meshes.empty() ||
        !std::isfinite(texels_per_meter) || texels_per_meter <= 0) {
        error = "static surface requires an uncharted, complete single rung and finite density";
        return false;
    }
    for (uint32_t index : part.indices) {
        if (index >= part.vertices.size()) {
            error = "static surface index is outside its vertices";
            return false;
        }
        const auto& p = part.vertices[index].position;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            error = "static surface has a nonfinite position";
            return false;
        }
    }
    const size_t count = part.indices.size() / 3;
    std::vector<Tri> tris(count);
    std::vector<TriEx> extra(count);
    for (size_t t = 0; t < count; ++t) {
        float3* corners[] = {&tris[t].vertex0, &tris[t].vertex1, &tris[t].vertex2};
        for (size_t k = 0; k < 3; ++k) {
            const auto& p = part.vertices[part.indices[t * 3 + k]].position;
            *corners[k] = make_float3(p.x, p.y, p.z);
        }
    }
    chart_atlas::ChartAtlasRung atlas;
    if (!lod_bake::build_chart_rung(tris, extra, texels_per_meter,
            chart_atlas::kChartNormalConeDeg, atlas)) {
        error = "static surface chart packing failed";
        return false;
    }

    std::vector<VkRasterVertex> vertices;
    std::vector<uint32_t> indices;
    VkScenePartChartMesh mesh;
    vertices.reserve(part.indices.size());
    indices.reserve(part.indices.size());
    mesh.positions.reserve(part.indices.size() * 3);
    mesh.normals.reserve(part.indices.size() * 3);
    mesh.surface_uvs.reserve(part.indices.size() * 2);
    mesh.material_ids.reserve(part.indices.size());
    for (size_t t = 0; t < count; ++t) {
        const float2 uv[] = {extra[t].uv0, extra[t].uv1, extra[t].uv2};
        for (size_t k = 0; k < 3; ++k) {
            auto v = part.vertices[part.indices[t * 3 + k]];
            v.surface.x = uv[k].x;
            v.surface.y = uv[k].y;
            indices.push_back(static_cast<uint32_t>(vertices.size()));
            vertices.push_back(v);
            mesh.positions.insert(mesh.positions.end(), {v.position.x, v.position.y, v.position.z});
            mesh.normals.insert(mesh.normals.end(), {v.normal.x, v.normal.y, v.normal.z});
            mesh.surface_uvs.insert(mesh.surface_uvs.end(), {v.surface.x, v.surface.y});
            mesh.material_ids.push_back(v.material_index);
        }
    }
    mesh.indices = indices;
    mesh.vertex_count = static_cast<uint32_t>(vertices.size());
    mesh.dominant_material = mesh.material_ids.front();
    // Prepare allocations before committing, including the one-element arrays.
    std::vector<chart_atlas::ChartAtlasRung> atlases;
    std::vector<VkScenePartChartMesh> meshes;
    atlases.push_back(std::move(atlas));
    meshes.push_back(std::move(mesh));
    part.vertices = std::move(vertices);
    part.indices = std::move(indices);
    part.lod_charts = std::move(atlases);
    part.lod_chart_meshes = std::move(meshes);
    part.clusters[0].lods[0].chart_rung = 0;
    return true;
}

// Retain the chart geometry for demand registration without keeping a second
// VkScenePart and its raster buffers alive. Material registry data is supplied
// fresh by the request service. Subsequent recipe edits share this geometry
// through VtPartSnapshot::with_surface().
inline std::shared_ptr<const vt::VtPartSnapshot> capture_static_surface(
    const VkScenePart& part) {
    if (part.lod_charts.size() != 1 || part.lod_chart_meshes.size() != 1 ||
        part.lod_charts[0].charts.empty()) return {};
    const auto& mesh = part.lod_chart_meshes[0];
    vt::VtPartContext context;
    context.variant_hash = part.part_hash;
    context.rung_count = 1;
    context.positions = mesh.positions.data();
    context.normals = mesh.normals.data();
    context.surface_uvs = mesh.surface_uvs.data();
    context.material_ids = mesh.material_ids.data();
    context.vertex_count = mesh.vertex_count;
    context.indices = mesh.indices.data();
    context.triangle_count = static_cast<uint32_t>(mesh.indices.size() / 3);
    context.dominant_material = mesh.dominant_material;
    context.surface_weights = mesh.surface_weights.empty() ? nullptr : mesh.surface_weights.data();
    context.surface_materials = part.surface_materials.empty() ? nullptr : part.surface_materials.data();
    context.surface_material_count = static_cast<uint32_t>(part.surface_materials.size());
    context.surface_tape_hash = part.surface_tape_hash;
    context.surface_tape_text = part.surface_tape_text.empty() ? nullptr : part.surface_tape_text.c_str();
    context.surface_lanes = mesh.surface_lanes.empty() ? nullptr : mesh.surface_lanes.data();
    context.surface_lane_count = mesh.surface_lane_count;
    context.surface_world_anchored = part.surface_world_anchored;
    std::copy(std::begin(part.surface_local_to_world), std::end(part.surface_local_to_world),
              context.surface_local_to_world);
    return vt::VtPartSnapshot::capture(part.lod_charts[0], context);
}

} // namespace viewer
