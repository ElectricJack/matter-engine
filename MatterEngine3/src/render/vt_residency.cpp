#include "vt_residency.h"

#include <cstdio>
#include <cstdlib>

#include "matter/vulkan_device.h"
#include "vk_resources.h"

namespace vt {
namespace {

uint64_t variant_key(uint64_t hash, uint32_t rung) {
    return hash ^ (0x9E3779B97F4A7C15ull * (rung + 1u));
}

uint64_t page_key(uint32_t layer, const VtPageKey& p) {
    return (static_cast<uint64_t>(layer) << 40) |
           (static_cast<uint64_t>(p.mip) << 32) |
           (static_cast<uint64_t>(p.py) << 16) | p.px;
}

uint32_t env_u32(const char* name, uint32_t fallback, uint32_t lo, uint32_t hi) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) return fallback;
    if (parsed < lo) return lo;
    if (parsed > hi) return hi;
    return static_cast<uint32_t>(parsed);
}

void barrier(VkCommandBuffer cmd, VkImage image, uint32_t layers,
             VkImageLayout old_layout, VkImageLayout new_layout,
             VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
             VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

VtResidency::VtResidency() = default;
VtResidency::~VtResidency() { shutdown(); }

// ---------------------------------------------------------------------------
// Resource creation
// ---------------------------------------------------------------------------

bool VtResidency::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags properties, Buffer& out,
                                std::string& error) {
    destroy_buffer(out);
    if (size == 0) return true;
    const VkDevice device = vulkan_->device();
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = size;
    create.usage = usage;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &create, nullptr, &out.buffer) != VK_SUCCESS) {
        error = "vt: vkCreateBuffer failed";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
    uint32_t type = 0;
    VkMemoryPropertyFlags selected = 0;
    if (!matter::find_memory_type(vulkan_->physical_device(),
                                  requirements.memoryTypeBits, properties,
                                  properties, type, selected, error)) {
        destroy_buffer(out);
        return false;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &allocate, nullptr, &out.memory) != VK_SUCCESS) {
        error = "vt: vkAllocateMemory(buffer) failed";
        destroy_buffer(out);
        return false;
    }
    if (vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS) {
        error = "vt: vkBindBufferMemory failed";
        destroy_buffer(out);
        return false;
    }
    out.size = size;
    if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        if (vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) !=
            VK_SUCCESS) {
            error = "vt: vkMapMemory failed";
            destroy_buffer(out);
            return false;
        }
    }
    return true;
}

void VtResidency::destroy_buffer(Buffer& b) {
    if (!vulkan_) {
        b = Buffer{};
        return;
    }
    const VkDevice device = vulkan_->device();
    if (b.mapped) vkUnmapMemory(device, b.memory);
    if (b.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory != VK_NULL_HANDLE) vkFreeMemory(device, b.memory, nullptr);
    b = Buffer{};
}

namespace {
bool create_array_image(matter::VulkanDevice& vulkan, VkFormat format,
                        uint32_t width, uint32_t height, uint32_t layers,
                        VkImageUsageFlags usage, VkImage& image,
                        VkImageView& view, VkDeviceMemory& memory,
                        std::string& error,
                        VkImageViewType view_type =
                            VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
    const VkDevice device = vulkan.device();
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = format;
    create.extent = {width, height, 1};
    create.mipLevels = 1;
    create.arrayLayers = layers;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = usage;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &create, nullptr, &image) != VK_SUCCESS) {
        error = "vt: vkCreateImage failed";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    uint32_t type = 0;
    VkMemoryPropertyFlags selected = 0;
    if (!matter::find_memory_type(vulkan.physical_device(),
                                  requirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type,
                                  selected, error)) {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS) {
        error = "vt: vkAllocateMemory(image) failed";
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
        error = "vt: vkBindImageMemory failed";
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    VkImageViewCreateInfo view_create{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_create.image = image;
    view_create.viewType = view_type;
    view_create.format = format;
    view_create.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    if (vkCreateImageView(device, &view_create, nullptr, &view) != VK_SUCCESS) {
        error = "vt: vkCreateImageView failed";
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}
}  // namespace

bool VtResidency::create_pool_image(uint32_t channel, VkFormat format,
                                    uint32_t layers, std::string& error) {
    PoolImage& out = pool_[channel];
    destroy_pool_image(out);
    if (!create_array_image(*vulkan_, format, kVtPoolLayerEdgeTexels,
                            kVtPoolLayerEdgeTexels, layers,
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            out.image, out.view, out.memory, error)) {
        return false;
    }
    out.format = format;
    out.layers = layers;
    out.edge = kVtPoolLayerEdgeTexels;
    out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void VtResidency::destroy_pool_image(PoolImage& image) {
    if (!vulkan_) {
        image = PoolImage{};
        return;
    }
    const VkDevice device = vulkan_->device();
    if (image.view != VK_NULL_HANDLE) vkDestroyImageView(device, image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) vkDestroyImage(device, image.image, nullptr);
    if (image.memory != VK_NULL_HANDLE) vkFreeMemory(device, image.memory, nullptr);
    image = PoolImage{};
}

bool VtResidency::create_indirection(uint32_t layers, std::string& error) {
    destroy_pool_image(indirection_);
    if (!create_array_image(*vulkan_, VK_FORMAT_R16G16_UINT,
                            kVtIndirectionWidth, kVtIndirectionHeight, layers,
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            indirection_.image, indirection_.view,
                            indirection_.memory, error)) {
        return false;
    }
    indirection_.format = VK_FORMAT_R16G16_UINT;
    indirection_.layers = layers;
    indirection_.edge = kVtIndirectionWidth;
    indirection_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

bool VtResidency::init(matter::VulkanDevice& vulkan, std::string& error) {
    if (ready_) return true;
    vulkan_ = &vulkan;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(vulkan.physical_device(), &properties);
    const uint32_t max_layers = properties.limits.maxImageArrayLayers;

    pool_pages_ = env_u32("MATTER_VT_POOL_PAGES", 8192u, kVtPagesPerLayer,
                          kVtPagesPerLayer * 256u);
    // Round up to whole layers.
    const uint32_t pool_layers =
        (pool_pages_ + kVtPagesPerLayer - 1u) / kVtPagesPerLayer;
    pool_pages_ = pool_layers * kVtPagesPerLayer;
    if (pool_layers > max_layers) {
        error = "vt: pool layer count exceeds maxImageArrayLayers";
        shutdown();
        return false;
    }
    max_variants_ = env_u32("MATTER_VT_MAX_VARIANTS", 1024u, 4u,
                            max_layers < 4096u ? max_layers : 4096u);
    max_fills_per_frame_ = env_u32("MATTER_VT_FILLS_PER_FRAME", 8u, 1u, 64u);
    mesh_budget_bytes_ =
        static_cast<size_t>(env_u32("MATTER_VT_MESH_BUDGET_MB", 256u, 1u,
                                    8192u)) *
        1024u * 1024u;

    const VkFormat formats[kVtChannelCount] = {
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM};
    for (uint32_t c = 0; c < kVtChannelCount; ++c) {
        VkFormatProperties format_properties{};
        vkGetPhysicalDeviceFormatProperties(vulkan.physical_device(), formats[c],
                                            &format_properties);
        if ((format_properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
            error = "vt: required pool format is not sampleable on this device";
            shutdown();
            return false;
        }
        if (!create_pool_image(c, formats[c], pool_layers, error)) {
            shutdown();
            return false;
        }
    }
    if (!create_indirection(max_variants_, error)) {
        shutdown();
        return false;
    }

    VkSamplerCreateInfo linear{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    linear.magFilter = VK_FILTER_LINEAR;
    linear.minFilter = VK_FILTER_LINEAR;
    linear.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    linear.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    linear.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    linear.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    linear.minLod = 0.0f;
    linear.maxLod = 0.25f;
    if (vkCreateSampler(vulkan.device(), &linear, nullptr, &pool_sampler_) !=
        VK_SUCCESS) {
        error = "vt: vkCreateSampler(pool) failed";
        shutdown();
        return false;
    }
    VkSamplerCreateInfo point = linear;
    point.magFilter = VK_FILTER_NEAREST;
    point.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(vulkan.device(), &point, nullptr, &point_sampler_) !=
        VK_SUCCESS) {
        error = "vt: vkCreateSampler(point) failed";
        shutdown();
        return false;
    }

    variant_records_.assign(max_variants_, VariantRecordGpu{});
    if (!create_buffer(sizeof(VariantRecordGpu) * max_variants_,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       variant_buffer_, error)) {
        shutdown();
        return false;
    }
    std::memcpy(variant_buffer_.mapped, variant_records_.data(),
                variant_buffer_.size);
    // Staging for indirection uploads: one full layer per pending variant
    // update, bounded by the fill budget plus registrations in a frame.
    const VkDeviceSize indirection_layer_bytes =
        static_cast<VkDeviceSize>(kVtIndirectionWidth) * kVtIndirectionHeight * 4u;
    if (!create_buffer(indirection_layer_bytes * (max_fills_per_frame_ + 8u),
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       indirection_staging_, error)) {
        shutdown();
        return false;
    }

    slots_.reset(pool_pages_);
    variants_.assign(max_variants_, VariantRung{});
    free_layers_.clear();
    free_layers_.reserve(max_variants_);
    for (uint32_t i = max_variants_; i-- > 0;) free_layers_.push_back(i);

    for (uint32_t c = 0; c < kVtChannelCount; ++c) {
        pool_binding_.image[c] = pool_[c].image;
        pool_binding_.format[c] = pool_[c].format;
    }
    pool_binding_.layer_count = pool_layers;
    pool_binding_.transfer_dst_layout = true;

    if (!filler_) {
        std::unique_ptr<VtPageFiller> stub =
            make_vt_stub_filler(vulkan, max_fills_per_frame_, error);
        if (!stub) {
            shutdown();
            return false;
        }
        filler_ = std::move(stub);
    }

    // Pool bytes: BC7 + BC5 + BC7 are 1 byte/texel, aux is 4.
    const uint64_t layer_texels = static_cast<uint64_t>(kVtPoolLayerEdgeTexels) *
                                  kVtPoolLayerEdgeTexels;
    stats_ = Stats{};
    stats_.pool_capacity = pool_pages_;
    stats_.pool_bytes = layer_texels * pool_layers * (1 + 1 + 1 + 4);
    ready_ = true;
    return true;
}

void VtResidency::shutdown() {
    if (!vulkan_) {
        ready_ = false;
        return;
    }
    filler_.reset();
    for (uint32_t c = 0; c < kVtChannelCount; ++c) destroy_pool_image(pool_[c]);
    destroy_pool_image(indirection_);
    destroy_pool_image(feedback_);
    const VkDevice device = vulkan_->device();
    if (pool_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device, pool_sampler_, nullptr);
    if (point_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device, point_sampler_, nullptr);
    pool_sampler_ = VK_NULL_HANDLE;
    point_sampler_ = VK_NULL_HANDLE;
    destroy_buffer(variant_buffer_);
    destroy_buffer(indirection_staging_);
    for (uint32_t i = 0; i < kFeedbackSlots; ++i)
        destroy_buffer(feedback_readback_[i]);
    variants_.clear();
    free_layers_.clear();
    layer_of_.clear();
    queue_.clear();
    queued_keys_.clear();
    variant_records_.clear();
    mesh_bytes_used_ = 0;
    feedback_w_ = feedback_h_ = 0;
    ready_ = false;
    vulkan_ = nullptr;
}

VkImageView VtResidency::pool_view(uint32_t channel) const {
    return channel < kVtChannelCount ? pool_[channel].view : VK_NULL_HANDLE;
}

void VtResidency::set_filler(std::unique_ptr<VtPageFiller> filler) {
    filler_ = std::move(filler);
}

// ---------------------------------------------------------------------------
// Variant registration
// ---------------------------------------------------------------------------

uint32_t VtResidency::register_variant(uint64_t variant_hash, uint32_t rung,
                                       const chart_atlas::ChartAtlasRung& atlas,
                                       const VtPartContext& context) {
    if (!ready_) return kVtNoSlot;
    if (atlas.charts.empty() || atlas.atlas_w == 0 || atlas.atlas_h == 0)
        return kVtNoSlot;
    const uint64_t key = variant_key(variant_hash, rung);
    const auto found = layer_of_.find(key);
    if (found != layer_of_.end()) return found->second + 1u;

    VtVariantLayout layout{};
    if (!vt_build_layout(atlas.atlas_w, atlas.atlas_h, layout))
        return kVtNoSlot;
    if (free_layers_.empty()) {
        ++stats_.rejected_variants;
        return kVtNoSlot;
    }

    // Budget the CPU mesh copy BEFORE taking any slot, so a rejection leaves
    // no partial registration behind.
    const size_t mesh_bytes =
        static_cast<size_t>(context.vertex_count) *
            (sizeof(float) * 3 * (context.positions ? 1 : 0) +
             sizeof(float) * 3 * (context.normals ? 1 : 0) +
             sizeof(float) * 2 * (context.surface_uvs ? 1 : 0) +
             sizeof(uint32_t) * (context.material_ids ? 1 : 0) +
             4 * (context.tint_rgba ? 1 : 0)) +
        static_cast<size_t>(context.triangle_count) * 3 * sizeof(uint32_t) +
        static_cast<size_t>(context.material_count) * context.material_stride *
            sizeof(float) +
        atlas.charts.size() * sizeof(chart_atlas::ChartEntry) +
        atlas.tri_order.size() * sizeof(uint32_t);
    if (mesh_bytes_used_ + mesh_bytes > mesh_budget_bytes_) {
        ++stats_.rejected_variants;
        return kVtNoSlot;
    }

    // The tail page is pinned for the variant's whole life: it is what makes
    // "every loaded variant always has valid texels" true.
    uint32_t tail_slot = 0;
    VtSlotPool::Owner evicted;
    const VtPageKey tail_page{layout.mip_count - 1u, 0u, 0u};
    if (!slots_.acquire(key, tail_page, /*pinned=*/true, frame_index_,
                        tail_slot, evicted)) {
        return kVtNoSlot;
    }
    if (evicted.live) {
        // The pool was full of unpinned pages; unmap whatever we recycled.
        const auto owner_layer = layer_of_.find(evicted.variant_key);
        if (owner_layer != layer_of_.end())
            variants_[owner_layer->second].indirection.unmap(
                evicted.page.mip, evicted.page.px, evicted.page.py);
    }

    const uint32_t layer = free_layers_.back();
    free_layers_.pop_back();
    VariantRung& v = variants_[layer];
    v = VariantRung{};
    v.variant_hash = variant_hash;
    v.rung = rung;
    v.layer = layer;
    v.layout = layout;
    v.atlas = atlas;                 // owned copy: the filler borrows this
    v.context = context;
    v.context.atlas = &v.atlas;
    // Adopt the mesh: copy everything the context points at, then repoint.
    // After this the caller's arrays may go away at any time.
    const auto adopt_f = [](const float* src, size_t count,
                            std::vector<float>& dst, const float*& out) {
        if (src == nullptr || count == 0) { out = nullptr; return; }
        dst.assign(src, src + count);
        out = dst.data();
    };
    const size_t vertices = context.vertex_count;
    adopt_f(context.positions, vertices * 3, v.positions, v.context.positions);
    adopt_f(context.normals, vertices * 3, v.normals, v.context.normals);
    adopt_f(context.surface_uvs, vertices * 2, v.surface_uvs,
            v.context.surface_uvs);
    adopt_f(context.material_table,
            static_cast<size_t>(context.material_count) * context.material_stride,
            v.material_table, v.context.material_table);
    if (context.material_ids && vertices != 0) {
        v.material_ids.assign(context.material_ids,
                              context.material_ids + vertices);
        v.context.material_ids = v.material_ids.data();
    } else {
        v.context.material_ids = nullptr;
    }
    if (context.tint_rgba && vertices != 0) {
        v.tint_rgba.assign(context.tint_rgba, context.tint_rgba + vertices * 4);
        v.context.tint_rgba = v.tint_rgba.data();
    } else {
        v.context.tint_rgba = nullptr;
    }
    if (context.indices && context.triangle_count != 0) {
        v.indices.assign(context.indices,
                         context.indices + context.triangle_count * 3u);
        v.context.indices = v.indices.data();
    } else {
        v.context.indices = nullptr;
        v.context.triangle_count = 0;
    }
    v.mesh_bytes = mesh_bytes;
    mesh_bytes_used_ += mesh_bytes;
    v.tail_slot = tail_slot;
    v.tail_filled = false;
    v.live = true;
    v.indirection.reset(layout, tail_slot);
    v.indirection.map(tail_page.mip, 0, 0, tail_slot);
    layer_of_[key] = layer;
    write_variant_record(v);

    // The tail must be filled before anything samples it. It is ALREADY
    // mapped (that mapping is what makes every entry resolvable), so the
    // request has to bypass queue_page's already-resident fast-out -- without
    // `force` the tail would stay whatever undefined bytes its slot held.
    queue_page(v, tail_page, /*force=*/true, /*preassigned_slot=*/tail_slot);
    ++stats_.variants;
    stats_.pool_used = slots_.used();
    stats_.pool_pinned = slots_.pinned();
    stats_.evictions_total = slots_.evictions();
    stats_.queue_depth = static_cast<uint32_t>(queue_.size());
    stats_.mesh_bytes = mesh_bytes_used_;
    return layer + 1u;
}

void VtResidency::release_variant(uint64_t variant_hash) {
    if (!ready_) return;
    for (uint32_t rung = 0; rung < 32u; ++rung) {
        const uint64_t key = variant_key(variant_hash, rung);
        const auto found = layer_of_.find(key);
        if (found == layer_of_.end()) continue;
        const uint32_t layer = found->second;
        VariantRung& v = variants_[layer];
        // Release every slot this variant owns.
        for (uint32_t slot = 0; slot < slots_.capacity(); ++slot) {
            const VtSlotPool::Owner& o = slots_.owner(slot);
            if (o.live && o.variant_key == key) slots_.release(slot);
        }
        // Drop queued fills for it.
        for (size_t i = queue_.size(); i-- > 0;) {
            if (queue_[i].layer == layer) {
                queued_keys_.erase(page_key(layer, queue_[i].page));
                queue_.erase(queue_.begin() + static_cast<long>(i));
            }
        }
        mesh_bytes_used_ -= std::min(mesh_bytes_used_, v.mesh_bytes);
        v = VariantRung{};
        stats_.mesh_bytes = mesh_bytes_used_;
        variant_records_[layer] = VariantRecordGpu{};
        variant_records_dirty_ = true;
        free_layers_.push_back(layer);
        layer_of_.erase(found);
        if (stats_.variants) --stats_.variants;
    }
    stats_.pool_used = slots_.used();
    stats_.pool_pinned = slots_.pinned();
    stats_.queue_depth = static_cast<uint32_t>(queue_.size());
}

uint32_t VtResidency::invalidate_all_content() {
    if (!ready_) return 0;
    // The slot pool is the authority on what is resident: every indirection
    // mapping was created by an acquire() and is unmapped again when its slot
    // is evicted or released, so sweeping live slots covers exactly the
    // resident set (the pinned tails excepted -- those keep their mapping and
    // are re-filled in place below).
    uint32_t dropped = 0;
    for (uint32_t slot = 0; slot < slots_.capacity(); ++slot) {
        const VtSlotPool::Owner owner = slots_.owner(slot);   // copy: release() clears it
        if (!owner.live || owner.pinned) continue;
        const auto owner_layer = layer_of_.find(owner.variant_key);
        if (owner_layer != layer_of_.end())
            variants_[owner_layer->second].indirection.unmap(
                owner.page.mip, owner.page.px, owner.page.py);
        slots_.release(slot);
        ++dropped;
    }

    // Re-fill every pinned tail in place. Queued (not recorded) on purpose:
    // record_frame() drains the queue, and the caller rebinds the filler's
    // inputs before calling this, so a re-queued fill can only ever execute
    // against the NEW inputs.
    for (VariantRung& v : variants_) {
        if (!v.live || !v.layout.valid()) continue;
        const VtPageKey tail{v.layout.mip_count - 1u, 0u, 0u};
        v.tail_filled = false;
        queue_page(v, tail, /*force=*/true, /*preassigned_slot=*/v.tail_slot);
        // Outrank every feedback-derived request (priority is a mip distance,
        // so it never reaches kVtMaxMips). A steady feedback stream would
        // otherwise starve the tails, which is what the whole variant reads
        // wherever no finer page is resident.
        const auto queued = queued_keys_.find(page_key(v.layer, tail));
        if (queued != queued_keys_.end())
            queue_[queued->second].priority = kVtMaxMips;
    }

    ++stats_.invalidations_total;
    stats_.pages_dropped_total += dropped;
    stats_.pool_used = slots_.used();
    stats_.pool_pinned = slots_.pinned();
    stats_.queue_depth = static_cast<uint32_t>(queue_.size());
    return dropped;
}

uint32_t VtResidency::slot_for(uint64_t variant_hash, uint32_t rung) const {
    const auto found = layer_of_.find(variant_key(variant_hash, rung));
    return found == layer_of_.end() ? kVtNoSlot : found->second + 1u;
}

void VtResidency::write_variant_record(const VariantRung& v) {
    VariantRecordGpu& r = variant_records_[v.layer];
    r = VariantRecordGpu{};
    r.atlas_w = v.layout.atlas_w;
    r.atlas_h = v.layout.atlas_h;
    r.mip_count = v.layout.mip_count;
    r.flags = 1u;
    for (uint32_t m = 0; m < kVtMaxMips; ++m) r.mip_row[m] = v.layout.mip_row[m];
    variant_records_dirty_ = true;
}

// ---------------------------------------------------------------------------
// Fill queue
// ---------------------------------------------------------------------------

void VtResidency::queue_page(VariantRung& v, VtPageKey page, bool force,
                             uint32_t preassigned_slot) {
    if (!v.live || !v.indirection.in_range(page.mip, page.px, page.py)) return;
    if (!force && v.indirection.is_mapped(page.mip, page.px, page.py)) {
        // Already resident; just keep its slot warm.
        const VtEntry entry = v.indirection.resolve(page.mip, page.px, page.py);
        slots_.touch(entry.slot, frame_index_);
        return;
    }
    const uint64_t k = page_key(v.layer, page);
    const auto found = queued_keys_.find(k);
    // Priority: how many mips coarser the currently-served page is. A page
    // whose only coverage is the tail is the most starved, so it wins.
    const VtEntry served = v.indirection.resolve(page.mip, page.px, page.py);
    const uint32_t priority = served.mapped_mip > page.mip
                                  ? served.mapped_mip - page.mip
                                  : 0u;
    if (found != queued_keys_.end()) {
        PendingFill& existing = queue_[found->second];
        if (priority > existing.priority) existing.priority = priority;
        existing.requested_frame = frame_index_;
        if (preassigned_slot != 0xFFFFFFFFu)
            existing.preassigned_slot = preassigned_slot;
        return;
    }
    queued_keys_[k] = queue_.size();
    queue_.push_back(
        PendingFill{v.layer, page, priority, frame_index_, preassigned_slot});
}

void VtResidency::inject_feedback_for_test(const VtFeedbackRequest* requests,
                                           size_t count) {
    injected_.assign(requests, requests + count);
}

void VtResidency::drain_feedback(uint32_t frame_slot) {
    std::vector<VtFeedbackRequest> requests;
    requests.swap(injected_);
    if (frame_slot < kFeedbackSlots && feedback_slot_written_[frame_slot] &&
        feedback_readback_[frame_slot].mapped != nullptr && feedback_w_ != 0) {
        const uint16_t* texels =
            static_cast<const uint16_t*>(feedback_readback_[frame_slot].mapped);
        const size_t count = static_cast<size_t>(feedback_w_) * feedback_h_;
        requests.reserve(requests.size() + count / 8);
        for (size_t i = 0; i < count; ++i) {
            const uint16_t* t = texels + i * 4;
            if (t[0] == 0) continue;
            requests.push_back(VtFeedbackRequest{static_cast<uint32_t>(t[0] - 1u),
                                                 t[3], t[1], t[2]});
        }
        feedback_slot_written_[frame_slot] = false;
    }
    stats_.requests_last_frame = static_cast<uint32_t>(requests.size());
    for (const VtFeedbackRequest& r : requests) {
        if (r.layer >= variants_.size()) continue;
        VariantRung& v = variants_[r.layer];
        if (!v.live) continue;
        queue_page(v, VtPageKey{r.mip, r.px, r.py});
    }
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------

void VtResidency::begin_frame(uint64_t frame_index, uint32_t frame_slot) {
    if (!ready_) return;
    frame_index_ = frame_index;
    frame_slot_ = frame_slot % kFeedbackSlots;
    drain_feedback(frame_slot_);
}

bool VtResidency::ensure_feedback(uint32_t raster_width, uint32_t raster_height,
                                  std::string& error) {
    if (!ready_) return true;
    const uint32_t w = raster_width / 8u ? raster_width / 8u : 1u;
    const uint32_t h = raster_height / 8u ? raster_height / 8u : 1u;
    if (w == feedback_w_ && h == feedback_h_ && feedback_.image != VK_NULL_HANDLE)
        return true;
    destroy_pool_image(feedback_);
    for (uint32_t i = 0; i < kFeedbackSlots; ++i) {
        destroy_buffer(feedback_readback_[i]);
        feedback_slot_written_[i] = false;
    }
    feedback_w_ = feedback_h_ = 0;
    if (!create_array_image(*vulkan_, VK_FORMAT_R16G16B16A16_UINT, w, h, 1,
                            VK_IMAGE_USAGE_STORAGE_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            feedback_.image, feedback_.view, feedback_.memory,
                            error, VK_IMAGE_VIEW_TYPE_2D)) {
        return false;
    }
    feedback_.format = VK_FORMAT_R16G16B16A16_UINT;
    feedback_.layers = 1;
    feedback_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 8u;
    for (uint32_t i = 0; i < kFeedbackSlots; ++i) {
        if (!create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           feedback_readback_[i], error)) {
            return false;
        }
    }
    feedback_w_ = w;
    feedback_h_ = h;
    return true;
}

void VtResidency::record_feedback_clear(VkCommandBuffer cmd) {
    if (!ready_ || feedback_.image == VK_NULL_HANDLE) return;
    barrier(cmd, feedback_.image, 1, feedback_.layout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, feedback_.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    barrier(cmd, feedback_.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    feedback_.layout = VK_IMAGE_LAYOUT_GENERAL;
}

void VtResidency::record_feedback_readback(VkCommandBuffer cmd) {
    if (!ready_ || feedback_.image == VK_NULL_HANDLE) return;
    if (frame_slot_ >= kFeedbackSlots) return;
    if (feedback_readback_[frame_slot_].buffer == VK_NULL_HANDLE) return;
    barrier(cmd, feedback_.image, 1, feedback_.layout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {feedback_w_, feedback_h_, 1};
    vkCmdCopyImageToBuffer(cmd, feedback_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           feedback_readback_[frame_slot_].buffer, 1, &copy);
    feedback_.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    feedback_slot_written_[frame_slot_] = true;
}

bool VtResidency::record_frame(VkCommandBuffer cmd, std::string& error) {
    if (!ready_) return true;
    stats_.fills_last_frame = 0;
    stats_.pool_used = slots_.used();
    stats_.pool_pinned = slots_.pinned();
    stats_.evictions_total = slots_.evictions();
    stats_.queue_depth = static_cast<uint32_t>(queue_.size());

    // --- pool transitions -------------------------------------------------
    for (uint32_t c = 0; c < kVtChannelCount; ++c) {
        if (pool_[c].layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier(cmd, pool_[c].image, pool_[c].layers, pool_[c].layout,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
            pool_[c].layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }
    }
    if (indirection_.layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier(cmd, indirection_.image, indirection_.layers,
                indirection_.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        indirection_.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }

    // --- drain the fill queue --------------------------------------------
    batch_.clear();
    if (!queue_.empty() && filler_) {
        // Highest priority first; ties by insertion order (stable).
        std::stable_sort(queue_.begin(), queue_.end(),
                         [](const PendingFill& a, const PendingFill& b) {
                             return a.priority > b.priority;
                         });
        const size_t take = std::min<size_t>(queue_.size(), max_fills_per_frame_);
        size_t consumed = 0;
        for (size_t i = 0; i < take; ++i) {
            const PendingFill& p = queue_[i];
            VariantRung& v = variants_[p.layer];
            if (!v.live) {
                consumed = i + 1;
                continue;
            }
            uint32_t slot = p.preassigned_slot;
            if (slot == 0xFFFFFFFFu) {
                VtSlotPool::Owner evicted;
                if (!slots_.acquire(variant_key(v.variant_hash, v.rung), p.page,
                                    /*pinned=*/false, frame_index_, slot,
                                    evicted)) {
                    // Pool exhausted by pinned tails; retry next frame.
                    break;
                }
                if (evicted.live) {
                    const auto owner_layer = layer_of_.find(evicted.variant_key);
                    if (owner_layer != layer_of_.end())
                        variants_[owner_layer->second].indirection.unmap(
                            evicted.page.mip, evicted.page.px, evicted.page.py);
                }
            } else {
                slots_.touch(slot, frame_index_);
            }
            consumed = i + 1;
            VtFillRequest request;
            request.variant_hash = v.variant_hash;
            request.rung = static_cast<uint16_t>(v.rung);
            request.mip = static_cast<uint16_t>(p.page.mip);
            request.page_x = static_cast<uint16_t>(p.page.px);
            request.page_y = static_cast<uint16_t>(p.page.py);
            request.physical_slot = slot;
            request.atlas = &v.atlas;
            request.part_context = &v.context;
            request.pool = &pool_binding_;
            batch_.push_back(request);
            v.indirection.map(p.page.mip, p.page.px, p.page.py, slot);
            if (p.page.mip + 1u == v.layout.mip_count) v.tail_filled = true;
        }
        // Only the entries we actually dispatched (or dropped as dead) leave
        // the queue; a pool-exhaustion break keeps the rest for next frame.
        queue_.erase(queue_.begin(),
                     queue_.begin() + static_cast<long>(consumed));
        queued_keys_.clear();
        for (size_t i = 0; i < queue_.size(); ++i)
            queued_keys_[page_key(queue_[i].layer, queue_[i].page)] = i;
    }

    if (!batch_.empty()) {
        // Must agree with the transitions recorded above, which is why the two
        // are set together and not per-filler: BOTH shipped fillers write the
        // pool exclusively through transfer copies (the stub stages from host
        // memory, the compositor copies its encoded blocks out of transient
        // buffers/images), and the compositor honours this flag when choosing
        // the copy's destination layout. The pool images carry no STORAGE
        // usage and VtPoolBinding::storage_view is never populated, so a
        // filler that needs GENERAL cannot exist without changing
        // create_pool_image() -- and would then have to change this pair too.
        pool_binding_.transfer_dst_layout = true;
        filler_->fill(cmd, batch_.data(), batch_.size());
        stats_.fills_last_frame = static_cast<uint32_t>(batch_.size());
        stats_.fills_total += batch_.size();
    }

    // --- indirection uploads ---------------------------------------------
    const VkDeviceSize layer_bytes =
        static_cast<VkDeviceSize>(kVtIndirectionWidth) * kVtIndirectionHeight * 4u;
    VkDeviceSize staging_offset = 0;
    for (VariantRung& v : variants_) {
        if (!v.live || !v.indirection.dirty()) continue;
        if (staging_offset + layer_bytes > indirection_staging_.size) break;
        const std::vector<uint32_t>& texels = v.indirection.texels();
        std::memcpy(static_cast<uint8_t*>(indirection_staging_.mapped) +
                        staging_offset,
                    texels.data(), layer_bytes);
        VkBufferImageCopy copy{};
        copy.bufferOffset = staging_offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, v.layer, 1};
        copy.imageExtent = {kVtIndirectionWidth, kVtIndirectionHeight, 1};
        vkCmdCopyBufferToImage(cmd, indirection_staging_.buffer,
                               indirection_.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        v.indirection.clear_dirty();
        staging_offset += layer_bytes;
    }

    // --- back to shader-read ---------------------------------------------
    for (uint32_t c = 0; c < kVtChannelCount; ++c) {
        barrier(cmd, pool_[c].image, pool_[c].layers, pool_[c].layout,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        pool_[c].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    barrier(cmd, indirection_.image, indirection_.layers, indirection_.layout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    indirection_.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (variant_records_dirty_ && variant_buffer_.mapped) {
        std::memcpy(variant_buffer_.mapped, variant_records_.data(),
                    variant_buffer_.size);
        variant_records_dirty_ = false;
    }

    stats_.pool_used = slots_.used();
    stats_.pool_pinned = slots_.pinned();
    stats_.evictions_total = slots_.evictions();
    stats_.queue_depth = static_cast<uint32_t>(queue_.size());
    (void)error;
    return true;
}

}  // namespace vt
