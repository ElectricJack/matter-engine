#include "render/vk_cuda_interop.h"

#include <windows.h>
#include <cuda.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>

#include "matter/vulkan_device.h"
#include "shaders_gen/embedded_cuda_interop_ptx.h"

namespace matter {
namespace {

std::string hex_id(const uint8_t* bytes, size_t size) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) out << std::setw(2) << unsigned(bytes[i]);
    return out.str();
}

float half_to_float(uint16_t value) {
    const uint32_t sign = uint32_t(value & 0x8000u) << 16u;
    uint32_t exponent = (value >> 10u) & 0x1fu;
    uint32_t mantissa = value & 0x3ffu;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 113;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1u;
                --exponent;
            }
            bits = sign | (exponent << 23u) | ((mantissa & 0x3ffu) << 13u);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13u);
    } else {
        bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
    }
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

bool vk_result(VkResult result, const char* call, std::string& error) {
    if (result == VK_SUCCESS) return true;
    error = std::string(call) + " failed with VkResult " +
            std::to_string(static_cast<int>(result));
    return false;
}

struct CudaApi {
    HMODULE library = nullptr;
#define CUDA_FN(member, type) type member = nullptr
    CUDA_FN(init, decltype(&cuInit));
    CUDA_FN(device_get_count, decltype(&cuDeviceGetCount));
    CUDA_FN(device_get, decltype(&cuDeviceGet));
    CUDA_FN(device_get_name, decltype(&cuDeviceGetName));
    CUDA_FN(device_get_uuid, decltype(&cuDeviceGetUuid));
    CUDA_FN(device_get_luid, decltype(&cuDeviceGetLuid));
    CUDA_FN(ctx_create, decltype(&cuCtxCreate));
    CUDA_FN(ctx_destroy, decltype(&cuCtxDestroy));
    CUDA_FN(ctx_set_current, decltype(&cuCtxSetCurrent));
    CUDA_FN(stream_create, decltype(&cuStreamCreate));
    CUDA_FN(stream_destroy, decltype(&cuStreamDestroy));
    CUDA_FN(module_load_data, decltype(&cuModuleLoadData));
    CUDA_FN(module_unload, decltype(&cuModuleUnload));
    CUDA_FN(module_get_function, decltype(&cuModuleGetFunction));
    CUDA_FN(launch_kernel, decltype(&cuLaunchKernel));
    CUDA_FN(import_memory, decltype(&cuImportExternalMemory));
    CUDA_FN(destroy_memory, decltype(&cuDestroyExternalMemory));
    CUDA_FN(map_mipmapped, decltype(&cuExternalMemoryGetMappedMipmappedArray));
    CUDA_FN(mip_level, decltype(&cuMipmappedArrayGetLevel));
    CUDA_FN(destroy_mipmapped, decltype(&cuMipmappedArrayDestroy));
    CUDA_FN(surface_create, decltype(&cuSurfObjectCreate));
    CUDA_FN(surface_destroy, decltype(&cuSurfObjectDestroy));
    CUDA_FN(import_semaphore, decltype(&cuImportExternalSemaphore));
    CUDA_FN(destroy_semaphore, decltype(&cuDestroyExternalSemaphore));
    CUDA_FN(wait_semaphore, decltype(&cuWaitExternalSemaphoresAsync));
    CUDA_FN(signal_semaphore, decltype(&cuSignalExternalSemaphoresAsync));
    CUDA_FN(stream_synchronize, decltype(&cuStreamSynchronize));
    CUDA_FN(error_name, decltype(&cuGetErrorName));
    CUDA_FN(error_string, decltype(&cuGetErrorString));
#undef CUDA_FN

    ~CudaApi() {
        if (library) FreeLibrary(library);
    }

    template <typename T>
    bool load(T& target, const char* name, std::string& error) {
        const FARPROC symbol = GetProcAddress(library, name);
        static_assert(sizeof(target) == sizeof(symbol),
                      "Win32 function pointers must have equal size");
        std::memcpy(&target, &symbol, sizeof(target));
        if (target) return true;
        error = std::string("nvcuda.dll is missing CUDA Driver API symbol ") + name;
        return false;
    }

    bool open(std::string& error) {
        library = LoadLibraryA("nvcuda.dll");
        if (!library) {
            error = "LoadLibraryA(nvcuda.dll) failed: " + std::to_string(GetLastError());
            return false;
        }
#define LOAD(member, symbol) if (!load(member, symbol, error)) return false
        LOAD(init, "cuInit");
        LOAD(device_get_count, "cuDeviceGetCount");
        LOAD(device_get, "cuDeviceGet");
        LOAD(device_get_name, "cuDeviceGetName");
        LOAD(device_get_uuid, "cuDeviceGetUuid_v2");
        LOAD(device_get_luid, "cuDeviceGetLuid");
        LOAD(ctx_create, "cuCtxCreate_v4");
        LOAD(ctx_destroy, "cuCtxDestroy_v2");
        LOAD(ctx_set_current, "cuCtxSetCurrent");
        LOAD(stream_create, "cuStreamCreate");
        LOAD(stream_destroy, "cuStreamDestroy_v2");
        LOAD(module_load_data, "cuModuleLoadData");
        LOAD(module_unload, "cuModuleUnload");
        LOAD(module_get_function, "cuModuleGetFunction");
        LOAD(launch_kernel, "cuLaunchKernel");
        LOAD(import_memory, "cuImportExternalMemory");
        LOAD(destroy_memory, "cuDestroyExternalMemory");
        LOAD(map_mipmapped, "cuExternalMemoryGetMappedMipmappedArray");
        LOAD(mip_level, "cuMipmappedArrayGetLevel");
        LOAD(destroy_mipmapped, "cuMipmappedArrayDestroy");
        LOAD(surface_create, "cuSurfObjectCreate");
        LOAD(surface_destroy, "cuSurfObjectDestroy");
        LOAD(import_semaphore, "cuImportExternalSemaphore");
        LOAD(destroy_semaphore, "cuDestroyExternalSemaphore");
        LOAD(wait_semaphore, "cuWaitExternalSemaphoresAsync");
        LOAD(signal_semaphore, "cuSignalExternalSemaphoresAsync");
        LOAD(stream_synchronize, "cuStreamSynchronize");
        LOAD(error_name, "cuGetErrorName");
        LOAD(error_string, "cuGetErrorString");
#undef LOAD
        return true;
    }

    bool ok(CUresult result, const char* call, std::string& error) const {
        if (result == CUDA_SUCCESS) return true;
        const char* name = nullptr;
        const char* message = nullptr;
        if (error_name) error_name(result, &name);
        if (error_string) error_string(result, &message);
        error = std::string(call) + " failed: " + (name ? name : "CUDA_ERROR") +
                (message ? std::string(" (") + message + ")" : "");
        return false;
    }
};

uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits,
                     VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & required) == required)
            return i;
    }
    return UINT32_MAX;
}

}  // namespace

bool cuda_vulkan_device_ids_match_for_test(
    const std::array<uint8_t, VK_UUID_SIZE>& vulkan_uuid,
    const std::array<uint8_t, VK_UUID_SIZE>& cuda_uuid,
    bool force_mismatch) noexcept {
    return !force_mismatch && vulkan_uuid == cuda_uuid;
}

uint32_t win32_process_handle_count() noexcept {
    DWORD count = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &count) ? count : UINT32_MAX;
}

struct CudaVulkanInterop::Impl {
    VulkanDevice* vulkan = nullptr;
    CudaApi cuda;
    CUdevice cuda_device = 0;
    CUcontext cuda_context = nullptr;
    CUstream stream = nullptr;
    CUmodule module = nullptr;
    CUfunction kernel = nullptr;
    CUexternalSemaphore cuda_semaphore = nullptr;
    VkSemaphore vk_semaphore = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::array<uint8_t, VK_UUID_SIZE> vk_uuid{};
    std::array<uint8_t, VK_UUID_SIZE> cu_uuid{};
    std::string vk_name;
    std::string cu_name;

    bool diagnostics() const noexcept {
        const char* value = std::getenv("MATTER_VK_INTEROP_HANDLE_DIAGNOSTICS");
        return value && std::strcmp(value, "1") == 0;
    }
    void trace(const char* boundary, int result = 0,
               const char* sync = "n/a") const noexcept {
        if (diagnostics())
            std::printf("HANDLE_DIAG %-38s count=%u result=%d sync=%s\n",
                        boundary, win32_process_handle_count(), result, sync);
    }

    struct Cycle {
        Impl& owner;
        VkExtent2D extent{};
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBuffer readback = VK_NULL_HANDLE;
        VkDeviceMemory readback_memory = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer commands[2]{};
        CUexternalMemory external_memory = nullptr;
        CUmipmappedArray mipmapped = nullptr;
        CUsurfObject surface = 0;
        bool cuda_work_pending = false;
        uint64_t pending_timeline = 0;
        bool initialized_layout = false;
        ~Cycle() {
            if (pending_timeline && owner.vk_semaphore) {
                VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                wait.semaphoreCount = 1;
                wait.pSemaphores = &owner.vk_semaphore;
                wait.pValues = &pending_timeline;
                const VkResult result = vkWaitSemaphores(owner.vulkan->device(),
                                                         &wait, UINT64_MAX);
                owner.trace("vkWaitSemaphores(failure)", int(result),
                            "blocking failure cleanup only");
            }
            if (cuda_work_pending && owner.stream) {
                const CUresult result = owner.cuda.stream_synchronize(owner.stream);
                owner.trace("cuStreamSynchronize(failure)", int(result),
                            "blocking teardown only");
            }
            const CUresult sr = surface ? owner.cuda.surface_destroy(surface) : CUDA_SUCCESS;
            owner.trace("cuSurfObjectDestroy", int(sr), "synchronous API");
            const CUresult ar = mipmapped ? owner.cuda.destroy_mipmapped(mipmapped) : CUDA_SUCCESS;
            owner.trace("cuMipmappedArrayDestroy", int(ar), "synchronous API");
            const CUresult mr = external_memory ? owner.cuda.destroy_memory(external_memory) : CUDA_SUCCESS;
            owner.trace("cuDestroyExternalMemory", int(mr), "synchronous API");
            if (sr != CUDA_SUCCESS || ar != CUDA_SUCCESS || mr != CUDA_SUCCESS)
                std::fprintf(stderr, "CUDA interop cleanup errors: surface=%d mip=%d memory=%d\n",
                             int(sr), int(ar), int(mr));
            VkDevice device = owner.vulkan->device();
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (commands[0]) vkFreeCommandBuffers(device, owner.command_pool, 2, commands);
            if (readback) vkDestroyBuffer(device, readback, nullptr);
            if (readback_memory) vkFreeMemory(device, readback_memory, nullptr);
            if (image) vkDestroyImage(device, image, nullptr);
            if (memory) vkFreeMemory(device, memory, nullptr);
        }
    };
    std::vector<std::unique_ptr<Cycle>> size_classes;
    static constexpr size_t kMaxSizeClasses = 4;
    uint64_t last_serial = 0;

    ~Impl() { reset(); }

    void reset() noexcept {
        if (cuda_context) cuda.ctx_set_current(cuda_context);
        size_classes.clear();
        if (cuda_semaphore) { const CUresult r = cuda.destroy_semaphore(cuda_semaphore); trace("cuDestroyExternalSemaphore", int(r), "synchronous API"); }
        if (module) { const CUresult r = cuda.module_unload(module); trace("cuModuleUnload", int(r), "synchronous API"); }
        if (stream) { const CUresult r = cuda.stream_destroy(stream); trace("cuStreamDestroy", int(r), "may defer pending work"); }
        cuda_semaphore = nullptr;
        module = nullptr;
        stream = nullptr;
        kernel = nullptr;
        if (cuda_context) { const CUresult r = cuda.ctx_destroy(cuda_context); trace("cuCtxDestroy", int(r), "destroys context resources"); }
        cuda_context = nullptr;
        if (vulkan && vulkan->device() != VK_NULL_HANDLE) {
            if (command_pool) { vkDestroyCommandPool(vulkan->device(), command_pool, nullptr); trace("vkDestroyCommandPool"); }
            if (vk_semaphore) { vkDestroySemaphore(vulkan->device(), vk_semaphore, nullptr); trace("vkDestroySemaphore"); }
        }
        command_pool = VK_NULL_HANDLE;
        vk_semaphore = VK_NULL_HANDLE;
    }

    bool select_cuda_device(std::string& error) {
        VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &ids;
        vkGetPhysicalDeviceProperties2(vulkan->physical_device(), &properties);
        vk_name = properties.properties.deviceName;
        std::copy(ids.deviceUUID, ids.deviceUUID + VK_UUID_SIZE, vk_uuid.begin());

        if (!cuda.ok(cuda.init(0), "cuInit", error)) return false;
        int count = 0;
        if (!cuda.ok(cuda.device_get_count(&count), "cuDeviceGetCount", error)) return false;
        std::vector<std::string> reported;
        for (int ordinal = 0; ordinal < count; ++ordinal) {
            CUdevice candidate;
            CUuuid uuid{};
            char name[256]{};
            if (cuda.device_get(&candidate, ordinal) != CUDA_SUCCESS ||
                cuda.device_get_uuid(&uuid, candidate) != CUDA_SUCCESS ||
                cuda.device_get_name(name, sizeof(name), candidate) != CUDA_SUCCESS)
                continue;
            std::array<uint8_t, VK_UUID_SIZE> candidate_uuid{};
            std::memcpy(candidate_uuid.data(), uuid.bytes, VK_UUID_SIZE);
            char cuda_luid[VK_LUID_SIZE]{};
            unsigned node_mask = 0;
            const bool cuda_luid_valid =
                cuda.device_get_luid(cuda_luid, &node_mask, candidate) == CUDA_SUCCESS;
            const bool luid_matches = !ids.deviceLUIDValid || !cuda_luid_valid ||
                std::memcmp(ids.deviceLUID, cuda_luid, VK_LUID_SIZE) == 0;
            reported.emplace_back(std::string(name) + " uuid=" +
                                  hex_id(candidate_uuid.data(), candidate_uuid.size()) +
                                  (cuda_luid_valid ? " luid=" + hex_id(
                                      reinterpret_cast<uint8_t*>(cuda_luid), VK_LUID_SIZE) : ""));
            if (cuda_vulkan_device_ids_match_for_test(vk_uuid, candidate_uuid) && luid_matches) {
                cuda_device = candidate;
                cu_uuid = candidate_uuid;
                cu_name = name;
                return true;
            }
        }
        error = "Vulkan/CUDA adapter mismatch: Vulkan " + vk_name + " uuid=" +
                hex_id(vk_uuid.data(), vk_uuid.size()) +
                (ids.deviceLUIDValid ? " luid=" + hex_id(ids.deviceLUID, VK_LUID_SIZE) : "") +
                "; CUDA devices=";
        for (const auto& item : reported) error += " [" + item + "]";
        return false;
    }

    bool create_sync(std::string& error) {
        VkExportSemaphoreCreateInfo export_info{
            VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timeline.pNext = &export_info;
        timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timeline.initialValue = 0;
        VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        create.pNext = &timeline;
        if (!vk_result(vkCreateSemaphore(vulkan->device(), &create, nullptr, &vk_semaphore),
                       "vkCreateSemaphore(external timeline)", error)) return false;
        trace("vkCreateSemaphore");
        const auto get_handle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(vulkan->device(), "vkGetSemaphoreWin32HandleKHR"));
        if (!get_handle) { error = "missing vkGetSemaphoreWin32HandleKHR"; return false; }
        VkSemaphoreGetWin32HandleInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
        get.semaphore = vk_semaphore;
        get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        HANDLE handle = nullptr;
        if (!vk_result(get_handle(vulkan->device(), &get, &handle),
                       "vkGetSemaphoreWin32HandleKHR", error)) return false;
        trace("vkGetSemaphoreWin32HandleKHR");
        CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC desc{};
        desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_WIN32;
        desc.handle.win32.handle = handle;
        if (!cuda.ok(cuda.import_semaphore(&cuda_semaphore, &desc),
                     "cuImportExternalSemaphore", error)) {
            CloseHandle(handle);
            return false;
        }
        trace("cuImportExternalSemaphore", 0,
              "synchronous import; async wait/signal later");
        if (!CloseHandle(handle)) {
            error = "CloseHandle(exported semaphore) failed: " +
                    std::to_string(GetLastError());
            return false;
        }
        trace("CloseHandle(semaphore export)");
        return true;
    }

    bool init(std::string& error) {
        if (!cuda.open(error)) return false;
        trace("LoadLibrary(nvcuda.dll)");
        if (!select_cuda_device(error)) return false;
        trace("cuInit + adapter selection");
        if (!cuda.ok(cuda.ctx_create(&cuda_context, nullptr,
                                    CU_CTX_SCHED_AUTO, cuda_device),
                     "cuCtxCreate", error) ||
            !cuda.ok(cuda.ctx_set_current(cuda_context), "cuCtxSetCurrent", error) ||
            !cuda.ok(cuda.stream_create(&stream, CU_STREAM_NON_BLOCKING),
                     "cuStreamCreate", error) ||
            !cuda.ok(cuda.module_load_data(&module, matter_cuda_embedded::invert_ptx),
                     "cuModuleLoadData(vk_cuda_invert)", error) ||
            !cuda.ok(cuda.module_get_function(&kernel, module, "vk_cuda_invert"),
                     "cuModuleGetFunction(vk_cuda_invert)", error)) return false;
        trace("cuCtxCreate + stream + module");
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                     VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = vulkan->graphics_queue_family();
        if (!vk_result(vkCreateCommandPool(vulkan->device(), &pool, nullptr,
                                           &command_pool),
                       "vkCreateCommandPool(interop)", error)) return false;
        trace("vkCreateCommandPool");
        if (!create_sync(error)) return false;
        std::printf("CUDA adapter: %s | uuid=%s\n", cu_name.c_str(),
                    hex_id(cu_uuid.data(), cu_uuid.size()).c_str());
        return true;
    }

    bool run(VkExtent2D extent, uint64_t serial, CudaVulkanInteropPixel& pixel,
             std::string& error) {
        if (!extent.width || !extent.height) { error = "interop extent must be nonzero"; return false; }
        if (serial == 0 || serial <= last_serial || serial > UINT64_MAX / 2) {
            error = "interop frame serial must be strictly increasing and fit timeline values";
            return false;
        }
        if (!cuda.ok(cuda.ctx_set_current(cuda_context), "cuCtxSetCurrent", error)) return false;
        Cycle* selected = nullptr;
        for (const auto& entry : size_classes) {
            if (entry->extent.width == extent.width &&
                entry->extent.height == extent.height) {
                selected = entry.get();
                break;
            }
        }
        const bool create_resources = selected == nullptr;
        if (!selected) {
            // FIFO eviction bounds resize resources; dimensions already in the
            // cache reuse their imported image and submission objects.
            if (size_classes.size() == kMaxSizeClasses)
                size_classes.erase(size_classes.begin());
            auto entry = std::unique_ptr<Cycle>(new Cycle{*this});
            entry->extent = extent;
            selected = entry.get();
            size_classes.push_back(std::move(entry));
        }
        Cycle& cycle = *selected;
        struct FailedCycleGuard {
            Impl& owner;
            Cycle* cycle;
            bool armed = true;
            ~FailedCycleGuard() {
                if (!armed) return;
                const auto found = std::find_if(
                    owner.size_classes.begin(), owner.size_classes.end(),
                    [this](const std::unique_ptr<Cycle>& entry) {
                        return entry.get() == cycle;
                    });
                if (found != owner.size_classes.end()) owner.size_classes.erase(found);
            }
        } failed_cycle{*this, &cycle};
        const VkDeviceSize byte_size = VkDeviceSize(extent.width) * extent.height * 8;

        if (create_resources) {
        VkPhysicalDeviceExternalImageFormatInfo external_format{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        external_format.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkPhysicalDeviceImageFormatInfo2 format_info{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        format_info.pNext = &external_format;
        format_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        format_info.type = VK_IMAGE_TYPE_2D;
        format_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        format_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkExternalImageFormatProperties external_properties{
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 format_properties{
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        format_properties.pNext = &external_properties;
        if (!vk_result(vkGetPhysicalDeviceImageFormatProperties2(
                           vulkan->physical_device(), &format_info, &format_properties),
                       "vkGetPhysicalDeviceImageFormatProperties2(external RGBA16F)", error)) return false;
        const auto features = external_properties.externalMemoryProperties.externalMemoryFeatures;
        if ((features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0) {
            error = "RGBA16F opaque Win32 image memory is not exportable";
            return false;
        }

        VkExternalMemoryImageCreateInfo external_image{
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.pNext = &external_image;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        image_info.extent = {extent.width, extent.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = format_info.usage;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!vk_result(vkCreateImage(vulkan->device(), &image_info, nullptr, &cycle.image),
                       "vkCreateImage(external RGBA16F)", error)) return false;
        trace("vkCreateImage(external)");

        VkMemoryDedicatedRequirements dedicated_requirements{
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        requirements.pNext = &dedicated_requirements;
        VkImageMemoryRequirementsInfo2 requirements_info{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        requirements_info.image = cycle.image;
        vkGetImageMemoryRequirements2(vulkan->device(), &requirements_info, &requirements);
        const uint32_t type = memory_type(vulkan->physical_device(),
                                          requirements.memoryRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) { error = "no device-local memory type for external image"; return false; }
        VkExportMemoryAllocateInfo export_memory{
            VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        export_memory.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.pNext = &export_memory;
        dedicated.image = cycle.image;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &dedicated;
        allocate.allocationSize = requirements.memoryRequirements.size;
        allocate.memoryTypeIndex = type;
        if (!vk_result(vkAllocateMemory(vulkan->device(), &allocate, nullptr, &cycle.memory),
                       "vkAllocateMemory(external dedicated image)", error) ||
            !vk_result(vkBindImageMemory(vulkan->device(), cycle.image, cycle.memory, 0),
                       "vkBindImageMemory(external)", error)) return false;
        trace("vkAllocate+BindMemory(external)");

        const auto get_memory_handle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
            vkGetDeviceProcAddr(vulkan->device(), "vkGetMemoryWin32HandleKHR"));
        if (!get_memory_handle) { error = "missing vkGetMemoryWin32HandleKHR"; return false; }
        VkMemoryGetWin32HandleInfoKHR get_memory{
            VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
        get_memory.memory = cycle.memory;
        get_memory.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        HANDLE memory_handle = nullptr;
        if (!vk_result(get_memory_handle(vulkan->device(), &get_memory, &memory_handle),
                       "vkGetMemoryWin32HandleKHR", error)) return false;
        trace("vkGetMemoryWin32HandleKHR");
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC memory_desc{};
        memory_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
        memory_desc.handle.win32.handle = memory_handle;
        memory_desc.size = requirements.memoryRequirements.size;
        memory_desc.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
        if (!cuda.ok(cuda.import_memory(&cycle.external_memory, &memory_desc),
                     "cuImportExternalMemory", error)) {
            CloseHandle(memory_handle);
            return false;
        }
        trace("cuImportExternalMemory", 0, "synchronous import");
        if (!CloseHandle(memory_handle)) {
            error = "CloseHandle(exported image memory) failed: " +
                    std::to_string(GetLastError());
            return false;
        }
        trace("CloseHandle(memory export)");

        CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC map_desc{};
        map_desc.arrayDesc.Width = extent.width;
        map_desc.arrayDesc.Height = extent.height;
        map_desc.arrayDesc.Depth = 0;
        map_desc.arrayDesc.Format = CU_AD_FORMAT_HALF;
        map_desc.arrayDesc.NumChannels = 4;
        map_desc.arrayDesc.Flags = CUDA_ARRAY3D_SURFACE_LDST;
        map_desc.numLevels = 1;
        if (!cuda.ok(cuda.map_mipmapped(&cycle.mipmapped, cycle.external_memory,
                                        &map_desc),
                     "cuExternalMemoryGetMappedMipmappedArray", error)) return false;
        trace("cuExternalMemoryMapMipmapped", 0, "synchronous API");
        CUarray level = nullptr;
        if (!cuda.ok(cuda.mip_level(&level, cycle.mipmapped, 0),
                     "cuMipmappedArrayGetLevel", error)) return false;
        CUDA_RESOURCE_DESC resource{};
        resource.resType = CU_RESOURCE_TYPE_ARRAY;
        resource.res.array.hArray = level;
        if (!cuda.ok(cuda.surface_create(&cycle.surface, &resource),
                     "cuSurfObjectCreate", error)) return false;
        trace("cuSurfObjectCreate", 0, "synchronous API");

        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = byte_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!vk_result(vkCreateBuffer(vulkan->device(), &buffer_info, nullptr,
                                      &cycle.readback),
                       "vkCreateBuffer(interop readback)", error)) return false;
        VkMemoryRequirements buffer_requirements{};
        vkGetBufferMemoryRequirements(vulkan->device(), cycle.readback, &buffer_requirements);
        const uint32_t host_type = memory_type(vulkan->physical_device(),
            buffer_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (host_type == UINT32_MAX) { error = "no coherent readback memory"; return false; }
        VkMemoryAllocateInfo readback_allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        readback_allocate.allocationSize = buffer_requirements.size;
        readback_allocate.memoryTypeIndex = host_type;
        if (!vk_result(vkAllocateMemory(vulkan->device(), &readback_allocate, nullptr,
                                        &cycle.readback_memory),
                       "vkAllocateMemory(interop readback)", error) ||
            !vk_result(vkBindBufferMemory(vulkan->device(), cycle.readback,
                                          cycle.readback_memory, 0),
                       "vkBindBufferMemory(interop readback)", error)) return false;

        VkCommandBufferAllocateInfo command_allocate{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_allocate.commandPool = command_pool;
        command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocate.commandBufferCount = 2;
        if (!vk_result(vkAllocateCommandBuffers(vulkan->device(), &command_allocate,
                                                cycle.commands),
                       "vkAllocateCommandBuffers(interop)", error)) return false;
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!vk_result(vkCreateFence(vulkan->device(), &fence_info, nullptr, &cycle.fence),
                       "vkCreateFence(interop readback)", error)) return false;
        } else {
            if (!vk_result(vkResetFences(vulkan->device(), 1, &cycle.fence),
                           "vkResetFences(interop)", error) ||
                !vk_result(vkResetCommandBuffer(cycle.commands[0], 0),
                           "vkResetCommandBuffer(clear)", error) ||
                !vk_result(vkResetCommandBuffer(cycle.commands[1], 0),
                           "vkResetCommandBuffer(readback)", error)) return false;
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!vk_result(vkBeginCommandBuffer(cycle.commands[0], &begin),
                       "vkBeginCommandBuffer(clear)", error)) return false;
        VkImageMemoryBarrier2 to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_transfer.srcStageMask = cycle.initialized_layout
                                       ? VK_PIPELINE_STAGE_2_COPY_BIT
                                       : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        to_transfer.srcAccessMask = cycle.initialized_layout
                                        ? VK_ACCESS_2_TRANSFER_READ_BIT
                                        : 0;
        to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = cycle.initialized_layout
                                    ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                    : VK_IMAGE_LAYOUT_UNDEFINED;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.image = cycle.image;
        to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &to_transfer;
        vkCmdPipelineBarrier2(cycle.commands[0], &dependency);
        const VkClearColorValue red{{1.0f, 0.0f, 0.0f, 1.0f}};
        vkCmdClearColorImage(cycle.commands[0], cycle.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1,
                             &to_transfer.subresourceRange);
        VkImageMemoryBarrier2 to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_general.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        to_general.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_general.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        to_general.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general.srcQueueFamilyIndex = vulkan->graphics_queue_family();
        to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        to_general.image = cycle.image;
        to_general.subresourceRange = to_transfer.subresourceRange;
        dependency.pImageMemoryBarriers = &to_general;
        vkCmdPipelineBarrier2(cycle.commands[0], &dependency);
        if (!vk_result(vkEndCommandBuffer(cycle.commands[0]),
                       "vkEndCommandBuffer(clear)", error)) return false;

        const uint64_t red_value = serial * 2 - 1;
        const uint64_t cyan_value = serial * 2;
        last_serial = serial;
        VkCommandBufferSubmitInfo clear_command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        clear_command.commandBuffer = cycle.commands[0];
        VkSemaphoreSubmitInfo signal_red{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal_red.semaphore = vk_semaphore;
        signal_red.value = red_value;
        signal_red.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 clear_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        clear_submit.commandBufferInfoCount = 1;
        clear_submit.pCommandBufferInfos = &clear_command;
        clear_submit.signalSemaphoreInfoCount = 1;
        clear_submit.pSignalSemaphoreInfos = &signal_red;
        if (!vk_result(vkQueueSubmit2(vulkan->graphics_queue(), 1, &clear_submit,
                                      VK_NULL_HANDLE),
                       "vkQueueSubmit2(clear/signals red)", error)) return false;
        cycle.pending_timeline = red_value;
        trace("vkQueueSubmit2(clear)", 0, "asynchronous");

        CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wait{};
        wait.params.fence.value = red_value;
        if (!cuda.ok(cuda.wait_semaphore(&cuda_semaphore, &wait, 1, stream),
                     "cuWaitExternalSemaphoresAsync(red)", error)) return false;
        void* arguments[] = {&cycle.surface, &extent.width, &extent.height};
        if (!cuda.ok(cuda.launch_kernel(kernel,
                                        (extent.width + 7) / 8,
                                        (extent.height + 7) / 8, 1,
                                        8, 8, 1, 0, stream, arguments, nullptr),
                     "cuLaunchKernel(vk_cuda_invert)", error)) return false;
        CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signal{};
        signal.params.fence.value = cyan_value;
        if (!cuda.ok(cuda.signal_semaphore(&cuda_semaphore, &signal, 1, stream),
                     "cuSignalExternalSemaphoresAsync(cyan)", error)) return false;
        cycle.cuda_work_pending = true;
        cycle.pending_timeline = cyan_value;
        trace("CUDA wait+kernel+signal queued", 0, "asynchronous");

        if (!vk_result(vkBeginCommandBuffer(cycle.commands[1], &begin),
                       "vkBeginCommandBuffer(readback)", error)) return false;
        VkImageMemoryBarrier2 to_source{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_source.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        to_source.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        to_source.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_source.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_source.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        to_source.dstQueueFamilyIndex = vulkan->graphics_queue_family();
        to_source.image = cycle.image;
        to_source.subresourceRange = to_transfer.subresourceRange;
        dependency.pImageMemoryBarriers = &to_source;
        vkCmdPipelineBarrier2(cycle.commands[1], &dependency);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cycle.commands[1], cycle.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               cycle.readback, 1, &copy);
        VkMemoryBarrier2 host_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo host_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        host_dependency.memoryBarrierCount = 1;
        host_dependency.pMemoryBarriers = &host_barrier;
        vkCmdPipelineBarrier2(cycle.commands[1], &host_dependency);
        if (!vk_result(vkEndCommandBuffer(cycle.commands[1]),
                       "vkEndCommandBuffer(readback)", error)) return false;
        VkSemaphoreSubmitInfo wait_cyan{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait_cyan.semaphore = vk_semaphore;
        wait_cyan.value = cyan_value;
        wait_cyan.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo read_command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        read_command.commandBuffer = cycle.commands[1];
        VkSubmitInfo2 read_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        read_submit.waitSemaphoreInfoCount = 1;
        read_submit.pWaitSemaphoreInfos = &wait_cyan;
        read_submit.commandBufferInfoCount = 1;
        read_submit.pCommandBufferInfos = &read_command;
        if (!vk_result(vkQueueSubmit2(vulkan->graphics_queue(), 1, &read_submit,
                                      cycle.fence),
                       "vkQueueSubmit2(wait cyan/readback)", error) ||
            !vk_result(vkWaitForFences(vulkan->device(), 1, &cycle.fence, VK_TRUE,
                                       UINT64_MAX),
                       "vkWaitForFences(final interop readback)", error)) return false;
        cycle.cuda_work_pending = false;
        cycle.pending_timeline = 0;
        cycle.initialized_layout = true;
        trace("vkWaitForFences(final)", 0, "blocking completion proof");

        void* mapped = nullptr;
        if (!vk_result(vkMapMemory(vulkan->device(), cycle.readback_memory, 0,
                                   byte_size, 0, &mapped),
                       "vkMapMemory(interop readback)", error)) return false;
        const size_t center = (size_t(extent.height / 2) * extent.width +
                               extent.width / 2) * 4;
        const auto* half = static_cast<const uint16_t*>(mapped);
        pixel = {half_to_float(half[center]), half_to_float(half[center + 1]),
                 half_to_float(half[center + 2]), half_to_float(half[center + 3])};
        vkUnmapMemory(vulkan->device(), cycle.readback_memory);
        failed_cycle.armed = false;
        return true;
    }
};

CudaVulkanInterop::CudaVulkanInterop() : impl_(std::make_unique<Impl>()) {}
CudaVulkanInterop::~CudaVulkanInterop() = default;

std::unique_ptr<CudaVulkanInterop> CudaVulkanInterop::create(
    VulkanDevice& vulkan, std::string& error) {
    auto result = std::unique_ptr<CudaVulkanInterop>(new CudaVulkanInterop());
    result->impl_->vulkan = &vulkan;
    if (!result->impl_->init(error)) return nullptr;
    return result;
}

bool CudaVulkanInterop::round_trip(VkExtent2D extent, uint64_t frame_serial,
                                   CudaVulkanInteropPixel& pixel,
                                   std::string& error) {
    error.clear();
    return impl_->run(extent, frame_serial, pixel, error);
}

const std::array<uint8_t, VK_UUID_SIZE>& CudaVulkanInterop::vulkan_uuid() const noexcept {
    return impl_->vk_uuid;
}
const std::array<uint8_t, VK_UUID_SIZE>& CudaVulkanInterop::cuda_uuid() const noexcept {
    return impl_->cu_uuid;
}

}  // namespace matter
