#pragma once

// MatterEngine3/include/matter/vulkan_device.h
//
// The engine's Vulkan device + swapchain owner, and the per-frame handle every
// render path is driven through. This is the ONLY renderer path in the repo:
// the GL/raylib window and render code was deleted, so a MatterEngine3 or
// MatterEditor process creates one VulkanDevice over a GLFW window and hands it
// to the engine through EngineDesc::render_device (matter/engine_context.h),
// which stores it NON-OWNINGLY.
//
// The implementation lives in MatterEngine3/src/render/vk_context.cpp
// (VulkanDevice::Impl). Instance, physical/logical device, graphics queue,
// surface, swapchain, per-frame command buffers and fences, the Streamline
// (DLSS) bridge and the ray-tracing capability probe are all behind that pimpl.
//
// FRAME LOOP. Once per frame, on the thread that owns the window:
//
//     VulkanFrame frame;
//     if (!device.begin_frame(frame, err)) { /* handle; error is latched */ }
//     session.render(cam, frame, opts, err);          // records into frame
//     bool presented = false;
//     device.end_frame(frame, presented, err);        // submits + presents
//     session.finish_vulkan_frame(frame.serial, presented);
//
// Exactly one frame is active at a time. retain_for_frame() and
// readback_swapchain_rgba8() both validate the VulkanFrame they are handed
// against the active one and fail rather than act on a stale copy. Two frames
// are in flight (kFramesInFlight = 2 in vk_context.cpp): begin_frame waits on
// the incoming slot's fence, and that wait is where the resources retained for
// that slot are released.
//
// FAILURE MODEL. No exceptions cross this API — every fallible call returns
// bool and fills a std::string& error. A hard failure POISONS the device: the
// message is latched and every later call returns false with that same latched
// text, so the first failure is what gets reported instead of the cascade
// behind it. There is no un-poisoning; the process must recreate the device.
//
// CAPABILITY PROBES. ray_tracing_available(), dlss_available() and
// wireframe_available() each pair with a *_unavailable_reason() string. The
// reason is the point, not decoration: a silently degraded path that still
// reports "on" is the failure mode these accessors exist to prevent (see the
// wireframe policy comment below for the worked example).

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace matter {

class StreamlineBridge;

// Raw per-device ray-tracing probe, filled during physical-device selection in
// vk_context.cpp. The `*_extension` flags say the extension is present; the
// remaining flags say the corresponding feature bit / format support is there.
//
// EVERY field is a hard requirement: supports_native_ray_tracing() below walks
// them in order and rejects the device at the first false one, naming it in the
// reason string. A default-constructed instance (all false) therefore reads as
// "no ray tracing".
struct VulkanRayTracingCapabilities {
    bool acceleration_structure_extension = false;
    bool ray_tracing_pipeline_extension = false;
    bool deferred_host_operations_extension = false;
    bool spirv_1_4_extension = false;
    bool shader_float_controls_extension = false;
    // Ray query is not optional alongside the pipeline: vol_scatter.comp and
    // the tileset bake shaders declare GL_EXT_ray_query unconditionally, and
    // VkVolumetrics keys its own ray_query_available_ off
    // VulkanDevice::ray_tracing_available(). Treating the two as one
    // capability keeps that predicate honest, so a device that reports native
    // ray tracing can always compile those modules.
    bool ray_query_extension = false;
    bool buffer_device_address = false;
    bool acceleration_structure = false;
    bool ray_tracing_pipeline = false;
    bool ray_query = false;
    bool storage_image_r8 = false;
    bool shader_storage_image_extended_formats = false;
};

// Device limits copied out of VkPhysicalDeviceRayTracingPipelinePropertiesKHR /
// VkPhysicalDeviceAccelerationStructurePropertiesKHR at device creation. The
// RT pipeline uses them to lay out its shader binding table (handle size and
// the two alignments), to bound recursion, and to align acceleration-structure
// scratch buffers. All zero on a device where the probe never ran, so treat 0
// as "unknown", not as a real limit; read them through
// VulkanDevice::ray_tracing_properties() only once ray_tracing_available().
struct VulkanRayTracingProperties {
    uint32_t shader_group_handle_size = 0;
    uint32_t shader_group_handle_alignment = 0;
    uint32_t shader_group_base_alignment = 0;
    uint32_t max_ray_recursion_depth = 0;
    uint32_t min_acceleration_structure_scratch_offset_alignment = 0;
    uint32_t max_shader_group_stride = 0;
    uint32_t max_ray_dispatch_invocation_count = 0;
};

// Per-frame ray-tracing controls, carried in RenderOptions::vulkan_ray_tracing
// (matter/world_session.h) rather than stored on the device — these are what
// the caller ASKS for each frame. What actually happened is reported back in
// FrameStats::vk_rt_available / vk_rt_effective / vk_rt_fallback_reason, so a
// request that the device cannot honour degrades with a reason instead of
// failing the frame.
//
// max_distance and bias are in world metres; samples is rays per pixel;
// debug_view swaps in the RT debug visualization.
struct VulkanRayTracingSettings {
    bool enabled = false;
    float max_distance = 10000.0f;
    float bias = 0.001f;
    uint32_t samples = 1;
    bool debug_view = false;
};

// The single device feature the wireframe view needs
// (VkPhysicalDeviceFeatures::fillModeNonSolid), split out from the device so
// evaluate_wireframe_capabilities() below can be tested without one.
struct VulkanWireframeCapabilities {
    bool fill_mode_non_solid = false;
};

struct VulkanWireframeCapabilityPolicy {
    // This feature is diagnostic-only; it never rejects an otherwise suitable
    // physical device.
    bool device_accepted = true;
    bool wireframe_available = false;
    std::string unavailable_reason;
};

// Wireframe is an editor diagnostic rather than a device admission
// requirement. Keep this policy header-only so compatibility tests can prove
// a fill-only device remains usable without creating a logical device.
//
// The reason string is the whole point of the struct: a device without
// fillModeNonSolid must SAY so. The alternative -- silently drawing the same
// filled frame while the control reads "on" -- is the failure mode this
// policy exists to prevent, because a wireframe view that lies about
// triangle density is worse than no wireframe view at all.
inline VulkanWireframeCapabilityPolicy evaluate_wireframe_capabilities(
    const VulkanWireframeCapabilities& capabilities) {
    VulkanWireframeCapabilityPolicy policy;
    if (!capabilities.fill_mode_non_solid) {
        policy.unavailable_reason =
            "VkPhysicalDeviceFeatures::fillModeNonSolid is not supported";
        return policy;
    }
    policy.wireframe_available = true;
    return policy;
}

bool supports_native_ray_tracing(const VulkanRayTracingCapabilities& capabilities,
                                 std::string& reason);

namespace detail {
class DeviceRetentionAccess;
class DeviceLifetimeAccess;
class DeviceSubmitAccess;
}  // namespace detail

// One acquired, recording swapchain frame. Produced by
// VulkanDevice::begin_frame and consumed by end_frame; passed along to
// retain_for_frame(), readback_swapchain_rgba8() and WorldSession::render in
// between.
//
// It is a plain copyable value, but it is only meaningful while it is the
// device's ACTIVE frame: those calls compare `serial` / `frame_slot` /
// `command_buffer` against the active frame and return an error for anything
// stale, so holding a copy past end_frame() is detected rather than acted on.
struct VulkanFrame {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkImage swapchain_image = VK_NULL_HANDLE;
    VkImageView swapchain_image_view = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    uint32_t image_index = 0;         // acquired swapchain image, [0, image_count)
    uint32_t image_count = 0;         // swapchain images (presentation depth)
    uint32_t frame_slot = 0;          // in-flight resource set, [0, frame_slot_count)
    uint32_t frame_slot_count = 0;    // frames in flight (2)
    VkExtent2D extent{};              // swapchain size in pixels this frame
    uint64_t serial = 0;              // monotonic frame id from 1; also the token
                                      // passed to WorldSession::finish_vulkan_frame
    bool swapchain_recreated = false; // set on the first frame after a swapchain
                                      // rebuild (resize / out-of-date); consumers
                                      // must drop size-dependent and temporal state
};

// Owns the Vulkan instance, physical and logical device, graphics queue,
// surface, swapchain and per-frame command/sync resources for ONE window, and
// hands out VulkanFrames to record into.
//
// Constructed only through create() — the constructor is private, so there is
// no partially initialized device. Non-copyable, held by unique_ptr. Destroy it
// LAST: the destructor tears the device down, so anything still holding VkDevice
// handles (engine, renderers, retained resources) must be gone first, and the
// window must outlive it.
//
// NOT thread-safe. begin_frame / end_frame / submit_and_wait / wait_idle all
// touch the same queue and the same per-frame state; drive them from the single
// thread that owns the window.
//
// Once poisoned by a failure (see the file header) the device stays poisoned:
// every call returns false with the latched message, so a caller that ignores
// one error does not get a second, unrelated-looking one.
//
// The friend detail::Device*Access classes are the internal seams renderers use
// for resource retention, device-lifetime tokens and phase-tagged immediate
// submits without widening this public surface. The MATTER_VK_TEST_FAULT_
// INJECTION block is the fault/observation surface the Vulkan smoke suite
// drives; it is compiled out of production builds.
class VulkanDevice {
public:
    // The only way to build one. `window` must be non-null (a GLFW window with
    // a Vulkan-capable surface); `enable_validation` turns on the validation
    // layers, which is what makes validation_error_count() meaningful.
    // Initialization is attempted through the Streamline (DLSS) bridge first
    // and retried ONCE on plain native Vulkan when Streamline's requirements
    // turn out to be unavailable — a run therefore never fails just because a
    // user-installed Streamline runtime is unusable. Returns nullptr with
    // `error` filled if both attempts fail.
    static std::unique_ptr<VulkanDevice> create(GLFWwindow* window,
                                                 bool enable_validation,
                                                 std::string& error);

    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    // Start the next frame: wait on the incoming slot's fence (which is where
    // the resources retained for that slot are released), recreate the
    // swapchain if the window resized or the last acquire went out of date,
    // acquire an image, begin its command buffer and barrier the image into
    // colour-attachment layout. On success `frame` is the active frame and its
    // command_buffer is open for recording. False means the acquire failed or
    // the device is poisoned; `error` carries the latched message.
    bool begin_frame(VulkanFrame& frame, std::string& error);
    // End and submit the frame's command buffer, resolve any readback queued
    // by readback_swapchain_rgba8(), and present. Must be given the frame
    // begin_frame() produced. The two-argument form discards the presented
    // flag; the three-argument form reports whether the image was actually
    // presented, which WorldSession::finish_vulkan_frame needs in order to
    // resolve (or discard) the temporal candidate recorded for this frame.
    bool end_frame(const VulkanFrame& frame, std::string& error);
    bool end_frame(const VulkanFrame& frame, bool& presented,
                   std::string& error);
    // Keep `resources` alive until the GPU is done with THIS frame: they are
    // dropped at the next begin_frame() that reuses this frame slot, after its
    // fence wait. This is how a renderer safely releases a buffer, image or
    // descriptor set it has just recorded a reference to. Null entries are
    // ignored. `frame` must be the currently active frame — a stale or
    // mismatched one is rejected with an error and nothing is retained, so the
    // caller must not treat a false return as "retained anyway".
    bool retain_for_frame(const VulkanFrame& frame,
                          std::vector<std::shared_ptr<void>> resources,
                          std::string& error);
    // Records a copy of the fully composed swapchain image. The destination is
    // populated by end_frame after GPU completion and normalized to RGBA8.
    //
    // Call between begin_frame() and end_frame() with the active frame, at most
    // ONCE per frame (a second queued readback is an error). `rgba` is written
    // during end_frame(), so it must still be alive at that point. Fails when
    // the swapchain format is not one of the B8G8R8A8 / R8G8B8A8 UNORM/SRGB
    // forms. This is the path behind MATTER_SCREENSHOT and the QA shot tooling.
    bool readback_swapchain_rgba8(const VulkanFrame& frame,
                                  std::vector<uint8_t>& rgba,
                                  std::string& error);
    // Immediate, out-of-frame submission of a one-off command buffer on the
    // graphics queue, blocking on `fence` until it completes. Used for uploads
    // and bake work that cannot wait for the frame loop.
    //
    // `completion_proven` is the important out-param and is valid on failure
    // too: true means the command buffer is provably not pending (it either
    // completed, or the submit itself failed so it never became pending) and
    // its pool may be destroyed; false — which happens only when the fence wait
    // failed — means completion could not be established, and the caller must
    // LEAK the command pool rather than destroy resources the GPU may still be
    // reading. Any failure here also poisons the device.
    bool submit_and_wait(VkCommandBuffer command_buffer, VkFence fence,
                         bool& completion_proven, std::string& error);
    // Block until the device is idle. Unlike everything else here it reports
    // nothing: a failed wait poisons the device and prints to stderr, and a
    // call on an already-poisoned or never-created device is a silent no-op.
    void wait_idle();

    VkInstance instance() const;
    VkPhysicalDevice physical_device() const;
    VkDevice device() const;
    VkQueue graphics_queue() const;
    uint32_t graphics_queue_family() const;
    VkFormat swapchain_format() const;
    uint32_t swapchain_image_count() const;
    bool draw_indirect_first_instance_enabled() const;
    bool multi_draw_indirect_enabled() const;
    bool wireframe_available() const;
    const std::string& wireframe_unavailable_reason() const;
    bool dlss_available() const;
    const std::string& dlss_unavailable_reason() const;
    // Non-owning access for render passes which record Streamline work on this
    // device's command buffers. VulkanDevice remains the sole bridge owner.
    StreamlineBridge& streamline_bridge();
    const StreamlineBridge& streamline_bridge() const;
    bool ray_tracing_available() const;
    const std::string& ray_tracing_unavailable_reason() const;
    const VulkanRayTracingProperties& ray_tracing_properties() const;
    // Validation-layer errors this device has recorded since creation. Always 0
    // when create() was called with enable_validation == false, so a zero here
    // is only evidence of a clean run if validation was actually on — this is
    // the number the Vulkan smoke gate asserts against its baseline.
    uint32_t validation_error_count() const;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    static bool test_present_result_was_presented(VkResult result);
    static uint32_t test_validation_error_total();
    const std::vector<std::string>& test_presentation_events() const;
    void test_clear_presentation_events();
    uint64_t test_last_present_common_serial() const;
#endif

private:
    struct Impl;
    explicit VulkanDevice(std::unique_ptr<Impl> impl);
    friend class detail::DeviceRetentionAccess;
    friend class detail::DeviceLifetimeAccess;
    friend class detail::DeviceSubmitAccess;
    bool submit_and_wait_for_phase(VkCommandBuffer command_buffer,
                                   VkFence fence, bool& completion_proven,
                                   const char* fault_phase,
                                   std::string& error);
    std::unique_ptr<Impl> impl_;
};

}  // namespace matter
