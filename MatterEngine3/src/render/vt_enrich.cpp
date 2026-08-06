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

// std430 mirror of vt_dirocc.comp's GpuDirOccRequest. Same 64-byte shape as
// the AO request on purpose (the ring buffers are sized for either), but the
// field meanings differ: d.x is reach metres, d.y a bias scale.
struct GpuDirOccRequest {
    uint32_t a[4];   // page_x, page_y, mip, out_layer
    uint32_t b[4];   // cand_offset, cand_count, sample_count, seed
    uint32_t c[4];   // slot_origin_x, slot_origin_y, pool_layer, unused
    float    d[4];   // reach_meters, bias_scale, unused, unused
};
static_assert(sizeof(GpuDirOccRequest) == sizeof(GpuEnrichRequest),
              "dir-occ requests ride the same ring buffers as AO requests");

// Ray-origin lift as a fraction of the reach (vt_dirocc.comp: bias =
// d.y * reach). The TLAS holds CASTERS only, never the receiver, so this is
// not a self-intersection guard — it only keeps the origin out of caster
// geometry that interpenetrates the receiver surface (a trunk sunk into the
// terrain). 0.001 x 64 m = 6.4 cm, far below the >= 2 m texels this tier
// bakes, so it cannot open a visible gap under anything.
constexpr float kDirOccBiasScale = 0.001f;

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
    s.dirocc_min_footprint = clamp_f32(s.dirocc_min_footprint, 0.01f, 1000.0f);
    s.dirocc_reach = clamp_f32(s.dirocc_reach, 1.0f, 1024.0f);
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

    // ---- M6.5 directional tier -------------------------------------------
    // Everything below is created LAZILY by dirocc_init() on the first
    // enrich_dir_occ() call, so a session that never raises
    // MATTER_VT_DIROCC_PER_FRAME allocates none of it (the rings alone are
    // ~2 MiB each). A failed init latches dirocc_failed and every later
    // request skips silently — the channel keeps its cleared "no occlusion".
    //
    // The tier gets its OWN rings rather than sharing the AO ones: both tiers
    // can record one batch per frame, and two batches per frame through one
    // 4-deep ring would rewrite a ring slot's host-visible request buffer
    // after only 2 frames — inside the in-flight window the AO tier's
    // once-per-frame cadence was sized against.
    VkPipeline dirocc_pipe = VK_NULL_HANDLE;
    VkDescriptorPool dirocc_pool = VK_NULL_HANDLE;
    Ring dirocc_rings[kMaxBatchesInFlight];
    uint32_t dirocc_ring_cursor = 0;
    bool dirocc_init_attempted = false;
    bool dirocc_failed = false;
    bool dirocc_init_recorded = false;
    VkImageView bound_pool_dirocc = VK_NULL_HANDLE;

    // One caster BLAS over BORROWED scene geometry. THE LIFETIME RULE
    // (vt_types.h): a device address is a number and does not keep its buffer
    // alive; the renderer's release_part can drop its own reference between
    // harvest and record, and the resulting use-after-free only appears under
    // streaming — the class that already cost this project a DEVICE_LOST
    // investigation. geometry_lifetime / index_lifetime therefore hold the
    // SAME shared_ptrs the renderer holds, and they ride every cache entry
    // and graveyard entry that stores the addresses, until that entry
    // retires.
    struct CasterBlas {
        matter::VkBufferResource scratch;
        matter::VkAccelerationStructureResource blas;
        std::shared_ptr<void> geometry_lifetime;
        std::shared_ptr<void> index_lifetime;
        uint64_t vertex_address = 0;
        uint64_t index_address = 0;
        uint32_t vertex_stride = 0;
        uint32_t max_vertex = 0;
        uint32_t primitive_count = 0;
        uint64_t bytes = 0;
        uint64_t last_used = 0;
        bool built = false;   // false => the build still has to be recorded
    };
    // Dedup map. Keyed by a fold of the borrowed ADDRESSES + counts rather
    // than part_hash_low alone: a part is many clusters and each cluster
    // contributes its own index range, so the hash word by itself would
    // collapse distinct geometry. Addresses are not deterministic across
    // runs, but this key never reaches the baked bytes — a BLAS's content is
    // the geometry either way; the key only decides whether two receivers
    // SHARE the structure, which is exactly the cross-sector dedup wanted.
    std::map<uint64_t, std::shared_ptr<CasterBlas>> caster_blas;

    struct DirOccEntry {
        matter::VkBufferResource charts;         // the RECEIVER's streams
        matter::VkBufferResource tris;
        matter::VkBufferResource as_instances;   // caster TLAS instances
        matter::VkBufferResource tlas_scratch;
        matter::VkAccelerationStructureResource tlas;
        // Shared caster BLASes this TLAS references. Holding the shared_ptrs
        // here is what carries every geometry_lifetime through the graveyard:
        // the entry retires => the refs drop => an unreferenced CasterBlas
        // becomes prunable (see retire_dirocc).
        std::vector<std::shared_ptr<CasterBlas>> blas_refs;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t instance_count = 0;
        uint64_t caster_set_hash = 0;
        uint64_t bytes = 0;
        uint64_t last_used = 0;
        bool tlas_built = false;
    };
    // Keyed (variant_hash, rung); the caster_set_hash lives INSIDE the entry
    // and a mismatch retires + rebuilds, so "cache keyed by (variant, rung,
    // caster_set_hash)" holds with at most one live set per rung — which is
    // the truth (a receiver has exactly one current caster set).
    std::map<std::pair<uint64_t, uint32_t>, DirOccEntry> dirocc_variants;
    struct DirOccRetired {
        DirOccEntry entry;
        uint64_t frame = 0;
    };
    std::vector<DirOccRetired> dirocc_graveyard;

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
        // M6.5: the directional tier's lazy resources. The maps drop their
        // shared_ptrs here, which releases the borrowed caster buffers'
        // lifetimes — legal only because ~VtEnricher runs under the caller's
        // device-idle teardown, the same contract the AO graveyard rides.
        for (auto& kv : dirocc_variants) free_dirocc_set(kv.second);
        dirocc_variants.clear();
        for (DirOccRetired& r : dirocc_graveyard) free_dirocc_set(r.entry);
        dirocc_graveyard.clear();
        caster_blas.clear();
        for (Ring& r : dirocc_rings) destroy_raw_image(device, r.inter);
        if (dirocc_pipe) vkDestroyPipeline(device, dirocc_pipe, nullptr);
        if (dirocc_pool) vkDestroyDescriptorPool(device, dirocc_pool, nullptr);
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

    void free_dirocc_set(DirOccEntry& e) {
        if (e.set && dirocc_pool)
            vkFreeDescriptorSets(device, dirocc_pool, 1, &e.set);
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

    // ---- M6.5 directional tier -------------------------------------------
    bool dirocc_init(std::string& err);
    void dirocc_record_init(VkCommandBuffer cmd);
    void bind_pool_dirocc(VkImageView view);
    std::shared_ptr<CasterBlas> get_or_build_caster_blas(
        const VtCasterInstance& caster, uint64_t frame_index, Stats& stats);
    DirOccEntry* get_or_build_dirocc_entry(
        uint64_t variant_hash, uint32_t rung,
        const chart_atlas::ChartAtlasRung* atlas, const VtPartContext* ctx,
        uint64_t frame_index, Stats& stats);
    void record_dirocc_as_builds(VkCommandBuffer cmd,
                                 const std::vector<DirOccEntry*>& entries);
    void retire_dirocc(uint64_t frame_index);
    void evict_dirocc_lru(uint64_t frame_index, Stats& stats);
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

// Lazy: called from the first enrich_dir_occ(). Reuses the AO pass's layouts
// wholesale — vt_dirocc.comp's two sets are BINDING-IDENTICAL to
// vt_enrich_ao.comp's (set 0: charts/tris/AS, set 1: requests/cands/sampler/
// storage image, push constant u32), which is not a coincidence: the shader
// was written against them so this file would add one pipeline, not a layout
// family. Only the descriptor POOL and the rings are new, because the AO pool
// was sized before this tier existed and both tiers can be in flight at once.
bool VtEnricher::Impl::dirocc_init(std::string& err) {
    if (!create_pipeline("vt_dirocc.comp.spv", enrich_pl, dirocc_pipe, err))
        return false;

    {
        const uint32_t entry_sets = as_cache_cap + 32u;   // live + graveyard
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             entry_sets * 2 + kMaxBatchesInFlight * 5},
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, entry_sets},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxBatchesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxBatchesInFlight * 4},
        };
        VkDescriptorPoolCreateInfo info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = entry_sets + kMaxBatchesInFlight * 2;
        info.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
        info.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &info, nullptr, &dirocc_pool) !=
            VK_SUCCESS) {
            err = "vt_enrich: vkCreateDescriptorPool (dirocc) failed";
            return false;
        }
    }

    for (Ring& r : dirocc_rings) {
        if (!matter::create_buffer(
                *vulkan, sizeof(GpuDirOccRequest) * kMaxRequestsPerBatch,
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
        alloc.descriptorPool = dirocc_pool;
        alloc.descriptorSetCount = 2;
        alloc.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(device, &alloc, sets) != VK_SUCCESS) {
            err = "vt_enrich: vkAllocateDescriptorSets (dirocc ring) failed";
            return false;
        }
        r.batch_set = sets[0];
        r.encode_set = sets[1];
        write_ring_descriptors(r);
    }
    return true;
}

void VtEnricher::Impl::dirocc_record_init(VkCommandBuffer cmd) {
    for (Ring& r : dirocc_rings) {
        cmd_image_barrier(cmd, r.inter.image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                          kMaxRequestsPerBatch);
    }
    dirocc_init_recorded = true;
}

// The shader declares poolDirOcc but never reads it (the pass OVERWRITES its
// channel — see vt_dirocc.comp's header). Bound anyway: an unwritten combined-
// image-sampler descriptor in a set the pipeline uses is a validation error on
// drivers that consider a declared binding statically used.
void VtEnricher::Impl::bind_pool_dirocc(VkImageView view) {
    if (view == bound_pool_dirocc) return;
    VkDescriptorImageInfo info{point_sampler, view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[kMaxBatchesInFlight];
    for (uint32_t i = 0; i < kMaxBatchesInFlight; ++i) {
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = dirocc_rings[i].batch_set;
        writes[i].dstBinding = 2;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &info;
    }
    vkUpdateDescriptorSets(device, kMaxBatchesInFlight, writes, 0, nullptr);
    bound_pool_dirocc = view;
}

std::shared_ptr<VtEnricher::Impl::CasterBlas>
VtEnricher::Impl::get_or_build_caster_blas(const VtCasterInstance& caster,
                                           uint64_t frame_index, Stats& stats) {
    // Address-derived dedup key (see the map's comment for why not
    // part_hash_low alone). FNV-1a over the fields that define the geometry
    // the BLAS is built from.
    uint64_t key = 1469598103934665603ull;
    const auto fold = [&key](uint64_t v) {
        key ^= v;
        key *= 1099511628211ull;
    };
    fold(caster.vertex_address);
    fold(caster.index_address);
    fold(caster.primitive_count);
    fold(caster.vertex_stride);
    fold(caster.max_vertex);

    const auto found = caster_blas.find(key);
    if (found != caster_blas.end()) {
        found->second->last_used = frame_index;
        return found->second;
    }

    auto entry = std::make_shared<CasterBlas>();
    entry->geometry_lifetime = caster.geometry_lifetime;
    entry->index_lifetime = caster.index_lifetime;
    entry->vertex_address = caster.vertex_address;
    entry->index_address = caster.index_address;
    entry->vertex_stride = caster.vertex_stride;
    entry->max_vertex = caster.max_vertex;
    entry->primitive_count = caster.primitive_count;
    entry->last_used = frame_index;

    // Size the BLAS against the BORROWED addresses directly — geometry is
    // never copied. This mirrors the renderer's own BLAS setup over the same
    // rt_geometry/rt_index buffers (vk_scene_renderer.cpp, build_ray_*):
    // vertex base + stride over VkRasterVertex, index address pre-offset to
    // the cluster rung's range.
    VkAccelerationStructureGeometryKHR geom{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geom.geometry.triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress = entry->vertex_address;
    geom.geometry.triangles.vertexStride = entry->vertex_stride;
    geom.geometry.triangles.maxVertex = entry->max_vertex;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geom.geometry.triangles.indexData.deviceAddress = entry->index_address;

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
              &entry->primitive_count, &sizes);
    std::string err;
    if (!matter::create_acceleration_structure(
            *vulkan, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            sizes.accelerationStructureSize, entry->blas, err))
        return nullptr;
    if (!matter::create_buffer(*vulkan,
                               sizes.buildScratchSize + scratch_align - 1,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                               entry->scratch, err))
        return nullptr;
    entry->bytes = sizes.accelerationStructureSize + sizes.buildScratchSize;

    ++stats.dir_occ_blas_builds;
    caster_blas.emplace(key, entry);
    stats.dir_occ_blas_cached = static_cast<uint32_t>(caster_blas.size());
    return entry;
}

VtEnricher::Impl::DirOccEntry* VtEnricher::Impl::get_or_build_dirocc_entry(
    uint64_t variant_hash, uint32_t rung,
    const chart_atlas::ChartAtlasRung* atlas, const VtPartContext* ctx,
    uint64_t frame_index, Stats& stats) {
    const auto key = std::make_pair(variant_hash, rung);
    auto it = dirocc_variants.find(key);
    if (it != dirocc_variants.end()) {
        if (it->second.caster_set_hash == ctx->caster_set_hash) {
            it->second.last_used = frame_index;
            return &it->second;
        }
        // The caster SET changed (something streamed in/out or moved). The
        // old TLAS may still be referenced by an unretired batch, so it goes
        // through the graveyard — never destroyed in place — carrying its
        // blas_refs, i.e. the borrowed buffers' lifetimes, with it.
        dirocc_graveyard.push_back(
            DirOccRetired{std::move(it->second), frame_index});
        dirocc_variants.erase(it);
    }

    if (!vt_build_chart_gpu_streams(*atlas, *ctx, scratch_charts, scratch_tris))
        return nullptr;

    evict_dirocc_lru(frame_index, stats);

    DirOccEntry entry;
    entry.caster_set_hash = ctx->caster_set_hash;
    entry.last_used = frame_index;
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

    // Gather the caster instances. Per-caster failures skip that caster only
    // (fail closed — its shadow is simply absent); a caster carrying an
    // address WITHOUT its lifetime shared_ptr is refused outright, because
    // referencing it is exactly the use-after-free this design exists to
    // prevent.
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.reserve(ctx->caster_count);
    for (uint32_t i = 0; i < ctx->caster_count; ++i) {
        const VtCasterInstance& c = ctx->casters[i];
        if (c.vertex_address == 0 || c.index_address == 0 ||
            c.primitive_count == 0 || c.vertex_stride == 0)
            continue;
        if (!c.geometry_lifetime || !c.index_lifetime) continue;
        std::shared_ptr<CasterBlas> blas =
            get_or_build_caster_blas(c, frame_index, stats);
        if (!blas) continue;
        VkAccelerationStructureInstanceKHR instance{};
        // to_receiver is row-major 3x4 (vt_types.h) and VkTransformMatrixKHR
        // is row-major 3x4: a straight copy, same as the renderer's TLAS path.
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t col = 0; col < 4; ++col)
                instance.transform.matrix[row][col] =
                    c.to_receiver[row * 4 + col];
        instance.instanceCustomIndex = 0;
        instance.mask = 0xFF;
        instance.instanceShaderBindingTableRecordOffset = 0;
        // Cull-disable, like every occlusion query in this file: the ray only
        // asks "is there geometry", and winding is a raster concern.
        instance.flags =
            VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = blas->blas.address;
        instances.push_back(instance);
        entry.blas_refs.push_back(std::move(blas));
    }
    if (instances.empty() && ctx->caster_count != 0) {
        // Casters were supplied and EVERY one failed — treat the request as
        // unservable rather than baking a false all-clear over a page whose
        // content key says "N casters".
        return nullptr;
    }
    // instances.empty() with caster_count == 0 is the deliberate
    // "bake-to-clear" case (the set went empty): an empty TLAS traces
    // nothing, every ray reaches the sky, and the pass overwrites the channel
    // with its open value — which is exactly the shadow removal wanted.
    entry.instance_count = static_cast<uint32_t>(instances.size());

    const VkBufferUsageFlags as_input =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VkDeviceSize instance_bytes =
        sizeof(VkAccelerationStructureInstanceKHR) *
        std::max<size_t>(instances.size(), 1);
    if (!matter::create_buffer(*vulkan, instance_bytes, as_input,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               entry.as_instances, err) ||
        !matter::map_buffer(entry.as_instances, err))
        return nullptr;
    if (!instances.empty())
        std::memcpy(entry.as_instances.mapped, instances.data(),
                    sizeof(VkAccelerationStructureInstanceKHR) *
                        instances.size());

    VkAccelerationStructureGeometryKHR tlas_geom{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlas_geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geom.geometry.instances = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    tlas_geom.geometry.instances.data.deviceAddress = entry.as_instances.address;
    VkAccelerationStructureBuildGeometryInfoKHR tlas_build{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlas_build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlas_build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlas_build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlas_build.geometryCount = 1;
    tlas_build.pGeometries = &tlas_geom;
    VkAccelerationStructureBuildSizesInfoKHR tlas_sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    get_sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
              &tlas_build, &entry.instance_count, &tlas_sizes);
    if (!matter::create_acceleration_structure(
            *vulkan, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            tlas_sizes.accelerationStructureSize, entry.tlas, err))
        return nullptr;
    if (!matter::create_buffer(*vulkan,
                               tlas_sizes.buildScratchSize + scratch_align - 1,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                               entry.tlas_scratch, err))
        return nullptr;
    entry.bytes = charts_bytes + tris_bytes + instance_bytes +
                  tlas_sizes.accelerationStructureSize +
                  tlas_sizes.buildScratchSize;

    VkDescriptorSetAllocateInfo alloc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = dirocc_pool;
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

    ++stats.dir_occ_tlas_builds;
    auto inserted = dirocc_variants.emplace(key, std::move(entry));
    stats.dir_occ_entries_cached =
        static_cast<uint32_t>(dirocc_variants.size());
    return &inserted.first->second;
}

// Record every not-yet-built structure this batch references: all caster
// BLASes first, one barrier, then the TLASes. Split from the AO path's
// record_as_build because a dir-occ TLAS references BLASes SHARED across
// entries, so the build set must be gathered over the whole batch and deduped
// before anything is recorded.
void VtEnricher::Impl::record_dirocc_as_builds(
    VkCommandBuffer cmd, const std::vector<DirOccEntry*>& entries) {
    std::vector<CasterBlas*> blas_builds;
    for (DirOccEntry* entry : entries) {
        if (entry->tlas_built) continue;
        for (const std::shared_ptr<CasterBlas>& blas : entry->blas_refs) {
            if (blas->built) continue;
            if (std::find(blas_builds.begin(), blas_builds.end(), blas.get()) ==
                blas_builds.end())
                blas_builds.push_back(blas.get());
        }
    }
    if (!blas_builds.empty()) {
        for (CasterBlas* blas : blas_builds) {
            VkAccelerationStructureGeometryKHR geom{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            geom.geometry.triangles.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            geom.geometry.triangles.vertexData.deviceAddress =
                blas->vertex_address;
            geom.geometry.triangles.vertexStride = blas->vertex_stride;
            geom.geometry.triangles.maxVertex = blas->max_vertex;
            geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
            geom.geometry.triangles.indexData.deviceAddress =
                blas->index_address;

            VkAccelerationStructureBuildGeometryInfoKHR build{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            build.flags =
                VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            build.geometryCount = 1;
            build.pGeometries = &geom;
            build.dstAccelerationStructure = blas->blas.handle;
            build.scratchData.deviceAddress =
                align_up(blas->scratch.address, scratch_align);
            VkAccelerationStructureBuildRangeInfoKHR range{};
            range.primitiveCount = blas->primitive_count;
            const VkAccelerationStructureBuildRangeInfoKHR* range_ptr = &range;
            cmd_build(cmd, 1, &build, &range_ptr);
            blas->built = true;
        }
        cmd_memory_barrier(
            cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    }
    bool any_tlas = false;
    for (DirOccEntry* entry : entries) {
        if (entry->tlas_built) continue;
        VkAccelerationStructureGeometryKHR tlas_geom{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        tlas_geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlas_geom.geometry.instances = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        tlas_geom.geometry.instances.data.deviceAddress =
            entry->as_instances.address;
        VkAccelerationStructureBuildGeometryInfoKHR tlas_build{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        tlas_build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlas_build.flags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        tlas_build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tlas_build.geometryCount = 1;
        tlas_build.pGeometries = &tlas_geom;
        tlas_build.dstAccelerationStructure = entry->tlas.handle;
        tlas_build.scratchData.deviceAddress =
            align_up(entry->tlas_scratch.address, scratch_align);
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = entry->instance_count;
        const VkAccelerationStructureBuildRangeInfoKHR* range_ptr = &range;
        cmd_build(cmd, 1, &tlas_build, &range_ptr);
        entry->tlas_built = true;
        any_tlas = true;
    }
    if (any_tlas) {
        cmd_memory_barrier(
            cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    }
}

void VtEnricher::Impl::retire_dirocc(uint64_t frame_index) {
    for (size_t i = dirocc_graveyard.size(); i-- > 0;) {
        if (frame_index < dirocc_graveyard[i].frame + kRetireFrames) continue;
        free_dirocc_set(dirocc_graveyard[i].entry);
        // The entry's blas_refs (and with them the borrowed geometry
        // lifetimes) drop here — at least kRetireFrames after the last batch
        // that could reference them was recorded.
        dirocc_graveyard.erase(dirocc_graveyard.begin() +
                               static_cast<long>(i));
    }
    // Prune caster BLASes nothing references any more. use_count()==1 means
    // no live entry and no graveyard entry holds it, so every batch that
    // traced it has already survived a full graveyard retirement; the age
    // check guards the one remaining window (a BLAS built and recorded this
    // frame whose TLAS entry then failed to finish).
    for (auto it = caster_blas.begin(); it != caster_blas.end();) {
        if (it->second.use_count() == 1 &&
            frame_index >= it->second->last_used + kRetireFrames) {
            it = caster_blas.erase(it);
        } else {
            ++it;
        }
    }
}

void VtEnricher::Impl::evict_dirocc_lru(uint64_t frame_index, Stats& stats) {
    while (dirocc_variants.size() >= as_cache_cap) {
        auto victim = dirocc_variants.end();
        for (auto it = dirocc_variants.begin(); it != dirocc_variants.end();
             ++it) {
            // NEVER evict an entry this frame's batch already references —
            // the recording loop holds raw pointers into these map nodes.
            if (it->second.last_used >= frame_index) continue;
            if (victim == dirocc_variants.end() ||
                it->second.last_used < victim->second.last_used)
                victim = it;
        }
        if (victim == dirocc_variants.end()) break;
        dirocc_graveyard.push_back(
            DirOccRetired{std::move(victim->second), frame_index});
        dirocc_variants.erase(victim);
        ++stats.as_evictions;
    }
    stats.dir_occ_entries_cached =
        static_cast<uint32_t>(dirocc_variants.size());
}

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
    for (auto it = impl_->variants.begin(); it != impl_->variants.end();) {
        if (it->first.first == variant_hash) {
            impl_->free_variant_set(it->second);
            it = impl_->variants.erase(it);
        } else {
            ++it;
        }
    }
    stats_.as_cached = static_cast<uint32_t>(impl_->variants.size());
    // M6.5: the directional entries carry the same variant's chart/tri
    // streams, so they die with it. Device-idle per this method's contract,
    // which is what makes the in-place destruction (no graveyard) legal here
    // and only here.
    for (auto it = impl_->dirocc_variants.begin();
         it != impl_->dirocc_variants.end();) {
        if (it->first.first == variant_hash) {
            impl_->free_dirocc_set(it->second);
            it = impl_->dirocc_variants.erase(it);
        } else {
            ++it;
        }
    }
    stats_.dir_occ_entries_cached =
        static_cast<uint32_t>(impl_->dirocc_variants.size());
}

void VtEnricher::enrich(VkCommandBuffer cmd, const VtEnrichRequest* batch,
                        size_t count) {
    Impl& im = *impl_;
    if (!im.init_recorded) im.record_init(cmd);
    if (!batch || count == 0) return;

    const uint64_t frame_index = batch[0].frame_index;
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

// MATTER_VT_DIROCC_MIN_FOOTPRINT, live like the AO thresholds: the residency
// layer queries it per queued page.
float VtEnricher::dir_occ_min_footprint_meters() const {
    return live_enrich_settings().dirocc_min_footprint;
}

void VtEnricher::enrich_dir_occ(VkCommandBuffer cmd,
                                const VtEnrichRequest* batch, size_t count) {
    Impl& im = *impl_;
    if (!batch || count == 0) return;

    // Lazy creation, once: the tier is off by default and its rings alone are
    // megabytes, so nothing is allocated until the residency layer actually
    // drains a request here. A failure latches the tier off for the session —
    // every later request skips silently and pages keep the channel's cleared
    // "no occlusion" value, which is the fail-closed contract.
    if (!im.dirocc_init_attempted) {
        im.dirocc_init_attempted = true;
        std::string err;
        if (!im.dirocc_init(err)) {
            im.dirocc_failed = true;
            std::fprintf(stderr,
                         "[vt] directional occlusion tier unavailable "
                         "(pages keep no-occlusion): %s\n",
                         err.c_str());
            std::fflush(stderr);
        }
    }
    if (im.dirocc_failed) {
        stats_.dir_occ_skipped += count;
        return;
    }
    if (!im.dirocc_init_recorded) im.dirocc_record_init(cmd);

    const uint64_t frame_index = batch[0].frame_index;
    im.retire_dirocc(frame_index);
    const matter::VtEnrichSettings settings = live_enrich_settings();

    Impl::Ring& ring = im.dirocc_rings[im.dirocc_ring_cursor];
    im.dirocc_ring_cursor = (im.dirocc_ring_cursor + 1) % kMaxBatchesInFlight;

    struct Rec {
        const Impl::DirOccEntry* entry;
        uint32_t req_index;
        uint32_t dst_layer;
        int32_t dst_x, dst_y;
    };
    std::vector<Rec> recs;
    recs.reserve(std::min<size_t>(count, kMaxRequestsPerBatch));
    std::vector<Impl::DirOccEntry*> batch_entries;

    auto* gpu_reqs = static_cast<GpuDirOccRequest*>(ring.requests.mapped);
    auto* gpu_cands = static_cast<uint32_t*>(ring.cands.mapped);
    uint32_t cand_cursor = 0;
    VkImage dst_image = VK_NULL_HANDLE;
    uint32_t dst_layers = 0;

    for (size_t i = 0; i < count; ++i) {
        const VtEnrichRequest& req = batch[i];
        const VtPoolBinding* pool = req.pool;
        if (!req.atlas || !req.part_context || !pool ||
            !pool->image[kVtChannelDirOcc] ||
            !pool->sampled_view[kVtChannelDirOcc] ||
            recs.size() >= kMaxRequestsPerBatch) {
            ++stats_.dir_occ_skipped;
            continue;
        }
        uint32_t layer = 0, sx = 0, sy = 0;
        vt_slot_origin(req.physical_slot, layer, sx, sy);
        if (pool->layer_count != 0 && layer >= pool->layer_count) {
            ++stats_.dir_occ_skipped;
            continue;
        }
        const auto* ctx = static_cast<const VtPartContext*>(req.part_context);
        // The residency layer already gates on world-anchoring and the caster
        // cap is a batch-shape limit (see kMaxDirOccCasters: skip whole, never
        // truncate — truncation would bake order-dependent bytes under an
        // order-independent content key).
        if (!ctx->surface_world_anchored ||
            ctx->caster_count > kMaxDirOccCasters ||
            (ctx->caster_count != 0 && ctx->casters == nullptr)) {
            ++stats_.dir_occ_skipped;
            continue;
        }
        Impl::DirOccEntry* entry = im.get_or_build_dirocc_entry(
            req.variant_hash, req.rung, req.atlas, ctx, frame_index, stats_);
        if (!entry) {
            ++stats_.dir_occ_skipped;
            continue;
        }
        if (std::find(batch_entries.begin(), batch_entries.end(), entry) ==
            batch_entries.end())
            batch_entries.push_back(entry);

        const uint32_t cand_offset = cand_cursor;
        im.scratch_cands.clear();
        vt_page_candidate_charts(*req.atlas, req.page_x, req.page_y, req.mip,
                                 im.scratch_cands);
        if (im.scratch_cands.empty() ||
            cand_cursor + im.scratch_cands.size() > kMaxCandEntriesPerBatch) {
            ++stats_.dir_occ_skipped;
            continue;
        }
        std::memcpy(gpu_cands + cand_cursor, im.scratch_cands.data(),
                    im.scratch_cands.size() * sizeof(uint32_t));
        cand_cursor += static_cast<uint32_t>(im.scratch_cands.size());

        const uint32_t rec_index = static_cast<uint32_t>(recs.size());
        GpuDirOccRequest& g = gpu_reqs[rec_index];
        g.a[0] = req.page_x;
        g.a[1] = req.page_y;
        g.a[2] = req.mip;
        g.a[3] = rec_index;
        g.b[0] = cand_offset;
        g.b[1] = static_cast<uint32_t>(im.scratch_cands.size());
        g.b[2] = settings.samples;
        g.b[3] = im.seed;   // fixed constant — the determinism property
        g.c[0] = sx;
        g.c[1] = sy;
        g.c[2] = layer;
        g.c[3] = 0;
        g.d[0] = settings.dirocc_reach;
        g.d[1] = kDirOccBiasScale;
        g.d[2] = 0.0f;
        g.d[3] = 0.0f;
        dst_image = pool->image[kVtChannelDirOcc];
        dst_layers = pool->layer_count ? pool->layer_count : 1u;
        im.bind_pool_dirocc(pool->sampled_view[kVtChannelDirOcc]);
        recs.push_back(Rec{entry, rec_index, layer, static_cast<int32_t>(sx),
                           static_cast<int32_t>(sy)});
    }
    if (recs.empty()) return;

    // 1. Acceleration structures this batch needs but whose builds have not
    //    been recorded yet — all missing caster BLASes, then the TLASes.
    im.record_dirocc_as_builds(cmd, batch_entries);

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

    // 3. Trace: writes bent normal + aperture into the intermediate. Unlike
    //    the AO pass this reads nothing back from the pool — the channel is
    //    overwritten, which is what makes re-baking idempotent.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.dirocc_pipe);
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

    // 4. BC7-encode. vt_bc_encode.comp compresses three channels from the
    //    bound sources (all three point at the same intermediate here, as in
    //    the AO path); only the BC7 "orm" block stream — which is the
    //    intermediate's RGBA, aperture in A — is copied out. BC7 mode 6
    //    carries alpha, which is load-bearing: the aperture IS the shadow.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.encode_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.encode_pl, 0,
                            1, &ring.encode_set, 0, nullptr);
    for (const Rec& rec : recs) {
        const uint32_t push[2] = {rec.req_index, rec.req_index};
        vkCmdPushConstants(cmd, im.encode_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8,
                           push);
        const uint32_t groups = (kBlocksPerAxis + 7) / 8;
        vkCmdDispatch(cmd, groups, groups, 1);
    }

    cmd_memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);

    // 5. Overwrite the DirOcc page slots. Same layout discipline as the AO
    //    write-back: TRANSFER_DST for the copies, restored to
    //    SHADER_READ_ONLY before returning so the residency layer's layout
    //    tracking stays true.
    cmd_image_barrier(cmd, dst_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, dst_layers);
    for (const Rec& rec : recs) {
        VkBufferImageCopy region{};
        region.bufferOffset =
            VkDeviceSize(rec.req_index) * kBlocksPerPage * 16;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, rec.dst_layer,
                                   1};
        region.imageOffset = {rec.dst_x, rec.dst_y, 0};
        region.imageExtent = {kPageStore, kPageStore, 1};
        vkCmdCopyBufferToImage(cmd, ring.blocks[2].buffer, dst_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        ++stats_.dir_occ_pages;
    }
    cmd_image_barrier(cmd, dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, dst_layers);
}

}  // namespace vt
