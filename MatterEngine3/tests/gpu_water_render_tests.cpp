#include "check.h"

#include "render/gpu_meshing/water_scene_part.h"

extern "C" {
#include "material_registry.h"
}

#include <cmath>
#include <limits>
#include <memory>

namespace {

gpu_meshing::MeshResult triangle_mesh() {
    gpu_meshing::MeshResult mesh{};
    mesh.positions = {-2.0f, 3.0f, 1.0f, 4.0f, 3.0f, 1.0f,
                      -2.0f, 7.0f, 1.0f};
    mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                    0.0f, 0.0f, 1.0f};
    mesh.indices = {0u, 1u, 2u};
    mesh.material = 4u;
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
    return mesh;
}

std::uint32_t dynamic_water_material() {
    MaterialRegistryResetDynamic();
    MaterialDef material{};
    MaterialRegistryDefaultDynamicDef(&material);
    material.surfaceFlags |= MATERIAL_WATER_SURFACE;
    const int id = MaterialRegistryDefineDynamic(&material, "GpuWaterTest");
    CHECK(id >= MaterialRegistryStaticCount(),
          "test water receives a dynamic material id");
    return static_cast<std::uint32_t>(id);
}

void test_converts_to_one_authored_water_part() {
    const auto mesh = triangle_mesh();
    const std::uint32_t water_material = dynamic_water_material();
    std::shared_ptr<const viewer::VkScenePart> part;
    uint64_t instance_id = 0;
    gpu_meshing::Error error{};
    const viewer::WaterFieldBinding field_binding{3u, 17u};
    CHECK(gpu_meshing::build_water_scene_part(
              mesh, 0x123456789abcdef0ull, water_material,
              part, instance_id, error, field_binding),
          error.message.c_str());
    CHECK(part && part->part_hash != 0u && instance_id != 0u &&
              instance_id != part->part_hash,
          "water conversion derives distinct nonzero stable identities");
    CHECK(part->clusters.size() == 1u &&
              part->clusters[0].lods.size() == 1u &&
              part->clusters[0].lods[0].first_index == 0u &&
              part->clusters[0].lods[0].index_count == mesh.indices.size(),
          "water conversion produces one cluster and one LOD");
    CHECK(part->water_field_binding.slot == field_binding.slot &&
              part->water_field_binding.generation == field_binding.generation,
          "water conversion preserves its explicit immutable field binding");
    CHECK(part->raster_water_surface,
          "water conversion classifies the immutable proxy for forward raster");
    CHECK(!viewer::VkScenePart{}.raster_water_surface,
          "ordinary scene parts remain opaque by default");
    CHECK(part->vertices.size() == 3u && part->indices == mesh.indices,
          "water conversion retains exact indexed topology");
    for (size_t i = 0; i < part->vertices.size(); ++i) {
        const auto& vertex = part->vertices[i];
        CHECK(vertex.position.x == mesh.positions[i * 3u + 0u] &&
                  vertex.position.y == mesh.positions[i * 3u + 1u] &&
                  vertex.position.z == mesh.positions[i * 3u + 2u] &&
                  vertex.normal.x == mesh.normals[i * 3u + 0u] &&
                  vertex.normal.y == mesh.normals[i * 3u + 1u] &&
                  vertex.normal.z == mesh.normals[i * 3u + 2u],
              "water conversion retains exact positions and normals");
        CHECK(vertex.material_index == water_material && vertex.tint.x == 1.0f &&
                  vertex.tint.y == 1.0f && vertex.tint.z == 1.0f &&
                  vertex.surface.z == 1.0f && vertex.surface.w == 1.0f,
              "water conversion selects the resolved authored water material");
    }
    CHECK(part->clusters[0].aabb_min.x == -2.0f &&
              part->clusters[0].aabb_min.y == 3.0f &&
              part->clusters[0].aabb_min.z == 1.0f &&
              part->clusters[0].aabb_max.x == 4.0f &&
              part->clusters[0].aabb_max.y == 7.0f &&
              part->clusters[0].aabb_max.z == 1.0f,
          "water conversion computes exact bounds");
    CHECK(std::fabs(part->clusters[0].radius -
                    0.5f * std::sqrt(52.0f)) < 1e-6f,
          "water conversion computes cluster radius from its diagonal");

    viewer::VkSceneInstance proxy{};
    proxy.part_hash = part->part_hash;
    proxy.instance_id = instance_id;
    proxy.ray_traced = true;
    gpu_meshing::set_water_scene_animation_active(proxy, false);
    CHECK(!proxy.rt_proxy_only && !proxy.ray_traced,
          "accepted proxy without a direct frame remains the raster-only fallback");
    gpu_meshing::set_water_scene_animation_active(proxy, true);
    CHECK(proxy.rt_proxy_only && !proxy.ray_traced &&
              proxy.part_hash == part->part_hash &&
              proxy.instance_id == instance_id,
          "healthy animation suppresses only the raster proxy and keeps water out of RT");
    gpu_meshing::set_water_scene_animation_active(proxy, false);
    CHECK(!proxy.rt_proxy_only && !proxy.ray_traced &&
              proxy.part_hash == part->part_hash &&
              proxy.instance_id == instance_id,
          "fallback restores raster visibility while water remains raster-only");

    std::shared_ptr<const viewer::VkScenePart> repeated;
    uint64_t repeated_instance = 0;
    CHECK(gpu_meshing::build_water_scene_part(
              mesh, 0x123456789abcdef0ull, water_material,
              repeated, repeated_instance, error) &&
              repeated->part_hash == part->part_hash &&
              repeated_instance == instance_id,
          "artifact digest yields stable part and instance identities");

    std::shared_ptr<const viewer::VkScenePart> rejected = part;
    std::uint64_t rejected_instance = instance_id;
    CHECK(!gpu_meshing::build_water_scene_part(
              mesh, 0x123456789abcdef0ull, 4u,
              rejected, rejected_instance, error) &&
              rejected == part && rejected_instance == instance_id,
          "ordinary unflagged glass is rejected transactionally");

    std::shared_ptr<const viewer::VkScenePart> builtin;
    std::uint64_t builtin_instance = 0;
    CHECK(gpu_meshing::build_water_scene_part(
              mesh, 0x123456789abcdef0ull, 7u,
              builtin, builtin_instance, error) && builtin,
          "the explicitly flagged builtin compatibility water remains valid");
}

void test_dry_omission_and_malformed_transactionality() {
    gpu_meshing::MeshResult dry{};
    dry.material = 4u;
    dry.content_digest = gpu_meshing::mesh_content_digest(dry);
    std::shared_ptr<const viewer::VkScenePart> part =
        std::make_shared<viewer::VkScenePart>();
    uint64_t instance_id = 99u;
    gpu_meshing::Error error{};
    CHECK(gpu_meshing::build_water_scene_part(dry, 10u, 7u, part, instance_id,
                                              error) &&
              !part && instance_id == 0u,
          "dry water mesh is omitted without an empty renderer part");

    const auto old = std::make_shared<viewer::VkScenePart>();
    old->part_hash = 77u;
    part = old;
    instance_id = 88u;
    auto malformed = triangle_mesh();
    malformed.indices[2] = 99u;
    CHECK(!gpu_meshing::build_water_scene_part(malformed, 11u, 7u, part,
                                               instance_id, error) &&
              part == old && instance_id == 88u,
          "invalid replacement leaves the previously accepted binding intact");
    malformed = triangle_mesh();
    malformed.normals.pop_back();
    CHECK(!gpu_meshing::build_water_scene_part(malformed, 11u, 7u, part,
                                               instance_id, error),
          "mismatched position and normal streams are rejected");
    malformed = triangle_mesh();
    malformed.positions[0] = std::numeric_limits<float>::infinity();
    CHECK(!gpu_meshing::build_water_scene_part(malformed, 11u, 7u, part,
                                               instance_id, error),
          "non-finite renderer geometry is rejected");
}

}  // namespace

int main() {
    test_converts_to_one_authored_water_part();
    test_dry_omission_and_malformed_transactionality();
    return check_summary();
}
