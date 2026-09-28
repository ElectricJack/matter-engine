// vt_compositor.cpp — WP-D tier-1 chart-page compositor + GPU BC encode.
// See vt_compositor.h for the module contract and shaders_vk/vt_composite.comp
// / vt_bc_encode.comp for the GPU passes this records.
//
// WHAT ONE fill() DOES, end to end:
//   1. On the very first call only, record_init() transitions every ring's
//      intermediate images to GENERAL and clears the 1x1 dummy tileset.
//   2. Take the next Ring (kMaxBatchesInFlight of them, round-robin through
//      ring_cursor) and flush that ring's retire list — reaching a ring again
//      is the proof that the batch which last used it has retired.
//   3. Per request: find or build the cached MeshEntry for (variant, rung)
//      (chart + triangle SSBOs plus their set-0 descriptor set), append the
//      page's candidate chart list into the ring's cand buffer, and write one
//      GpuFillRequest into the ring's request buffer — including the
//      weight-seam mode this page will run in.
//   4. In groups of kBatchStride: dispatch vt_composite.comp once per page
//      into the group's intermediate image layer, barrier, dispatch
//      vt_bc_encode.comp once per page into the ring's block buffers,
//      barrier, then copy the three compressed channels and the uncompressed
//      aux layer into each page's destination pool slot.
//
// LIFETIME MODEL. Nothing here waits on a fence; every "is this safe to
// destroy?" question is answered by batch_counter instead. A resource last
// used by batch N is unreferenced once batch N + kMaxBatchesInFlight begins,
// because the caller promises to submit batches in record order and to keep
// at most kMaxBatchesInFlight unretired. All the destruction paths lean on
// exactly that: evict_lru_mesh_entry() destroys in place only outside the
// window, one-shot entries and invalidate_part() park in mesh_retire[] until
// their ring comes round, and a ring's own host-visible buffers are simply
// overwritten when it does.
//
// PROFILING. The steps that have actually hurt are scoped for ProfileLib —
// vt.cpu_prepare (worker), vt.gpu_prepare, vt.mesh_alloc, vt.geometry_upload,
// vt.surface_upload, vt.mesh_entry and vt.candidates. Owned requests finish
// bounded GPU preparation before recording; borrowed clients remain synchronous.

#include "vt_compositor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>

#include "matter/log.h"
#include "profile.h"
#include "shaders_gen/embedded_spirv.h"
// GpuChart / GpuTri and the stream builder: shared with WP-H's enricher so the
// two page passes can never disagree about a texel's owning triangle.
#include "vt_chart_gpu.h"
// P2: the tape packer (GpuSurfOp encoding + field-lane scan) — shared with
// the producers, which compute the per-vertex lane VALUES with the same scan.
#include "vt_surface_tape.h"
#include "vt_prepare.h"
#include "vt_canonical_page.h"
#include "vt_encoded_pages.h"

namespace vt {

// This is the only translation unit that sees both headers, so it is where
// the two independently-declared lane widths are tied together. A mismatch
// would let the packer emit more lanes per vertex than GpuTri's wA/wB/wC rows
// can hold, silently truncating the tail.
static_assert(kVtMaxSurfaceLanes == kGpuTriLanesPerVertex,
              "vt_surface_tape lane width must match GpuTri's per-vertex row");

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
    uint32_t coat[4]; // RGB registers packed in x, coverage y, roughness z
    uint32_t source_a[4]; // RGB and roughness registers
    uint32_t source_b[4]; // metallic, AO, height registers, source version
    float source_range[4]; // height minimum/maximum in metres
    float height_output[4]; // composed min/max, finite binding count, reserved
    float material_origin[4], material_du[4], material_dv[4], material_normal[4];
    uint32_t canonical[4]; // finite candidate offset/count; remaining reserved
    uint32_t periodic[4]; // logical width/height, producer version, coverage-only flag
    uint32_t export_data[4]; // optional physical address, remaining reserved
};
static_assert(sizeof(GpuFillRequest) == 336, "GpuFillRequest layout");

struct GpuVtMaterial {
    float albedo[4];
    float orm[4];
    int32_t slot[4];
};
static_assert(sizeof(GpuVtMaterial) == 48, "GpuVtMaterial layout");

struct VtParamsUbo {
    float tile_size_m[8];        // vec4[2]
    float texels_per_meter[8];   // vec4[2]
    float height_min_m[8],height_max_m[8];
};
static_assert(sizeof(VtParamsUbo) == 128, "VtParamsUbo layout");

// Per-fill() caps, sized to the ring buffers allocated in init(). Requests
// past kMaxRequestsPerFill, and a page whose candidate list would overrun
// kMaxCandEntriesPerFill, are SKIPPED and counted in Stats::requests_skipped —
// never silently truncated to a partial page. kMaxMeshEntries is the mesh
// cache's budget rather than a hard ceiling: a variant arriving past it is
// served either by evicting an LRU entry or by a one-shot entry, so a fill is
// never dropped for cache pressure alone.
constexpr uint32_t kMaxRequestsPerFill = 256;
constexpr uint32_t kMaxCandEntriesPerFill = 65536;
constexpr uint32_t kTilesetArraySize =
    VtCompositor::kMaxDetailSlots * 4;   // slot*4 + (albedo|normal|orm|height)
constexpr uint32_t kMaxMeshEntries = 512;
constexpr uint32_t kMaxPendingMeshEntries = uint32_t(VtCpuPreparer::Limits{}.jobs);

// The instruction arena keeps its existing byte budget. Legacy tapes occupy
// one 96-op block; direct sources can reserve up to six adjacent blocks. Arena
// pressure defers preparation, preserving the previous complete page.
constexpr uint32_t kTapeSlotOps = VtTapeBlockAllocator::kOpsPerBlock;
constexpr uint32_t kTapeArenaSlots =
    kMaxMeshEntries +
    VtCompositor::kMaxBatchesInFlight * kMaxRequestsPerFill + kMaxPendingMeshEntries;

// ---------------------------------------------------------------------------
// Raw resource helpers (this module takes plain Vk handles, so it cannot use
// the VulkanDevice-coupled helpers in vk_resources.h).
// ---------------------------------------------------------------------------
// A buffer plus the device memory it exclusively owns. `mapped` is non-null
// exactly when the buffer was created host-visible; the mapping is made once
// at creation and never explicitly unmapped (freeing the memory in
// destroy_raw_buffer unmaps it). `size` is the size that was requested, which
// may be smaller than the allocation vkGetBufferMemoryRequirements asked for.
struct RawBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
};

// A device-local 2D ARRAY image, its memory, and one view covering all layers.
// Always single-mip. The image layout is not tracked here — the caller's
// barriers own it (see record_init, which parks the intermediates in GENERAL
// for their whole life).
struct RawImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// MATTER_VT_MEM_LOG=1: dump the device's memory heaps and types once.
//
// Diagnostic for the mesh-entry allocation problem
// (docs/vt-mesh-entry-allocation-2026-08-09.md). Entry buffers ask for
// HOST_VISIBLE|HOST_COHERENT with DEVICE_LOCAL *preferred*, which targets the
// BAR heap -- commonly 256 MiB without resizable BAR. The cache is already
// ~155 MiB of triangle buffers at 512 entries, so the suspicion is that the
// "host-visible memory pressure" shed path is BAR exhaustion rather than a
// general shortage. That is a suspicion; this prints the heap sizes and the
// type actually chosen so it stops being one.
void log_memory_heaps_once(const VkPhysicalDeviceMemoryProperties& props) {
    static bool done = false;
    if (done || !std::getenv("MATTER_VT_MEM_LOG")) return;
    done = true;
    for (uint32_t h = 0; h < props.memoryHeapCount; ++h) {
        MATTER_LOGI("vt.mem", "heap %u: %.0f MiB%s\n", h,
                     double(props.memoryHeaps[h].size) / (1024.0 * 1024.0),
                     (props.memoryHeaps[h].flags &
                      VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "");
    }
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = props.memoryTypes[i].propertyFlags;
        MATTER_LOGI("vt.mem", "type %2u -> heap %u %s%s%s\n", i,
                     props.memoryTypes[i].heapIndex,
                     (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL " : "",
                     (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "HOST_VISIBLE " : "",
                     (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "HOST_COHERENT" : "");
    }
}

bool find_memory_type_raw(VkPhysicalDevice phys, uint32_t allowed_bits,
                          VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred, uint32_t& out) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    log_memory_heaps_once(props);
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

// prefer_device_local: for a HOST_VISIBLE buffer, whether to prefer memory
// that is ALSO device-local -- i.e. the BAR heap.
//
// Default true, which is right for the small per-frame ring buffers the GPU
// reads every frame. It is WRONG for the big per-mesh-entry chart/tri buffers,
// and that was the sub-second-hitch bug: measured on this machine the BAR heap
// is 214 MiB total, while the mesh cache alone wants ~155 MiB of it (512
// entries x ~310 KiB of triangles). Allocation therefore failed routinely,
// and every failure ran the shed path -- destroy a batch of entries, retry,
// rebuild them all a moment later. The source already recorded 348.9 / 352.9 /
// 364.5 ms single-call outliers from that path; a flying capture measured
// whole frames at 299-881 ms.
//
// These buffers are written once by the CPU and read by the GPU during the
// fill pass, so they do not need to be device-local at all. Sending them to
// plain host-visible system memory (65 GiB here, against 214 MiB of BAR)
// removes the failures, and with them the shed path.
bool create_raw_buffer(VkDevice device, VkPhysicalDevice phys,
                       VkDeviceSize size, VkBufferUsageFlags usage,
                       bool host_visible, RawBuffer& out, std::string& err,
                       bool prefer_device_local = true) {
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
        (host_visible && prefer_device_local)
            ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0;
    uint32_t type = 0;
    if (!find_memory_type_raw(phys, reqs.memoryTypeBits, required, preferred,
                              type)) {
        err = "vt_compositor: no suitable buffer memory type";
        return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        alloc.pNext = &flags;
    }
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
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        address_info.buffer = out.buffer;
        out.address = vkGetBufferDeviceAddress(device, &address_info);
        if (!out.address) { err = "vt_compositor: zero geometry device address"; return false; }
    }
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

// Barriers the COLOR aspect of mip 0, array layers [0, layers) — every image
// this file creates is single-mip, so that covers the whole resource.
// Barriers the COLOR aspect of mip 0, array layers [0, layers) — every image
// this file creates is single-mip, so that covers the whole resource.
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
// Everything the compositor owns, and every rule about when it may be freed.
// One instance per VtCompositor, built by create() and torn down by ~Impl ->
// destroy(); `device` and `phys` are BORROWED handles. destroy() nulls
// `device` when it finishes so a second call is a no-op. No member is guarded
// by a lock — see the threading note on VtCompositor in vt_compositor.h.
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
    RawBuffer dummy_finite;

    // Setters only change desired CPU inputs. Each retired batch ring captures
    // these once; later setters cannot alter already-recorded GPU work.
    GpuVtMaterial materials[kMaxMaterials]{};
    VtParamsUbo params{};
    uint64_t input_revision = 1;

    // P2: the shared tape-op arena (96-op blocks; see kTapeArenaSlots).
    // Host-visible with device-local preference — the same residency the
    // chart/tri streams ride. Slots are allocated with the mesh entry that
    // owns them and freed on the same retirement paths.
    RawBuffer tape_arena;
    VtTapeBlockAllocator tape_blocks;
    bool tape_gpu_enabled = true;      // MATTER_VT_TAPE_GPU, read at init
    bool tape_overflow_warned = false; // warn-once for the lane-cap fallback

    int32_t tape_slot_acquire(uint32_t op_count) {
        return tape_blocks.acquire(op_count);
    }
    void tape_slot_release(int32_t& slot) {
        tape_blocks.release(slot);
    }

    // One batch's transient resources. kMaxBatchesInFlight of these rotate
    // through ring_cursor. A fill() may overwrite a ring's host-visible
    // request/cand buffers and reuse its intermediates and block buffers only
    // because reaching the ring again means the batch that last used it has
    // retired (see the lifetime model at the top of this file). The
    // intermediate images carry kBatchStride page layers, which is why one
    // fill() splits its requests into groups of kBatchStride.
    struct Ring {
        RawBuffer requests;              // host-visible
        RawBuffer cands;                 // host-visible
        RawBuffer materials;             // immutable until this batch retires
        RawBuffer params;
        uint64_t input_revision = 0;
        std::shared_ptr<void> source_lifetimes[kMaxDetailSlots][4];
        RawBuffer out_albedo;            // device-local block buffers
        RawBuffer out_normal;
        RawBuffer out_orm;
        RawImage inter_albedo;           // rgba8 arrays, kBatchStride layers
        RawImage inter_normal;
        RawImage inter_orm;
        RawImage inter_aux;
        RawImage inter_height;           // R16_UNORM composed height
        VkDescriptorSet batch_set = VK_NULL_HANDLE;
        VkDescriptorSet encode_set = VK_NULL_HANDLE;
    };
    Ring rings[kMaxBatchesInFlight];
    uint32_t ring_cursor = 0;

    // The GPU streams for one (variant_hash, rung): the chart table, the
    // chart-grouped triangle stream (both built by vt_chart_gpu.h) and the
    // set-0 descriptor set that binds them. It owns all three, plus its arena
    // slot; they are released by flush_mesh_retire(), evict_lru_mesh_entry()
    // or Impl::destroy(). The handles are raw and the struct has no copy
    // control, so entries are always MOVED between mesh_cache and
    // mesh_retire[] — duplicating one would double-free.
    struct GeometryEntry {
        VkDevice device = VK_NULL_HANDLE;
        RawBuffer charts;
        RawBuffer tris;
        VtPreparedCorners corners;
        std::shared_ptr<const VtSurfaceBoundary> boundary;
        uint32_t chart_count = 0;
        uint32_t seed_node_count = 0;
        ~GeometryEntry() {
            destroy_raw_buffer(device, charts);
            destroy_raw_buffer(device, tris);
        }
    };
    struct FinitePayloadEntry {
        VkDevice device=VK_NULL_HANDLE;
        std::shared_ptr<const VtFiniteSources> inputs;
        RawBuffer pixels,levels;
        size_t payload_index=0,payload_offset=0,payload_copied=0,levels_copied=0;
        ~FinitePayloadEntry() {destroy_raw_buffer(device,pixels);destroy_raw_buffer(device,levels);}
    };
    struct FiniteEntry {
        VkDevice device=VK_NULL_HANDLE;
        std::shared_ptr<const VtFiniteSources> inputs;
        std::shared_ptr<FinitePayloadEntry> payload;
        RawBuffer bindings,lookup;
        size_t bindings_copied=0,lookup_copied=0;
        ~FiniteEntry() {destroy_raw_buffer(device,bindings);destroy_raw_buffer(device,lookup);}
    };
    std::map<uint64_t,std::weak_ptr<FiniteEntry>> finite_cache;
    std::map<uint64_t,std::weak_ptr<FinitePayloadEntry>> finite_payload_cache;
    struct MeshEntry {
        // Retired material versions retain this same geometry until their
        // readers retire; the current cache entry survives surface edits.
        std::shared_ptr<GeometryEntry> geometry;
        RawBuffer surface;
        RawBuffer finite_ids;
        std::shared_ptr<FiniteEntry> finite;
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
        uint8_t tape_coat_reg[5] = {255,255,255,255,255};
        terrain_field::SurfaceSource source;
    };
    std::map<VtPreparationKey, MeshEntry> mesh_cache;
    // Pages can outlive the composition cache. Reuse their immutable geometry
    // on a later refill, and include those allocations in the memory census.
    std::map<VtPreparationKey, std::weak_ptr<GeometryEntry>> draw_geometries;
    std::vector<std::weak_ptr<GeometryEntry>> geometry_allocations;
    struct PendingMesh {
        std::shared_ptr<const VtPartSnapshot> input;
        std::shared_ptr<const VtPreparedInputs> prepared;
        MeshEntry entry;
        uint64_t requested_epoch = 0;
        size_t charts_copied = 0, geometry_copied = 0, surface_copied = 0, tape_copied = 0;
        size_t seed_nodes_copied = 0;
        size_t finite_ids_copied=0;
        bool initialized = false, new_geometry = false;
    };
    std::map<VtPreparationKey, PendingMesh> pending_mesh;
    PreparationLimits preparation_limits, desired_preparation_limits;
    GpuPreparationStats gpu_preparation;
    uint64_t preparation_epoch = 0;
    const bool preparation_profile = [] {
        const char* value = std::getenv("MATTER_VT_PREPARATION_PROFILE");
        return value && value[0] == '1';
    }();
    std::chrono::steady_clock::time_point preparation_profile_last{};
    bool reclaimed_this_frame = false;
    VtCpuPreparer cpu_preparer{VtCpuPreparer::Limits{},
        [](const VtPartSnapshot& inputs, VtPreparedCorners reuse, bool tape_gpu,
           VtPreparedInputs& output) {
#if MATTER_PROFILE_ENABLED
            // This callback runs only on the private preparation worker.
            // Untagged threads default to the render lane in ProfileLib.
            matter::profile::set_thread_lane(matter::profile::kLaneWorker);
#endif
            PROFILE_SCOPE("vt.cpu_prepare");
            PROFILE_COUNT("vt.cpu_prepare_vertices", inputs.context.vertex_count);
            return vt_prepare_cpu(*inputs.context.atlas, inputs.context,
                                  std::move(reuse), tape_gpu, output);
        }};
    bool cpu_oversize_warned = false;
    void destroy_unpublished(MeshEntry& entry) {
        if (entry.set) vkFreeDescriptorSets(device, descriptor_pool, 1, &entry.set);
        destroy_raw_buffer(device, entry.surface);
        destroy_raw_buffer(device, entry.finite_ids);
        tape_slot_release(entry.tape_slot);
        entry = MeshEntry{};
    }
    void cancel_gpu_preparation(const VtPreparationKey& key) {
        const auto it = pending_mesh.find(key);
        if (it == pending_mesh.end()) return;
        // These buffers/descriptors have never been recorded into a fill.
        // Shared geometry can still belong to a cached/retired GPU reader.
        destroy_unpublished(it->second.entry);
        pending_mesh.erase(it);
        ++gpu_preparation.cancelled;
    }
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

    // Destroy everything parked for `ring_index`. Only correct at the moment
    // fill() claims that ring again — the entries in it were retired at least
    // kMaxBatchesInFlight batches ago — or during teardown.
    void flush_mesh_retire(uint32_t ring_index) {
        for (MeshEntry& entry : mesh_retire[ring_index]) {
            if (entry.set)
                vkFreeDescriptorSets(device, descriptor_pool, 1, &entry.set);
            destroy_raw_buffer(device, entry.surface);
            destroy_raw_buffer(device, entry.finite_ids);
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
        destroy_raw_buffer(device, victim->second.surface);
        destroy_raw_buffer(device, victim->second.finite_ids);
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
        cpu_preparer.shutdown();
        if (!device) return;
        for (auto& kv : pending_mesh) destroy_unpublished(kv.second.entry);
        pending_mesh.clear();
        for (auto& kv : mesh_cache) {
            destroy_raw_buffer(device, kv.second.surface);
            destroy_raw_buffer(device, kv.second.finite_ids);
        }
        mesh_cache.clear();
        for (uint32_t i = 0; i < kMaxBatchesInFlight; ++i)
            flush_mesh_retire(i);
        for (Ring& r : rings) {
            destroy_raw_buffer(device, r.requests);
            destroy_raw_buffer(device, r.cands);
            destroy_raw_buffer(device, r.materials);
            destroy_raw_buffer(device, r.params);
            destroy_raw_buffer(device, r.out_albedo);
            destroy_raw_buffer(device, r.out_normal);
            destroy_raw_buffer(device, r.out_orm);
            destroy_raw_image(device, r.inter_albedo);
            destroy_raw_image(device, r.inter_normal);
            destroy_raw_image(device, r.inter_orm);
            destroy_raw_image(device, r.inter_aux);
            destroy_raw_image(device, r.inter_height);
        }
        destroy_raw_buffer(device, tape_arena);
        destroy_raw_buffer(device, dummy_finite);
        destroy_raw_image(device, dummy_tileset);
        if (sampler) vkDestroySampler(device, sampler, nullptr);
        if (descriptor_pool)
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        for (Ring& r : rings)
            for (auto& slot : r.source_lifetimes)
                for (auto& source : slot) source.reset();
        for (auto& slot : tileset_slots) slot = VtTilesetSlotViews{};
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
    void capture_inputs(Ring& r);
    bool advance_mesh_entry(PendingMesh& pending, const VtPreparedInputs& prepared,
                            const VtPartContext& ctx, Stats& stats, bool bounded,
                            const char*& why);
    MeshEntry* publish_mesh_entry(const VtPreparationKey& key, MeshEntry& entry,
                                 Stats& stats, uint32_t ring_index);
    MeshEntry* get_or_build_mesh_entry(const VtPreparationKey& key,
                                       const chart_atlas::ChartAtlasRung* atlas,
                                       const VtPartContext* ctx, Stats& stats,
                                       uint32_t ring_index, const char*& why,
                                       const std::shared_ptr<const VtPartSnapshot>& input);
    void record_init(VkCommandBuffer cmd);
};

// One-time CPU-side setup: descriptor set layouts, pipeline layouts, both
// compute pipelines from embedded SPIR-V, the tileset sampler, the dummy
// tileset image, the global material/params/tape-arena buffers (filled with
// neutral defaults so an unset table still composes deterministically) and
// every ring's transient resources and descriptor sets. Fails closed: any
// failure returns false with `err` set, and the half-built Impl is destroyed
// by its own destructor. GPU-side initialization is separate — see
// record_init(), which needs a command buffer.
//
// The binding numbers below are a contract with the shaders; keep them in
// step with shaders_vk/vt_composite.comp and shaders_vk/vt_bc_encode.comp:
//   set 0  mesh_layout   (per MeshEntry): 0 charts, 1 immutable triangles,
//                                         2 surface rows (all SSBOs)
//   set 1  batch_layout  (per Ring):      0 requests, 1 candidate charts,
//                                         2 materials, 3 params UBO,
//                                         4 tileset sampler array, indexed
//                                           slot*4 + albedo|normal|orm|height,
//                                         5-8 intermediate storage images
//                                           (albedo, normal, orm, aux),
//                                         9 tape-op arena, 10 R16 height
//   set 0  encode_layout (per Ring):      0-2 the same three intermediates,
//                                         3-5 the BC block output buffers
// Push constants: composite takes 4 bytes (the request's index into the ring
// request buffer), encode takes 8 (the group slot, written twice).
bool VtCompositor::Impl::init(std::string& err) {
    VkFormatProperties height_format{};
    vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R16_UNORM, &height_format);
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(phys, &features);
    constexpr VkFormatFeatureFlags height_required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (!features.shaderStorageImageExtendedFormats ||
        (height_format.optimalTilingFeatures & height_required) != height_required) {
        err = "vt_compositor: composed height requires R16_UNORM storage and filtered sampling";
        return false;
    }
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
                      binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                      binding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1)},
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
             binding(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
             binding(10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1)},
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

    // P2: the tape-op arena + its block allocator, and the process-wide env
    // gate (vt_types.h — read once, so a session can never mix modes).
    if (!create_raw_buffer(device, phys,
                           VkDeviceSize(kTapeArenaSlots) * kTapeSlotOps *
                               sizeof(VtGpuSurfOp),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                           tape_arena, err))
        return false;
    std::memset(tape_arena.mapped, 0, static_cast<size_t>(tape_arena.size));
    if (!create_raw_buffer(device,phys,sizeof(VtGpuFiniteSource),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           true,dummy_finite,err)) return false;
    std::memset(dummy_finite.mapped,0,size_t(dummy_finite.size));
    tape_blocks.reset(kTapeArenaSlots);
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
        for (uint32_t i = 0; i < kMaxMaterials; ++i) {
            materials[i] = GpuVtMaterial{{0.5f, 0.5f, 0.5f, 1.0f},
                                    {1.0f, 0.8f, 0.0f, 0.0f},
                                    {-1, 0, 0, 0}};
        }
        for (int i = 0; i < 8; ++i) {
            params.tile_size_m[i] = 1.0f;
            params.texels_per_meter[i] = 1024.0f;
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
             (kMaxMeshEntries + oneshot_sets + kMaxPendingMeshEntries) * 8 + kMaxBatchesInFlight * 7},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxBatchesInFlight},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             kMaxBatchesInFlight * kTilesetArraySize},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxBatchesInFlight * 8},
        };
        VkDescriptorPoolCreateInfo info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = kMaxMeshEntries + oneshot_sets + kMaxPendingMeshEntries + ring_sets;
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
        if (!create_raw_buffer(device, phys, sizeof(materials),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                               r.materials, err) ||
            !create_raw_buffer(device, phys, sizeof(params),
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true,
                               r.params, err))
            return false;
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
        if (!create_raw_image_array(device, phys, kPageStore, kPageStore,
                                    kBatchStride, VK_FORMAT_R16_UNORM, inter_usage,
                                    r.inter_height, err))
            return false;
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
        capture_inputs(r);
    }
    return true;
}

void VtCompositor::Impl::write_ring_descriptors(Ring& r) {
    VkDescriptorBufferInfo requests{r.requests.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo cands{r.cands.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo mats{r.materials.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo params_info{r.params.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo tape_ops{tape_arena.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo inter[5] = {
        {VK_NULL_HANDLE, r.inter_albedo.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, r.inter_normal.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, r.inter_orm.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, r.inter_aux.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, r.inter_height.view, VK_IMAGE_LAYOUT_GENERAL},
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
    write_buf(r.batch_set, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &params_info);
    write_buf(r.batch_set, 9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &tape_ops);
    write_img(r.batch_set, 5, &inter[0]);
    write_img(r.batch_set, 6, &inter[1]);
    write_img(r.batch_set, 7, &inter[2]);
    write_img(r.batch_set, 8, &inter[3]);
    write_img(r.batch_set, 10, &inter[4]);
    write_img(r.encode_set, 0, &inter[0]);
    write_img(r.encode_set, 1, &inter[1]);
    write_img(r.encode_set, 2, &inter[2]);
    write_buf(r.encode_set, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &out_bufs[0]);
    write_buf(r.encode_set, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &out_bufs[1]);
    write_buf(r.encode_set, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &out_bufs[2]);
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

// Called only for a new or retired ring, under the same completion contract
// as its request/candidate buffers. Material bytes, source parameters and
// source descriptors remain unchanged for this ring's entire GPU lifetime.
// Source tokens follow that same lifetime, independently of desired-input or
// caller replacement. Null-token channels retain the borrowed-image contract.
void VtCompositor::Impl::capture_inputs(Ring& r) {
    if (r.input_revision == input_revision) return;
    std::memcpy(r.materials.mapped, materials, sizeof(materials));
    std::memcpy(r.params.mapped, &params, sizeof(params));
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
            r.source_lifetimes[slot][ch] =
                slot < tileset_slot_count && views[ch] ? s.lifetimes[ch] : nullptr;
        }
    }
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = r.batch_set;
    write.dstBinding = 4;
    write.descriptorCount = kTilesetArraySize;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = infos;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    r.input_revision = input_revision;
}

// Advance unpublished buffers without exposing partially copied input to the
// GPU. Owned requests use the per-frame allowance; borrowed standalone clients
// run this same builder to completion inside fill(). Neither path submits/waits.
bool VtCompositor::Impl::advance_mesh_entry(PendingMesh& pending,
    const VtPreparedInputs& prepared, const VtPartContext& ctx, Stats& stats,
    bool bounded, const char*& why) {
    PROFILE_SCOPE("vt.gpu_prepare");
    auto& entry = pending.entry;
    if (entry.set) return true;
    using Clock = std::chrono::steady_clock;
    struct Measure {
        GpuPreparationStats& stats;
        bool bounded;
        Clock::time_point start = Clock::now();
        double elapsed() const { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
        ~Measure() {
            if (!bounded) return;
            stats.cpu_ms_this_frame += elapsed();
            stats.peak_cpu_ms = std::max(stats.peak_cpu_ms, stats.cpu_ms_this_frame);
        }
    } measure{gpu_preparation, bounded};
    const auto time_available = [&] {
        return !bounded || preparation_limits.cpu_budget_ms <= 0 ||
            gpu_preparation.cpu_ms_this_frame + measure.elapsed() < preparation_limits.cpu_budget_ms;
    };
    const auto take_allocation = [&] {
        if (!bounded) return true;
        if (!time_available() ||
            gpu_preparation.allocations_this_frame >= preparation_limits.allocations_per_frame) {
            why = "GPU preparation allocation deferred";
            return false;
        }
        ++gpu_preparation.allocations;
        ++gpu_preparation.allocations_this_frame;
        gpu_preparation.peak_allocations_per_frame = std::max(
            gpu_preparation.peak_allocations_per_frame, gpu_preparation.allocations_this_frame);
        return true;
    };
    const auto allocate = [&](RawBuffer& buffer, size_t bytes, bool draw_geometry = false) {
        if (buffer.buffer) return true;
        if (!take_allocation()) return false;
        PROFILE_SCOPE("vt.mesh_alloc");
        std::string error;
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            (draw_geometry ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        if (create_raw_buffer(device, phys, bytes, usage,
                              true, buffer, error, /*prefer_device_local=*/false)) return true;
        destroy_raw_buffer(device, buffer);
        if (bounded) {
            ++gpu_preparation.allocation_failures;
            // Reclaim at most one retired cache entry per frame. Retry on a
            // later frame, preserving all successfully allocated/copied input.
            if (!reclaimed_this_frame) {
                reclaimed_this_frame = true;
                if (evict_lru_mesh_entry(ring_cursor)) ++stats.mesh_cache_evictions;
            }
        }
        why = "GPU preparation buffer allocation failed";
        return false;
    };
    const auto copy = [&](void* destination, const void* source, size_t bytes, size_t& copied) {
        constexpr size_t kCopySlice = 64u * 1024u;
        while (copied < bytes) {
            const size_t remaining = bounded
                ? preparation_limits.upload_bytes_per_frame - std::min(
                    preparation_limits.upload_bytes_per_frame, gpu_preparation.uploaded_bytes_this_frame)
                : bytes - copied;
            if (!remaining || !time_available()) { why = "GPU preparation upload deferred"; return false; }
            const size_t count = std::min({bytes - copied, remaining, kCopySlice});
            std::memcpy(static_cast<uint8_t*>(destination) + copied,
                        static_cast<const uint8_t*>(source) + copied, count);
            copied += count;
            if (bounded) {
                gpu_preparation.uploaded_bytes += count;
                gpu_preparation.uploaded_bytes_this_frame += count;
                gpu_preparation.peak_uploaded_bytes_per_frame = std::max(
                    gpu_preparation.peak_uploaded_bytes_per_frame, gpu_preparation.uploaded_bytes_this_frame);
            }
        }
        return true;
    };

    if (!pending.initialized) {
        if (!prepared.corners || prepared.corners->empty() ||
            (!entry.geometry && (prepared.charts.empty() || prepared.geometry.empty()))) {
            why = "empty prepared geometry";
            return false;
        }
        const auto& tape = prepared.tape;
        if (prepared.tape_state == VtPreparedInputs::Tape::Failed) ++stats.tape_pack_failures;
        if (prepared.tape_state == VtPreparedInputs::Tape::LaneOverflow) {
            ++stats.tape_lane_overflows;
            if (!tape_overflow_warned) {
                tape_overflow_warned = true;
                MATTER_LOGW("vt", "surfaces() tape %016llx exceeds %u field lanes "
                    "(input %d, radius %.3f); using per-vertex weights",
                    static_cast<unsigned long long>(ctx.surface_tape_hash), kVtMaxSurfaceLanes,
                    tape.scan.overflow_code, tape.scan.overflow_radius);
            }
        }
        if (prepared.tape_state == VtPreparedInputs::Tape::Ready) {
            if (entry.tape_slot < 0)
                entry.tape_slot = tape_slot_acquire(static_cast<uint32_t>(tape.ops.size()));
            if (entry.tape_slot < 0) {
                ++stats.tape_pack_failures;
                // Reclaim only readers that already retired, at most once per
                // frame on the bounded preparation path. Never publish a
                // vertex-weight substitute for a recipe we could not upload.
                if (!bounded || !reclaimed_this_frame) {
                    reclaimed_this_frame = true;
                    if (evict_lru_mesh_entry(ring_cursor)) ++stats.mesh_cache_evictions;
                }
                why = "tape arena full";
                return false;
            } else {
                entry.tape_op_count = static_cast<uint32_t>(tape.ops.size());
                entry.tape_lane_count = tape.scan.count;
                std::memcpy(entry.tape_weight_reg, tape.weight_reg, sizeof(entry.tape_weight_reg));
                std::memcpy(entry.tape_tint_reg, tape.tint_reg, sizeof(entry.tape_tint_reg));
                entry.source = tape.source;
                entry.tape_rough_reg = tape.rough_bias_reg;
                entry.tape_wet_reg = tape.wetness_reg;
                entry.tape_metal_reg = tape.metallic_reg;
                std::memcpy(entry.tape_coat_reg,tape.coat_reg,sizeof(entry.tape_coat_reg));
            }
        }
        // Admit the complete tape before creating geometry: retries must not
        // mistake a newly created but unuploaded geometry object for reuse.
        pending.new_geometry = !entry.geometry;
        if (pending.new_geometry) {
            entry.geometry = std::make_shared<GeometryEntry>();
            entry.geometry->device = device;
            entry.geometry->corners = prepared.corners;
            entry.geometry->boundary = prepared.boundary;
            entry.geometry->chart_count = static_cast<uint32_t>(prepared.charts.size());
            entry.geometry->seed_node_count = static_cast<uint32_t>(prepared.seed_nodes.size());
            geometry_allocations.push_back(entry.geometry);
        }
        pending.initialized = true;
    }
    if (pending.new_geometry) {
        const size_t chart_bytes = prepared.charts.size() * sizeof(GpuChart);
        const size_t geometry_bytes = prepared.geometry.size() * sizeof(GpuTriGeometry);
        const size_t seed_bytes = prepared.seed_nodes.size() * sizeof(VtSeedNode);
        if (!allocate(entry.geometry->charts, chart_bytes, true)) return false;
        {
            PROFILE_SCOPE("vt.geometry_upload");
            if (!copy(entry.geometry->charts.mapped, prepared.charts.data(), chart_bytes,
                      pending.charts_copied)) return false;
        }
        if (!allocate(entry.geometry->tris, geometry_bytes + seed_bytes, true)) return false;
        {
            PROFILE_SCOPE("vt.geometry_upload");
            if (!copy(entry.geometry->tris.mapped, prepared.geometry.data(), geometry_bytes,
                      pending.geometry_copied)) return false;
            if (seed_bytes && !copy(static_cast<uint8_t*>(entry.geometry->tris.mapped) + geometry_bytes,
                                   prepared.seed_nodes.data(), seed_bytes, pending.seed_nodes_copied)) return false;
        }
    }
    const auto& surface = entry.tape_slot >= 0 ? prepared.lanes : prepared.weights;
    const size_t surface_bytes = surface.size() * sizeof(GpuTriSurface);
    if (!allocate(entry.surface, surface_bytes)) return false;
    {
        PROFILE_SCOPE("vt.surface_upload");
        if (!copy(entry.surface.mapped, surface.data(), surface_bytes, pending.surface_copied)) return false;
    }
    if (entry.tape_slot >= 0) {
        auto* destination = static_cast<VtGpuSurfOp*>(tape_arena.mapped) + size_t(entry.tape_slot) * kTapeSlotOps;
        if (!copy(destination, prepared.tape.ops.data(), prepared.tape.ops.size() * sizeof(VtGpuSurfOp),
                  pending.tape_copied)) return false;
    }
    if (ctx.finite_sources) {
        if (!entry.finite) {
            const auto hash=ctx.finite_sources->content_hash;
            auto found=finite_cache.find(hash);
            if (found!=finite_cache.end()) entry.finite=found->second.lock();
            if (!entry.finite) {
                for (auto it=finite_cache.begin();it!=finite_cache.end();)
                    if (it->second.expired()) it=finite_cache.erase(it); else ++it;
                entry.finite=std::make_shared<FiniteEntry>();
                entry.finite->device=device;entry.finite->inputs=ctx.finite_sources;
                finite_cache[hash]=entry.finite;
                auto& payload_slot=finite_payload_cache[ctx.finite_sources->payload_hash];
                entry.finite->payload=payload_slot.lock();
                if(!entry.finite->payload) {
                    entry.finite->payload=std::make_shared<FinitePayloadEntry>();
                    entry.finite->payload->device=device;entry.finite->payload->inputs=ctx.finite_sources;
                    payload_slot=entry.finite->payload;
                }
                for(auto it=finite_payload_cache.begin();it!=finite_payload_cache.end();)
                    if(it->second.expired())it=finite_payload_cache.erase(it);else ++it;
            }
        }
        auto &finite=*entry.finite; const auto &inputs=*finite.inputs;
        auto &payload=*finite.payload; const auto &bank=*payload.inputs;
        if (!allocate(payload.pixels,bank.pixel_count*sizeof(surface_stamp::Channels)) ||
            !allocate(payload.levels,bank.levels.size()*sizeof(surface_stamp::Level)) ||
            !allocate(finite.bindings,inputs.bindings.size()*sizeof(VtGpuFiniteSource))) return false;
        while (payload.payload_index<bank.payloads.size()) {
            const auto &pixels=bank.payloads[payload.payload_index]->pixels;
            const size_t bytes=pixels.size()*sizeof(pixels[0]);
            if (!copy(static_cast<uint8_t*>(payload.pixels.mapped)+payload.payload_offset,
                      pixels.data(),bytes,payload.payload_copied)) return false;
            payload.payload_offset+=bytes;payload.payload_copied=0;++payload.payload_index;
        }
        if (!copy(payload.levels.mapped,bank.levels.data(),bank.levels.size()*sizeof(bank.levels[0]),payload.levels_copied) ||
            !copy(finite.bindings.mapped,inputs.bindings.data(),inputs.bindings.size()*sizeof(inputs.bindings[0]),finite.bindings_copied)) return false;
        if (!inputs.lookup.empty() &&
            (!allocate(finite.lookup,inputs.lookup.size()*sizeof(inputs.lookup[0])) ||
             !copy(finite.lookup.mapped,inputs.lookup.data(),inputs.lookup.size()*sizeof(inputs.lookup[0]),finite.lookup_copied))) return false;
        if (!allocate(entry.finite_ids,prepared.finite_ids.size()*sizeof(uint32_t)) ||
            !copy(entry.finite_ids.mapped,prepared.finite_ids.data(),prepared.finite_ids.size()*sizeof(uint32_t),pending.finite_ids_copied)) return false;
    }
    if (!take_allocation()) return false;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptor_pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &mesh_layout;
    if (vkAllocateDescriptorSets(device, &alloc, &entry.set) != VK_SUCCESS) {
        entry.set = VK_NULL_HANDLE;
        if (bounded) ++gpu_preparation.allocation_failures;
        why = "mesh descriptor set allocation failed";
        return false;
    }
    VkDescriptorBufferInfo buffers[8] = {
        {entry.geometry->charts.buffer, 0, VK_WHOLE_SIZE},
        {entry.geometry->tris.buffer, 0, VK_WHOLE_SIZE},
        {entry.surface.buffer, 0, VK_WHOLE_SIZE},
        {entry.finite ? entry.finite->payload->pixels.buffer : dummy_finite.buffer,0,VK_WHOLE_SIZE},
        {entry.finite ? entry.finite->payload->levels.buffer : dummy_finite.buffer,0,VK_WHOLE_SIZE},
        {entry.finite ? entry.finite->bindings.buffer : dummy_finite.buffer,0,VK_WHOLE_SIZE},
        {entry.finite ? entry.finite_ids.buffer : dummy_finite.buffer,0,VK_WHOLE_SIZE},
        {entry.finite && entry.finite->lookup.buffer ? entry.finite->lookup.buffer : dummy_finite.buffer,0,VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[8]{};
    for (uint32_t i = 0; i < 8; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = entry.set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffers[i];
    }
    vkUpdateDescriptorSets(device, 8, writes, 0, nullptr);
    entry.chart_count = entry.geometry->chart_count;
    if (pending.new_geometry) ++stats.geometry_builds;
    else ++stats.geometry_reuses;
    if (entry.tape_slot >= 0) ++stats.tape_mode3_entries;
    ++stats.mesh_cache_builds;
    PROFILE_COUNT("vt.mesh_tris", prepared.corners->size());
    if (bounded) ++gpu_preparation.completed;
    return true;
}

VtCompositor::Impl::MeshEntry* VtCompositor::Impl::publish_mesh_entry(
    const VtPreparationKey& key, MeshEntry& entry, Stats& stats, uint32_t ring_index) {
    entry.last_used_batch = batch_counter;
    draw_geometries[key] = entry.geometry;
    auto found = mesh_cache.find(key);
    if (found != mesh_cache.end()) {
        found->second = std::move(entry); // surface invalidation retained only geometry
        return &found->second;
    }
    if (mesh_cache.size() >= kMaxMeshEntries) {
        if (evict_lru_mesh_entry(ring_index)) ++stats.mesh_cache_evictions;
        else {
            mesh_retire[ring_index].push_back(std::move(entry));
            return &mesh_retire[ring_index].back();
        }
    }
    return &mesh_cache.emplace(key, std::move(entry)).first->second;
}

VtCompositor::Impl::MeshEntry* VtCompositor::Impl::get_or_build_mesh_entry(
    const VtPreparationKey& key, const chart_atlas::ChartAtlasRung* atlas,
    const VtPartContext* ctx, Stats& stats, uint32_t ring_index, const char*& why,
    const std::shared_ptr<const VtPartSnapshot>& input) {
    auto found = mesh_cache.find(key);
    if (found != mesh_cache.end() && found->second.set) {
        found->second.last_used_batch = batch_counter;
        return &found->second;
    }
    if (input) {
        auto pending = pending_mesh.find(key);
        if (pending == pending_mesh.end() || pending->second.input != input || !pending->second.entry.set) {
            why = "GPU preparation pending or superseded";
            return nullptr;
        }
        MeshEntry* entry = nullptr;
        try { entry = publish_mesh_entry(key, pending->second.entry, stats, ring_index); }
        catch (const std::exception&) { why = "mesh publication allocation failed"; return nullptr; }
        cpu_preparer.consume(key);
        // Ownership moved into the cache/retire ring; don't destroy GPU resources.
        pending_mesh.erase(pending);
        return entry;
    }
    VtPreparedInputs prepared;
    PendingMesh pending;
    if (found != mesh_cache.end()) pending.entry.geometry = found->second.geometry;
    else if (const auto live = draw_geometries.find(key); live != draw_geometries.end())
        pending.entry.geometry = live->second.lock();
    try {
        if (!vt_prepare_cpu(*atlas, *ctx, pending.entry.geometry ? pending.entry.geometry->corners
                                                               : VtPreparedCorners{},
                            tape_gpu_enabled, prepared)) {
            why = "chart CPU preparation failed";
            return nullptr;
        }
        if (advance_mesh_entry(pending, prepared, *ctx, stats, false, why))
            return publish_mesh_entry(key, pending.entry, stats, ring_index);
    } catch (const std::exception&) { why = "mesh preparation allocation failed"; }
    destroy_unpublished(pending.entry);
    return nullptr;
}

// One-time GPU-side initialization, recorded into the FIRST fill()'s command
// buffer rather than done at create() time — the compositor is handed no
// queue and never submits anything itself. The caller's promise to submit
// command buffers in record order is what makes deferring it safe. Runs once
// per compositor; init_recorded is the latch.
void VtCompositor::Impl::record_init(VkCommandBuffer cmd) {
    // Intermediates: UNDEFINED -> GENERAL (they stay GENERAL forever; GENERAL
    // is a valid transfer-src layout, which keeps the aux copy barrier-only).
    for (Ring& r : rings) {
        for (RawImage* img : {&r.inter_albedo, &r.inter_normal, &r.inter_orm,
                              &r.inter_aux, &r.inter_height}) {
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
    if (count != 0 && slots == nullptr) {
        err = "vt_compositor: nonzero tileset count requires slots";
        return false;
    }
    for(uint32_t i=0;i<count;++i)if(!std::isfinite(slots[i].height_min_m)||
        !std::isfinite(slots[i].height_max_m)||slots[i].height_max_m<slots[i].height_min_m) {
        err="vt_compositor: invalid tileset physical height range";return false;
    }
    for (uint32_t i = 0; i < kMaxDetailSlots; ++i)
        impl_->tileset_slots[i] =
            (i < count) ? slots[i] : VtTilesetSlotViews{};
    impl_->tileset_slot_count = count;
    for (uint32_t i = 0; i < kMaxDetailSlots; ++i) {
        impl_->params.tile_size_m[i] =
            std::max(impl_->tileset_slots[i].tile_size_m, 1e-4f);
        impl_->params.texels_per_meter[i] =
            std::max(impl_->tileset_slots[i].texels_per_meter, 1e-4f);
        impl_->params.height_min_m[i]=impl_->tileset_slots[i].height_min_m;
        impl_->params.height_max_m[i]=impl_->tileset_slots[i].height_max_m;
    }
    ++impl_->input_revision;
    return true;
}

std::array<uint64_t, 2> VtCompositor::encoded_input_identity() const {
    const auto& im = *impl_;
    std::vector<uint8_t> record;
    const auto word = [&](uint32_t value) { asset_store::push_u32(record, value); };
    const auto wide = [&](uint64_t value) { asset_store::push_u64(record, value); };
    const auto bytes = [&](const void* data, size_t count) {
        const auto hash = asset_store::hash_bytes(data, count);
        wide(count); wide(hash.lo); wide(hash.hi);
    };
    word(encoded::kVersion); word(kVtBakeVersion); word(1); // input identity schema
    bytes(im.materials, sizeof(im.materials)); bytes(&im.params, sizeof(im.params));
    word(uint32_t(im.weight_mode)); word(im.debug_mat_a); word(im.debug_mat_b);
    bytes(&im.debug_blend_start, sizeof(float)); bytes(&im.debug_blend_width, sizeof(float));
    word(im.tape_gpu_enabled ? 1 : 0);
    for (const auto& slot : im.tileset_slots) {
        const uint32_t present = (slot.albedo ? 1u : 0u) | (slot.normal ? 2u : 0u) |
            (slot.orm ? 4u : 0u) | (slot.height ? 8u : 0u);
        word(present);
        if (present && !(slot.pixel_hash[0] || slot.pixel_hash[1])) return {};
        wide(present ? slot.pixel_hash[0] : 0); wide(present ? slot.pixel_hash[1] : 0);
    }
    // Shader updates invalidate pixels automatically, including transitive GLSL
    // includes. The CPU preparation policy still carries kVtBakeVersion above.
    static const auto shader_identity = [] {
        std::vector<uint8_t> identity;
        for (const char* name : {"vt_composite.comp.spv", "vt_bc_encode.comp.spv"}) {
            const auto shader = matter::find_spirv(name);
            if (!shader.words || !shader.word_count) return asset_store::BlobHash{};
            const auto hash = asset_store::hash_bytes(shader.words, shader.word_count*sizeof(uint32_t));
            asset_store::push_u64(identity, hash.lo); asset_store::push_u64(identity, hash.hi);
        }
        return asset_store::hash_bytes(identity.data(), identity.size());
    }();
    if (!shader_identity.valid()) return {};
    wide(shader_identity.lo); wide(shader_identity.hi);
    const auto hash = asset_store::hash_bytes(record.data(), record.size());
    return {hash.lo, hash.hi};
}

void VtCompositor::set_materials(const VtCompositorMaterial* materials,
                                 uint32_t count) {
    auto* mats = impl_->materials;
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
        mats[i].slot[1] = m.height_from_top ? 1 : 0;
        mats[i].slot[2] = mats[i].slot[3] = 0;
    }
    ++impl_->input_revision;
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
    impl_->cpu_preparer.cancel_part(variant_hash);
    for (auto it = impl_->draw_geometries.begin(); it != impl_->draw_geometries.end();)
        if (it->first.variant_hash == variant_hash) it = impl_->draw_geometries.erase(it); else ++it;
    for (auto it = impl_->pending_mesh.begin(); it != impl_->pending_mesh.end();) {
        const auto key = (it++)->first;
        if (key.variant_hash == variant_hash) impl_->cancel_gpu_preparation(key);
    }
    // Explicit standalone whole-part invalidation. Runtime retirement uses
    // release_preparation's exact owner lifetime. Both preserve earlier GPU
    // readers through the ring of the most recent batch, never ring_cursor
    // itself (which the next fill consumes).
    const uint32_t retire_ring =
        (impl_->ring_cursor + kMaxBatchesInFlight - 1u) % kMaxBatchesInFlight;
    for (auto it = impl_->mesh_cache.begin(); it != impl_->mesh_cache.end();) {
        if (it->first.variant_hash == variant_hash) {
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

void VtCompositor::release_preparation(const VtPreparationKey& key) {
    impl_->cpu_preparer.cancel(key);
    impl_->draw_geometries.erase(key);
    impl_->cancel_gpu_preparation(key);
    const auto it = impl_->mesh_cache.find(key);
    if (it == impl_->mesh_cache.end()) return;
    const uint32_t retire_ring =
        (impl_->ring_cursor + kMaxBatchesInFlight - 1u) % kMaxBatchesInFlight;
    impl_->mesh_retire[retire_ring].push_back(std::move(it->second));
    impl_->mesh_cache.erase(it);
}

void VtCompositor::invalidate_surface(uint64_t variant_hash) {
    impl_->cpu_preparer.cancel_part(variant_hash);
    for (auto it = impl_->pending_mesh.begin(); it != impl_->pending_mesh.end();) {
        const auto key = (it++)->first;
        if (key.variant_hash == variant_hash) impl_->cancel_gpu_preparation(key);
    }
    for (const auto& item : impl_->mesh_cache)
        if (item.first.variant_hash == variant_hash) invalidate_surface(item.first);
}

void VtCompositor::invalidate_surface(const VtPreparationKey& key) {
    impl_->cpu_preparer.cancel(key);
    impl_->cancel_gpu_preparation(key);
    const auto it = impl_->mesh_cache.find(key);
    if (it == impl_->mesh_cache.end() || !it->second.set) return;
    const uint32_t retire_ring =
        (impl_->ring_cursor + kMaxBatchesInFlight - 1u) % kMaxBatchesInFlight;
    Impl::MeshEntry retired = std::move(it->second);
    it->second = Impl::MeshEntry{};
    it->second.geometry = retired.geometry;
    it->second.last_used_batch = retired.last_used_batch;
    impl_->mesh_retire[retire_ring].push_back(std::move(retired));
}

VtCompositor::PreparationMemory VtCompositor::preparation_memory() const {
    PreparationMemory result;
    std::vector<const Impl::GeometryEntry*> geometries;
    std::vector<const Impl::FiniteEntry*> finite_sources;
    std::vector<const Impl::FinitePayloadEntry*> finite_payloads;
    const auto add = [&](const Impl::MeshEntry& entry) {
        if (entry.surface.buffer) {
            result.surface_gpu_bytes += entry.surface.size;
            ++result.surface_versions;
        }
        result.surface_gpu_bytes+=entry.finite_ids.size;
        if (entry.finite && std::find(finite_sources.begin(),finite_sources.end(),entry.finite.get())==finite_sources.end()) {
            finite_sources.push_back(entry.finite.get());
            result.surface_gpu_bytes+=entry.finite->bindings.size+entry.finite->lookup.size;
            const auto* payload=entry.finite->payload.get();
            if(payload && std::find(finite_payloads.begin(),finite_payloads.end(),payload)==finite_payloads.end()) {
                finite_payloads.push_back(payload);
                result.surface_gpu_bytes+=payload->pixels.size+payload->levels.size;
            }
        }
        const auto* geometry = entry.geometry.get();
        if (!geometry || std::find(geometries.begin(), geometries.end(), geometry) !=
                             geometries.end()) return;
        geometries.push_back(geometry);
        result.geometry_gpu_bytes += geometry->charts.size + geometry->tris.size;
        result.corner_cpu_bytes += geometry->corners ? geometry->corners->capacity() * sizeof(VtTriangleCorners) : 0;
        result.boundary_cpu_bytes += geometry->boundary ? geometry->boundary->bytes() : 0;
        ++result.geometries;
    };
    for (const auto& item : impl_->mesh_cache) add(item.second);
    for (const auto& item : impl_->pending_mesh) add(item.second.entry);
    for (const auto& ring : impl_->mesh_retire)
        for (const auto& entry : ring) add(entry);
    for (const auto& item : impl_->geometry_allocations) {
        Impl::MeshEntry retained;
        retained.geometry = item.lock();
        add(retained);
    }
    return result;
}

void VtCompositor::begin_preparation_frame() {
    auto& im = *impl_;
    // Sample the previous frame before clearing its counters. This diagnostic
    // separates worker backlog from bounded buffer preparation; no file I/O
    // occurs unless explicitly enabled, and output is capped at one row/second.
    if (im.preparation_profile) {
        const auto now = std::chrono::steady_clock::now();
        if (now - im.preparation_profile_last >= std::chrono::seconds(1)) {
            im.preparation_profile_last = now;
            const auto cpu = im.cpu_preparer.stats();
            const auto& gpu = im.gpu_preparation;
            MATTER_LOGI("vt", "preparation epoch=%llu submitted=%llu consumed=%llu cancelled=%llu failed=%llu "
                "cpu_jobs=%zu cpu_bytes=%zu gpu_pending=%zu gpu_completed=%llu allocations=%llu uploaded_bytes=%llu "
                "frame_allocations=%u frame_upload_bytes=%zu frame_cpu_ms=%.3f allocation_limit=%u upload_limit=%zu time_limit_ms=%.3f",
                static_cast<unsigned long long>(im.preparation_epoch),
                static_cast<unsigned long long>(cpu.submitted), static_cast<unsigned long long>(cpu.completed),
                static_cast<unsigned long long>(cpu.cancelled), static_cast<unsigned long long>(cpu.failed),
                cpu.retained_jobs, cpu.reserved_bytes, im.pending_mesh.size(),
                static_cast<unsigned long long>(gpu.completed), static_cast<unsigned long long>(gpu.allocations),
                static_cast<unsigned long long>(gpu.uploaded_bytes), gpu.allocations_this_frame,
                gpu.uploaded_bytes_this_frame, gpu.cpu_ms_this_frame,
                im.preparation_limits.allocations_per_frame, im.preparation_limits.upload_bytes_per_frame,
                im.preparation_limits.cpu_budget_ms);
        }
    }
    im.cpu_preparer.begin_frame();
    ++im.preparation_epoch;
    im.preparation_limits = im.desired_preparation_limits;
    im.gpu_preparation.allocations_this_frame = 0;
    im.gpu_preparation.uploaded_bytes_this_frame = 0;
    im.gpu_preparation.cpu_ms_this_frame = 0;
    im.reclaimed_this_frame = false;
    for (auto it = im.draw_geometries.begin(); it != im.draw_geometries.end();)
        if (it->second.expired()) it = im.draw_geometries.erase(it); else ++it;
    im.geometry_allocations.erase(std::remove_if(im.geometry_allocations.begin(),
        im.geometry_allocations.end(), [](const auto& geometry) { return geometry.expired(); }),
        im.geometry_allocations.end());
    // Missing demand cannot pin all worker/result capacity forever. A request
    // seen in the last four frames retains its partial upload; older work can
    // be reconstructed if it becomes visible again. No published page changes.
    for (auto it = im.pending_mesh.begin(); it != im.pending_mesh.end();) {
        const auto key = it->first;
        const bool abandoned = it->second.requested_epoch + kMaxBatchesInFlight < im.preparation_epoch;
        ++it;
        if (abandoned) {
            im.cancel_gpu_preparation(key);
            im.cpu_preparer.cancel(key);
        }
    }
}

void VtCompositor::set_preparation_limits(PreparationLimits limits) {
    limits.allocations_per_frame = std::min(limits.allocations_per_frame, 64u);
    limits.upload_bytes_per_frame = std::min(limits.upload_bytes_per_frame, size_t(16u * 1024u * 1024u));
    if (!std::isfinite(limits.cpu_budget_ms) || limits.cpu_budget_ms < 0)
        limits.cpu_budget_ms = PreparationLimits{}.cpu_budget_ms;
    impl_->desired_preparation_limits = limits;
}

VtCompositor::GpuPreparationStats VtCompositor::gpu_preparation_stats() const {
    auto result = impl_->gpu_preparation;
    result.pending_jobs = impl_->pending_mesh.size();
    return result;
}

bool VtCompositor::prepare(const VtPreparationKey& key,
                           const std::shared_ptr<const VtPartSnapshot>& inputs) {
    auto& im = *impl_;
    if (!inputs) return false;
    auto pending = im.pending_mesh.find(key);
    if (pending != im.pending_mesh.end() && pending->second.input != inputs) {
        im.cancel_gpu_preparation(key);
        im.cpu_preparer.cancel(key);
        pending = im.pending_mesh.end();
    }
    const auto found = im.mesh_cache.find(key);
    if (found != im.mesh_cache.end()) found->second.last_used_batch = im.batch_counter + 1;
    if (found != im.mesh_cache.end() && found->second.set) return true;
    if (pending == im.pending_mesh.end()) {
        auto geometry = found == im.mesh_cache.end() ? std::shared_ptr<Impl::GeometryEntry>{}
                                                    : found->second.geometry;
        if (!geometry) {
            const auto live = im.draw_geometries.find(key);
            if (live != im.draw_geometries.end()) geometry = live->second.lock();
        }
        const auto corners = geometry ? geometry->corners : VtPreparedCorners{};
        if (!im.cpu_preparer.prepare(key, inputs, corners, im.tape_gpu_enabled)) {
            if (!im.cpu_oversize_warned && im.cpu_preparer.stats().oversized) {
                im.cpu_oversize_warned = true;
                MATTER_LOGW("vt", "CPU preparation exceeds the %zu MiB staging limit for variant %016llx; "
                    "retaining current/fallback coverage. Further rejections are counted.",
                    VtCpuPreparer::Limits{}.bytes / (1024u * 1024u),
                    static_cast<unsigned long long>(key.variant_hash));
            }
            return false;
        }
        if (im.pending_mesh.size() >= kMaxPendingMeshEntries) return false;
        try {
            pending = im.pending_mesh.try_emplace(key).first;
            pending->second.input = inputs;
            pending->second.prepared = im.cpu_preparer.retain_ready(key, inputs);
            pending->second.entry.geometry = std::move(geometry);
        } catch (const std::exception&) { return false; }
    }
    pending->second.requested_epoch = im.preparation_epoch;
    const char* why = "GPU preparation pending";
    const uint64_t builds_before = stats_.mesh_cache_builds;
    bool ready = false;
    try {
        ready = im.advance_mesh_entry(pending->second, *pending->second.prepared,
                                     inputs->context, stats_, true, why);
    } catch (const std::exception&) {
        im.cancel_gpu_preparation(key);
        im.cpu_preparer.cancel(key);
        ++im.gpu_preparation.allocation_failures;
    }
    PROFILE_COUNT("vt.mesh_builds", stats_.mesh_cache_builds - builds_before);
    return ready;
}

VtCompositor::CpuPreparationStats VtCompositor::cpu_preparation_stats() const {
    const auto s = impl_->cpu_preparer.stats();
    return {s.submitted, s.completed, s.cancelled, s.failed, s.deferred, s.oversized,
            s.retained_jobs, s.reserved_bytes, s.peak_bytes};
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
    im.capture_inputs(ring);
    ++im.batch_counter;

    struct Rec {
        const Impl::MeshEntry* entry;
        const VtPoolBinding* pool;
        const VtFillRequest* request;   // for mark_filled() after the copies
        uint32_t req_index;      // slot in the ring's request buffer
        uint32_t physical_slot;
        VtMaterialPixelKey material_key;
    };
    std::vector<Rec> recs;
    recs.reserve(std::min<size_t>(count, kMaxRequestsPerFill));

    auto* gpu_reqs = static_cast<GpuFillRequest*>(ring.requests.mapped);
    auto* gpu_cands = static_cast<uint32_t*>(ring.cands.mapped);
    uint32_t cand_cursor = 0;

    // Residency publishes only successful candidates and retains demand plus
    // prior/fallback coverage on refusal. Log genuine producer failures;
    // ordinary asynchronous preparation deferral is handled before fill().
    static std::atomic<uint32_t> skip_log_budget{32};
    auto log_skip = [&](const VtFillRequest& r, const char* reason) {
        if (skip_log_budget.load(std::memory_order_relaxed) == 0) return;
        if (skip_log_budget.fetch_sub(1, std::memory_order_relaxed) == 0)
            return;
        MATTER_LOGW("vt",
                     "compositor SKIPPED fill (%s): variant=%016llx "
                     "rung=%u mip=%u page=(%u,%u) slot=%u; retaining prior/fallback coverage\n",
                     reason,
                     static_cast<unsigned long long>(r.variant_hash),
                     unsigned(r.rung), unsigned(r.mip), unsigned(r.page_x),
                     unsigned(r.page_y), unsigned(r.physical_slot));
    };

    for (size_t i = 0; i < count; ++i) {
        const VtFillRequest& req = batch[i];
        const VtPoolBinding* pool = req.pool;
        if (!req.atlas || !req.part_context || !pool ||
            !pool->image[kVtChannelAlbedo] || !pool->image[kVtChannelNormal] ||
            !pool->image[kVtChannelOrm] || !pool->image[kVtChannelAux] ||
            !pool->image[kVtChannelHeight] ||
            recs.size() >= kMaxRequestsPerFill) {
            ++stats_.requests_skipped;
            log_skip(req, "missing inputs or batch overflow");
            continue;
        }
        if(pool->uncompressed &&
           (pool->format[kVtChannelAlbedo]!=VK_FORMAT_R8G8B8A8_UNORM ||
            pool->format[kVtChannelNormal]!=VK_FORMAT_R8G8B8A8_UNORM ||
            pool->format[kVtChannelOrm]!=VK_FORMAT_R8G8B8A8_UNORM)) {
            ++stats_.requests_skipped;log_skip(req,"invalid uncompressed export formats");continue;
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
        // Owned inputs publish already-complete preparation here. Borrowed
        // standalone clients still construct their streams synchronously.
        Impl::MeshEntry* entry = nullptr;
        {
            PROFILE_SCOPE("vt.mesh_entry");
            // Synchronous builds are counted here; owned builds are counted
            // by prepare() when their last upload/descriptor step completes.
            const uint64_t builds_before = stats_.mesh_cache_builds;
            entry = im.get_or_build_mesh_entry(req.preparation_key(),
                                               req.atlas, ctx, stats_,
                                               ring_index, why, req.part_snapshot);
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
        g.export_data[0]=uint32_t(req.export_points);g.export_data[1]=uint32_t(req.export_points>>32);
        g.export_data[2]=g.export_data[3]=0;
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
        g.coat[0]=0x00ffffffu;g.coat[1]=g.coat[2]=kVtNoAppearanceReg;g.coat[3]=0;
        std::fill(std::begin(g.source_a), std::end(g.source_a), 0u);
        std::fill(std::begin(g.source_b), std::end(g.source_b), 0u);
        std::fill(std::begin(g.source_range), std::end(g.source_range), 0.f);
        std::fill(std::begin(g.height_output), std::end(g.height_output), 0.f);
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
            g.coat[0]=uint32_t(entry->tape_coat_reg[0]) | (uint32_t(entry->tape_coat_reg[1])<<8) | (uint32_t(entry->tape_coat_reg[2])<<16);
            g.coat[1]=entry->tape_coat_reg[3];g.coat[2]=entry->tape_coat_reg[4];
            if (entry->source.version != 0) {
                for (int i = 0; i < 4; ++i) g.source_a[i] = uint32_t(entry->source.regs[i]);
                for (int i = 0; i < 3; ++i) g.source_b[i] = uint32_t(entry->source.regs[i + 4]);
                g.source_b[3] = entry->source.version;
                g.source_range[0] = entry->source.height_min;
                g.source_range[1] = entry->source.height_max;
            }
            std::memcpy(g.xform, ctx->surface_local_to_world,
                        sizeof(g.xform));
        }
        g.height_output[0]=g.source_range[0];g.height_output[1]=g.source_range[1];
        if(req.export_points && g.source_b[3]==0u) {
            // The normalized height used to choose the material blend stays
            // unchanged. Export additionally retains its physical metre decode.
            for(const auto& material:im.materials) {
                const int slot=material.slot[0];if(slot<0 || slot>=int(im.tileset_slot_count))continue;
                const float datum=material.slot[1]?im.params.height_max_m[slot]:0.f;
                g.height_output[0]=std::min(g.height_output[0],im.params.height_min_m[slot]-datum);
                g.height_output[1]=std::max(g.height_output[1],im.params.height_max_m[slot]-datum);
            }
        }
        if (entry->finite) {
            if (g.source_b[3]!=1u) {
                ++stats_.requests_skipped;log_skip(req,"finite source requires direct base evaluation");continue;
            }
            const auto &source=*entry->finite->inputs;
            g.height_output[0]=std::min(g.height_output[0],source.height_min_m-.00005f);
            g.height_output[1]=std::max(g.height_output[1],source.height_max_m+.00005f);
            g.height_output[2]=float(source.bindings.size());
            g.height_output[3]=float(source.receivers.size());
        }
        std::fill(std::begin(g.material_origin),std::end(g.material_origin),0.f);
        std::fill(std::begin(g.material_du),std::end(g.material_du),0.f);
        std::fill(std::begin(g.material_dv),std::end(g.material_dv),0.f);
        std::fill(std::begin(g.material_normal),std::end(g.material_normal),0.f);
        std::fill(std::begin(g.canonical),std::end(g.canonical),0u);
        std::fill(std::begin(g.periodic),std::end(g.periodic),0u);
        VtCanonicalPage canonical;
        if(ctx->periodic.version) {
            const auto& d=ctx->periodic;
            if(!vt_valid_periodic_domain(d) || g.source_b[3]!=1u || req.mip>vt_periodic_tail_mip(d) ||
               req.page_x>=(std::max(d.width>>req.mip,1u)+127)/128 ||
               req.page_y>=(std::max(d.height>>req.mip,1u)+127)/128) {
                ++stats_.requests_skipped;log_skip(req,"invalid periodic material page");continue;
            }
            g.periodic[0]=d.width;g.periodic[1]=d.height;g.periodic[2]=d.version;
            for(int k=0;k<3;++k) {
                g.material_origin[k]=d.origin[k];g.material_du[k]=d.u[k]*d.period[0];
                g.material_dv[k]=d.v[k]*d.period[1];g.material_normal[k]=d.n[k];
            }
            g.material_origin[3]=2;
        } else if(!req.export_points && g.source_b[3]==1u && cand_count==1 && vt_canonical_page(req,im.scratch_cands[0],
                {g.height_output[0],g.height_output[1]-g.height_output[0],1},canonical) &&
                cand_cursor+canonical.candidates.size()<=kMaxCandEntriesPerFill) {
            std::memcpy(g.material_origin,canonical.origin.data(),sizeof(g.material_origin));
            std::memcpy(g.material_du,canonical.du.data(),sizeof(g.material_du));
            std::memcpy(g.material_dv,canonical.dv.data(),sizeof(g.material_dv));
            std::memcpy(g.material_normal,canonical.normal.data(),sizeof(g.material_normal));
            g.canonical[0]=cand_cursor;g.canonical[1]=uint32_t(canonical.candidates.size());
            for(uint32_t id:canonical.candidates) gpu_cands[cand_cursor++]=id;
        } else canonical.key={};
        if(req.coverage_only && !req.export_points && !ctx->periodic.version &&
           g.source_b[3]==1u && cand_count==1 && ctx->surface_material_count==1) {
            g.periodic[3]=1u;
            canonical.key={};
        }
        recs.push_back(Rec{entry, pool, &req, rec_index, req.physical_slot,canonical.key});
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
            if(gpu_reqs[recs[r].req_index].periodic[3]==1u) continue;
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
            const bool coverage_only=gpu_reqs[rec.req_index].periodic[3]==1u;
            if(!coverage_only && !rec.pool->uncompressed) {
            vkCmdCopyBufferToImage(cmd, ring.out_albedo.buffer,
                                   rec.pool->image[kVtChannelAlbedo],
                                   dst_layout, 1, &region);
            vkCmdCopyBufferToImage(cmd, ring.out_normal.buffer,
                                   rec.pool->image[kVtChannelNormal],
                                   dst_layout, 1, &region);
            vkCmdCopyBufferToImage(cmd, ring.out_orm.buffer,
                                   rec.pool->image[kVtChannelOrm],
                                   dst_layout, 1, &region);
            }

            VkImageCopy aux{};
            aux.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, group_slot, 1};
            aux.srcOffset = {0, 0, 0};
            aux.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, dst_layer, 1};
            aux.dstOffset = {dst_x, dst_y, 0};
            aux.extent = {kPageStore, kPageStore, 1};
            if(!coverage_only && rec.pool->uncompressed) {
                const VkImage raw[]={ring.inter_albedo.image,ring.inter_normal.image,ring.inter_orm.image};
                for(uint32_t channel=0;channel<3;++channel)
                    vkCmdCopyImage(cmd,raw[channel],VK_IMAGE_LAYOUT_GENERAL,
                        rec.pool->image[channel],dst_layout,1,&aux);
            }
            vkCmdCopyImage(cmd, ring.inter_aux.image, VK_IMAGE_LAYOUT_GENERAL,
                           rec.pool->image[kVtChannelAux], dst_layout, 1,
                           &aux);
            if(!coverage_only)
                vkCmdCopyImage(cmd, ring.inter_height.image, VK_IMAGE_LAYOUT_GENERAL,
                               rec.pool->image[kVtChannelHeight], dst_layout, 1, &aux);
            // The page is written: tell the residency layer it may map the
            // indirection entry (vt_types.h VtFillRequest::out_filled). Every
            // `continue` in the request loop above therefore leaves the flag
            // false and the entry unmapped, instead of black.
            const GpuFillRequest& produced = gpu_reqs[rec.req_index];
            VtPageHeight height{};
            if (produced.source_b[3] == 1u || produced.source_b[3] == 2u || rec.request->export_points) {
                height = {produced.height_output[0],
                          produced.height_output[1] - produced.height_output[0], 1u};
            }
            VtDrawGeometry geometry;
            if (height.version == 1u && produced.periodic[2] == 0u) {
                const auto& source = rec.entry->geometry;
                geometry.gpu.charts = source->charts.address;
                geometry.gpu.triangles = source->tris.address;
                geometry.gpu.chart_count = source->chart_count;
                geometry.gpu.triangle_count = static_cast<uint32_t>(source->corners->size());
                geometry.gpu.page_flags=coverage_only?kVtCoverageOnly:0u;
                geometry.gpu.seed_node_count=source->seed_node_count;
                geometry.lifetime = source;
                geometry.boundary = source->boundary;
            }
            rec.request->mark_filled(height, std::move(geometry), rec.material_key);
            if(coverage_only) ++stats_.coverage_pages_filled;
            ++stats_.pages_filled;
        }
    }
}

}  // namespace vt
