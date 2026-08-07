// vt_enrich.cpp — WP-H tier-2 hemisphere AO page enrichment.
// See vt_enrich.h for the module contract and shaders_vk/vt_enrich_ao.comp for
// the trace itself. The BC re-encode reuses vt_bc_encode.comp verbatim (the
// same fast mode-6 BC7 tier tier-1 pages go through), so this module adds one
// shader, not two.

#include "vt_enrich.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

#include "matter/vt_budgets.h"
#include "matter/vulkan_device.h"
#include "shaders_gen/embedded_spirv.h"
#include "vk_resources.h"
#include "vt_chart_gpu.h"

namespace vt {

namespace {

// std430 mirror of vt_enrich_ao.comp's GpuEnrichRequest.
struct GpuEnrichRequest {
    uint32_t a[4];   // page_x, page_y, mip, out_layer
    uint32_t b[4];   // cand_offset, cand_count, sample_count, seed
    uint32_t c[4];   // slot_origin_x, slot_origin_y, pool_layer, unused
    float    d[4];   // strength, cap_texels, cap_meters, min_ao
};
static_assert(sizeof(GpuEnrichRequest) == 64, "GpuEnrichRequest layout");

constexpr uint32_t kMaxCandEntriesPerBatch = 8192;
// Strength reaches 0 when a page texel is this multiple of the absolute cap
// wide. MUST match VT_ENRICH_FADE_SPAN in vt_enrich_ao.comp; the residency
// layer's coarse-page skip reads it through max_footprint_meters().
constexpr float kEnrichFadeSpan = 4.0f;
// Frames a retired acceleration structure waits before destruction. The
// residency layer's frame counter advances once per recorded frame and at most
// a handful of frames are in flight, so 8 is comfortably past retirement.
constexpr uint64_t kRetireFrames = 8;

// The private env_u32 / env_f32 pair this file used to carry is gone: the six
// MATTER_VT_ENRICH_* vars now live in matter::VtEnrichSettings
// (matter/vt_budgets.h) and reach this file through props::apply_env, so the
// clamps below are the schema's ranges restated for the no-registry case.
uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float clamp_f32(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// The live enrichment parameters, clamped, as of THIS call. Read per batch:
// every field feeds a push constant, so an editor edit lands on the next
// enriched page with no reload.
matter::VtEnrichSettings live_enrich_settings() {
    matter::ensure_vt_enrich_env_applied();
    matter::VtEnrichSettings s = matter::vt_enrich_settings();
    s.samples = clamp_u32(s.samples, 8u, 64u);
    s.strength = clamp_f32(s.strength, 0.0f, 1.0f);
    s.cap_texels = clamp_f32(s.cap_texels, 0.25f, 64.0f);
    s.cap_meters = clamp_f32(s.cap_meters, 0.02f, 64.0f);
    s.min_ao = clamp_f32(s.min_ao, 0.0f, 1.0f);
    return s;
}

// matter::create_image only supports a single mip level and array layer, so the
// multi-layer intermediate is built directly (same reasoning as the tileset
// images in vk_scene_renderer.h).
struct RawImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

bool create_raw_image_array(matter::VulkanDevice& vulkan, uint32_t width,
                            uint32_t height, uint32_t layers, VkFormat format,
                            VkImageUsageFlags usage, RawImage& out,
                            std::string& err) {
    const VkDevice device = vulkan.device();
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
        err = "vt_enrich: vkCreateImage failed";
        return false;
    }
    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(device, out.image, &reqs);
    uint32_t type = 0;
    VkMemoryPropertyFlags selected = 0;
    if (!matter::find_memory_type(vulkan.physical_device(), reqs.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type,
                                  selected, err)) {
        vkDestroyImage(device, out.image, nullptr);
        out.image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, out.image, out.memory, 0) != VK_SUCCESS) {
        err = "vt_enrich: image memory allocation failed";
        if (out.memory) vkFreeMemory(device, out.memory, nullptr);
        vkDestroyImage(device, out.image, nullptr);
        out = RawImage{};
        return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    if (vkCreateImageView(device, &view, nullptr, &out.view) != VK_SUCCESS) {
        err = "vt_enrich: vkCreateImageView failed";
        vkFreeMemory(device, out.memory, nullptr);
        vkDestroyImage(device, out.image, nullptr);
        out = RawImage{};
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

VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    if (alignment <= 1) return value;
    return (value + alignment - 1) / alignment * alignment;
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct VtEnricher::Impl {
    matter::VulkanDevice* vulkan = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

    PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build = nullptr;
    VkDeviceSize scratch_align = 1;

    VkDescriptorSetLayout variant_layout = VK_NULL_HANDLE;   // set 0
    VkDescriptorSetLayout batch_layout = VK_NULL_HANDLE;     // set 1
    VkDescriptorSetLayout encode_layout = VK_NULL_HANDLE;
    VkPipelineLayout enrich_pl = VK_NULL_HANDLE;
    VkPipelineLayout encode_pl = VK_NULL_HANDLE;
    VkPipeline enrich_pipe = VK_NULL_HANDLE;
    VkPipeline encode_pipe = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkSampler point_sampler = VK_NULL_HANDLE;

    // sample_count / strength / cap_texels / cap_meters / min_ao moved to
    // matter::VtEnrichSettings (matter/vt_budgets.h): every one of them is a
    // push-constant input read per enrich() batch, so reading them from the
    // settings struct at the use site makes them genuinely live-editable
    // instead of latched at init. See live_enrich_settings() above.
    //
    // Fixed constant, NOT derived from time or frame: the whole determinism
    // property rests on this.
    uint32_t seed = 0x5D7C9A31u;
    // as_cache_cap is the one that stays a member: it sizes the descriptor
    // pool at init (variant_sets = as_cache_cap + 32), so a later change would
    // desync the cap from the pool it was allocated against. ReadOnly.
    uint32_t as_cache_cap = 8;
    // Frame index of the most recent recorded batch, across BOTH the AO and
    // directional entry points. invalidate_part has no frame of its own, and
    // stamping a retirement with anything older than the last recorded batch
    // would retire it before that batch can complete.
    uint64_t last_frame_index = 0;

    struct Ring {
        matter::VkBufferResource requests;
        matter::VkBufferResource cands;
        matter::VkBufferResource blocks[3];   // albedo/normal/orm; only ORM used
        RawImage inter;                       // rgba8 array, one layer/request
        VkDescriptorSet batch_set = VK_NULL_HANDLE;
        VkDescriptorSet encode_set = VK_NULL_HANDLE;
    };
    Ring rings[kMaxBatchesInFlight];
    uint32_t ring_cursor = 0;

    struct VariantEntry {
        matter::VkBufferResource charts;
        matter::VkBufferResource tris;
        matter::VkBufferResource as_vertices;
        matter::VkBufferResource as_indices;
        matter::VkBufferResource as_instances;
        matter::VkBufferResource blas_scratch;
        matter::VkBufferResource tlas_scratch;
        matter::VkAccelerationStructureResource blas;
        matter::VkAccelerationStructureResource tlas;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t primitive_count = 0;
        uint32_t vertex_count = 0;
        uint64_t bytes = 0;
        uint64_t last_used = 0;
        bool built = false;      // false => the AS build still has to be recorded
    };
    std::map<std::pair<uint64_t, uint32_t>, VariantEntry> variants;

    struct Retired {
        VariantEntry entry;
        uint64_t frame = 0;
    };
    std::vector<Retired> graveyard;

    VkImageView bound_pool_orm = VK_NULL_HANDLE;
    bool init_recorded = false;

    // Scratch reused across enrich() calls (no per-call allocation churn).
    std::vector<GpuChart> scratch_charts;
    std::vector<GpuTri> scratch_tris;
    std::vector<uint32_t> scratch_cands;

    ~Impl() { destroy(); }

    void destroy() {
        if (!device) return;
        for (auto& kv : variants) free_variant_set(kv.second);
        variants.clear();
        for (Retired& r : graveyard) free_variant_set(r.entry);
        graveyard.clear();
        for (Ring& r : rings) destroy_raw_image(device, r.inter);
        if (point_sampler) vkDestroySampler(device, point_sampler, nullptr);
        if (descriptor_pool)
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        if (enrich_pipe) vkDestroyPipeline(device, enrich_pipe, nullptr);
        if (encode_pipe) vkDestroyPipeline(device, encode_pipe, nullptr);
        if (enrich_pl) vkDestroyPipelineLayout(device, enrich_pl, nullptr);
        if (encode_pl) vkDestroyPipelineLayout(device, encode_pl, nullptr);
        if (variant_layout)
            vkDestroyDescriptorSetLayout(device, variant_layout, nullptr);
        if (batch_layout)
            vkDestroyDescriptorSetLayout(device, batch_layout, nullptr);
        if (encode_layout)
            vkDestroyDescriptorSetLayout(device, encode_layout, nullptr);
        device = VK_NULL_HANDLE;
    }

    void free_variant_set(VariantEntry& e) {
        if (e.set && descriptor_pool)
            vkFreeDescriptorSets(device, descriptor_pool, 1, &e.set);
        e.set = VK_NULL_HANDLE;
    }

    bool init(std::string& err);
    bool create_pipeline(const char* spirv_name, VkPipelineLayout layout,
                         VkPipeline& out, std::string& err);
    void write_ring_descriptors(Ring& r);
    void bind_pool_orm(VkImageView view);
    VariantEntry* get_or_build_variant(uint64_t variant_hash, uint32_t rung,
                                       const chart_atlas::ChartAtlasRung* atlas,
                                       const VtPartContext* ctx,
                                       uint64_t frame_index, Stats& stats);
    bool build_acceleration_structures(VariantEntry& e, const VtPartContext* ctx,
                                       std::string& err);
    void record_as_build(VkCommandBuffer cmd, VariantEntry& e);
    void record_init(VkCommandBuffer cmd);
    void retire(uint64_t frame_index);
    void evict_lru(uint64_t frame_index, Stats& stats);

};

bool VtEnricher::Impl::create_pipeline(const char* spirv_name,
                                      VkPipelineLayout layout, VkPipeline& out,
                                      std::string& err) {
    const matter::EmbeddedSpirvView spirv = matter::find_spirv(spirv_name);
    if (!spirv.words || spirv.word_count == 0) {
        err = std::string("vt_enrich: embedded SPIR-V not found: ") + spirv_name;
        return false;
    }
    VkShaderModuleCreateInfo mod{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mod.codeSize = spirv.word_count * sizeof(uint32_t);
    mod.pCode = spirv.words;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &mod, nullptr, &module) != VK_SUCCESS) {
        err = "vt_enrich: vkCreateShaderModule failed";
        return false;
    }
    VkComputePipelineCreateInfo info{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout;
    const VkResult result =
        vkCreateComputePipelines(device, pipeline_cache, 1, &info, nullptr, &out);
    vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS) {
        err = "vt_enrich: vkCreateComputePipelines failed";
        return false;
    }
    return true;
}

bool VtEnricher::Impl::init(std::string& err) {
    // Layer 5 for standalone engine runs (headless tests, tools) that never
    // bind a registry; the editor binds the SAME struct and runs its own
    // apply_env — both idempotent, both reading the same environment.
    matter::ensure_vt_enrich_env_applied();
    as_cache_cap = clamp_u32(matter::vt_enrich_settings().as_cache, 1u, 64u);

    get_sizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
    cmd_build = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
    if (!get_sizes || !cmd_build) {
        err = "vt_enrich: acceleration-structure build entry points unavailable";
        return false;
    }
    scratch_align = std::max<VkDeviceSize>(
        1, vulkan->ray_tracing_properties()
               .min_acceleration_structure_scratch_offset_alignment);

    auto binding = [](uint32_t idx, VkDescriptorType type, uint32_t count) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = idx;
        b.descriptorType = type;
        b.descriptorCount = count;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        return b;
    };
    auto make_layout = [&](const std::vector<VkDescriptorSetLayoutBinding>& b,
                           VkDescriptorSetLayout& out) -> bool {
        VkDescriptorSetLayoutCreateInfo info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = static_cast<uint32_t>(b.size());
        info.pBindings = b.data();
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &out) !=
            VK_SUCCESS) {
            err = "vt_enrich: vkCreateDescriptorSetLayout failed";
            return false;
        }
        return true;
    };

    if (!make_layout(
            {binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
             binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
             binding(2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1)},
            variant_layout))
        return false;
    if (!make_layout({binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1),
                      binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1)},
                     batch_layout))
        return false;
    // vt_bc_encode.comp's set 0, unchanged.
    if (!make_layout({binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1),
                      binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1),
                      binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1),
                      binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1)},
                     encode_layout))
        return false;

    {
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkDescriptorSetLayout sets[2] = {variant_layout, batch_layout};
        VkPipelineLayoutCreateInfo info{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        info.setLayoutCount = 2;
        info.pSetLayouts = sets;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device, &info, nullptr, &enrich_pl) !=
            VK_SUCCESS) {
            err = "vt_enrich: vkCreatePipelineLayout (enrich) failed";
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
            err = "vt_enrich: vkCreatePipelineLayout (encode) failed";
            return false;
        }
    }

    if (!create_pipeline("vt_enrich_ao.comp.spv", enrich_pl, enrich_pipe, err))
        return false;
    if (!create_pipeline("vt_bc_encode.comp.spv", encode_pl, encode_pipe, err))
        return false;

    {
        // NEAREST + CLAMP: the shader fetches exact pool texel centres, and a
        // linear filter would smear neighbouring pages' border content into the
        // value it multiplies.
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = VK_FILTER_NEAREST;
        info.minFilter = VK_FILTER_NEAREST;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 0.25f;
        if (vkCreateSampler(device, &info, nullptr, &point_sampler) !=
            VK_SUCCESS) {
            err = "vt_enrich: vkCreateSampler failed";
            return false;
        }
    }

    {
        // Live cache entries plus whatever is waiting in the deferred-destroy
        // graveyard (which keeps its descriptor sets until retirement).
        const uint32_t variant_sets = as_cache_cap + 32u;
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             variant_sets * 2 + kMaxBatchesInFlight * 5},
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, variant_sets},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxBatchesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxBatchesInFlight * 4},
        };
        VkDescriptorPoolCreateInfo info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = variant_sets + kMaxBatchesInFlight * 2;
        info.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
        info.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &info, nullptr, &descriptor_pool) !=
            VK_SUCCESS) {
            err = "vt_enrich: vkCreateDescriptorPool failed";
            return false;
        }
    }

    for (Ring& r : rings) {
        if (!matter::create_buffer(
                *vulkan, sizeof(GpuEnrichRequest) * kMaxRequestsPerBatch,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, r.requests, err) ||
            !matter::map_buffer(r.requests, err))
            return false;
        if (!matter::create_buffer(*vulkan,
                                   sizeof(uint32_t) * kMaxCandEntriesPerBatch,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   r.cands, err) ||
            !matter::map_buffer(r.cands, err))
            return false;
        const VkDeviceSize block_bytes =
            VkDeviceSize(kMaxRequestsPerBatch) * kBlocksPerPage * 16;
        for (matter::VkBufferResource& block : r.blocks) {
            if (!matter::create_buffer(*vulkan, block_bytes,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                                       block, err))
                return false;
        }
        if (!create_raw_image_array(*vulkan, kPageStore, kPageStore,
                                    kMaxRequestsPerBatch,
                                    VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_USAGE_STORAGE_BIT, r.inter, err))
            return false;
        VkDescriptorSetLayout layouts[2] = {batch_layout, encode_layout};
        VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkDescriptorSetAllocateInfo alloc{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptor_pool;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(device, &alloc, sets) != VK_SUCCESS) {
            err = "vt_enrich: vkAllocateDescriptorSets (ring) failed";
            return false;
        }
        r.batch_set = sets[0];
        r.encode_set = sets[1];
        write_ring_descriptors(r);
    }
    return true;
}

void VtEnricher::Impl::write_ring_descriptors(Ring& r) {
    VkDescriptorBufferInfo requests{r.requests.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo cands{r.cands.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo inter{VK_NULL_HANDLE, r.inter.view,
                                VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo blocks[3] = {
        {r.blocks[0].buffer, 0, VK_WHOLE_SIZE},
        {r.blocks[1].buffer, 0, VK_WHOLE_SIZE},
        {r.blocks[2].buffer, 0, VK_WHOLE_SIZE},
    };

    std::vector<VkWriteDescriptorSet> writes;
    auto write_buf = [&](VkDescriptorSet set, uint32_t bind,
                         const VkDescriptorBufferInfo* info) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = bind;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
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

    write_buf(r.batch_set, 0, &requests);
    write_buf(r.batch_set, 1, &cands);
    // binding 2 (the pool ORM sampler) is written lazily by bind_pool_orm.
    write_img(r.batch_set, 3, &inter);
    // The encode shader compresses albedo/normal/ORM from three source images;
    // only the ORM result is copied back, so the same intermediate is bound to
    // all three (read-only) and the other two block buffers are scratch.
    write_img(r.encode_set, 0, &inter);
    write_img(r.encode_set, 1, &inter);
    write_img(r.encode_set, 2, &inter);
    write_buf(r.encode_set, 3, &blocks[0]);
    write_buf(r.encode_set, 4, &blocks[1]);
    write_buf(r.encode_set, 5, &blocks[2]);
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void VtEnricher::Impl::bind_pool_orm(VkImageView view) {
    if (view == bound_pool_orm) return;
    // The residency layer creates the pool exactly once in init() and installs
    // the enricher afterwards, so in practice this runs on the first enrich()
    // and never again. A later change would require the caller's idle bracket
    // (documented in vt_enrich.h).
    VkDescriptorImageInfo info{point_sampler, view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[kMaxBatchesInFlight];
    for (uint32_t i = 0; i < kMaxBatchesInFlight; ++i) {
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = rings[i].batch_set;
        writes[i].dstBinding = 2;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &info;
    }
    vkUpdateDescriptorSets(device, kMaxBatchesInFlight, writes, 0, nullptr);
    bound_pool_orm = view;
}

bool VtEnricher::Impl::build_acceleration_structures(VariantEntry& e,
                                                     const VtPartContext* ctx,
                                                     std::string& err) {
    const uint32_t vertices = ctx->vertex_count;
    const uint32_t triangles = ctx->triangle_count;
    if (vertices == 0 || triangles == 0 || !ctx->positions || !ctx->indices) {
        err = "vt_enrich: rung mesh has no geometry";
        return false;
    }
    e.vertex_count = vertices;
    e.primitive_count = triangles;

    const VkBufferUsageFlags as_input =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VkDeviceSize vertex_bytes =
        VkDeviceSize(vertices) * 3u * sizeof(float);
    const VkDeviceSize index_bytes =
        VkDeviceSize(triangles) * 3u * sizeof(uint32_t);
    if (!matter::create_buffer(*vulkan, vertex_bytes, as_input,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               e.as_vertices, err) ||
        !matter::map_buffer(e.as_vertices, err))
        return false;
    std::memcpy(e.as_vertices.mapped, ctx->positions,
                static_cast<size_t>(vertex_bytes));
    if (!matter::create_buffer(*vulkan, index_bytes, as_input,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               e.as_indices, err) ||
        !matter::map_buffer(e.as_indices, err))
        return false;
    std::memcpy(e.as_indices.mapped, ctx->indices,
                static_cast<size_t>(index_bytes));

    VkAccelerationStructureGeometryKHR geom{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geom.geometry.triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress = e.as_vertices.address;
    geom.geometry.triangles.vertexStride = 3 * sizeof(float);
    geom.geometry.triangles.maxVertex = vertices - 1u;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geom.geometry.triangles.indexData.deviceAddress = e.as_indices.address;

    VkAccelerationStructureBuildGeometryInfoKHR build{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    get_sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
              &e.primitive_count, &sizes);
    if (!matter::create_acceleration_structure(
            *vulkan, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            sizes.accelerationStructureSize, e.blas, err))
        return false;
    // Over-allocate by (align - 1) so the aligned scratch address still has
    // buildScratchSize bytes of room on an unaligned allocation base (same
    // reasoning as tileset_bake_vk.cpp / emit_ray_instances).
    if (!matter::create_buffer(*vulkan,
                               sizes.buildScratchSize + scratch_align - 1,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                               e.blas_scratch, err))
        return false;

    // One instance, identity transform: the chart table and the mesh are both
    // in part-local space, so the AS is queried in exactly that space.
    VkAccelerationStructureInstanceKHR instance{};
    instance.transform.matrix[0][0] = 1.0f;
    instance.transform.matrix[1][1] = 1.0f;
    instance.transform.matrix[2][2] = 1.0f;
    instance.instanceCustomIndex = 0;
    instance.mask = 0xFF;
    instance.instanceShaderBindingTableRecordOffset = 0;
    // Cull-disable: the occlusion query only asks "is there geometry", and a
    // rung mesh's winding is a raster concern, not a visibility one.
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = e.blas.address;
    if (!matter::create_buffer(*vulkan, sizeof(instance), as_input,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               e.as_instances, err) ||
        !matter::map_buffer(e.as_instances, err))
        return false;
    std::memcpy(e.as_instances.mapped, &instance, sizeof(instance));

    VkAccelerationStructureGeometryKHR tlas_geom{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlas_geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geom.geometry.instances = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    tlas_geom.geometry.instances.data.deviceAddress = e.as_instances.address;
    VkAccelerationStructureBuildGeometryInfoKHR tlas_build{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlas_build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlas_build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlas_build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlas_build.geometryCount = 1;
    tlas_build.pGeometries = &tlas_geom;
    const uint32_t instance_count = 1;
    VkAccelerationStructureBuildSizesInfoKHR tlas_sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    get_sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
              &tlas_build, &instance_count, &tlas_sizes);
    if (!matter::create_acceleration_structure(
            *vulkan, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            tlas_sizes.accelerationStructureSize, e.tlas, err))
        return false;
    if (!matter::create_buffer(*vulkan,
                               tlas_sizes.buildScratchSize + scratch_align - 1,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                               e.tlas_scratch, err))
        return false;

    e.bytes = vertex_bytes + index_bytes + sizes.accelerationStructureSize +
              sizes.buildScratchSize + tlas_sizes.accelerationStructureSize +
              tlas_sizes.buildScratchSize;
    return true;
}

void VtEnricher::Impl::record_as_build(VkCommandBuffer cmd, VariantEntry& e) {
    VkAccelerationStructureGeometryKHR geom{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geom.geometry.triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress = e.as_vertices.address;
    geom.geometry.triangles.vertexStride = 3 * sizeof(float);
    geom.geometry.triangles.maxVertex = e.vertex_count - 1u;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geom.geometry.triangles.indexData.deviceAddress = e.as_indices.address;

    VkAccelerationStructureBuildGeometryInfoKHR build{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;
    build.dstAccelerationStructure = e.blas.handle;
    build.scratchData.deviceAddress =
        align_up(e.blas_scratch.address, scratch_align);
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = e.primitive_count;
    const VkAccelerationStructureBuildRangeInfoKHR* range_ptr = &range;
    cmd_build(cmd, 1, &build, &range_ptr);

    cmd_memory_barrier(
        cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);

    VkAccelerationStructureGeometryKHR tlas_geom{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlas_geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geom.geometry.instances = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    tlas_geom.geometry.instances.data.deviceAddress = e.as_instances.address;
    VkAccelerationStructureBuildGeometryInfoKHR tlas_build{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlas_build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlas_build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlas_build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlas_build.geometryCount = 1;
    tlas_build.pGeometries = &tlas_geom;
    tlas_build.dstAccelerationStructure = e.tlas.handle;
    tlas_build.scratchData.deviceAddress =
        align_up(e.tlas_scratch.address, scratch_align);
    VkAccelerationStructureBuildRangeInfoKHR tlas_range{};
    tlas_range.primitiveCount = 1;
    const VkAccelerationStructureBuildRangeInfoKHR* tlas_range_ptr = &tlas_range;
    cmd_build(cmd, 1, &tlas_build, &tlas_range_ptr);

    cmd_memory_barrier(
        cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    e.built = true;
}

VtEnricher::Impl::VariantEntry* VtEnricher::Impl::get_or_build_variant(
    uint64_t variant_hash, uint32_t rung,
    const chart_atlas::ChartAtlasRung* atlas, const VtPartContext* ctx,
    uint64_t frame_index, Stats& stats) {
    const auto key = std::make_pair(variant_hash, rung);
    auto it = variants.find(key);
    if (it != variants.end()) {
        it->second.last_used = frame_index;
        return &it->second;
    }
    if (!vt_build_chart_gpu_streams(*atlas, *ctx, scratch_charts, scratch_tris))
        return nullptr;

    evict_lru(frame_index, stats);

    VariantEntry entry;
    std::string err;
    const VkDeviceSize charts_bytes = sizeof(GpuChart) * scratch_charts.size();
    const VkDeviceSize tris_bytes = sizeof(GpuTri) * scratch_tris.size();
    if (!matter::create_buffer(*vulkan, charts_bytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               entry.charts, err) ||
        !matter::map_buffer(entry.charts, err) ||
        !matter::create_buffer(*vulkan, tris_bytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               entry.tris, err) ||
        !matter::map_buffer(entry.tris, err))
        return nullptr;
    std::memcpy(entry.charts.mapped, scratch_charts.data(),
                static_cast<size_t>(charts_bytes));
    std::memcpy(entry.tris.mapped, scratch_tris.data(),
                static_cast<size_t>(tris_bytes));
    if (!build_acceleration_structures(entry, ctx, err)) return nullptr;
    entry.bytes += charts_bytes + tris_bytes;
    entry.last_used = frame_index;

    VkDescriptorSetAllocateInfo alloc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptor_pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &variant_layout;
    if (vkAllocateDescriptorSets(device, &alloc, &entry.set) != VK_SUCCESS)
        return nullptr;
    VkDescriptorBufferInfo charts_info{entry.charts.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo tris_info{entry.tris.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSetAccelerationStructureKHR as_write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures = &entry.tlas.handle;
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = entry.set;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &charts_info;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &tris_info;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[2].pNext = &as_write;
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    ++stats.as_builds;
    auto inserted = variants.emplace(key, std::move(entry));
    stats.as_cached = static_cast<uint32_t>(variants.size());
    return &inserted.first->second;
}

void VtEnricher::Impl::evict_lru(uint64_t frame_index, Stats& stats) {
    while (variants.size() >= as_cache_cap) {
        // NEVER evict an entry this frame's batch already references: the
        // recording loop holds raw pointers into these map nodes.
        auto victim = variants.end();
        for (auto it = variants.begin(); it != variants.end(); ++it) {
            if (it->second.last_used >= frame_index) continue;
            if (victim == variants.end() ||
                it->second.last_used < victim->second.last_used)
                victim = it;
        }
        if (victim == variants.end()) break;   // all in use this frame
        // Deferred destruction: the victim may still be referenced by an
        // unretired batch, and this runs mid-recording.
        graveyard.push_back(Retired{std::move(victim->second), frame_index});
        variants.erase(victim);
        ++stats.as_evictions;
    }
    stats.as_cached = static_cast<uint32_t>(variants.size());
}

void VtEnricher::Impl::retire(uint64_t frame_index) {
    for (size_t i = graveyard.size(); i-- > 0;) {
        if (frame_index < graveyard[i].frame + kRetireFrames) continue;
        free_variant_set(graveyard[i].entry);
        graveyard.erase(graveyard.begin() + static_cast<long>(i));
    }
}

void VtEnricher::Impl::record_init(VkCommandBuffer cmd) {
    for (Ring& r : rings) {
        cmd_image_barrier(cmd, r.inter.image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                          kMaxRequestsPerBatch);
    }
    init_recorded = true;
}

// ---------------------------------------------------------------------------
// M6.5 directional tier
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
VtEnricher::VtEnricher(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

VtEnricher::~VtEnricher() = default;

std::unique_ptr<VtEnricher> VtEnricher::create(matter::VulkanDevice& vulkan,
                                               VkPipelineCache pipeline_cache,
                                               std::string& err) {
    if (!vulkan.ray_tracing_available()) {
        err = "vt_enrich: ray tracing unavailable: " +
              vulkan.ray_tracing_unavailable_reason();
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->vulkan = &vulkan;
    impl->device = vulkan.device();
    impl->pipeline_cache = pipeline_cache;
    if (!impl->init(err)) return nullptr;
    return std::unique_ptr<VtEnricher>(new VtEnricher(std::move(impl)));
}

// Both are queried by the residency layer per frame (the coarse-page skip
// reads max_footprint_meters()), so both read the live settings rather than an
// init-time copy.
uint32_t VtEnricher::sample_count() const {
    return live_enrich_settings().samples;
}

float VtEnricher::max_footprint_meters() const {
    return kEnrichFadeSpan * live_enrich_settings().cap_meters;
}

void VtEnricher::invalidate_part(uint64_t variant_hash) {
    // Both sweeps retire through their graveyards rather than destroying in
    // place. The old "device is idle per this method's contract" reasoning did
    // not hold: the caller's horizon is measured from when the PART was
    // released, but a variant can be rebuilt AFTER that -- a queued page
    // request serviced in the intervening frames -- leaving an entry one frame
    // old whose acceleration-structure build is still writing its scratch.
    // Device fault reports caught exactly that: an invalid WRITE to a buffer
    // that had lived 27 ms and been freed 12 ms earlier, a lifetime no
    // graveyard-routed entry can have (kRetireFrames guarantees more).
    for (auto it = impl_->variants.begin(); it != impl_->variants.end();) {
        if (it->first.first == variant_hash) {
            impl_->graveyard.push_back(
                Impl::Retired{std::move(it->second), impl_->last_frame_index});
            it = impl_->variants.erase(it);
        } else {
            ++it;
        }
    }
    stats_.as_cached = static_cast<uint32_t>(impl_->variants.size());
}

void VtEnricher::enrich(VkCommandBuffer cmd, const VtEnrichRequest* batch,
                        size_t count) {
    Impl& im = *impl_;
    if (!im.init_recorded) im.record_init(cmd);
    if (!batch || count == 0) return;

    const uint64_t frame_index = batch[0].frame_index;
    im.last_frame_index = std::max(im.last_frame_index, frame_index);
    im.retire(frame_index);

    // One read for the whole batch: every request in it must be enriched with
    // the same parameters, and a mid-batch change would make the recorded push
    // constants disagree with the candidate gather below.
    const matter::VtEnrichSettings settings = live_enrich_settings();

    Impl::Ring& ring = im.rings[im.ring_cursor];
    im.ring_cursor = (im.ring_cursor + 1) % kMaxBatchesInFlight;

    struct Rec {
        const Impl::VariantEntry* entry;
        VkImage orm_image;
        uint32_t pool_layers;
        uint32_t req_index;
        uint32_t dst_layer;
        int32_t dst_x, dst_y;
    };
    std::vector<Rec> recs;
    recs.reserve(std::min<size_t>(count, kMaxRequestsPerBatch));
    std::vector<Impl::VariantEntry*> pending_builds;

    auto* gpu_reqs = static_cast<GpuEnrichRequest*>(ring.requests.mapped);
    auto* gpu_cands = static_cast<uint32_t*>(ring.cands.mapped);
    uint32_t cand_cursor = 0;
    VkImage orm_image = VK_NULL_HANDLE;
    uint32_t orm_layers = 0;

    for (size_t i = 0; i < count; ++i) {
        const VtEnrichRequest& req = batch[i];
        const VtPoolBinding* pool = req.pool;
        if (!req.atlas || !req.part_context || !pool ||
            !pool->image[kVtChannelOrm] ||
            !pool->sampled_view[kVtChannelOrm] ||
            recs.size() >= kMaxRequestsPerBatch) {
            ++stats_.requests_skipped;
            continue;
        }
        uint32_t layer = 0, sx = 0, sy = 0;
        vt_slot_origin(req.physical_slot, layer, sx, sy);
        if (pool->layer_count != 0 && layer >= pool->layer_count) {
            ++stats_.requests_skipped;
            continue;
        }
        const auto* ctx = static_cast<const VtPartContext*>(req.part_context);
        Impl::VariantEntry* entry = im.get_or_build_variant(
            req.variant_hash, req.rung, req.atlas, ctx, frame_index, stats_);
        if (!entry) {
            ++stats_.requests_skipped;
            continue;
        }
        // Two pages of the SAME variant in one batch must not schedule the
        // build twice: a second build would write the BLAS while the first
        // TLAS build is reading it, and would be pure waste even if the
        // barriers covered it.
        if (!entry->built &&
            std::find(pending_builds.begin(), pending_builds.end(), entry) ==
                pending_builds.end())
            pending_builds.push_back(entry);

        const uint32_t cand_offset = cand_cursor;
        im.scratch_cands.clear();
        vt_page_candidate_charts(*req.atlas, req.page_x, req.page_y, req.mip,
                                 im.scratch_cands);
        if (im.scratch_cands.empty() ||
            cand_cursor + im.scratch_cands.size() > kMaxCandEntriesPerBatch) {
            ++stats_.requests_skipped;
            continue;
        }
        std::memcpy(gpu_cands + cand_cursor, im.scratch_cands.data(),
                    im.scratch_cands.size() * sizeof(uint32_t));
        cand_cursor += static_cast<uint32_t>(im.scratch_cands.size());

        const uint32_t rec_index = static_cast<uint32_t>(recs.size());
        GpuEnrichRequest& g = gpu_reqs[rec_index];
        g.a[0] = req.page_x;
        g.a[1] = req.page_y;
        g.a[2] = req.mip;
        g.a[3] = rec_index;
        g.b[0] = cand_offset;
        g.b[1] = static_cast<uint32_t>(im.scratch_cands.size());
        g.b[2] = settings.samples;
        g.b[3] = im.seed;
        g.c[0] = sx;
        g.c[1] = sy;
        g.c[2] = layer;
        g.c[3] = 0;
        g.d[0] = settings.strength;
        g.d[1] = settings.cap_texels;
        g.d[2] = settings.cap_meters;
        g.d[3] = settings.min_ao;
        orm_image = pool->image[kVtChannelOrm];
        orm_layers = pool->layer_count ? pool->layer_count : 1u;
        im.bind_pool_orm(pool->sampled_view[kVtChannelOrm]);
        recs.push_back(Rec{entry, orm_image, orm_layers, rec_index, layer,
                           static_cast<int32_t>(sx), static_cast<int32_t>(sy)});
    }
    if (recs.empty()) return;

    // 1. Acceleration structures whose builds have not been recorded yet.
    for (Impl::VariantEntry* entry : pending_builds)
        im.record_as_build(cmd, *entry);

    // 2. This batch's compute writes must wait for the previous batch in this
    //    ring slot (its encode reads / block-buffer copies).
    cmd_memory_barrier(cmd,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                           VK_ACCESS_2_TRANSFER_READ_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // 3. Trace + apply: reads the pool's ORM page (SHADER_READ_ONLY on entry,
    //    per the vt_enrich.h contract), writes the intermediate.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.enrich_pipe);
    for (const Rec& rec : recs) {
        VkDescriptorSet sets[2] = {rec.entry->set, ring.batch_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                im.enrich_pl, 0, 2, sets, 0, nullptr);
        vkCmdPushConstants(cmd, im.enrich_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4,
                           &rec.req_index);
        vkCmdDispatch(cmd, (kPageStore + 7) / 8, (kPageStore + 7) / 8, 1);
    }

    cmd_memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // 4. Re-encode. vt_bc_encode.comp compresses all three channels from the
    //    bound sources; only the ORM block buffer is copied back.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.encode_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.encode_pl, 0,
                            1, &ring.encode_set, 0, nullptr);
    for (const Rec& rec : recs) {
        const uint32_t push[2] = {rec.req_index, rec.req_index};
        vkCmdPushConstants(cmd, im.encode_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8,
                           push);
        const uint32_t groups = (kBlocksPerAxis + 7) / 8;   // 34 blocks -> 5
        vkCmdDispatch(cmd, groups, groups, 1);
    }

    cmd_memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);

    // 5. Write the refined ORM blocks back over the same page slots. The pool
    //    ORM image flips to TRANSFER_DST here and is restored to
    //    SHADER_READ_ONLY before returning (vt_enrich.h contract), so the
    //    residency layer's layout tracking stays true.
    cmd_image_barrier(cmd, orm_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, orm_layers);
    for (const Rec& rec : recs) {
        VkBufferImageCopy region{};
        region.bufferOffset =
            VkDeviceSize(rec.req_index) * kBlocksPerPage * 16;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, rec.dst_layer,
                                   1};
        region.imageOffset = {rec.dst_x, rec.dst_y, 0};
        region.imageExtent = {kPageStore, kPageStore, 1};
        vkCmdCopyBufferToImage(cmd, ring.blocks[2].buffer, rec.orm_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        ++stats_.pages_enriched;
    }
    cmd_image_barrier(cmd, orm_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, orm_layers);

    stats_.as_cached = static_cast<uint32_t>(im.variants.size());
    uint64_t bytes = 0;
    for (const auto& kv : im.variants) bytes += kv.second.bytes;
    stats_.as_bytes = bytes;
}

}  // namespace vt
