#include "streamline_bridge.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <utility>

#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
#include <windows.h>

#include <sl.h>
#include <sl_helpers_vk.h>
#include <sl_security.h>
#endif

namespace matter {
namespace {

template <typename Features>
void merge_feature_bits(Features& destination, const Features& source) {
    // Both Vulkan core feature structs start with sType and pNext; every
    // remaining member is VkBool32.  Preserve the caller-owned pNext chain.
    constexpr size_t kFirstFeature = sizeof(VkBaseOutStructure);
    static_assert(sizeof(Features) >= kFirstFeature);
    auto* destination_bits = reinterpret_cast<VkBool32*>(
        reinterpret_cast<unsigned char*>(&destination) + kFirstFeature);
    const auto* source_bits = reinterpret_cast<const VkBool32*>(
        reinterpret_cast<const unsigned char*>(&source) + kFirstFeature);
    constexpr size_t kFeatureCount =
        (sizeof(Features) - kFirstFeature) / sizeof(VkBool32);
    for (size_t i = 0; i < kFeatureCount; ++i) {
        destination_bits[i] = destination_bits[i] || source_bits[i]
                                  ? VK_TRUE
                                  : VK_FALSE;
    }
}

template <typename Features>
bool required_feature_bits_supported(const Features& required,
                                    const Features& supported) {
    constexpr size_t kFirstFeature = sizeof(VkBaseOutStructure);
    const auto* required_bits = reinterpret_cast<const VkBool32*>(
        reinterpret_cast<const unsigned char*>(&required) + kFirstFeature);
    const auto* supported_bits = reinterpret_cast<const VkBool32*>(
        reinterpret_cast<const unsigned char*>(&supported) + kFirstFeature);
    constexpr size_t kFeatureCount =
        (sizeof(Features) - kFirstFeature) / sizeof(VkBool32);
    for (size_t i = 0; i < kFeatureCount; ++i) {
        if (required_bits[i] && !supported_bits[i]) return false;
    }
    return true;
}

#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
std::wstring interposer_path() {
    std::array<wchar_t, MAX_PATH> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                             executable.size());
    if (length == 0 || length >= executable.size()) return {};
    std::wstring path(executable.data(), length);
    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return {};
    path.resize(separator + 1);
    path += L"sl.interposer.dll";
    return path;
}

std::string result_reason(const char* operation, sl::Result result) {
    std::ostringstream out;
    out << operation << " failed (Streamline result "
        << static_cast<int>(result) << ")";
    return out.str();
}

using SlInitFn = sl::Result (*)(const sl::Preferences&, uint64_t);
using SlGetFeatureRequirementsFn =
    sl::Result (*)(sl::Feature, sl::FeatureRequirements&);
using SlSetVulkanInfoFn = sl::Result (*)(const sl::VulkanInfo&);
using SlShutdownFn = sl::Result (*)();

template <typename Function>
Function streamline_function(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}
#endif

}  // namespace

StreamlineBridge StreamlineBridge::initialize_before_vulkan() {
    StreamlineBridge bridge;
    bridge.initialized_ = true;
#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
    const std::wstring dll_path = interposer_path();
    if (dll_path.empty() || GetFileAttributesW(dll_path.c_str()) ==
                                 INVALID_FILE_ATTRIBUTES) {
        bridge.dlss_unavailable_reason_ =
            "Streamline SDK not found: sl.interposer.dll is missing beside the executable";
        return bridge;
    }
    if (!sl::security::verifyEmbeddedSignature(dll_path.c_str())) {
        bridge.dlss_unavailable_reason_ =
            "Streamline SDK signature verification failed for sl.interposer.dll";
        return bridge;
    }
    const HMODULE module = LoadLibraryW(dll_path.c_str());
    if (!module) {
        bridge.dlss_unavailable_reason_ =
            "Streamline SDK could not load the signed sl.interposer.dll";
        return bridge;
    }
    bridge.streamline_module_ = module;
    bridge.sl_init_ = reinterpret_cast<void*>(
        streamline_function<SlInitFn>(module, "slInit"));
    bridge.sl_get_feature_requirements_ = reinterpret_cast<void*>(
        streamline_function<SlGetFeatureRequirementsFn>(
            module, "slGetFeatureRequirements"));
    bridge.sl_set_vulkan_info_ = reinterpret_cast<void*>(
        streamline_function<SlSetVulkanInfoFn>(module, "slSetVulkanInfo"));
    bridge.sl_shutdown_ = reinterpret_cast<void*>(
        streamline_function<SlShutdownFn>(module, "slShutdown"));
    if (!bridge.sl_init_ || !bridge.sl_get_feature_requirements_ ||
        !bridge.sl_set_vulkan_info_ || !bridge.sl_shutdown_) {
        bridge.dlss_unavailable_reason_ =
            "Streamline SDK is missing a required exported entry point";
        FreeLibrary(module);
        bridge.streamline_module_ = nullptr;
        return bridge;
    }
    bridge.get_instance_proc_addr_proxy_ =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(module, "vkGetInstanceProcAddr"));
    bridge.get_device_proc_addr_proxy_ = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        GetProcAddress(module, "vkGetDeviceProcAddr"));
    if (!bridge.get_instance_proc_addr_proxy_ ||
        !bridge.get_device_proc_addr_proxy_) {
        bridge.disable("Streamline SDK is missing Vulkan manual-hook dispatchers");
        return bridge;
    }
    bridge.create_instance_proxy_ = reinterpret_cast<PFN_vkCreateInstance>(
        bridge.get_instance_proc_addr_proxy_(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!bridge.create_instance_proxy_) {
        bridge.disable("Streamline SDK could not provide vkCreateInstance proxy");
        return bridge;
    }

    const std::array<sl::Feature, 1> features = {sl::kFeatureDLSS};
    sl::Preferences preferences{};
    preferences.featuresToLoad = features.data();
    preferences.numFeaturesToLoad = static_cast<uint32_t>(features.size());
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    preferences.flags |= sl::PreferenceFlags::eUseManualHooking |
                         sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    const sl::Result init_result =
        reinterpret_cast<SlInitFn>(bridge.sl_init_)(preferences, sl::kSDKVersion);
    if (init_result != sl::Result::eOk) {
        bridge.disable(result_reason("slInit", init_result));
        return bridge;
    }
    bridge.streamline_initialized_ = true;

    sl::FeatureRequirements requirements{};
    const sl::Result requirements_result =
        reinterpret_cast<SlGetFeatureRequirementsFn>(
            bridge.sl_get_feature_requirements_)(sl::kFeatureDLSS, requirements);
    if (requirements_result != sl::Result::eOk) {
        bridge.disable(
            result_reason("slGetFeatureRequirements(DLSS)", requirements_result));
        return bridge;
    }

    bridge.dlss_requested_ = true;
    bridge.dlss_available_ = true;
    bridge.instance_extensions_.assign(
        requirements.vkInstanceExtensions,
        requirements.vkInstanceExtensions + requirements.vkNumInstanceExtensions);
    bridge.device_extensions_.assign(
        requirements.vkDeviceExtensions,
        requirements.vkDeviceExtensions + requirements.vkNumDeviceExtensions);
    bridge.required_features12_ = sl::getVkPhysicalDeviceVulkan12Features(
        requirements.vkNumFeatures12, requirements.vkFeatures12);
    bridge.required_features13_ = sl::getVkPhysicalDeviceVulkan13Features(
        requirements.vkNumFeatures13, requirements.vkFeatures13);
    bridge.additional_graphics_queues_ = requirements.vkNumGraphicsQueuesRequired;
    bridge.additional_compute_queues_ = requirements.vkNumComputeQueuesRequired;
    bridge.use_proxy_dispatch_ = true;
#else
    bridge.dlss_unavailable_reason_ =
        "Streamline SDK not found: build with HAVE_STREAMLINE=1 to enable DLSS";
#endif
    return bridge;
}

StreamlineBridge StreamlineBridge::native_fallback(std::string reason) {
    StreamlineBridge bridge;
    bridge.initialized_ = true;
    bridge.dlss_unavailable_reason_ = std::move(reason);
    return bridge;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
void StreamlineBridge::record_test_presentation_event(const char* event) {
    test_presentation_events_.emplace_back(event);
}
#endif

bool StreamlineBridge::validate_requirements(
    const VkPhysicalDeviceVulkan12Features& supported_features12,
    const VkPhysicalDeviceVulkan13Features& supported_features13,
    std::string& error) const {
    if (!dlss_requested_) return true;
    if (!required_feature_bits_supported(required_features12_,
                                         supported_features12)) {
        error = "Streamline requires unavailable Vulkan 1.2 feature bits";
        return false;
    }
    if (!required_feature_bits_supported(required_features13_,
                                         supported_features13)) {
        error = "Streamline requires unavailable Vulkan 1.3 feature bits";
        return false;
    }
    return true;
}

std::vector<const char*> StreamlineBridge::merge_extensions(
    const std::vector<const char*>& first, const std::vector<const char*>& second) {
    std::vector<const char*> merged;
    merged.reserve(first.size() + second.size());
    const auto append_unique = [&merged](const std::vector<const char*>& names) {
        for (const char* name : names) {
            const bool already_present = std::any_of(
                merged.begin(), merged.end(), [name](const char* existing) {
                    return std::strcmp(existing, name) == 0;
                });
            if (!already_present) merged.push_back(name);
        }
    };
    append_unique(first);
    append_unique(second);
    return merged;
}

void StreamlineBridge::append_requirements(
    std::vector<const char*>& instance_extensions,
    std::vector<const char*>& device_extensions,
    VkPhysicalDeviceVulkan12Features& features12,
    VkPhysicalDeviceVulkan13Features& features13, uint32_t& graphics_queue_count,
    uint32_t& compute_queue_count) const {
    instance_extensions = merge_extensions(instance_extensions, instance_extensions_);
    device_extensions = merge_extensions(device_extensions, device_extensions_);
    merge_feature_bits(features12, required_features12_);
    merge_feature_bits(features13, required_features13_);
    graphics_queue_count += additional_graphics_queues_;
    compute_queue_count += additional_compute_queues_;
}

bool StreamlineBridge::set_vulkan_info(
    VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
    uint32_t graphics_queue_family, uint32_t graphics_queue_index,
    uint32_t compute_queue_family, uint32_t compute_queue_index) {
#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
    if (!dlss_requested_) return true;
    sl::VulkanInfo info{};
    info.instance = instance;
    info.physicalDevice = physical_device;
    info.device = device;
    info.graphicsQueueFamily = graphics_queue_family;
    info.graphicsQueueIndex = graphics_queue_index;
    info.computeQueueFamily = compute_queue_family;
    info.computeQueueIndex = compute_queue_index;
    const sl::Result result =
        reinterpret_cast<SlSetVulkanInfoFn>(sl_set_vulkan_info_)(info);
    if (result == sl::Result::eOk) return true;
    disable(result_reason("slSetVulkanInfo", result));
    return true;
#else
    (void)instance;
    (void)physical_device;
    (void)device;
    (void)graphics_queue_family;
    (void)graphics_queue_index;
    (void)compute_queue_family;
    (void)compute_queue_index;
    return true;
#endif
}

void StreamlineBridge::disable(std::string reason) {
    dlss_requested_ = false;
    dlss_available_ = false;
    dlss_unavailable_reason_ = std::move(reason);
    // A proxy-created Vulkan object must keep the interposer loaded and all
    // proxy calls routed until that object is destroyed.  The owner retries
    // the complete initialization natively after this stack is torn down.
    if (proxy_object_created_) return;
    use_proxy_dispatch_ = false;
    instance_extensions_.clear();
    device_extensions_.clear();
    required_features12_ = {};
    required_features13_ = {};
    additional_graphics_queues_ = 0;
    additional_compute_queues_ = 0;
#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
    if (streamline_initialized_ && sl_shutdown_) {
        reinterpret_cast<SlShutdownFn>(sl_shutdown_)();
        streamline_initialized_ = false;
    }
    if (streamline_module_) {
        FreeLibrary(static_cast<HMODULE>(streamline_module_));
        streamline_module_ = nullptr;
    }
#endif
}

void StreamlineBridge::shutdown() {
#if defined(MATTER_HAVE_STREAMLINE) && MATTER_HAVE_STREAMLINE
    if (streamline_initialized_) {
        reinterpret_cast<SlShutdownFn>(sl_shutdown_)();
        streamline_initialized_ = false;
    }
    if (streamline_module_) {
        FreeLibrary(static_cast<HMODULE>(streamline_module_));
        streamline_module_ = nullptr;
    }
#endif
}

bool StreamlineBridge::populate_instance_proxies(VkInstance instance) {
    if (!get_instance_proc_addr_proxy_) return false;
    create_device_proxy_ = reinterpret_cast<PFN_vkCreateDevice>(
        get_instance_proc_addr_proxy_(instance, "vkCreateDevice"));
    return create_device_proxy_ != nullptr;
}

bool StreamlineBridge::populate_device_proxies(VkDevice device) {
    if (!get_device_proc_addr_proxy_) return false;
    queue_present_proxy_ = reinterpret_cast<PFN_vkQueuePresentKHR>(
        get_device_proc_addr_proxy_(device, "vkQueuePresentKHR"));
    create_swapchain_proxy_ = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        get_device_proc_addr_proxy_(device, "vkCreateSwapchainKHR"));
    get_swapchain_images_proxy_ = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
        get_device_proc_addr_proxy_(device, "vkGetSwapchainImagesKHR"));
    destroy_swapchain_proxy_ = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
        get_device_proc_addr_proxy_(device, "vkDestroySwapchainKHR"));
    acquire_next_image_proxy_ = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
        get_device_proc_addr_proxy_(device, "vkAcquireNextImageKHR"));
    device_wait_idle_proxy_ = reinterpret_cast<PFN_vkDeviceWaitIdle>(
        get_device_proc_addr_proxy_(device, "vkDeviceWaitIdle"));
    return queue_present_proxy_ && create_swapchain_proxy_ &&
           get_swapchain_images_proxy_ && destroy_swapchain_proxy_ &&
           acquire_next_image_proxy_ && device_wait_idle_proxy_;
}

VkResult StreamlineBridge::create_instance(
    const VkInstanceCreateInfo* create, const VkAllocationCallbacks* allocator,
    VkInstance* instance) {
    if (use_proxy_dispatch_ && create_instance_proxy_) {
        proxy_dispatch_used_ = true;
        const VkResult result = create_instance_proxy_(create, allocator, instance);
        if (result == VK_SUCCESS) {
            proxy_object_created_ = true;
#ifdef _WIN32
            create_win32_surface_proxy_ =
                reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
                    get_instance_proc_addr_proxy_(*instance,
                                                  "vkCreateWin32SurfaceKHR"));
            destroy_surface_proxy_ = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
                get_instance_proc_addr_proxy_(*instance, "vkDestroySurfaceKHR"));
            if (!populate_instance_proxies(*instance) ||
                !create_win32_surface_proxy_ || !destroy_surface_proxy_) {
                disable("Streamline SDK could not provide required instance manual hooks");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
#else
            if (!populate_instance_proxies(*instance)) {
                disable("Streamline SDK could not provide vkCreateDevice proxy");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
#endif
        }
        return result;
    }
    return vkCreateInstance(create, allocator, instance);
}

VkResult StreamlineBridge::create_device(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create,
    const VkAllocationCallbacks* allocator, VkDevice* device) {
    if (use_proxy_dispatch_ && create_device_proxy_) {
        proxy_dispatch_used_ = true;
        const VkResult result =
            create_device_proxy_(physical_device, create, allocator, device);
        if (result == VK_SUCCESS) {
            proxy_object_created_ = true;
            if (!populate_device_proxies(*device)) {
                disable("Streamline SDK could not provide required Vulkan manual hooks");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        return result;
    }
    return vkCreateDevice(physical_device, create, allocator, device);
}

VkResult StreamlineBridge::get_swapchain_images(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t* image_count,
    VkImage* images) {
    if (use_proxy_dispatch_ && get_swapchain_images_proxy_) {
        proxy_dispatch_used_ = true;
        return get_swapchain_images_proxy_(device, swapchain, image_count, images);
    }
    return vkGetSwapchainImagesKHR(device, swapchain, image_count, images);
}

VkResult StreamlineBridge::queue_present(VkQueue queue,
                                         const VkPresentInfoKHR* present) {
    if (use_proxy_dispatch_ &&
        (!present_common_pending_ || present_common_serial_ == 0)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (use_proxy_dispatch_) {
        present_common_pending_ = false;
        present_common_serial_ = 0;
    }
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    record_test_presentation_event("present");
#endif
    if (use_proxy_dispatch_ && queue_present_proxy_) {
        proxy_dispatch_used_ = true;
        return queue_present_proxy_(queue, present);
    }
    return vkQueuePresentKHR(queue, present);
}

bool StreamlineBridge::present_common(uint64_t frame_serial) {
    if (frame_serial == 0) return false;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    last_present_common_serial_ = frame_serial;
    record_test_presentation_event("present_common");
#endif
    if (!use_proxy_dispatch_) return true;
    if (present_common_pending_) return false;
    // vkQueuePresentKHR is one of Streamline's required Vulkan hooks.  Its
    // proxy enters the common plugin's presentCommon() implementation.
    present_common_pending_ = true;
    present_common_serial_ = frame_serial;
    return true;
}

VkResult StreamlineBridge::create_swapchain(
    VkDevice device, const VkSwapchainCreateInfoKHR* create,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    record_test_presentation_event("create_swapchain");
#endif
    if (use_proxy_dispatch_ && create_swapchain_proxy_) {
        proxy_dispatch_used_ = true;
        return create_swapchain_proxy_(device, create, allocator, swapchain);
    }
    return vkCreateSwapchainKHR(device, create, allocator, swapchain);
}

void StreamlineBridge::destroy_swapchain(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* allocator) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    record_test_presentation_event("destroy_swapchain");
#endif
    if (use_proxy_dispatch_ && destroy_swapchain_proxy_) {
        proxy_dispatch_used_ = true;
        destroy_swapchain_proxy_(device, swapchain, allocator);
        return;
    }
    vkDestroySwapchainKHR(device, swapchain, allocator);
}

VkResult StreamlineBridge::acquire_next_image(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t* image_index) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    record_test_presentation_event("acquire");
#endif
    if (use_proxy_dispatch_ && acquire_next_image_proxy_) {
        proxy_dispatch_used_ = true;
        return acquire_next_image_proxy_(device, swapchain, timeout, semaphore,
                                         fence, image_index);
    }
    return vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence,
                                 image_index);
}

VkResult StreamlineBridge::device_wait_idle(VkDevice device) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    record_test_presentation_event("device_wait_idle");
#endif
    if (use_proxy_dispatch_ && device_wait_idle_proxy_) {
        proxy_dispatch_used_ = true;
        return device_wait_idle_proxy_(device);
    }
    return vkDeviceWaitIdle(device);
}

#ifdef _WIN32
VkResult StreamlineBridge::create_win32_surface(
    VkInstance instance, const VkWin32SurfaceCreateInfoKHR* create,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    if (use_proxy_dispatch_ && create_win32_surface_proxy_) {
        proxy_dispatch_used_ = true;
        return create_win32_surface_proxy_(instance, create, allocator, surface);
    }
    return vkCreateWin32SurfaceKHR(instance, create, allocator, surface);
}
#endif

void StreamlineBridge::destroy_surface(
    VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks* allocator) {
    if (use_proxy_dispatch_ && destroy_surface_proxy_) {
        proxy_dispatch_used_ = true;
        destroy_surface_proxy_(instance, surface, allocator);
        return;
    }
    vkDestroySurfaceKHR(instance, surface, allocator);
}

}  // namespace matter
