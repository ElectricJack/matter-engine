#pragma once

// MatterEngine3/src/render/streamline_bridge.h
//
// StreamlineBridge — the engine's ONE boundary with NVIDIA's proprietary
// Streamline SDK (DLSS super-resolution), plus the plain structs used to
// describe one evaluation. Everything SDK-specific is confined to
// streamline_bridge.cpp: no `sl::` type appears in this header, which is why
// the SDK function pointers below are stored as `void*`.
//
// BUILD GATING. The default build has no Streamline headers, libraries or
// runtime files. Without `MATTER_HAVE_STREAMLINE` every method here compiles
// to a native Vulkan passthrough that reports DLSS unavailable, and the class
// still exists so the caller has no #ifdefs. `MATTER_VK_TEST_FAULT_INJECTION`
// adds the fake-evaluator and missing-proxy test seams at the bottom. The real
// path is Windows-only in practice — it LoadLibraryW's `sl.interposer.dll`.
//
// LIFECYCLE, in the order vk_context.cpp performs it:
//   1. `initialize_before_vulkan()` — BEFORE any Vulkan object exists. Finds
//      and signature-verifies the interposer, slInit's it, and asks for DLSS's
//      feature requirements. The result is moved into VulkanDevice::Impl.
//      `native_fallback(reason)` builds the same object already disabled.
//   2. `append_requirements()` while planning instance and device creation —
//      it merges Streamline's extensions, feature bits and extra queue counts
//      into the caller's lists.
//   3. `create_instance()` / `create_win32_surface()` / `create_device()` and
//      the other WSI wrappers — Vulkan objects are still created natively, but
//      the interposer's manual-hook proxies are acquired and used for the WSI
//      calls Streamline requires.
//   4. `validate_requirements()` during physical-device admission.
//   5. `set_vulkan_info()` after the device exists — this is what finally makes
//      `dlss_available()` true.
//   6. Per frame: `evaluate_dlss()` inside the frame's command buffer, then
//      `present_common(serial)` immediately before `queue_present()`.
//   7. `shutdown()` at device teardown.
//
// FAIL-OPEN IS THE DOCTRINE. Every failure calls `disable(reason)` and the
// renderer continues on the native path with the reason retained in
// `dlss_unavailable_reason()`. The one thing `disable()` will NOT do is unload
// the interposer once a swapchain or surface was created through a proxy —
// those objects must outlive nothing but that library. The owner instead tears
// the whole device down and retries initialization natively, which is what
// `native_retry_required()` and `proxy_dispatch_used()` exist to tell it
// (see VulkanDevice::create in vk_context.cpp).
//
// THREADING. There is no synchronization here at all: the bridge is touched
// only from the thread that owns the Vulkan device and the window.

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "matter/world_session.h"

namespace matter {

struct DlssFloat2 {
    float x = 0.0f;
    float y = 0.0f;
};

// The per-frame camera state DLSS needs to reproject its history. Matrices are
// row-major `float[16]` in the engine's usual convention (matter/math_types.h),
// copied row by row into the SDK's own 4x4 type.
//
// `internal_extent` is what the frame was RENDERED at, `output_extent` what
// DLSS must upscale it to; evaluate_dlss requires output_extent to equal
// DlssOptions::output_extent and the input images to match internal_extent
// exactly. The three booleans describe the caller's velocity buffer, and
// `reset` asks DLSS to discard its accumulated history for this frame (used
// after a camera cut or a resolution change).
struct DlssConstants {
    float camera_view_to_clip[16]{};
    float clip_to_camera_view[16]{};
    float clip_to_prev_clip[16]{};
    float prev_clip_to_clip[16]{};
    DlssFloat2 jitter_offset{};
    DlssFloat2 motion_vector_scale{};
    float camera_position[3]{};
    float camera_up[3]{};
    float camera_right[3]{};
    float camera_forward[3]{};
    float camera_near = 0.0f;
    float camera_far = 0.0f;
    float camera_fov = 0.0f;
    float camera_aspect_ratio = 0.0f;
    bool depth_inverted = false;
    bool camera_motion_included = true;
    bool motion_vectors_jittered = true;
    bool reset = true;
    VkExtent2D internal_extent{};
    VkExtent2D output_extent{};
};

// One image handed to DLSS, described completely enough for the SDK to tag it:
// the VkImage plus the view, the backing memory, and the format/extent/aspect/
// usage. `layout`, `stage` and `access` are the state the CALLER has already
// transitioned the image into before evaluate_dlss records anything — they are
// an assertion about the command buffer, not a request. evaluate_dlss checks
// every one of these fields against an exact contract and refuses the whole
// evaluation on any mismatch.
//
// Non-owning: the renderer owns these images and must keep them alive for the
// submitted frame.
struct DlssResource {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageUsageFlags usage = 0;
    VkImageAspectFlags aspect = 0;
};

struct DlssResources {
    DlssResource hdr{};
    DlssResource depth{};
    DlssResource velocity{};
    DlssResource output{};
};

// What the caller is asking for this frame. `mode` selects the DLSS quality
// preset, or Native for "no upscaling" — passing Native to evaluate_dlss is a
// real transition (it tells the SDK to switch off), not a no-op.
// evaluate_dlss additionally REQUIRES `color_buffers_hdr` and
// `use_auto_exposure` to be true and refuses the evaluation otherwise.
struct DlssOptions {
    DlssMode mode = DlssMode::Native;
    VkExtent2D output_extent{};
    bool color_buffers_hdr = true;
    bool use_auto_exposure = true;
};

struct DlssOptimalSettings {
    VkExtent2D render_extent{};
    float sharpness = 0.0f;
};

// What evaluate_dlss did to the output image, so the caller can barrier
// correctly afterwards. `output_written` false with a true return means DLSS
// was not run for this frame (the Native fast path); the layout/stage/access
// triple is only meaningful when it is true.
struct DlssEvaluationOutput {
    bool output_written = false;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
};

// Keeps the proprietary Streamline SDK at one optional boundary.  The default
// build intentionally has no Streamline headers, libraries, or runtime files.
class StreamlineBridge {
public:
    // The two ways to get a bridge; there is no useful default-constructed one.
    //
    // initialize_before_vulkan() must run before the VkInstance exists, because
    // Streamline's requirements have to reach instance/device creation. It
    // NEVER fails: every problem (no interposer beside the executable, a failed
    // signature check, a missing export, slInit refusing) produces a working
    // bridge that simply reports DLSS unavailable with the reason attached.
    //
    // native_fallback() is the explicitly-disabled bridge used by the native
    // retry after a Streamline attempt failed, and by the Vulkan smoke modes so
    // an ordinary test run never binds a user-installed Streamline runtime.
    static StreamlineBridge initialize_before_vulkan();
    static StreamlineBridge native_fallback(std::string reason);

    // The four state questions, which are NOT interchangeable:
    //   initialized()      — a factory produced this object. True even for the
    //                        native fallback; it says nothing about Streamline.
    //   dlss_requested()   — slInit + slGetFeatureRequirements succeeded, so
    //                        device creation must carry Streamline's extra
    //                        extensions, feature bits and queues.
    //   dlss_available()   — DLSS can actually be evaluated. Only set once
    //                        set_vulkan_info() confirmed adapter support and
    //                        resolved the DLSS entry points; cleared for the
    //                        rest of the session by any evaluation failure.
    //   proxy_dispatch_used() / native_retry_required() — whether a Streamline
    //                        proxy was actually called, and whether the owner
    //                        should tear down and retry natively on failure.
    bool initialized() const { return initialized_; }
    bool dlss_requested() const { return dlss_requested_; }
    bool dlss_available() const { return dlss_available_; }
    const std::string& dlss_unavailable_reason() const {
        return dlss_unavailable_reason_;
    }
    bool proxy_dispatch_used() const { return proxy_dispatch_used_; }
    bool native_retry_required() const { return native_retry_required_; }
    bool supports_dlss_mode(DlssMode mode) const {
        return mode == DlssMode::Native || dlss_available_;
    }
    DlssMode active_dlss_mode() const { return active_dlss_mode_; }
    // Ask DLSS what internal render extent to use for `options.mode`. Native
    // mode short-circuits to render_extent == output_extent and returns true
    // without touching the SDK. On failure `settings` is zeroed and `error`
    // carries either the SDK's reason or the standing unavailability reason —
    // and, unlike evaluate_dlss, a failure here does NOT disable DLSS.
    bool query_dlss_optimal_settings(const DlssOptions& options,
                                     DlssOptimalSettings& settings,
                                     std::string& error) const;
    // Record a DLSS evaluation into `command_buffer` for this frame.
    //
    // FAILURE IS STICKY — this is the sharp edge of the whole class. Any
    // failure path clears dlss_available_ AND dlss_requested_, latches the
    // reason, forces active mode back to Native and flags a history reset, so a
    // single bad frame drops the session to native rendering permanently.
    // Everything below therefore matters:
    //   - `resources` must satisfy an EXACT contract: hdr/output
    //     R16G16B16A16_SFLOAT, depth D32_SFLOAT, velocity R16G16_SFLOAT; the
    //     three inputs at constants.internal_extent and the output at
    //     options.output_extent; inputs in SHADER_READ_ONLY_OPTIMAL and the
    //     output in GENERAL; all four in the compute stage with matching access
    //     masks; four DISTINCT images, each with a view, memory, the right
    //     aspect and non-zero usage. A mismatch is a failure, not a warning.
    //   - `options.color_buffers_hdr`, `options.use_auto_exposure` and
    //     `constants.motion_vectors_jittered` must all be true.
    //   - `options.mode == Native` is the switch-DLSS-OFF transition: it tells
    //     the SDK eOff, records a history reset and returns true. Only a bridge
    //     already in Native mode returns immediately.
    //   - `attempt_token` is passed through to the fault-injection evaluator
    //     only; the real path takes its frame token from slGetNewFrameToken.
    // Feature and output allocations are deliberately RETAINED across a switch
    // to Native so toggling back is fast; free_dlss_resources() releases them.
    bool evaluate_dlss(VkCommandBuffer command_buffer, uint64_t attempt_token,
                       const DlssOptions& options,
                       const DlssConstants& constants,
                       const DlssResources& resources,
                       DlssEvaluationOutput& output, std::string& error);
    // Release the DLSS feature's internal allocations. A no-op returning true
    // when nothing was ever allocated. On success it also drops back to Native
    // and flags a history reset, so the next non-Native frame starts clean.
    bool free_dlss_resources(std::string& error);
    // Read-and-clear: returns whether a history reset was flagged since the
    // last call and clears the flag, so exactly one consumer sees each reset.
    bool consume_dlss_history_reset();

    // Merges the second sequence after the first, preserving first-seen order.
    static std::vector<const char*> merge_extensions(
        const std::vector<const char*>& first,
        const std::vector<const char*>& second);
    // Whether slSetVulkanInfo still has to be called explicitly. When the
    // device was created THROUGH a Streamline proxy the SDK already knows the
    // Vulkan objects, and calling again is redundant; this engine creates
    // Vulkan natively, so in practice the answer is yes.
    static bool requires_explicit_vulkan_info(bool proxy_object_created) {
        return !proxy_object_created;
    }

    // Applies requirements returned by slGetFeatureRequirements before Vulkan
    // availability checks and device creation.  The existing 1.2/1.3 feature
    // structs remain in the caller's pNext chain; only their feature bits grow.
    void append_requirements(std::vector<const char*>& instance_extensions,
                             std::vector<const char*>& device_extensions,
                             VkPhysicalDeviceVulkan12Features& features12,
                             VkPhysicalDeviceVulkan13Features& features13,
                             uint32_t& graphics_queue_count,
                             uint32_t& compute_queue_count) const;
    // Check a candidate physical device against the feature bits Streamline
    // asked for. Trivially true when DLSS was never requested. Used during
    // device admission so a GPU that cannot satisfy Streamline is rejected with
    // a reason rather than failing later at device creation.
    bool validate_requirements(
        const VkPhysicalDeviceVulkan12Features& supported_features12,
        const VkPhysicalDeviceVulkan13Features& supported_features13,
        std::string& error) const;

    // Hand the created Vulkan objects to Streamline, confirm the adapter
    // supports DLSS, and resolve the two DLSS entry points. This is the call
    // that sets dlss_available().
    //
    // ALWAYS RETURNS TRUE: every failure inside disables DLSS with a reason and
    // lets the renderer continue natively, so the bool is not a success signal —
    // query dlss_available() afterwards if you need to know.
    bool set_vulkan_info(VkInstance instance, VkPhysicalDevice physical_device,
                         VkDevice device, uint32_t graphics_queue_family,
                         uint32_t graphics_queue_index,
                         uint32_t compute_queue_family,
                         uint32_t compute_queue_index);
    // Disabling Streamline is always fail-open: the caller continues through
    // the native Vulkan path with no residual feature or queue requirements.
    void disable(std::string reason);
    // Unconditional teardown of the SDK: slShutdown plus FreeLibrary on the
    // interposer. Unlike disable(), it does not consider whether proxy-created
    // objects still exist, so the caller must already have destroyed the
    // swapchain and surface. Safe to call on a bridge that never loaded
    // anything.
    void shutdown();

    // Vulkan creation remains native. Required WSI calls use Streamline's
    // manual-hook proxies after they have been acquired successfully.
    // Both creators call plain vkCreateInstance / vkCreateDevice and then
    // ACQUIRE the interposer's manual-hook proxies for the WSI entry points
    // Streamline requires. If any required proxy is missing the object has
    // already been created, so they disable Streamline and return
    // VK_ERROR_INITIALIZATION_FAILED, which is the owner's cue to tear down and
    // retry the whole initialization natively.
    VkResult create_instance(const VkInstanceCreateInfo* create,
                             const VkAllocationCallbacks* allocator,
                             VkInstance* instance);
    VkResult create_device(VkPhysicalDevice physical_device,
                           const VkDeviceCreateInfo* create,
                           const VkAllocationCallbacks* allocator,
                           VkDevice* device);
    // Present, through Streamline's proxy when proxy dispatch is active.
    //
    // ONE HANDOFF PER FRAME IS ENFORCED HERE: with proxy dispatch on, a present
    // that was not preceded by a matching present_common(serial) returns
    // VK_ERROR_INITIALIZATION_FAILED without presenting anything. Calling
    // present_common twice without an intervening present fails the same way
    // from the other side. The pairing is consumed on every call.
    VkResult queue_present(VkQueue queue, const VkPresentInfoKHR* present);
    // The intercepted present call is where Streamline's common plugin runs
    // presentCommon().  Record the completed frame immediately before that
    // call so the manual-hook path can enforce one handoff per frame.
    bool present_common(uint64_t frame_serial);
    VkResult create_swapchain(VkDevice device,
                              const VkSwapchainCreateInfoKHR* create,
                              const VkAllocationCallbacks* allocator,
                              VkSwapchainKHR* swapchain);
    VkResult get_swapchain_images(VkDevice device, VkSwapchainKHR swapchain,
                                  uint32_t* image_count, VkImage* images);
    void destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
                           const VkAllocationCallbacks* allocator);
    VkResult acquire_next_image(VkDevice device, VkSwapchainKHR swapchain,
                                uint64_t timeout, VkSemaphore semaphore,
                                VkFence fence, uint32_t* image_index);
    VkResult device_wait_idle(VkDevice device);
#ifdef _WIN32
    VkResult create_win32_surface(VkInstance instance,
                                  const VkWin32SurfaceCreateInfoKHR* create,
                                  const VkAllocationCallbacks* allocator,
                                  VkSurfaceKHR* surface);
#endif
    void destroy_surface(VkInstance instance, VkSurfaceKHR surface,
                         const VkAllocationCallbacks* allocator);

#ifdef MATTER_VK_TEST_FAULT_INJECTION
    using TestDlssEvaluator = std::function<bool(
        VkCommandBuffer, uint64_t, const DlssOptions&, const DlssConstants&,
        const DlssResources&, DlssEvaluationOutput&, std::string&)>;
    using TestDlssOptimalEvaluator = std::function<bool(
        const DlssOptions&, DlssOptimalSettings&, std::string&)>;
    static StreamlineBridge test_fake_dlss(
        TestDlssEvaluator evaluator,
        TestDlssOptimalEvaluator optimal_evaluator = {});
    static StreamlineBridge test_missing_proxy(const char* stage);
    uint64_t test_dlss_evaluation_count() const {
        return test_dlss_evaluation_count_;
    }
    uint64_t test_dlss_resource_free_count() const {
        return test_dlss_resource_free_count_;
    }
    const std::vector<std::string>& test_presentation_events() const {
        return test_presentation_events_;
    }
    void clear_test_presentation_events() { test_presentation_events_.clear(); }
    uint64_t test_last_present_common_serial() const {
        return last_present_common_serial_;
    }
#endif

private:
    // State flags, in the order they become true during a successful bring-up.
    //   initialized_             a factory produced this object
    //   dlss_requested_          slInit + requirements succeeded; device
    //                            creation must carry Streamline's demands
    //   dlss_available_          DLSS is actually evaluable (set by
    //                            set_vulkan_info; cleared for good by any
    //                            evaluation failure)
    //   use_proxy_dispatch_      route the WSI calls through the interposer
    //   proxy_dispatch_used_     at least one proxy call really happened
    //   proxy_object_created_    a swapchain or surface came from a proxy, so
    //                            disable() must NOT unload the interposer
    //   native_retry_required_   the owner should retry natively on failure
    //   device_created_by_proxy_ feeds requires_explicit_vulkan_info
    //   present_common_pending_ / present_common_serial_
    //                            the one-handoff-per-frame latch queue_present
    //                            consumes; serial 0 is "none"
    //   dlss_history_reset_pending_  read and cleared by
    //                            consume_dlss_history_reset()
    //   dlss_resources_allocated_    whether free_dlss_resources has work
    bool initialized_ = false;
    bool dlss_requested_ = false;
    bool dlss_available_ = false;
    bool proxy_dispatch_used_ = false;
    bool use_proxy_dispatch_ = false;
    bool proxy_object_created_ = false;
    bool native_retry_required_ = false;
    bool device_created_by_proxy_ = false;
    bool streamline_initialized_ = false;
    bool present_common_pending_ = false;
    bool dlss_history_reset_pending_ = false;
    bool dlss_resources_allocated_ = false;
    DlssMode active_dlss_mode_ = DlssMode::Native;
    uint64_t present_common_serial_ = 0;
    std::string dlss_unavailable_reason_;
    std::vector<const char*> instance_extensions_;
    std::vector<const char*> device_extensions_;
    VkPhysicalDeviceVulkan12Features required_features12_{};
    VkPhysicalDeviceVulkan13Features required_features13_{};
    uint32_t additional_graphics_queues_ = 0;
    uint32_t additional_compute_queues_ = 0;
    // The loaded interposer and its exported entry points, type-erased to
    // `void*` so no `sl::` type reaches this header — that erasure is what
    // keeps the proprietary SDK confined to streamline_bridge.cpp. Each is
    // cast back to its real signature at the call site. All null in a build
    // without MATTER_HAVE_STREAMLINE.
    void* streamline_module_ = nullptr;
    void* sl_init_ = nullptr;
    void* sl_get_feature_requirements_ = nullptr;
    void* sl_set_vulkan_info_ = nullptr;
    void* sl_shutdown_ = nullptr;
    void* sl_is_feature_supported_ = nullptr;
    void* sl_evaluate_feature_ = nullptr;
    void* sl_set_tag_for_frame_ = nullptr;
    void* sl_set_constants_ = nullptr;
    void* sl_get_feature_function_ = nullptr;
    void* sl_get_new_frame_token_ = nullptr;
    void* sl_free_resources_ = nullptr;
    void* sl_dlss_get_optimal_settings_ = nullptr;
    void* sl_dlss_set_options_ = nullptr;
    // Streamline's manual-hook proxies for the Vulkan entry points it has to
    // observe. Acquired through the interposer's own vkGet*ProcAddr, and used
    // only while use_proxy_dispatch_ is set; a null one falls back to the
    // global Vulkan function of the same name.
    PFN_vkGetInstanceProcAddr get_instance_proc_addr_proxy_ = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr_proxy_ = nullptr;
    PFN_vkQueuePresentKHR queue_present_proxy_ = nullptr;
    PFN_vkCreateSwapchainKHR create_swapchain_proxy_ = nullptr;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images_proxy_ = nullptr;
    PFN_vkDestroySwapchainKHR destroy_swapchain_proxy_ = nullptr;
    PFN_vkAcquireNextImageKHR acquire_next_image_proxy_ = nullptr;
    PFN_vkDeviceWaitIdle device_wait_idle_proxy_ = nullptr;
#ifdef _WIN32
    PFN_vkCreateWin32SurfaceKHR create_win32_surface_proxy_ = nullptr;
#endif
    PFN_vkDestroySurfaceKHR destroy_surface_proxy_ = nullptr;

#ifdef MATTER_VK_TEST_FAULT_INJECTION
    TestDlssEvaluator test_dlss_evaluator_;
    TestDlssOptimalEvaluator test_dlss_optimal_evaluator_;
    uint64_t test_dlss_evaluation_count_ = 0;
    uint64_t test_dlss_resource_free_count_ = 0;
    std::vector<std::string> test_presentation_events_;
    uint64_t last_present_common_serial_ = 0;
    enum class TestProxyFault { None, Instance, Device };
    TestProxyFault test_proxy_fault_ = TestProxyFault::None;
    void record_test_presentation_event(const char* event);
#endif

    bool populate_device_proxies(VkDevice device);
};

}  // namespace matter
