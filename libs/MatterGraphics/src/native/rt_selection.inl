std::vector<uint32_t> dense_rt_lod_offsets(const VkScenePart& part) {
    std::vector<uint32_t> offsets;
    offsets.reserve(part.clusters.size() + 1);
    uint32_t total = 0;
    offsets.push_back(total);
    for (const VkSceneCluster& cluster : part.clusters) {
        total += static_cast<uint32_t>(cluster.lods.size());
        offsets.push_back(total);
    }
    return offsets;
}

bool dense_rt_lod_index(const std::vector<uint32_t>& offsets,
                        uint32_t cluster_index, uint32_t lod_index,
                        uint32_t& record_index) noexcept {
    if (cluster_index + 1 >= offsets.size()) return false;
    const uint32_t begin = offsets[cluster_index];
    const uint32_t end = offsets[cluster_index + 1];
    if (lod_index >= end - begin) return false;
    record_index = begin + lod_index;
    return true;
}

bool lod_is_billboard(const VkScenePart& part,
                      const VkSceneLod& lod) noexcept {
    // Two triangles, and the first vertex they index carries the marker.
    // Same threshold adopt_part_impostors patches by (kQuadMarker is 1e30,
    // the shaders compare against 1e29), so all three agree by construction.
    if (lod.index_count != 6) return false;
    if (part.indices.empty() || part.vertices.empty()) return false;
    if (lod.first_index >= part.indices.size()) return false;
    const uint32_t vertex = part.indices[lod.first_index];
    if (vertex >= part.vertices.size()) return false;
    return part.vertices[vertex].surface.x > 1.0e29f;
}

uint32_t cluster_mesh_lod_count(const VkScenePart& part,
                                uint32_t cluster_index) noexcept {
    if (cluster_index >= part.clusters.size()) return 0;
    const std::vector<VkSceneLod>& lods = part.clusters[cluster_index].lods;
    uint32_t count = static_cast<uint32_t>(lods.size());
    // Trailing, not "any": part_flatten appends the billboard AFTER the mesh
    // ladder bottoms out, so the mesh rungs are always the leading prefix.
    // Peeling from the back keeps a hypothetical future second billboard
    // rung out too without ever reordering the ladder.
    while (count > 0 && lod_is_billboard(part, lods[count - 1])) --count;
    return count;
}

std::vector<RtGeometrySelection> select_rt_instance_geometry(
    const VkScenePart& part, const matter::Mat4f& object_to_world,
    matter::Float3 camera_eye, float pixel_budget) {
    std::vector<RtGeometrySelection> result;
    result.reserve(part.clusters.size());
    for (uint32_t cluster_index = 0; cluster_index < part.clusters.size();
         ++cluster_index) {
        const VkSceneCluster& cluster = part.clusters[cluster_index];
        if (cluster.lods.empty()) continue;
        // M2.5: a cluster whose selected rung is the terminal billboard
        // contributes NO traced geometry -- see build_ray_geometry for the
        // measurement behind dropping it rather than clamping down to the
        // last mesh rung. Same rule for a cluster with no mesh rung at all.
        const uint32_t mesh_lods = cluster_mesh_lod_count(part, cluster_index);
        const uint32_t lod_index = select_scene_cluster_lod(
            cluster, object_to_world, camera_eye, pixel_budget);
        if (lod_index >= mesh_lods) continue;
        const VkSceneLod& lod = cluster.lods[lod_index];
        result.push_back(
            {cluster_index, lod_index, lod.first_index, lod.index_count});
    }
    return result;
}
