#pragma once
#include "geometry/geometry_hierarchy.h"
#include "asset_binary.h"
#include "vk_scene_renderer.h"
#include <array>
#include <cmath>
#include <cstring>


namespace viewer {
// Ordinary indexed-mesh adapter for the hierarchy reference path and page
// uploads. Stable source UV/material attributes survive each independently
// decoded group. This does not construct per-rung VT charts or bake materials.
inline bool build_geometry_page_part(uint64_t part_hash, const geometry::NodeView& page,
                                     VkScenePart& out, std::string& error) {
    geometry::NodeView validated;
    if (!part_hash || !geometry::decode_node(page.page, validated, error)) return false;
    // Geometry page schema 1 stores positions, indices, and per-triangle
    // shading in sections 2/3/4. Read validated lanes directly: constructing a
    // MeshIndexed here duplicates every index and expands shading into TriEx
    // only to discard those allocations immediately after GPU conversion.
    const auto& positions = *validated.page->view.find(2);
    const auto& indices = *validated.page->view.find(3);
    const auto& shading = *validated.page->view.find(4);
    const auto f32 = [](const uint8_t* p) {
        const uint32_t bits = asset_store::get_u32(p);
        float value; std::memcpy(&value, &bits, sizeof(value)); return value;
    };
    VkScenePart part; part.part_hash = part_hash;
    // Flat, page-local deduplication: one index table rather than a tree node
    // allocation for each unique vertex. First occurrence still defines the
    // output index, so page bytes and GPU draw order remain deterministic.
    size_t table_size = 1;
    while (table_size < indices.count * 2) table_size <<= 1;
    std::vector<uint32_t> vertices(table_size, UINT32_MAX);
    part.vertices.reserve(indices.count);
    part.indices.reserve(indices.count);
    for (size_t t = 0; t < shading.count; ++t) {
        const auto* row = shading.data + t * shading.stride;
        const uint32_t material = asset_store::get_u32(row + 72);
        for (size_t c = 0; c < 3; ++c) {
            const uint32_t index = asset_store::get_u32(indices.data + (t * 3 + c) * 4);
            const auto* position = positions.data + size_t(index) * positions.stride;
            const auto* corner = row + c * 24;
            VkRasterVertex vertex{};
            vertex.position = {f32(position), f32(position + 4), f32(position + 8)};
            vertex.normal = {f32(corner + 8), f32(corner + 12), f32(corner + 16)};
            vertex.tint = {f32(row + 76), f32(row + 80), f32(row + 84), f32(row + 88)};
            vertex.material_index = material;
            vertex.surface = {f32(corner), f32(corner + 4), f32(corner + 20), material != UINT32_MAX ? 1.0f : 0.0f};
            std::array<uint32_t, sizeof(VkRasterVertex) / 4> key{};
            std::memcpy(key.data(), &vertex, sizeof(vertex));
            uint64_t hash = 14695981039346656037ull;
            for (uint32_t word : key) { hash ^= word; hash *= 1099511628211ull; }
            hash ^= hash >> 32;
            size_t slot = static_cast<size_t>(hash) & (table_size - 1);
            while (vertices[slot] != UINT32_MAX &&
                   std::memcmp(&part.vertices[vertices[slot]], &vertex, sizeof(vertex)) != 0)
                slot = (slot + 1) & (table_size - 1);
            if (vertices[slot] == UINT32_MAX) {
                vertices[slot] = static_cast<uint32_t>(part.vertices.size());
                part.vertices.push_back(vertex);
            }
            part.indices.push_back(vertices[slot]);
        }
    }
    VkSceneCluster cluster;
    const auto& bounds = validated.self.bounds;
    cluster.aabb_min = {bounds.lo[0], bounds.lo[1], bounds.lo[2]};
    cluster.aabb_max = {bounds.hi[0], bounds.hi[1], bounds.hi[2]};
    const float x = bounds.hi[0] - bounds.lo[0], y = bounds.hi[1] - bounds.lo[1], z = bounds.hi[2] - bounds.lo[2];
    cluster.radius = .5f * std::sqrt(x*x + y*y + z*z);
    VkSceneLod lod; lod.first_index = 0; lod.index_count = static_cast<uint32_t>(part.indices.size());
    cluster.lods.push_back(lod); part.clusters.push_back(std::move(cluster));
    out = std::move(part); error.clear(); return true;
}
} // namespace viewer
