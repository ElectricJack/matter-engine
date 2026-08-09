// vt_compositor.cpp — WP-D tier-1 chart-page compositor + GPU BC encode.
// See vt_compositor.h for the module contract and shaders_vk/vt_composite.comp
// / vt_bc_encode.comp for the GPU passes this records.

#include "vt_compositor.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>

#include "profile.h"
#include "shaders_gen/embedded_spirv.h"
// GpuChart / GpuTri and the stream builder: shared with WP-H's enricher so the
// two page passes can never disagree about a texel's owning triangle.
#include "vt_chart_gpu.h"
// P2: the tape packer (GpuSurfOp encoding + field-lane scan) — shared with
// the producers, which compute the per-vertex lane VALUES with the same scan.
#include "vt_surface_tape.h"

namespace vt {

namespace {

// ---------------------------------------------------------------------------
// GPU struct mirrors — layouts must match vt_composite.comp / vt_bc_encode.comp
// (std430 for the SSBOs, std140 for the params UBO; every field is 16-byte
// packed so the C++ mirrors are exact). GpuChart/GpuTri live in vt_chart_gpu.h.
// ---------------------------------------------------------------------------
struct GpuFillRequest {
    uint32_t a[4];   // page_x, page_y, mip, out_layer
    uint32_t b[4];   // cand_offset, cand_count, weight_mode, debug materials
    float debug_params[4];
    // WP-F: tape material registry ids for weight columns 0-7 (u8 each, x =
    // cols 0-3, y = cols 4-7), z = declared column count, w = field-lane
    // count (P2, mode 3 only).
    uint32_t tape[4];
    // P2 (mode 3): the part's packed tape in the shared op arena — x = op
    // offset (in ops), y = op count (0 for non-mode-3 requests), z/w = weight
    // registers for columns 0-3 / 4-7 (u8 each).
    uint32_t tape2[4];
    // P2 (mode 3): local_to_world rows (row-major 4x3); identity when the
    // part is not world-anchored (never read there — the packer pre-resolved
    // its world ops to constants).
    float xform[12];
    // P3 (appearance lanes, mode 3): the tape registers the output directives
    // read — x = tint regs (r | g << 8 | b << 16), y = roughbias reg,
    // z = wetness reg, w = pad. Each byte kVtNoAppearanceReg when absent.
    uint32_t app[4];
};
static_assert(sizeof(GpuFillRequest) == 144, "GpuFillRequest layout");

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

// P2: the shared tape-op arena. Fixed-size slots (one per mesh entry with a
// packed tape) — a SurfaceProgram is capped at kMaxSurfaceOps dedup'd ops, so
// a slot is that many * 48 B; sizing covers every cached entry plus the
// worst-case one-shot burst (kMaxBatchesInFlight batches of
// kMaxRequestsPerFill distinct variants), so a fill is never demoted to mode 2
// for arena pressure alone.
//
// Derived, not a copy: this was a literal 96 next to a comment still claiming
// 64 from before the cap was raised, i.e. exactly the drift the alias removes.
constexpr uint32_t kTapeSlotOps =
    static_cast<uint32_t>(terrain_field::kMaxSurfaceOps);
constexpr uint32_t kTapeArenaSlots =
    kMaxMeshEntries +
    VtCompositor::kMaxBatchesInFlight * kMaxRequestsPerFill;   // 1536

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

    // P2: the shared tape-op arena (fixed 64-op slots; see kTapeArenaSlots).
    // Host-visible with device-local preference — the same residency the
    // chart/tri streams ride. Slots are allocated with the mesh entry that
    // owns them and freed on the same retirement paths.
    RawBuffer tape_arena;
    std::vector<uint32_t> tape_free_slots;
    bool tape_gpu_enabled = true;      // MATTER_VT_TAPE_GPU, read at init
    bool tape_overflow_warned = false; // warn-once for the lane-cap fallback

    int32_t tape_slot_acquire() {
        if (tape_free_slots.empty()) return -1;
        const uint32_t slot = tape_free_slots.back();
        tape_free_slots.pop_back();
        return static_cast<int32_t>(slot);
    }
    void tape_slot_release(int32_t& slot) {
        if (slot < 0) return;
        tape_free_slots.push_back(static_cast<uint32_t>(slot));
        slot = -1;
    }

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
        uint64_t last_used_batch = 0;
        // P2 (mode 3): the entry's packed tape. tape_slot >= 0 means the
        // entry was promoted to mode 3 — its triangle stream carries f16
        // field lanes instead of u8 weight columns, and its ops live at
        // tape_slot * kTapeSlotOps in the arena. -1 = mode-2 packing.
        int32_t tape_slot = -1;
        uint32_t tape_op_count = 0;
        uint32_t tape_lane_count = 0;
        uint8_t tape_weight_reg[8]{};
        // P3: the appearance directives' registers (kVtNoAppearanceReg when
        // the tape declares none). They travel with the entry exactly like the
        // weight registers — same tape, same pack, same lifetime.
        uint8_t tape_tint_reg[3] = {kVtNoAppearanceReg, kVtNoAppearanceReg,
                                    kVtNoAppearanceReg};
        uint8_t tape_rough_reg = kVtNoAppearanceReg;
        uint8_t tape_wet_reg = kVtNoAppearanceReg;
        uint8_t tape_metal_reg = kVtNoAppearanceReg;
    };
    std::map<std::pair<uint64_t, uint32_t>, MeshEntry> mesh_cache;
    // Monotonic fill()-batch counter, and per-ring lists of evicted or
    // one-shot entries whose GPU resources must outlive any batch that
    // referenced them. An entry retired during batch N is destroyed when N's
    // ring is reused (batch N + kMaxBatchesInFlight) — the same retirement
    // window the ring's own host-visible request buffers already rely on.
    // Entries used within the in-flight window are never evicted. deque:
    // one-shot entries hand out pointers to their elements mid-batch, so
    // growth must not relocate existing elements.
    uint64_t batch_counter = 0;
    std::deque<MeshEntry> mesh_retire[kMaxBatchesInFlight];

    void flush_mesh_retire(uint32_t ring_index) {
        for (MeshEntry& entry : mesh_retire[ring_index]) {
            if (entry.set)
                vkFreeDescriptorSets(device, descriptor_pool, 1, &entry.set);
            destroy_raw_buffer(device, entry.charts);
            destroy_raw_buffer(device, entry.tris);
            tape_slot_release(entry.tape_slot);
        }
        mesh_retire[ring_index].clear();
    }

    // Evict the least-recently-used cache entry that is provably outside the
    // in-flight window. Returns false when every entry is too recent (the
    // caller then builds a one-shot entry instead). Destruction is immediate:
    // an entry whose last use is at least kMaxBatchesInFlight batches old is
    // not referenced by any unretired batch (the ring reuse the request
    // buffers rely on proves that window has retired), so its memory can be
    // reclaimed right now — which is what the allocation-pressure retry in
    // get_or_build_mesh_entry depends on.
    bool evict_lru_mesh_entry(uint32_t ring_index) {
        (void)ring_index;
        auto victim = mesh_cache.end();
        for (auto it = mesh_cache.begin(); it != mesh_cache.end(); ++it) {
            if (it->second.last_used_batch + kMaxBatchesInFlight >
                batch_counter)
                continue;   // may still be referenced by an unretired batch
            if (victim == mesh_cache.end() ||
                it->second.last_used_batch < victim->second.last_used_batch)
                victim = it;
        }
        if (victim == mesh_cache.end()) return false;
        if (victim->second.set)
            vkFreeDescriptorSets(device, descriptor_pool, 1,
                                 &victim->second.set);
        destroy_raw_buffer(device, victim->second.charts);
        destroy_raw_buffer(device, victim->second.tris);
        // The arena slot follows the same proof: an entry outside the
        // in-flight window has no unretired batch reading its ops.
        tape_slot_release(victim->second.tape_slot);
        mesh_cache.erase(victim);
        return true;
    }

    VtTilesetSlotViews tileset_slots[kMaxDetailSlots]{};
    uint32_t tileset_slot_count = 0;

    WeightMode weight_mode = WeightMode::kTriangleMaterial;
    uint32_t debug_mat_a = 0, debug_mat_b = 0;
    float debug_blend_start = 0.0f, debug_blend_width = 1.0f;
    // Reused across fill() calls so the candidate scan allocates nothing.
    std::vector<uint32_t> scratch_cands;

    bool init_recorded = false;

    ~Impl() { destroy(); }

    void destroy() {
        if (!device) return;
        for (auto& kv : mesh_cache) {
            destroy_raw_buffer(device, kv.second.charts);
            destroy_raw_buffer(device, kv.second.tris);
        }
        mesh_cache.clear();
        for (uint32_t i = 0; i < kMaxBatchesInFlight; ++i)
            flush_mesh_retire(i);
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
        destroy_raw_buffer(device, tape_arena);
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
                                       const VtPartContext* ctx, Stats& stats,
                                       uint32_t ring_index, const char*& why);
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
             binding(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1),
             // P2: the shared tape-op arena (vt_surface_tape.glsl).
             binding(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1)},
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
    // P2: the tape-op arena + its slot free list, and the process-wide env
    // gate (vt_types.h — read once, so a session can never mix modes).
    if (!create_raw_buffer(device, phys,
                           VkDeviceSize(kTapeArenaSlots) * kTapeSlotOps *
                               sizeof(VtGpuSurfOp),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                           tape_arena, err))
        return false;
    std::memset(tape_arena.mapped, 0, static_cast<size_t>(tape_arena.size));
    tape_free_slots.reserve(kTapeArenaSlots);
    for (uint32_t i = kTapeArenaSlots; i-- > 0;) tape_free_slots.push_back(i);
    // Same rule as vt_tape_gpu_enabled() (vt_types.h) but read PER INSTANCE
    // rather than through its process-wide cache, so a test can construct an
    // escape-hatch compositor after flipping the env. In a real session the
    // env is set before launch and every reader agrees.
    {
        const char* v = std::getenv("MATTER_VT_TAPE_GPU");
        tape_gpu_enabled = !(v != nullptr && v[0] == '0' && v[1] == '\0');
    }
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
        // One-shot mesh entries (cache full of in-flight entries) allocate
        // sets beyond the cache budget: at most kMaxRequestsPerFill per batch
        // across kMaxBatchesInFlight unretired batches.
        const uint32_t oneshot_sets =
            kMaxBatchesInFlight * kMaxRequestsPerFill;
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             (kMaxMeshEntries + oneshot_sets) * 2 + kMaxBatchesInFlight * 7},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxBatchesInFlight},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             kMaxBatchesInFlight * kTilesetArraySize},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxBatchesInFlight * 7},
        };
        VkDescriptorPoolCreateInfo info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = kMaxMeshEntries + oneshot_sets + ring_sets;
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
    VkDescriptorBufferInfo tape_ops{tape_arena.buffer, 0, VK_WHOLE_SIZE};
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
    write_buf(r.batch_set, 9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &tape_ops);
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
    Stats& stats, uint32_t ring_index, const char*& why) {
    const auto key = std::make_pair(variant_hash, rung);
    auto it = mesh_cache.find(key);
    if (it != mesh_cache.end()) {
        it->second.last_used_batch = batch_counter;
        return &it->second;
    }
    if (atlas->charts.empty()) {
        why = "empty chart table";
        return nullptr;
    }
    // Full cache: evict the LRU entry (streamed worlds register far more
    // variant-rungs than kMaxMeshEntries; a hard stop here used to leave
    // every later variant's pages — its pinned TAIL included — silently
    // unfilled, i.e. whole sectors rendering black). When every entry is
    // still inside the in-flight window (a burst of distinct variants), the
    // entry is built as a ONE-SHOT: it serves this batch from the retire
    // list and is destroyed when the ring cycles, so a fill request is never
    // dropped for cache pressure.
    bool cache_it = true;
    if (mesh_cache.size() >= kMaxMeshEntries) {
        if (evict_lru_mesh_entry(ring_index))
            ++stats.mesh_cache_evictions;
        else
            cache_it = false;
    }

    // P2: try to promote the entry to weight-seam mode 3 — the part must
    // carry BOTH the per-vertex weight columns (the has-tape gate + mode-2
    // fallback) and the canonical tape text, the env gate must be open, the
    // text must parse, the pack must fit the lane cap, and the producer's
    // lane stream must agree with our scan of the same text. Every failure
    // is fail-soft: the entry packs mode-2 weight columns exactly as before.
    VtSurfaceTapePack tape_pack;
    bool mode3 = false;
    const bool has_tape = vt_context_has_tape(*ctx);
    if (has_tape && tape_gpu_enabled && ctx->surface_tape_text != nullptr) {
        terrain_field::SurfaceProgram prog;
        std::string perr;
        if (!terrain_field::SurfaceProgram::parse(ctx->surface_tape_text,
                                                  prog, perr)) {
            ++stats.tape_pack_failures;
        } else if (!vt_pack_surface_tape(
                       prog, ctx->surface_world_anchored != 0, tape_pack)) {
            if (tape_pack.lane_overflow) {
                ++stats.tape_lane_overflows;
                if (!tape_overflow_warned) {
                    tape_overflow_warned = true;
                    std::fprintf(
                        stderr,
                        "[vt] WARNING: surfaces() tape %016llx needs more "
                        "than %u field-derived vertex lanes (overflow: "
                        "input code %d, curv radius %.3f) -- the part stays "
                        "on weight-seam mode 2 (per-vertex weights). "
                        "Warning once.\n",
                        static_cast<unsigned long long>(
                            ctx->surface_tape_hash),
                        kVtMaxSurfaceLanes, tape_pack.scan.overflow_code,
                        tape_pack.scan.overflow_radius);
                    std::fflush(stderr);
                }
            } else {
                ++stats.tape_pack_failures;
            }
        } else if (tape_pack.weight_reg_count !=
                       ctx->surface_material_count ||
                   tape_pack.scan.count != ctx->surface_lane_count ||
                   (tape_pack.scan.count > 0 &&
                    ctx->surface_lanes == nullptr)) {
            // Producer/packer disagreement (stale lanes after a live tape
            // edit that missed a path, or a column-count mismatch): fail
            // soft, count it — mode 2 renders correctly from the weights.
            ++stats.tape_pack_failures;
        } else {
            mode3 = true;
        }
    }

    // Chart-grouped triangle reorder + per-vertex plane coordinates + WP-F tape
    // weight packing: vt_chart_gpu.h owns that packing (WP-H's enricher builds
    // the identical streams from the same function). Mode-3 entries pack the
    // per-vertex f16 field lanes instead of the u8 weight columns.
    std::vector<GpuChart> gcharts;
    std::vector<GpuTri> gtris;
    {
        // O(triangles) repack of the variant's whole mesh. If ONE variant is
        // simply enormous, the cost lands here and vt.mesh_tris says so.
        PROFILE_SCOPE("vt.chart_streams");
        if (!vt_build_chart_gpu_streams(*atlas, *ctx, gcharts, gtris,
                                        mode3 ? ctx->surface_lanes : nullptr,
                                        mode3 ? ctx->surface_lane_count : 0u)) {
            why = "chart GPU stream build failed";
            return nullptr;
        }
    }
    PROFILE_COUNT("vt.mesh_tris", gtris.size());

    MeshEntry entry;
    if (mode3) {
        entry.tape_slot = tape_slot_acquire();
        if (entry.tape_slot < 0) {
            // Arena exhausted (cannot happen while kTapeArenaSlots covers the
            // cache + one-shot worst case, but fail soft anyway)... except the
            // triangle stream above was packed with lanes. Rebuild it with
            // weight columns so mode 2 reads real data.
            ++stats.tape_pack_failures;
            mode3 = false;
            if (!vt_build_chart_gpu_streams(*atlas, *ctx, gcharts, gtris)) {
                why = "chart GPU stream rebuild failed";
                return nullptr;
            }
        } else {
            entry.tape_op_count =
                static_cast<uint32_t>(tape_pack.ops.size());
            entry.tape_lane_count = tape_pack.scan.count;
            std::memcpy(entry.tape_weight_reg, tape_pack.weight_reg,
                        sizeof(entry.tape_weight_reg));
            std::memcpy(entry.tape_tint_reg, tape_pack.tint_reg,
                        sizeof(entry.tape_tint_reg));
            entry.tape_rough_reg = tape_pack.rough_bias_reg;
            entry.tape_wet_reg = tape_pack.wetness_reg;
            entry.tape_metal_reg = tape_pack.metallic_reg;
            auto* arena_ops = static_cast<VtGpuSurfOp*>(tape_arena.mapped) +
                              static_cast<size_t>(entry.tape_slot) *
                                  kTapeSlotOps;
            std::memcpy(arena_ops, tape_pack.ops.data(),
                        tape_pack.ops.size() * sizeof(VtGpuSurfOp));
            ++stats.tape_mode3_entries;
        }
    }
    std::string err;
    const VkDeviceSize charts_bytes = sizeof(GpuChart) * gcharts.size();
    const VkDeviceSize tris_bytes = sizeof(GpuTri) * gtris.size();
    auto try_create_buffers = [&]() {
        return create_raw_buffer(device, phys, charts_bytes,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                                 entry.charts, err) &&
               create_raw_buffer(device, phys, tris_bytes,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                                 entry.tris, err);
    };
    PROFILE_SCOPE_NAMED(z_mesh_alloc, "vt.mesh_alloc");
    if (!try_create_buffers()) {
        // Host-visible memory pressure: shed every evictable cache entry and
        // retry once before giving up on this fill.
        //
        // PRIME SUSPECT for the 364.5 ms single-call outlier a 2026-08-08
        // capture caught (37% of all mesh-entry time in ONE call, next largest
        // 13.2 ms). This loop destroys buffers and frees descriptor sets for up
        // to kMaxMeshEntries = 512 entries in one go, and the cache is heavily
        // oversubscribed -- that capture had 2150 live variants against 512
        // slots -- so a single trip through here is a whole-cache wipe followed
        // by a rebuild of everything still wanted. vt.mesh_cache_wipes counts
        // the trips and vt.mesh_shed counts what each one destroyed; a spike in
        // either alongside a spike in vt.mesh_alloc confirms it.
        // SHED INCREMENTALLY, retrying as we go. This loop used to destroy the
        // ENTIRE evictable cache and then retry once, which a 2026-08-08
        // capture caught costing 352.9 ms in a single call -- it shed 498 of
        // 512 entries to make room for one, then every variant still on screen
        // had to be rebuilt over the following frames. That was the largest
        // hitch in the trace by a factor of 28.
        //
        // Freeing enough is almost always a handful of entries (an entry is
        // charts + tris, a few hundred KB at the observed ~5k triangles), so
        // retrying in small batches turns a whole-cache wipe into a few
        // evictions. The batch keeps the failed-allocation retries bounded too:
        // a failing vkAllocateMemory is not cheap, so this does not retry after
        // every single eviction.
        PROFILE_COUNT("vt.mesh_cache_wipes", 1);
        destroy_raw_buffer(device, entry.charts);
        destroy_raw_buffer(device, entry.tris);
        constexpr uint32_t kShedBatch = 8;
        uint64_t shed = 0;
        bool created = false;
        while (!created) {
            uint32_t batch = 0;
            while (batch < kShedBatch && evict_lru_mesh_entry(ring_index)) {
                ++stats.mesh_cache_evictions;
                ++shed;
                ++batch;
            }
            if (batch == 0) break;   // nothing left to give up
            if (try_create_buffers()) {
                created = true;
                break;
            }
            // Partial success leaks the first buffer; clear both before the
            // next attempt (try_create_buffers is short-circuiting).
            destroy_raw_buffer(device, entry.charts);
            destroy_raw_buffer(device, entry.tris);
        }
        PROFILE_COUNT("vt.mesh_shed", shed);
        if (!created) {
            destroy_raw_buffer(device, entry.charts);
            destroy_raw_buffer(device, entry.tris);
            tape_slot_release(entry.tape_slot);
            why = "mesh buffer allocation failed";
            return nullptr;
        }
    }
    z_mesh_alloc.stop();
    {
        // First touch of freshly mapped host-visible memory, so this is page
        // faults as much as copy bandwidth -- worth separating from the
        // allocation itself.
        PROFILE_SCOPE("vt.mesh_upload");
        std::memcpy(entry.charts.mapped, gcharts.data(), charts_bytes);
        std::memcpy(entry.tris.mapped, gtris.data(), tris_bytes);
    }
    entry.chart_count = static_cast<uint32_t>(gcharts.size());
    entry.last_used_batch = batch_counter;

    VkDescriptorSetAllocateInfo alloc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptor_pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &mesh_layout;
    if (vkAllocateDescriptorSets(device, &alloc, &entry.set) != VK_SUCCESS) {
        destroy_raw_buffer(device, entry.charts);
        destroy_raw_buffer(device, entry.tris);
        tape_slot_release(entry.tape_slot);
        why = "mesh descriptor set allocation failed";
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
    if (!cache_it) {
        mesh_retire[ring_index].push_back(std::move(entry));
        return &mesh_retire[ring_index].back();
    }
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
    // DEFERRED, never in place. This runs from drain_vt_invalidations() inside
    // vt_begin_frame() -- on the render thread, with earlier frames' command
    // buffers still executing. Destroying here freed descriptor sets, chart/tri
    // buffers and tape slots that an unretired fill() batch was still reading:
    //
    //   vkFreeDescriptorSets(): pDescriptorSets[0] can't be called on
    //   VkDescriptorSet ... that is currently in use by VkCommandBuffer ...
    //   (VUID-vkFreeDescriptorSets-pDescriptorSets-00309)
    //
    // followed by VK_ERROR_DEVICE_LOST. The two sibling paths in this file both
    // already carry the proof this one lacked: evict_lru_mesh_entry() refuses
    // any entry inside the in-flight window, and one-shot entries park in
    // mesh_retire[] until their ring comes round again. VtEnricher::invalidate_
    // part() was moved onto the same discipline in an earlier pass; the
    // compositor half was missed even though drain_vt_invalidations() calls
    // both, back to back, at the same point in the frame.
    //
    // The retire list is keyed to the ring of the MOST RECENT batch, which is
    // the one behind ring_cursor -- fill() flushes a ring just before reusing
    // it, so parking here buys exactly kMaxBatchesInFlight batches, the same
    // window the one-shot path relies on. Parking under ring_cursor itself
    // would be freed by the very next fill().
    const uint32_t retire_ring =
        (impl_->ring_cursor + kMaxBatchesInFlight - 1u) % kMaxBatchesInFlight;
    for (auto it = impl_->mesh_cache.begin(); it != impl_->mesh_cache.end();) {
        if (it->first.first == variant_hash) {
            impl_->mesh_retire[retire_ring].push_back(std::move(it->second));
            it = impl_->mesh_cache.erase(it);
        } else {
            ++it;
        }
    }
}

bool VtCompositor::tape_gpu_enabled() const {
    return impl_->tape_gpu_enabled;
}

void VtCompositor::fill(VkCommandBuffer cmd, const VtFillRequest* batch,
                        size_t count) {
    Impl& im = *impl_;
    if (!im.init_recorded) im.record_init(cmd);
    if (!batch || count == 0) return;

    const uint32_t ring_index = im.ring_cursor;
    Impl::Ring& ring = im.rings[ring_index];
    im.ring_cursor = (im.ring_cursor + 1) % kMaxBatchesInFlight;
    // This ring's previous batch has retired (the host-visible request/cand
    // rewrites below already depend on that), so mesh entries evicted while
    // that batch was recorded are now unreferenced and safe to destroy.
    im.flush_mesh_retire(ring_index);
    ++im.batch_counter;

    struct Rec {
        const Impl::MeshEntry* entry;
        const VtPoolBinding* pool;
        const VtFillRequest* request;   // for mark_filled() after the copies
        uint32_t req_index;      // slot in the ring's request buffer
        uint32_t physical_slot;
    };
    std::vector<Rec> recs;
    recs.reserve(std::min<size_t>(count, kMaxRequestsPerFill));

    auto* gpu_reqs = static_cast<GpuFillRequest*>(ring.requests.mapped);
    auto* gpu_cands = static_cast<uint32_t*>(ring.cands.mapped);
    uint32_t cand_cursor = 0;

    // A skipped request is NOT harmless: the residency layer maps the
    // indirection entry before fill() runs (vt_residency.cpp record_frame),
    // so a skip leaves that page pointing at never-written pool memory,
    // which BC7-decodes to black. Log the first few with a reason so the
    // failure is visible instead of rendering as silent black terrain.
    static std::atomic<uint32_t> skip_log_budget{32};
    auto log_skip = [&](const VtFillRequest& r, const char* reason) {
        if (skip_log_budget.load(std::memory_order_relaxed) == 0) return;
        if (skip_log_budget.fetch_sub(1, std::memory_order_relaxed) == 0)
            return;
        std::fprintf(stderr,
                     "[vt] compositor SKIPPED fill (%s): variant=%016llx "
                     "rung=%u mip=%u page=(%u,%u) slot=%u — page will render "
                     "black\n",
                     reason,
                     static_cast<unsigned long long>(r.variant_hash),
                     unsigned(r.rung), unsigned(r.mip), unsigned(r.page_x),
                     unsigned(r.page_y), unsigned(r.physical_slot));
        std::fflush(stderr);
    };

    for (size_t i = 0; i < count; ++i) {
        const VtFillRequest& req = batch[i];
        const VtPoolBinding* pool = req.pool;
        if (!req.atlas || !req.part_context || !pool ||
            !pool->image[kVtChannelAlbedo] || !pool->image[kVtChannelNormal] ||
            !pool->image[kVtChannelOrm] || !pool->image[kVtChannelAux] ||
            recs.size() >= kMaxRequestsPerFill) {
            ++stats_.requests_skipped;
            log_skip(req, "missing inputs or batch overflow");
            continue;
        }
        {
            uint32_t layer, sx, sy;
            vt_slot_origin(req.physical_slot, layer, sx, sy);
            if (pool->layer_count != 0 && layer >= pool->layer_count) {
                ++stats_.requests_skipped;
                log_skip(req, "physical slot outside pool");
                continue;
            }
        }
        const auto* ctx = static_cast<const VtPartContext*>(req.part_context);
        if (!ctx->positions || !ctx->indices || ctx->triangle_count == 0) {
            ++stats_.requests_skipped;
            log_skip(req, "part context has no CPU mesh");
            continue;
        }
        const char* why = "unknown";
        // The suspect. A 2026-08-08 capture caught a single fill() at 348.9 ms
        // with vt.fill_batch <= 4 -- roughly 87 ms per page, which is far too
        // much for compositing and points at mesh-entry CONSTRUCTION instead:
        // chart/tri buffer creation and upload, descriptor allocation, or the
        // allocation-pressure retry this function falls into when the cache is
        // full of in-flight entries. Split out so the next capture says which.
        Impl::MeshEntry* entry = nullptr;
        {
            PROFILE_SCOPE("vt.mesh_entry");
            // A cache HIT is a map lookup; a MISS builds buffers and a
            // descriptor set. This counter is what separates "the cache is
            // thrashing" from "one entry is genuinely enormous".
            const uint64_t builds_before = stats_.mesh_cache_builds;
            entry = im.get_or_build_mesh_entry(req.variant_hash, req.rung,
                                               req.atlas, ctx, stats_,
                                               ring_index, why);
            PROFILE_COUNT("vt.mesh_builds",
                          stats_.mesh_cache_builds - builds_before);
        }
        if (!entry) {
            ++stats_.requests_skipped;
            log_skip(req, why);
            continue;
        }

        // Candidate charts (vt_chart_gpu.h): rects intersecting the page's
        // finest-mip footprint expanded by a dilation margin, ascending chart
        // index (fixed order — determinism); none -> the nearest chart by rect
        // distance so dilation always has content. Shared with WP-H's enricher
        // so both passes resolve a texel against the same candidate set.
        const uint32_t cand_offset = cand_cursor;
        im.scratch_cands.clear();
        {
            // Per-page chart search over the variant's rect table. Scales with
            // chart count, so a heavily-charted variant pays here every page.
            PROFILE_SCOPE("vt.candidates");
            vt_page_candidate_charts(*req.atlas, req.page_x, req.page_y,
                                     req.mip, im.scratch_cands);
        }
        const uint32_t cand_count =
            static_cast<uint32_t>(im.scratch_cands.size());
        if (cand_count == 0 ||
            cand_cursor + cand_count > kMaxCandEntriesPerFill) {
            ++stats_.requests_skipped;
            log_skip(req, cand_count == 0 ? "no candidate charts"
                                          : "candidate buffer overflow");
            continue;
        }
        std::memcpy(gpu_cands + cand_cursor, im.scratch_cands.data(),
                    cand_count * sizeof(uint32_t));
        cand_cursor += cand_count;

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
        // P2: an entry whose tape was packed for the GPU interpreter promotes
        // straight to mode 3 (its triangle stream carries field lanes, not
        // weight columns — the two modes travel together with the entry).
        const bool has_tape =
            ctx->surface_material_count > 0 &&
            ctx->surface_material_count <= kMaxSurfaceMaterials &&
            ctx->surface_weights != nullptr &&
            ctx->surface_materials != nullptr;
        const bool mode3 = entry->tape_slot >= 0;
        const WeightMode mode =
            (has_tape && im.weight_mode == WeightMode::kTriangleMaterial)
                ? (mode3 ? WeightMode::kSurfaceTapeGpu
                         : WeightMode::kSurfaceTape)
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
        g.tape2[0] = 0;
        g.tape2[1] = 0;
        g.tape2[2] = 0;
        g.tape2[3] = 0;
        // Identity transform by default; overwritten for mode-3 requests.
        static constexpr float kIdentity12[12] = {1, 0, 0, 0, 0, 1,
                                                  0, 0, 0, 0, 1, 0};
        std::memcpy(g.xform, kIdentity12, sizeof(g.xform));
        // P3: no appearance directives by default (three sentinel bytes in x;
        // w carries the metallic register and must default to the sentinel
        // too — 0 is a valid register index).
        g.app[0] = uint32_t(kVtNoAppearanceReg) |
                   (uint32_t(kVtNoAppearanceReg) << 8) |
                   (uint32_t(kVtNoAppearanceReg) << 16);
        g.app[1] = kVtNoAppearanceReg;
        g.app[2] = kVtNoAppearanceReg;
        g.app[3] = kVtNoAppearanceReg;
        if (has_tape) {
            for (uint32_t k = 0; k < ctx->surface_material_count; ++k)
                g.tape[k >> 2] |= (ctx->surface_materials[k] & 0xFFu)
                                  << ((k & 3u) * 8u);
            g.tape[2] = ctx->surface_material_count;
        }
        if (mode == WeightMode::kSurfaceTapeGpu && mode3) {
            g.tape[3] = entry->tape_lane_count;
            g.tape2[0] = static_cast<uint32_t>(entry->tape_slot) *
                         kTapeSlotOps;
            g.tape2[1] = entry->tape_op_count;
            for (uint32_t k = 0; k < 8u; ++k)
                g.tape2[2 + (k >> 2)] |=
                    static_cast<uint32_t>(entry->tape_weight_reg[k])
                    << ((k & 3u) * 8u);
            g.app[0] = uint32_t(entry->tape_tint_reg[0]) |
                       (uint32_t(entry->tape_tint_reg[1]) << 8) |
                       (uint32_t(entry->tape_tint_reg[2]) << 16);
            g.app[1] = entry->tape_rough_reg;
            g.app[2] = entry->tape_wet_reg;
            g.app[3] = entry->tape_metal_reg;
            std::memcpy(g.xform, ctx->surface_local_to_world,
                        sizeof(g.xform));
        }
        recs.push_back(Rec{entry, pool, &req, rec_index, req.physical_slot});
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
            // The page is written: tell the residency layer it may map the
            // indirection entry (vt_types.h VtFillRequest::out_filled). Every
            // `continue` in the request loop above therefore leaves the flag
            // false and the entry unmapped, instead of black.
            rec.request->mark_filled();
            ++stats_.pages_filled;
        }
    }
}

}  // namespace vt
