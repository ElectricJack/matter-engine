#ifndef VIEWER_IMAGE_PREVIEW_H
#define VIEWER_IMAGE_PREVIEW_H

// image_preview.h — turn CPU RGBA8 pixels into something ImGui can draw.
//
// The issue reporter needs this twice: to show the frozen full-screen frame you
// drag a selection box over, and to show thumbnails of the shots already in a
// report. ImGui has no "draw this pixel buffer" call — it needs a descriptor
// set — and nothing else in the editor uploads a host buffer to a sampled
// image (the viewport target in ui.cpp is a render target, written by the GPU).
//
// Images are LINEAR-tiled in host-visible memory: a staging buffer plus a
// device-local copy would be faster to sample, but these are a handful of small
// textures drawn once per frame, and the linear path is a mapped memcpy and one
// layout transition instead of a second allocation and a transfer queue dance.
// Linear sampled RGBA8 is checked against the device's format properties, and
// previews are simply skipped if it is unsupported.

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace matter { class VulkanDevice; }

namespace viewer {

// Owns every preview texture in the editor: the Vulkan images, their memory
// and views, and the ImGui descriptor sets that make them drawable.
//
// Lifecycle, in order: configure() with the device, then create() per image,
// collect() once per frame before drawing, destroy()/destroy_all() as previews
// go away, and shutdown() while the VulkanDevice and the ImGui Vulkan backend
// are BOTH still alive. main() keeps one as a stack local, which is exactly why
// shutdown() is not optional — the destructor deliberately leaks rather than
// touching a device that has already been destroyed.
//
// Handles are void* only so imgui.h stays out of this header; under the Vulkan
// backend an ImTextureID is a VkDescriptorSet, which is what create() returns.
// The cache retains ownership of every handle it hands out.
//
// Main/render thread only, and it holds raw Vulkan handles — not meant to be
// copied.
class ImagePreviewCache {
public:
    ~ImagePreviewCache();

    // Point the cache at the device. Call once, before the first create();
    // create() fails with "no device" until it has been called. This only
    // stores the pointer — it releases nothing, so it is not a way to swap
    // devices mid-run (shutdown() first if you ever need that).
    void configure(matter::VulkanDevice* vulkan) { vulkan_ = vulkan; }

    // Uploads RGBA8 (tightly packed, width*height*4 bytes) and returns a handle
    // usable as an ImTextureID — the Vulkan backend's ImTextureID is the
    // VkDescriptorSet. Returns nullptr on failure and sets `error`; a missing
    // preview is a cosmetic loss, never a reason to drop a capture.
    void* create(const std::vector<uint8_t>& rgba, uint32_t width,
                 uint32_t height, std::string& error);

    // Destruction is DEFERRED, and must be. Both callers free textures from the
    // middle of the UI pass — the drag overlay releases the frozen frame on the
    // same frame it drew it, and filing a report drops thumbnails that this
    // frame's shot list already drew. ImGui has those ImTextureIDs recorded in
    // draw lists that are not submitted until ImGui::Render() later in the
    // frame, so freeing the descriptor set inline is a use-after-free that
    // crashes at render time. These queue instead; collect() does the freeing.
    void destroy(void* handle);
    void destroy_all();

    // Frees anything queued long enough ago to be unreferenced. Call once per
    // frame BEFORE any drawing.
    void collect();

    // Release everything and forget the device. Call while the VulkanDevice and
    // the ImGui Vulkan backend are BOTH still alive — this cache is a stack
    // local in main(), so its destructor would otherwise run after
    // vulkan.reset() and touch a dead device (the same hazard main.cpp already
    // documents for bake_lab's workbench). Idempotent; the destructor is a
    // no-op afterwards.
    void shutdown();

private:
    struct Entry {
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    // An entry awaiting collect(). frames_remaining counts down the frames that
    // may still hold this texture in an unsubmitted draw list.
    struct Pending {
        Entry entry;
        int frames_remaining = 0;
    };

    bool ensure_pool_and_sampler(std::string& error);
    void destroy_entry(Entry& entry);

    // Non-owning; set by configure(), cleared by shutdown(). Null means every
    // Vulkan-touching method here is a no-op or an immediate failure.
    matter::VulkanDevice* vulkan_ = nullptr;
    // Created lazily on the first create() and kept for the life of the cache:
    // the sampler is baked into descriptor sets that may still be queued in
    // pending_, so destroy_all() deliberately leaves both alone. shutdown()
    // is what releases them.
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    // Live textures, one per outstanding handle. Looked up linearly by
    // descriptor set in destroy(), which is fine at the handful of previews a
    // report holds.
    std::vector<Entry> entries_;
    // Retired textures still counting down to a safe free; drained by collect().
    std::vector<Pending> pending_;
};

} // namespace viewer

#endif // VIEWER_IMAGE_PREVIEW_H
