#pragma once

// MatterEngine3/src/render/vk_resources.h
//
// Low-level Vulkan resource helpers: move-only RAII wrappers for buffers,
// images and acceleration structures, the memory-type selection they share,
// blocking upload/readback paths, image layout transitions, and the one-shot
// "immediate submit" primitive the rest of the renderer builds on. It also
// exposes the process-wide GPU/process memory counters and the device-address
// registry used to attribute a VK_EXT_device_fault address to an allocation.
//
// How it fits
// -----------
// This is the bottom of MatterEngine3's Vulkan stack. It sits directly on
// `matter/vulkan_device.h` (`VulkanDevice` owns the instance, device and
// queues) and on `vk_device_internal.h` (the device-lifetime and retention
// machinery), and most of `src/render/` allocates through it. It knows
// nothing about the engine's world/part model -- it only speaks Vulkan.
//
// Ownership and lifetime
// ----------------------
// The three `Vk*Resource` structs are move-only handles, not values. Each
// carries a `lifetime` shared_ptr to a `detail::Vk*Allocation` that owns the
// actual Vulkan objects; the plain `buffer`/`image`/`handle`/`memory` fields
// beside it are non-owning mirrors kept for direct use in Vulkan calls.
// Destroying or `reset()`ing a resource drops its reference -- the device
// objects are destroyed only when the last reference goes. That is the
// mechanism that lets a caller release a resource while the GPU is still
// reading it: hand `lifetime` to `submit_immediate`'s `dependencies` and the
// allocation outlives the submission.
//
// Conventions
// -----------
// - Every fallible entry point returns `bool` and writes a human-readable
//   message into the `std::string& error` out-parameter on failure. On
//   `false` the output resource is left empty / unchanged.
// - All sizes, offsets and counts are bytes unless the name says otherwise.
// - Memory selection takes a `required` mask (a hard constraint) and a
//   `preferred` mask (best effort, silently dropped when unavailable).
//
// Gotchas
// -------
// - `submit_immediate`, `upload_buffer`, `readback_buffer`, `transition_image`
//   and `create_acceleration_structure` all block on a fence. They are
//   setup/streaming primitives, not per-frame render-thread calls.
// - `upload_buffer`/`readback_buffer` take a cheap path when the buffer is
//   host-visible (memcpy plus flush, no submit) and an expensive staging path
//   otherwise, so their cost depends on the memory type, not the call site.
// - `create_buffer`/`create_image` make one dedicated `vkAllocateMemory` per
//   resource. There is no sub-allocator here; many small buffers means many
//   allocations.
// - None of the resource wrappers is internally synchronized. The memory
//   counters and the device-address registry are (atomics plus a mutex).

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "matter/vulkan_device.h"

namespace matter {

namespace detail {
struct VkBufferAllocation;
struct VkImageAllocation;
struct VkAccelerationStructureAllocation;
}  // namespace detail

// Process-wide GPU memory accounting. Every vkAllocateMemory in create_buffer
// and create_image is tracked; the corresponding vkFreeMemory decrements.
// Thread-safe (atomic). The breakdown separates device-local (VRAM) from
// host-visible (system RAM mapped for staging).
struct GpuMemoryStats {
    uint64_t device_local_bytes = 0;   // VRAM: DEVICE_LOCAL allocations
    uint64_t host_visible_bytes = 0;   // staging: HOST_VISIBLE allocations
    uint64_t total_bytes = 0;          // sum of the two
    uint64_t allocation_count = 0;     // live vkAllocateMemory calls, both kinds
};
GpuMemoryStats gpu_memory_stats() noexcept;
void gpu_memory_track_alloc(VkDeviceSize bytes, VkMemoryPropertyFlags props);
void gpu_memory_track_free(VkDeviceSize bytes, VkMemoryPropertyFlags props);

// Process memory snapshot (working set on Windows, RSS on Linux).
// Both fields are bytes, and both are zero when the platform query fails.
// `peak_working_set_bytes` is Windows-only -- the Linux path reads
// /proc/self/statm, which carries no peak, and leaves it at 0.
struct ProcessMemoryStats {
    uint64_t working_set_bytes = 0;
    uint64_t peak_working_set_bytes = 0;
};
ProcessMemoryStats process_memory_stats() noexcept;

// Device-fault forensics: matches a faulting GPU VA (from VK_EXT_device_fault)
// against every tracked device-addressable range — buffers created with
// SHADER_DEVICE_ADDRESS usage and acceleration structures — both live and
// recently destroyed. Returns a human-readable summary of the matches.
// `span` is the fault's addressPrecision: the driver reports the address with
// its low bits cleared, so the true faulting address is anywhere in
// [address, address + span) and every range INTERSECTING that window is a
// candidate -- not just the ones containing the reported base.
std::string debug_describe_device_address(uint64_t address, uint64_t span = 1);

// Advances the frame counter those reports quote ages in. Wall-clock ages are
// ambiguous across a stall -- a device-lost fence wait blocks indefinitely --
// so "freed 3 frames before the fault" and "freed 3000 frames before" have to
// be distinguishable. Called once per begin_frame.
void debug_advance_device_address_frame();

// A Vulkan buffer plus its own dedicated device memory, as a move-only RAII
// handle. Produce one with `create_buffer`; `map_buffer`, `flush_buffer`,
// `invalidate_buffer`, `upload_buffer` and `readback_buffer` all operate on
// one.
//
// `lifetime` is the real owner -- it holds the VkBuffer and VkDeviceMemory and
// destroys them in its destructor. The handle fields below are non-owning
// mirrors; never destroy them by hand. Copying is deleted precisely because
// two mirrors of one allocation would double-free. `reset()` (and the
// destructor) only drops this handle's reference, so the Vulkan objects
// survive as long as any submission still holds `lifetime`.
//
// Not internally synchronized: treat an instance as owned by one thread at a
// time.
struct VkBufferResource {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;   // non-owning; `lifetime` owns it
    VkDeviceMemory memory = VK_NULL_HANDLE;  // non-owning; `lifetime` owns it
    VkDeviceSize size = 0;              // bytes requested from create_buffer
    VkDeviceAddress address = 0;        // GPU VA; 0 unless SHADER_DEVICE_ADDRESS
    void* mapped = nullptr;             // host pointer; null until map_buffer

    // Bytes the driver actually allocated (VkMemoryRequirements::size). Always
    // >= `size`, and it is this value -- not `size` -- that flush/invalidate
    // clamp their aligned ranges against.
    VkDeviceSize allocation_size = 0;
    // Flush/invalidate granularity in bytes (the device's nonCoherentAtomSize).
    // 1 means "no constraint". Ranges are aligned down/up to it before the
    // vkFlush/vkInvalidate call.
    VkDeviceSize non_coherent_atom_size = 1;
    // Property flags of the memory type that was actually chosen. Not the
    // requested mask: it may carry extra bits, and it may lack the `preferred`
    // ones. Everything downstream (host-visible fast paths, coherency skips)
    // keys off this field, not off what the caller asked for.
    VkMemoryPropertyFlags memory_properties = 0;
    // Owns the VkBuffer/VkDeviceMemory. Shared so an in-flight submission can
    // keep the allocation alive after the caller has dropped this handle.
    std::shared_ptr<detail::VkBufferAllocation> lifetime;

    VkBufferResource() = default;
    ~VkBufferResource();
    VkBufferResource(const VkBufferResource&) = delete;
    VkBufferResource& operator=(const VkBufferResource&) = delete;
    VkBufferResource(VkBufferResource&& other) noexcept;
    VkBufferResource& operator=(VkBufferResource&& other) noexcept;

    void reset();
};

// A Vulkan image, its default view and its dedicated device memory, as a
// move-only RAII handle. Produced by `create_image`, which always makes a
// single-mip, single-layer, OPTIMAL-tiled image -- there is no path here for
// mip chains or array layers.
//
// Same ownership model as `VkBufferResource`: `lifetime` owns the image, view
// and memory; the handle fields are non-owning mirrors; `reset()` drops a
// reference rather than destroying immediately.
struct VkImageResource {
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;     // non-owning; `lifetime` owns it
    VkImageView view = VK_NULL_HANDLE;  // full-subresource view made at create
    VkDeviceMemory memory = VK_NULL_HANDLE;  // non-owning; `lifetime` owns it
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};                // texels; depth is 1 for 2D images
    // CPU-side bookkeeping of the image's current layout, used as the
    // `oldLayout` of the next barrier. It is written by
    // `record_image_transition` at RECORD time, not at execution time, and it
    // is never queried back from the driver -- so it is only correct if every
    // layout change for this image goes through the helpers in this header.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::shared_ptr<detail::VkImageAllocation> lifetime;

    VkImageResource() = default;
    ~VkImageResource();
    VkImageResource(const VkImageResource&) = delete;
    VkImageResource& operator=(const VkImageResource&) = delete;
    VkImageResource(VkImageResource&& other) noexcept;
    VkImageResource& operator=(VkImageResource&& other) noexcept;

    void reset();
};

// A ray-tracing acceleration structure (BLAS or TLAS) together with the
// backing storage buffer `create_acceleration_structure` allocated for it.
//
// The structure is created EMPTY -- this type carries no build. The caller
// records the `vkCmdBuildAccelerationStructuresKHR` itself and supplies its
// own scratch buffer.
//
// `lifetime` owns the VkAccelerationStructureKHR and also holds a reference to
// the storage buffer's allocation, so the storage cannot be freed out from
// under a live structure. Unlike the buffer and image wrappers there is no
// hand-destroy fallback: `reset()` simply drops `lifetime`.
struct VkAccelerationStructureResource {
    VkDevice device = VK_NULL_HANDLE;
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;        // AS device address; nonzero on success
    VkDeviceSize size = 0;              // bytes of backing storage requested
    std::shared_ptr<detail::VkAccelerationStructureAllocation> lifetime;

    VkAccelerationStructureResource() = default;
    ~VkAccelerationStructureResource();
    VkAccelerationStructureResource(const VkAccelerationStructureResource&) = delete;
    VkAccelerationStructureResource& operator=(const VkAccelerationStructureResource&) = delete;
    VkAccelerationStructureResource(VkAccelerationStructureResource&& other) noexcept;
    VkAccelerationStructureResource& operator=(VkAccelerationStructureResource&& other) noexcept;
    void reset();
};

// Picks a memory type index out of `allowed_type_bits` (a
// VkMemoryRequirements::memoryTypeBits mask).
//
// `required` is a hard constraint; `preferred` is best effort. The first type
// satisfying both wins; otherwise the first type satisfying `required` alone
// is used and the call still returns true -- so a true return does NOT mean
// the `preferred` bits were honoured. `selected_properties` reports the flags
// of whatever was chosen, and callers must key their behaviour off that.
// Returns false only when nothing in the mask satisfies `required`.
bool find_memory_type(VkPhysicalDevice physical_device,
                      uint32_t allowed_type_bits,
                      VkMemoryPropertyFlags required,
                      VkMemoryPropertyFlags preferred,
                      uint32_t& memory_type,
                      VkMemoryPropertyFlags& selected_properties,
                      std::string& error);

// Creates a buffer with its own dedicated `vkAllocateMemory` (no
// sub-allocation), binds it at offset 0, and move-assigns the result into
// `output`, releasing whatever `output` previously held.
//
// `size` must be nonzero. When `usage` includes
// VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT the allocation additionally gets
// VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, `output.address` is filled in, and
// the range is entered into the device-address registry that
// `debug_describe_device_address` searches after a device fault. Successful
// calls also bump the process-wide GPU memory counters.
//
// Safe from any thread; does not submit any GPU work.
bool create_buffer(VulkanDevice& vulkan, VkDeviceSize size,
                   VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags required_memory,
                   VkMemoryPropertyFlags preferred_memory,
                   VkBufferResource& output, std::string& error);

// Persistently maps the whole allocation and caches the pointer in
// `resource.mapped`; a no-op returning true if already mapped. Fails if the
// memory is not HOST_VISIBLE. There is no `unmap_buffer` and no map refcount
// -- the mapping lives until the underlying allocation is destroyed.
bool map_buffer(VkBufferResource& resource, std::string& error);
// Make host writes visible to the device (`flush_buffer`) / device writes
// visible to the host (`invalidate_buffer`) for `[offset, offset + size)`
// bytes of `resource`.
//
// Both require the buffer to be mapped, both return true immediately for
// `size == 0` or HOST_COHERENT memory, and both fail if the range runs past
// `resource.size`. The range handed to Vulkan is widened to
// `non_coherent_atom_size` boundaries, so bytes outside the requested window
// may be flushed/invalidated too -- harmless, but it means these are not
// usable to fence off sub-atom regions against concurrent writers.
bool flush_buffer(VkBufferResource& resource, VkDeviceSize offset,
                  VkDeviceSize size, std::string& error);
bool invalidate_buffer(VkBufferResource& resource, VkDeviceSize offset,
                       VkDeviceSize size, std::string& error);

// Copy `byte_count` bytes of host memory into `destination` at `offset`
// (upload), or out of `source` at `offset` into `data` (readback). `offset`
// is always the byte offset within the DEVICE buffer; the host side is
// contiguous from `data`.
//
// Two very different cost profiles hide behind one signature:
//  - host-visible memory: map, memcpy, flush/invalidate. No GPU submission.
//  - anything else: allocate a temporary staging buffer, record a copy, and
//    BLOCK on a fence via `submit_immediate`. Expect milliseconds, and do not
//    call it on the render thread's per-frame path.
// Which path runs is decided by `memory_properties`, i.e. by what
// `find_memory_type` actually chose, not by what the creator asked for.
//
// Both fail (without touching anything) if the range runs past the buffer or
// `data` is null.
bool upload_buffer(VulkanDevice& vulkan, VkBufferResource& destination,
                   const void* data, size_t byte_count, VkDeviceSize offset,
                   std::string& error);
bool readback_buffer(VulkanDevice& vulkan, VkBufferResource& source, void* data,
                     size_t byte_count, VkDeviceSize offset,
                     std::string& error);

// Creates an image, its dedicated memory and a full-subresource view of it.
//
// The image is fixed at 1 mip level, 1 array layer, 1 sample,
// VK_IMAGE_TILING_OPTIMAL, VK_SHARING_MODE_EXCLUSIVE and an initial layout of
// VK_IMAGE_LAYOUT_UNDEFINED -- callers needing mips or array layers must build
// the image themselves. `extent` is in texels and every component must be
// nonzero (depth 1 for 2D). The view type follows `type` (1D/2D/3D) and
// `aspect` selects the view's aspect mask.
//
// The returned image is still in UNDEFINED layout; transition it before use.
bool create_image(VulkanDevice& vulkan, VkImageType type, VkFormat format,
                  VkExtent3D extent, VkImageUsageFlags usage,
                  VkImageAspectFlags aspect,
                  VkMemoryPropertyFlags required_memory,
                  VkImageResource& output, std::string& error);

// Allocates the backing storage buffer and creates an empty acceleration
// structure of `type` over it. `size` is the storage size in bytes, normally
// taken from `vkGetAccelerationStructureBuildSizesKHR`.
//
// Fails with `vulkan.ray_tracing_unavailable_reason()` when the device has no
// ray tracing, so a false return is an expected outcome on RT-less hardware,
// not necessarily a bug. On success `output.address` is nonzero and the range
// is entered into the device-address registry.
//
// This only creates the structure -- the caller records the build and owns the
// scratch buffer.
bool create_acceleration_structure(
    VulkanDevice& vulkan, VkAccelerationStructureTypeKHR type,
    VkDeviceSize size, VkAccelerationStructureResource& output,
    std::string& error);

// Records a single VkImageMemoryBarrier2 into `command_buffer` taking `image`
// from its tracked `image.layout` to `new_layout`, and updates
// `image.layout` immediately -- at RECORD time, not at execution time. Record
// transitions for one image in the same order you intend them to execute, or
// the tracked layout will not match reality.
//
// Covers exactly one mip level and one array layer, matching what
// `create_image` produces.
void record_image_transition(VkCommandBuffer command_buffer,
                             VkImageResource& image,
                             VkImageLayout new_layout,
                             VkPipelineStageFlags2 source_stage,
                             VkAccessFlags2 source_access,
                             VkPipelineStageFlags2 destination_stage,
                             VkAccessFlags2 destination_access,
                             VkImageAspectFlags aspect);

// The standalone form of `record_image_transition`: allocates a throwaway
// command buffer, records the barrier, submits it and BLOCKS until it
// completes. On failure the previously tracked `image.layout` is restored so
// the caller can retry.
bool transition_image(VulkanDevice& vulkan, VkImageResource& image,
                      VkImageLayout new_layout,
                      VkPipelineStageFlags2 source_stage,
                      VkAccessFlags2 source_access,
                      VkPipelineStageFlags2 destination_stage,
                      VkAccessFlags2 destination_access,
                      VkImageAspectFlags aspect, std::string& error);

// Callback that records the work for one `submit_immediate`. It is invoked
// synchronously, between vkBeginCommandBuffer and vkEndCommandBuffer, with
// the `user_data` pointer passed through untouched.
using ImmediateRecordFn = void (*)(VkCommandBuffer, void*);

// Monotonic count of `submit_immediate` calls since process start, incremented
// on entry so failed submissions count too. Diagnostics and tests only -- it
// is the cheap way to assert that a code path did not silently start doing
// blocking submits.
uint64_t immediate_submit_count() noexcept;

// What a `submit_immediate` is for. Purely a label: it names the submission in
// device-fault diagnostics and is the key the fault-injection test hook
// (MATTER_VK_TEST_FORCE_IMMEDIATE_COMPLETED_FAILURE) matches against, so the
// strings these map to are part of the test contract.
enum class ImmediateSubmitPhase {
    staging_upload,      // host -> device staging copy
    staging_readback,    // device -> host staging copy
    image_transition,    // standalone layout barrier
    compute_dispatch,    // one-shot compute work
    raster_submission,   // one-shot graphics work
};
// Records `record` into a fresh transient command buffer, submits it on the
// graphics queue and BLOCKS on a fence until it completes. This is the
// synchronous escape hatch every setup path here uses; it is not for
// per-frame work.
//
// `dependencies` are opaque references (normally `Vk*Resource::lifetime`
// shared_ptrs) kept alive for the duration of the submission. Pass every
// resource the recorded commands touch -- that is what makes it safe for the
// caller to drop its own handle straight after the call.
//
// If completion cannot be PROVEN (device lost, or the fence wait did not come
// back clean) the command pool, the fence and all `dependencies` are handed to
// the device's retention list instead of being destroyed, so nothing is freed
// while the GPU may still be reading it. That memory is only reclaimed when
// the VkDevice itself goes away.
//
// Returns false with `error` set on any failure.
bool submit_immediate(VulkanDevice& vulkan, ImmediateRecordFn record,
                      void* user_data, std::string& error,
                      ImmediateSubmitPhase phase,
                      std::vector<std::shared_ptr<void>> dependencies = {});

}  // namespace matter
