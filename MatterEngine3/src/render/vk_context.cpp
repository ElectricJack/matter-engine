#include "matter/vulkan_device.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace matter {
namespace {

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
            return {i};
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

}  // namespace

struct VulkanDevice::Impl {
    struct FrameSlot {
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkFence acquire_fence = VK_NULL_HANDLE;
        bool acquire_fence_pending = false;
        VkFence fence = VK_NULL_HANDLE;
    };

    GLFWwindow* window = nullptr;
    bool validation_enabled = false;
    std::atomic<uint32_t> validation_errors{0};
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkReleaseSwapchainImagesEXT release_swapchain_images = nullptr;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    uint32_t graphics_queue_family = std::numeric_limits<uint32_t>::max();
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent{};
    std::vector<VkImage> swapchain_images;
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
    bool wsi_completion_ambiguous = false;
    std::string poison_error;
    VulkanFrame active_frame{};

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
        if (!vk_ok(vkCreateInstance(&create, nullptr, &instance),
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
        return vk_ok(glfwCreateWindowSurface(instance, window, nullptr, &surface),
                     "glfwCreateWindowSurface", error);
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
        constexpr std::array<const char*, 4> required_extensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
            VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
        };
        for (const char* extension : required_extensions) {
            if (extensions.count(extension) == 0) missing.emplace_back(extension);
        }

        queue = find_graphics_present_queue(candidate, surface);
        if (queue.family == std::numeric_limits<uint32_t>::max()) {
            missing.emplace_back("graphics+present queue family");
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
        constexpr std::array<const char*, 4> extensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
            VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
        };
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_create{
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_create.queueFamilyIndex = graphics_queue_family;
        queue_create.queueCount = 1;
        queue_create.pQueuePriorities = &priority;

        VkPhysicalDeviceVulkan13Features features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        VkPhysicalDeviceVulkan12Features features12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance1{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        maintenance1.swapchainMaintenance1 = VK_TRUE;
        features12.pNext = &features13;
        features13.pNext = &maintenance1;
        features12.timelineSemaphore = VK_TRUE;
        features12.descriptorIndexing = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        features12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.bufferDeviceAddress = VK_TRUE;

        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.pNext = &features12;
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos = &queue_create;
        create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        if (!vk_ok(vkCreateDevice(physical_device, &create, nullptr, &device),
                   "vkCreateDevice", error)) {
            return false;
        }
        vkGetDeviceQueue(device, graphics_queue_family, 0, &graphics_queue);
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
        create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
        if (!vk_ok(vkCreateSwapchainKHR(device, &create, nullptr, &replacement),
                   "vkCreateSwapchainKHR", error)) {
            return false;
        }
        uint32_t actual_count = 0;
        VkResult result =
            vkGetSwapchainImagesKHR(device, replacement, &actual_count, nullptr);
        if (result != VK_SUCCESS || actual_count == 0) {
            vkDestroySwapchainKHR(device, replacement, nullptr);
            if (result == VK_SUCCESS) {
                error = "vkGetSwapchainImagesKHR returned zero images";
                return false;
            }
            return vk_ok(result, "vkGetSwapchainImagesKHR", error);
        }
        std::vector<VkImage> images(actual_count);
        result = vkGetSwapchainImagesKHR(device, replacement, &actual_count,
                                         images.data());
        if (result != VK_SUCCESS) {
            vkDestroySwapchainKHR(device, replacement, nullptr);
            return vk_ok(result, "vkGetSwapchainImagesKHR", error);
        }
        if (old_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, old_swapchain, nullptr);
        }
        swapchain = replacement;
        swapchain_format = format.format;
        swapchain_extent = extent;
        swapchain_images = std::move(images);
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
        if (!vk_ok(vkDeviceWaitIdle(device), "vkDeviceWaitIdle", error)) {
            return poison_device(error,
                                 "swapchain recreation could not establish idle");
        }
        if (wait_for_present_completion(error) != VK_SUCCESS)
            return poison_device(
                error, "swapchain recreation could not establish present completion");
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

        uint32_t image_index = 0;
        if (!vk_ok(vkResetFences(device, 1, &slot.acquire_fence),
                   "vkResetFences(acquire completion)", error)) {
            return false;
        }
        VkResult acquire = vkAcquireNextImageKHR(
            device, swapchain, std::numeric_limits<uint64_t>::max(),
            slot.image_available, slot.acquire_fence, &image_index);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!recreate_swapchain(error)) return false;
            if (!vk_ok(vkResetFences(device, 1, &slot.acquire_fence),
                       "vkResetFences(acquire completion)", error)) {
                return false;
            }
            acquire = vkAcquireNextImageKHR(
                device, swapchain, std::numeric_limits<uint64_t>::max(),
                slot.image_available, slot.acquire_fence, &image_index);
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            return vk_ok(acquire, "vkAcquireNextImageKHR", error);
        }
        slot.acquire_fence_pending = true;
        if (!vk_ok(vkWaitForFences(
                       device, 1, &slot.acquire_fence, VK_TRUE,
                       std::numeric_limits<uint64_t>::max()),
                   "vkWaitForFences(acquire completion)", error)) {
            return poison_device(
                error, "acquisition completion could not be established");
        }
        slot.acquire_fence_pending = false;
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
        output.image_index = image_index;
        output.extent = swapchain_extent;
        output.serial = next_serial++;
        output.swapchain_recreated = report_recreated;
        report_recreated = false;
        active_frame = output;
        frame_active = true;
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
        bool recovered = true;
        std::string recovery_error;
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

    bool end_frame(const VulkanFrame& input, std::string& error) {
        error.clear();
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
        if (!vk_ok(vkEndCommandBuffer(slot.command_buffer),
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
        if (!vk_ok(vkQueueSubmit2(graphics_queue, 1, &submit, slot.fence),
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
        const VkResult present_result =
            vkQueuePresentKHR(graphics_queue, &present);
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
        if (device != VK_NULL_HANDLE) {
            const VkResult idle = vkDeviceWaitIdle(device);
            bool device_lost = idle == VK_ERROR_DEVICE_LOST;
            if (!destruction_safe_after_wait(idle)) {
                std::fprintf(stderr,
                             "Vulkan cleanup intentionally preserving the "
                             "logical device, WSI resources, and their parents "
                             "after %s; completion is unproven\n",
                             result_name(idle));
                return;
            }
            if (wsi_completion_ambiguous && !device_lost) {
                std::fprintf(
                    stderr,
                    "Vulkan cleanup intentionally preserving the logical "
                    "device, ambiguous presentation resources, and their "
                    "parents; vkQueuePresentKHR completion is unproven\n");
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
                    return;
                }
            }
            if (device_lost) {
                std::fprintf(stderr,
                             "Vulkan device was lost; destroying device objects "
                             "without further completion waits\n");
            }
        }
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
        if (swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(instance, surface, nullptr);
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

VulkanDevice::VulkanDevice() : impl_(std::make_unique<Impl>()) {}

VulkanDevice::~VulkanDevice() {
    if (impl_) impl_->cleanup();
}

std::unique_ptr<VulkanDevice> VulkanDevice::create(GLFWwindow* window,
                                                   bool enable_validation,
                                                   std::string& error) {
    error.clear();
    std::unique_ptr<VulkanDevice> result(new VulkanDevice());
    if (!result->impl_->initialize(window, enable_validation, error)) {
        return nullptr;
    }
    return result;
}

bool VulkanDevice::begin_frame(VulkanFrame& frame, std::string& error) {
    return impl_->begin_frame(frame, error);
}

bool VulkanDevice::end_frame(const VulkanFrame& frame, std::string& error) {
    return impl_->end_frame(frame, error);
}

bool VulkanDevice::submit_and_wait(VkCommandBuffer command_buffer,
                                   VkFence fence, bool& completion_proven,
                                   std::string& error) {
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

void VulkanDevice::wait_idle() {
    if (impl_->device == VK_NULL_HANDLE) return;
    if (impl_->device_poisoned) {
        std::fprintf(stderr, "%s\n", impl_->poison_error.c_str());
        return;
    }
    const VkResult result = vkDeviceWaitIdle(impl_->device);
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
uint32_t VulkanDevice::validation_error_count() const {
    return impl_->validation_errors.load(std::memory_order_relaxed);
}

}  // namespace matter
