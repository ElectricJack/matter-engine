// tileset_bake_vk.cpp — Vulkan hardware-RT .gtex bake orchestrator (V1).
//
// See tileset_bake_vk.h and MatterEngine3/docs/vulkan-rt-gtex-bake.md §I.4/§II.1.
// V1 delivers the PRIMARY pass only: geometry lift from the CPU-side BLAS/TLAS
// entries assemble_torus_bvh builds -> staging upload -> bake-only BLAS(es)+TLAS
// -> tileset_bake_primary.comp dispatch -> readback -> repack -> save_gtex.
//
// This is a viewer-only translation unit (compiled into WIN_ME3_CPP under
// -DMATTER_VULKAN_ONLY); the headless kernel library never sees it.

// Keep windows.h (pulled in by vulkan_win32.h and by raylib.h, reached here via
// blas_manager.hpp) from declaring GDI/USER symbols that collide with raylib.
// Same pattern as vk_volumetrics.cpp / part_asset_v2.cpp.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#endif

#include "tileset_bake_vk.h"

#include "tileset_bake.h"        // SettledTorus, BakeInputs, SettledInstance
#include "tileset_spec.h"        // TileConfig
#include "tileset_layout.h"      // kTorusN
#include "tileset_gtex.h"        // save_gtex, gtex_cache_hit, gtex_content_hash, GTexHeader
#include "tileset_torus_bvh.h"   // assemble_torus_bvh

#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "material_registry.h"

#include "matter/vulkan_device.h"
#include "vk_resources.h"
#include "shaders_gen/embedded_spirv.h"

// Declarations only (stbi_write_png). The STB implementation lives in
// tileset_gtex.cpp; the viewer defines TILESET_GTEX_USE_RAYLIB_STB.
#include "external/stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace tileset {

namespace {

// ---------------------------------------------------------------------------
// Height range + ray origin — copied verbatim from tileset_bake_gpu.cpp so the
// Vulkan bake's ray_y / height_min / height_max match the GL bake exactly.
// ---------------------------------------------------------------------------
void compute_height_range(const SettledTorus& st, float& hmin, float& hmax,
                          float& max_instance_top) {
    hmin = 1e30f; hmax = -1e30f;
    max_instance_top = -1e30f;
    for (float y : st.base.heights) {
        if (y < hmin) hmin = y;
        if (y > hmax) hmax = y;
    }
    for (const SettledInstance& si : st.instances) {
        float y  = si.pose.py;
        float lo = y - 0.5f * si.scale;
        float hi = y + 0.5f * si.scale;
        if (lo < hmin) hmin = lo;
        if (hi > hmax) hmax = hi;
        float top = y + 2.0f * si.scale;
        if (top > max_instance_top) max_instance_top = top;
    }
    if (hmin > hmax) { hmin = 0.0f; hmax = 1.0f; }
    if (max_instance_top < hmax) max_instance_top = hmax;
    hmax += 0.05f;
    hmin -= 0.05f;
}

bool dump_pngs(const std::string& base, int W, int H,
               const std::vector<uint8_t>& a, const std::vector<uint8_t>& n,
               const std::vector<uint8_t>& o, const std::vector<uint16_t>& h) {
    std::string p = base + "-albedo.png";
    if (!stbi_write_png(p.c_str(), W, H, 3, a.data(), W * 3)) return false;
    p = base + "-normal.png";
    if (!stbi_write_png(p.c_str(), W, H, 2, n.data(), W * 2)) return false;
    p = base + "-orm.png";
    if (!stbi_write_png(p.c_str(), W, H, 3, o.data(), W * 3)) return false;
    std::vector<uint8_t> h8(h.size());
    for (size_t i = 0; i < h.size(); ++i) h8[i] = (uint8_t)(h[i] >> 8);
    p = base + "-height.png";
    if (!stbi_write_png(p.c_str(), W, H, 1, h8.data(), W)) return false;
    return true;
}

// Push constants — must match tileset_bake_primary.comp's Push block (24 bytes).
struct PrimaryPush {
    int32_t atlasW;
    int32_t atlasH;
    int32_t texelsPerMeter;
    float   rayY;
    float   heightMin;
    float   heightMax;
};

// Push constants — must match tileset_bake_ao.comp's Push block (36 bytes).
// Extends PrimaryPush with the AO trace parameters (seed/aoSamples/maxRayDist),
// sourced exactly as the GL driver does: seed = (uint32_t)cfg.seed,
// aoSamples = 64, maxRayDist = cfg.edge_strip_width (tileset_bake_gpu.cpp:200-202,
// tileset_bake_ao.cpp:111-113).
struct AoPush {
    int32_t  atlasW;
    int32_t  atlasH;
    int32_t  texelsPerMeter;
    float    rayY;
    float    heightMin;
    float    heightMax;
    uint32_t seed;
    int32_t  aoSamples;
    float    maxRayDist;
};

// A bake-only BLAS: single-use vertex/index buffers + acceleration structure.
struct BakeBlas {
    matter::VkBufferResource vertices;   // vec3 positions, 3 per triangle
    matter::VkBufferResource indices;    // uint32, 0..3N-1
    matter::VkAccelerationStructureResource as;
    matter::VkBufferResource scratch;
    uint32_t primitive_count = 0;
    uint32_t vertex_count = 0;
};

// Everything the immediate-submit record callback needs, in stable storage.
struct RecordContext {
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build = nullptr;
    // BLAS builds (pointers into the vectors below stay valid for the submit).
    std::vector<VkAccelerationStructureGeometryKHR> blas_geoms;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> blas_builds;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> blas_ranges;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> blas_range_ptrs;
    // TLAS build.
    VkAccelerationStructureGeometryKHR tlas_geom{};
    VkAccelerationStructureBuildGeometryInfoKHR tlas_build{};
    VkAccelerationStructureBuildRangeInfoKHR tlas_range{};
    // Dispatch (primary).
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    PrimaryPush push{};
    // Dispatch (AO, V2). Same TLAS + geometry SSBOs, own pipeline/set + the R8
    // AO output image (images[4]).
    VkPipeline ao_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout ao_pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSet ao_descriptor_set = VK_NULL_HANDLE;
    AoPush ao_push{};
    uint32_t group_x = 0, group_y = 0;
    // Output images + their readback buffers. [0..3] = primary (albedo/normal/
    // orm/height); [4] = AO (R8).
    matter::VkImageResource* images[5] = {nullptr, nullptr, nullptr, nullptr,
                                          nullptr};
    VkBuffer readbacks[5] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                             VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t width = 0, height = 0;
};

void record_bake(VkCommandBuffer cmd, void* user_data) {
    auto& c = *static_cast<RecordContext*>(user_data);

    // 1. Build all bake-only BLASes in one batch (disjoint per-BLAS scratch).
    if (!c.blas_builds.empty()) {
        c.cmd_build(cmd, static_cast<uint32_t>(c.blas_builds.size()),
                    c.blas_builds.data(), c.blas_range_ptrs.data());
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        b.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        b.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                          VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // 2. Build the TLAS.
    const VkAccelerationStructureBuildRangeInfoKHR* tlas_range_ptr = &c.tlas_range;
    c.cmd_build(cmd, 1, &c.tlas_build, &tlas_range_ptr);
    {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        b.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // 3. Transition all five output images UNDEFINED -> GENERAL for storage
    //    write (four primary + the R8 AO image).
    for (auto* img : c.images) {
        matter::record_image_transition(
            cmd, *img, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VkAccessFlags2(0),
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // 4. Dispatch the primary pass.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            c.pipeline_layout, 0, 1, &c.descriptor_set, 0,
                            nullptr);
    vkCmdPushConstants(cmd, c.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(PrimaryPush), &c.push);
    vkCmdDispatch(cmd, c.group_x, c.group_y, 1);

    // 4b. Dispatch the AO pass (V2). No barrier vs the primary dispatch: the two
    //     write disjoint images (primary -> images[0..3], AO -> images[4]) and
    //     only read the shared, read-only TLAS + geometry SSBOs, so there is no
    //     RAW/WAR/WAW hazard between them. Same grid (same W/H, 8x8 local size).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.ao_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            c.ao_pipeline_layout, 0, 1, &c.ao_descriptor_set, 0,
                            nullptr);
    vkCmdPushConstants(cmd, c.ao_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(AoPush), &c.ao_push);
    vkCmdDispatch(cmd, c.group_x, c.group_y, 1);

    // 5. Transition GENERAL -> TRANSFER_SRC and copy each image to its readback.
    for (int i = 0; i < 5; ++i) {
        matter::record_image_transition(
            cmd, *c.images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {c.width, c.height, 1};
        vkCmdCopyImageToBuffer(cmd, c.images[i]->image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               c.readbacks[i], 1, &copy);
    }
    // Barrier transfer-write -> host-read so readback_buffer sees complete data.
    VkMemoryBarrier2 hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    hb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo hdep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    hdep.memoryBarrierCount = 1;
    hdep.pMemoryBarriers = &hb;
    vkCmdPipelineBarrier2(cmd, &hdep);
}

bool vkfail(const char* op, VkResult r, std::string& err) {
    err = std::string(op) + " failed (VkResult " +
          std::to_string(static_cast<int>(r)) + ")";
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Orchestrator.
// ---------------------------------------------------------------------------
bool bake_tileset_vk(matter::VulkanDevice& vulkan, const SettledTorus& settled,
                     uint64_t script_source_hash,
                     const std::string& out_gtex_path, const BakeInputs& inputs,
                     bool force_rebake, bool dump_png, std::string& err) {
    try {
        // 1. Cache-hit check (same key scheme as the GL bake; version unchanged).
        const uint64_t expected = gtex_content_hash(
            settled.report.pose_hash, script_source_hash, kEngineBakeVersion,
            kBox3dVersion);
        if (!force_rebake && gtex_cache_hit(out_gtex_path, expected)) return true;

        // 2. Hardware ray tracing is required (fail-closed, like volumetrics).
        if (!vulkan.ray_tracing_available()) {
            err = "bake_tileset_vk: ray tracing unavailable: " +
                  vulkan.ray_tracing_unavailable_reason();
            return false;
        }
        const VkDevice device = vulkan.device();

        // 3. Assemble the CPU-side BLAS/TLAS (part loading, torus tessellation,
        //    instance transforms, per-triangle materials). V1 consumes the entry
        //    triangle arrays; the CPU BVH it also builds is unused (spec §I.4).
        BLASManager blas;
        const int n_settled = (int)settled.instances.size();
        const int tlas_cap = (n_settled + 1) * 5 / 4 + 32;
        TLASManager tlas(tlas_cap);
        if (!assemble_torus_bvh(settled, inputs, blas, tlas, err)) return false;

        // 4. Geometry lift: walk BLAS entries into global per-triangle SSBO data
        //    plus per-entry vertex/index buffers for the AS build.
        const auto& entries = blas.get_entries();
        if (entries.empty()) { err = "bake_tileset_vk: no BLAS entries"; return false; }

        std::unordered_map<BLASHandle, uint32_t> handle_to_index;
        std::vector<float> tri_data;   // 18 floats per triangle (v0..v2, n0..n2)
        std::vector<int32_t> tri_mat;  // per-triangle material id (-1 fallback)
        std::vector<uint32_t> tri_first(entries.size(), 0);
        std::vector<BakeBlas> bake_blas(entries.size());

        for (size_t ei = 0; ei < entries.size(); ++ei) {
            const BLASManager::BLASEntry& e = *entries[ei];
            handle_to_index[e.handle] = (uint32_t)ei;
            tri_first[ei] = (uint32_t)tri_mat.size();

            const size_t ntri = e.triangles.size();
            const bool have_ex = !e.tri_extra.empty();

            std::vector<float> verts;       // 3 * ntri vec3 positions (12B each)
            std::vector<uint32_t> idx(ntri * 3);
            verts.reserve(ntri * 9);
            tri_data.reserve(tri_data.size() + ntri * 18);
            tri_mat.reserve(tri_mat.size() + ntri);

            for (size_t ti = 0; ti < ntri; ++ti) {
                const Tri& t = e.triangles[ti];
                const float3 v0 = t.vertex0, v1 = t.vertex1, v2 = t.vertex2;
                // AS build vertices (positions only).
                verts.push_back(v0.x); verts.push_back(v0.y); verts.push_back(v0.z);
                verts.push_back(v1.x); verts.push_back(v1.y); verts.push_back(v1.z);
                verts.push_back(v2.x); verts.push_back(v2.y); verts.push_back(v2.z);
                idx[ti * 3 + 0] = (uint32_t)(ti * 3 + 0);
                idx[ti * 3 + 1] = (uint32_t)(ti * 3 + 1);
                idx[ti * 3 + 2] = (uint32_t)(ti * 3 + 2);

                // Per-vertex normals: smooth from TriEx where present, else the
                // face normal for all three (matches the GL decode's fallback).
                float3 n0, n1, n2;
                if (have_ex) {
                    n0 = e.tri_extra[ti].N0;
                    n1 = e.tri_extra[ti].N1;
                    n2 = e.tri_extra[ti].N2;
                } else {
                    const float3 a{v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
                    const float3 b{v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};
                    float3 fn{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                              a.x * b.y - a.y * b.x};
                    n0 = n1 = n2 = fn;
                }
                const float packed[18] = {
                    v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z,
                    n0.x, n0.y, n0.z, n1.x, n1.y, n1.z, n2.x, n2.y, n2.z};
                tri_data.insert(tri_data.end(), packed, packed + 18);
                tri_mat.push_back(have_ex ? (int32_t)e.tri_extra[ti].materialId
                                          : (int32_t)-1);
            }

            bake_blas[ei].primitive_count = (uint32_t)ntri;
            bake_blas[ei].vertex_count = (uint32_t)(ntri * 3);

            // Upload AS build vertex/index buffers (host visible + device address).
            const VkBufferUsageFlags as_input_usage =
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            if (!matter::create_buffer(
                    vulkan, verts.size() * sizeof(float), as_input_usage,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, bake_blas[ei].vertices,
                    err) ||
                !matter::upload_buffer(vulkan, bake_blas[ei].vertices,
                                       verts.data(), verts.size() * sizeof(float),
                                       0, err))
                return false;
            if (!matter::create_buffer(
                    vulkan, idx.size() * sizeof(uint32_t), as_input_usage,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, bake_blas[ei].indices,
                    err) ||
                !matter::upload_buffer(vulkan, bake_blas[ei].indices, idx.data(),
                                       idx.size() * sizeof(uint32_t), 0, err))
                return false;
        }

        // 5. TLAS instances + per-instance fallback materials.
        const auto& records = tlas.get_draw_records();
        if (records.empty()) { err = "bake_tileset_vk: no TLAS instances"; return false; }
        std::vector<VkAccelerationStructureInstanceKHR> instances;
        std::vector<int32_t> inst_mat;
        instances.reserve(records.size());
        inst_mat.reserve(records.size());
        for (const auto& r : records) {
            auto it = handle_to_index.find(r.blas_handle);
            if (it == handle_to_index.end()) continue;  // BLAS was refused
            const uint32_t blas_index = it->second;
            VkAccelerationStructureInstanceKHR inst{};
            // Matrix4x4 is row-major (m[row*4+col]); VkTransformMatrixKHR is a
            // 3x4 row-major matrix — copy the first three rows directly.
            for (uint32_t row = 0; row < 3; ++row)
                for (uint32_t col = 0; col < 4; ++col)
                    inst.transform.matrix[row][col] = r.transform.m[row * 4 + col];
            inst.instanceCustomIndex = blas_index & 0xFFFFFFu;
            inst.mask = 0xFF;
            inst.instanceShaderBindingTableRecordOffset = 0;
            inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            inst.accelerationStructureReference = bake_blas[blas_index].as.address;
            instances.push_back(inst);
            inst_mat.push_back((int32_t)r.material_id);
        }
        // bake_blas[*].as.address is filled below (create), so patch references
        // after the AS objects exist. Build the AS objects first, then fill.

        // 6. KHR entry points.
        const auto get_sizes = reinterpret_cast<
            PFN_vkGetAccelerationStructureBuildSizesKHR>(
            vkGetDeviceProcAddr(device,
                                "vkGetAccelerationStructureBuildSizesKHR"));
        const auto cmd_build = reinterpret_cast<
            PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(
            device, "vkCmdBuildAccelerationStructuresKHR"));
        if (!get_sizes || !cmd_build) {
            err = "bake_tileset_vk: AS build entry points unavailable";
            return false;
        }

        const VkDeviceSize scratch_align = std::max<VkDeviceSize>(
            1, vulkan.ray_tracing_properties()
                   .min_acceleration_structure_scratch_offset_alignment);
        auto align_up = [](VkDeviceSize v, VkDeviceSize a) {
            return (v + a - 1) / a * a;
        };

        // 7. Create BLAS acceleration structures + scratch, prepare build infos.
        RecordContext ctx;
        ctx.cmd_build = cmd_build;
        ctx.blas_geoms.resize(entries.size());
        ctx.blas_builds.resize(entries.size());
        ctx.blas_ranges.resize(entries.size());
        ctx.blas_range_ptrs.resize(entries.size());
        for (size_t ei = 0; ei < entries.size(); ++ei) {
            BakeBlas& bb = bake_blas[ei];
            VkAccelerationStructureGeometryKHR& geom = ctx.blas_geoms[ei];
            geom = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            auto& tris = geom.geometry.triangles;
            tris.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            tris.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            tris.vertexData.deviceAddress = bb.vertices.address;
            tris.vertexStride = 3 * sizeof(float);
            tris.maxVertex = bb.vertex_count - 1;
            tris.indexType = VK_INDEX_TYPE_UINT32;
            tris.indexData.deviceAddress = bb.indices.address;

            VkAccelerationStructureBuildGeometryInfoKHR& build = ctx.blas_builds[ei];
            build = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            build.geometryCount = 1;
            build.pGeometries = &ctx.blas_geoms[ei];

            VkAccelerationStructureBuildSizesInfoKHR sizes{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            get_sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                      &build, &bb.primitive_count, &sizes);
            if (!matter::create_acceleration_structure(
                    vulkan, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                    sizes.accelerationStructureSize, bb.as, err))
                return false;
            // Over-allocate by (align-1) so the aligned scratch address below
            // still has buildScratchSize bytes of room even when the allocator
            // returns an unaligned base (mirrors emit_ray_instances,
            // vk_scene_renderer.cpp:5764). align_up-of-size alone does NOT
            // guarantee this and can overrun on unaligned base addresses.
            if (!matter::create_buffer(
                    vulkan, sizes.buildScratchSize + scratch_align - 1,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, bb.scratch, err))
                return false;
            build.dstAccelerationStructure = bb.as.handle;
            build.scratchData.deviceAddress =
                align_up(bb.scratch.address, scratch_align);

            VkAccelerationStructureBuildRangeInfoKHR& range = ctx.blas_ranges[ei];
            range = {};
            range.primitiveCount = bb.primitive_count;
            ctx.blas_range_ptrs[ei] = &ctx.blas_ranges[ei];
        }

        // Now the BLAS device addresses exist — patch instance references.
        for (auto& inst : instances) {
            // instanceCustomIndex still holds the blas_index we assigned above.
            inst.accelerationStructureReference =
                bake_blas[inst.instanceCustomIndex].as.address;
        }

        // 8. Upload the TLAS instance buffer + build the TLAS.
        matter::VkBufferResource instance_buf;
        const VkDeviceSize inst_bytes =
            instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
        if (!matter::create_buffer(
                vulkan, inst_bytes,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, instance_buf, err) ||
            !matter::upload_buffer(vulkan, instance_buf, instances.data(),
                                   (size_t)inst_bytes, 0, err))
            return false;

        ctx.tlas_geom = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        ctx.tlas_geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        ctx.tlas_geom.geometry.instances = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        ctx.tlas_geom.geometry.instances.data.deviceAddress = instance_buf.address;
        ctx.tlas_build = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        ctx.tlas_build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        ctx.tlas_build.flags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        ctx.tlas_build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        ctx.tlas_build.geometryCount = 1;
        ctx.tlas_build.pGeometries = &ctx.tlas_geom;
        const uint32_t inst_count = (uint32_t)instances.size();
        VkAccelerationStructureBuildSizesInfoKHR tlas_sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        get_sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                  &ctx.tlas_build, &inst_count, &tlas_sizes);
        matter::VkAccelerationStructureResource tlas_as;
        matter::VkBufferResource tlas_scratch;
        if (!matter::create_acceleration_structure(
                vulkan, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                tlas_sizes.accelerationStructureSize, tlas_as, err))
            return false;
        // Same (align-1) slack as the BLAS scratch above (mirrors
        // emit_ray_instances' TLAS scratch, vk_scene_renderer.cpp:5918).
        if (!matter::create_buffer(
                vulkan, tlas_sizes.buildScratchSize + scratch_align - 1,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, tlas_scratch, err))
            return false;
        ctx.tlas_build.dstAccelerationStructure = tlas_as.handle;
        ctx.tlas_build.scratchData.deviceAddress =
            align_up(tlas_scratch.address, scratch_align);
        ctx.tlas_range = {};
        ctx.tlas_range.primitiveCount = inst_count;

        // 9. Material table (canonical 12-float pack, identical to the GL bake's
        //    ssboMat layout so shader indices 0..4 + flatShading at 9 match).
        const int n_mat = MaterialRegistryCount();
        std::vector<float> mat_packed((size_t)n_mat * 12, 0.0f);
        for (int i = 0; i < n_mat; ++i) {
            const MaterialDef& m = *MaterialRegistryGet(i);
            float* r = mat_packed.data() + (size_t)i * 12;
            r[0] = m.albedo[0]; r[1] = m.albedo[1]; r[2] = m.albedo[2];
            r[3] = m.roughness;
            r[4] = m.metallic;  r[5] = m.emission;
            r[6] = 0.0f;
            r[7] = m.translucency;
            r[8] = m.ior;
            r[9] = (float)m.flatShading;
            r[10] = (float)m.mergeGroup;
            r[11] = (float)m.groundTilesetSlot;
        }

        // 10. Upload the shader SSBOs.
        auto make_ssbo = [&](const void* data, VkDeviceSize bytes,
                             matter::VkBufferResource& out) -> bool {
            if (bytes == 0) bytes = 4;  // never create a zero-size buffer
            return matter::create_buffer(
                       vulkan, bytes,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, out, err) &&
                   (data == nullptr ||
                    matter::upload_buffer(vulkan, out, data, (size_t)bytes, 0,
                                          err));
        };
        matter::VkBufferResource ssbo_tri_data, ssbo_tri_mat, ssbo_tri_first,
            ssbo_inst_mat, ssbo_mat;
        if (!make_ssbo(tri_data.data(), tri_data.size() * sizeof(float),
                       ssbo_tri_data) ||
            !make_ssbo(tri_mat.data(), tri_mat.size() * sizeof(int32_t),
                       ssbo_tri_mat) ||
            !make_ssbo(tri_first.data(), tri_first.size() * sizeof(uint32_t),
                       ssbo_tri_first) ||
            !make_ssbo(inst_mat.data(), inst_mat.size() * sizeof(int32_t),
                       ssbo_inst_mat) ||
            !make_ssbo(mat_packed.data(), mat_packed.size() * sizeof(float),
                       ssbo_mat))
            return false;

        // 11. Output images (RGBA8 / RG8 / RGBA8 / R16) + readback buffers.
        const int W = kTorusN * (int)settled.cfg.size * settled.cfg.texels_per_meter;
        const int H = W;
        const VkExtent3D extent{(uint32_t)W, (uint32_t)H, 1};
        const VkImageUsageFlags img_usage =
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        // [4] = R8 AO output image (V2). Same extent + usage as the primary set.
        matter::VkImageResource img_albedo, img_normal, img_orm, img_height,
            img_ao;
        const VkFormat fmts[5] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8_UNORM,
                                  VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16_UNORM,
                                  VK_FORMAT_R8_UNORM};
        matter::VkImageResource* imgs[5] = {&img_albedo, &img_normal, &img_orm,
                                            &img_height, &img_ao};
        for (int i = 0; i < 5; ++i) {
            if (!matter::create_image(vulkan, VK_IMAGE_TYPE_2D, fmts[i], extent,
                                      img_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                      *imgs[i], err))
                return false;
        }
        const VkDeviceSize texels = (VkDeviceSize)W * H;
        const VkDeviceSize rb_bytes[5] = {texels * 4, texels * 2, texels * 4,
                                          texels * 2, texels * 1};
        matter::VkBufferResource rb_albedo, rb_normal, rb_orm, rb_height, rb_ao;
        matter::VkBufferResource* rbs[5] = {&rb_albedo, &rb_normal, &rb_orm,
                                            &rb_height, &rb_ao};
        for (int i = 0; i < 5; ++i) {
            if (!matter::create_buffer(vulkan, rb_bytes[i],
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT, *rbs[i],
                                       err))
                return false;
        }

        // 12. Descriptor set layout + compute pipeline.
        VkDescriptorSetLayoutBinding binds[10]{};
        for (int i = 0; i < 10; ++i) {
            binds[i].binding = (uint32_t)i;
            binds[i].descriptorCount = 1;
            binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        for (int i = 0; i < 4; ++i)
            binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binds[4].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        for (int i = 5; i < 10; ++i)
            binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkDescriptorSetLayoutCreateInfo set_ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set_ci.bindingCount = 10;
        set_ci.pBindings = binds;
        VkResult vr =
            vkCreateDescriptorSetLayout(device, &set_ci, nullptr, &set_layout);
        if (vr != VK_SUCCESS) return vkfail("vkCreateDescriptorSetLayout", vr, err);

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.size = sizeof(PrimaryPush);
        VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
        VkPipelineLayoutCreateInfo pl_ci{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl_ci.setLayoutCount = 1;
        pl_ci.pSetLayouts = &set_layout;
        pl_ci.pushConstantRangeCount = 1;
        pl_ci.pPushConstantRanges = &pcr;
        vr = vkCreatePipelineLayout(device, &pl_ci, nullptr, &pipe_layout);
        if (vr != VK_SUCCESS) {
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            return vkfail("vkCreatePipelineLayout", vr, err);
        }

        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        // AO pass objects (V2), created below after the primary ones. Declared
        // here so the single cleanup lambda can tear them all down on any error.
        VkDescriptorSetLayout ao_set_layout = VK_NULL_HANDLE;
        VkPipelineLayout ao_pipe_layout = VK_NULL_HANDLE;
        VkPipeline ao_pipeline = VK_NULL_HANDLE;
        VkDescriptorPool ao_pool = VK_NULL_HANDLE;
        auto destroy_pipeline_objs = [&]() {
            if (ao_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, ao_pipeline, nullptr);
            if (ao_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, ao_pool, nullptr);
            if (ao_pipe_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, ao_pipe_layout, nullptr);
            if (ao_set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, ao_set_layout, nullptr);
            if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
            if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, pool, nullptr);
            vkDestroyPipelineLayout(device, pipe_layout, nullptr);
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        };

        const matter::EmbeddedSpirvView spirv =
            matter::find_spirv("tileset_bake_primary.comp.spv");
        if (!spirv.words || spirv.word_count == 0) {
            destroy_pipeline_objs();
            err = "bake_tileset_vk: embedded SPIR-V tileset_bake_primary.comp.spv "
                  "not found";
            return false;
        }
        VkShaderModule module = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo sm_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm_ci.codeSize = spirv.word_count * sizeof(uint32_t);
        sm_ci.pCode = spirv.words;
        vr = vkCreateShaderModule(device, &sm_ci, nullptr, &module);
        if (vr != VK_SUCCESS) {
            destroy_pipeline_objs();
            return vkfail("vkCreateShaderModule", vr, err);
        }
        VkComputePipelineCreateInfo cp_ci{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp_ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cp_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cp_ci.stage.module = module;
        cp_ci.stage.pName = "main";
        cp_ci.layout = pipe_layout;
        vr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp_ci, nullptr,
                                      &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        if (vr != VK_SUCCESS) {
            destroy_pipeline_objs();
            return vkfail("vkCreateComputePipelines", vr, err);
        }

        // 13. Descriptor pool + set + writes.
        const VkDescriptorPoolSize pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5}};
        VkDescriptorPoolCreateInfo pool_ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_ci.maxSets = 1;
        pool_ci.poolSizeCount = 3;
        pool_ci.pPoolSizes = pool_sizes;
        vr = vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool);
        if (vr != VK_SUCCESS) {
            destroy_pipeline_objs();
            return vkfail("vkCreateDescriptorPool", vr, err);
        }
        VkDescriptorSet dset = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ds_ai{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ds_ai.descriptorPool = pool;
        ds_ai.descriptorSetCount = 1;
        ds_ai.pSetLayouts = &set_layout;
        vr = vkAllocateDescriptorSets(device, &ds_ai, &dset);
        if (vr != VK_SUCCESS) {
            destroy_pipeline_objs();
            return vkfail("vkAllocateDescriptorSets", vr, err);
        }

        VkDescriptorImageInfo image_infos[4]{};
        for (int i = 0; i < 4; ++i) {
            image_infos[i].imageView = imgs[i]->view;
            image_infos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        VkWriteDescriptorSetAccelerationStructureKHR as_write{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        as_write.accelerationStructureCount = 1;
        as_write.pAccelerationStructures = &tlas_as.handle;
        VkDescriptorBufferInfo buf_infos[5] = {
            {ssbo_tri_data.buffer, 0, ssbo_tri_data.size},
            {ssbo_tri_mat.buffer, 0, ssbo_tri_mat.size},
            {ssbo_tri_first.buffer, 0, ssbo_tri_first.size},
            {ssbo_inst_mat.buffer, 0, ssbo_inst_mat.size},
            {ssbo_mat.buffer, 0, ssbo_mat.size}};

        VkWriteDescriptorSet writes[10]{};
        for (int i = 0; i < 4; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = dset;
            writes[i].dstBinding = (uint32_t)i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &image_infos[i];
        }
        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].pNext = &as_write;
        writes[4].dstSet = dset;
        writes[4].dstBinding = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        for (int i = 0; i < 5; ++i) {
            writes[5 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[5 + i].dstSet = dset;
            writes[5 + i].dstBinding = (uint32_t)(5 + i);
            writes[5 + i].descriptorCount = 1;
            writes[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[5 + i].pBufferInfo = &buf_infos[i];
        }
        vkUpdateDescriptorSets(device, 10, writes, 0, nullptr);

        // 13b. AO pass (V2): its own 7-binding descriptor set layout, pipeline,
        //      pool, and set. Bindings: 0 = R8 AO storage image, 1 = the SAME
        //      TLAS, 2..6 = the SAME tri_data/tri_mat/tri_first/inst_mat/material
        //      SSBOs the primary pass reads.
        VkDescriptorSetLayoutBinding ao_binds[7]{};
        for (int i = 0; i < 7; ++i) {
            ao_binds[i].binding = (uint32_t)i;
            ao_binds[i].descriptorCount = 1;
            ao_binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        ao_binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ao_binds[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        for (int i = 2; i < 7; ++i)
            ao_binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        VkDescriptorSetLayoutCreateInfo ao_set_ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ao_set_ci.bindingCount = 7;
        ao_set_ci.pBindings = ao_binds;
        vr = vkCreateDescriptorSetLayout(device, &ao_set_ci, nullptr,
                                         &ao_set_layout);
        if (vr != VK_SUCCESS) {
            destroy_pipeline_objs();
            return vkfail("vkCreateDescriptorSetLayout(ao)", vr, err);
        }

        VkPushConstantRange ao_pcr{};
        ao_pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        ao_pcr.size = sizeof(AoPush);
        VkPipelineLayoutCreateInfo ao_pl_ci{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ao_pl_ci.setLayoutCount = 1;
        ao_pl_ci.pSetLayouts = &ao_set_layout;
        ao_pl_ci.pushConstantRangeCount = 1;
        ao_pl_ci.pPushConstantRanges = &ao_pcr;
        vr = vkCreatePipelineLayout(device, &ao_pl_ci, nullptr, &ao_pipe_layout);
        if (vr != VK_SUCCESS) {
            destroy_pipeline_objs();
            return vkfail("vkCreatePipelineLayout(ao)", vr, err);
        }

        const matter::EmbeddedSpirvView ao_spirv =
            matter::find_spirv("tileset_bake_ao.comp.spv");
        if (!ao_spirv.words || ao_spirv.word_count == 0) {
            destroy_pipeline_objs();
            err = "bake_tileset_vk: embedded SPIR-V tileset_bake_ao.comp.spv "
                  "not found";
            return false;
        }
        VkShaderModule ao_module = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo ao_sm_ci{
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ao_sm_ci.codeSize = ao_spirv.word_count * sizeof(uint32_t);
        ao_sm_ci.pCode = ao_spirv.words;
        vr = vkCreateShaderModule(device, &ao_sm_ci, nullptr, &ao_module);
        if (vr != VK_SUCCESS) {
            destroy_pipeline_objs();
            return vkfail("vkCreateShaderModule(ao)", vr, err);
        }
        VkComputePipelineCreateInfo ao_cp_ci{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ao_cp_ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ao_cp_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ao_cp_ci.stage.module = ao_module;
        ao_cp_ci.stage.pName = "main";
        ao_cp_ci.layout = ao_pipe_layout;
        vr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ao_cp_ci,
                                      nullptr, &ao_pipeline);
        vkDestroyShaderModule(device, ao_module, nullptr);
        if (vr != VK_SUCCESS) {
            destroy_pipeline_objs();
            return vkfail("vkCreateComputePipelines(ao)", vr, err);
        }

        const VkDescriptorPoolSize ao_pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5}};
        VkDescriptorPoolCreateInfo ao_pool_ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ao_pool_ci.maxSets = 1;
        ao_pool_ci.poolSizeCount = 3;
        ao_pool_ci.pPoolSizes = ao_pool_sizes;
        vr = vkCreateDescriptorPool(device, &ao_pool_ci, nullptr, &ao_pool);
        if (vr != VK_SUCCESS) {
            destroy_pipeline_objs();
            return vkfail("vkCreateDescriptorPool(ao)", vr, err);
        }
        VkDescriptorSet ao_dset = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ao_ds_ai{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ao_ds_ai.descriptorPool = ao_pool;
        ao_ds_ai.descriptorSetCount = 1;
        ao_ds_ai.pSetLayouts = &ao_set_layout;
        vr = vkAllocateDescriptorSets(device, &ao_ds_ai, &ao_dset);
        if (vr != VK_SUCCESS) {
            destroy_pipeline_objs();
            return vkfail("vkAllocateDescriptorSets(ao)", vr, err);
        }

        VkDescriptorImageInfo ao_image_info{};
        ao_image_info.imageView = img_ao.view;
        ao_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        // Reuse as_write / buf_infos (same TLAS + SSBOs) — only the binding
        // slots differ (TLAS 4->1, SSBOs 5..9->2..6).
        VkWriteDescriptorSet ao_writes[7]{};
        ao_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ao_writes[0].dstSet = ao_dset;
        ao_writes[0].dstBinding = 0;
        ao_writes[0].descriptorCount = 1;
        ao_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ao_writes[0].pImageInfo = &ao_image_info;
        ao_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ao_writes[1].pNext = &as_write;
        ao_writes[1].dstSet = ao_dset;
        ao_writes[1].dstBinding = 1;
        ao_writes[1].descriptorCount = 1;
        ao_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        for (int i = 0; i < 5; ++i) {
            ao_writes[2 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ao_writes[2 + i].dstSet = ao_dset;
            ao_writes[2 + i].dstBinding = (uint32_t)(2 + i);
            ao_writes[2 + i].descriptorCount = 1;
            ao_writes[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            ao_writes[2 + i].pBufferInfo = &buf_infos[i];
        }
        vkUpdateDescriptorSets(device, 7, ao_writes, 0, nullptr);

        // 14. Height range + ray origin.
        float hmin = 0.0f, hmax = 1.0f, max_top = 1.0f;
        compute_height_range(settled, hmin, hmax, max_top);
        const float ray_y_floor = hmax + 2.0f;
        const float ray_y_env = max_top + 0.5f;
        const float ray_y = ray_y_floor > ray_y_env ? ray_y_floor : ray_y_env;

        // 15. Fill the record context and submit the whole bake.
        ctx.pipeline = pipeline;
        ctx.pipeline_layout = pipe_layout;
        ctx.descriptor_set = dset;
        ctx.push.atlasW = W;
        ctx.push.atlasH = H;
        ctx.push.texelsPerMeter = settled.cfg.texels_per_meter;
        ctx.push.rayY = ray_y;
        ctx.push.heightMin = hmin;
        ctx.push.heightMax = hmax;
        // AO push constants — sourced exactly as the GL driver
        // (tileset_bake_gpu.cpp:200-202, tileset_bake_ao.cpp:111-113).
        ctx.ao_pipeline = ao_pipeline;
        ctx.ao_pipeline_layout = ao_pipe_layout;
        ctx.ao_descriptor_set = ao_dset;
        ctx.ao_push.atlasW = W;
        ctx.ao_push.atlasH = H;
        ctx.ao_push.texelsPerMeter = settled.cfg.texels_per_meter;
        ctx.ao_push.rayY = ray_y;
        ctx.ao_push.heightMin = hmin;
        ctx.ao_push.heightMax = hmax;
        ctx.ao_push.seed = (uint32_t)settled.cfg.seed;
        ctx.ao_push.aoSamples = 64;
        ctx.ao_push.maxRayDist = settled.cfg.edge_strip_width;
        ctx.group_x = (uint32_t)((W + 7) / 8);
        ctx.group_y = (uint32_t)((H + 7) / 8);
        for (int i = 0; i < 5; ++i) {
            ctx.images[i] = imgs[i];
            ctx.readbacks[i] = rbs[i]->buffer;
        }
        ctx.width = (uint32_t)W;
        ctx.height = (uint32_t)H;

        std::vector<std::shared_ptr<void>> deps;
        deps.push_back(tlas_as.lifetime);
        deps.push_back(tlas_scratch.lifetime);
        deps.push_back(instance_buf.lifetime);
        deps.push_back(ssbo_tri_data.lifetime);
        deps.push_back(ssbo_tri_mat.lifetime);
        deps.push_back(ssbo_tri_first.lifetime);
        deps.push_back(ssbo_inst_mat.lifetime);
        deps.push_back(ssbo_mat.lifetime);
        for (auto& bb : bake_blas) {
            deps.push_back(bb.vertices.lifetime);
            deps.push_back(bb.indices.lifetime);
            deps.push_back(bb.as.lifetime);
            deps.push_back(bb.scratch.lifetime);
        }
        for (int i = 0; i < 5; ++i) {
            deps.push_back(imgs[i]->lifetime);
            deps.push_back(rbs[i]->lifetime);
        }

        const bool submitted = matter::submit_immediate(
            vulkan, record_bake, &ctx, err,
            matter::ImmediateSubmitPhase::compute_dispatch, std::move(deps));
        destroy_pipeline_objs();
        if (!submitted) return false;

        // 16. Readback + repack into the shapes save_gtex expects.
        std::vector<uint8_t> rb_a((size_t)rb_bytes[0]);
        std::vector<uint8_t> rb_n((size_t)rb_bytes[1]);
        std::vector<uint8_t> rb_o((size_t)rb_bytes[2]);
        std::vector<uint8_t> rb_h((size_t)rb_bytes[3]);
        std::vector<uint8_t> rb_ao_bytes((size_t)rb_bytes[4]);
        if (!matter::readback_buffer(vulkan, rb_albedo, rb_a.data(), rb_a.size(), 0, err) ||
            !matter::readback_buffer(vulkan, rb_normal, rb_n.data(), rb_n.size(), 0, err) ||
            !matter::readback_buffer(vulkan, rb_orm, rb_o.data(), rb_o.size(), 0, err) ||
            !matter::readback_buffer(vulkan, rb_height, rb_h.data(), rb_h.size(), 0, err) ||
            !matter::readback_buffer(vulkan, rb_ao, rb_ao_bytes.data(), rb_ao_bytes.size(), 0, err))
            return false;

        std::vector<uint8_t> albedo, orm, normal_rg;
        std::vector<uint16_t> height((size_t)texels);
        gtex_drop_alpha_rgba8(rb_a.data(), (size_t)texels, albedo);
        gtex_drop_alpha_rgba8(rb_o.data(), (size_t)texels, orm);
        // V2: fold the computed R8 AO over the V1 ORM.r placeholder, exactly as
        // the GL driver does (pack_orm_ao, tileset_bake_gpu.cpp:212).
        gtex_pack_orm_ao(orm, rb_ao_bytes);
        normal_rg.assign(rb_n.begin(), rb_n.end());
        std::memcpy(height.data(), rb_h.data(), rb_h.size());

        // 17. Write .gtex. V1 emits the four core channels only (no horizon);
        //     kEngineBakeVersion is intentionally unchanged (validation build).
        GTexHeader hdr{};
        hdr.tile_size_m = settled.cfg.size;
        hdr.texels_per_meter = settled.cfg.texels_per_meter;
        hdr.atlas_tiles_x = kTorusN;
        hdr.atlas_tiles_y = kTorusN;
        hdr.height_min = hmin;
        hdr.height_max = hmax;
        hdr.content_hash = expected;
        hdr.box3d_version = kBox3dVersion;
        hdr.engine_bake_version = kEngineBakeVersion;
        if (!save_gtex(out_gtex_path, hdr, W, H, albedo.data(), normal_rg.data(),
                       orm.data(), height.data(), err))
            return false;

        if (dump_png) {
            std::string base = out_gtex_path;
            if (base.size() >= 5 && base.compare(base.size() - 5, 5, ".gtex") == 0)
                base.resize(base.size() - 5);
            if (!dump_pngs(base, W, H, albedo, normal_rg, orm, height)) {
                err = "bake_tileset_vk: --dump-png emit failed near " + base;
                return false;
            }
        }
        return true;

    } catch (const std::bad_alloc&) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "OOM in bake_tileset_vk (pose_hash=%016llx)",
                      (unsigned long long)settled.report.pose_hash);
        err = buf;
        return false;
    }
}

}  // namespace tileset
