#pragma once
#include <matter/graphics/graphics.h>
#include <matter/graphics/vulkan_resources.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
namespace mg = matter::graphics;
namespace ms = matter::services;
namespace mc = matter::contracts;
namespace {
void check(bool condition, const char *id) {
  if (!condition) { std::cerr << "FAIL " << id << '\n'; std::exit(1); }
}
void vkcheck(VkResult r, const char *id) { check(r == VK_SUCCESS, id); }
void valid(const mc::ValidationResult &r, const char *id) {
  if (!r.ok()) for (const auto &d : r.diagnostics) std::cerr << d.message << '\n';
  check(r.ok(), id);
}
std::atomic<unsigned> validation_errors{0};
VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) ++validation_errors;
    std::cerr << "VULKAN " << ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) ? "VALIDATION: " : "LOADER: ")
              << data->pMessage << '\n';
  }
  return VK_FALSE;
}
struct NativeDevice {
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
  explicit NativeDevice(bool raster_features = false) {
    uint32_t count = 0; vkcheck(vkEnumerateInstanceLayerProperties(&count, nullptr), "VK layers");
    std::vector<VkLayerProperties> layers(count);
    vkcheck(vkEnumerateInstanceLayerProperties(&count, layers.data()), "VK layer names");
    bool validation = false;
    for (const auto &l : layers) if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) validation = true;
    if (std::getenv("MATTER_GRAPHICS_REQUIRE_VALIDATION")) check(validation, "VK required validation layer installed");
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = raster_features ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;
    VkDebugUtilsMessengerCreateInfoEXT debug{}; debug.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = message;
    VkInstanceCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; info.pApplicationInfo = &app;
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features{}; features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    features.enabledValidationFeatureCount = 1; features.pEnabledValidationFeatures = &sync; features.pNext = &debug;
    if (validation) {
      info.enabledLayerCount = 1; info.ppEnabledLayerNames = &layer;
      info.enabledExtensionCount = 2; info.ppEnabledExtensionNames = extensions; info.pNext = &features;
    }
    vkcheck(vkCreateInstance(&info, nullptr, &instance), "VK create instance");
    if (validation) {
      auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
      check(create != nullptr, "VK debug function"); vkcheck(create(instance, &debug, nullptr, &messenger), "VK debug messenger");
    }
    vkcheck(vkEnumeratePhysicalDevices(instance, &count, nullptr), "VK enumerate devices");
    std::vector<VkPhysicalDevice> devices(count);
    vkcheck(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "VK physical devices");
    for (auto candidate : devices) {
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
      for (uint32_t i = 0; i < count; ++i) if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        physical = candidate; family = i; break;
      }
      if (physical) break;
    }
    check(physical != VK_NULL_HANDLE, "VK graphics device required");
    const float priority = 1;
    VkDeviceQueueCreateInfo q{}; q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = family; q.queueCount = 1; q.pQueuePriorities = &priority;
    VkDeviceCreateInfo d{}; d.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; d.queueCreateInfoCount = 1; d.pQueueCreateInfos = &q;
    VkPhysicalDeviceVulkan13Features f13{}; f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{}; f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = raster_features ? &f13 : nullptr;
    VkPhysicalDeviceVulkan11Features f11{}; f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES; f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 available{}; available.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    available.pNext = raster_features ? static_cast<void*>(&f11) : &f12;
    vkGetPhysicalDeviceFeatures2(physical, &available);
    check(f12.timelineSemaphore, "VK timeline feature required");
    if (raster_features) check(f13.dynamicRendering && f13.synchronization2 && f13.shaderDemoteToHelperInvocation &&
          f12.timelineSemaphore && f12.bufferDeviceAddress && f12.shaderSampledImageArrayNonUniformIndexing &&
          f11.shaderDrawParameters && available.features.drawIndirectFirstInstance,
          "VK production raster features required");
    f13 = {}; f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = f13.synchronization2 = f13.shaderDemoteToHelperInvocation = VK_TRUE;
    f12 = {}; f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = raster_features ? &f13 : nullptr;
    f12.timelineSemaphore = VK_TRUE;
    f12.bufferDeviceAddress = f12.shaderSampledImageArrayNonUniformIndexing = raster_features ? VK_TRUE : VK_FALSE;
    f11 = {}; f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES; f11.pNext = &f12;
    f11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceFeatures enabled{}; enabled.drawIndirectFirstInstance = raster_features ? VK_TRUE : VK_FALSE;
    d.pEnabledFeatures = &enabled; d.pNext = raster_features ? static_cast<void*>(&f11) : &f12;
    vkcheck(vkCreateDevice(physical, &d, nullptr, &device), "VK create device");
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties);
    std::cout << "DEVICE " << properties.deviceName << "; validation=" << validation << '\n';
  }
  ~NativeDevice() {
    vkDestroyDevice(device, nullptr);
    if (messenger) {
      auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
      destroy(instance, messenger, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
  }
  mg::VulkanDeviceBinding binding(const std::shared_ptr<NativeDevice> &owner) {
    return {physical, device, queue, family, owner, mutex};
  }
};
} // namespace
