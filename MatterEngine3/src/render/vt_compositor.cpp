// vt_compositor.cpp — WP-D tier-1 chart-page compositor + GPU BC encode.
// See vt_compositor.h for the module contract and shaders_vk/vt_composite.comp
// / vt_bc_encode.comp for the GPU passes this records.

#include "vt_compositor.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "shaders_gen/embedded_spirv.h"

namespace vt {

namespace {

// ---------------------------------------------------------------------------
// GPU struct mirrors — layouts must match vt_composite.comp / vt_bc_encode.comp
// (std430 for the SSBOs, std140 for the params UBO; every field is 16-byte
// packed so the C++ mirrors are exact).
// ---------------------------------------------------------------------------
struct GpuChart {
    float origin_tpm[4];
    float tangent_ou[4];
    float bitangent_ov[4];
    uint32_t rect[4];
    uint32_t tri_range[4];
};
static_assert(sizeof(GpuChart) == 80, "GpuChart must match std430 layout");

struct GpuTri {
    float p0[4], p1[4], p2[4];
    float n0[4], n1[4], n2[4];
    uint32_t mat[4];
    // WP-F: per-vertex surfaces()-tape weights, 8 u8 columns per vertex
    // (kMaxSurfaceMaterials), 2 u32 per vertex:
    //   wA = {v0 cols 0-3, v0 cols 4-7, v1 cols 0-3, v1 cols 4-7}
    //   wB = {v2 cols 0-3, v2 cols 4-7, 0, 0}
    // All zero when the part carries no tape (weight mode never reads them).
    uint32_t wA[4];
    uint32_t wB[4];
};
static_assert(sizeof(GpuTri) == 144, "GpuTri must match std430 layout");

struct GpuFillRequest {
    uint32_t a[4];   // page_x, page_y, mip, out_layer
    uint32_t b[4];   // cand_offset, cand_count, weight_mode, debug materials
    float debug_params[4];
    // WP-F: tape material registry ids for weight columns 0-7 (u8 each, x =
    // cols 0-3, y = cols 4-7), z = declared column count, w unused.
    uint32_t tape[4];
};
static_assert(sizeof(GpuFillRequest) == 64, "GpuFillRequest layout");

struct GpuVtMaterial {
    float albedo[4];
    float orm[4];
    int32_t slot[4];
};
static_assert(sizeof(GpuVtMaterial) == 48, "GpuVtMaterial layout");

struct VtParamsUbo {
    float tile_size_m[8];        // vec4[2]
    float texels_per_meter[8];   // vec4[2]
};
static_assert(sizeof(VtParamsUbo) == 64, "VtParamsUbo layout");

constexpr uint32_t kMaxRequestsPerFill = 256;
constexpr uint32_t kMaxCandEntriesPerFill = 65536;
constexpr uint32_t kTilesetArraySize =
    VtCompositor::kMaxDetailSlots * 4;   // slot*4 + (albedo|normal|orm|height)
constexpr uint32_t kMaxMeshEntries = 512;

// ---------------------------------------------------------------------------
// Raw resource helpers (this module takes plain Vk handles, so it cannot use
// the VulkanDevice-coupled helpers in vk_resources.h).
// ---------------------------------------------------------------------------
struct RawBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

struct RawImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

bool find_memory_type_raw(VkPhysicalDevice phys, uint32_t allowed_bits,
                          VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred, uint32_t& out) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if (!(allowed_bits & (1u << i))) continue;
        VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        if ((flags & preferred) == preferred) { out = i; return true; }
        if (fallback == UINT32_MAX) fallback = i;
    }
    if (fallback != UINT32_MAX) { out = fallback; return true; }
    return false;
}

bool create_raw_buffer(VkDevice device, VkPhysicalDevice phys,
                       VkDeviceSize size, VkBufferUsageFlags usage,
                       bool host_visible, RawBuffer& out, std::string& err) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS) {
        err = "vt_compositor: vkCreateBuffer failed";
        return false;
    }
    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device, out.buffer, &reqs);
    const VkMemoryPropertyFlags required =
        host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkMemoryPropertyFlags preferred =
        host_visible ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0;
    uint32_t type = 0;
    if (!find_memory_type_raw(phys, reqs.memoryTypeBits, required, preferred,
                              type)) {
        err = "vt_compositor: no suitable buffer memory type";
        return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS) {
        err = "vt_compositor: vkAllocateMemory (buffer) failed";
        return false;
    }
    if (vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS) {
        err = "vt_compositor: vkBindBufferMemory failed";
        return false;
    }
    if (host_visible &&
        vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) !=
            VK_SUCCESS) {
        err = "vt_compositor: vkMapMemory failed";
        return false;
    }
    out.size = size;
    return true;
}

void destroy_raw_buffer(VkDevice device, RawBuffer& b) {
    if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device, b.memory, nullptr);
    b = RawBuffer{};
}

bool create_raw_image_array(VkDevice device, VkPhysicalDevice phys,
                            uint32_t width, uint32_t height, uint32_t layers,
                            VkFormat format, VkImageUsageFlags usage,
                            RawImage& out, std::string& err) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &info, nullptr, &out.image) != VK_SUCCESS) {
        err = "vt_compositor: vkCreateImage failed";
        return false;
    }
    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(device, out.image, &reqs);
    uint32_t type = 0;
    if (!find_memory_type_raw(phys, reqs.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, type)) {
        err = "vt_compositor: no suitable image memory type";
        return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS) {
        err = "vt_compositor: vkAllocateMemory (image) failed";
        return false;
    }
    if (vkBindImageMemory(device, out.image, out.memory, 0) != VK_SUCCESS) {
        err = "vt_compositor: vkBindImageMemory failed";
        return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    if (vkCreateImageView(device, &view, nullptr, &out.view) != VK_SUCCESS) {
        err = "vt_compositor: vkCreateImageView failed";
        return false;
    }
    return true;
}

void destroy_raw_image(VkDevice device, RawImage& img) {
    if (img.view) vkDestroyImageView(device, img.view, nullptr);
    if (img.image) vkDestroyImage(device, img.image, nullptr);
    if (img.memory) vkFreeMemory(device, img.memory, nullptr);
    img = RawImage{};
}

void cmd_memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage,
                        VkAccessFlags2 src_access,
                        VkPipelineStageFlags2 dst_stage,
                        VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void cmd_image_barrier(VkCommandBuffer cmd, VkImage image,
                       VkImageLayout old_layout, VkImageLayout new_layout,
                       VkPipelineStageFlags2 src_stage,
                       VkAccessFlags2 src_access,
                       VkPipelineStageFlags2 dst_stage,
                       VkAccessFlags2 dst_access, uint32_t layers) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct VtCompositor::Impl {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

    VkDescriptorSetLayout mesh_layout = VK_NULL_HANDLE;    // set 0 (composite)
    VkDescriptorSetLayout batch_layout = VK_NULL_HANDLE;   // set 1 (composite)
    VkDescriptorSetLayout encode_layout = VK_NULL_HANDLE;  // set 0 (encode)
    VkPipelineLayout composite_pl = VK_NULL_HANDLE;
    VkPipelineLayout encode_pl = VK_NULL_HANDLE;
    VkPipeline composite_pipe = VK_NULL_HANDLE;
    VkPipeline encode_pipe = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    RawImage dummy_tileset;   // 1x1x1 RGBA8 array, neutral 0.5 gray

    RawBuffer materials_buf;  // host-visible, kMaxMaterials entries
    RawBuffer params_buf;     // host-visible UBO

    struct Ring {
        RawBuffer requests;              // host-visible
        RawBuffer cands;                 // host-visible
        RawBuffer out_albedo;            // device-local block buffers
        RawBuffer out_normal;
        RawBuffer out_orm;
        RawImage inter_albedo;           // rgba8 arrays, kBatchStride layers
        RawImage inter_normal;
        RawImage inter_orm;
        RawImage inter_aux;
        VkDescriptorSet batch_set = VK_NULL_HANDLE;
        VkDescriptorSet encode_set = VK_NULL_HANDLE;
    };
    Ring rings[kMaxBatchesInFlight];
    uint32_t ring_cursor = 0;

    struct MeshEntry {
        RawBuffer charts;
        RawBuffer tris;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t chart_count = 0;
    };
    std::map<std::pair<uint64_t, uint32_t>, MeshEntry> mesh_cache;

    VtTilesetSlotViews tileset_slots[kMaxDetailSlots]{};
    uint32_t tileset_slot_count = 0;

    WeightMode weight_mode = WeightMode::kTriangleMaterial;
    uint32_t debug_mat_a = 0, debug_mat_b = 0;
    float debug_blend_start = 0.0f, debug_blend_width = 1.0f;

    bool init_recorded = false;

    ~Impl() { destroy(); }

    void destroy() {
        if (!device) return;
        for (auto& kv : mesh_cache) {
            destroy_raw_buffer(device, kv.second.charts);
            destroy_raw_buffer(device, kv.second.tris);
        }
        mesh_cache.clear();
        for (Ring& r : rings) {
            destroy_raw_buffer(device, r.requests);
            destroy_raw_buffer(device, r.cands);
            destroy_raw_buffer(device, r.out_albedo);
            destroy_raw_buffer(device, r.out_normal);
            destroy_raw_buffer(device, r.out_orm);
            destroy_raw_image(device, r.inter_albedo);
            destroy_raw_image(device, r.inter_normal);
            destroy_raw_image(device, r.inter_orm);
            destroy_raw_image(device, r.inter_aux);
        }
        destroy_raw_buffer(device, materials_buf);
        destroy_raw_buffer(device, params_buf);
        destroy_raw_image(device, dummy_tileset);
        if (sampler) vkDestroySampler(device, sampler, nullptr);
        if (descriptor_pool)
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        if (composite_pipe) vkDestroyPipeline(device, composite_pipe, nullptr);
        if (encode_pipe) vkDestroyPipeline(device, encode_pipe, nullptr);
        if (composite_pl) vkDestroyPipelineLayout(device, composite_pl, nullptr);
        if (encode_pl) vkDestroyPipelineLayout(device, encode_pl, nullptr);
        if (mesh_layout)
            vkDestroyDescriptorSetLayout(device, mesh_layout, nullptr);
        if (batch_layout)
            vkDestroyDescriptorSetLayout(device, batch_layout, nullptr);
        if (encode_layout)
            vkDestroyDescriptorSetLayout(device, encode_layout, nullptr);
        device = VK_NULL_HANDLE;
    }

    bool create_pipeline(const char* spirv_name, VkPipelineLayout layout,
                         VkPipeline& out, std::string& err) {
        const matter::EmbeddedSpirvView spirv = matter::find_spirv(spirv_name);
        if (!spirv.words || spirv.word_count == 0) {
            err = std::string("vt_compositor: embedded SPIR-V not found: ") +
                  spirv_name;
            return false;
        }
        VkShaderModuleCreateInfo mod{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mod.codeSize = spirv.word_count * sizeof(uint32_t);
        mod.pCode = spirv.words;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &mod, nullptr, &module) != VK_SUCCESS) {
            err = "vt_compositor: vkCreateShaderModule failed";
            return false;
        }
        VkComputePipelineCreateInfo info{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = layout;
        const VkResult result = vkCreateComputePipelines(
            device, pipeline_cache, 1, &info, nullptr, &out);
        vkDestroyShaderModule(device, module, nullptr);
        if (result != VK_SUCCESS) {
            err = "vt_compositor: vkCreateComputePipelines failed";
            return false;
        }
        return true;
    }

    bool init(std::string& err);
    void write_ring_descriptors(Ring& r);
    void write_tileset_descriptors();
    MeshEntry* get_or_build_mesh_entry(uint64_t variant_hash, uint32_t rung,
                                       const chart_atlas::ChartAtlasRung* atlas,
                                       const VtPartContext* ctx,
                                       Stats& stats);
    void record_init(VkCommandBuffer cmd);
};

bool VtCompositor::Impl::init(std::string& err) {
    // ---- descriptor set layouts ----
    auto make_layout = [&](const std::vector<VkDescriptorSetLayoutBinding>& b,
                           VkDescriptorSetLayout& out) -> bool {
        VkDescriptorSetLayoutCreateInfo info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = static_cast<uint32_t>(b.size());
        info.pBindings = b.data();
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &out) !=
            VK_SUCCESS) {
            err = "vt_compositor: vkCreateDescriptorSetLayout failed";
            return false;
        }
        return true;
    };
    auto binding = [](uint32_t idx, VkDescriptorType type, uint32_t count) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = idx;
        b.descriptorType = type;
        b.descriptorCount = count;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        return b;
    };

    if (!make_layout({binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1)},
                     mesh_layout))
        return false;
    if (!make_layout(
            {binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
             binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
             binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
             binding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1),
             binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     kTilesetArraySize),
             binding(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1),
             binding(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1),
             binding(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1),
             binding(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1)},
            batch_layout))
        return false;
    if (!make_layout({binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1),
                      binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1),
                      binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1),
                      binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1)},
                     encode_layout))
        return false;

    // ---- pipeline layouts ----
    {
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkDescriptorSetLayout sets[2] = {mesh_layout, batch_layout};
        VkPipelineLayoutCreateInfo info{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        info.setLayoutCount = 2;
        info.pSetLayouts = sets;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device, &info, nullptr, &composite_pl) !=
            VK_SUCCESS) {
            err = "vt_compositor: vkCreatePipelineLayout (composite) failed";
            return false;
        }
    }
    {
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo info{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        info.setLayoutCount = 1;
        info.pSetLayouts = &encode_layout;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device, &info, nullptr, &encode_pl) !=
            VK_SUCCESS) {
            err = "vt_compositor: vkCreatePipelineLayout (encode) failed";
            return false;
        }
    }

    if (!create_pipeline("vt_composite.comp.spv", composite_pl, composite_pipe,
                         err))
        return false;
    if (!create_pipeline("vt_bc_encode.comp.spv", encode_pl, encode_pipe, err))
        return false;

    // ---- sampler (linear, repeat, trilinear across the tileset mips) ----
    {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) {
            err = "vt_compositor: vkCreateSampler failed";
            return false;
        }
    }

    // ---- dummy tileset (neutral for every channel-kind) ----
    if (!create_raw_image_array(device, phys, 1, 1, 1,
                                VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                dummy_tileset, err))
        return false;

    // ---- global host-visible tables ----
    if (!create_raw_buffer(device, phys,
                           sizeof(GpuVtMaterial) * kMaxMaterials,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                           materials_buf, err))
        return false;
    if (!create_raw_buffer(device, phys, sizeof(VtParamsUbo),
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true,
                           params_buf, err))
        return false;
    {
        // Neutral defaults so an unset table still composes deterministically.
        auto* mats = static_cast<GpuVtMaterial*>(materials_buf.mapped);
        for (uint32_t i = 0; i < kMaxMaterials; ++i) {
            mats[i] = GpuVtMaterial{{0.5f, 0.5f, 0.5f, 1.0f},
                                    {1.0f, 0.8f, 0.0f, 0.0f},
                                    {-1, 0, 0, 0}};
        }
        auto* params = static_cast<VtParamsUbo*>(params_buf.mapped);
        for (int i = 0; i < 8; ++i) {
            params->tile_size_m[i] = 1.0f;
            params->texels_per_meter[i] = 1024.0f;
        }
    }

    // ---- descriptor pool ----
    {
        const uint32_t ring_sets = kMaxBatchesInFlight * 2;
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             kMaxMeshEntries * 2 + kMaxBatchesInFlight * 6},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxBatchesInFlight},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             kMaxBatchesInFlight * kTilesetArraySize},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxBatchesInFlight * 7},
        };
        VkDescriptorPoolCreateInfo info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = kMaxMeshEntries + ring_sets;
        info.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
        info.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &info, nullptr, &descriptor_pool) !=
            VK_SUCCESS) {
            err = "vt_compositor: vkCreateDescriptorPool failed";
            return false;
        }
    }

    // ---- per-ring transient resources ----
    for (Ring& r : rings) {
        if (!create_raw_buffer(device, phys,
                               sizeof(GpuFillRequest) * kMaxRequestsPerFill,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                               r.requests, err))
            return false;
        if (!create_raw_buffer(device, phys,
                               sizeof(uint32_t) * kMaxCandEntriesPerFill,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                               r.cands, err))
            return false;
        const VkDeviceSize block_bytes =
            VkDeviceSize(kBatchStride) * kBlocksPerPage * 16;
        for (RawBuffer* buf : {&r.out_albedo, &r.out_normal, &r.out_orm}) {
            if (!create_raw_buffer(device, phys, block_bytes,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   false, *buf, err))
                return false;
        }
        const VkImageUsageFlags inter_usage =
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        for (RawImage* img : {&r.inter_albedo, &r.inter_normal, &r.inter_orm,
                              &r.inter_aux}) {
            if (!create_raw_image_array(device, phys, kPageStore, kPageStore,
                                        kBatchStride,
                                        VK_FORMAT_R8G8B8A8_UNORM, inter_usage,
                                        *img, err))
                return false;
        }
        VkDescriptorSetLayout layouts[2] = {batch_layout, encode_layout};
        VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkDescriptorSetAllocateInfo alloc{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptor_pool;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(device, &alloc, sets) != VK_SUCCESS) {
            err = "vt_compositor: vkAllocateDescriptorSets (ring) failed";
            return false;
        }
        r.batch_set = sets[0];
        r.encode_set = sets[1];
        write_ring_descriptors(r);
    }
    write_tileset_descriptors();
    return true;
}

void VtCompositor::Impl::write_ring_descriptors(Ring& r) {
    VkDescriptorBufferInfo requests{r.requests.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo cands{r.cands.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo mats{materials_buf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo params{params_buf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo inter[4] = {
        {VK_NULL_HANDLE, r.inter_albedo.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, r.inter_normal.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, r.inter_orm.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, r.inter_aux.view, VK_IMAGE_LAYOUT_GENERAL},
    };
    VkDescriptorBufferInfo out_bufs[3] = {
        {r.out_albedo.buffer, 0, VK_WHOLE_SIZE},
        {r.out_normal.buffer, 0, VK_WHOLE_SIZE},
        {r.out_orm.buffer, 0, VK_WHOLE_SIZE},
    };

    std::vector<VkWriteDescriptorSet> writes;
    auto write_buf = [&](VkDescriptorSet set, uint32_t bind,
                         VkDescriptorType type,
                         const VkDescriptorBufferInfo* info) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = bind;
        w.descriptorCount = 1;
        w.descriptorType = type;
        w.pBufferInfo = info;
        writes.push_back(w);
    };
    auto write_img = [&](VkDescriptorSet set, uint32_t bind,
                         const VkDescriptorImageInfo* info) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = bind;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = info;
        writes.push_back(w);
    };

    write_buf(r.batch_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &requests);
    write_buf(r.batch_set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &cands);
    write_buf(r.batch_set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &mats);
    write_buf(r.batch_set, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &params);
    write_img(r.batch_set, 5, &inter[0]);
    write_img(r.batch_set, 6, &inter[1]);
    write_img(r.batch_set, 7, &inter[2]);
    write_img(r.batch_set, 8, &inter[3]);
    write_img(r.encode_set, 0, &inter[0]);
    write_img(r.encode_set, 1, &inter[1]);
    write_img(r.encode_set, 2, &inter[2]);
    write_buf(r.encode_set, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &out_bufs[0]);
    write_buf(r.encode_set, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &out_bufs[1]);
    write_buf(r.encode_set, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &out_bufs[2]);
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void VtCompositor::Impl::write_tileset_descriptors() {
    VkDescriptorImageInfo infos[kTilesetArraySize];
    for (uint32_t slot = 0; slot < kMaxDetailSlots; ++slot) {
        const VtTilesetSlotViews& s = tileset_slots[slot];
        const VkImageView views[4] = {s.albedo, s.normal, s.orm, s.height};
        for (uint32_t ch = 0; ch < 4; ++ch) {
            VkImageView view =
                (slot < tileset_slot_count && views[ch]) ? views[ch]
                                                         : dummy_tileset.view;
            infos[slot * 4 + ch] = {sampler, view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
    }
    VkWriteDescriptorSet writes[kMaxBatchesInFlight];
    for (uint32_t i = 0; i < kMaxBatchesInFlight; ++i) {
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = rings[i].batch_set;
        writes[i].dstBinding = 4;
        writes[i].descriptorCount = kTilesetArraySize;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = infos;
    }
    vkUpdateDescriptorSets(device, kMaxBatchesInFlight, writes, 0, nullptr);
}

VtCompositor::Impl::MeshEntry* VtCompositor::Impl::get_or_build_mesh_entry(
    uint64_t variant_hash, uint32_t rung,
    const chart_atlas::ChartAtlasRung* atlas, const VtPartContext* ctx,
    Stats& stats) {
    const auto key = std::make_pair(variant_hash, rung);
    auto it = mesh_cache.find(key);
    if (it != mesh_cache.end()) return &it->second;
    if (mesh_cache.size() >= kMaxMeshEntries) return nullptr;
    if (atlas->charts.empty()) return nullptr;

    // WP-F: whether this part carries a surfaces()-tape classification whose
    // per-vertex weight columns get packed into the triangle stream below.
    const bool has_tape =
        ctx->surface_material_count > 0 &&
        ctx->surface_material_count <= VtCompositor::kMaxSurfaceMaterials &&
        ctx->surface_weights != nullptr;
    const uint32_t tape_cols = has_tape ? ctx->surface_material_count : 0;

    // Reorder triangles chart-grouped (tri_order) with per-vertex plane
    // coordinates precomputed against each chart's basis.
    std::vector<GpuChart> gcharts(atlas->charts.size());
    std::vector<GpuTri> gtris;
    gtris.reserve(atlas->tri_order.size());
    for (size_t ci = 0; ci < atlas->charts.size(); ++ci) {
        const chart_atlas::ChartEntry& c = atlas->charts[ci];
        GpuChart& g = gcharts[ci];
        const float ou = c.origin[0] * c.tangent[0] +
                         c.origin[1] * c.tangent[1] +
                         c.origin[2] * c.tangent[2];
        const float ov = c.origin[0] * c.bitangent[0] +
                         c.origin[1] * c.bitangent[1] +
                         c.origin[2] * c.bitangent[2];
        g.origin_tpm[0] = c.origin[0];
        g.origin_tpm[1] = c.origin[1];
        g.origin_tpm[2] = c.origin[2];
        g.origin_tpm[3] = c.texels_per_meter;
        g.tangent_ou[0] = c.tangent[0];
        g.tangent_ou[1] = c.tangent[1];
        g.tangent_ou[2] = c.tangent[2];
        g.tangent_ou[3] = ou;
        g.bitangent_ov[0] = c.bitangent[0];
        g.bitangent_ov[1] = c.bitangent[1];
        g.bitangent_ov[2] = c.bitangent[2];
        g.bitangent_ov[3] = ov;
        g.rect[0] = c.rect_x;
        g.rect[1] = c.rect_y;
        g.rect[2] = c.rect_w;
        g.rect[3] = c.rect_h;
        g.tri_range[0] = static_cast<uint32_t>(gtris.size());
        uint32_t emitted = 0;
        for (uint32_t i = 0; i < c.tri_count; ++i) {
            const uint32_t oi = c.first_tri + i;
            if (oi >= atlas->tri_order.size()) break;
            const uint32_t ti = atlas->tri_order[oi];
            if (ti >= ctx->triangle_count) continue;
            GpuTri g_tri{};
            float* pdst[3] = {g_tri.p0, g_tri.p1, g_tri.p2};
            float* ndst[3] = {g_tri.n0, g_tri.n1, g_tri.n2};
            float pos[3][3];
            bool corners_ok = true;
            uint32_t corner0 = 0;
            uint32_t corners[3] = {0, 0, 0};
            for (int v = 0; v < 3; ++v) {
                const uint32_t corner = ctx->indices[3 * ti + v];
                if (corner >= ctx->vertex_count) { corners_ok = false; break; }
                corners[v] = corner;
                if (v == 0) corner0 = corner;
                for (int k = 0; k < 3; ++k)
                    pos[v][k] = ctx->positions[3 * corner + k];
                pdst[v][0] = pos[v][0];
                pdst[v][1] = pos[v][1];
                pdst[v][2] = pos[v][2];
                pdst[v][3] = pos[v][0] * c.tangent[0] +
                             pos[v][1] * c.tangent[1] +
                             pos[v][2] * c.tangent[2];   // plane U
                if (ctx->normals) {
                    ndst[v][0] = ctx->normals[3 * corner + 0];
                    ndst[v][1] = ctx->normals[3 * corner + 1];
                    ndst[v][2] = ctx->normals[3 * corner + 2];
                }
                ndst[v][3] = pos[v][0] * c.bitangent[0] +
                             pos[v][1] * c.bitangent[1] +
                             pos[v][2] * c.bitangent[2];  // plane V
            }
            if (!corners_ok) continue;
            if (!ctx->normals) {
                // Geometric face normal fallback (no vertex normals).
                const float e1[3] = {pos[1][0] - pos[0][0],
                                     pos[1][1] - pos[0][1],
                                     pos[1][2] - pos[0][2]};
                const float e2[3] = {pos[2][0] - pos[0][0],
                                     pos[2][1] - pos[0][1],
                                     pos[2][2] - pos[0][2]};
                const float fn[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                                     e1[2] * e2[0] - e1[0] * e2[2],
                                     e1[0] * e2[1] - e1[1] * e2[0]};
                for (int v = 0; v < 3; ++v) {
                    ndst[v][0] = fn[0];
                    ndst[v][1] = fn[1];
                    ndst[v][2] = fn[2];
                }
            }
            uint32_t mat = ctx->material_ids ? ctx->material_ids[corner0]
                                             : ctx->dominant_material;
            if (mat == 0xFFFFFFFFu) {
                mat = (ctx->dominant_material != 0xFFFFFFFFu)
                          ? ctx->dominant_material
                          : 0u;
            }
            g_tri.mat[0] = mat & 0xFFu;
            if (has_tape) {
                // Pack each corner's u8 weight columns: 2 u32 per vertex,
                // little-endian within the u32 (column k at bit 8*(k&3)).
                const auto pack_pair = [&](uint32_t corner, uint32_t out[2]) {
                    out[0] = 0;
                    out[1] = 0;
                    const uint8_t* w =
                        ctx->surface_weights + size_t(corner) * tape_cols;
                    for (uint32_t k = 0; k < tape_cols; ++k)
                        out[k >> 2] |= uint32_t(w[k]) << ((k & 3u) * 8u);
                };
                uint32_t pair[2];
                pack_pair(corners[0], pair);
                g_tri.wA[0] = pair[0];
                g_tri.wA[1] = pair[1];
                pack_pair(corners[1], pair);
                g_tri.wA[2] = pair[0];
                g_tri.wA[3] = pair[1];
                pack_pair(corners[2], pair);
                g_tri.wB[0] = pair[0];
                g_tri.wB[1] = pair[1];
            }
            gtris.push_back(g_tri);
            ++emitted;
        }
        g.tri_range[1] = emitted;
        g.tri_range[2] = 0;
        g.tri_range[3] = 0;
    }
    if (gtris.empty()) return nullptr;

    MeshEntry entry;
    std::string err;
    const VkDeviceSize charts_bytes = sizeof(GpuChart) * gcharts.size();
    const VkDeviceSize tris_bytes = sizeof(GpuTri) * gtris.size();
    if (!create_raw_buffer(device, phys, charts_bytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                           entry.charts, err) ||
        !create_raw_buffer(device, phys, tris_bytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                           entry.tris, err)) {
        destroy_raw_buffer(device, entry.charts);
        destroy_raw_buffer(device, entry.tris);
        return nullptr;
    }
    std::memcpy(entry.charts.mapped, gcharts.data(), charts_bytes);
    std::memcpy(entry.tris.mapped, gtris.data(), tris_bytes);
    entry.chart_count = static_cast<uint32_t>(gcharts.size());

    VkDescriptorSetAllocateInfo alloc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptor_pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &mesh_layout;
    if (vkAllocateDescriptorSets(device, &alloc, &entry.set) != VK_SUCCESS) {
        destroy_raw_buffer(device, entry.charts);
        destroy_raw_buffer(device, entry.tris);
        return nullptr;
    }
    VkDescriptorBufferInfo charts_info{entry.charts.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo tris_info{entry.tris.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2];
    for (int i = 0; i < 2; ++i) {
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = entry.set;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    writes[0].pBufferInfo = &charts_info;
    writes[1].pBufferInfo = &tris_info;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    ++stats.mesh_cache_builds;
    auto inserted = mesh_cache.emplace(key, std::move(entry));
    return &inserted.first->second;
}

void VtCompositor::Impl::record_init(VkCommandBuffer cmd) {
    // Intermediates: UNDEFINED -> GENERAL (they stay GENERAL forever; GENERAL
    // is a valid transfer-src layout, which keeps the aux copy barrier-only).
    for (Ring& r : rings) {
        for (RawImage* img : {&r.inter_albedo, &r.inter_normal, &r.inter_orm,
                              &r.inter_aux}) {
            cmd_image_barrier(cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                              kBatchStride);
        }
    }
    // Dummy tileset: clear to neutral 0.5 gray, then SHADER_READ_ONLY.
    cmd_image_barrier(cmd, dummy_tileset.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
    VkClearColorValue neutral{};
    neutral.float32[0] = 0.5f;
    neutral.float32[1] = 0.5f;
    neutral.float32[2] = 0.5f;
    neutral.float32[3] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, dummy_tileset.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &neutral, 1,
                         &range);
    cmd_image_barrier(cmd, dummy_tileset.image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);
    init_recorded = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
VtCompositor::VtCompositor(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

VtCompositor::~VtCompositor() = default;

std::unique_ptr<VtCompositor> VtCompositor::create(
    VkDevice device, VkPhysicalDevice physical_device,
    VkPipelineCache pipeline_cache, std::string& err) {
    if (!device || !physical_device) {
        err = "vt_compositor: null device handles";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->device = device;
    impl->phys = physical_device;
    impl->pipeline_cache = pipeline_cache;
    if (!impl->init(err)) return nullptr;
    return std::unique_ptr<VtCompositor>(new VtCompositor(std::move(impl)));
}

bool VtCompositor::set_tilesets(const VtTilesetSlotViews* slots, uint32_t count,
                                std::string& err) {
    if (count > kMaxDetailSlots) {
        err = "vt_compositor: too many tileset slots";
        return false;
    }
    for (uint32_t i = 0; i < kMaxDetailSlots; ++i)
        impl_->tileset_slots[i] =
            (i < count) ? slots[i] : VtTilesetSlotViews{};
    impl_->tileset_slot_count = count;
    auto* params = static_cast<VtParamsUbo*>(impl_->params_buf.mapped);
    for (uint32_t i = 0; i < kMaxDetailSlots; ++i) {
        params->tile_size_m[i] =
            std::max(impl_->tileset_slots[i].tile_size_m, 1e-4f);
        params->texels_per_meter[i] =
            std::max(impl_->tileset_slots[i].texels_per_meter, 1e-4f);
    }
    impl_->write_tileset_descriptors();
    return true;
}

void VtCompositor::set_materials(const VtCompositorMaterial* materials,
                                 uint32_t count) {
    auto* mats = static_cast<GpuVtMaterial*>(impl_->materials_buf.mapped);
    for (uint32_t i = 0; i < kMaxMaterials; ++i) {
        VtCompositorMaterial m =
            (i < count) ? materials[i] : VtCompositorMaterial{};
        mats[i].albedo[0] = m.albedo[0];
        mats[i].albedo[1] = m.albedo[1];
        mats[i].albedo[2] = m.albedo[2];
        mats[i].albedo[3] = m.albedo[3];
        mats[i].orm[0] = m.orm[0];
        mats[i].orm[1] = m.orm[1];
        mats[i].orm[2] = m.orm[2];
        mats[i].orm[3] = 0.0f;
        mats[i].slot[0] =
            (m.detail_slot >= 0 &&
             m.detail_slot < static_cast<int>(kMaxDetailSlots))
                ? m.detail_slot
                : -1;
        mats[i].slot[1] = mats[i].slot[2] = mats[i].slot[3] = 0;
    }
}

void VtCompositor::set_weight_mode(WeightMode mode, uint32_t debug_mat_a,
                                   uint32_t debug_mat_b,
                                   float debug_blend_start_m,
                                   float debug_blend_width_m) {
    impl_->weight_mode = mode;
    impl_->debug_mat_a = debug_mat_a;
    impl_->debug_mat_b = debug_mat_b;
    impl_->debug_blend_start = debug_blend_start_m;
    impl_->debug_blend_width = debug_blend_width_m;
}

void VtCompositor::invalidate_part(uint64_t variant_hash) {
    for (auto it = impl_->mesh_cache.begin(); it != impl_->mesh_cache.end();) {
        if (it->first.first == variant_hash) {
            if (it->second.set)
                vkFreeDescriptorSets(impl_->device, impl_->descriptor_pool, 1,
                                     &it->second.set);
            destroy_raw_buffer(impl_->device, it->second.charts);
            destroy_raw_buffer(impl_->device, it->second.tris);
            it = impl_->mesh_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void VtCompositor::fill(VkCommandBuffer cmd, const VtFillRequest* batch,
                        size_t count) {
    Impl& im = *impl_;
    if (!im.init_recorded) im.record_init(cmd);
    if (!batch || count == 0) return;

    Impl::Ring& ring = im.rings[im.ring_cursor];
    im.ring_cursor = (im.ring_cursor + 1) % kMaxBatchesInFlight;

    struct Rec {
        const Impl::MeshEntry* entry;
        const VtPoolBinding* pool;
        uint32_t req_index;      // slot in the ring's request buffer
        uint32_t physical_slot;
    };
    std::vector<Rec> recs;
    recs.reserve(std::min<size_t>(count, kMaxRequestsPerFill));

    auto* gpu_reqs = static_cast<GpuFillRequest*>(ring.requests.mapped);
    auto* gpu_cands = static_cast<uint32_t*>(ring.cands.mapped);
    uint32_t cand_cursor = 0;

    for (size_t i = 0; i < count; ++i) {
        const VtFillRequest& req = batch[i];
        const VtPoolBinding* pool = req.pool;
        if (!req.atlas || !req.part_context || !pool ||
            !pool->image[kVtChannelAlbedo] || !pool->image[kVtChannelNormal] ||
            !pool->image[kVtChannelOrm] || !pool->image[kVtChannelAux] ||
            recs.size() >= kMaxRequestsPerFill) {
            ++stats_.requests_skipped;
            continue;
        }
        {
            uint32_t layer, sx, sy;
            vt_slot_origin(req.physical_slot, layer, sx, sy);
            if (pool->layer_count != 0 && layer >= pool->layer_count) {
                ++stats_.requests_skipped;
                continue;
            }
        }
        const auto* ctx = static_cast<const VtPartContext*>(req.part_context);
        if (!ctx->positions || !ctx->indices || ctx->triangle_count == 0) {
            ++stats_.requests_skipped;
            continue;
        }
        Impl::MeshEntry* entry = im.get_or_build_mesh_entry(
            req.variant_hash, req.rung, req.atlas, ctx, stats_);
        if (!entry) {
            ++stats_.requests_skipped;
            continue;
        }

        // Candidate charts: rects intersecting the page's finest-mip
        // footprint expanded by a dilation margin; ascending chart index
        // (fixed order — determinism). Empty -> the nearest chart by rect
        // distance so dilation always has content.
        const int64_t mip_scale = int64_t(1) << req.mip;
        const int64_t page_lo_x =
            (int64_t(req.page_x) * chart_atlas::kVtPagePayload -
             chart_atlas::kVtPageBorder) * mip_scale;
        const int64_t page_lo_y =
            (int64_t(req.page_y) * chart_atlas::kVtPagePayload -
             chart_atlas::kVtPageBorder) * mip_scale;
        const int64_t page_hi_x =
            page_lo_x + int64_t(kPageStore) * mip_scale;
        const int64_t page_hi_y =
            page_lo_y + int64_t(kPageStore) * mip_scale;
        const int64_t margin = int64_t(32) * mip_scale;

        const uint32_t cand_offset = cand_cursor;
        uint32_t cand_count = 0;
        int64_t nearest_d2 = INT64_MAX;
        uint32_t nearest_chart = 0;
        const auto& charts = req.atlas->charts;
        for (size_t ci = 0; ci < charts.size(); ++ci) {
            const chart_atlas::ChartEntry& c = charts[ci];
            const int64_t clo_x = c.rect_x, clo_y = c.rect_y;
            const int64_t chi_x = clo_x + c.rect_w, chi_y = clo_y + c.rect_h;
            const int64_t dx = std::max<int64_t>(
                0, std::max(page_lo_x - margin - chi_x,
                            clo_x - (page_hi_x + margin)));
            const int64_t dy = std::max<int64_t>(
                0, std::max(page_lo_y - margin - chi_y,
                            clo_y - (page_hi_y + margin)));
            if (dx == 0 && dy == 0) {
                if (cand_cursor < kMaxCandEntriesPerFill) {
                    gpu_cands[cand_cursor++] = static_cast<uint32_t>(ci);
                    ++cand_count;
                }
            } else {
                const int64_t d2 = dx * dx + dy * dy;
                if (d2 < nearest_d2) {
                    nearest_d2 = d2;
                    nearest_chart = static_cast<uint32_t>(ci);
                }
            }
        }
        if (cand_count == 0 && !charts.empty() &&
            cand_cursor < kMaxCandEntriesPerFill) {
            gpu_cands[cand_cursor++] = nearest_chart;
            cand_count = 1;
        }
        if (cand_count == 0) {
            ++stats_.requests_skipped;
            continue;
        }

        const uint32_t rec_index = static_cast<uint32_t>(recs.size());
        GpuFillRequest& g = gpu_reqs[rec_index];
        g.a[0] = req.page_x;
        g.a[1] = req.page_y;
        g.a[2] = req.mip;
        g.a[3] = rec_index % kBatchStride;   // intermediate layer in group
        g.b[0] = cand_offset;
        g.b[1] = cand_count;
        // WP-F: a part carrying surfaces()-tape weights promotes the resting
        // default to the tape mode per request; the debug-ramp test override
        // still wins so the WP-D goldens keep exercising their fixed path.
        const bool has_tape =
            ctx->surface_material_count > 0 &&
            ctx->surface_material_count <= kMaxSurfaceMaterials &&
            ctx->surface_weights != nullptr &&
            ctx->surface_materials != nullptr;
        const WeightMode mode =
            (has_tape && im.weight_mode == WeightMode::kTriangleMaterial)
                ? WeightMode::kSurfaceTape
                : im.weight_mode;
        g.b[2] = static_cast<uint32_t>(mode);
        g.b[3] = (im.debug_mat_a & 0xFFFFu) | (im.debug_mat_b << 16);
        g.debug_params[0] = im.debug_blend_start;
        g.debug_params[1] = im.debug_blend_width;
        g.debug_params[2] = 0.0f;
        g.debug_params[3] = 0.0f;
        g.tape[0] = 0;
        g.tape[1] = 0;
        g.tape[2] = 0;
        g.tape[3] = 0;
        if (has_tape) {
            for (uint32_t k = 0; k < ctx->surface_material_count; ++k)
                g.tape[k >> 2] |= (ctx->surface_materials[k] & 0xFFu)
                                  << ((k & 3u) * 8u);
            g.tape[2] = ctx->surface_material_count;
        }
        recs.push_back(Rec{entry, pool, rec_index, req.physical_slot});
    }
    if (recs.empty()) return;

    for (size_t group_start = 0; group_start < recs.size();
         group_start += kBatchStride) {
        const size_t group_end =
            std::min(recs.size(), group_start + kBatchStride);

        // Prior group's encodes/copies (and any prior batch in this ring
        // slot) must complete before this group's composite writes reuse the
        // intermediates and block buffers.
        cmd_memory_barrier(
            cmd,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                VK_ACCESS_2_UNIFORM_READ_BIT |
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          im.composite_pipe);
        for (size_t r = group_start; r < group_end; ++r) {
            const Rec& rec = recs[r];
            VkDescriptorSet sets[2] = {rec.entry->set, ring.batch_set};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    im.composite_pl, 0, 2, sets, 0, nullptr);
            vkCmdPushConstants(cmd, im.composite_pl,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, 4,
                               &rec.req_index);
            vkCmdDispatch(cmd, kPageStore / 8, kPageStore / 8, 1);
        }

        cmd_memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.encode_pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                im.encode_pl, 0, 1, &ring.encode_set, 0,
                                nullptr);
        for (size_t r = group_start; r < group_end; ++r) {
            const uint32_t group_slot =
                static_cast<uint32_t>(r - group_start);
            const uint32_t push[2] = {group_slot, group_slot};
            vkCmdPushConstants(cmd, im.encode_pl, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, 8, push);
            const uint32_t groups =
                (kBlocksPerAxis + 7) / 8;   // 34 blocks -> 5 groups
            vkCmdDispatch(cmd, groups, groups, 1);
        }

        cmd_memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT);

        for (size_t r = group_start; r < group_end; ++r) {
            const Rec& rec = recs[r];
            const uint32_t group_slot =
                static_cast<uint32_t>(r - group_start);
            uint32_t dst_layer, sx, sy;
            vt_slot_origin(rec.physical_slot, dst_layer, sx, sy);
            const int32_t dst_x = static_cast<int32_t>(sx);
            const int32_t dst_y = static_cast<int32_t>(sy);
            const VkImageLayout dst_layout =
                rec.pool->transfer_dst_layout
                    ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                    : VK_IMAGE_LAYOUT_GENERAL;

            VkBufferImageCopy region{};
            region.bufferOffset =
                VkDeviceSize(group_slot) * kBlocksPerPage * 16;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, dst_layer,
                                       1};
            region.imageOffset = {dst_x, dst_y, 0};
            region.imageExtent = {kPageStore, kPageStore, 1};
            vkCmdCopyBufferToImage(cmd, ring.out_albedo.buffer,
                                   rec.pool->image[kVtChannelAlbedo],
                                   dst_layout, 1, &region);
            vkCmdCopyBufferToImage(cmd, ring.out_normal.buffer,
                                   rec.pool->image[kVtChannelNormal],
                                   dst_layout, 1, &region);
            vkCmdCopyBufferToImage(cmd, ring.out_orm.buffer,
                                   rec.pool->image[kVtChannelOrm],
                                   dst_layout, 1, &region);

            VkImageCopy aux{};
            aux.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, group_slot, 1};
            aux.srcOffset = {0, 0, 0};
            aux.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, dst_layer, 1};
            aux.dstOffset = {dst_x, dst_y, 0};
            aux.extent = {kPageStore, kPageStore, 1};
            vkCmdCopyImage(cmd, ring.inter_aux.image, VK_IMAGE_LAYOUT_GENERAL,
                           rec.pool->image[kVtChannelAux], dst_layout, 1,
                           &aux);
            ++stats_.pages_filled;
        }
    }
}

}  // namespace vt
