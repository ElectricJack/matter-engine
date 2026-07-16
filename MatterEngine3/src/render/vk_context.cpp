#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "matter/vulkan_device.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "vk_device_internal.h"
#include "streamline_bridge.h"

namespace matter {
namespace {

#ifdef MATTER_VK_TEST_FAULT_INJECTION
std::atomic<uint32_t> g_test_validation_error_total{0};
std::atomic<uint32_t> g_test_resource_destroy_call_total{0};
#endif

constexpr uint32_t kFramesInFlight = 2;
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

const char* result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        default: return "unknown VkResult";
    }
}

bool vk_ok(VkResult result, const char* operation, std::string& error) {
    if (result == VK_SUCCESS) return true;
    std::ostringstream out;
    out << operation << " failed: " << result_name(result) << " ("
        << static_cast<int>(result) << ")";
    error = out.str();
    return false;
}

enum class PresentResultState { completed_or_trackable, unchanged, ambiguous };

constexpr PresentResultState present_result_state(VkResult result) {
    switch (result) {
        case VK_SUCCESS:
        case VK_SUBOPTIMAL_KHR:
        case VK_ERROR_OUT_OF_DATE_KHR:
        case VK_ERROR_SURFACE_LOST_KHR:
            return PresentResultState::completed_or_trackable;
        case VK_ERROR_OUT_OF_HOST_MEMORY:
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return PresentResultState::unchanged;
        default:
            return PresentResultState::ambiguous;
    }
}

constexpr bool should_recreate_swapchain_after_present(
    VkResult result, bool acquired_suboptimal) {
    return present_result_state(result) ==
               PresentResultState::completed_or_trackable &&
           (result == VK_ERROR_OUT_OF_DATE_KHR ||
            result == VK_SUBOPTIMAL_KHR || acquired_suboptimal);
}

constexpr bool present_result_was_presented(VkResult result) {
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

static_assert(present_result_state(VK_ERROR_OUT_OF_HOST_MEMORY) ==
              PresentResultState::unchanged);
static_assert(present_result_state(VK_ERROR_DEVICE_LOST) ==
              PresentResultState::ambiguous);
static_assert(should_recreate_swapchain_after_present(VK_SUCCESS, true));
static_assert(should_recreate_swapchain_after_present(
    VK_ERROR_OUT_OF_DATE_KHR, false));
static_assert(!should_recreate_swapchain_after_present(VK_ERROR_DEVICE_LOST,
                                                       true));
static_assert(!should_recreate_swapchain_after_present(
    VK_ERROR_OUT_OF_HOST_MEMORY, true));

constexpr bool destruction_safe_after_wait(VkResult result) {
    return result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST;
}

static_assert(destruction_safe_after_wait(VK_SUCCESS));
static_assert(destruction_safe_after_wait(VK_ERROR_DEVICE_LOST));
static_assert(!destruction_safe_after_wait(VK_TIMEOUT));

std::string join(const std::vector<std::string>& values, const char* separator) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out << separator;
        out << values[i];
    }
    return out.str();
}

std::unordered_set<std::string> instance_extensions() {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> properties(count);
    if (count != 0) {
        vkEnumerateInstanceExtensionProperties(nullptr, &count,
                                               properties.data());
    }
    std::unordered_set<std::string> names;
    for (const auto& property : properties) names.insert(property.extensionName);
    return names;
}

std::unordered_set<std::string> device_extensions(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> properties(count);
    if (count != 0) {
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                             properties.data());
    }
    std::unordered_set<std::string> names;
    for (const auto& property : properties) names.insert(property.extensionName);
    return names;
}

VkDebugUtilsMessengerCreateInfoEXT debug_create_info(void* user_data) {
    VkDebugUtilsMessengerCreateInfoEXT info{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pUserData = user_data;
    return info;
}

struct QueueSelection {
    uint32_t family = std::numeric_limits<uint32_t>::max();
    uint32_t queue_count = 0;
};

QueueSelection find_graphics_present_queue(VkPhysicalDevice device,
                                            VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, queues.data());
    for (uint32_t i = 0; i < count; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present);
        if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && present) {
            return {i, queues[i].queueCount};
        }
    }
    return {};
}

struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> present_modes;
};

bool query_swapchain_support(VkPhysicalDevice device, VkSurfaceKHR surface,
                             SwapchainSupport& support, std::string& error) {
    if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                   device, surface, &support.capabilities),
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", error)) {
        return false;
    }
    uint32_t count = 0;
    VkResult result =
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
    if (!vk_ok(result, "vkGetPhysicalDeviceSurfaceFormatsKHR", error)) return false;
    support.formats.resize(count);
    if (count != 0) {
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            device, surface, &count, support.formats.data());
        if (!vk_ok(result, "vkGetPhysicalDeviceSurfaceFormatsKHR", error)) {
            return false;
        }
    }
    count = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count,
                                                       nullptr);
    if (!vk_ok(result, "vkGetPhysicalDeviceSurfacePresentModesKHR", error)) {
        return false;
    }
    support.present_modes.resize(count);
    if (count != 0) {
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(
            device, surface, &count, support.present_modes.data());
        if (!vk_ok(result, "vkGetPhysicalDeviceSurfacePresentModesKHR", error)) {
            return false;
        }
    }
    return true;
}

VkSurfaceFormatKHR choose_surface_format(
    const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

std::vector<std::string> missing_presentation_blit_features(
    VkPhysicalDevice device, VkFormat swapchain_format) {
    VkFormatProperties2 hdr{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    VkFormatProperties2 present{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    vkGetPhysicalDeviceFormatProperties2(
        device, VK_FORMAT_R16G16B16A16_SFLOAT, &hdr);
    vkGetPhysicalDeviceFormatProperties2(device, swapchain_format, &present);

    std::vector<std::string> missing;
    const VkFormatFeatureFlags hdr_required =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((hdr.formatProperties.optimalTilingFeatures & hdr_required) !=
        hdr_required) {
        missing.emplace_back(
            "VK_FORMAT_R16G16B16A16_SFLOAT optimal-tiling "
            "VK_FORMAT_FEATURE_BLIT_SRC_BIT+"
            "VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT");
    }
    if ((present.formatProperties.optimalTilingFeatures &
         VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0) {
        missing.emplace_back(
            "swapchain format optimal-tiling VK_FORMAT_FEATURE_BLIT_DST_BIT");
    }
    return missing;
}

}  // namespace

bool supports_native_ray_tracing(
    const VulkanRayTracingCapabilities& capabilities, std::string& reason) {
    struct Requirement { bool present; const char* name; };
    const Requirement requirements[] = {
        {capabilities.acceleration_structure_extension,
         VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME},
        {capabilities.ray_tracing_pipeline_extension,
         VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME},
        {capabilities.deferred_host_operations_extension,
         VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME},
        {capabilities.spirv_1_4_extension, VK_KHR_SPIRV_1_4_EXTENSION_NAME},
        {capabilities.shader_float_controls_extension,
         VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME},
        {capabilities.buffer_device_address,
         "VkPhysicalDeviceVulkan12Features::bufferDeviceAddress"},
        {capabilities.acceleration_structure,
         "VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure"},
        {capabilities.ray_tracing_pipeline,
         "VkPhysicalDeviceRayTracingPipelineFeaturesKHR::rayTracingPipeline"},
        {capabilities.storage_image_r8,
         "VK_FORMAT_R8_UNORM storage image support"},
        {capabilities.shader_storage_image_extended_formats,
         "VkPhysicalDeviceFeatures::shaderStorageImageExtendedFormats"},
    };
    for (const auto& requirement : requirements) {
        if (!requirement.present) {
            reason = std::string("native ray tracing unavailable: missing ") +
                     requirement.name;
            return false;
        }
    }
    reason.clear();
    return true;
}

struct VulkanDevice::Impl {
    explicit Impl(StreamlineBridge input_streamline)
        : streamline(std::move(input_streamline)) {}

    struct FrameSlot {
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkFence acquire_fence = VK_NULL_HANDLE;
        bool acquire_fence_pending = false;
        VkFence fence = VK_NULL_HANDLE;
        std::vector<std::shared_ptr<void>> retained;
    };

    GLFWwindow* window = nullptr;
    StreamlineBridge streamline;
    bool validation_enabled = false;
    std::atomic<uint32_t> validation_errors{0};
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::shared_ptr<detail::DeviceAccessToken> device_lifetime;
    PFN_vkReleaseSwapchainImagesEXT release_swapchain_images = nullptr;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    std::vector<VkQueue> streamline_graphics_queues;
    std::vector<VkQueue> streamline_compute_queues;
    uint32_t graphics_queue_family = std::numeric_limits<uint32_t>::max();
    bool draw_indirect_first_instance_enabled = false;
    bool multi_draw_indirect_enabled = false;
    bool ray_tracing_enabled = false;
    std::string ray_tracing_reason;
    VulkanRayTracingProperties ray_tracing_properties{};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent{};
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> swapchain_image_views;
    std::vector<bool> swapchain_image_initialized;
    std::vector<VkSemaphore> render_finished;
    std::vector<VkFence> present_fences;
    std::vector<bool> present_fence_pending;
    std::array<FrameSlot, kFramesInFlight> frames{};
    uint32_t frame_slot = 0;
    uint64_t next_serial = 1;
    bool frame_active = false;
    bool acquired_suboptimal = false;
    bool report_recreated = false;
    bool swapchain_recreate_required = false;
    bool device_poisoned = false;
    bool preserve_external_work = false;
    bool wsi_completion_ambiguous = false;
    detail::DeviceRetainedResource* retained_resources = nullptr;
    std::string poison_error;
    VulkanFrame active_frame{};
    VkBuffer readback_buffer = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    VkDeviceSize readback_size = 0;
    std::vector<uint8_t>* readback_output = nullptr;
    VkFormat readback_format = VK_FORMAT_UNDEFINED;

    void clear_readback() {
        if (readback_buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, readback_buffer, nullptr);
        if (readback_memory != VK_NULL_HANDLE)
            vkFreeMemory(device, readback_memory, nullptr);
        readback_buffer = VK_NULL_HANDLE;
        readback_memory = VK_NULL_HANDLE;
        readback_size = 0;
        readback_output = nullptr;
        readback_format = VK_FORMAT_UNDEFINED;
    }

    bool poison_device(std::string& error, const char* reason) {
        if (!device_poisoned) {
            poison_error = "Vulkan device disabled: ";
            poison_error += reason;
            if (!error.empty()) poison_error += ": " + error;
            device_poisoned = true;
            frame_active = false;
            acquired_suboptimal = false;
            swapchain_recreate_required = true;
        }
        error = poison_error;
        return false;
    }

    bool ensure_healthy(std::string& error) {
        if (!device_poisoned) return true;
        error = poison_error;
        return false;
    }

    bool ensure_frame_resources(std::string& error) {
        if (!ensure_healthy(error)) return false;
        const size_t image_count = swapchain_images.size();
        bool complete = swapchain != VK_NULL_HANDLE && image_count != 0 &&
                        render_finished.size() == image_count &&
                        present_fences.size() == image_count &&
                        present_fence_pending.size() == image_count &&
                        swapchain_image_initialized.size() == image_count;
        for (size_t i = 0; complete && i < image_count; ++i) {
            complete = render_finished[i] != VK_NULL_HANDLE &&
                       present_fences[i] != VK_NULL_HANDLE;
        }
        if (frame_slot >= frames.size()) complete = false;
        if (complete) {
            const FrameSlot& slot = frames[frame_slot];
            complete = slot.command_pool != VK_NULL_HANDLE &&
                       slot.command_buffer != VK_NULL_HANDLE &&
                       slot.image_available != VK_NULL_HANDLE &&
                       slot.acquire_fence != VK_NULL_HANDLE &&
                       slot.fence != VK_NULL_HANDLE;
        }
        if (complete) return true;
        error = "internal Vulkan frame resources are incomplete";
        return poison_device(error, "frame resource invariant failed");
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT message_type,
        const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
        void* user_data) {
        auto* self = static_cast<Impl*>(user_data);
        const bool is_validation =
            (message_type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0;
        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 &&
            is_validation) {
            self->validation_errors.fetch_add(1, std::memory_order_relaxed);
#ifdef MATTER_VK_TEST_FAULT_INJECTION
            g_test_validation_error_total.fetch_add(1,
                                                    std::memory_order_relaxed);
#endif
        }
        std::fprintf(stderr, "Vulkan %s %s: %s\n",
                     is_validation ? "validation" : "loader/general",
                     (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                         ? "ERROR"
                         : "warning",
                     callback_data && callback_data->pMessage
                         ? callback_data->pMessage
                         : "(no message)");
        return VK_FALSE;
    }

    bool initialize(GLFWwindow* input_window, bool enable_validation,
                    std::string& error) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        g_test_validation_error_total.store(0, std::memory_order_relaxed);
#endif
        window = input_window;
        validation_enabled = enable_validation;
        if (!window) {
            error = "VulkanDevice::create requires a non-null GLFWwindow";
            return false;
        }
        return create_instance(error) && create_surface(error) &&
               select_physical_device(error) && create_logical_device(error) &&
               create_swapchain(VK_NULL_HANDLE, error) &&
               create_frame_slots(error);
    }

    bool create_instance(std::string& error) {
        uint32_t loader_version = VK_API_VERSION_1_0;
        const VkResult version_result = vkEnumerateInstanceVersion(&loader_version);
        if (version_result != VK_SUCCESS ||
            loader_version < VK_API_VERSION_1_3) {
            std::ostringstream out;
            out << "Missing Vulkan 1.3 loader capability (reported "
                << VK_VERSION_MAJOR(loader_version) << "."
                << VK_VERSION_MINOR(loader_version) << "."
                << VK_VERSION_PATCH(loader_version) << ")";
            error = out.str();
            return false;
        }

        uint32_t glfw_count = 0;
        const char** glfw_extensions =
            glfwGetRequiredInstanceExtensions(&glfw_count);
        if (!glfw_extensions || glfw_count == 0) {
            error = "GLFW reported no required Vulkan instance extensions";
            return false;
        }
        std::vector<const char*> required_extensions(
            glfw_extensions, glfw_extensions + glfw_count);
        required_extensions.push_back(
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        required_extensions.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        if (validation_enabled) {
            required_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        std::vector<const char*> streamline_device_extensions;
        VkPhysicalDeviceVulkan12Features streamline_features12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features streamline_features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        uint32_t streamline_graphics_queues = 1;
        uint32_t streamline_compute_queues = 0;
        streamline.append_requirements(
            required_extensions, streamline_device_extensions,
            streamline_features12, streamline_features13,
            streamline_graphics_queues, streamline_compute_queues);

        const auto available_extensions = instance_extensions();
        std::vector<std::string> missing;
        for (const char* name : required_extensions) {
            if (available_extensions.count(name) == 0) missing.emplace_back(name);
        }

        std::vector<const char*> layers;
        if (validation_enabled) {
            uint32_t layer_count = 0;
            vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
            std::vector<VkLayerProperties> available_layers(layer_count);
            if (layer_count != 0) {
                vkEnumerateInstanceLayerProperties(&layer_count,
                                                   available_layers.data());
            }
            bool found = false;
            for (const auto& layer : available_layers) {
                found |= std::strcmp(layer.layerName, kValidationLayer) == 0;
            }
            if (!found) missing.emplace_back(kValidationLayer);
            layers.push_back(kValidationLayer);
        }
        if (!missing.empty()) {
            error = "Missing Vulkan instance capabilities: " +
                    join(missing, ", ");
            return false;
        }

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Matter Vulkan";
        app.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        app.pEngineName = "MatterEngine3";
        app.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        app.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        create.enabledExtensionCount =
            static_cast<uint32_t>(required_extensions.size());
        create.ppEnabledExtensionNames = required_extensions.data();
        create.enabledLayerCount = static_cast<uint32_t>(layers.size());
        create.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
        auto debug_info = debug_create_info(this);
        debug_info.pfnUserCallback = debug_callback;
        if (validation_enabled) create.pNext = &debug_info;
        if (!vk_ok(streamline.create_instance(&create, nullptr, &instance),
                   "vkCreateInstance", error)) {
            return false;
        }

        if (validation_enabled) {
            const auto create_debug =
                reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(instance,
                                          "vkCreateDebugUtilsMessengerEXT"));
            if (!create_debug) {
                error = "Missing Vulkan function vkCreateDebugUtilsMessengerEXT";
                return false;
            }
            if (!vk_ok(create_debug(instance, &debug_info, nullptr,
                                    &debug_messenger),
                       "vkCreateDebugUtilsMessengerEXT", error)) {
                return false;
            }
        }
        return true;
    }

    bool create_surface(std::string& error) {
#ifdef _WIN32
        VkWin32SurfaceCreateInfoKHR create{
            VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        create.hinstance = GetModuleHandleW(nullptr);
        create.hwnd = glfwGetWin32Window(window);
        if (!create.hinstance || !create.hwnd) {
            error = "GLFW did not expose a native Win32 window for Vulkan";
            return false;
        }
        return vk_ok(streamline.create_win32_surface(instance, &create, nullptr,
                                                      &surface),
                     "vkCreateWin32SurfaceKHR", error);
#else
        return vk_ok(glfwCreateWindowSurface(instance, window, nullptr, &surface),
                     "glfwCreateWindowSurface", error);
#endif
    }

    std::vector<std::string> missing_device_capabilities(
        VkPhysicalDevice candidate, QueueSelection& queue,
        VkPhysicalDeviceVulkan12Features& features12,
        VkPhysicalDeviceVulkan13Features& features13,
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT& maintenance1) {
        std::vector<std::string> missing;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) {
            missing.emplace_back("Vulkan device API version 1.3");
        }
        const auto extensions = device_extensions(candidate);
        std::vector<const char*> required_extensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
            VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
        };
        std::vector<const char*> streamline_instance_extensions;
        uint32_t required_graphics_queues = 1;
        uint32_t required_compute_queues = 0;
        VkPhysicalDeviceVulkan12Features streamline_features12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features streamline_features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        streamline.append_requirements(
            streamline_instance_extensions, required_extensions,
            streamline_features12, streamline_features13,
            required_graphics_queues, required_compute_queues);
        for (const char* extension : required_extensions) {
            if (extensions.count(extension) == 0) missing.emplace_back(extension);
        }

        queue = find_graphics_present_queue(candidate, surface);
        if (queue.family == std::numeric_limits<uint32_t>::max()) {
            missing.emplace_back("graphics+present queue family");
        } else if (required_graphics_queues + required_compute_queues >
                   queue.queue_count) {
            missing.emplace_back("enough queues for Streamline requirements");
        }

        VkPhysicalDeviceFeatures2 features2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        features13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        maintenance1 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        features2.pNext = &features12;
        features12.pNext = &features13;
        features13.pNext = &maintenance1;
        vkGetPhysicalDeviceFeatures2(candidate, &features2);
        std::string streamline_feature_error;
        if (!streamline.validate_requirements(features12, features13,
                                              streamline_feature_error)) {
            missing.emplace_back(streamline_feature_error);
        }
        if (!features2.features.drawIndirectFirstInstance) {
            missing.emplace_back(
                "VkPhysicalDeviceFeatures::drawIndirectFirstInstance");
        }
        if (!features2.features.multiDrawIndirect) {
            missing.emplace_back("VkPhysicalDeviceFeatures::multiDrawIndirect");
        }
        if (!features12.timelineSemaphore) {
            missing.emplace_back(
                "VkPhysicalDeviceVulkan12Features::timelineSemaphore");
        }
        if (!features13.dynamicRendering) {
            missing.emplace_back(
                "VkPhysicalDeviceVulkan13Features::dynamicRendering");
        }
        if (!features13.synchronization2) {
            missing.emplace_back(
                "VkPhysicalDeviceVulkan13Features::synchronization2");
        }
        if (!features12.bufferDeviceAddress) {
            missing.emplace_back(
                "VkPhysicalDeviceVulkan12Features::bufferDeviceAddress");
        }
        if (!features12.descriptorIndexing) {
            missing.emplace_back(
                "VkPhysicalDeviceVulkan12Features::descriptorIndexing");
        }
        if (!features12.runtimeDescriptorArray) {
            missing.emplace_back(
                "VkPhysicalDeviceVulkan12Features::runtimeDescriptorArray");
        }
        if (!features12.descriptorBindingPartiallyBound) {
            missing.emplace_back(
                "VkPhysicalDeviceVulkan12Features::descriptorBindingPartiallyBound");
        }
        if (!features12.descriptorBindingVariableDescriptorCount) {
            missing.emplace_back("VkPhysicalDeviceVulkan12Features::"
                                 "descriptorBindingVariableDescriptorCount");
        }
        if (!features12.shaderSampledImageArrayNonUniformIndexing) {
            missing.emplace_back("VkPhysicalDeviceVulkan12Features::"
                                 "shaderSampledImageArrayNonUniformIndexing");
        }
        if (!features12.shaderStorageBufferArrayNonUniformIndexing) {
            missing.emplace_back("VkPhysicalDeviceVulkan12Features::"
                                 "shaderStorageBufferArrayNonUniformIndexing");
        }
        if (!maintenance1.swapchainMaintenance1) {
            missing.emplace_back("VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT::"
                                 "swapchainMaintenance1");
        }

        // External memory/semaphore are Vulkan 1.1 core capabilities.  Their
        // Win32 extension names above expose handles; these queries verify that
        // opaque Win32 handles are actually importable and exportable.
        VkPhysicalDeviceExternalBufferInfo buffer_info{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
        buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        buffer_info.handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkExternalBufferProperties buffer_properties{
            VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vkGetPhysicalDeviceExternalBufferProperties(
            candidate, &buffer_info, &buffer_properties);
        const auto memory_features =
            buffer_properties.externalMemoryProperties.externalMemoryFeatures;
        const VkExternalMemoryFeatureFlags required_memory =
            VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
            VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
        if ((memory_features & required_memory) != required_memory) {
            missing.emplace_back(
                "VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT "
                "import+export");
        }

        VkPhysicalDeviceExternalSemaphoreInfo semaphore_info{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
        VkSemaphoreTypeCreateInfo semaphore_type{
            VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        semaphore_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        semaphore_info.pNext = &semaphore_type;
        semaphore_info.handleType =
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkExternalSemaphoreProperties semaphore_properties{
            VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        vkGetPhysicalDeviceExternalSemaphoreProperties(
            candidate, &semaphore_info, &semaphore_properties);
        const VkExternalSemaphoreFeatureFlags required_semaphore =
            VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
            VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
        if ((semaphore_properties.externalSemaphoreFeatures &
             required_semaphore) != required_semaphore) {
            missing.emplace_back(
                "timeline VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT "
                "VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT+"
                "VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT");
        }

        if (extensions.count(VK_KHR_SWAPCHAIN_EXTENSION_NAME) != 0) {
            SwapchainSupport support;
            std::string query_error;
            if (!query_swapchain_support(candidate, surface, support,
                                         query_error)) {
                missing.push_back(query_error);
            } else {
                if (support.formats.empty()) {
                    missing.emplace_back("nonempty surface format list");
                } else {
                    auto format_missing = missing_presentation_blit_features(
                        candidate, choose_surface_format(support.formats).format);
                    missing.insert(missing.end(), format_missing.begin(),
                                   format_missing.end());
                }
                if (support.present_modes.empty()) {
                    missing.emplace_back("nonempty present mode list");
                }
                if ((support.capabilities.supportedUsageFlags &
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
                    missing.emplace_back(
                        "swapchain VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT");
                }
            }
        }
        return missing;
    }

    bool select_physical_device(std::string& error) {
        uint32_t count = 0;
        if (!vk_ok(vkEnumeratePhysicalDevices(instance, &count, nullptr),
                   "vkEnumeratePhysicalDevices", error)) {
            return false;
        }
        if (count == 0) {
            error = "No Vulkan physical devices were reported";
            return false;
        }
        std::vector<VkPhysicalDevice> candidates(count);
        if (!vk_ok(vkEnumeratePhysicalDevices(instance, &count,
                                              candidates.data()),
                   "vkEnumeratePhysicalDevices", error)) {
            return false;
        }

        int best_score = -1;
        std::vector<std::string> rejected;
        for (VkPhysicalDevice candidate : candidates) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            QueueSelection queue;
            VkPhysicalDeviceVulkan12Features features12{};
            VkPhysicalDeviceVulkan13Features features13{};
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance1{};
            auto missing = missing_device_capabilities(
                candidate, queue, features12, features13, maintenance1);
            if (!missing.empty()) {
                rejected.emplace_back(std::string(properties.deviceName) +
                                      ": " + join(missing, ", "));
                continue;
            }
            int score = 0;
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                score = 2000;
            } else if (properties.deviceType ==
                       VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                score = 1000;
            }
            score += static_cast<int>(properties.limits.maxImageDimension2D);
            if (score > best_score) {
                best_score = score;
                physical_device = candidate;
                graphics_queue_family = queue.family;
            }
        }
        if (physical_device == VK_NULL_HANDLE) {
            error = "No Vulkan 1.3 device satisfies required capabilities:\n  " +
                    join(rejected, "\n  ");
            return false;
        }

        VkPhysicalDeviceVulkan12Properties properties12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &properties12;
        vkGetPhysicalDeviceProperties2(physical_device, &properties2);
        const auto& properties = properties2.properties;
        std::printf(
            "Vulkan adapter: %s | driver: %s (%s, 0x%08x) | API %u.%u.%u\n",
            properties.deviceName, properties12.driverName,
            properties12.driverInfo, properties.driverVersion,
            VK_VERSION_MAJOR(properties.apiVersion),
            VK_VERSION_MINOR(properties.apiVersion),
            VK_VERSION_PATCH(properties.apiVersion));
        return true;
    }

    bool create_logical_device(std::string& error) {
        std::vector<const char*> extensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
            VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
        };
        VkPhysicalDeviceVulkan12Features features12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        uint32_t graphics_queue_count = 1;
        uint32_t compute_queue_count = 0;
        std::vector<const char*> streamline_instance_extensions;
        streamline.append_requirements(
            streamline_instance_extensions, extensions, features12, features13,
            graphics_queue_count, compute_queue_count);
        const auto available_extensions = device_extensions(physical_device);
        VulkanRayTracingCapabilities rt_capabilities{};
        rt_capabilities.acceleration_structure_extension =
            available_extensions.count(
                VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) != 0;
        rt_capabilities.ray_tracing_pipeline_extension =
            available_extensions.count(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) != 0;
        rt_capabilities.deferred_host_operations_extension =
            available_extensions.count(
                VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) != 0;
        rt_capabilities.spirv_1_4_extension =
            available_extensions.count(VK_KHR_SPIRV_1_4_EXTENSION_NAME) != 0;
        rt_capabilities.shader_float_controls_extension =
            available_extensions.count(
                VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME) != 0;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR rt_as_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt_pipeline_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceVulkan12Features rt_features12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 rt_features2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        rt_features2.pNext = &rt_features12;
        rt_features12.pNext = &rt_as_features;
        rt_as_features.pNext = &rt_pipeline_features;
        vkGetPhysicalDeviceFeatures2(physical_device, &rt_features2);
        rt_capabilities.buffer_device_address = rt_features12.bufferDeviceAddress;
        rt_capabilities.acceleration_structure = rt_as_features.accelerationStructure;
        rt_capabilities.ray_tracing_pipeline = rt_pipeline_features.rayTracingPipeline;
        rt_capabilities.shader_storage_image_extended_formats =
            rt_features2.features.shaderStorageImageExtendedFormats;
        VkFormatProperties2 r8_properties{
            VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        vkGetPhysicalDeviceFormatProperties2(physical_device,
                                             VK_FORMAT_R8_UNORM,
                                             &r8_properties);
        rt_capabilities.storage_image_r8 =
            (r8_properties.formatProperties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        ray_tracing_enabled =
            supports_native_ray_tracing(rt_capabilities, ray_tracing_reason);
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        const char* force_rt_unavailable =
            std::getenv("MATTER_VK_TEST_FORCE_RT_UNAVAILABLE");
        if (force_rt_unavailable && std::string(force_rt_unavailable) == "1") {
            ray_tracing_enabled = false;
            ray_tracing_reason =
                "native ray tracing forced unavailable by test fixture";
        }
#endif
        if (ray_tracing_enabled) {
            extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
            extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            extensions.push_back(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
            extensions.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
        }
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                                 &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                                 &queue_family_count,
                                                 queue_families.data());
        const uint32_t total_queue_count =
            graphics_queue_count + compute_queue_count;
        if (graphics_queue_family >= queue_families.size() ||
            total_queue_count >
                queue_families[graphics_queue_family].queueCount) {
            error = "selected Vulkan queue family cannot satisfy Streamline queue requirements";
            return false;
        }
        std::vector<float> priorities(total_queue_count, 1.0f);
        VkDeviceQueueCreateInfo queue_create{
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_create.queueFamilyIndex = graphics_queue_family;
        queue_create.queueCount = total_queue_count;
        queue_create.pQueuePriorities = priorities.data();

        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance1{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        maintenance1.swapchainMaintenance1 = VK_TRUE;
        features12.pNext = &features13;
        features13.pNext = &maintenance1;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR enabled_as{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR enabled_pipeline{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        if (ray_tracing_enabled) {
            maintenance1.pNext = &enabled_as;
            enabled_as.pNext = &enabled_pipeline;
            enabled_as.accelerationStructure = VK_TRUE;
            enabled_pipeline.rayTracingPipeline = VK_TRUE;
        }
        features12.timelineSemaphore = VK_TRUE;
        features12.descriptorIndexing = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        features12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.bufferDeviceAddress = VK_TRUE;

        VkPhysicalDeviceFeatures2 features2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.features.drawIndirectFirstInstance = VK_TRUE;
        features2.features.multiDrawIndirect = VK_TRUE;
        features2.features.shaderStorageImageExtendedFormats =
            ray_tracing_enabled ? VK_TRUE : VK_FALSE;
        features2.pNext = &features12;

        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.pNext = &features2;
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos = &queue_create;
        create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        if (!vk_ok(streamline.create_device(physical_device, &create, nullptr, &device),
                   "vkCreateDevice", error)) {
            return false;
        }
        draw_indirect_first_instance_enabled = true;
        multi_draw_indirect_enabled = true;
        if (ray_tracing_enabled) {
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR pipeline_properties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
            VkPhysicalDeviceAccelerationStructurePropertiesKHR as_properties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
            VkPhysicalDeviceProperties2 properties2{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties2.pNext = &pipeline_properties;
            pipeline_properties.pNext = &as_properties;
            vkGetPhysicalDeviceProperties2(physical_device, &properties2);
            ray_tracing_properties.shader_group_handle_size =
                pipeline_properties.shaderGroupHandleSize;
            ray_tracing_properties.shader_group_handle_alignment =
                pipeline_properties.shaderGroupHandleAlignment;
            ray_tracing_properties.shader_group_base_alignment =
                pipeline_properties.shaderGroupBaseAlignment;
            ray_tracing_properties.max_ray_recursion_depth =
                pipeline_properties.maxRayRecursionDepth;
            ray_tracing_properties.max_shader_group_stride =
                pipeline_properties.maxShaderGroupStride;
            ray_tracing_properties.max_ray_dispatch_invocation_count =
                pipeline_properties.maxRayDispatchInvocationCount;
            ray_tracing_properties
                .min_acceleration_structure_scratch_offset_alignment =
                as_properties.minAccelerationStructureScratchOffsetAlignment;
        }
        device_lifetime =
            std::make_shared<detail::DeviceAccessToken>(device);
        vkGetDeviceQueue(device, graphics_queue_family, 0, &graphics_queue);
        const uint32_t streamline_graphics_begin =
            graphics_queue_count > 1 ? 1 : 0;
        const uint32_t streamline_compute_begin =
            compute_queue_count != 0 ? graphics_queue_count : 0;
        streamline_graphics_queues.resize(graphics_queue_count - 1);
        for (uint32_t i = 0; i < streamline_graphics_queues.size(); ++i) {
            vkGetDeviceQueue(device, graphics_queue_family, i + 1,
                             &streamline_graphics_queues[i]);
        }
        streamline_compute_queues.resize(compute_queue_count);
        for (uint32_t i = 0; i < streamline_compute_queues.size(); ++i) {
            vkGetDeviceQueue(device, graphics_queue_family,
                             streamline_compute_begin + i,
                             &streamline_compute_queues[i]);
        }
        if (!streamline.set_vulkan_info(
                instance, physical_device, device, graphics_queue_family,
                streamline_graphics_begin, graphics_queue_family,
                streamline_compute_begin))
            return false;
        release_swapchain_images =
            reinterpret_cast<PFN_vkReleaseSwapchainImagesEXT>(
                vkGetDeviceProcAddr(device, "vkReleaseSwapchainImagesEXT"));
        if (!release_swapchain_images) {
            error = "Missing Vulkan function vkReleaseSwapchainImagesEXT";
            return false;
        }
        return true;
    }

    bool framebuffer_extent(const VkSurfaceCapabilitiesKHR& capabilities,
                            VkExtent2D& extent) {
        if (capabilities.currentExtent.width !=
            std::numeric_limits<uint32_t>::max()) {
            extent = capabilities.currentExtent;
            return extent.width != 0 && extent.height != 0;
        }
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width <= 0 || height <= 0) return false;
        extent.width = std::clamp(static_cast<uint32_t>(width),
                                  capabilities.minImageExtent.width,
                                  capabilities.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(height),
                                   capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);
        return true;
    }

    bool desired_framebuffer_extent(VkExtent2D& extent, std::string& error) {
        VkSurfaceCapabilitiesKHR capabilities{};
        if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                       physical_device, surface, &capabilities),
                   "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", error)) {
            return false;
        }
        if (!framebuffer_extent(capabilities, extent)) {
            error = "Framebuffer is zero-sized; frame skipped";
            return false;
        }
        return true;
    }

    bool create_swapchain(VkSwapchainKHR old_swapchain, std::string& error) {
        SwapchainSupport support;
        if (!query_swapchain_support(physical_device, surface, support, error)) {
            return false;
        }
        if (support.formats.empty() || support.present_modes.empty()) {
            error = "Swapchain support disappeared during creation";
            return false;
        }
        VkExtent2D extent{};
        if (!framebuffer_extent(support.capabilities, extent)) {
            error = "Cannot create swapchain for a zero-sized framebuffer";
            return false;
        }
        const VkSurfaceFormatKHR format =
            choose_surface_format(support.formats);
        const auto format_missing =
            missing_presentation_blit_features(physical_device, format.format);
        if (!format_missing.empty()) {
            error = "presentation blit format capabilities missing: " +
                    join(format_missing, ", ");
            return false;
        }
        uint32_t image_count = support.capabilities.minImageCount + 1;
        if (support.capabilities.maxImageCount != 0) {
            image_count =
                std::min(image_count, support.capabilities.maxImageCount);
        }

        VkSwapchainCreateInfoKHR create{
            VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = surface;
        create.minImageCount = image_count;
        create.imageFormat = format.format;
        create.imageColorSpace = format.colorSpace;
        create.imageExtent = extent;
        create.imageArrayLayers = 1;
        const VkImageUsageFlags required_usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if ((support.capabilities.supportedUsageFlags & required_usage) !=
            required_usage) {
            error = "swapchain lacks color/transfer-src/transfer-dst image usage";
            return false;
        }
        create.imageUsage = required_usage;
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.preTransform = support.capabilities.currentTransform;
        constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> composite_modes = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (VkCompositeAlphaFlagBitsKHR mode : composite_modes) {
            if ((support.capabilities.supportedCompositeAlpha & mode) != 0) {
                create.compositeAlpha = mode;
                break;
            }
        }
        create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        create.clipped = VK_TRUE;
        create.oldSwapchain = old_swapchain;

        VkSwapchainKHR replacement = VK_NULL_HANDLE;
        if (!vk_ok(streamline.create_swapchain(device, &create, nullptr,
                                               &replacement),
                   "vkCreateSwapchainKHR", error)) {
            return false;
        }
        uint32_t actual_count = 0;
        VkResult result = streamline.get_swapchain_images(
            device, replacement, &actual_count, nullptr);
        if (result != VK_SUCCESS || actual_count == 0) {
            streamline.destroy_swapchain(device, replacement, nullptr);
            if (result == VK_SUCCESS) {
                error = "vkGetSwapchainImagesKHR returned zero images";
                return false;
            }
            return vk_ok(result, "vkGetSwapchainImagesKHR", error);
        }
        std::vector<VkImage> images(actual_count);
        result = streamline.get_swapchain_images(device, replacement,
                                                  &actual_count, images.data());
        if (result != VK_SUCCESS) {
            streamline.destroy_swapchain(device, replacement, nullptr);
            return vk_ok(result, "vkGetSwapchainImagesKHR", error);
        }
        std::vector<VkImageView> views(actual_count, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < actual_count; ++i) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = images[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format.format;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            if (!vk_ok(vkCreateImageView(device, &view, nullptr, &views[i]),
                       "vkCreateImageView(swapchain)", error)) {
                for (VkImageView created : views)
                    if (created != VK_NULL_HANDLE)
                        vkDestroyImageView(device, created, nullptr);
                streamline.destroy_swapchain(device, replacement, nullptr);
                return false;
            }
        }
        for (VkImageView old_view : swapchain_image_views)
            if (old_view != VK_NULL_HANDLE)
                vkDestroyImageView(device, old_view, nullptr);
        if (old_swapchain != VK_NULL_HANDLE)
            streamline.destroy_swapchain(device, old_swapchain, nullptr);
        swapchain = replacement;
        swapchain_format = format.format;
        swapchain_extent = extent;
        swapchain_images = std::move(images);
        swapchain_image_views = std::move(views);
        swapchain_image_initialized.assign(swapchain_images.size(), false);
        return true;
    }

    bool create_swapchain_sync(std::string& error) {
        std::vector<VkSemaphore> replacement_semaphores(
            swapchain_images.size(), VK_NULL_HANDLE);
        std::vector<VkFence> replacement_fences(swapchain_images.size(),
                                                VK_NULL_HANDLE);
        VkSemaphoreCreateInfo create{
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fence_create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        bool allocated = true;
        for (size_t i = 0; i < replacement_semaphores.size(); ++i) {
            if (!vk_ok(vkCreateSemaphore(device, &create, nullptr,
                                         &replacement_semaphores[i]),
                        "vkCreateSemaphore(render finished)", error)) {
                allocated = false;
                break;
            }
            if (!vk_ok(vkCreateFence(device, &fence_create, nullptr,
                                     &replacement_fences[i]),
                       "vkCreateFence(present completion)", error)) {
                allocated = false;
                break;
            }
        }
        if (!allocated) {
            for (VkSemaphore semaphore : replacement_semaphores) {
                if (semaphore != VK_NULL_HANDLE)
                    vkDestroySemaphore(device, semaphore, nullptr);
            }
            for (VkFence fence : replacement_fences) {
                if (fence != VK_NULL_HANDLE)
                    vkDestroyFence(device, fence, nullptr);
            }
            return false;
        }
        for (VkSemaphore semaphore : render_finished) {
            if (semaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(device, semaphore, nullptr);
        }
        for (VkFence fence : present_fences) {
            if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
        }
        render_finished = std::move(replacement_semaphores);
        present_fences = std::move(replacement_fences);
        present_fence_pending.assign(swapchain_images.size(), false);
        return true;
    }

    VkResult wait_for_present_completion(std::string& error) {
        if (present_fences.size() != present_fence_pending.size()) {
            error = "present completion tracking vectors are inconsistent";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        std::vector<VkFence> pending;
        for (size_t i = 0; i < present_fences.size(); ++i) {
            if (!present_fence_pending[i]) continue;
            if (present_fences[i] == VK_NULL_HANDLE) {
                error = "pending present completion fence is null";
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            pending.push_back(present_fences[i]);
        }
        if (pending.empty()) return VK_SUCCESS;
        const VkResult result = vkWaitForFences(
            device, static_cast<uint32_t>(pending.size()), pending.data(), VK_TRUE,
            std::numeric_limits<uint64_t>::max());
        vk_ok(result, "vkWaitForFences(present completion)", error);
        return result;
    }

    bool recreate_swapchain(std::string& error) {
        swapchain_recreate_required = true;
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width <= 0 || height <= 0) {
            error = "Framebuffer is zero-sized; frame skipped";
            swapchain_recreate_required = true;
            return false;
        }
        if (!vk_ok(streamline.device_wait_idle(device), "vkDeviceWaitIdle", error)) {
            return poison_device(error,
                                 "swapchain recreation could not establish idle");
        }
        if (wait_for_present_completion(error) != VK_SUCCESS)
            return poison_device(
                error, "swapchain recreation could not establish present completion");
        for (FrameSlot& frame : frames) {
            if (!settle_acquire_fence(frame, error))
                return poison_device(error,
                                     "swapchain recreation could not establish "
                                     "acquisition completion");
        }
        if (!create_swapchain(swapchain, error)) return false;
        if (!create_swapchain_sync(error))
            return poison_device(
                error, "replacement swapchain synchronization allocation failed");
        report_recreated = true;
        swapchain_recreate_required = false;
        acquired_suboptimal = false;
        return true;
    }

    bool create_frame_slots(std::string& error) {
        for (FrameSlot& frame : frames) {
            VkCommandPoolCreateInfo pool{
                VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = graphics_queue_family;
            if (!vk_ok(vkCreateCommandPool(device, &pool, nullptr,
                                           &frame.command_pool),
                       "vkCreateCommandPool", error)) {
                return false;
            }
            VkCommandBufferAllocateInfo allocate{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = frame.command_pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            if (!vk_ok(vkAllocateCommandBuffers(device, &allocate,
                                                &frame.command_buffer),
                       "vkAllocateCommandBuffers", error)) {
                return false;
            }
            VkSemaphoreCreateInfo semaphore{
                VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (!vk_ok(vkCreateSemaphore(device, &semaphore, nullptr,
                                          &frame.image_available),
                        "vkCreateSemaphore(image available)", error)) {
                return false;
            }
            VkFenceCreateInfo acquire_fence{
                VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (!vk_ok(vkCreateFence(device, &acquire_fence, nullptr,
                                     &frame.acquire_fence),
                       "vkCreateFence(acquire completion)", error)) {
                return false;
            }
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (!vk_ok(vkCreateFence(device, &fence, nullptr, &frame.fence),
                       "vkCreateFence", error)) {
                return false;
            }
        }
        return create_swapchain_sync(error);
    }

    bool begin_frame(VulkanFrame& output, std::string& error) {
        error.clear();
        output = {};
        if (!ensure_healthy(error)) return false;
        if (frame_active) {
            error = "begin_frame called while a frame is already active";
            return false;
        }
        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
        if (framebuffer_width <= 0 || framebuffer_height <= 0) {
            error = "Framebuffer is zero-sized; frame skipped";
            swapchain_recreate_required = true;
            return false;
        }

        VkExtent2D desired_extent{};
        if (!desired_framebuffer_extent(desired_extent, error)) return false;
        if (swapchain_recreate_required ||
            desired_extent.width != swapchain_extent.width ||
            desired_extent.height != swapchain_extent.height) {
            if (!recreate_swapchain(error)) return false;
        }
        if (!ensure_frame_resources(error)) return false;

        FrameSlot& slot = frames[frame_slot];
        if (!vk_ok(vkWaitForFences(device, 1, &slot.fence, VK_TRUE,
                                   std::numeric_limits<uint64_t>::max()),
                    "vkWaitForFences", error)) {
            return poison_device(error, "frame fence wait failed");
        }
        slot.retained.clear();

        uint32_t image_index = 0;
        if (!settle_acquire_fence(slot, error)) {
            return poison_device(
                error, "acquisition completion could not be established");
        }
        if (!vk_ok(vkResetFences(device, 1, &slot.acquire_fence),
                   "vkResetFences(acquire completion)", error)) {
            return false;
        }
        VkResult acquire = streamline.acquire_next_image(
            device, swapchain, std::numeric_limits<uint64_t>::max(),
            slot.image_available, slot.acquire_fence, &image_index);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!recreate_swapchain(error)) return false;
            if (!vk_ok(vkResetFences(device, 1, &slot.acquire_fence),
                       "vkResetFences(acquire completion)", error)) {
                return false;
            }
            acquire = streamline.acquire_next_image(
                device, swapchain, std::numeric_limits<uint64_t>::max(),
                slot.image_available, slot.acquire_fence, &image_index);
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            return vk_ok(acquire, "vkAcquireNextImageKHR", error);
        }
        // Acquisition completion is proven lazily via settle_acquire_fence
        // (slot reuse, recovery, swapchain retirement) and teardown instead of
        // stalling here for up to a vsync in FIFO; GPU-side ordering is
        // covered by the submit's image_available semaphore wait.
        slot.acquire_fence_pending = true;
        acquired_suboptimal = acquire == VK_SUBOPTIMAL_KHR;

        if (present_fence_pending[image_index]) {
            if (!vk_ok(vkWaitForFences(
                           device, 1, &present_fences[image_index], VK_TRUE,
                           std::numeric_limits<uint64_t>::max()),
                        "vkWaitForFences(previous present)", error)) {
                recover_unsubmitted_acquire(slot, image_index, error);
                return poison_device(
                    error, "previous present completion could not be established");
            }
            present_fence_pending[image_index] = false;
        }

        if (!vk_ok(vkResetCommandPool(device, slot.command_pool, 0),
                    "vkResetCommandPool", error)) {
            recover_unsubmitted_acquire(slot, image_index, error);
            return false;
        }
        VkCommandBufferBeginInfo begin{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!vk_ok(vkBeginCommandBuffer(slot.command_buffer, &begin),
                    "vkBeginCommandBuffer", error)) {
            recover_unsubmitted_acquire(slot, image_index, error);
            return false;
        }

        VkImageMemoryBarrier2 to_color{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_color.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_color.srcAccessMask = VK_ACCESS_2_NONE;
        to_color.dstStageMask =
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_color.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_color.oldLayout = swapchain_image_initialized[image_index]
                                 ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                 : VK_IMAGE_LAYOUT_UNDEFINED;
        to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_color.image = swapchain_images[image_index];
        to_color.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_color.subresourceRange.baseMipLevel = 0;
        to_color.subresourceRange.levelCount = 1;
        to_color.subresourceRange.baseArrayLayer = 0;
        to_color.subresourceRange.layerCount = 1;
        VkDependencyInfo to_color_dependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        to_color_dependency.imageMemoryBarrierCount = 1;
        to_color_dependency.pImageMemoryBarriers = &to_color;
        vkCmdPipelineBarrier2(slot.command_buffer, &to_color_dependency);

        output.command_buffer = slot.command_buffer;
        output.swapchain_image = swapchain_images[image_index];
        output.swapchain_image_view = swapchain_image_views[image_index];
        output.swapchain_format = swapchain_format;
        output.image_index = image_index;
        output.image_count = static_cast<uint32_t>(swapchain_images.size());
        output.frame_slot = frame_slot;
        output.frame_slot_count = static_cast<uint32_t>(frames.size());
        output.extent = swapchain_extent;
        output.serial = next_serial++;
        output.swapchain_recreated = report_recreated;
        report_recreated = false;
        active_frame = output;
        frame_active = true;
        return true;
    }

    bool retain_for_frame(const VulkanFrame& input,
                          std::vector<std::shared_ptr<void>> resources,
                          std::string& error) {
        error.clear();
        if (!ensure_healthy(error)) return false;
        const uint32_t slot_count = static_cast<uint32_t>(frames.size());
        if (input.frame_slot_count != slot_count ||
            input.frame_slot >= slot_count) {
            error = "retain_for_frame received an out-of-range frame slot";
            return false;
        }
        if (!frame_active || input.serial != active_frame.serial ||
            input.command_buffer != active_frame.command_buffer ||
            input.frame_slot != active_frame.frame_slot) {
            error = "retain_for_frame requires the active VulkanFrame";
            return false;
        }
        FrameSlot& slot = frames[frame_slot];
        for (std::shared_ptr<void>& resource : resources) {
            if (resource) slot.retained.push_back(std::move(resource));
        }
        return true;
    }

    bool queue_readback(const VulkanFrame& input, std::vector<uint8_t>& rgba,
                        std::string& error) {
        if (!frame_active || input.serial != active_frame.serial ||
            input.image_index != active_frame.image_index) {
            error = "swapchain readback requires the active VulkanFrame";
            return false;
        }
        if (readback_output) {
            error = "only one swapchain readback may be queued per frame";
            return false;
        }
        if (swapchain_format != VK_FORMAT_B8G8R8A8_UNORM &&
            swapchain_format != VK_FORMAT_B8G8R8A8_SRGB &&
            swapchain_format != VK_FORMAT_R8G8B8A8_UNORM &&
            swapchain_format != VK_FORMAT_R8G8B8A8_SRGB) {
            error = "unsupported swapchain readback format";
            return false;
        }
        clear_readback();
        readback_size = static_cast<VkDeviceSize>(swapchain_extent.width) *
                        swapchain_extent.height * 4u;
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = readback_size;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!vk_ok(vkCreateBuffer(device, &buffer, nullptr, &readback_buffer),
                   "vkCreateBuffer(swapchain readback)", error)) return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, readback_buffer, &requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
        uint32_t memory_type = UINT32_MAX;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) == 0) continue;
            const auto flags = properties.memoryTypes[i].propertyFlags;
            if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                memory_type = i;
                break;
            }
        }
        if (memory_type == UINT32_MAX) {
            error = "no host-coherent memory for swapchain readback";
            clear_readback();
            return false;
        }
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type;
        if (!vk_ok(vkAllocateMemory(device, &allocate, nullptr,
                                    &readback_memory),
                   "vkAllocateMemory(swapchain readback)", error) ||
            !vk_ok(vkBindBufferMemory(device, readback_buffer,
                                      readback_memory, 0),
                   "vkBindBufferMemory(swapchain readback)", error)) {
            clear_readback();
            return false;
        }
        VkImageMemoryBarrier2 to_transfer{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_transfer.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_transfer.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = swapchain_images[input.image_index];
        to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_transfer.subresourceRange.levelCount = 1;
        to_transfer.subresourceRange.layerCount = 1;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &to_transfer;
        vkCmdPipelineBarrier2(input.command_buffer, &dependency);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {swapchain_extent.width, swapchain_extent.height, 1};
        vkCmdCopyImageToBuffer(input.command_buffer,
                               swapchain_images[input.image_index],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_buffer, 1, &copy);
        std::swap(to_transfer.srcStageMask, to_transfer.dstStageMask);
        std::swap(to_transfer.srcAccessMask, to_transfer.dstAccessMask);
        std::swap(to_transfer.oldLayout, to_transfer.newLayout);
        vkCmdPipelineBarrier2(input.command_buffer, &dependency);
        readback_output = &rgba;
        readback_format = swapchain_format;
        return true;
    }

    void abandon_active_frame() {
        frame_active = false;
        acquired_suboptimal = false;
        swapchain_recreate_required = true;
    }

    void append_recovery_error(std::string& error,
                               const std::string& recovery_error) {
        if (!recovery_error.empty()) error += "; additionally, " + recovery_error;
    }

    // Blocks only until the presentation engine has released the image; on
    // every call site the acquisition has long since completed (the frame was
    // submitted, recovered, or the device was idled), so this returns
    // immediately in practice.
    bool settle_acquire_fence(FrameSlot& slot, std::string& error) {
        if (!slot.acquire_fence_pending) return true;
        if (!vk_ok(vkWaitForFences(device, 1, &slot.acquire_fence, VK_TRUE,
                                   std::numeric_limits<uint64_t>::max()),
                   "vkWaitForFences(acquire completion)", error)) {
            return false;
        }
        slot.acquire_fence_pending = false;
        return true;
    }

    bool release_acquired_image(uint32_t image_index, std::string& error) {
        VkReleaseSwapchainImagesInfoEXT release{
            VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT};
        release.swapchain = swapchain;
        release.imageIndexCount = 1;
        release.pImageIndices = &image_index;
        return vk_ok(release_swapchain_images(device, &release),
                     "vkReleaseSwapchainImagesEXT", error);
    }

    bool recover_unsubmitted_acquire(FrameSlot& slot, uint32_t image_index,
                                     std::string& error) {
        std::string recovery_error;
        if (!settle_acquire_fence(slot, recovery_error)) {
            append_recovery_error(error, recovery_error);
            swapchain_recreate_required = true;
            return poison_device(
                error, "acquisition completion could not be established");
        }
        bool recovered = true;
        recovery_error.clear();
        if (!release_acquired_image(image_index, recovery_error)) {
            append_recovery_error(error, recovery_error);
            recovered = false;
        }
        if (slot.image_available != VK_NULL_HANDLE)
            vkDestroySemaphore(device, slot.image_available, nullptr);
        slot.image_available = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        recovery_error.clear();
        if (!vk_ok(vkCreateSemaphore(device, &create, nullptr,
                                     &slot.image_available),
                    "vkCreateSemaphore(image available recovery)",
                    recovery_error)) {
            append_recovery_error(error, recovery_error);
            recovered = false;
        }
        swapchain_recreate_required = true;
        if (!recovered)
            return poison_device(
                error, "unsubmitted acquisition recovery failed");
        return true;
    }

    bool recover_submitted_without_present(FrameSlot& slot,
                                           uint32_t image_index,
                                           std::string& error) {
        std::string recovery_error;
        if (!vk_ok(vkWaitForFences(device, 1, &slot.fence, VK_TRUE,
                                   std::numeric_limits<uint64_t>::max()),
                    "vkWaitForFences(abandoned submission)", recovery_error)) {
            append_recovery_error(error, recovery_error);
            swapchain_recreate_required = true;
            return poison_device(
                error, "abandoned submission completion could not be established");
        }
        recovery_error.clear();
        if (!settle_acquire_fence(slot, recovery_error)) {
            append_recovery_error(error, recovery_error);
            swapchain_recreate_required = true;
            return poison_device(
                error, "acquisition completion could not be established");
        }
        recovery_error.clear();
        if (!release_acquired_image(image_index, recovery_error)) {
            append_recovery_error(error, recovery_error);
            swapchain_recreate_required = true;
            return poison_device(error,
                                 "submitted image release recovery failed");
        }
        swapchain_recreate_required = true;
        return true;
    }

    bool restore_signaled_frame_fence(FrameSlot& slot, std::string& error) {
        vkDestroyFence(device, slot.fence, nullptr);
        slot.fence = VK_NULL_HANDLE;
        VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        create.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        std::string fence_error;
        if (!vk_ok(vkCreateFence(device, &create, nullptr, &slot.fence),
                   "vkCreateFence(frame recovery)", fence_error)) {
            append_recovery_error(error, fence_error);
            return poison_device(error, "frame fence restoration failed");
        }
        return true;
    }

    bool end_frame(const VulkanFrame& input, bool& presented,
                   std::string& error) {
        error.clear();
        presented = false;
        if (!ensure_healthy(error)) return false;
        if (!frame_active) {
            error = "end_frame called without an active frame";
            return false;
        }
        if (input.serial != active_frame.serial ||
            input.image_index != active_frame.image_index ||
            input.command_buffer != active_frame.command_buffer) {
            error = "end_frame received a frame other than the active frame";
            return false;
        }
        if (!ensure_frame_resources(error)) return false;
        FrameSlot& slot = frames[frame_slot];
        VkImageMemoryBarrier2 to_present{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_present.srcStageMask =
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_present.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_present.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_present.dstAccessMask = VK_ACCESS_2_NONE;
        to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_present.image = swapchain_images[input.image_index];
        to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_present.subresourceRange.baseMipLevel = 0;
        to_present.subresourceRange.levelCount = 1;
        to_present.subresourceRange.baseArrayLayer = 0;
        to_present.subresourceRange.layerCount = 1;
        VkDependencyInfo to_present_dependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        to_present_dependency.imageMemoryBarrierCount = 1;
        to_present_dependency.pImageMemoryBarriers = &to_present;
        vkCmdPipelineBarrier2(slot.command_buffer, &to_present_dependency);
        VkResult end_command_buffer = vkEndCommandBuffer(slot.command_buffer);
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        const char* end_frame_fault = std::getenv("MATTER_VK_TEST_END_FRAME_FAULT");
        if (end_command_buffer == VK_SUCCESS && end_frame_fault &&
            std::strcmp(end_frame_fault, "record") == 0) {
            end_command_buffer = VK_ERROR_INITIALIZATION_FAILED;
        }
#endif
        if (!vk_ok(end_command_buffer,
                    "vkEndCommandBuffer", error)) {
            recover_unsubmitted_acquire(slot, input.image_index, error);
            abandon_active_frame();
            return false;
        }

        VkSemaphoreSubmitInfo wait{
            VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = slot.image_available;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo command{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        command.commandBuffer = slot.command_buffer;
        VkSemaphoreSubmitInfo signal{
            VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = render_finished[input.image_index];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        if (!vk_ok(vkResetFences(device, 1, &slot.fence), "vkResetFences(frame)",
                   error)) {
            recover_unsubmitted_acquire(slot, input.image_index, error);
            abandon_active_frame();
            return false;
        }
        VkResult submit_result = VK_SUCCESS;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        if (end_frame_fault && std::strcmp(end_frame_fault, "submit") == 0) {
            submit_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        } else
#endif
        {
            submit_result = vkQueueSubmit2(graphics_queue, 1, &submit, slot.fence);
        }
        if (!vk_ok(submit_result,
                    "vkQueueSubmit2", error)) {
            if (!restore_signaled_frame_fence(slot, error)) {
                recover_unsubmitted_acquire(slot, input.image_index, error);
                abandon_active_frame();
                return false;
            }
            recover_unsubmitted_acquire(slot, input.image_index, error);
            abandon_active_frame();
            return false;
        }

        if (readback_output) {
            if (!vk_ok(vkWaitForFences(device, 1, &slot.fence, VK_TRUE,
                                       std::numeric_limits<uint64_t>::max()),
                       "vkWaitForFences(swapchain readback)", error)) {
                clear_readback();
                abandon_active_frame();
                return false;
            }
            void* mapped = nullptr;
            if (!vk_ok(vkMapMemory(device, readback_memory, 0, readback_size, 0,
                                   &mapped),
                       "vkMapMemory(swapchain readback)", error)) {
                clear_readback();
                abandon_active_frame();
                return false;
            }
            readback_output->assign(static_cast<uint8_t*>(mapped),
                                    static_cast<uint8_t*>(mapped) + readback_size);
            vkUnmapMemory(device, readback_memory);
            if (readback_format == VK_FORMAT_B8G8R8A8_UNORM ||
                readback_format == VK_FORMAT_B8G8R8A8_SRGB) {
                for (size_t i = 0; i < readback_output->size(); i += 4)
                    std::swap((*readback_output)[i], (*readback_output)[i + 2]);
            }
            clear_readback();
        }

        if (!vk_ok(vkResetFences(device, 1,
                                 &present_fences[input.image_index]),
                   "vkResetFences(present completion)", error)) {
            recover_submitted_without_present(slot, input.image_index, error);
            abandon_active_frame();
            return false;
        }
        VkSwapchainPresentFenceInfoEXT present_fence_info{
            VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
        present_fence_info.swapchainCount = 1;
        present_fence_info.pFences = &present_fences[input.image_index];
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.pNext = &present_fence_info;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished[input.image_index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &input.image_index;
        if (!streamline.present_common(input.serial)) {
            error = "Streamline common present was already handed off for this frame";
            recover_submitted_without_present(slot, input.image_index, error);
            abandon_active_frame();
            return false;
        }
        const VkResult present_result =
            streamline.queue_present(graphics_queue, &present);
        presented = present_result_was_presented(present_result);
        const PresentResultState present_state =
            present_result_state(present_result);
        if (present_state == PresentResultState::completed_or_trackable) {
            present_fence_pending[input.image_index] = true;
        }
        swapchain_image_initialized[input.image_index] = true;
        frame_active = false;
        frame_slot = (frame_slot + 1) % kFramesInFlight;

        if (present_state == PresentResultState::ambiguous) {
            vk_ok(present_result, "vkQueuePresentKHR", error);
            wsi_completion_ambiguous = true;
            return poison_device(
                error, "presentation result left resource ownership ambiguous");
        }
        if (present_state == PresentResultState::unchanged) {
            acquired_suboptimal = false;
            vk_ok(present_result, "vkQueuePresentKHR", error);
            recover_submitted_without_present(slot, input.image_index, error);
            return false;
        }
        if (should_recreate_swapchain_after_present(present_result,
                                                    acquired_suboptimal)) {
            acquired_suboptimal = false;
            return recreate_swapchain(error);
        }
        acquired_suboptimal = false;
        return vk_ok(present_result, "vkQueuePresentKHR", error);
    }

    void cleanup() {
        if (preserve_external_work) {
            std::fprintf(stderr,
                         "Vulkan cleanup intentionally preserving the logical "
                         "device and children because external work completion "
                         "is unproven\n");
            if (device_lifetime) device_lifetime->invalidate();
            return;
        }
        if (device != VK_NULL_HANDLE) {
            VkResult idle = VK_SUCCESS;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
            const char* force_unproven =
                std::getenv("MATTER_VK_TEST_FORCE_CLEANUP_UNPROVEN");
            if (force_unproven && std::strcmp(force_unproven, "1") == 0) {
                idle = VK_TIMEOUT;
            } else
#endif
            {
                idle = streamline.device_wait_idle(device);
            }
            bool device_lost = idle == VK_ERROR_DEVICE_LOST;
            if (!destruction_safe_after_wait(idle)) {
                std::fprintf(stderr,
                             "Vulkan cleanup intentionally preserving the "
                             "logical device, WSI resources, and their parents "
                             "after %s; completion is unproven\n",
                             result_name(idle));
                if (device_lifetime) device_lifetime->invalidate();
                return;
            }
            if (wsi_completion_ambiguous && !device_lost) {
                std::fprintf(
                    stderr,
                    "Vulkan cleanup intentionally preserving the logical "
                    "device, ambiguous presentation resources, and their "
                    "parents; vkQueuePresentKHR completion is unproven\n");
                if (device_lifetime) device_lifetime->invalidate();
                return;
            }
            for (FrameSlot& frame : frames) {
                if (device_lost) break;
                if (frame.acquire_fence_pending) {
                    const VkResult waited = vkWaitForFences(
                        device, 1, &frame.acquire_fence, VK_TRUE,
                        std::numeric_limits<uint64_t>::max());
                    if (waited == VK_ERROR_DEVICE_LOST) {
                        device_lost = true;
                        break;
                    }
                    if (waited != VK_SUCCESS) {
                        std::fprintf(stderr,
                                     "Vulkan cleanup intentionally preserving "
                                     "the logical device, WSI resources, and "
                                     "their parents after %s while waiting for "
                                     "acquisition; completion is unproven\n",
                                     result_name(waited));
                        if (device_lifetime) device_lifetime->invalidate();
                        return;
                    }
                    frame.acquire_fence_pending = false;
                }
            }
            if (!device_lost) {
                std::string wait_error;
                const VkResult waited = wait_for_present_completion(wait_error);
                if (waited == VK_ERROR_DEVICE_LOST) {
                    device_lost = true;
                } else if (waited != VK_SUCCESS) {
                    std::fprintf(
                        stderr,
                        "Vulkan cleanup intentionally preserving the logical "
                        "device, present fences/semaphores, swapchain, and their "
                        "parents after present completion failed: %s\n",
                        wait_error.empty() ? result_name(waited)
                                           : wait_error.c_str());
                    if (device_lifetime) device_lifetime->invalidate();
                    return;
                }
            }
            if (device_lost) {
                std::fprintf(stderr,
                             "Vulkan device was lost; destroying device objects "
                             "without further completion waits\n");
            }
        }
        while (retained_resources) {
            detail::DeviceRetainedResource* resource = retained_resources;
            retained_resources = resource->next;
            resource->next = nullptr;
            delete resource;
        }
        if (device_lifetime)
            device_lifetime->destroy_registered_resources();
        for (FrameSlot& frame : frames) {
            if (frame.fence != VK_NULL_HANDLE)
                vkDestroyFence(device, frame.fence, nullptr);
            if (frame.acquire_fence != VK_NULL_HANDLE)
                vkDestroyFence(device, frame.acquire_fence, nullptr);
            if (frame.image_available != VK_NULL_HANDLE)
                vkDestroySemaphore(device, frame.image_available, nullptr);
            if (frame.command_pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(device, frame.command_pool, nullptr);
        }
        for (VkSemaphore semaphore : render_finished) {
            if (semaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(device, semaphore, nullptr);
        }
        for (VkFence fence : present_fences) {
            if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
        }
        clear_readback();
        for (VkImageView view : swapchain_image_views) {
            if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
        }
        if (swapchain != VK_NULL_HANDLE)
            streamline.destroy_swapchain(device, swapchain, nullptr);
        if (surface != VK_NULL_HANDLE) {
            streamline.destroy_surface(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        // In the native-device manual-hooking path Streamline owns Vulkan
        // children which slShutdown must release while device and instance are
        // still valid. All intercepted WSI objects have been destroyed above.
        streamline.shutdown();
        if (device != VK_NULL_HANDLE) {
            if (device_lifetime) device_lifetime->invalidate();
            vkDestroyDevice(device, nullptr);
        }
        if (debug_messenger != VK_NULL_HANDLE) {
            const auto destroy_debug =
                reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(instance,
                                          "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_debug)
                destroy_debug(instance, debug_messenger, nullptr);
        }
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }
};

detail::DeviceLifetimeControl::DeviceLifetimeControl(
    std::shared_ptr<DeviceAccessToken> device_access) noexcept
    : device_access_(std::move(device_access)) {
    if (device_access_) device_access_->register_control(*this);
}

detail::DeviceLifetimeControl::~DeviceLifetimeControl() {
    if (device_access_) device_access_->unregister_control(*this);
}

VkDevice detail::DeviceLifetimeControl::live_device() const noexcept {
    const VkDevice device =
        device_access_ ? device_access_->device() : VK_NULL_HANDLE;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (device != VK_NULL_HANDLE) {
        g_test_resource_destroy_call_total.fetch_add(1,
                                                     std::memory_order_relaxed);
    }
#endif
    return device;
}

void detail::DeviceAccessToken::register_control(
    DeviceLifetimeControl& control) noexcept {
    control.next_ = controls_;
    if (controls_) controls_->previous_ = &control;
    controls_ = &control;
}

void detail::DeviceAccessToken::unregister_control(
    DeviceLifetimeControl& control) noexcept {
    if (control.previous_) {
        control.previous_->next_ = control.next_;
    } else if (controls_ == &control) {
        controls_ = control.next_;
    }
    if (control.next_) control.next_->previous_ = control.previous_;
    control.previous_ = nullptr;
    control.next_ = nullptr;
}

void detail::DeviceAccessToken::destroy_registered_resources() noexcept {
    for (DeviceLifetimeControl* control = controls_; control;
         control = control->next_) {
        control->release_device_objects();
    }
}

VulkanDevice::VulkanDevice(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

VulkanDevice::~VulkanDevice() {
    if (impl_) impl_->cleanup();
}

std::unique_ptr<VulkanDevice> VulkanDevice::create(GLFWwindow* window,
                                                   bool enable_validation,
                                                   std::string& error) {
    error.clear();
    const auto make_device = [](StreamlineBridge bridge) {
        return std::unique_ptr<VulkanDevice>(
            new VulkanDevice(std::make_unique<Impl>(std::move(bridge))));
    };
    std::unique_ptr<VulkanDevice> result;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    const char* smoke_mode = std::getenv("MATTER_VK_SMOKE_MODE");
    if (smoke_mode &&
        std::strcmp(smoke_mode, "streamline-missing-instance-proxy") == 0) {
        result = make_device(StreamlineBridge::test_missing_proxy("instance"));
    } else if (smoke_mode &&
               std::strcmp(smoke_mode,
                           "streamline-missing-device-proxy") == 0) {
        result = make_device(StreamlineBridge::test_missing_proxy("device"));
    } else {
        // Ordinary smoke cases must not bind to a user-installed Streamline
        // runtime or its presentation proxies.
        result = make_device(StreamlineBridge::native_fallback(
            "Streamline disabled for Vulkan test device"));
    }
#else
    result = make_device(StreamlineBridge::initialize_before_vulkan());
#endif
    if (result->impl_->initialize(window, enable_validation, error)) {
        return result;
    }
    const bool retry_native = result->impl_->streamline.dlss_requested() ||
                              result->impl_->streamline.proxy_dispatch_used() ||
                              result->impl_->streamline.native_retry_required();
    if (!retry_native) return nullptr;

    result.reset();  // Teardown keeps proxy dispatch alive through destruction.
    error.clear();
    result = make_device(StreamlineBridge::native_fallback(
        "Streamline Vulkan requirements unavailable; retried native Vulkan"));
    if (result->impl_->initialize(window, enable_validation, error)) {
        return result;
    }
    return nullptr;
}

bool VulkanDevice::begin_frame(VulkanFrame& frame, std::string& error) {
    return impl_->begin_frame(frame, error);
}

bool VulkanDevice::end_frame(const VulkanFrame& frame, std::string& error) {
    bool presented = false;
    return impl_->end_frame(frame, presented, error);
}

bool VulkanDevice::end_frame(const VulkanFrame& frame, bool& presented,
                             std::string& error) {
    return impl_->end_frame(frame, presented, error);
}

bool VulkanDevice::retain_for_frame(
    const VulkanFrame& frame, std::vector<std::shared_ptr<void>> resources,
    std::string& error) {
    return impl_->retain_for_frame(frame, std::move(resources), error);
}

bool VulkanDevice::readback_swapchain_rgba8(const VulkanFrame& frame,
                                            std::vector<uint8_t>& rgba,
                                            std::string& error) {
    return impl_->queue_readback(frame, rgba, error);
}

bool VulkanDevice::submit_and_wait(VkCommandBuffer command_buffer,
                                   VkFence fence, bool& completion_proven,
                                   std::string& error) {
    return submit_and_wait_for_phase(command_buffer, fence, completion_proven,
                                     nullptr, error);
}

bool VulkanDevice::submit_and_wait_for_phase(VkCommandBuffer command_buffer,
                                             VkFence fence,
                                             bool& completion_proven,
                                             const char* fault_phase,
                                             std::string& error) {
#ifndef MATTER_VK_TEST_FAULT_INJECTION
    (void)fault_phase;
#endif
    error.clear();
    // No work is pending until vkQueueSubmit2 succeeds.
    completion_proven = true;
    if (!impl_->ensure_healthy(error)) return false;
    if (command_buffer == VK_NULL_HANDLE || fence == VK_NULL_HANDLE) {
        error = "submit_and_wait requires valid command buffer and fence handles";
        return impl_->poison_device(error,
                                    "immediate submission invariant failed");
    }
    VkCommandBufferSubmitInfo command_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    command_info.commandBuffer = command_buffer;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command_info;
    const VkResult submitted =
        vkQueueSubmit2(impl_->graphics_queue, 1, &submit, fence);
    if (submitted != VK_SUCCESS) {
        // A failed queue submission did not make this command buffer pending,
        // so its owning pool can be released even though the device is poisoned.
        completion_proven = true;
        vk_ok(submitted, "vkQueueSubmit2(immediate)", error);
        return impl_->poison_device(error, "immediate queue submission failed");
    }
    completion_proven = false;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    const char* force_ambiguous =
        std::getenv("MATTER_VK_TEST_FORCE_IMMEDIATE_WAIT_AMBIGUOUS");
    if (force_ambiguous && fault_phase &&
        std::strcmp(force_ambiguous, fault_phase) == 0) {
        error = "forced ambiguous immediate completion for retention test";
        return impl_->poison_device(
            error, "immediate submission completion is unproven");
    }
#endif
    const VkResult waited = vkWaitForFences(
        impl_->device, 1, &fence, VK_TRUE,
        std::numeric_limits<uint64_t>::max());
    if (waited != VK_SUCCESS) {
        vk_ok(waited, "vkWaitForFences(immediate)", error);
        return impl_->poison_device(
            error, "immediate submission completion is unproven");
    }
    completion_proven = true;
    return true;
}

void detail::DeviceRetentionAccess::retain(
    VulkanDevice& owner,
    std::unique_ptr<DeviceRetainedResource> resource) noexcept {
    if (!resource) return;
    resource->next = owner.impl_->retained_resources;
    owner.impl_->retained_resources = resource.release();
}

std::shared_ptr<detail::DeviceAccessToken>
detail::DeviceLifetimeAccess::token(VulkanDevice& owner) {
    return owner.impl_->device_lifetime;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
void detail::DeviceLifetimeAccess::reset_test_destroy_call_count() {
    g_test_resource_destroy_call_total.store(0, std::memory_order_relaxed);
}

uint32_t detail::DeviceLifetimeAccess::test_destroy_call_count() {
    return g_test_resource_destroy_call_total.load(std::memory_order_relaxed);
}
#endif

bool detail::DeviceSubmitAccess::submit_and_wait(
    VulkanDevice& owner, VkCommandBuffer command_buffer, VkFence fence,
    bool& completion_proven, const char* fault_phase, std::string& error) {
    return owner.submit_and_wait_for_phase(command_buffer, fence,
                                           completion_proven, fault_phase,
                                           error);
}

void VulkanDevice::wait_idle() {
    if (impl_->device == VK_NULL_HANDLE) return;
    if (impl_->device_poisoned) {
        std::fprintf(stderr, "%s\n", impl_->poison_error.c_str());
        return;
    }
    const VkResult result = impl_->streamline.device_wait_idle(impl_->device);
    if (result != VK_SUCCESS) {
        std::string error;
        vk_ok(result, "vkDeviceWaitIdle", error);
        impl_->poison_device(error, "wait_idle failed");
        std::fprintf(stderr, "%s\n", error.c_str());
    }
}

VkInstance VulkanDevice::instance() const { return impl_->instance; }
VkPhysicalDevice VulkanDevice::physical_device() const {
    return impl_->physical_device;
}
VkDevice VulkanDevice::device() const { return impl_->device; }
VkQueue VulkanDevice::graphics_queue() const { return impl_->graphics_queue; }
uint32_t VulkanDevice::graphics_queue_family() const {
    return impl_->graphics_queue_family;
}

VkFormat VulkanDevice::swapchain_format() const { return impl_->swapchain_format; }

uint32_t VulkanDevice::swapchain_image_count() const {
    return static_cast<uint32_t>(impl_->swapchain_images.size());
}
bool VulkanDevice::draw_indirect_first_instance_enabled() const {
    return impl_->draw_indirect_first_instance_enabled;
}
bool VulkanDevice::multi_draw_indirect_enabled() const {
    return impl_->multi_draw_indirect_enabled;
}

bool VulkanDevice::dlss_available() const {
    return impl_->streamline.dlss_available();
}

const std::string& VulkanDevice::dlss_unavailable_reason() const {
    return impl_->streamline.dlss_unavailable_reason();
}

StreamlineBridge& VulkanDevice::streamline_bridge() {
    return impl_->streamline;
}

const StreamlineBridge& VulkanDevice::streamline_bridge() const {
    return impl_->streamline;
}

bool VulkanDevice::ray_tracing_available() const {
    return impl_->ray_tracing_enabled;
}

const std::string& VulkanDevice::ray_tracing_unavailable_reason() const {
    return impl_->ray_tracing_reason;
}

const VulkanRayTracingProperties& VulkanDevice::ray_tracing_properties() const {
    return impl_->ray_tracing_properties;
}
uint32_t VulkanDevice::validation_error_count() const {
    return impl_->validation_errors.load(std::memory_order_relaxed);
}

void VulkanDevice::preserve_after_unproven_external_work() noexcept {
    impl_->preserve_external_work = true;
}
#ifdef MATTER_VK_TEST_FAULT_INJECTION
bool VulkanDevice::test_present_result_was_presented(VkResult result) {
    return present_result_was_presented(result);
}

uint32_t VulkanDevice::test_validation_error_total() {
    return g_test_validation_error_total.load(std::memory_order_relaxed);
}

const std::vector<std::string>& VulkanDevice::test_presentation_events() const {
    return impl_->streamline.test_presentation_events();
}

void VulkanDevice::test_clear_presentation_events() {
    impl_->streamline.clear_test_presentation_events();
}

uint64_t VulkanDevice::test_last_present_common_serial() const {
    return impl_->streamline.test_last_present_common_serial();
}
#endif

}  // namespace matter
