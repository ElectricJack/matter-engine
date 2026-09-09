#include "water_scene_part.h"

extern "C" {
#include "material_registry.h"
}

#include <algorithm>
#include <cmath>
#include <limits>

namespace gpu_meshing {
namespace {

bool fail(Error& error, const char* message) {
    error.code = ErrorCode::InvalidInput;
    error.message = message;
    return false;
}

std::uint64_t stable_identity(std::uint64_t digest, std::uint64_t domain,
                              std::uint32_t material_id) {
    std::uint64_t result = 1469598103934665603ull;
    for (std::uint64_t value : {domain, digest,
                                static_cast<std::uint64_t>(material_id)}) {
        for (unsigned shift = 0; shift != 64; shift += 8) {
            result ^= static_cast<std::uint8_t>(value >> shift);
            result *= 1099511628211ull;
        }
    }
    return result == 0 ? domain | 1u : result;
}

bool validate_mesh(const MeshResult& mesh) {
    if (mesh.positions.size() != mesh.normals.size() ||
        mesh.positions.size() % 3u != 0u || mesh.indices.size() % 3u != 0u ||
        mesh.content_digest != mesh_content_digest(mesh))
        return false;
    for (float value : mesh.positions)
        if (!std::isfinite(value)) return false;
    for (float value : mesh.normals)
        if (!std::isfinite(value)) return false;
    const std::size_t vertices = mesh.positions.size() / 3u;
    for (std::uint32_t index : mesh.indices)
        if (index >= vertices) return false;
    return true;
}

}  // namespace

bool build_water_scene_part(
    const MeshResult& mesh, std::uint64_t artifact_digest,
    std::uint32_t material_id,
    std::shared_ptr<const viewer::VkScenePart>& part,
    std::uint64_t& instance_id, Error& error,
    viewer::WaterFieldBinding water_field_binding) {
    error = {};
    if (artifact_digest == 0u)
        return fail(error, "water visual artifact digest must be nonzero");
    const MaterialDef* material =
        material_id < static_cast<std::uint32_t>(MaterialRegistryCount())
            ? MaterialRegistryGet(static_cast<int>(material_id)) : nullptr;
    if (!material ||
        (material->surfaceFlags & MATERIAL_WATER_SURFACE) == 0u)
        return fail(error, "water visual material must declare the water-surface domain");
    if (!validate_mesh(mesh))
        return fail(error, "water visual mesh geometry is invalid");
    if (mesh.positions.empty()) {
        part.reset();
        instance_id = 0u;
        return true;
    }

    auto candidate = std::make_shared<viewer::VkScenePart>();
    candidate->part_hash = stable_identity(
        artifact_digest, 0x5741544552504152ull, material_id);
    candidate->water_field_binding = water_field_binding;
    candidate->raster_water_surface = true;
    candidate->vertices.reserve(mesh.positions.size() / 3u);
    candidate->indices = mesh.indices;
    matter::Float3 minimum{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    matter::Float3 maximum{
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()};
    for (std::size_t vertex = 0; vertex != mesh.positions.size() / 3u;
         ++vertex) {
        viewer::VkRasterVertex output{};
        output.position = {mesh.positions[vertex * 3u + 0u],
                           mesh.positions[vertex * 3u + 1u],
                           mesh.positions[vertex * 3u + 2u]};
        output.normal = {mesh.normals[vertex * 3u + 0u],
                         mesh.normals[vertex * 3u + 1u],
                         mesh.normals[vertex * 3u + 2u]};
        output.tint = {1.0f, 1.0f, 1.0f, 0.0f};
        output.surface = {0.0f, 0.0f, 1.0f, 1.0f};
        output.material_index = material_id;
        candidate->vertices.push_back(output);
        minimum.x = std::min(minimum.x, output.position.x);
        minimum.y = std::min(minimum.y, output.position.y);
        minimum.z = std::min(minimum.z, output.position.z);
        maximum.x = std::max(maximum.x, output.position.x);
        maximum.y = std::max(maximum.y, output.position.y);
        maximum.z = std::max(maximum.z, output.position.z);
    }
    viewer::VkSceneCluster cluster{};
    cluster.aabb_min = minimum;
    cluster.aabb_max = maximum;
    const float dx = maximum.x - minimum.x;
    const float dy = maximum.y - minimum.y;
    const float dz = maximum.z - minimum.z;
    cluster.radius =
        std::max(0.001f, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
    cluster.lods.push_back(
        {0u, static_cast<std::uint32_t>(candidate->indices.size()), 0.0f,
         UINT32_MAX});
    candidate->clusters.push_back(std::move(cluster));

    const std::uint64_t candidate_instance = stable_identity(
        artifact_digest, 0x5741544552494e53ull, material_id);
    part = std::move(candidate);
    instance_id = candidate_instance;
    return true;
}

void set_water_scene_animation_active(
    viewer::VkSceneInstance& proxy, bool active) noexcept {
    proxy.rt_proxy_only = active;
    proxy.ray_traced = false;
}

}  // namespace gpu_meshing
