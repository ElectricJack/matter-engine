#include "vt_residency.h"

#include <algorithm>
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
    // The fallback obeys [lo, hi] too: callers pass device-derived caps as
    // `hi` (e.g. max_variants_' indirection layer limit), and an unset env
    // var must not smuggle a default past them. NVIDIA driver 610.74 dropped
    // maxImageArrayLayers for the indirection's R16G16_UINT to 2048, which
    // turned the unclamped 8192 default into a validation failure at
    // vkCreateImage time.
    if (fallback < lo) fallback = lo;
    if (fallback > hi) fallback = hi;
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
    // One indirection ARRAY LAYER per (variant, rung), so this is hard-capped
    // by what the device can actually CREATE for the indirection image, no
    // matter what the env asks for.
    //
    // That cap is NOT properties.limits.maxImageArrayLayers alone: the
    // per-format Image Creation Limits (VUID-VkImageCreateInfo-arrayLayers-
    // 02256) can be tighter -- current NVIDIA drivers report 2048 for
    // R16G16_UINT while the general limit is 8192, and creating the 8192-layer
    // indirection tripped validation. Query the format-specific limit for the
    // exact image we create below and clamp to the smaller of the two.
    //
    // 8192, not 1024, as the default ask: a streamed world registers one
    // variant per resident sector rung, and StreamMountain's 2560 m ring is
    // ~5000 sectors. At 1024 most of the world was refused a layer and fell
    // back to the legacy path, which renders but IGNORES the authored
    // surfaces() classification -- the uniform tan far field with a visible
    // boundary. The cost is 32 KiB of indirection image per layer; drop
    // MATTER_VT_MAX_VARIANTS on a memory-tight device.
    VkImageFormatProperties indirection_limits{};
    uint32_t max_indirection_layers = max_layers;
    if (vkGetPhysicalDeviceImageFormatProperties(
            vulkan.physical_device(), VK_FORMAT_R16G16_UINT,
            VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0,
            &indirection_limits) == VK_SUCCESS &&
        indirection_limits.maxArrayLayers > 0) {
        max_indirection_layers =
            std::min(max_indirection_layers,
                     indirection_limits.maxArrayLayers);
    }
    const uint32_t requested_variants =
        env_u32("MATTER_VT_MAX_VARIANTS", 8192u, 4u, 0xFFFFFFFFu);
    max_variants_ = env_u32("MATTER_VT_MAX_VARIANTS", 8192u, 4u,
                            max_indirection_layers < 8192u
                                ? max_indirection_layers
                                : 8192u);
    if (requested_variants > max_variants_) {
        // Say BOTH numbers, loudly and once. Without this the census reads
        // "variants=2048/2048, rejected=N" and the obvious move -- raise
        // MATTER_VT_MAX_VARIANTS -- silently does nothing, because the wall is
        // the device's per-format array-layer limit, not the setting. Getting
        // past it needs the indirection restructure (several variants per
        // layer, or a buffer instead of an array image), not a bigger number.
        std::fprintf(stderr,
                     "[vt] variant layers CLAMPED to %u: %u were requested "
                     "(MATTER_VT_MAX_VARIANTS) but this device allows at most "
                     "%u array layers for the R16G16_UINT indirection image "
                     "(maxImageArrayLayers=%u, per-format maxArrayLayers=%u). "
                     "One layer is one (variant, rung). Registration is "
                     "demand-driven, so this caps the on-screen WORKING SET, "
                     "not the resident ring; only frames that WANT more than "
                     "%u (variant, rung)s at once fall back (tape-classified "
                     "legacy) for the excess.\n",
                     max_variants_, requested_variants, max_indirection_layers,
                     max_layers, indirection_limits.maxArrayLayers,
                     max_variants_);
        std::fflush(stderr);
    }
    max_fills_per_frame_ =
        env_u32("MATTER_VT_FILLS_PER_FRAME", 8u, 1u, kMaxFillFlags);
    // WP-H: tier-2 budget is deliberately SEPARATE from the fill budget --
    // enrichment is background refinement of already-correct pages, so it must
    // never compete with getting a page resident in the first place. 0 disables
    // the tier without unloading the enricher. The upper bound is the
    // enricher's own per-batch capacity (VtEnricher::kMaxRequestsPerBatch).
    max_enrich_per_frame_ = env_u32("MATTER_VT_ENRICH_PER_FRAME", 2u, 0u, 16u);
    // CPU mesh copies (see register_variant's LIFETIME note). Sized for a
    // streamed world: a rung-0 StreamMountain terrain sector costs on the order
    // of 100 KB of copied streams, so a full 2048-layer census lands in the low
    // hundreds of MB. 1024 MB leaves headroom for denser rungs (a rung-2 sector
    // has ~16x the vertices of rung 0) without letting a runaway world grow the
    // heap without bound.
    mesh_budget_bytes_ =
        static_cast<size_t>(env_u32("MATTER_VT_MESH_BUDGET_MB", 1024u, 1u,
                                    16384u)) *
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
    slot_tier_.assign(pool_pages_, 0u);
    enrich_queue_.clear();
    enrich_queued_slot_.clear();
    variants_.assign(max_variants_, VariantRung{});
    free_layers_.clear();
    free_layers_.reserve(max_variants_);
    for (uint32_t i = max_variants_; i-- > 0;) free_layers_.push_back(i);

    for (uint32_t c = 0; c < kVtChannelCount; ++c) {
        pool_binding_.image[c] = pool_[c].image;
        pool_binding_.format[c] = pool_[c].format;
        // WP-H: the enricher must READ resident page content back out of the
        // pool. The images are BC-compressed and carry no STORAGE usage, so the
        // only way in is a sampled fetch through these views.
        pool_binding_.sampled_view[c] = pool_[c].view;
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
    stats_.max_variants = max_variants_;
    stats_.mesh_budget_bytes = mesh_budget_bytes_;
    stats_.pool_bytes = layer_texels * pool_layers * (1 + 1 + 1 + 4);
    stats_.enrich_samples = enricher_ ? enricher_->sample_count() : 0u;
    ready_ = true;
    return true;
}

void VtResidency::shutdown() {
    if (!vulkan_) {
        ready_ = false;
        return;
    }
    filler_.reset();
    enricher_.reset();
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
    enrich_queue_.clear();
    enrich_queued_slot_.clear();
    enrich_batch_.clear();
    slot_tier_.clear();
    variant_records_.clear();
    mesh_bytes_used_ = 0;
    warned_rejection_ = false;
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

void VtResidency::set_enricher(std::unique_ptr<VtPageEnricher> enricher) {
    enricher_ = std::move(enricher);
    stats_.enrich_samples = enricher_ ? enricher_->sample_count() : 0u;
    if (!enricher_) {
        // Tier 2 just went away: forget every candidate and every tier bit so
        // the stats never claim pages are enriched when nothing enriches them.
        enrich_queue_.clear();
        enrich_queued_slot_.clear();
        std::fill(slot_tier_.begin(), slot_tier_.end(), uint8_t{0});
        stats_.enrich_queue_depth = 0;
        stats_.enriched_pages = 0;
    }
}

// ---------------------------------------------------------------------------
// Variant registration
// ---------------------------------------------------------------------------

void VtResidency::note_rejection(const char* reason, size_t wanted_bytes) {
    ++stats_.rejected_variants;
    if (warned_rejection_) return;
    warned_rejection_ = true;
    // WARN ONCE, loudly. A rejected variant does not fail -- it falls back to
    // the legacy per-material path, which renders but ignores the authored
    // surfaces() classification. In a streamed world that shows up as a uniform
    // far field with a visible boundary, and nothing else in the pipeline
    // reports it. Raise the knobs named here rather than guessing from pixels.
    const double used_mb =
        static_cast<double>(mesh_bytes_used_) / (1024.0 * 1024.0);
    const double budget_mb =
        static_cast<double>(mesh_budget_bytes_) / (1024.0 * 1024.0);
    const double wanted_kb = static_cast<double>(wanted_bytes) / 1024.0;
    std::fprintf(stderr,
                 "[vt] WARNING: variant registration REJECTED (%s) -- this "
                 "part falls back to the legacy path and ignores its "
                 "surfaces() classification. variants=%u/%u, mesh=%.1f/%.1f "
                 "MiB, wanted=%.1f KiB. Raise MATTER_VT_MAX_VARIANTS "
                 "(indirection layers -- already clamped to the most this "
                 "device can create, so variants==max means the CAP is the "
                 "wall, not the setting) and/or "
                 "MATTER_VT_MESH_BUDGET_MB. Further rejections are counted in "
                 "the VT census (vt_rejected_variants) but not logged.\n",
                 reason, stats_.variants, max_variants_, used_mb, budget_mb,
                 wanted_kb);
    std::fflush(stderr);
}

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
        note_rejection("no free indirection layer", 0);
        return kVtNoSlot;
    }

    // WP-F: a usable tape classification needs both arrays and an exact
    // per-vertex weight matrix; anything else fails closed to the TriEx path.
    const bool has_surface_tape = vt_context_has_surface_tape(context);

    // Budget the CPU mesh copy BEFORE taking any slot, so a rejection leaves
    // no partial registration behind.
    const size_t mesh_bytes = vt_variant_mesh_bytes(atlas, context);
    if (mesh_bytes_used_ + mesh_bytes > mesh_budget_bytes_) {
        note_rejection("CPU mesh budget spent", mesh_bytes);
        return kVtNoSlot;
    }

    // The tail page is pinned for the variant's whole life: it is what makes
    // "every loaded variant always has valid texels" true.
    uint32_t tail_slot = 0;
    VtSlotPool::Owner evicted;
    const VtPageKey tail_page{layout.mip_count - 1u, 0u, 0u};
    if (!slots_.acquire(key, tail_page, /*pinned=*/true, frame_index_,
                        tail_slot, evicted)) {
        // Every page slot is a pinned tail already: the pool cannot hold one
        // more variant. Same fail-closed-to-legacy outcome as the two budget
        // gates above, so it is counted (and warned about) the same way.
        note_rejection("page pool exhausted by pinned tails "
                       "(raise MATTER_VT_POOL_PAGES)", 0);
        return kVtNoSlot;
    }
    if (evicted.live) {
        // The pool was full of unpinned pages; unmap whatever we recycled.
        const auto owner_layer = layer_of_.find(evicted.variant_key);
        if (owner_layer != layer_of_.end())
            variants_[owner_layer->second].indirection.unmap(
                evicted.page.mip, evicted.page.px, evicted.page.py);
    }
    // The slot's previous content (and any tier-2 candidacy for it) is gone.
    slot_reset_tier(tail_slot);

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
    // WP-F: adopt the surfaces()-tape classification (fail-closed to the
    // TriEx materialId path when absent or malformed — has_surface_tape).
    if (has_surface_tape) {
        v.surface_weights.assign(
            context.surface_weights,
            context.surface_weights +
                static_cast<size_t>(vertices) * context.surface_material_count);
        v.surface_materials.assign(
            context.surface_materials,
            context.surface_materials + context.surface_material_count);
        v.context.surface_weights = v.surface_weights.data();
        v.context.surface_materials = v.surface_materials.data();
        v.context.surface_material_count = context.surface_material_count;
        v.context.surface_tape_hash = context.surface_tape_hash;
    } else {
        v.context.surface_weights = nullptr;
        v.context.surface_materials = nullptr;
        v.context.surface_material_count = 0;
        v.context.surface_tape_hash = 0;
    }
    // WP-H: the rung's finest chart density, for the coarse-page enrichment
    // skip in queue_enrich.
    v.finest_texels_per_meter = 0.0f;
    for (const chart_atlas::ChartEntry& chart : v.atlas.charts) {
        if (chart.texels_per_meter > v.finest_texels_per_meter)
            v.finest_texels_per_meter = chart.texels_per_meter;
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

bool VtResidency::release_variant_key(uint64_t key) {
    const auto found = layer_of_.find(key);
    if (found == layer_of_.end()) return false;
    const uint32_t layer = found->second;
    VariantRung& v = variants_[layer];
    // Release every slot this variant owns.
    for (uint32_t slot = 0; slot < slots_.capacity(); ++slot) {
        const VtSlotPool::Owner& o = slots_.owner(slot);
        if (o.live && o.variant_key == key) {
            slot_reset_tier(slot);
            slots_.release(slot);
        }
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
    return true;
}

void VtResidency::release_variant(uint64_t variant_hash) {
    if (!ready_) return;
    for (uint32_t rung = 0; rung < 32u; ++rung)
        release_variant_key(variant_key(variant_hash, rung));
    stats_.pool_used = slots_.used();
    stats_.pool_pinned = slots_.pinned();
    stats_.queue_depth = static_cast<uint32_t>(queue_.size());
}

void VtResidency::release_variant(uint64_t variant_hash, uint32_t rung) {
    if (!ready_) return;
    if (!release_variant_key(variant_key(variant_hash, rung))) return;
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

    // WP-H: enrichment state is page CONTENT, so it dies with the content.
    // Every tier bit is cleared (the pinned tails included -- they are about to
    // be re-filled in place) and every pending candidate is dropped; the
    // re-fills below queue fresh candidates, so the whole pool re-enriches from
    // the new inputs rather than keeping occlusion baked against the old ones.
    stats_.enrich_dropped_total += enrich_queue_.size();
    enrich_queue_.clear();
    enrich_queued_slot_.clear();
    std::fill(slot_tier_.begin(), slot_tier_.end(), uint8_t{0});
    stats_.enriched_pages = 0;
    stats_.enrich_queue_depth = 0;

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

bool VtResidency::update_variant_surface(uint64_t variant_hash, uint32_t rung,
                                         const uint8_t* weights,
                                         size_t weight_bytes,
                                         const uint32_t* materials,
                                         uint32_t material_count,
                                         uint64_t tape_hash) {
    if (!ready_) return false;
    const auto found = layer_of_.find(variant_key(variant_hash, rung));
    if (found == layer_of_.end()) return false;
    VariantRung& v = variants_[found->second];
    if (!v.live) return false;

    const size_t old_bytes =
        v.surface_weights.size() + v.surface_materials.size() * sizeof(uint32_t);

    const bool strip = material_count == 0 || weights == nullptr ||
                       materials == nullptr || material_count > 8u;
    const size_t expected =
        static_cast<size_t>(v.context.vertex_count) * material_count;
    if (!strip && weight_bytes != expected) return false;

    if (strip) {
        v.surface_weights.clear();
        v.surface_materials.clear();
        v.context.surface_weights = nullptr;
        v.context.surface_materials = nullptr;
        v.context.surface_material_count = 0;
        v.context.surface_tape_hash = 0;
    } else {
        v.surface_weights.assign(weights, weights + weight_bytes);
        v.surface_materials.assign(materials, materials + material_count);
        v.context.surface_weights = v.surface_weights.data();
        v.context.surface_materials = v.surface_materials.data();
        v.context.surface_material_count = material_count;
        v.context.surface_tape_hash = tape_hash;
    }

    const size_t new_bytes =
        v.surface_weights.size() + v.surface_materials.size() * sizeof(uint32_t);
    v.mesh_bytes = v.mesh_bytes - std::min(v.mesh_bytes, old_bytes) + new_bytes;
    mesh_bytes_used_ =
        mesh_bytes_used_ - std::min(mesh_bytes_used_, old_bytes) + new_bytes;
    stats_.mesh_bytes = mesh_bytes_used_;
    return true;
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

// ---------------------------------------------------------------------------
// WP-H tier-2 enrichment queue
// ---------------------------------------------------------------------------

void VtResidency::slot_reset_tier(uint32_t slot) {
    if (slot < slot_tier_.size()) {
        if (slot_tier_[slot] != 0 && stats_.enriched_pages != 0)
            --stats_.enriched_pages;
        slot_tier_[slot] = 0;
    }
    const auto found = enrich_queued_slot_.find(slot);
    if (found == enrich_queued_slot_.end()) return;
    const size_t index = found->second;
    enrich_queued_slot_.erase(found);
    if (index < enrich_queue_.size()) {
        enrich_queue_.erase(enrich_queue_.begin() + static_cast<long>(index));
        for (auto& entry : enrich_queued_slot_)
            if (entry.second > index) --entry.second;
    }
    ++stats_.enrich_dropped_total;
    stats_.enrich_queue_depth = static_cast<uint32_t>(enrich_queue_.size());
}

void VtResidency::queue_enrich(uint32_t layer, VtPageKey page, uint32_t slot) {
    if (!enricher_ || max_enrich_per_frame_ == 0) return;
    if (slot >= slot_tier_.size()) return;
    // COARSE-PAGE SKIP. Tier 2 bakes a contact-scale term (sub-metre); once a
    // page texel is wider than the enricher's fade end, the enrichment would be
    // multiplied by zero, so tracing it is pure cost. This is also the guard
    // that stops coarse mips of a streamed terrain sector from being enriched
    // at all — the case where the old texel-relative-only cap grew to tens of
    // metres and blackened open slopes.
    if (layer < variants_.size()) {
        const float tpm = variants_[layer].finest_texels_per_meter;
        if (tpm > 0.0f) {
            const float footprint_m =
                static_cast<float>(1u << page.mip) / tpm;
            if (footprint_m >= enricher_->max_footprint_meters()) {
                ++stats_.enrich_skipped_coarse_total;
                return;
            }
        }
    }
    const auto found = enrich_queued_slot_.find(slot);
    if (found != enrich_queued_slot_.end()) {
        enrich_queue_[found->second] =
            PendingEnrich{layer, page, slot, frame_index_};
        return;
    }
    // Bounded: a thrashing pool must not grow this without limit. The oldest
    // candidate is the least likely to still be on screen, so it loses.
    constexpr size_t kMaxEnrichQueue = 1024;
    if (enrich_queue_.size() >= kMaxEnrichQueue)
        slot_reset_tier(enrich_queue_.front().slot);
    enrich_queued_slot_[slot] = enrich_queue_.size();
    enrich_queue_.push_back(PendingEnrich{layer, page, slot, frame_index_});
    stats_.enrich_queue_depth = static_cast<uint32_t>(enrich_queue_.size());
}

void VtResidency::drain_enrich(VkCommandBuffer cmd) {
    stats_.enrich_last_frame = 0;
    if (!enricher_ || max_enrich_per_frame_ == 0 || enrich_queue_.empty())
        return;
    enrich_batch_.clear();
    size_t consumed = 0;
    for (size_t i = 0; i < enrich_queue_.size() &&
                       enrich_batch_.size() < max_enrich_per_frame_;
         ++i) {
        const PendingEnrich p = enrich_queue_[i];
        consumed = i + 1;
        if (p.layer >= variants_.size()) continue;
        VariantRung& v = variants_[p.layer];
        if (!v.live || p.slot >= slots_.capacity()) continue;
        const VtSlotPool::Owner& owner = slots_.owner(p.slot);
        // The slot must still hold exactly the page we queued. An eviction or a
        // re-fill in between makes the candidate stale: the re-fill queued its
        // own candidate, so dropping this one loses nothing.
        if (!owner.live ||
            owner.variant_key != variant_key(v.variant_hash, v.rung) ||
            !(owner.page == p.page)) {
            ++stats_.enrich_dropped_total;
            continue;
        }
        if (slot_tier_[p.slot] != 0) continue;   // already tier-2
        VtEnrichRequest request;
        request.variant_hash = v.variant_hash;
        request.rung = static_cast<uint16_t>(v.rung);
        request.mip = static_cast<uint16_t>(p.page.mip);
        request.page_x = static_cast<uint16_t>(p.page.px);
        request.page_y = static_cast<uint16_t>(p.page.py);
        request.physical_slot = p.slot;
        request.atlas = &v.atlas;
        request.part_context = &v.context;
        request.pool = &pool_binding_;
        request.frame_index = frame_index_;
        enrich_batch_.push_back(request);
        // Marked tier-2 at RECORD time, not on completion: the enrichment
        // multiplies into the page in place, so a second pass over the same
        // fill would darken it twice. A request the enricher then fails closed
        // on (no acceleration structure, no sampled pool view) simply stays
        // tier-1 content flagged as done -- which is the "skipped silently"
        // contract, since tier-1 pages are already correct.
        slot_tier_[p.slot] = 1;
        ++stats_.enriched_pages;
    }
    enrich_queue_.erase(enrich_queue_.begin(),
                        enrich_queue_.begin() + static_cast<long>(consumed));
    enrich_queued_slot_.clear();
    for (size_t i = 0; i < enrich_queue_.size(); ++i)
        enrich_queued_slot_[enrich_queue_[i].slot] = i;
    stats_.enrich_queue_depth = static_cast<uint32_t>(enrich_queue_.size());
    if (enrich_batch_.empty()) return;

    // The enricher SAMPLES the ORM pool image (vt_enrich.h contract) and
    // restores this layout itself after its write-back, so the tracked layout
    // is unchanged on the far side.
    PoolImage& orm = pool_[kVtChannelOrm];
    if (orm.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier(cmd, orm.image, orm.layers, orm.layout,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        orm.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    enricher_->enrich(cmd, enrich_batch_.data(), enrich_batch_.size());
    stats_.enrich_last_frame = static_cast<uint32_t>(enrich_batch_.size());
    stats_.enrich_total += enrich_batch_.size();
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

    // --- WP-H: tier-2 enrichment, BEFORE this frame's fills ---------------
    // Ordering matters twice over. (1) It runs while the pool is still in its
    // shader-read layout, which is what the enricher samples the page's current
    // ORM texels from. (2) Draining before the fills guarantees a page queued
    // by THIS frame's fills is never enriched in the same command buffer that
    // wrote it -- the earliest it can run is the next frame, by which point the
    // fill's transfer has been submitted and the layout transition below is the
    // dependency that orders them.
    drain_enrich(cmd);

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
    // The indirection is NOT mapped here any more. Mapping a page before the
    // filler has written it is what turned every skipped request into a page
    // pointing at never-written pool memory (BC7 -> black); the mapping now
    // happens after fill() returns, gated on the per-request success flag
    // (vt_types.h VtFillRequest::out_filled). `pending_map_` remembers what to
    // map or roll back.
    batch_.clear();
    pending_map_.clear();
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
            pending_map_.push_back(PendingMap{
                p.layer, p.page, slot, p.preassigned_slot != 0xFFFFFFFFu});
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
        // One flag per request, false until the filler says otherwise. The
        // vector is sized before any pointer into it is handed out, so the
        // addresses stay valid for the whole fill() call.
        for (size_t i = 0; i < batch_.size(); ++i) {
            fill_flags_[i] = false;
            batch_[i].out_filled = &fill_flags_[i];
        }
        filler_->fill(cmd, batch_.data(), batch_.size());

        // --- map or roll back, per request --------------------------------
        uint32_t mapped = 0;
        for (size_t i = 0; i < pending_map_.size(); ++i) {
            const PendingMap& m = pending_map_[i];
            VariantRung& v = variants_[m.layer];
            if (fill_flags_[i]) {
                if (!v.live) continue;
                v.indirection.map(m.page.mip, m.page.px, m.page.py, m.slot);
                if (m.page.mip + 1u == v.layout.mip_count) v.tail_filled = true;
                // WP-H: the slot now holds FRESH tier-1 content, so any tier-2
                // state it carried is void and the new content becomes an
                // enrichment candidate. Tails go through here too (they are
                // small, permanent, and what most of a variant reads).
                slot_reset_tier(m.slot);
                queue_enrich(m.layer, m.page, m.slot);
                ++mapped;
                continue;
            }
            // The filler skipped this request. Nothing wrote the slot, so the
            // page must NOT become resident.
            ++stats_.fills_failed_total;
            if (!m.preassigned) {
                // Freshly acquired: hand it straight back. The entry was never
                // mapped, so every sample of this page keeps resolving to the
                // variant's tail, exactly as before the request.
                slot_reset_tier(m.slot);
                slots_.release(m.slot);
            } else if (v.live) {
                // A pinned tail keeps its slot (every unmapped entry resolves
                // to it, so releasing it would break that invariant) but its
                // content is still undefined: leave tail_filled false and
                // re-queue the in-place re-fill so the next frame retries.
                v.tail_filled = false;
                queue_page(v, m.page, /*force=*/true, /*preassigned_slot=*/m.slot);
            }
        }
        stats_.fills_last_frame = mapped;
        stats_.fills_total += mapped;
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
