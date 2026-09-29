// MatterEngine3/src/render/vt_residency.cpp
//
// Implementation of VtResidency, the GPU-facing half of chart-space virtual
// texturing. vt_residency.h carries the CONTRACT — the indirection packing that
// shaders_vk/vt_common.glsl must agree with, the GPU-timeline recycling rules,
// and every public method's pre/postconditions. Read it first; this file is the
// machinery behind it.
//
// WHAT IS HERE, in file order:
//   - anonymous helpers: the two key folds and the two pipeline-barrier wrappers
//   - resource creation and teardown (create_buffer, create_array_image,
//     create_pool_image, init, shutdown)
//   - variant registration and release (register_variant, release_rung_alias,
//     release_variant_key, release_variant, update_variant_surface,
//     write_variant_record, invalidate_all_content)
//   - the fill queue (queue_page) and the WP-H tier-2 enrichment queue
//     (queue_enrich, drain_enrich, slot_reset_tier)
//   - feedback consumption (drain_feedback) and the per-frame recording
//     (refresh_budgets, begin_frame, ensure_feedback, record_feedback_clear,
//     record_frame, record_feedback_readback)
//
// THREADING. Every method here belongs to the thread that owns the Vulkan frame
// — VkSceneRenderer's render thread. Nothing is internally synchronised: the
// fill and enrich queues, the slot pool, the table allocator and the CPU mesh
// copies are all plain single-threaded state.
//
// PER-FRAME ORDER, as VkSceneRenderer's vt_begin_frame / vt_record_pre_pass /
// vt_record_post_pass hooks establish it:
//   begin_frame(serial, slot)     CPU only: refresh budgets, collect the
//                                 graveyards, consume this slot's readback
//   ensure_feedback(w, h)         resize the visible-feedback attachment and compact readback
//   record_feedback_clear(cmd)  \ both BEFORE vkCmdBeginRendering — they are
//   record_frame(cmd)           / transfers plus the filler's own passes
//   ... the G-buffer pass writes feedback as storage ...
//   record_feedback_readback(cmd) AFTER vkCmdEndRendering
//
// M6 — WHAT A "VARIANT" IS KEYED BY, and the single easiest thing to get wrong
// in this file. A LAYER is keyed by the PARAMETERISATION:
// variant_key(hash, chart_atlas::parameterisation_id(atlas)). The caller-facing
// (hash, rung) pair is only an ALIAS: param_key_of_rung_ maps the alias onto the
// layer key, layer_of_ maps the layer key onto the layer index, and
// VariantRung::alias_refs refcounts the rungs sharing it. Every rung of a part
// that shares a chart table therefore shares one page set — which is the point,
// since the pages then survive a rung switch. Any code that recomputes
// variant_key(hash, rung) and looks it up in layer_of_ directly is wrong: that
// key is not registered there.
//
// FAIL-CLOSED EVERYWHERE. Every gate in register_variant (unusable layout, no
// free layer, mesh budget, indirection arena, page pool) rejects the whole
// registration rather than half-completing it, and the rung then draws through
// the legacy per-material path. Rejections are counted (Stats::rejected_variants)
// and the first one logs loudly, because on screen a rejection is only a
// uniform far field with a boundary — nothing else in the pipeline reports it.
//
// SIZING NOTE carried from the header: the indirection is a BUFFER because the
// old one-array-layer-per-(variant, rung) image ran into NVIDIA's per-format
// maxArrayLayers of 2048 for R16G16_UINT, which capped the simultaneously
// registered working set. A buffer has no such wall; capacity is now just
// MATTER_VT_INDIRECTION_MB of 4-byte entries.
//
// TUNABLES all come from matter::VtResidencyBudgets (matter/vt_budgets.h) — no
// local getenv reads remain except MATTER_VT_DEBUG_GENERATIONS, which arms the
// abort-on-failure recycling audit rather than setting a value. The LIVE
// budgets are re-read every begin_frame (refresh_budgets), so an editor slider
// takes effect on the next frame; max_variants_ and the indirection arena size
// a buffer at init and are not live-editable.

#include "vt_residency.h"
#include "vt_periodic_material.h"
#include "vt_density.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

#include "matter/log.h"
#include "matter/gpu_timing_sample.h"
#include "matter/vt_budgets.h"
#include "matter/vulkan_device.h"
#include "profile.h"
#include "vk_resources.h"
#include "vk_pipeline.h"

namespace vt {
struct VtReceiverMaterialState {
    std::shared_ptr<const VtPartSnapshot> inputs;
    std::vector<VtReceiverMaterialChart> charts;
    std::vector<VtReceiverMaterialGpu> records;
    matter::VkBufferResource buffer;
};
using VtSurfacePairKey = std::array<uint64_t,5>;
static VtSurfacePairKey surface_pair_key(const VtSurfaceConnectionPair& pair) {
    return {pair.first,pair.first_rung,pair.second,pair.second_rung,pair.domain};
}
struct VtSurfacePairCache {
    std::array<std::shared_ptr<const VtSurfaceBoundarySource>,2> sources;
    std::array<std::vector<VtSurfaceLinkGpu>,2> links;
};
struct VtSurfaceLinkTable {
    std::vector<uint8_t> bytes;
    matter::VkBufferResource buffer;
};
struct VtSurfaceConnectionState {
    std::vector<std::shared_ptr<const VtSurfaceBoundarySource>> sources;
    std::map<VtSurfacePairKey,std::shared_ptr<const VtSurfacePairCache>> pairs;
    std::map<uint32_t,std::shared_ptr<const VtSurfaceLinkTable>> tables;
};
namespace {

// Fold a (part hash, discriminator) pair into one 64-bit map key by xoring in a
// golden-ratio multiple of discriminator + 1 (so discriminator 0 is not the
// identity).
//
// The parameter is named `rung` for history, but under M6 this is called with
// TWO different discriminators and the distinction matters:
//   variant_key(hash, rung)                      -> the caller-facing ALIAS key,
//                                                   the key of param_key_of_rung_
//   variant_key(hash, parameterisation_id(atlas)) -> the LAYER key, the key of
//                                                   layer_of_
// Looking an alias key up in layer_of_ finds nothing for any unified part.
uint64_t variant_key(uint64_t hash, uint32_t rung) {
    return hash ^ (0x9E3779B97F4A7C15ull * (rung + 1u));
}

// Pack (layer, mip, page y, page x) into the single key queued_keys_ uses to
// find an already-queued fill. 16 bits each for px and py, 8 for the mip, the
// layer above. Nothing ever decodes it — it only has to be injective.
uint64_t page_key(uint32_t layer, const VtPageKey& p) {
    return (static_cast<uint64_t>(layer) << 40) |
           (static_cast<uint64_t>(p.mip) << 32) |
           (static_cast<uint64_t>(p.py) << 16) | p.px;
}

// No local env reads remain: every tunable this file consumes now lives in
// matter::VtResidencyBudgets (matter/vt_budgets.h) and is read from that struct
// below. MATTER_VT_POOL_PAGES was the last holdout — its DEVICE-derived ceiling
// (maxImageArrayLayers) is genuinely inexpressible in a schema range, but that
// check is a separate init failure below, not the value's own bounds, which are
// the static [64, 16384] the schema now carries.
//
// MATTER_VT_DEBUG_GENERATIONS stays a raw getenv on purpose: it arms an
// abort-on-failure audit, not a setting.
uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

bool env_flag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

// The two vkCmdPipelineBarrier2 wrappers every recorded path in this file goes
// through. `barrier` covers mip 0 and ALL array layers, which is the whole
// image here: pool and feedback images are single-mip arrays by construction
// (VT pages are addressed through the indirection, not through hardware mips).
//
// The caller supplies the OLD layout because this file tracks each image's
// current layout itself (PoolImage::layout) rather than re-querying, and updates
// that field right next to the call. Keeping the two together is what stops the
// tracked layout and the recorded transition from drifting apart.
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

void buffer_barrier(VkCommandBuffer cmd, VkBuffer buffer,
                    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

// ---------------------------------------------------------------------------
// Generation audit failures (MATTER_VT_DEBUG_GENERATIONS=1).
// ---------------------------------------------------------------------------
// WHAT THE AUDIT CATCHES: any CPU-side hand-out of a table block, page slot or
// variant record before its retirement serial — the entire stale-mapping bug
// class, because every GPU-visible mutation is either (a) a queue-ordered copy
// (in-flight frames execute strictly before it, so they read the bytes they
// were submitted against) or (b) a host write to host-visible memory that is
// only reachable through the index whose reuse these asserts guard.
// WHAT IT CANNOT CATCH: a shader indexing the indirection with a vt_slot that
// never came from the residency layer (corrupted draw records), or cross-queue
// hazards (VT records/samples exclusively on the graphics queue today). Those
// would need a GPU-side generation compare, which is disproportionate while
// (a)/(b) above hold by construction.

namespace {
[[noreturn]] void generation_audit_fail(const char* domain, const char* what) {
    MATTER_LOGE("vt", "GENERATION AUDIT FAILED (%s): %s\n", domain,
                 what);
    std::fflush(stderr);
    std::abort();
}
}  // namespace

// RAII: the destructor runs shutdown(), which is idempotent and safe on an
// object that was never init()ed (it early-outs on the null device). It does
// NOT wait the device idle — see shutdown().
struct VtResidency::FeedbackGpu {
    matter::VkComputePipelineResource pipeline;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[kFeedbackSlots]{};
    ~FeedbackGpu() {
        if (pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(pipeline.device, pool, nullptr);
    }
};

VtResidency::VtResidency() = default;
VtResidency::~VtResidency() { shutdown(); }

// Ordinary draw rungs occupy 0..31. Independent modules use a private alias
// and a separate parameterisation key; releasing a part cannot release them.
static constexpr uint32_t kMaterialModuleRung = 32;
static constexpr uint32_t kMaterialModuleParameterisation = 0x4d4f4401u;
struct VtModuleOwner { VtResidency* runtime = nullptr; };
VtMaterialModule::~VtMaterialModule() {
    if (const auto owner=owner_.lock(); owner && owner->runtime)
        owner->runtime->release_material_module(*this);
}

// ---------------------------------------------------------------------------
// Resource creation
// ---------------------------------------------------------------------------

// One buffer with its own dedicated VkDeviceMemory — there is no suballocator
// in this layer, so keep the call count low rather than the sizes small. The
// allocation is registered with the engine's GPU memory census, and a
// HOST_VISIBLE buffer is mapped permanently (Buffer::mapped) for the life of the
// object; nothing here ever unmaps and remaps.
//
// `properties` is REQUIRED, `preferred` is nice-to-have and falls back to
// `properties` when zero — that split is what lets the feedback readback ask for
// HOST_CACHED while still accepting a device that cannot offer it.
//
// Destroys `out` first, so this doubles as a resize. A zero size leaves an empty
// but valid Buffer and returns true. Every failure path destroys what it built,
// so `out` is never left half-constructed.
bool VtResidency::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags properties, Buffer& out,
                                std::string& error,
                                VkMemoryPropertyFlags preferred) {
    if (preferred == 0) preferred = properties;
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
                                  preferred, type, selected, error)) {
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
    matter::gpu_memory_track_alloc(requirements.size, selected);
    out.tracked_alloc_size = requirements.size;
    out.tracked_mem_props = selected;
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

// Unmaps, destroys, un-tracks and zeroes. Safe on an already-empty Buffer, and
// safe after shutdown() cleared vulkan_ (it then just resets the struct — the
// device that owned the handles is gone).
void VtResidency::destroy_buffer(Buffer& b) {
    if (!vulkan_) {
        b = Buffer{};
        return;
    }
    const VkDevice device = vulkan_->device();
    if (b.mapped) vkUnmapMemory(device, b.memory);
    if (b.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, b.memory, nullptr);
        if (b.tracked_alloc_size > 0)
            matter::gpu_memory_track_free(b.tracked_alloc_size, b.tracked_mem_props);
    }
    b = Buffer{};
}

namespace {
// The 2D array image helper both the page pool and the feedback target are built
// from. mipLevels is 1 by construction: a pool layer has no hardware mip chain,
// because VT mips are virtual and resolved through the indirection instead.
// Device-local memory only. Rolls back the image, memory and census entry on any
// failure, and reports the allocation size so the caller can un-track it later.
bool create_array_image(matter::VulkanDevice& vulkan, VkFormat format,
                        uint32_t width, uint32_t height, uint32_t layers,
                        VkImageUsageFlags usage, VkImage& image,
                        VkImageView& view, VkDeviceMemory& memory,
                        std::string& error,
                        VkImageViewType view_type =
                            VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                        VkDeviceSize* out_alloc_size = nullptr) {
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
    matter::gpu_memory_track_alloc(requirements.size, selected);
    if (out_alloc_size) *out_alloc_size = requirements.size;
    if (vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
        error = "vt: vkBindImageMemory failed";
        matter::gpu_memory_track_free(requirements.size, selected);
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
        matter::gpu_memory_track_free(requirements.size, selected);
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}
}  // namespace

// (Re)creates one CHANNEL of the physical page pool: a square
// kVtPoolLayerEdgeTexels array image of `layers` layers. Usage is SAMPLED plus
// TRANSFER_DST and nothing else — deliberately no STORAGE, which is why fills
// reach the pool exclusively through transfer copies and why
// VtPoolBinding::transfer_dst_layout is set rather than negotiated per filler.
// A filler that needed GENERAL could not exist without changing this function.
//
// Destroys whatever the channel held first, and leaves the tracked layout at
// UNDEFINED; the first record_frame transitions it.
bool VtResidency::create_pool_image(uint32_t channel, VkFormat format,
                                    uint32_t layers, std::string& error) {
    PoolImage& out = pool_[channel];
    destroy_pool_image(out);
    VkDeviceSize alloc_size = 0;
    if (!create_array_image(*vulkan_, format, kVtPoolLayerEdgeTexels,
                            kVtPoolLayerEdgeTexels, layers,
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            out.image, out.view, out.memory, error,
                            VK_IMAGE_VIEW_TYPE_2D_ARRAY, &alloc_size)) {
        return false;
    }
    out.format = format;
    out.layers = layers;
    out.edge = kVtPoolLayerEdgeTexels;
    out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    out.tracked_alloc_size = alloc_size;
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
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, image.memory, nullptr);
        if (image.tracked_alloc_size > 0)
            matter::gpu_memory_track_free(image.tracked_alloc_size,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    image = PoolImage{};
}

// Brings the runtime up. Called lazily by the renderer on the first
// chart-bearing part, because the physical pool is large and a world with no
// charts should pay nothing for it. Returns true immediately if already up.
//
// ORDER MATTERS at the top: the env/registry budget pass runs before pool_pages
// is read, because that value is now one of the budgets. The pool size is then
// derived either from pool_mb (at 9 bytes per page texel: BC7 + BC5 + BC7 +
// RGBA8 + R16) or from an explicit page count, rounded UP to whole layers — and a
// layer count past the device's maxImageArrayLayers is a hard init failure, not
// a clamp.
//
// After that, in order: per-channel format-support check, the four pool images,
// the device-local indirection buffer, the zeroed staging for the one-time pool
// scrub, the linear and point samplers, the variant record buffer, the
// per-frame-slot table staging ring, the slot pool and table allocator, the free
// layer list, the pool binding handed to fillers, and finally a stub filler if
// none was installed.
//
// ANY failure calls shutdown() and returns false with `error` set. That is not
// fatal to the frame: the renderer keeps every part on the legacy path and
// records vt_unavailable_reason_.
bool VtResidency::init(matter::VulkanDevice& vulkan, std::string& error) {
    if (ready_) return true;
    vulkan_ = &vulkan;
    const char* event_log = std::getenv("MATTER_VT_EVENT_LOG");
    event_log_ = event_log && event_log[0] != '\0' && event_log[0] != '0';
    density_frame_ = 0;
    if (const char* density_frame = std::getenv("MATTER_VT_DENSITY_FRAME")) {
        char* end = nullptr;
        const uint64_t parsed = std::strtoull(density_frame, &end, 10);
        if (density_frame[0] >= '1' && density_frame[0] <= '9' &&
            end != density_frame && *end == '\0') density_frame_ = parsed;
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(vulkan.physical_device(), &properties);
    const uint32_t max_layers = properties.limits.maxImageArrayLayers;

    // Layer 5 for standalone engine runs (headless tests, tools) that never
    // bind a registry; the editor binds the SAME struct and runs its own
    // apply_env — both are idempotent and read the same environment. Hoisted
    // above the pool sizing because pool_pages is now one of these.
    matter::ensure_vt_residency_env_applied();
    const matter::VtResidencyBudgets& budgets = matter::vt_residency_budgets();

    // Derive pool page count. When pool_mb > 0 (the default), compute pages
    // from the VRAM budget; otherwise fall back to the explicit page count.
    // Pool cost includes composed R16 height as well as BC7/BC5/BC7/RGBA8.
    if (budgets.pool_mb > 0) {
        const uint64_t budget_bytes =
            static_cast<uint64_t>(budgets.pool_mb) * 1024u * 1024u;
        // Pages that fit inside the layer grid. Deliberately NOT
        // (kVtPageStride^2 * bytes_per_texel) * 256: the grid has no gaps, so a
        // layer costs kVtPoolLayerEdgeTexels^2 * bytes_per_texel, not 256 whole
        // strided pages. A per-page constant for the naive form used to sit
        // above this block; it was never read (the superseded calculation) and
        // tripped -Wunused-variable in the -Werror smoke-test build.
        constexpr uint64_t kBytesPerLayer =
            static_cast<uint64_t>(kVtPoolLayerEdgeTexels) *
            kVtPoolLayerEdgeTexels * kVtPoolBytesPerTexel;
        uint32_t layers_from_mb = static_cast<uint32_t>(
            std::min<uint64_t>(budget_bytes / kBytesPerLayer, 256u));
        if (layers_from_mb < 1u) layers_from_mb = 1u;
        pool_pages_ = layers_from_mb * kVtPagesPerLayer;
    } else {
        pool_pages_ = clamp_u32(budgets.pool_pages, kVtPagesPerLayer,
                                kVtPagesPerLayer * 256u);
    }
    // Round up to whole layers.
    const uint32_t pool_layers =
        (pool_pages_ + kVtPagesPerLayer - 1u) / kVtPagesPerLayer;
    pool_pages_ = pool_layers * kVtPagesPerLayer;
    if (pool_layers > max_layers) {
        error = "vt: pool layer count exceeds maxImageArrayLayers";
        shutdown();
        return false;
    }
    // MATTER_VT_MAX_VARIANTS is a SOFT bookkeeping bound now, not a hardware
    // wall. The old image-array indirection burned one array layer per
    // (variant, rung), and NVIDIA's per-format maxArrayLayers for R16G16_UINT
    // is 2048 — that cap is what forced the buffer-based indirection this
    // sizes. The remaining hard ceiling is 65534: the feedback image and the
    // draw records transport (variant slot + 1) in 16 bits. The real limits on
    // a streamed world are the CPU mesh budget (MATTER_VT_MESH_BUDGET_MB) and
    // the physical page pool (MATTER_VT_POOL_PAGES, one pinned tail per
    // registration) — this knob just sizes the record table (64 B per slot).
    //
    // The budgets all live in matter::VtResidencyBudgets; the env pass that
    // seeds them ran above, before pool_pages was read.
    max_variants_ = clamp_u32(budgets.max_variants, 4u, 65534u);
    // The four live budgets (fills / tail fills / enrich / mesh) come from
    // refresh_budgets, which also runs every begin_frame so an editor edit
    // takes effect on the next frame.
    refresh_budgets();
    // Indirection table arena. 64 MiB = 16.7M entries: thousands of worst-case
    // (16384^2, 21846-entry) tables, hundreds of thousands of typical ones — an
    // order of magnitude past any working set the mesh budget admits, so the
    // arena is never the first wall. See VtTableAllocator's growth-policy note
    // for why it is pre-sized rather than grown live. Like max_variants it
    // sizes a buffer at init, so it is not live-editable.
    const uint32_t indirection_mb = clamp_u32(budgets.indirection_mb, 1u, 1024u);
    const uint32_t indirection_words =
        static_cast<uint32_t>(std::min<uint64_t>(
            static_cast<uint64_t>(indirection_mb) * 1024u * 1024u / 4u,
            0xFFFFFFF0ull));
    debug_generations_ = env_flag("MATTER_VT_DEBUG_GENERATIONS");

    const VkFormat formats[kVtChannelCount] = {
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16_UNORM};
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
    // The indirection: one device-local storage buffer of packed u32 entries,
    // written exclusively through queue-ordered vkCmdCopyBuffer (host-visible
    // would let a CPU write race an in-flight frame's reads — the exact hazard
    // the recycling contract exists to exclude).
    if (!create_buffer(static_cast<VkDeviceSize>(indirection_words) * 4u,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       indirection_buffer_, error)) {
        shutdown();
        return false;
    }
    indirection_cleared_ = false;

    // Zeroed staging for the one-time pool-image clear (see the header note on
    // pool_zero_staging_). One BC channel-layer is layer_edge^2 texels at 1
    // byte each; the buffer is reused for every compressed channel and layer.
    {
        const VkDeviceSize zero_bytes =
            static_cast<VkDeviceSize>(kVtPoolLayerEdgeTexels) *
            kVtPoolLayerEdgeTexels;
        if (!create_buffer(zero_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           pool_zero_staging_, error)) {
            shutdown();
            return false;
        }
        std::memset(pool_zero_staging_.mapped, 0,
                    static_cast<size_t>(zero_bytes));
    }
    pool_cleared_ = false;
    zero_staging_retire_ = 0;
    activation_dirty_ = false;

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
    // Table upload staging: a RING, one buffer per frame slot, because a
    // single buffer rewritten every frame would clobber bytes a still-
    // executing previous frame's copy has not consumed. Each slot holds the
    // frame's fill-dirtied tables plus a healthy registration burst at the
    // worst-case table size; overflow just defers the upload a frame
    // (Stats::table_uploads_deferred_total counts it).
    const VkDeviceSize staging_bytes =
        static_cast<VkDeviceSize>(kVtMaxTableWords) * 4u *
        (max_fills_per_frame_ + 24u);
    for (uint32_t i = 0; i < kFeedbackSlots; ++i) {
        if (!create_buffer(staging_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           indirection_staging_[i], error)) {
            shutdown();
            return false;
        }
    }

    // Scratch destinations are disjoint from every resident/retiring slot.
    // One batch is enough: queue-ordered copy-back consumes each candidate
    // before the next frame can reuse this reserve. No extra image allocation.
    slots_.reset(pool_pages_ - kMaxFillFlags);
    material_pages_.reset(slots_.capacity());
    slots_.set_debug(debug_generations_);
    // Eviction hysteresis window. One frame is NOT enough in practice:
    // temporal jitter (DLSS) shifts which feedback blocks sample which pages,
    // so at a fixed camera a hot page is requested only every few frames; a
    // one-frame window let an oversubscribed pool ping-pong pages forever
    // (measured ~27 evictions/s on StreamMountain at MATTER_VT_POOL_PAGES=256)
    // instead of settling blurry-but-stable. 16 frames (~a quarter second)
    // comfortably covers the jitter cycle while adding negligible latency to
    // legitimate eviction of pages that left the view.
    // Pushed here so the window is right from the first frame, and again from
    // refresh_budgets every begin_frame so an editor edit is live.
    slots_.set_protect_frames(
        clamp_u32(budgets.evict_protect_frames, 1u, 100000u));
    tables_.reset(indirection_words);
    tables_.set_debug(debug_generations_);
    slot_tier_.assign(pool_pages_, 0u);
    slot_input_snapshots_.assign(pool_pages_, nullptr);
    slot_geometry_lifetimes_.assign(pool_pages_, nullptr);
    slot_occlusion_pages_.assign(pool_pages_,nullptr);
    occlusion_pages_=std::make_unique<VtOcclusionPages>(pool_pages_+16u*uint32_t(kVtRetireHorizonFrames+1));
    slot_material_mappings_.assign(pool_pages_, nullptr);
    slot_content_revisions_.assign(pool_pages_, 0);
    slot_page_metadata_.assign(pool_pages_, VtPageMetadata{});
    if (!create_buffer(static_cast<VkDeviceSize>(pool_pages_) * sizeof(VtPageMetadata),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, input_snapshot_buffer_, error)) {
        shutdown();
        return false;
    }
    input_indices_dirty_begin_ = 0;
    input_indices_dirty_end_ = pool_pages_;
    enrich_queue_.clear();
    enrich_queued_slot_.clear();
    variants_.clear();           // grows lazily with the slot high-water mark
    // VariantRung::context is self-referential: it borrows the owning rung's
    // vector storage and its atlas. Any relocation of a VariantRung during
    // variants_ growth leaves both the context storage pointers and
    // context.atlas aimed at the old rung. The slot ceiling is fixed for this
    // residency lifetime, so reserve it once and make every rung address
    // stable before any context is adopted.
    variants_.reserve(max_variants_);
    layer_graveyard_.clear();
    debug_layer_reuse_.clear();
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

    // Image bytes include the two-byte composed height; metadata is a separate buffer.
    const uint64_t layer_texels = static_cast<uint64_t>(kVtPoolLayerEdgeTexels) *
                                  kVtPoolLayerEdgeTexels;
    stats_ = Stats{};
    stats_.pool_capacity = pool_pages_;
    stats_.replacement_reserve_pages = kMaxFillFlags;
    stats_.max_variants = max_variants_;
    stats_.mesh_budget_bytes = mesh_budget_bytes_;
    stats_.pool_bytes = layer_texels * pool_layers * kVtPoolBytesPerTexel;
    stats_.indirection_capacity_bytes =
        static_cast<uint64_t>(indirection_words) * 4u;
    stats_.enrich_samples = enricher_ ? enricher_->sample_count() : 0u;
    ready_ = true;
    return true;
}

// Full teardown, in dependency order: the filler and enricher first (they own
// their own GPU objects and reference the pool binding), then the pool and
// feedback images, samplers and buffers, then every piece of CPU bookkeeping.
// Idempotent, and safe on an object that was never init()ed.
//
// It does NOT wait the device idle. Every caller must already have done so —
// the destructor's caller included.
void VtResidency::shutdown() {
    // Invalidate the epoch before releasing any registration. Old leases can
    // neither call this runtime nor bind to a later init that recycles slots.
    module_owner_.reset();
    modules_.clear();
    if (!vulkan_) {
        ready_ = false;
        return;
    }
    filler_.reset();
    enricher_.reset();
    feedback_gpu_.reset();
    for (uint32_t c = 0; c < kVtChannelCount; ++c) destroy_pool_image(pool_[c]);
    feedback_source_lifetime_.reset();
    feedback_source_view_ = VK_NULL_HANDLE;
    const VkDevice device = vulkan_->device();
    if (pool_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device, pool_sampler_, nullptr);
    if (point_sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device, point_sampler_, nullptr);
    pool_sampler_ = VK_NULL_HANDLE;
    point_sampler_ = VK_NULL_HANDLE;
    destroy_buffer(variant_buffer_);
    destroy_buffer(input_snapshot_buffer_);
    destroy_buffer(indirection_buffer_);
    destroy_buffer(pool_zero_staging_);
    for (uint32_t i = 0; i < kFeedbackSlots; ++i)
        destroy_buffer(indirection_staging_[i]);
    for (uint32_t i = 0; i < kFeedbackSlots; ++i)
        destroy_buffer(feedback_readback_[i]);
    variants_.clear();
    surface_connections_.reset();surface_connection_pairs_.clear();
    free_layers_.clear();
    layer_graveyard_.clear();
    debug_layer_reuse_.clear();
    layer_of_.clear();
    param_key_of_rung_.clear();   // M6: alias table dies with the layers
    material_dependents_.clear();
    queue_.clear();
    queued_keys_.clear();
    batch_.clear();
    dirty_pages_.clear();
    enrich_queue_.clear();
    enrich_queued_slot_.clear();
    enrich_batch_.clear();
    slot_tier_.clear();
    slot_input_snapshots_.clear();
    slot_geometry_lifetimes_.clear();
    slot_occlusion_pages_.clear();
    slot_material_mappings_.clear();
    slot_content_revisions_.clear();
    retired_geometries_.clear();
    occlusion_pages_.reset();
    for (auto& candidate : fill_geometries_) candidate = {};
    slot_page_metadata_.clear();
    material_pages_.reset(0);
    input_snapshot_.reset();
    for (auto& retired : retired_input_snapshots_) retired = {};
    for (auto& registered : input_snapshot_registry_) registered.reset();
    input_indices_dirty_begin_ = UINT32_MAX;
    input_indices_dirty_end_ = 0;
    page_fills_paused_for_test_ = false;
    input_update_pending_ = false;
    slot_input_counts_.fill(0);
    variant_records_.clear();
    mesh_bytes_used_ = 0;
    warned_rejection_ = false;
    indirection_cleared_ = false;
    feedback_w_ = feedback_h_ = 0;
    feedback_raster_w_ = feedback_raster_h_ = 0;
    ready_ = false;
    vulkan_ = nullptr;
}

bool VtResidency::set_input_snapshot(std::shared_ptr<const VtInputSnapshot> snapshot,
                                    const std::vector<uint32_t>& changed_material_ids,
                                    VtInvalidationReason reason) {
    if (!ready_) return false;
    if (snapshot) {
        if (snapshot->index >= kVtMaxInputSnapshots || !snapshot->lifetime) return false;
        const auto previous = input_snapshot_registry_[snapshot->index].lock();
        if (previous && previous != snapshot) return false;
    }
    if (snapshot == input_snapshot_) {
        input_update_pending_ = false;
        return true;
    }
    if (snapshot) input_snapshot_registry_[snapshot->index] = snapshot;
    input_snapshot_ = std::move(snapshot);
    // Dirtiness is durable across successive edits. In particular, an A page
    // waiting for B must not be retagged as C just because B -> C changed a
    // different material. Only already-clean, unaffected pages can be retagged.
    invalidate_material_content(changed_material_ids, reason);
    rebind_compatible_input_snapshots(changed_material_ids);
    input_update_pending_ = false;
    return true;
}

std::array<std::shared_ptr<const VtInputSnapshot>, kVtMaxInputSnapshots>
VtResidency::active_input_snapshots() const {
    std::array<std::shared_ptr<const VtInputSnapshot>, kVtMaxInputSnapshots> result{};
    for (uint32_t i = 0; i < kVtMaxInputSnapshots; ++i)
        if (slot_input_counts_[i] || retired_input_snapshots_[i].snapshot ||
            (input_snapshot_ && input_snapshot_->index == i))
            result[i] = input_snapshot_registry_[i].lock();
    return result;
}

void VtResidency::retire_slot_input_snapshot(uint32_t slot) {
    auto& previous = slot_input_snapshots_[slot];
    if (!previous) return;
    --slot_input_counts_[previous->index];
    auto& retired = retired_input_snapshots_[previous->index];
    retired.retire_serial = std::max(retired.retire_serial,
                                    frame_index_ + kVtRetireHorizonFrames);
    // The weak registry prevents a different live object from sharing this
    // index. There are at most eight entries, regardless of page/edit count.
    retired.snapshot = std::move(previous);
}

void VtResidency::set_slot_input_snapshot(uint32_t slot,
                                        std::shared_ptr<const VtInputSnapshot> snapshot) {
    const uint32_t index = snapshot ? snapshot->index : kVtNoInputSnapshot;
    if (slot_input_snapshots_[slot] == snapshot && slot_page_metadata_[slot].input_snapshot == index) return;
    retire_slot_input_snapshot(slot);
    slot_input_snapshots_[slot] = std::move(snapshot);
    if (slot_input_snapshots_[slot]) ++slot_input_counts_[index];
    slot_page_metadata_[slot].input_snapshot = index;
    const auto owner=layer_of_.find(slots_.owner(slot).variant_key);
    if(owner!=layer_of_.end()) {
        auto& v=variants_[owner->second];
        if(v.live && v.tail_slot==slot && v.boundary_source && !dirty_pages_.count(slot)) {
            auto next=std::make_shared<VtSurfaceBoundarySource>(*v.boundary_source);
            next->material_inputs=slot_input_snapshots_[slot];next->metadata=slot_page_metadata_[slot];
            v.boundary_source=std::move(next);
        }
    }
    input_indices_dirty_begin_ = std::min(input_indices_dirty_begin_, slot);
    input_indices_dirty_end_ = std::max(input_indices_dirty_end_, slot + 1u);
}

std::shared_ptr<const VtSurfaceBoundarySource> VtResidency::surface_boundary_source(uint32_t slot) const {
    if(!slot || slot>variants_.size())return {};
    const auto& source=variants_[slot-1].boundary_source;
    return source && surface_boundary_source_current(*source) ? source : nullptr;
}

bool VtResidency::surface_boundary_source_current(const VtSurfaceBoundarySource& source) const {
    if(!source.slot || source.slot>variants_.size())return false;
    const auto& v=variants_[source.slot-1];
    return v.live && v.tail_filled && v.tail_ready_serial<=frame_index_ &&
        uint32_t(v.table_generation)==source.generation && v.inputs==source.inputs &&
        v.content_revision==source.content_revision && v.boundary_source.get()==&source &&
        source.geometry.lifetime && source.geometry.boundary &&
        slot_geometry_lifetimes_[v.tail_slot]==source.geometry.lifetime &&
        slot_input_snapshots_[v.tail_slot]==source.material_inputs &&
        slot_content_revisions_[v.tail_slot]==source.content_revision &&
        !dirty_pages_.count(v.tail_slot);
}

bool VtResidency::set_surface_connections(const std::vector<VtSurfaceConnectionPair>& pairs,std::string& error) {
    if(pairs.size()>65536){error="surface connection pair budget exceeded";return false;}
    for(const auto& p:pairs)if(!p.domain || !p.first || !p.second || p.first==p.second) {
        error="surface connections require distinct owners and an explicit continuity domain";return false;
    }
    auto canonical=pairs;
    for(auto& pair:canonical) {
        if(std::tie(pair.second,pair.second_rung)<std::tie(pair.first,pair.first_rung)) {
            std::swap(pair.first,pair.second);std::swap(pair.first_rung,pair.second_rung);
        }
    }
    std::sort(canonical.begin(),canonical.end(),[](const auto& a,const auto& b){
        return surface_pair_key(a)<surface_pair_key(b);
    });
    const auto equal=[](const auto& a,const auto& b){return surface_pair_key(a)==surface_pair_key(b);};
    canonical.erase(std::unique(canonical.begin(),canonical.end(),equal),canonical.end());
    if(canonical.size()!=surface_connection_pairs_.size() ||
       !std::equal(canonical.begin(),canonical.end(),surface_connection_pairs_.begin(),equal)) {
        surface_connection_pairs_=std::move(canonical);surface_connections_changed_=true;
    }
    error.clear();return true;
}

void VtResidency::publish_surface_connections(std::string& error) {
    PROFILE_SCOPE("vt.surface_connections");
    if (!surface_walk_enabled_) return;
    if(surface_connection_pairs_.empty() && !surface_connections_)return;
    std::vector<std::shared_ptr<const VtSurfaceBoundarySource>> sources;
    sources.reserve(surface_connection_pairs_.size()*2);
    for(const auto& pair:surface_connection_pairs_) {
        sources.push_back(surface_boundary_source(slot_for(pair.first,pair.first_rung)));
        sources.push_back(surface_boundary_source(slot_for(pair.second,pair.second_rung)));
    }
    const auto sync=[&](const std::shared_ptr<VtSurfaceConnectionState>& state,bool clear) {
        if(!state)return;
        for(const auto& table:state->tables) {
            if(table.first>=variant_records_.size())continue;
            auto& record=variant_records_[table.first];const uint64_t address=clear?0:table.second->buffer.address;
            if(record.surface_links_low!=uint32_t(address) || record.surface_links_high!=uint32_t(address>>32)) {
                record.surface_links_low=uint32_t(address);record.surface_links_high=uint32_t(address>>32);
                variant_records_dirty_=true;
            }
        }
    };
    if(!surface_connections_changed_ && surface_connections_ && sources==surface_connections_->sources) {
        sync(surface_connections_,false);return;
    }
    auto next=std::make_shared<VtSurfaceConnectionState>();next->sources=std::move(sources);
    std::map<uint32_t,std::vector<VtSurfaceLinkGpu>> links;
    std::map<uint32_t,std::shared_ptr<const VtSurfaceBoundarySource>> owners;
    size_t total_links=0;
    bool valid=true;
    for(size_t pair_index=0;pair_index<surface_connection_pairs_.size() && valid;++pair_index) {
        const auto& pair=surface_connection_pairs_[pair_index];
        const auto a=next->sources[pair_index*2],b=next->sources[pair_index*2+1];
        if(!a || !b || a->slot==b->slot)continue;
        const auto key=surface_pair_key(pair);
        std::shared_ptr<const VtSurfacePairCache> cached;
        if(surface_connections_) {
            const auto old=surface_connections_->pairs.find(key);
            if(old!=surface_connections_->pairs.end() && old->second->sources[0]==a && old->second->sources[1]==b)
                cached=old->second;
        }
        if(cached) {
            next->pairs.emplace(key,cached);
            for(int direction=0;direction<2;++direction) {
                const auto& from=cached->sources[direction];
                const auto& added=cached->links[direction];
                if(added.size()>262144-total_links) {
                    error="surface connection GPU storage budget exceeded";valid=false;break;
                }
                total_links+=added.size();owners[from->slot-1]=from;
                auto& records=links[from->slot-1];records.insert(records.end(),added.begin(),added.end());
            }
            continue;
        }
        // This publication implements continuous world-authored direct sources.
        // Independently mapped modules need their own retained mapping contract.
        if(!a->inputs->context.surface_world_anchored || !b->inputs->context.surface_world_anchored ||
           a->inputs->context.finite_sources || b->inputs->context.finite_sources ||
           a->metadata.mapping_count || b->metadata.mapping_count ||
           a->material_inputs!=b->material_inputs || a->metadata.height.version!=1 ||
           std::memcmp(&a->metadata.height,&b->metadata.height,sizeof(VtPageHeight)) ||
           a->inputs->surface->tape_hash!=b->inputs->surface->tape_hash ||
           a->inputs->surface->tape_text!=b->inputs->surface->tape_text)continue;
        auto compiled=std::make_shared<VtSurfacePairCache>();compiled->sources={a,b};
        ++stats_.surface_pairs_compiled_total;
        for(int direction=0;direction<2 && valid;++direction) {
            const auto from=direction?b:a,to=direction?a:b;
            std::vector<VtSurfaceBoundaryLink> matches;
            const float* fm=from->inputs->context.surface_local_to_world;
            const float* tm=to->inputs->context.surface_local_to_world;
            valid=vt_link_surface_boundaries(*from->geometry.boundary,fm,pair.domain,
                *to->geometry.boundary,tm,pair.domain,matches,error);
            if(!valid)break;
            auto& records=links[from->slot-1];owners[from->slot-1]=from;
            for(const auto& match:matches) {
                if(++total_links>262144){error="surface connection GPU storage budget exceeded";valid=false;break;}
                const auto& edge=from->geometry.boundary->edges[match.source_edge];
                const auto& target=to->geometry.boundary->edges[match.target_edge];
                VtSurfaceLinkGpu record;
                record.edge[0]=edge.triangle;record.edge[1]=edge.edge;
                record.edge[2]=target.triangle;record.edge[3]=target.chart;
                record.target[0]=to->slot;record.target[1]=to->generation;
                record.target[2]=to->inputs->geometry->atlas.atlas_w;
                record.target[3]=to->inputs->geometry->atlas.atlas_h;
                record.metadata=to->metadata;
                record.interval[0]=float(match.source_begin);record.interval[1]=float(match.source_end);
                record.interval[2]=float(match.target_begin);record.interval[3]=float(match.target_end);
                for(int row=0;row<3;++row) {
                    for(int col=0;col<3;++col)for(int k=0;k<3;++k)
                        record.transform[row*4+col]+=tm[k*4+row]*fm[k*4+col];
                    double offset=0;for(int k=0;k<3;++k)
                        offset+=double(tm[k*4+row])*(double(fm[k*4+3])-tm[k*4+3]);
                    record.transform[row*4+3]=float(offset);
                }
                records.push_back(record);
                compiled->links[direction].push_back(record);
            }
        }
        if(valid)next->pairs.emplace(key,std::move(compiled));
    }
    for(auto& item:links) {
        if(!valid)break;
        auto& records=item.second;if(records.empty())continue;
        std::sort(records.begin(),records.end(),[](const auto& a,const auto& b){
            return std::tie(a.edge[0],a.edge[1],a.interval[0],a.target[0])<
                   std::tie(b.edge[0],b.edge[1],b.interval[0],b.target[0]);
        });
        size_t edge_count=0;
        for(size_t i=0;i<records.size();++i) {
            const bool same=i && records[i-1].edge[0]==records[i].edge[0] && records[i-1].edge[1]==records[i].edge[1];
            edge_count=same?edge_count+1:1;
            if(edge_count>64 || (same && records[i-1].interval[1]>records[i].interval[0]+1e-6f)) {
                error="surface connection destinations overlap or exceed the per-edge bound";valid=false;break;
            }
        }
        if(!valid)break;
        const auto& owner=owners[item.first];VtSurfaceLinksHeaderGpu header;
        header.metadata=owner->metadata;header.source[0]=owner->slot;header.source[1]=owner->generation;
        header.source[2]=uint32_t(records.size());
        std::vector<uint8_t> bytes(sizeof(header)+records.size()*sizeof(VtSurfaceLinkGpu));
        std::memcpy(bytes.data(),&header,sizeof(header));
        std::memcpy(bytes.data()+sizeof(header),records.data(),records.size()*sizeof(VtSurfaceLinkGpu));
        // Preserve the exact GPU address of every unaffected owner. Retained
        // states keep old source leases alive for frames already in flight.
        if(surface_connections_) {
            const auto old=surface_connections_->tables.find(item.first);
            if(old!=surface_connections_->tables.end() && old->second->bytes==bytes) {
                next->tables.emplace(item.first,old->second);continue;
            }
        }
        auto table=std::make_shared<VtSurfaceLinkTable>();table->bytes=std::move(bytes);
        valid=matter::create_buffer(*vulkan_,table->bytes.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,table->buffer,error) &&
            matter::upload_buffer(*vulkan_,table->buffer,table->bytes.data(),table->bytes.size(),0,error);
        if(valid) {++stats_.surface_table_uploads_total;next->tables.emplace(item.first,std::move(table));}
    }
    if(!valid) {next->tables.clear();MATTER_LOGW("vt-surface-links","%s",error.c_str());}
    sync(surface_connections_,true);
    if(surface_connections_) {
        auto& retired=retired_geometries_[surface_connections_.get()];
        retired.retire_serial=frame_index_+kVtRetireHorizonFrames;retired.lifetime=surface_connections_;
    }
    surface_connections_=std::move(next);surface_connections_changed_=false;
    sync(surface_connections_,false);
    stats_.surface_link_tables=uint32_t(surface_connections_->tables.size());
    stats_.surface_link_table_bytes=0;
    for(const auto& table:surface_connections_->tables)stats_.surface_link_table_bytes+=table.second->bytes.size();
}

void VtResidency::retire_slot_geometry(uint32_t slot) {
    retire_slot_occlusion(slot);
    if(slot_page_metadata_[slot].geometry.page_flags&kVtCoverageOnly) {
        --stats_.coverage_only_pages;
        slot_page_metadata_[slot].geometry.page_flags&=~kVtCoverageOnly;
    }
    input_indices_dirty_begin_ = std::min(input_indices_dirty_begin_, slot);
    input_indices_dirty_end_ = std::max(input_indices_dirty_end_, slot + 1u);
    auto& previous = slot_geometry_lifetimes_[slot];
    if (!previous) return;
    auto& retired = retired_geometries_[previous.get()];
    retired.retire_serial = std::max(retired.retire_serial,
                                    frame_index_ + kVtRetireHorizonFrames);
    retired.lifetime = std::move(previous);
}

void VtResidency::set_slot_geometry(uint32_t slot, const VtDrawGeometry& geometry) {
    retire_slot_occlusion(slot);
    if (slot_geometry_lifetimes_[slot] != geometry.lifetime) {
        retire_slot_geometry(slot);
        slot_geometry_lifetimes_[slot] = geometry.lifetime;
    }
    // A producer cannot publish an unowned device address. Legacy pages use
    // zero addresses and continue to take the existing chart-local route.
    if(slot_page_metadata_[slot].geometry.page_flags&kVtCoverageOnly) --stats_.coverage_only_pages;
    slot_page_metadata_[slot].geometry = geometry.lifetime ? geometry.gpu : VtDrawGeometryGpu{};
    if(slot_page_metadata_[slot].geometry.page_flags&kVtCoverageOnly) ++stats_.coverage_only_pages;
}

void VtResidency::retire_occlusion(std::shared_ptr<VtOcclusionPages::Page> page) {
    if(!page)return;
    auto& retired=retired_geometries_[page.get()];
    retired.retire_serial=std::max(retired.retire_serial,frame_index_+kVtRetireHorizonFrames);
    retired.lifetime=std::move(page);
}

void VtResidency::retire_slot_occlusion(uint32_t slot) {
    if(slot_occlusion_pages_[slot]) {
        retire_occlusion(std::move(slot_occlusion_pages_[slot]));
        --stats_.occlusion_pages;
    }
    slot_page_metadata_[slot].occlusion_address=0;
    input_indices_dirty_begin_=std::min(input_indices_dirty_begin_,slot);
    input_indices_dirty_end_=std::max(input_indices_dirty_end_,slot+1);
}

void VtResidency::retire_slot_material_mapping(uint32_t slot) {
    auto& previous=slot_material_mappings_[slot];
    if (!previous) return;
    auto& retired=retired_geometries_[previous.get()];
    retired.retire_serial=std::max(retired.retire_serial,frame_index_+kVtRetireHorizonFrames);
    retired.lifetime=std::move(previous);
}

void VtResidency::set_slot_material_mapping(uint32_t slot,
    std::shared_ptr<VtReceiverMaterialState> mapping) {
    if (slot_material_mappings_[slot]!=mapping) {
        retire_slot_material_mapping(slot);
        slot_material_mappings_[slot]=mapping;
    }
    auto& metadata=slot_page_metadata_[slot];
    const uint64_t address=mapping?mapping->buffer.address:0;
    metadata.mapping_address[0]=uint32_t(address);
    metadata.mapping_address[1]=uint32_t(address>>32);
    metadata.mapping_count=mapping?uint32_t(mapping->records.size()):0;
    input_indices_dirty_begin_=std::min(input_indices_dirty_begin_,slot);
    input_indices_dirty_end_=std::max(input_indices_dirty_end_,slot+1);
}

bool VtResidency::bind_receiver_materials(uint64_t hash,uint32_t rung,
    const std::vector<VtReceiverMaterialChart>& charts,std::string& error) {
    const auto fail=[&](const char* message){error=message;return false;};
    const uint32_t slot=slot_for(hash,rung);
    if (!ready_ || !slot || rung>=32) return fail("receiver material owner is not registered");
    auto& receiver=variants_[slot-1];
    if (!receiver.live || receiver.rung==kMaterialModuleRung) return fail("material modules cannot receive module mappings");
    try {
        auto next=std::make_shared<VtReceiverMaterialState>();
        next->inputs=receiver.inputs;next->charts=charts;
        if (!charts.empty()) {
            std::array<float,2> envelope;
            if (!vt_receiver_height_range(*receiver.inputs,envelope,error)) return false;
            const size_t count=receiver.inputs->geometry->atlas.charts.size();
            if (!count || count>65536 || charts.size()>count) return fail("receiver material chart count exceeds coverage encoding");
            next->records.resize(count);
            for (const auto& chart:charts) {
                const auto& lease=chart.module;
                if (!lease || lease->owner_.lock()!=module_owner_ || !lease->slot_ || lease->slot_>variants_.size())
                    return fail("receiver material module belongs to a different runtime");
                const auto& module=variants_[lease->slot_-1];
                if (!module.live || module.rung!=kMaterialModuleRung || module.table_generation!=lease->generation_ ||
                    module.variant_hash!=lease->hash_) return fail("receiver material module has expired");
                if (chart.chart>=count || next->records[chart.chart].binding[0]) return fail("duplicate or absent receiver material chart");
                auto& record=next->records[chart.chart];
                if (!vt_receiver_material_chart(*receiver.inputs,module.inputs->context.periodic,chart,record,error)) return false;
                record.binding[0]=lease->slot_;record.binding[1]=uint32_t(lease->generation_);
                std::array<float,2> range;
                if (!vt_receiver_height_range(*module.inputs,range,error)) return false;
                envelope[0]=std::min(envelope[0],range[0]+chart.datum_m);
                envelope[1]=std::max(envelope[1],range[1]+chart.datum_m);
            }
            if (!std::isfinite(envelope[0]) || !std::isfinite(envelope[1]) ||
                !std::isfinite(envelope[1]-envelope[0])) return fail("receiver material height envelope overflow");
            for (auto& record:next->records) {
                record.metrics[2]=envelope[0];record.metrics[3]=envelope[1]-envelope[0];
            }
        }
        const auto same=[&](const std::shared_ptr<VtReceiverMaterialState>& current) {
            return current && current->inputs==next->inputs && current->records.size()==next->records.size() &&
                (next->records.empty() || std::memcmp(current->records.data(),next->records.data(),
                    next->records.size()*sizeof(VtReceiverMaterialGpu))==0);
        };
        if(same(receiver.material_candidate)){error.clear();return true;}
        if(same(receiver.material_published)){receiver.material_candidate.reset();error.clear();return true;}
        if(!next->records.empty()) {
            if (!matter::create_buffer(*vulkan_,next->records.size()*sizeof(VtReceiverMaterialGpu),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,next->buffer,error) ||
                !matter::upload_buffer(*vulkan_,next->buffer,next->records.data(),next->buffer.size,0,error)) return false;
        }
        // The table is immutable before publication. The old published table
        // remains retained by its resident pages and all earlier GPU readers.
        receiver.material_candidate=std::move(next);
        error.clear();return true;
    } catch (const std::bad_alloc&) {return fail("receiver material mapping allocation failed; previous mapping retained");}
}

void VtResidency::publish_receiver_materials() {
    for (auto& receiver:variants_) {
        if (!receiver.live || receiver.rung==kMaterialModuleRung) continue;
        // A compatible LOD promotion or a source edit owns a new snapshot.
        // It cannot silently inherit a projection validated for the old one.
        if (receiver.material_candidate && receiver.material_candidate->inputs!=receiver.inputs)
            receiver.material_candidate.reset();
        if (receiver.material_published && receiver.material_published->inputs!=receiver.inputs)
            receiver.material_published.reset();
        auto& candidate=receiver.material_candidate;
        if (!candidate) continue;
        bool ready=true;
        for (const auto& chart:candidate->charts)
            ready=ready && material_module_binding(chart.module).slot!=0;
        if (!ready) continue;
        // A removed/narrowed mapping needs full finite material again. Refill
        // those coverage-only pages while their previous mapping remains live;
        // publish the new table only once every incompatible page is complete.
        for(uint32_t slot=0;slot<slots_.capacity();++slot) {
            const auto& owner=slots_.owner(slot);
            if(!owner.live || owner.variant_key!=receiver.param_key ||
               !(slot_page_metadata_[slot].geometry.page_flags&kVtCoverageOnly) ||
               slot_content_revisions_[slot]!=receiver.content_revision) continue;
            if(vt_receiver_material_page(*receiver.inputs,candidate->records,
                    owner.page.mip,owner.page.px,owner.page.py)) continue;
            ready=false;
            dirty_pages_.try_emplace(slot,PendingFill{receiver.layer,owner.page,kVtMaxMips,
                frame_index_,UINT32_MAX,receiver.table_generation,receiver.content_revision});
            queue_page(receiver,owner.page,true);
        }
        if(!ready) continue;
        receiver.material_published=std::move(candidate);
        for (uint32_t slot=0;slot<slots_.capacity();++slot) {
            const auto& owner=slots_.owner(slot);
            if (owner.live && owner.variant_key==receiver.param_key &&
                slot_content_revisions_[slot]==receiver.content_revision &&
                receiver.indirection.is_mapped(owner.page.mip,owner.page.px,owner.page.py))
                set_slot_material_mapping(slot,receiver.material_published);
        }
    }
}

void VtResidency::rebind_compatible_input_snapshots(
        const std::vector<uint32_t>& changed_material_ids) {
    std::vector<uint8_t> affected(variants_.size(), 0);
    for (uint32_t id : changed_material_ids) {
        const auto found = material_dependents_.find(id);
        if (found == material_dependents_.end()) continue;
        for (uint32_t layer : found->second) affected[layer] = 1;
    }
    for (uint32_t slot = 0; slot < slots_.capacity(); ++slot) {
        const auto& owner = slots_.owner(slot);
        if (!owner.live || slot_input_snapshots_[slot] == input_snapshot_ ||
            dirty_pages_.find(slot) != dirty_pages_.end()) continue;
        const auto found = layer_of_.find(owner.variant_key);
        if (found == layer_of_.end() || affected[found->second]) continue;
        const auto& variant = variants_[found->second];
        if (!variant.tail_filled || !variant.indirection.is_mapped(
                owner.page.mip, owner.page.px, owner.page.py)) continue;
        set_slot_input_snapshot(slot, input_snapshot_);
    }
}

void VtResidency::record_input_snapshot_indices(VkCommandBuffer cmd) {
    if (input_indices_dirty_begin_ >= input_indices_dirty_end_) return;
    buffer_barrier(cmd, input_snapshot_buffer_.buffer,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    // vkCmdUpdateBuffer captures these CPU bytes at record time. Multiple
    // frames can be recorded before submission without rewriting staging used
    // by an earlier reader. Updates are four-byte aligned and <= 65536 bytes.
    constexpr uint32_t max_records = 65536u / sizeof(VtPageMetadata);
    for (uint32_t begin = input_indices_dirty_begin_; begin < input_indices_dirty_end_;) {
        const uint32_t count = std::min(max_records, input_indices_dirty_end_ - begin);
        vkCmdUpdateBuffer(cmd, input_snapshot_buffer_.buffer,
                          static_cast<VkDeviceSize>(begin) * sizeof(VtPageMetadata),
                          static_cast<VkDeviceSize>(count) * sizeof(VtPageMetadata),
                          slot_page_metadata_.data() + begin);
        begin += count;
    }
    buffer_barrier(cmd, input_snapshot_buffer_.buffer,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    input_indices_dirty_begin_ = UINT32_MAX;
    input_indices_dirty_end_ = 0;
}

VkImageView VtResidency::pool_view(uint32_t channel) const {
    return channel < kVtChannelCount ? pool_[channel].view : VK_NULL_HANDLE;
}

// Installs the tier-1 page filler, replacing the stub init() puts in when none
// was set. The residency layer OWNS it; the renderer borrows the pointer back
// through filler(). Already-queued fills are not re-targeted: each one executes
// against whichever filler is installed when record_frame drains it, so a swap
// must be paired with invalidate_all_content() if the new filler bakes
// different content.
void VtResidency::set_filler(std::unique_ptr<VtPageFiller> filler) {
    filler_ = std::move(filler);
}

void VtResidency::set_enricher(std::unique_ptr<VtPageEnricher> enricher) {
    enricher_ = std::move(enricher);
    // Context-dependent ORM must become private before enrichment is queued.
    if (enricher_ && max_enrich_per_frame_ && !enricher_->supports_separate_occlusion() &&
        (material_pages_.shared_references() != 0 || stats_.coverage_only_pages))
        invalidate_all_content();
    stats_.enrich_samples = enricher_ ? enricher_->sample_count() : 0u;
    if(ready_ && enricher_)queue_resident_enrichment();
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
    // Warn once and retain the rejection counter. Initial admission falls
    // back to the legacy material path; a compatible promotion retains its
    // existing texture owner. The reason names the exhausted resource.
    const double used_mb =
        static_cast<double>(mesh_bytes_used_) / (1024.0 * 1024.0);
    const double budget_mb =
        static_cast<double>(mesh_budget_bytes_) / (1024.0 * 1024.0);
    const double wanted_kb = static_cast<double>(wanted_bytes) / 1024.0;
    MATTER_LOGW("vt",
                 "WARNING: variant registration REJECTED (%s) -- "
                 "unadmitted rungs use their existing fallback. "
                 "variants=%u/%u, mesh=%.1f/%.1f "
                 "MiB, indirection=%.1f/%.1f MiB, wanted=%.1f KiB. Raise "
                 "MATTER_VT_MAX_VARIANTS / MATTER_VT_MESH_BUDGET_MB / "
                 "MATTER_VT_INDIRECTION_MB / MATTER_VT_POOL_PAGES as named. "
                 "Further rejections are counted in the VT census "
                 "(vt_rejected_variants) but not logged.\n",
                 reason, stats_.variants, max_variants_, used_mb, budget_mb,
                 static_cast<double>(tables_.used_words()) * 4.0 /
                     (1024.0 * 1024.0),
                 static_cast<double>(stats_.indirection_capacity_bytes) /
                     (1024.0 * 1024.0),
                 wanted_kb);
    std::fflush(stderr);
}

void VtResidency::refresh_indirection_stats() {
    if(occlusion_pages_) {
        occlusion_pages_->collect();
        stats_.occlusion_allocated_bytes=occlusion_pages_->allocated_bytes();
        stats_.occlusion_retained_pages=occlusion_pages_->retained_pages();
    }
    stats_.material_pages = material_pages_.used();
    stats_.shared_material_references = material_pages_.shared_references();
    stats_.material_read_pages = material_pages_.read_pages();
    stats_.indirection_used_bytes =
        static_cast<uint64_t>(tables_.used_words()) * 4u;
    stats_.tables_live = tables_.live_blocks();
    stats_.graveyard_tables = tables_.graveyard_blocks();
    stats_.graveyard_slots = slots_.graveyard_slots();
    stats_.graveyard_layers = static_cast<uint32_t>(layer_graveyard_.size());
}

bool VtResidency::context_storage_owned_for_test(
    uint32_t transport_slot) const {
    if (transport_slot == kVtNoSlot) return false;
    const uint32_t layer = transport_slot - 1u;
    if (layer >= variants_.size()) return false;
    const VariantRung& v = variants_[layer];
    return v.live && v.inputs && v.inputs->owns_context_inputs();
}

// Stage into an empty record before replacing a live mesh. Cold registration
// uses the same adoption path; all borrowed fields are repointed at owned data.
void VtResidency::copy_variant_mesh(VariantRung& v,
    const chart_atlas::ChartAtlasRung& atlas, const VtPartContext& context) {
    PROFILE_SCOPE("vt.mesh_copy");
    auto chosen = context;
    chosen.variant_hash = v.variant_hash;
    chosen.rung = v.rung;
    if (vt_context_has_surface_tape(context))
        chosen.surface_tape_hash = vt_page_content_salt(
            context.surface_tape_hash, vt_tape_gpu_enabled() ? 3u : 2u);
    v.inputs = VtPartSnapshot::capture(atlas, chosen);
    v.finest_texels_per_meter = 0;
    for (const auto& chart : atlas.charts)
        v.finest_texels_per_meter = std::max(v.finest_texels_per_meter, chart.texels_per_meter);
    v.mesh_bytes = vt_variant_mesh_bytes(atlas, context);
    PROFILE_COUNT("vt.mesh_copy_bytes", v.mesh_bytes);
}

// Page mappings, alias references and readiness stay with the owner. Any
// recorded request or running CPU job keeps its own immutable source snapshot.
void VtResidency::swap_variant_mesh(VariantRung& a, VariantRung& b) {
    std::swap(a.inputs, b.inputs);
    std::swap(a.mesh_bytes, b.mesh_bytes);
    std::swap(a.finest_texels_per_meter, b.finest_texels_per_meter);
}

// Implements the header's contract; two things about the IMPLEMENTATION are
// worth knowing before editing it.
//
// (1) The M6 fast paths come first and are documented inline: an exact re-
// registration is idempotent, a rung whose parameterisation already has a layer
// ALIASES it (taking a reference, spending no pages and no mesh budget), and a
// strictly FINER rung stages its own mesh and refreshes the existing owner.
//
// (2) The gates below run in a fixed order chosen so a rejection never leaves
// partial state: usable layout -> free layer slot -> CPU mesh budget ->
// indirection table block -> pinned tail page slot. Each of the last two rolls
// the previous one back if it fails (tables_.release_now is legal there because
// no frame has ever seen the allocation). NOTHING is mutated until all of them
// pass; only then is the caller's mesh ADOPTED (deep-copied, with v.inputs->context
// repointed at the copies), the tail page mapped and force-queued, and the GPU
// record written.
//
// The return value is the TRANSPORT slot — layer index + 1 — so that
// kVtNoSlot (0) can mean "this rung has no VT" everywhere downstream.
uint32_t VtResidency::register_variant(uint64_t variant_hash, uint32_t rung,
                                       const chart_atlas::ChartAtlasRung& atlas,
                                       const VtPartContext& context) {
    if (rung>=kMaterialModuleRung || context.periodic.version) return kVtNoSlot;
    return register_variant_impl(variant_hash,rung,atlas,context);
}

bool VtResidency::acquire_material_module(const std::shared_ptr<const VtPartSnapshot>& source,
    VtMaterialModuleLease& out, std::string& error) {
    if (!ready_ || !source || !source->owns_context_inputs() ||
        !vt_valid_periodic_domain(source->context.periodic) || !source->context.atlas ||
        source->context.atlas->atlas_w!=source->context.periodic.width ||
        source->context.atlas->atlas_h!=source->context.periodic.height ||
        source->context.vertex_count!=4 || source->context.triangle_count!=2) {
        error="material module requires a complete periodic producer and active residency";return false;
    }
    const auto hash=source->context.variant_hash;
    if (const auto found=modules_.find(hash); found!=modules_.end())
        if (auto shared=found->second.lock()) {
            out=std::move(shared);++stats_.module_reuses_total;error.clear();return true;
        }
    // Allocate lease/cache ownership before the registration takes resources.
    auto lease=std::shared_ptr<VtMaterialModule>(new VtMaterialModule);
    if (!module_owner_) module_owner_=std::make_shared<VtModuleOwner>(VtModuleOwner{this});
    modules_[hash]=lease;
    const uint32_t slot=register_variant_impl(hash,kMaterialModuleRung,
        *source->context.atlas,source->context);
    if (!slot) {modules_.erase(hash);error="material module registration was refused";return false;}
    lease->owner_=module_owner_;lease->hash_=hash;lease->slot_=slot;
    lease->generation_=variants_[slot-1].table_generation;
    ++stats_.module_variants;
    out=std::move(lease);error.clear();return true;
}

VtMaterialModuleBinding VtResidency::material_module_binding(const VtMaterialModuleLease& lease) const {
    if (!lease || !module_owner_ || lease->owner_.lock()!=module_owner_ ||
        !slot_active(lease->slot_)) return {};
    const auto& v=variants_[lease->slot_-1];
    if (v.rung!=kMaterialModuleRung || v.variant_hash!=lease->hash_ ||
        v.table_generation!=lease->generation_) return {};
    return {lease->slot_,uint32_t(lease->generation_)};
}

VtMaterialReadStatus VtResidency::acquire_material_read(const VtMaterialModuleLease& module,
    uint32_t mip, const VtMaterialReadBounds& bounds, VtMaterialReadLease& out, std::string& error) {
    out.reset();
    if (!ready_ || !module || !module_owner_ || module->owner_.lock() != module_owner_ ||
        !module->slot_ || module->slot_ > variants_.size()) {
        error = "material read requires a module from this active residency";
        return VtMaterialReadStatus::Invalid;
    }
    auto& v = variants_[module->slot_ - 1];
    if (!v.live || v.rung != kMaterialModuleRung || v.variant_hash != module->hash_ ||
        v.table_generation != module->generation_ || mip >= v.layout.mip_count) {
        error = "material read module generation or mip is invalid";
        return VtMaterialReadStatus::Invalid;
    }
    try {
        std::vector<std::array<uint32_t, 2>> tiles;
        if (!vt_material_read_tiles(v.inputs->context.periodic, mip, bounds, tiles, error))
            return VtMaterialReadStatus::Invalid;
        bool pending = !slot_active(module->slot_);
        for (const auto& tile : tiles) {
            const VtPageKey page{mip, tile[0], tile[1]};
            if (!v.indirection.is_mapped(mip, tile[0], tile[1])) {
                queue_page(v, page, false); pending = true; continue;
            }
            const auto slot = v.indirection.resolve(mip, tile[0], tile[1]).slot;
            if (dirty_pages_.count(slot) || slot_content_revisions_[slot] != v.content_revision ||
                material_pages_.slot(slot) == UINT32_MAX || slot_page_metadata_[slot].height.version != 1) {
                queue_page(v, page, true, slot); pending = true;
            }
        }
        if (pending) {
            error = "material read is waiting for exact-mip base pages";
            refresh_queue_stats();
            return VtMaterialReadStatus::Pending;
        }
        auto read = std::shared_ptr<VtMaterialRead>(new VtMaterialRead);
        read->module_ = module; read->inputs_ = v.inputs; read->snapshot_ = input_snapshot_;
        read->revision_ = v.content_revision; read->mip_ = mip;
        read->pages_.reserve(tiles.size()); read->reads_.reserve(tiles.size());
        for (const auto& tile : tiles) {
            const auto receiver = v.indirection.resolve(mip, tile[0], tile[1]).slot;
            auto pixels = material_pages_.retain_read(receiver);
            if (!pixels) { error = "material read lost its pixel allocation"; return VtMaterialReadStatus::Pending; }
            pixels->retain_until(frame_index_ + kVtRetireHorizonFrames);
            read->pages_.push_back({tile[0], tile[1], pixels->slot(), slot_page_metadata_[receiver].height});
            read->reads_.push_back(std::move(pixels));
        }
        out = std::move(read);
        refresh_indirection_stats();
        error.clear(); return VtMaterialReadStatus::Ready;
    } catch (const std::bad_alloc&) {
        error = "material read dependency allocation deferred";
        return VtMaterialReadStatus::Pending;
    }
}

bool VtResidency::material_read_current(const VtMaterialReadLease& read) const {
    if (!read || !material_module_binding(read->module_).slot || read->snapshot_ != input_snapshot_) return false;
    const auto& v = variants_[read->module_->slot_ - 1];
    return v.inputs == read->inputs_ && v.content_revision == read->revision_;
}

bool VtResidency::retain_material_read(const VtMaterialReadLease& read) {
    if (!material_read_current(read)) return false;
    for (const auto& pixels : read->reads_) pixels->retain_until(frame_index_ + kVtRetireHorizonFrames);
    return true;
}

void VtResidency::release_material_module(const VtMaterialModule& lease) {
    modules_.erase(lease.hash_);
    if (!ready_ || !lease.slot_ || lease.slot_>variants_.size()) return;
    const auto& v=variants_[lease.slot_-1];
    if (!v.live || v.rung!=kMaterialModuleRung || v.variant_hash!=lease.hash_ ||
        v.table_generation!=lease.generation_) return;
    release_rung_alias(variant_key(lease.hash_,kMaterialModuleRung));
    if (stats_.module_variants)--stats_.module_variants;
    stats_.pool_used=slots_.used();stats_.pool_pinned=slots_.pinned();
    stats_.queue_depth=uint32_t(queue_.size());
}

uint32_t VtResidency::register_variant_impl(uint64_t variant_hash, uint32_t rung,
    const chart_atlas::ChartAtlasRung& atlas, const VtPartContext& context) {
    if (!ready_) return kVtNoSlot;
    if (atlas.charts.empty() || atlas.atlas_w == 0 || atlas.atlas_h == 0)
        return kVtNoSlot;
    // M6: the layer is keyed by the PARAMETERISATION, not by the rung. Rungs
    // of one part that share a chart table therefore share a layer — which is
    // the whole point: the pages stop being re-fetched when the rung switches.
    const uint64_t key = variant_key(variant_hash, chart_atlas::parameterisation_id(atlas) ^
        (rung==kMaterialModuleRung ? kMaterialModuleParameterisation : 0));
    const uint64_t alias = variant_key(variant_hash, rung);

    // This rung already resolves somewhere. Same layer: idempotent, hand it
    // back. DIFFERENT layer: the part re-baked into another parameterisation
    // under the same (hash, rung), so drop the old alias first or its refcount
    // leaks and the old layer is never reclaimed.
    if (const auto prior = param_key_of_rung_.find(alias);
        prior != param_key_of_rung_.end()) {
        if (prior->second == key) {
            const auto same = layer_of_.find(key);
            if (same != layer_of_.end()) return same->second + 1u;
        }
        release_rung_alias(alias);
    }

    const auto found = layer_of_.find(key);
    if (found != layer_of_.end()) {
        VariantRung& existing = variants_[found->second];
        if (rung < existing.rung) {
            // Keep the owner and its valid pages while preparing the finer
            // mesh. Account for old + staged CPU storage at peak; a rejected
            // promotion must change neither aliases nor displayed coverage.
            const size_t bytes = vt_variant_mesh_bytes(atlas, context);
            if (bytes > mesh_budget_bytes_ - std::min(mesh_bytes_used_, mesh_budget_bytes_)) {
                note_rejection("CPU mesh staging budget spent; retained compatible owner", bytes);
                return kVtNoSlot;
            }
            VariantRung prepared;
            prepared.variant_hash = variant_hash;
            prepared.rung = rung;
            try {
                copy_variant_mesh(prepared, atlas, context);
            } catch (const std::bad_alloc&) {
                note_rejection("CPU mesh staging allocation failed; retained compatible owner", bytes);
                return kVtNoSlot;
            }
            const VtPreparationKey old_preparation{existing.variant_hash, existing.rung,
                existing.param_key, existing.table_generation};
            if (filler_) filler_->release_preparation(old_preparation);
            if (enricher_) enricher_->release_preparation(old_preparation);
            remove_material_dependencies(existing);
            const size_t old_bytes = existing.mesh_bytes;
            swap_variant_mesh(existing, prepared);
            existing.rung = rung;
            mesh_bytes_used_ = mesh_bytes_used_ - old_bytes + existing.mesh_bytes;
            stats_.mesh_bytes = mesh_bytes_used_;
            rebuild_material_dependencies(existing);
            // Old pages remain valid until the candidate path publishes the
            // new revision. This also supersedes previously queued/recorded
            // coarse fills and prevents enrichment of dirty old content.
            invalidate_owners({existing.layer + 1u}, VtInvalidationReason::Geometry);
            ++stats_.finer_rebuilds_total;
        }
        // Coarser rungs reuse the canonical mesh without spending CPU/page
        // capacity. A finer rung joins the same alias set after promotion.
        param_key_of_rung_[alias] = key;
        ++existing.alias_refs;
        ++stats_.shared_refs_total;
        return found->second + 1u;
    }

    VtVariantLayout layout{};
    if (!vt_build_layout(atlas.atlas_w, atlas.atlas_h, layout))
        return kVtNoSlot;
    if (free_layers_.empty()) {
        // Either MATTER_VT_MAX_VARIANTS registrations are genuinely live, or
        // freed slots are still ageing in the retirement graveyard (a burst of
        // releases within the last kVtRetireHorizonFrames). Both fail closed;
        // the demand pass retries and the graveyard drains within 8 frames.
        note_rejection("no free variant slot (MATTER_VT_MAX_VARIANTS or "
                       "retirement backlog)", 0);
        return kVtNoSlot;
    }

    // Budget the CPU mesh copy BEFORE taking anything, so a rejection leaves
    // no partial registration behind.
    const size_t mesh_bytes = vt_variant_mesh_bytes(atlas, context);
    if (mesh_bytes_used_ + mesh_bytes > mesh_budget_bytes_) {
        note_rejection("CPU mesh budget spent", mesh_bytes);
        return kVtNoSlot;
    }

    // Exact-sized indirection table block (see the header's formula note).
    uint32_t table_offset = 0, table_block = 0;
    uint64_t table_generation = 0;
    if (!tables_.acquire(layout.entry_count, frame_index_, table_offset,
                         table_block, table_generation)) {
        note_rejection("indirection table arena spent "
                       "(raise MATTER_VT_INDIRECTION_MB)", 0);
        return kVtNoSlot;
    }

    // The tail page is pinned for the variant's whole life: it is what makes
    // "every loaded variant always has valid texels" true.
    uint32_t tail_slot = 0;
    VtSlotPool::Owner evicted;
    const VtPageKey tail_page{layout.mip_count - 1u, 0u, 0u};
    if (!slots_.acquire(key, tail_page, /*pinned=*/true, frame_index_,
                        tail_slot, evicted)) {
        // Every evictable page slot is pinned or protected by this frame's
        // hysteresis window: the pool cannot admit one more variant right now.
        // Roll the table block back (release_now is legal — no frame ever saw
        // this allocation) and fail closed like the other gates.
        tables_.release_now(table_offset, table_block);
        note_rejection("page pool exhausted by pinned tails "
                       "(raise MATTER_VT_POOL_PAGES)", 0);
        return kVtNoSlot;
    }
    if (evicted.live) {
        // The pool was full of unpinned pages; recycle exactly as record_frame
        // does. Every side table keyed by slot must be retired here too, or
        // the new owner inherits the old owner's input snapshot, geometry,
        // occlusion factor and material mapping -- and their GPU metadata
        // addresses -- until its own tail happens to fill.
        dirty_pages_.erase(tail_slot);
        material_pages_.release(tail_slot);
        retire_slot_input_snapshot(tail_slot);
        retire_slot_geometry(tail_slot);
        retire_slot_occlusion(tail_slot);
        retire_slot_material_mapping(tail_slot);
        if (event_log_)
            MATTER_LOGI("vt-evict", "frame=%llu owner=%016llx mip=%u x=%u y=%u slot=%u reason=tail",
                static_cast<unsigned long long>(frame_index_),
                static_cast<unsigned long long>(evicted.variant_key),
                evicted.page.mip, evicted.page.px, evicted.page.py, tail_slot);
        const auto owner_layer = layer_of_.find(evicted.variant_key);
        if (owner_layer != layer_of_.end())
            variants_[owner_layer->second].indirection.unmap(
                evicted.page.mip, evicted.page.px, evicted.page.py);
    }
    // The slot's previous content (and any tier-2 candidacy for it) is gone.
    slot_reset_tier(tail_slot);

    const uint32_t layer = free_layers_.back();
    free_layers_.pop_back();
    if (layer >= variants_.size()) variants_.resize(layer + 1u);
    VariantRung& v = variants_[layer];
    v = VariantRung{};
    v.variant_hash = variant_hash;
    v.rung = rung;
    v.layer = layer;
    // M6: the layer's own identity, and its first referencing rung. Every
    // later site that needs "which key owns this slot" reads param_key rather
    // than recomputing variant_key(hash, rung) — under unification that
    // recomputation names a key this layer is NOT registered under.
    v.param_key = key;
    v.alias_refs = 1;
    param_key_of_rung_[alias] = key;
    v.layout = layout;
    v.table_offset_words = table_offset;
    v.table_block_words = table_block;
    v.table_generation = table_generation;
    v.table_uploaded = false;
    copy_variant_mesh(v, atlas, context);
    mesh_bytes_used_ += mesh_bytes;
    v.tail_slot = tail_slot;
    v.tail_filled = false;
    v.live = true;
    rebuild_material_dependencies(v);
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
    stats_.lru_scan_count = slots_.lru_scan_count();
    stats_.lru_scan_ns = slots_.lru_scan_ns();
    stats_.queue_depth = static_cast<uint32_t>(queue_.size());
    stats_.mesh_bytes = mesh_bytes_used_;
    refresh_indirection_stats();
    return layer + 1u;
}

// Tears down ONE LAYER, named by its parameterisation key. This is the bottom
// half: callers hold (hash, rung) aliases and must come through
// release_rung_alias() below, which refcounts them — calling this directly frees
// a layer other live rungs may still be drawing through.
//
// Returns false when the key names no live layer, which callers use to decide
// whether the stats they refresh could have changed.
bool VtResidency::release_variant_key(uint64_t key) {
    const auto found = layer_of_.find(key);
    if (found == layer_of_.end()) return false;
    const uint32_t layer = found->second;
    VariantRung& v = variants_[layer];
    if (event_log_)
        MATTER_LOGI("vt-release", "frame=%llu owner=%016llx generation=%llu part=%016llx rung=%u slot=%u",
            static_cast<unsigned long long>(frame_index_),
            static_cast<unsigned long long>(v.param_key),
            static_cast<unsigned long long>(v.table_generation),
            static_cast<unsigned long long>(v.variant_hash), v.rung, layer + 1u);
    // Everything an in-flight frame's draw records could still resolve
    // through — the variant slot (and its GPU record), the indirection table
    // block, every page slot — ages in the graveyard until this serial. The
    // Recorded GPU work has already staged its CPU inputs. A preparation job
    // may still retain an immutable snapshot after the owner is released.
    const uint64_t retire = frame_index_ + kVtRetireHorizonFrames;
    for (uint32_t slot = 0; slot < slots_.capacity(); ++slot) {
        const VtSlotPool::Owner& o = slots_.owner(slot);
        if (o.live && o.variant_key == key) {
            dirty_pages_.erase(slot);
            slot_reset_tier(slot);
            // Keep the old GPU id, like the old page and variant record,
            // until this slot is reused. Retain its binding through the same
            // reader horizon even though CPU ownership ends now.
            retire_slot_input_snapshot(slot);
            retire_slot_geometry(slot);
            retire_slot_material_mapping(slot);
            material_pages_.release(slot, retire);
            slots_.release(slot, retire);
        }
    }
    // Erasing shifts every later entry. Rebuild surviving indices before a
    // registration/forced refresh can use dedup again, not just at frame drain.
    const size_t previous_queue_size = queue_.size();
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
        [layer](const PendingFill& pending) { return pending.layer == layer; }),
        queue_.end());
    if (queue_.size() != previous_queue_size) reindex_pending_fills();
    mesh_bytes_used_ -= std::min(mesh_bytes_used_, v.mesh_bytes);
    tables_.release(v.table_offset_words, v.table_block_words, retire);
    remove_material_dependencies(v);
    // Only the last alias (or an incompatible owner replacement) reaches here.
    // Producers retire their captured GPU resources independently of CPU mesh
    // storage; notify before losing the canonical context and generation.
    const VtPreparationKey preparation{v.variant_hash, v.rung, v.param_key, v.table_generation};
    if (filler_) filler_->release_preparation(preparation);
    if (enricher_) enricher_->release_preparation(preparation);
    v = VariantRung{};
    stats_.mesh_bytes = mesh_bytes_used_;
    // The GPU record is deliberately NOT cleared here: an in-flight frame that
    // still names this slot must keep resolving a valid (record, table, page)
    // triple. begin_frame's collect zeroes it when the horizon passes.
    layer_graveyard_.push_back(LayerGrave{layer, retire});
    if (debug_generations_) debug_layer_reuse_[layer] = retire;
    layer_of_.erase(found);
    if (stats_.variants) --stats_.variants;
    stats_.dirty_pages = static_cast<uint32_t>(dirty_pages_.size());
    refresh_indirection_stats();
    return true;
}

// M6: drop ONE (hash, rung) reference. The layer itself is torn down only when
// the last rung referencing it goes — see VariantRung::alias_refs. Returns true
// when the layer was actually released, so the callers below know whether the
// stats they refresh could have changed.
bool VtResidency::release_rung_alias(uint64_t alias) {
    const auto it = param_key_of_rung_.find(alias);
    if (it == param_key_of_rung_.end()) return false;
    const uint64_t key = it->second;
    param_key_of_rung_.erase(it);

    const auto found = layer_of_.find(key);
    if (found == layer_of_.end()) return false;   // already gone; alias was stale
    VariantRung& v = variants_[found->second];
    if (v.alias_refs > 1) {
        --v.alias_refs;
        return false;      // other rungs still draw through this layer
    }
    return release_variant_key(key);
}

// Releases every rung of one part. The 0..31 sweep is not arbitrary: 32 is the
// width of the renderer's chart-rung mask (viewer::kVkMaxChartRung), so a rung
// numbered past it could never have been registered in the first place.
void VtResidency::release_variant(uint64_t variant_hash) {
    if (!ready_) return;
    // Walks rung aliases, NOT layer keys. Under M6 a part's layer is keyed by
    // its parameterisation, so variant_key(hash, rung) is no longer a key in
    // layer_of_ at all — this loop used to find them directly and would now
    // free nothing, silently leaking every variant of every released part.
    for (uint32_t rung = 0; rung < 32u; ++rung)
        release_rung_alias(variant_key(variant_hash, rung));
    stats_.pool_used = slots_.used();
    stats_.pool_pinned = slots_.pinned();
    stats_.queue_depth = static_cast<uint32_t>(queue_.size());
}

void VtResidency::release_variant(uint64_t variant_hash, uint32_t rung) {
    if (!ready_ || rung>=kMaterialModuleRung) return;
    if (!release_rung_alias(variant_key(variant_hash, rung))) return;
    stats_.pool_used = slots_.used();
    stats_.pool_pinned = slots_.pinned();
    stats_.queue_depth = static_cast<uint32_t>(queue_.size());
}

void VtResidency::remove_material_dependencies(VariantRung& v) {
    for (uint32_t id : v.material_dependencies) {
        const auto found = material_dependents_.find(id);
        if (found == material_dependents_.end()) continue;
        found->second.erase(v.layer);
        if (found->second.empty()) material_dependents_.erase(found);
    }
    v.material_dependencies.clear();
}

void VtResidency::rebuild_material_dependencies(VariantRung& v) {
    remove_material_dependencies(v);
    // Every material channel in a module comes from its immutable direct
    // source program and projected payloads, independent of scalar materials.
    if (v.inputs->context.periodic.version) return;
    bool used[256]{};
    if (v.inputs->context.surface_material_count > 0 &&
        v.inputs->context.surface_material_count <= 8 &&
        v.inputs->context.surface_weights && v.inputs->context.surface_materials) {
        for (uint32_t id : v.inputs->surface->materials)
            used[id & 0xFFu] = true;
    }
    {
        // Even tape-classified surfaces can fall back to the triangle
        // material: vt_top2_select does so when all evaluated weights are
        // zero. Keep that dependency alongside the declared tape palette.
        // Match the chart-stream builder's first-corner material selection,
        // fallback and 8-bit transport. Unreferenced vertices are not inputs.
        for (const auto& chart : v.inputs->geometry->atlas.charts) {
            const size_t end = std::min(v.inputs->geometry->atlas.tri_order.size(),
                static_cast<size_t>(chart.first_tri) + chart.tri_count);
            for (size_t i = chart.first_tri; i < end; ++i) {
                const uint32_t tri = v.inputs->geometry->atlas.tri_order[i];
                if (tri >= v.inputs->context.triangle_count) continue;
                const size_t index = static_cast<size_t>(tri) * 3u;
                if (index + 2 >= v.inputs->geometry->indices.size()) continue;
                const uint32_t c0 = v.inputs->geometry->indices[index];
                if (c0 >= v.inputs->context.vertex_count ||
                    v.inputs->geometry->indices[index + 1] >= v.inputs->context.vertex_count ||
                    v.inputs->geometry->indices[index + 2] >= v.inputs->context.vertex_count) continue;
                uint32_t id = v.inputs->geometry->material_ids.empty()
                    ? v.inputs->context.dominant_material : v.inputs->geometry->material_ids[c0];
                if (id == 0xFFFFFFFFu)
                    id = v.inputs->context.dominant_material == 0xFFFFFFFFu
                        ? 0u : v.inputs->context.dominant_material;
                used[id & 0xFFu] = true;
            }
        }
    }
    for (uint32_t id = 0; id < 256; ++id) {
        if (!used[id]) continue;
        v.material_dependencies.push_back(id);
        material_dependents_[id].insert(v.layer);
    }
}

uint32_t VtResidency::invalidate_material_content(
    const std::vector<uint32_t>& material_ids, VtInvalidationReason reason) {
    std::set<uint32_t> owners;
    for (uint32_t id : material_ids) {
        const auto found = material_dependents_.find(id);
        if (found == material_dependents_.end()) continue;
        for (uint32_t layer : found->second) owners.insert(layer + 1u);
    }
    return invalidate_owners(std::vector<uint32_t>(owners.begin(), owners.end()), reason);
}

uint32_t VtResidency::invalidate_all_content() {
    std::vector<uint32_t> owners;
    for (const VariantRung& v : variants_)
        if (v.live) owners.push_back(v.layer + 1u);
    return invalidate_owners(owners);
}

uint32_t VtResidency::invalidate_owners(
    const std::vector<uint32_t>& transport_slots, VtInvalidationReason reason) {
    if (!ready_ || transport_slots.empty()) return 0;
    std::set<uint64_t> keys;
    std::vector<uint32_t> layers;
    for (uint32_t slot : transport_slots) {
        if (slot == kVtNoSlot || slot > variants_.size()) continue;
        const VariantRung& v = variants_[slot - 1u];
        if (v.live && v.layout.valid() && keys.insert(v.param_key).second)
            layers.push_back(v.layer);
    }
    if (layers.empty()) return 0;

    for (uint32_t layer : layers) {
        VariantRung& v = variants_[layer];
        ++v.content_revision;
        if (event_log_)
            MATTER_LOGI("vt-dirty", "frame=%llu owner=%016llx generation=%llu revision=%llu reason=%u",
                static_cast<unsigned long long>(frame_index_),
                static_cast<unsigned long long>(v.param_key),
                static_cast<unsigned long long>(v.table_generation),
                static_cast<unsigned long long>(v.content_revision), static_cast<unsigned>(reason));
        const VtPageKey tail{v.layout.mip_count - 1u, 0u, 0u};
        queue_page(v, tail, /*force=*/true, v.tail_slot);
        const auto queued = queued_keys_.find(page_key(v.layer, tail));
        if (queued != queued_keys_.end()) queue_[queued->second].priority = kVtMaxMips;
    }
    // Keep old bytes, mappings and readiness. The producer writes candidates;
    // record_frame copies successful, current revisions into these slots only
    // after prior readers. A capped execution queue cannot lose this dirtiness.
    uint32_t detail_pages = 0;
    for (uint32_t slot = 0; slot < slots_.capacity(); ++slot) {
        const auto& owner = slots_.owner(slot);
        if (!owner.live || keys.find(owner.variant_key) == keys.end()) continue;
        const auto found = layer_of_.find(owner.variant_key);
        if (found == layer_of_.end()) continue;
        const auto& v = variants_[found->second];
        auto [entry, inserted] = dirty_pages_.try_emplace(slot);
        const uint64_t first = inserted ? frame_index_ : entry->second.requested_frame;
        entry->second = PendingFill{v.layer, owner.page, kVtMaxMips, first,
            owner.pinned ? slot : UINT32_MAX, v.table_generation, v.content_revision};
        if (!owner.pinned) ++detail_pages;
    }
    ++stats_.invalidations_total;
    stats_.dirty_pages = static_cast<uint32_t>(dirty_pages_.size());
    stats_.pool_used = slots_.used();
    stats_.pool_pinned = slots_.pinned();
    stats_.queue_depth = static_cast<uint32_t>(queue_.size());
    return detail_pages;
}

uint32_t VtResidency::slot_for(uint64_t variant_hash, uint32_t rung) const {
    // M6: two hops. The rung names an alias; the alias names the
    // parameterisation key; that key owns the layer. Looking the rung up in
    // layer_of_ directly would miss every unified part.
    const auto alias = param_key_of_rung_.find(variant_key(variant_hash, rung));
    if (alias == param_key_of_rung_.end()) return kVtNoSlot;
    const auto found = layer_of_.find(alias->second);
    return found == layer_of_.end() ? kVtNoSlot : found->second + 1u;
}

uint32_t VtResidency::compatible_owner_slot(uint64_t variant_hash,
    const chart_atlas::ChartAtlasRung& atlas) const {
    const auto found = layer_of_.find(
        variant_key(variant_hash, chart_atlas::parameterisation_id(atlas)));
    return found == layer_of_.end() ? kVtNoSlot : found->second + 1u;
}

uint32_t VtResidency::canonical_rung_for_slot(uint32_t slot) const {
    if (!slot || slot > variants_.size() || !variants_[slot - 1u].live) return UINT32_MAX;
    return variants_[slot - 1u].rung;
}

// Swaps one registered rung's tape classification in place (the header carries
// the caller contract and the content invalidation owed after its edit
// bracket). Two implementation details:
//
//  - Every validity check runs BEFORE any mutation, so a call with mismatched
//    sizes returns false with the old classification completely intact.
//  - The mesh-byte accounting is applied as a DIFFERENCE against the bytes this
//    variant previously held, on both the per-variant and the pool-wide total.
//    Repeated updates therefore cannot inflate mesh_bytes_used_ and slowly
//    starve registration.
//
// `strip` (no materials, null arrays, or an implausible material count) is a
// normal outcome, not an error: the rung reverts to the TriEx materialId path.
bool VtResidency::update_variant_surface(uint64_t variant_hash, uint32_t rung,
                                         const uint8_t* weights,
                                         size_t weight_bytes,
                                         const uint32_t* materials,
                                         uint32_t material_count,
                                         uint64_t tape_hash,
                                         const char* tape_text,
                                         const uint16_t* lanes,
                                         uint32_t lane_count,
                                         bool* content_changed,
                                         const float* local_to_world,
                                         uint32_t world_anchored) {
    if (content_changed) *content_changed = false;
    if (!ready_ || rung>=kMaterialModuleRung) return false;
    const auto alias = param_key_of_rung_.find(variant_key(variant_hash, rung));
    if (alias == param_key_of_rung_.end()) return false;
    const auto found = layer_of_.find(alias->second);
    if (found == layer_of_.end()) return false;
    VariantRung& v = variants_[found->second];
    if (!v.live) return false;

    const auto& current = v.inputs->context;
    const auto& surface = *v.inputs->surface;
    const size_t old_bytes = surface.bytes();
    if (local_to_world) {
        if (world_anchored > 1) return false;
        for (unsigned i=0;i<12;++i) if (!std::isfinite(local_to_world[i])) return false;
    }
    const bool same_frame = !local_to_world ||
        (world_anchored == current.surface_world_anchored &&
         std::memcmp(local_to_world,current.surface_local_to_world,sizeof(current.surface_local_to_world))==0);

    const bool strip = material_count == 0 || weights == nullptr ||
                       materials == nullptr || material_count > 8u;
    const size_t expected =
        static_cast<size_t>(current.vertex_count) * material_count;
    if (!strip && weight_bytes != expected) return false;
    if (!strip && lanes != nullptr && lane_count > 8u) return false;

    const uint64_t desired_hash = strip ? 0 : vt_page_content_salt(
        tape_hash, vt_tape_gpu_enabled() ? 3u : 2u);
    const size_t desired_lane_count = !strip && lanes && lane_count
        ? static_cast<size_t>(current.vertex_count) * lane_count : 0;
    const auto matches = [](const auto& stored, const auto* data, size_t count) {
        return stored.size() == count &&
            (count == 0 || std::memcmp(stored.data(), data,
                count * sizeof(stored[0])) == 0);
    };
    if ((strip && current.surface_material_count == 0) ||
        (!strip && same_frame && current.surface_tape_hash == desired_hash &&
         matches(surface.weights, weights, weight_bytes) &&
         matches(surface.materials, materials, material_count) &&
         matches(surface.lanes, lanes, desired_lane_count) &&
         current.surface_lane_count == (desired_lane_count ? lane_count : 0) &&
         surface.tape_text == (tape_text ? tape_text : "") &&
         surface.has_tape_text == (tape_text != nullptr)))
        return true;

    auto updated = current;
    if (local_to_world) {
        std::memcpy(updated.surface_local_to_world,local_to_world,sizeof(updated.surface_local_to_world));
        updated.surface_world_anchored=world_anchored;
    }
    updated.surface_weights = strip ? nullptr : weights;
    updated.surface_materials = strip ? nullptr : materials;
    updated.surface_material_count = strip ? 0 : material_count;
    updated.surface_tape_hash = desired_hash;
    updated.surface_tape_text = strip ? nullptr : tape_text;
    updated.surface_lanes = desired_lane_count ? lanes : nullptr;
    updated.surface_lane_count = desired_lane_count ? lane_count : 0;
    auto next = v.inputs->with_surface(updated);
    const size_t new_bytes = next->surface->bytes();
    v.inputs = std::move(next);
    v.mesh_bytes = v.mesh_bytes - std::min(v.mesh_bytes, old_bytes) + new_bytes;
    mesh_bytes_used_ =
        mesh_bytes_used_ - std::min(mesh_bytes_used_, old_bytes) + new_bytes;
    stats_.mesh_bytes = mesh_bytes_used_;
    rebuild_material_dependencies(v);
    if (filler_) filler_->invalidate_surface(
        {v.variant_hash, v.rung, v.param_key, v.table_generation});
    if (content_changed) *content_changed = true;
    return true;
}

bool VtResidency::update_variant_finite_sources(uint64_t variant_hash,uint32_t rung,
    std::shared_ptr<const VtFiniteSources> sources,const uint32_t* ids,size_t id_count,bool* content_changed) {
    if (content_changed) *content_changed=false;
    if (!ready_ || rung>=kMaterialModuleRung) return false;
    const auto alias=param_key_of_rung_.find(variant_key(variant_hash,rung));
    if (alias==param_key_of_rung_.end()) return false;
    const auto found=layer_of_.find(alias->second);
    if (found==layer_of_.end()) return false;
    auto &v=variants_[found->second]; if (!v.live) return false;
    const auto &current=v.inputs->context;const auto &surface=*v.inputs->surface;
    if (sources ? (!ids || id_count!=current.vertex_count || sources->bindings.empty()) : (ids || id_count)) return false;
    for (size_t i=0;i<id_count;++i) if (ids[i]>sources->bindings.size()) return false;
    const bool same_catalog=(!sources && !surface.finite_sources) ||
        (sources && surface.finite_sources && sources->content_hash==surface.finite_sources->content_hash);
    if (same_catalog && surface.finite_source_ids.size()==id_count &&
        (!id_count || std::memcmp(surface.finite_source_ids.data(),ids,id_count*sizeof(uint32_t))==0)) return true;
    const size_t old_bytes=surface.bytes();
    auto updated=current;updated.finite_sources=std::move(sources);updated.finite_source_ids=ids;
    auto next=v.inputs->with_surface(updated);const size_t new_bytes=next->surface->bytes();
    v.inputs=std::move(next);
    v.mesh_bytes=v.mesh_bytes-std::min(v.mesh_bytes,old_bytes)+new_bytes;
    mesh_bytes_used_=mesh_bytes_used_-std::min(mesh_bytes_used_,old_bytes)+new_bytes;
    stats_.mesh_bytes=mesh_bytes_used_;
    if (filler_) filler_->invalidate_surface({v.variant_hash,v.rung,v.param_key,v.table_generation});
    if (content_changed) *content_changed=true;
    return true;
}

// Rebuilds one layer's GPU-visible record (the struct vt_common.glsl's
// VtVariantRecord mirrors) from its layout and table block, and marks the whole
// record buffer dirty — record_frame re-memcpys it wholesale, which is cheaper
// than tracking sub-ranges of a small host-visible buffer.
//
// This is also the audit point for record reuse under
// MATTER_VT_DEBUG_GENERATIONS: rewriting a record before its previous owner's
// retire serial IS the stale-mapping bug (an in-flight frame's draw records
// would resolve the old variant through the new variant's record), so the audit
// aborts rather than reporting.
void VtResidency::write_variant_record(const VariantRung& v) {
    if (debug_generations_) {
        // A record may only be (re)written for a slot whose previous owner has
        // fully retired — mutating it earlier is exactly the stale-mapping bug
        // (an in-flight frame's draw records would resolve the OLD variant
        // through the NEW variant's record).
        const auto grave = debug_layer_reuse_.find(v.layer);
        if (grave != debug_layer_reuse_.end()) {
            if (frame_index_ < grave->second)
                generation_audit_fail(
                    "variant records",
                    "record rewritten before its retire serial");
            debug_layer_reuse_.erase(grave);
        }
    }
    VariantRecordGpu& r = variant_records_[v.layer];
    r = VariantRecordGpu{};
    r.atlas_w = v.layout.atlas_w;
    r.atlas_h = v.layout.atlas_h;
    r.mip_count = v.layout.mip_count;
    r.flags = 1u;
    for (uint32_t m = 0; m < 8; ++m)
        r.mip_offset[m] = v.layout.mip_offset[m];
    for (uint32_t m = 8; m < kVtMaxMips; ++m)
        r.mip_offset_high[m - 8] = v.layout.mip_offset[m];
    r.table_offset = v.table_offset_words;
    r.pages_w = v.layout.page_w[0];
    r.pages_h = v.layout.page_h[0];
    r.generation = static_cast<uint32_t>(v.table_generation & 0xFFFFFFFFu);
    variant_records_dirty_ = true;
}

// ---------------------------------------------------------------------------
// Fill queue
// ---------------------------------------------------------------------------

bool VtResidency::queued_requests_consistent_for_test() const {
    if (queued_keys_.size() != queue_.size()) return false;
    for (size_t i = 0; i < queue_.size(); ++i) {
        const PendingFill& pending = queue_[i];
        const auto found = queued_keys_.find(page_key(pending.layer, pending.page));
        if (found == queued_keys_.end() || found->second != i ||
            pending.layer >= variants_.size()) return false;
        const VariantRung& variant = variants_[pending.layer];
        if (!variant.live || !variant.indirection.in_range(
                pending.page.mip, pending.page.px, pending.page.py)) return false;
        if (pending.preassigned_slot != 0xFFFFFFFFu &&
            pending.preassigned_slot != variant.tail_slot) return false;
    }
    return true;
}

void VtResidency::reindex_pending_fills() {
    queued_keys_.clear();
    for (size_t i = 0; i < queue_.size(); ++i)
        queued_keys_[page_key(queue_[i].layer, queue_[i].page)] = i;
}

void VtResidency::refresh_queue_stats() {
    stats_.dirty_pages = static_cast<uint32_t>(dirty_pages_.size());
    stats_.queue_depth = static_cast<uint32_t>(queue_.size());
    stats_.mandatory_queue_depth = stats_.detail_queue_depth = 0;
    stats_.oldest_mandatory_age_frames = stats_.oldest_detail_age_frames = 0;
    for (const PendingFill& pending : queue_) {
        const uint64_t age = frame_index_ - std::min(frame_index_, pending.requested_frame);
        if (pending.preassigned_slot != 0xFFFFFFFFu) {
            ++stats_.mandatory_queue_depth;
            stats_.oldest_mandatory_age_frames = std::max(stats_.oldest_mandatory_age_frames, age);
        } else {
            ++stats_.detail_queue_depth;
            stats_.oldest_detail_age_frames = std::max(stats_.oldest_detail_age_frames, age);
        }
    }
}

// Request one page of one variant. Not necessarily a queue push:
//
//  - An already-resident page just TOUCHES its slot and returns. The touch is
//    load-bearing twice over — it keeps the slot warm for the LRU and it arms
//    this frame's eviction hysteresis, so a page requested this frame can never
//    be this frame's eviction victim.
//  - `force` bypasses that fast-out. It is how a pinned tail gets filled at all:
//    a tail is mapped from the moment it is registered (that mapping is what
//    makes every unmapped entry resolvable), so without `force` it would keep
//    whatever undefined bytes its slot held.
//  - `preassigned_slot` marks a TAIL fill — rewrite this slot in place instead
//    of acquiring one. record_frame budgets tail fills separately from
//    feedback-driven page fills for exactly this reason.
//
// Duplicate requests coalesce onto the existing entry, keeping the HIGHER
// priority. Priority is how many mips coarser the currently-served page is, so
// a page whose only coverage is the variant's tail is the most starved and wins.
void VtResidency::queue_page(VariantRung& v, VtPageKey page, bool force,
                             uint32_t preassigned_slot) {
    if (!v.live || !v.indirection.in_range(page.mip, page.px, page.py)) return;
    const VtEntry served = v.indirection.resolve(page.mip, page.px, page.py);
    // Registration explicitly maps the pinned tail, and all other entries
    // resolve either to their own resident page or to a strictly coarser mip.
    // The shader's existing table therefore answers exact residency too; a
    // second tree lookup for every visible resident page is unnecessary.
    if (!force && served.mapped_mip == page.mip) {
        // Already resident; keep its slot warm. The touch also arms this
        // frame's eviction hysteresis: a page the current frame requested is
        // never this frame's eviction victim.
        slots_.touch(served.slot, frame_index_);
        return;
    }
    const uint64_t k = page_key(v.layer, page);
    const auto found = queued_keys_.find(k);
    // Priority: how many mips coarser the currently-served page is. A page
    // whose only coverage is the tail is the most starved, so it wins.
    const uint32_t priority = force ? kVtMaxMips : served.mapped_mip > page.mip
                                  ? served.mapped_mip - page.mip
                                  : 0u;
    // The page currently serving this request is wanted by definition — keep
    // it warm too, or the fill for a finer mip could evict the very coverage
    // it is refining (the coarse page still serves every OTHER texel of its
    // region until the fine page lands).
    slots_.touch(served.slot, frame_index_);
    if (found != queued_keys_.end()) {
        PendingFill& existing = queue_[found->second];
        if (priority > existing.priority) existing.priority = priority;
        // Keep the first request's age. Repeated feedback must not make a
        // waiting page appear newly requested forever.
        if (preassigned_slot != 0xFFFFFFFFu)
            existing.preassigned_slot = preassigned_slot;
        existing.owner_generation = v.table_generation;
        existing.content_revision = v.content_revision;
        return;
    }
    queued_keys_[k] = queue_.size();
    queue_.push_back(
        PendingFill{v.layer, page, priority, frame_index_, preassigned_slot,
                    v.table_generation, v.content_revision});
}

void VtResidency::queue_dirty_pages() {
    uint32_t detail_queued = 0;
    const uint32_t limit = std::max(max_queue_, max_fills_per_frame_);
    for (auto it = dirty_pages_.begin(); it != dirty_pages_.end();) {
        const auto& pending = it->second;
        const auto& owner = slots_.owner(it->first);
        if (!owner.live || pending.layer >= variants_.size() ||
            !variants_[pending.layer].live ||
            variants_[pending.layer].table_generation != pending.owner_generation ||
            owner.variant_key != variants_[pending.layer].param_key ||
            !(owner.page == pending.page)) {
            it = dirty_pages_.erase(it);
            continue;
        }
        if (pending.preassigned_slot != UINT32_MAX || detail_queued < limit) {
            auto& v = variants_[pending.layer];
            queue_page(v, pending.page, true, pending.preassigned_slot);
            // queue_page declines a page outside the owner's indirection
            // range; such a page can never be re-queued, so retire it.
            const auto queued = queued_keys_.find(page_key(v.layer, pending.page));
            if (queued == queued_keys_.end()) { it = dirty_pages_.erase(it); continue; }
            queue_[queued->second].requested_frame =
                std::min(queue_[queued->second].requested_frame, pending.requested_frame);
            if (pending.preassigned_slot == UINT32_MAX) ++detail_queued;
        }
        ++it;
    }
}

// ---------------------------------------------------------------------------
// WP-H tier-2 enrichment queue
// ---------------------------------------------------------------------------

// Forget everything tier 2 knows about one physical slot: clear its tier bit,
// decrement the enriched-page count, and drop any pending enrichment candidate
// naming it. Called from every site where a slot's CONTENT changes or goes away
// — eviction, release, a fresh fill, a global invalidation — because enrichment
// multiplies into the page IN PLACE: a stale tier bit either double-darkens a
// page or claims occlusion baked for texels that have since been replaced.
//
// O(enrich queue): removing from the middle of enrich_queue_ reindexes
// enrich_queued_slot_.
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

// Nominate a freshly filled page as a tier-2 candidate. A no-op when no
// enricher is installed or the per-frame budget is zero, so tier 2 costs
// literally nothing on a device without ray tracing. At most one candidate per
// physical slot (a second request replaces the first), and the queue is hard
// capped so a thrashing pool cannot grow it without bound.
void VtResidency::queue_enrich(uint32_t layer, VtPageKey page, uint32_t slot) {
    if (!enricher_ || max_enrich_per_frame_ == 0) return;
    if (slot >= slot_tier_.size()) return;
    if((slot_page_metadata_[slot].geometry.page_flags&kVtCoverageOnly) &&
       !enricher_->supports_separate_occlusion())return;
    if (layer < variants_.size() && variants_[layer].inputs &&
        variants_[layer].inputs->context.periodic.version) return;
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

void VtResidency::queue_resident_enrichment() {
    if(!ready_ || !enricher_ || !max_enrich_per_frame_)return;
    for(uint32_t slot=0;slot<slots_.capacity();++slot) {
        const auto& owner=slots_.owner(slot);
        if(!owner.live || slot_tier_[slot] || dirty_pages_.count(slot))continue;
        const auto layer=layer_of_.find(owner.variant_key);if(layer==layer_of_.end())continue;
        const auto& v=variants_[layer->second];
        if(v.live && slot_content_revisions_[slot]==v.content_revision)
            queue_enrich(layer->second,owner.page,slot);
    }
}

// Records up to max_enrich_per_frame_ tier-2 enrichments into `cmd`. Each
// candidate is re-validated against the slot pool first — the slot must still
// hold exactly the page that was queued, since an eviction or re-fill in between
// makes it stale (the re-fill queued its own candidate, so dropping this one
// loses nothing).
//
// Consumed candidates are removed whether or not they were dispatched, and the
// slot->index map is rebuilt from what is left. Transitions the ORM pool image
// to shader-read before dispatching, because sampling it is how the enricher
// reads the page's current texels back out of a BC-compressed pool.
void VtResidency::drain_enrich(VkCommandBuffer cmd) {
    stats_.enrich_last_frame = 0;
    if (!enricher_ || max_enrich_per_frame_ == 0 || enrich_queue_.empty())
        return;
    enrich_batch_.clear();
    struct Candidate {
        PendingEnrich pending;
        uint64_t generation,revision;
        std::shared_ptr<VtOcclusionPages::Page> factor;
    };
    std::vector<Candidate> candidates;
    std::vector<PendingEnrich> deferred;
    std::array<bool,16> written{};
    candidates.reserve(16);
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
        if (!owner.live || dirty_pages_.find(p.slot) != dirty_pages_.end() ||
            owner.variant_key != v.param_key ||
            !(owner.page == p.page)) {
            ++stats_.enrich_dropped_total;
            continue;
        }
        if (slot_tier_[p.slot] != 0) continue;   // already tier-2
        const bool separate=enricher_->supports_separate_occlusion() &&
            slot_geometry_lifetimes_[p.slot] && slot_page_metadata_[p.slot].height.version==1;
        std::shared_ptr<VtOcclusionPages::Page> factor;
        if(separate) {
            std::string allocation_error;
            factor=occlusion_pages_->allocate(*vulkan_,allocation_error);
            if(!factor) {++stats_.enrich_deferred_total;deferred.push_back(p);continue;}
        }
        VtEnrichRequest request;
        request.variant_hash = v.variant_hash;
        request.rung = static_cast<uint16_t>(v.rung);
        request.mip = static_cast<uint16_t>(p.page.mip);
        request.page_x = static_cast<uint16_t>(p.page.px);
        request.page_y = static_cast<uint16_t>(p.page.py);
        request.physical_slot = separate?p.slot:material_pages_.slot(p.slot);
        request.atlas = &v.inputs->geometry->atlas;
        request.part_context = &v.inputs->context;
        request.part_snapshot = v.inputs;
        request.pool = &pool_binding_;
        request.frame_index = frame_index_;
        request.owner_key = v.param_key;
        request.owner_generation = v.table_generation;
        if(factor) {
            request.occlusion_buffer=factor->slab->buffer.buffer;
            request.occlusion_offset=factor->offset();request.occlusion_address=factor->address();
            request.out_enriched=&written[candidates.size()];
        }
        enrich_batch_.push_back(request);
        candidates.push_back({p,owner.generation,v.content_revision,std::move(factor)});
        // Marked tier-2 at RECORD time, not on completion: the enrichment
        // multiplies into the page in place, so a second pass over the same
        // fill would darken it twice. A request the enricher then fails closed
        // on (no acceleration structure, no sampled pool view) simply stays
        // tier-1 content flagged as done -- which is the "skipped silently"
        // contract, since tier-1 pages are already correct.
        if(!separate) {slot_tier_[p.slot] = 1;++stats_.enriched_pages;}
    }
    enrich_queue_.erase(enrich_queue_.begin(),
                        enrich_queue_.begin() + static_cast<long>(consumed));
    enrich_queued_slot_.clear();
    for (size_t i = 0; i < enrich_queue_.size(); ++i)
        enrich_queued_slot_[enrich_queue_[i].slot] = i;
    stats_.enrich_queue_depth = static_cast<uint32_t>(enrich_queue_.size());
    if (enrich_batch_.empty()) {
        for(const auto& p:deferred)queue_enrich(p.layer,p.page,p.slot);
        return;
    }

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
    uint32_t published=0;
    for(size_t i=0;i<candidates.size();++i) {
        auto& candidate=candidates[i];
        if(!candidate.factor){++published;continue;}
        const auto& p=candidate.pending;const auto& request=enrich_batch_[i];
        const auto& owner=slots_.owner(p.slot);const auto& v=variants_[p.layer];
        const bool current=owner.live && owner.generation==candidate.generation &&
            owner.variant_key==request.owner_key && owner.page==p.page &&
            v.live && v.table_generation==request.owner_generation && v.content_revision==candidate.revision &&
            v.inputs==request.part_snapshot && !dirty_pages_.count(p.slot);
        if(current && written[i]) {
            retire_slot_occlusion(p.slot);
            slot_page_metadata_[p.slot].occlusion_address=candidate.factor->address();
            slot_occlusion_pages_[p.slot]=std::move(candidate.factor);
            ++stats_.occlusion_pages;slot_tier_[p.slot]=1;++stats_.enriched_pages;++published;
            input_indices_dirty_begin_=std::min(input_indices_dirty_begin_,p.slot);
            input_indices_dirty_end_=std::max(input_indices_dirty_end_,p.slot+1);
        } else {
            // A declined or stale producer may already have recorded writes.
            // Never reuse those bytes before the normal reader horizon.
            retire_occlusion(std::move(candidate.factor));
            if(current){++stats_.enrich_deferred_total;deferred.push_back(p);}
            else ++stats_.enrich_dropped_total;
        }
    }
    for(const auto& p:deferred) {
        const auto& owner=slots_.owner(p.slot);
        if(p.layer<variants_.size() && variants_[p.layer].live && owner.live &&
           owner.variant_key==variants_[p.layer].param_key && owner.page==p.page)
            queue_enrich(p.layer,p.page,p.slot);
    }
    stats_.enrich_last_frame=published;
    stats_.enrich_total+=published;
    // Only jobs that explicitly retain inputs should extend their lifetime.
    enrich_batch_.clear();
}

void VtResidency::inject_feedback_for_test(const VtFeedbackRequest* requests,
                                           size_t count) {
    injected_.assign(requests, requests + count);
}

// Turn one frame slot's completed feedback readback into page requests. Called
// only from begin_frame, where the slot's previous submission is known retired.
//
// The readback is 2-3 frames stale by construction, which is fine and is why the
// live checks below are tolerant: a request naming a released variant is
// dropped, and one naming a slot that has since been recycled — impossible
// inside the retirement horizon, which is wider than the readback ring — would
// at worst queue a valid fill for the new owner.
//
// The two dedup stages are pure CPU-cost engineering, not semantics: the feedback
// target is per-texel while a VT page covers hundreds of adjacent texels, and
// queue_page() is idempotent for a repeated key. The inline comments carry the
// equivalence argument and the measurement that motivated them.
//
// Test-injected requests (inject_feedback_for_test) are merged in and consumed
// here, so a headless test can drive residency with no GPU readback at all.
void VtResidency::drain_feedback(uint32_t frame_slot) {
    const bool have_readback = frame_slot < kFeedbackSlots &&
        feedback_slot_written_[frame_slot] && feedback_readback_[frame_slot].mapped &&
        feedback_w_ != 0;
    const size_t count = have_readback
        ? size_t(feedback_w_) * feedback_h_ * kVtFeedbackRequestsPerSample : 0;
    feedback_keys_.begin(injected_.size() + count / 8u);
    for (const auto& request : injected_)
        feedback_keys_.add_request(request.layer, request.mip, request.px, request.py);
    injected_.clear();
    if (have_readback) {
        PROFILE_SCOPE("vt.fb_scan");
        PROFILE_COUNT("vt.fb_texels", count);
        feedback_keys_.append_texels(
            static_cast<const uint16_t*>(feedback_readback_[frame_slot].mapped), count);
        PROFILE_COUNT("vt.fb_hits_raw", feedback_keys_.raw_hits());
        PROFILE_COUNT("vt.fb_hits_runlength", feedback_keys_.run_hits());
        PROFILE_COUNT("vt.fb_hits_filtered", feedback_keys_.candidates());
        feedback_slot_written_[frame_slot] = false;
    }
    // Preserve the same sorted distinct request order. The bounded filter
    // removes duplicates before this sort; a hash collision can only leave
    // more work for the sort, never suppress a different request.
    PROFILE_SCOPE_NAMED(dedup, "vt.fb_dedup");
    const auto& requests = feedback_keys_.finish();
    dedup.stop();
    stats_.requests_last_frame = static_cast<uint32_t>(requests.size());
    // Feedback is 2-3 frames stale. The frame/owner retirement horizon prevents
    // index reuse while a GPU readback can still reference the previous owner.
    PROFILE_SCOPE_NAMED(queue, "vt.fb_queue");
    for (uint64_t key : requests) {
        const uint32_t layer = uint32_t(key >> 48) - 1u;
        if (layer >= variants_.size()) continue;
        VariantRung& variant = variants_[layer];
        if (!variant.live) continue;
        queue_page(variant, VtPageKey{uint32_t((key >> 32) & 0xFFFFu),
                                     uint32_t(key & 0xFFFFu),
                                     uint32_t((key >> 16) & 0xFFFFu)});
    }
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------

// Live budgets are re-read from matter::vt_residency_budgets() every
// frame so an editor slider (or a FIFO `set vt.residency.fills_per_frame 24`)
// takes effect on the next frame. max_variants_ and the indirection arena are
// deliberately absent: both sized a buffer at init, which no world reload
// re-runs. Clamps are the ones the env helper used to apply.
void VtResidency::refresh_budgets() {
    const matter::VtResidencyBudgets& b = matter::vt_residency_budgets();
    const bool enabling_enrichment = max_enrich_per_frame_==0 && b.enrich_per_frame>0;
    max_fills_per_frame_ = clamp_u32(b.fills_per_frame, 1u, kMaxFillFlags);
    max_tail_fills_per_frame_ =
        clamp_u32(b.tail_fills_per_frame, 1u, kMaxFillFlags);
    fill_budget_ms_ = std::isfinite(b.fill_budget_ms)
        ? std::clamp(b.fill_budget_ms, 0.0f, 32.0f) : 12.0f;
    max_enrich_per_frame_ = clamp_u32(b.enrich_per_frame, 0u, 16u);
    if(enabling_enrichment && enricher_ && !enricher_->supports_separate_occlusion() &&
       (material_pages_.shared_references()!=0 || stats_.coverage_only_pages))
        invalidate_all_content();
    if(enabling_enrichment && enricher_ && enricher_->supports_separate_occlusion())queue_resident_enrichment();
    max_queue_ = clamp_u32(b.queue_cap, 16u, 65536u);
    // CPU mesh-copy budget, in bytes. Rejections past it fall back to the
    // legacy per-material path, i.e. the authored surfaces() tape is ignored
    // for that variant — so this is a quality dial, not a correctness one.
    mesh_budget_bytes_ =
        static_cast<size_t>(clamp_u32(b.mesh_budget_mb, 1u, 16384u)) * 1024u *
        1024u;
    stats_.mesh_budget_bytes = mesh_budget_bytes_;
    slots_.set_protect_frames(clamp_u32(b.evict_protect_frames, 1u, 100000u));
}

void VtResidency::observe_gpu_fill_ms(float vt_ms, uint32_t recorded_fills) {
    if (!recorded_fills || !std::isfinite(vt_ms) || vt_ms <= 0.0f) return;
    // The fill subzone includes the compositor's page bake, BC encode and
    // copies. An unusually costly page cuts the next quota immediately;
    // decay slowly after a one-time pool clear.
    const float observed = std::min(vt_ms / float(recorded_fills), 1000.0f);
    estimated_fill_ms_ = std::max(observed, estimated_fill_ms_ * 0.95f);
    estimated_fill_ms_ = std::max(estimated_fill_ms_, 0.25f);
}

// The frame's CPU-only phase, and the first VT call of a frame. Records nothing
// into a command buffer. In order: re-read the live budgets, adopt the frame
// clock (frame_index_ is what every LRU stamp, hysteresis window and retire
// serial in this file compares against), collect the three graveyards — page
// slots, indirection table blocks, and dead layers whose GPU record can finally
// be scrubbed — free the one-time pool-clear staging once it has retired, and
// drain this slot's feedback into the fill queue.
//
// PRECONDITION: the caller's frame fence has already retired this slot's
// previous submission. That is what makes reading its readback buffer and
// recycling anything past its retire serial legal.
void VtResidency::begin_frame(uint64_t frame_index, uint32_t frame_slot) {
    if (!ready_) return;
    refresh_budgets();
    frame_index_ = frame_index;
    frame_slot_ = frame_slot % kFeedbackSlots;
    if (filler_) filler_->begin_residency_frame(frame_index_, frame_slot_);
    // Collect the graveyards: a retire serial of (release frame +
    // kVtRetireHorizonFrames) has passed once the frame counter reaches it —
    // the caller's frame fences guarantee anything submitted that many frames
    // ago has retired on the GPU (the same guarantee the feedback readback and
    // the compositor's cache retirement already ride).
    PROFILE_SCOPE_NAMED(z_collect, "vt.collect");
    slots_.collect(frame_index_);
    material_pages_.collect(frame_index_);
    tables_.collect(frame_index_);
    for (auto& retired : retired_input_snapshots_)
        if (retired.snapshot && retired.retire_serial <= frame_index_) retired = {};
    for (auto it = retired_geometries_.begin(); it != retired_geometries_.end();) {
        if (it->second.retire_serial > frame_index_) { ++it; continue; }
        // Destruction can release the last module lease and retire its pages.
        // Remove this map node before invoking that reentrant teardown.
        auto lifetime=std::move(it->second.lifetime);
        it=retired_geometries_.erase(it);
        lifetime.reset();
    }
    publish_receiver_materials();
    if (pool_zero_staging_.buffer != VK_NULL_HANDLE && pool_cleared_ &&
        zero_staging_retire_ != 0 && frame_index_ >= zero_staging_retire_) {
        // The one-time pool clear's staging has retired on the GPU.
        destroy_buffer(pool_zero_staging_);
    }
    if (!layer_graveyard_.empty()) {
        size_t keep = 0;
        for (size_t i = 0; i < layer_graveyard_.size(); ++i) {
            const LayerGrave& g = layer_graveyard_[i];
            if (g.retire_serial <= frame_index_) {
                // Now — and only now — the dead registration's GPU record may
                // be scrubbed and its slot re-enter circulation.
                variant_records_[g.layer] = VariantRecordGpu{};
                variant_records_dirty_ = true;
                free_layers_.push_back(g.layer);
                continue;
            }
            layer_graveyard_[keep++] = layer_graveyard_[i];
        }
        layer_graveyard_.resize(keep);
    }
    refresh_indirection_stats();
    z_collect.stop();
    {
        // Full CPU scan of the 1/8-res feedback readback (raster/8 x raster/8
        // texels) plus a queue_page hash insert per non-zero hit, so this grows
        // with resolution AND with how much of the screen is VT-shaded.
        PROFILE_SCOPE("vt.drain_feedback");
        drain_feedback(frame_slot_);
        PROFILE_COUNT("vt.feedback_requests", stats_.requests_last_frame);
    }
}

bool VtResidency::ensure_feedback_pipeline(std::string& error) {
    if (feedback_gpu_) return true;
    auto gpu = std::make_unique<FeedbackGpu>();
    const std::vector<VkDescriptorSetLayoutBinding> bindings{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    if (!matter::create_compute_pipeline(*vulkan_, "vt_feedback.comp.spv",
                                         bindings, gpu->pipeline, error)) return false;
    const VkDescriptorPoolSize sizes[]{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFeedbackSlots},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFeedbackSlots}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = kFeedbackSlots;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    VkResult result = vkCreateDescriptorPool(vulkan_->device(), &pool, nullptr, &gpu->pool);
    if (result != VK_SUCCESS) {
        error = "VT feedback descriptor pool: " + std::to_string(result);
        return false;
    }
    VkDescriptorSetLayout layouts[kFeedbackSlots];
    for (auto& layout : layouts) layout = gpu->pipeline.descriptor_set_layout;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = gpu->pool;
    alloc.descriptorSetCount = kFeedbackSlots;
    alloc.pSetLayouts = layouts;
    result = vkAllocateDescriptorSets(vulkan_->device(), &alloc, gpu->sets);
    if (result != VK_SUCCESS) {
        error = "VT feedback descriptors: " + std::to_string(result);
        return false;
    }
    feedback_gpu_ = std::move(gpu);
    return true;
}

// A full-resolution integer attachment follows final G-buffer depth visibility.
// GPU extraction writes a receiver/material pair per 8x8 block to the cached readback
// ring. Rebuild only when the raster extent changes, under the renderer's
// existing target-resize retirement contract.
//
// A rebuild DROPS every slot's pending readback, so the frames immediately after
// a resize simply produce no page requests. Nothing goes black: resident pages
// stay resident and every unmapped entry still resolves to its variant's pinned
// tail. Returns true unchanged when the runtime never started.
bool VtResidency::ensure_feedback(const matter::VkImageResource& visible_feedback,
                                  std::string& error) {
    if (!ready_) return true;
    const uint32_t raster_width = visible_feedback.extent.width;
    const uint32_t raster_height = visible_feedback.extent.height;
    if (!raster_width || !raster_height || visible_feedback.view == VK_NULL_HANDLE ||
        visible_feedback.format != kVtFeedbackFormat) {
        error = "VT feedback requires the current RGBA32_UINT raster attachment";
        return false;
    }
    const uint32_t w = (raster_width - 1u) / 8u + 1u;
    const uint32_t h = (raster_height - 1u) / 8u + 1u;
    if (raster_width == feedback_raster_w_ && raster_height == feedback_raster_h_ &&
        feedback_source_view_ == visible_feedback.view)
        return true;
    if (!ensure_feedback_pipeline(error)) return false;
    feedback_source_view_ = VK_NULL_HANDLE;
    feedback_source_lifetime_.reset();
    for (uint32_t i = 0; i < kFeedbackSlots; ++i) {
        destroy_buffer(feedback_readback_[i]);
        feedback_slot_written_[i] = false;
    }
    feedback_w_ = feedback_h_ = 0;
    feedback_raster_w_ = feedback_raster_h_ = 0;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 8u *
                               kVtFeedbackRequestsPerSample;
    for (uint32_t i = 0; i < kFeedbackSlots; ++i) {
        // HOST_CACHED IS THE WHOLE POINT HERE. This is the one buffer the CPU
        // READS every frame -- drain_feedback scans all w*h texels of it. Asked
        // for HOST_VISIBLE|HOST_COHERENT alone, a driver is free to hand back
        // uncached write-combined memory, where every read is an uncached fetch
        // across PCIe. A 2026-08-08 capture measured that scan at 11.1 ms for
        // 29150 texels -- 381 ns per 8-byte read, roughly 200x what reading
        // cached memory costs, and 96% of drain_feedback.
        //
        // COHERENT stays REQUIRED so no vkInvalidateMappedMemoryRanges is
        // needed; CACHED is preferred, and find_memory_type falls back to the
        // required pair on a device that cannot offer both.
        if (!create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           feedback_readback_[i], error,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
            return false;
        }
    }
    const VkDescriptorImageInfo image{VK_NULL_HANDLE, visible_feedback.view,
                                      VK_IMAGE_LAYOUT_GENERAL};
    for (uint32_t i = 0; i < kFeedbackSlots; ++i) {
        const VkDescriptorBufferInfo buffer{feedback_readback_[i].buffer, 0, bytes};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t binding = 0; binding < 2; ++binding) {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = feedback_gpu_->sets[i];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &image;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &buffer;
        vkUpdateDescriptorSets(vulkan_->device(), 2, writes, 0, nullptr);
    }
    feedback_w_ = w;
    feedback_h_ = h;
    feedback_raster_w_ = raster_width;
    feedback_raster_h_ = raster_height;
    feedback_source_view_ = visible_feedback.view;
    feedback_source_lifetime_ = visible_feedback.lifetime;
    return true;
}

// The renderer clears and writes the request attachment with the G-buffer.
// Extract only final visible requests after vkCmdEndRendering. The image's
// tracked layout is shared with its renderer owner and updated by the barrier.
//
// The readback lands in THIS frame slot's buffer and is consumed by the
// begin_frame of the frame that next reuses the slot — which is where the 2-3
// frame staleness of the whole feedback loop comes from.
void VtResidency::record_feedback_readback(VkCommandBuffer cmd,
                                          matter::VkImageResource& visible_feedback) {
    if (!ready_ || !feedback_gpu_ || feedback_w_ == 0 || feedback_h_ == 0) return;
    if (visible_feedback.view != feedback_source_view_) return;
    if (frame_slot_ >= kFeedbackSlots) return;
    if (feedback_readback_[frame_slot_].buffer == VK_NULL_HANDLE) return;
    matter::record_image_transition(cmd, visible_feedback,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, feedback_gpu_->pipeline.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        feedback_gpu_->pipeline.pipeline_layout, 0, 1,
        &feedback_gpu_->sets[frame_slot_], 0, nullptr);
    vkCmdDispatch(cmd, (feedback_w_ + 7u) / 8u, (feedback_h_ + 7u) / 8u, 1);
    buffer_barrier(cmd, feedback_readback_[frame_slot_].buffer,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    feedback_slot_written_[frame_slot_] = true;
}

// Records the frame's entire VT GPU workload into `cmd`. Must be recorded
// BEFORE the G-buffer pass and outside any render pass — everything here is
// transfers plus the filler's and enricher's own dispatches.
//
// The order is load-bearing throughout:
//   1. tier-2 enrichment, while the pool is still shader-readable (what the
//      enricher samples) and before this frame's fills, so a page filled by this
//      command buffer can never be enriched by the same one;
//   2. pool images -> TRANSFER_DST, with the one-time zero scrub on first use;
//   3. the indirection buffer's transfer window, with its one-time arena fill;
//   4. the fill drain;
//   5. indirection table uploads through this frame slot's staging ring;
//   6. everything back to shader-read.
//
// Step 4 is where the policy lives: a stable priority sort, SEPARATE tail and
// page budgets (a registration tail gates a whole variant into the VT path, so
// it must never queue behind feedback-driven sharpening), page admission that
// STOPS for the frame rather than thrashing once the pool is exhausted, a trim
// of feedback-only work to max_queue_, and — the part that is easy to get wrong — a page
// becomes resident only AFTER the filler reports it actually wrote the slot.
// Mapping before that is what once turned every skipped request into a page
// pointing at never-written pool memory.
//
// Returns true unconditionally today, including when the runtime is not up;
// `error` is reserved for a filler that grows a failure path.
bool VtResidency::record_frame(VkCommandBuffer cmd, std::string& error,
                               VkQueryPool timing_pool,
                               uint8_t* timing_written) {
    if (!ready_) return true;
    recorded_fill_count_ = 0;
    const auto stamp = [&](uint32_t zone, bool end) {
        if (!timing_pool || !timing_written) return;
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             timing_pool, zone * 2u + (end ? 1u : 0u));
        timing_written[zone] |= end ? 2u : 1u;
    };
    stats_.fills_last_frame = 0;
    stats_.pool_used = slots_.used();
    stats_.pool_pinned = slots_.pinned();
    stats_.evictions_total = slots_.evictions();
    stats_.queue_depth = static_cast<uint32_t>(queue_.size());
    stats_.lru_scan_count = slots_.lru_scan_count();
    stats_.lru_scan_ns = slots_.lru_scan_ns();

    queue_dirty_pages();
    // --- WP-H: tier-2 enrichment, BEFORE this frame's fills ---------------
    // Ordering matters twice over. (1) It runs while the pool is still in its
    // shader-read layout, which is what the enricher samples the page's current
    // ORM texels from. (2) Draining before the fills guarantees a page queued
    // by THIS frame's fills is never enriched in the same command buffer that
    // wrote it -- the earliest it can run is the next frame, by which point the
    // fill's transfer has been submitted and the layout transition below is the
    // dependency that orders them.
    {
        PROFILE_SCOPE("vt.enrich");
        stamp(matter::kGpuTimingVtEnrich, false);
        drain_enrich(cmd);
        stamp(matter::kGpuTimingVtEnrich, true);
    }
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
    if (!pool_cleared_ && pool_zero_staging_.buffer != VK_NULL_HANDLE) {
        // One-time pool scrub: any tail-gate violation then samples a
        // deterministic flat black instead of undefined memory. The BC
        // channels cannot vkCmdClearColorImage (compressed formats), so
        // they are cleared by copying the zeroed staging buffer over every
        // layer; the uncompressed aux/height channels take the plain clear.
        for (uint32_t c = 0; c < kVtChannelCount; ++c) {
            if (c == kVtChannelAux || c == kVtChannelHeight) {
                const VkClearColorValue zero{};
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,
                                                    0, 1, 0, pool_[c].layers};
                vkCmdClearColorImage(cmd, pool_[c].image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &zero, 1, &range);
                continue;
            }
            for (uint32_t layer = 0; layer < pool_[c].layers; ++layer) {
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer,
                                         1};
                copy.imageExtent = {kVtPoolLayerEdgeTexels,
                                    kVtPoolLayerEdgeTexels, 1};
                vkCmdCopyBufferToImage(cmd, pool_zero_staging_.buffer,
                                       pool_[c].image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &copy);
            }
        }
        // WAW: this frame's fills rewrite subsets of the just-cleared images.
        VkMemoryBarrier2 waw{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        waw.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        waw.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        waw.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        waw.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &waw;
        vkCmdPipelineBarrier2(cmd, &dep);
        pool_cleared_ = true;
        // The staging is consumed by this frame alone; free it once the
        // frame has retired.
        zero_staging_retire_ = frame_index_ + kVtRetireHorizonFrames;
    }
    // Open the indirection buffer's transfer window. srcStage ALL_COMMANDS
    // orders these copies after every prior submission's sampling of the
    // buffer — the same discipline the image path used, and the reason
    // eviction/refill never needs a graveyard: in-flight frames execute
    // strictly before this frame's copies.
    buffer_barrier(cmd, indirection_buffer_.buffer,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!indirection_cleared_) {
        // One-time arena scrub: an entry never written decodes as (slot 0,
        // mip 0) — a bounded, valid pool address — instead of undefined bytes.
        vkCmdFillBuffer(cmd, indirection_buffer_.buffer, 0, VK_WHOLE_SIZE, 0u);
        buffer_barrier(cmd, indirection_buffer_.buffer,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT);
        indirection_cleared_ = true;
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
    PROFILE_COUNT("vt.queue_depth", queue_.size());
    // Advance staging lifetime even when demand vanished and no fill remains.
    // Otherwise abandoned partial uploads can keep their CPU/GPU leases idle.
    if (filler_) filler_->begin_preparation_frame();
    if (!queue_.empty() && filler_ && !page_fills_paused_for_test_ && !input_update_pending_) {
        PROFILE_SCOPE("vt.fill_select");
        // Reserve the shared batch ceiling for mandatory coverage first.
        // Separate per-class counters alone are insufficient: a detail budget
        // equal to kMaxFillFlags could otherwise occupy the entire batch before
        // any initial tail (priority zero) is visited. Within each class use
        // highest priority first; ties retain insertion order.
        std::stable_sort(queue_.begin(), queue_.end(),
                         [](const PendingFill& a, const PendingFill& b) {
                             const bool a_tail = a.preassigned_slot != 0xFFFFFFFFu;
                             const bool b_tail = b.preassigned_slot != 0xFFFFFFFFu;
                             if (a_tail != b_tail) return a_tail;
                             return a.priority > b.priority;
                         });
        // Two composition budgets over one pass (cached uploads use the
        // filler staging capacity plus the shared batch ceiling):
        // TAIL fills (preassigned slots — registration tails and in-place
        // invalidation re-fills) draw from max_tail_fills_per_frame_, page
        // fills from max_fills_per_frame_. A streaming burst's tails gate
        // whole variants out of the VT path, so they must never wait behind
        // feedback-driven sharpening; both classes share the kMaxFillFlags
        // batch ceiling.
        uint32_t tail_taken = 0, page_taken = 0;
        const uint32_t time_quota = fill_budget_ms_ > 0.0f
            ? std::max(1u, std::min(4u, static_cast<uint32_t>(
                  fill_budget_ms_ / std::max(estimated_fill_ms_, 0.25f))))
            : kMaxFillFlags;
        bool page_admission_blocked = false;
        std::map<VtPreparationKey, bool> prepared_owners;
        std::vector<uint8_t> taken(queue_.size(), 0u);
        for (size_t i = 0; i < queue_.size(); ++i) {
            if (batch_.size() >= kMaxFillFlags || batch_.size() >= time_quota) break;
            const PendingFill& p = queue_[i];
            // Bounds first, matching drain_enrich and the feedback drain. A
            // layer past the table can only appear if the queue outlived the
            // variant table (shutdown clears both today, so this is a guard,
            // not a live path) — drop it rather than index out of range.
            if (p.layer >= variants_.size()) {
                taken[i] = 1;
                continue;
            }
            VariantRung& v = variants_[p.layer];
            if (!v.live || v.table_generation != p.owner_generation ||
                v.content_revision != p.content_revision) {
                taken[i] = 1;   // dead entry: drop without dispatch
                continue;
            }
            const bool is_tail = p.preassigned_slot != 0xFFFFFFFFu;
            if (!is_tail && page_admission_blocked) continue;
            VtFillRequest request;
            request.variant_hash = v.variant_hash;
            request.rung = static_cast<uint16_t>(v.rung);
            request.mip = static_cast<uint16_t>(p.page.mip);
            request.page_x = static_cast<uint16_t>(p.page.px);
            request.page_y = static_cast<uint16_t>(p.page.py);
            request.physical_slot = UINT32_MAX;
            request.atlas = &v.inputs->geometry->atlas;
            request.part_context = &v.inputs->context;
            request.part_snapshot = v.inputs;
            request.owner_generation = p.owner_generation;
            request.content_revision = p.content_revision;
            request.owner_key = v.param_key;
            request.input_snapshot = input_snapshot_;
            const auto readiness = filler_->probe_page(request);
            if (readiness == VtPageFiller::PageReadiness::Pending) continue;
            // One readiness probe per admitted owner. Deferred CPU/GPU work
            // retains demand and request age, and
            // cannot evict an unrelated page just to wait for preparation.
            const VtPreparationKey preparation{v.variant_hash, v.rung,
                                               v.param_key, v.table_generation};
            if (readiness == VtPageFiller::PageReadiness::NeedsPreparation) {
                // Composition budgets apply only to work that needs baking.
                // Ready imports reserve their own bounded, fence-owned staging
                // slices in probe_page; both paths still share kMaxFillFlags.
                if (is_tail ? tail_taken >= max_tail_fills_per_frame_
                            : page_taken >= max_fills_per_frame_) continue;
                auto prepared = prepared_owners.find(preparation);
                if (prepared == prepared_owners.end()) {
                    prepared = prepared_owners.emplace(preparation,
                        filler_->prepare(preparation, v.inputs)).first;
                }
                if (!prepared->second) continue;
            }
            uint32_t slot = p.preassigned_slot;
            bool acquired = false;
            if (!is_tail && v.indirection.is_mapped(p.page.mip, p.page.px, p.page.py)) {
                slot = v.indirection.resolve(p.page.mip, p.page.px, p.page.py).slot;
                slots_.touch(slot, frame_index_);
            } else if (!is_tail) {
                VtSlotPool::Owner evicted;
                if (!slots_.acquire(v.param_key,
                                    p.page, /*pinned=*/false, frame_index_,
                                    slot, evicted)) {
                    // Pool exhausted: everything is pinned or protected by
                    // the request hysteresis window. Page fills retry next
                    // frame while the indirection keeps serving the coarser
                    // resident coverage (worst case the tail) — degrade,
                    // never thrash. Tails keep draining: they own their
                    // slots already.
                    page_admission_blocked = true;
                    continue;
                }
                if (evicted.live) {
                    dirty_pages_.erase(slot);
                    material_pages_.release(slot);
                    retire_slot_input_snapshot(slot);
                    retire_slot_geometry(slot);
                    retire_slot_occlusion(slot);
                    retire_slot_material_mapping(slot);
                    if (event_log_)
                        MATTER_LOGI("vt-evict", "frame=%llu owner=%016llx mip=%u x=%u y=%u slot=%u",
                            static_cast<unsigned long long>(frame_index_),
                            static_cast<unsigned long long>(evicted.variant_key),
                            evicted.page.mip, evicted.page.px, evicted.page.py, slot);
                    const auto owner_layer = layer_of_.find(evicted.variant_key);
                    if (owner_layer != layer_of_.end())
                        variants_[owner_layer->second].indirection.unmap(
                            evicted.page.mip, evicted.page.px, evicted.page.py);
                }
                acquired = true;
            } else {
                slots_.touch(slot, frame_index_);
            }
            taken[i] = 1;
            if (readiness == VtPageFiller::PageReadiness::NeedsPreparation) {
                if (is_tail) ++tail_taken; else ++page_taken;
            }
            request.physical_slot = slots_.capacity() + static_cast<uint32_t>(batch_.size());
            request.pool = &pool_binding_;
            if(p.page.mip+1<v.layout.mip_count &&
               (!(enricher_ && max_enrich_per_frame_) || enricher_->supports_separate_occlusion()) &&
               v.material_published && v.material_published->inputs==v.inputs) {
                request.coverage_only=vt_receiver_material_page(*v.inputs,v.material_published->records,
                    p.page.mip,p.page.px,p.page.py);
                if(request.coverage_only && v.material_candidate && v.material_candidate->inputs==v.inputs)
                    request.coverage_only=vt_receiver_material_page(*v.inputs,v.material_candidate->records,
                        p.page.mip,p.page.px,p.page.py);
            }
            batch_.push_back(request);
            pending_map_.push_back(PendingMap{p.layer, p.page, slot, is_tail, p.requested_frame,
                p.owner_generation, p.content_revision, slots_.owner(slot).generation, acquired, v.param_key});
        }
        // Only dispatched (or dead-dropped) entries leave the queue; budget-
        // or admission-skipped ones keep their order for next frame.
        size_t keep = 0;
        for (size_t i = 0; i < queue_.size(); ++i)
            if (!taken[i]) queue_[keep++] = queue_[i];
        queue_.resize(keep);
        // Cap only feedback-driven requests, retaining their priority order.
        // A mandatory tail already has a mapping and bypasses queue_page's
        // resident fast-out only through force/preassigned_slot. Feedback
        // cannot regenerate that work after a capacity drop. There is at most
        // one coalesced tail per admitted owner, so protected work is bounded
        // independently by variant admission.
        //
        // Without this the queue only ever grew: drain_feedback re-derives the
        // wanted set every frame while ~2.7 fills retire, so measured depths
        // ran 28920 -> 45082 -> 66701 across three sessions and were still
        // climbing. The sort and the queued_keys_ rebuild below are both O(n)
        // or worse per frame, which is how a 35 ms frame ended up spending
        // 14.3 ms in fill_select alone -- a cost that scaled with time flown
        // rather than with anything on screen.
        //
        // Visible missing detail is re-requested through asynchronous feedback;
        // initial/refresh tails must survive until success or owner cancellation.
        uint32_t detail_kept = 0;
        uint64_t dropped = 0;
        keep = 0;
        for (size_t i = 0; i < queue_.size(); ++i) {
            if (queue_[i].preassigned_slot != 0xFFFFFFFFu ||
                detail_kept < max_queue_) {
                if (queue_[i].preassigned_slot == 0xFFFFFFFFu) ++detail_kept;
                queue_[keep++] = queue_[i];
            } else {
                ++dropped;
            }
        }
        queue_.resize(keep);
        if (dropped != 0) {
            stats_.requests_dropped_total += dropped;
            PROFILE_COUNT("vt.requests_dropped", dropped);
        }
        reindex_pending_fills();
    }

    recorded_fill_count_ = static_cast<uint32_t>(batch_.size());

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
            fill_heights_[i] = {};
            batch_[i].out_height = &fill_heights_[i];
            fill_geometries_[i] = {};
            batch_[i].out_geometry = &fill_geometries_[i];
            fill_material_keys_[i] = {};
            batch_[i].out_material_key = &fill_material_keys_[i];
        }
        {
            // Tier-1 page bake: the compositor samples the tileset slots and
            // encodes BC blocks per page. The shared time quota is estimated
            // from retired GPU timestamps and still admits one page even if
            // that page alone exceeds the target.
            PROFILE_SCOPE("vt.fill");
            PROFILE_COUNT("vt.fill_batch", batch_.size());
            stamp(matter::kGpuTimingVtFill, false);
            filler_->fill(cmd, batch_.data(), batch_.size());
            stamp(matter::kGpuTimingVtFill, true);
        }

        // --- map or roll back, per request --------------------------------
        uint32_t mapped = 0;
        bool copy_window = false;
        for (size_t i = 0; i < pending_map_.size(); ++i) {
            const PendingMap& m = pending_map_[i];
            const bool owner_current = m.layer < variants_.size() &&
                variants_[m.layer].live && variants_[m.layer].table_generation == m.owner_generation;
            const bool slot_current = m.slot < slots_.capacity() &&
                slots_.owner(m.slot).live && slots_.owner(m.slot).generation == m.slot_generation &&
                slots_.owner(m.slot).variant_key == m.owner_key && slots_.owner(m.slot).page == m.page;
            const bool current = owner_current && slot_current &&
                variants_[m.layer].content_revision == m.content_revision &&
                batch_[i].input_snapshot == input_snapshot_;
            if (!current) {
                ++stats_.fills_stale_total;
                if (m.acquired && slot_current) {
                    dirty_pages_.erase(m.slot);
                    slot_reset_tier(m.slot);
                    slots_.release_now(m.slot);
                    material_pages_.release(m.slot);
                }
                // Released owners need no retry. Live owners' durable dirty
                // state (or initial tail request) targets their newest revision.
                if (owner_current && m.preassigned) {
                    queue_page(variants_[m.layer], m.page, true, m.slot);
                    const auto retry = queued_keys_.find(page_key(m.layer, m.page));
                    if (retry != queued_keys_.end())
                        queue_[retry->second].requested_frame =
                            std::min(queue_[retry->second].requested_frame, m.requested_frame);
                }
                if (event_log_)
                    MATTER_LOGI("vt-stale", "frame=%llu owner=%016llx generation=%llu revision=%llu slot=%u",
                        static_cast<unsigned long long>(frame_index_),
                        static_cast<unsigned long long>(m.owner_key),
                        static_cast<unsigned long long>(m.owner_generation),
                        static_cast<unsigned long long>(m.content_revision), m.slot);
                continue; // never copy stale scratch bytes to any resident slot
            }
            VariantRung& v = variants_[m.layer];
            if (event_log_)
                MATTER_LOGI("vt-page", "frame=%llu owner=%016llx generation=%llu revision=%llu mip=%u x=%u y=%u slot=%u mandatory=%u first_frame=%llu result=%s",
                    static_cast<unsigned long long>(frame_index_),
                    static_cast<unsigned long long>(v.param_key),
                    static_cast<unsigned long long>(v.table_generation),
                    static_cast<unsigned long long>(v.content_revision),
                    m.page.mip, m.page.px, m.page.py, m.slot, m.preassigned ? 1u : 0u,
                    static_cast<unsigned long long>(m.requested_frame),
                    fill_flags_[i] ? "recorded" : "producer_refused");
            VtMaterialPages::Binding pixels;
            const bool coverage_only=(fill_geometries_[i].gpu.page_flags&kVtCoverageOnly)!=0;
            if(fill_flags_[i] && coverage_only &&
               (!batch_[i].coverage_only || !fill_geometries_[i].lifetime)) fill_flags_[i]=false;
            if (fill_flags_[i]) {
                // Enrichment modifies ORM using receiver-specific geometry.
                // Preserve it with private payloads until it has overrides.
                const bool private_enrichment=enricher_ && max_enrich_per_frame_ &&
                    !(enricher_->supports_separate_occlusion() && fill_geometries_[i].lifetime && fill_heights_[i].version==1);
                const auto key = VtMaterialPages::key(private_enrichment ? VtMaterialPixelKey{} : fill_material_keys_[i],
                    batch_[i].input_snapshot ? batch_[i].input_snapshot->identity : 0,
                    fill_heights_[i]);
                if(coverage_only) {
                    // Queue-ordered replacement: old GPU readers precede the
                    // pool barrier, and the mapping retains its module lease.
                    material_pages_.release(m.slot);
                    pixels={0,false}; // no owned material address; shader flag guards fallback
                } else {
                    pixels = material_pages_.publish(m.slot, key);
                    if (pixels.slot == UINT32_MAX) fill_flags_[i] = false;
                }
            }
            if (fill_flags_[i]) {
                if (!copy_window) {
                    // Same-image copy requires GENERAL on source/destination.
                    // The opening pool barrier already orders prior frame
                    // readers before this frame. This barrier makes producer
                    // writes available to the publication copies.
                    for (uint32_t c = 0; c < kVtChannelCount; ++c) {
                        barrier(cmd, pool_[c].image, pool_[c].layers, pool_[c].layout,
                                VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
                        pool_[c].layout = VK_IMAGE_LAYOUT_GENERAL;
                    }
                    copy_window = true;
                }
                uint32_t src_layer, src_x, src_y, dst_layer, dst_x, dst_y;
                vt_slot_origin(batch_[i].physical_slot, src_layer, src_x, src_y);
                VkImageCopy copy{};
                copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, src_layer, 1};
                copy.srcOffset = {static_cast<int32_t>(src_x), static_cast<int32_t>(src_y), 0};
                copy.extent = {kVtPageStride, kVtPageStride, 1};
                for (uint32_t c = 0; c < kVtChannelCount; ++c) {
                    if (c != kVtChannelAux && !pixels.write) continue;
                    vt_slot_origin(c == kVtChannelAux ? m.slot : pixels.slot, dst_layer, dst_x, dst_y);
                    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, dst_layer, 1};
                    copy.dstOffset = {static_cast<int32_t>(dst_x), static_cast<int32_t>(dst_y), 0};
                    vkCmdCopyImage(cmd, pool_[c].image, VK_IMAGE_LAYOUT_GENERAL,
                                   pool_[c].image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
                }
                v.indirection.map(m.page.mip, m.page.px, m.page.py, m.slot);
                set_slot_input_snapshot(m.slot, batch_[i].input_snapshot);
                slot_page_metadata_[m.slot].height = fill_heights_[i];
                slot_page_metadata_[m.slot].material_slot = pixels.slot;
                slot_page_metadata_[m.slot].surface_revision = v.content_revision;
                set_slot_geometry(m.slot, fill_geometries_[i]);
                slot_content_revisions_[m.slot]=v.content_revision;
                set_slot_material_mapping(m.slot,
                    v.material_published && v.material_published->inputs==v.inputs ?
                    v.material_published : nullptr);
                input_indices_dirty_begin_ = std::min(input_indices_dirty_begin_, m.slot);
                input_indices_dirty_end_ = std::max(input_indices_dirty_end_, m.slot + 1u);
                dirty_pages_.erase(m.slot);
                if (m.page.mip + 1u == v.layout.mip_count) {
                    v.tail_filled = true;
                    v.boundary_source.reset();
                    if(fill_geometries_[i].lifetime && fill_geometries_[i].boundary) {
                        auto source=std::make_shared<VtSurfaceBoundarySource>();
                        source->slot=m.layer+1;source->generation=uint32_t(v.table_generation);
                        source->content_revision=v.content_revision;source->inputs=v.inputs;
                        source->material_inputs=slot_input_snapshots_[m.slot];
                        source->metadata=slot_page_metadata_[m.slot];
                        source->geometry=fill_geometries_[i];v.boundary_source=std::move(source);
                    }
                    // TAIL GATE: the fill is recorded in THIS frame's command
                    // buffer, so a draw recorded from the NEXT frame on is
                    // queue-ordered after it and reads written texels. Flip
                    // activation then, and cue the renderer to republish its
                    // vt_slot table.
                    if (v.tail_ready_serial == kVtTailNotReady) {
                        v.tail_ready_serial = frame_index_ + 1u;
                        activation_dirty_ = true;
                    }
                }
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
            if (m.acquired) {
                // Freshly acquired: hand it straight back — release_now is
                // legal because the entry was never mapped, so no frame past
                // or present can resolve into this slot. Every sample of this
                // page keeps resolving to the variant's tail, exactly as
                // before the request.
                slot_reset_tier(m.slot);
                slots_.release_now(m.slot);
                material_pages_.release(m.slot);
            } else if (m.preassigned) {
                // A pinned tail keeps its slot (every unmapped entry resolves
                // to it, so releasing it would break that invariant) but its
                // content is still undefined: leave tail_filled false and
                // re-queue the in-place re-fill so the next frame retries.
                queue_page(v, m.page, /*force=*/true, /*preassigned_slot=*/m.slot);
                const auto retry = queued_keys_.find(page_key(v.layer, m.page));
                if (retry != queued_keys_.end())
                    queue_[retry->second].requested_frame = std::min(
                        queue_[retry->second].requested_frame, m.requested_frame);
            }
        }
        stats_.fills_last_frame = mapped;
        stats_.fills_total += mapped;
        // Failed/stale candidates never become persistent owners. Successful
        // candidates have transferred ownership to their resident slots.
        for (size_t i = 0; i < batch_.size(); ++i) fill_geometries_[i] = {};
    }

    // --- indirection table uploads ----------------------------------------
    // Staged through this frame slot's ring buffer (its previous submission
    // has retired — the caller's frame fence). Never-uploaded tables go
    // first: until a new registration's table has been copied once, the GPU
    // side of its block is the arena's zero-fill, and its draws land THIS
    // frame. Dirty re-uploads follow; either kind that misses the window
    // stays dirty and retries next frame.
    {
        // Two full scans over variants_ every frame, regardless of how many
        // tables are actually dirty.
        PROFILE_SCOPE("vt.table_upload");
        PROFILE_COUNT("vt.variants", variants_.size());
        Buffer& staging = indirection_staging_[frame_slot_];
        VkDeviceSize staging_offset = 0;
        const auto upload_table = [&](VariantRung& v) -> bool {
            const VkDeviceSize bytes =
                static_cast<VkDeviceSize>(v.layout.entry_count) * 4u;
            if (staging.mapped == nullptr ||
                staging_offset + bytes > staging.size)
                return false;
            const std::vector<uint32_t>& texels = v.indirection.texels();
            std::memcpy(static_cast<uint8_t*>(staging.mapped) + staging_offset,
                        texels.data(), bytes);
            VkBufferCopy copy{};
            copy.srcOffset = staging_offset;
            copy.dstOffset =
                static_cast<VkDeviceSize>(v.table_offset_words) * 4u;
            copy.size = bytes;
            vkCmdCopyBuffer(cmd, staging.buffer, indirection_buffer_.buffer, 1,
                            &copy);
            v.indirection.clear_dirty();
            v.table_uploaded = true;
            staging_offset += bytes;
            return true;
        };
        for (VariantRung& v : variants_) {
            if (!v.live || v.table_uploaded) continue;
            if (!upload_table(v)) ++stats_.table_uploads_deferred_total;
        }
        for (VariantRung& v : variants_) {
            if (!v.live || !v.table_uploaded || !v.indirection.dirty())
                continue;
            if (!upload_table(v)) ++stats_.table_uploads_deferred_total;
        }
    }

    publish_surface_connections(error);
    record_input_snapshot_indices(cmd);

    // --- back to shader-read ---------------------------------------------
    for (uint32_t c = 0; c < kVtChannelCount; ++c) {
        barrier(cmd, pool_[c].image, pool_[c].layers, pool_[c].layout,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        pool_[c].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    buffer_barrier(cmd, indirection_buffer_.buffer,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    if (variant_records_dirty_ && variant_buffer_.mapped) {
        std::memcpy(variant_buffer_.mapped, variant_records_.data(),
                    variant_buffer_.size);
        variant_records_dirty_ = false;
    }

    stats_.pool_used = slots_.used();
    stats_.pool_pinned = slots_.pinned();
    stats_.evictions_total = slots_.evictions();
    stats_.lru_scan_count = slots_.lru_scan_count();
    stats_.lru_scan_ns = slots_.lru_scan_ns();
    refresh_queue_stats();
    refresh_indirection_stats();
    if (density_frame_ && frame_index_ >= density_frame_) {
        log_page_density();
        density_frame_ = 0;
    }
    // GPU commands have staged their inputs; reusable request capacity must
    // not keep obsolete snapshots alive until another frame happens to run.
    batch_.clear();
    (void)error;
    return true;
}

void VtResidency::log_page_density() const {
    // A CPU snapshot of occupied slots after this frame's recording. No GPU
    // readback or wait. Not a completion receipt, visible-pixel metric, or a
    // performance sample. Count aliases once, through physical ownership.
    MATTER_LOGI("vt-density", "begin schema=1 frame=%llu capacity=%u occupied=%u pinned=%u "
                "format_bytes=%llu payload=%u stride=%u reserved=%u resident_capacity=%u",
                static_cast<unsigned long long>(frame_index_), pool_pages_,
                slots_.used(), slots_.pinned(),
                static_cast<unsigned long long>(stats_.pool_bytes),
                chart_atlas::kVtPagePayload, kVtPageStride, kMaxFillFlags, slots_.capacity());
    uint32_t reported = 0;
    for (uint32_t slot = 0; slot < slots_.capacity(); ++slot) {
        const auto& owner = slots_.owner(slot);
        if (!owner.live) continue;
        const auto found = layer_of_.find(owner.variant_key);
        if (found == layer_of_.end()) continue;
        const auto& v = variants_[found->second];
        if (!v.live) continue;
        const auto d = vt_measure_page_density(v.inputs->geometry->atlas, v.inputs->context, owner.page.mip,
                                               owner.page.px, owner.page.py);
        MATTER_LOGI("vt-density-page", "slot=%u owner=%016llx generation=%llu "
                    "mip=%u x=%u y=%u pinned=%u tail_filled=%u geometry=%u "
                    "atlas=%u block=%u bounds=%u gutter=%u triangle=%u",
                    slot, static_cast<unsigned long long>(owner.variant_key),
                    static_cast<unsigned long long>(owner.generation),
                    unsigned(owner.page.mip), unsigned(owner.page.px),
                    unsigned(owner.page.py), owner.pinned ? 1u : 0u,
                    v.tail_filled ? 1u : 0u, d.geometry_available ? 1u : 0u,
                    d.atlas_texels, d.chart_block_texels, d.content_bounds_texels,
                    d.gutter_bounds_texels, d.triangle_texels);
        ++reported;
    }
    MATTER_LOGI("vt-density", "end frame=%llu reported=%u occupied=%u",
                static_cast<unsigned long long>(frame_index_), reported, slots_.used());
}

}  // namespace vt
