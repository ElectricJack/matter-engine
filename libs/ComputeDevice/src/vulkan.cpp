#include <matter/compute/vulkan.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace matter::compute {
namespace {
data::ValidationResult error(data::ErrorCode code, const std::string &message) {
    return {{{data::Severity::error, code, message, {}, {}, {}}}};
}
struct Failure { data::ValidationResult diagnostic; };
void need(bool value, data::ErrorCode code, const char *message) {
    if (!value) throw Failure{error(code, message)};
}
void check(VkResult r, const char *operation) {
    if (r != VK_SUCCESS) throw Failure{error(data::ErrorCode::backend_failure,
        std::string(operation) + " failed: VkResult " + std::to_string(r))};
}
struct Native {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::atomic<uint32_t> errors{0};
    std::mutex messages_mutex;
    std::vector<std::string> messages;
    ~Native() {
        if (device) vkDestroyDevice(device, nullptr);
        if (messenger) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(instance, messenger, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};
VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        auto &native = *static_cast<Native *>(user);
        if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) native.errors.fetch_add(1);
        try {
            std::lock_guard<std::mutex> guard(native.messages_mutex);
            if (native.messages.size() < 32) native.messages.push_back(
                std::string(type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT ? "VALIDATION: " : "LOADER: ") +
                (data->pMessage ? data->pMessage : "Vulkan error"));
        } catch (...) {} // Nothing may unwind through a Vulkan callback.
    }
    return VK_FALSE;
}
struct Allocation { VkBuffer buffer{}; VkDeviceMemory memory{}; void *mapped = nullptr; };
struct Run {
    VulkanBinding binding;
    std::vector<Allocation> buffers;
    std::vector<VkPipeline> pipelines;
    VkDescriptorSetLayout descriptor_layout{};
    VkDescriptorPool descriptor_pool{};
    VkPipelineLayout pipeline_layout{};
    VkCommandPool command_pool{};
    VkFence fence{};
    bool pending = false;
    explicit Run(VulkanBinding b) : binding(std::move(b)) {}
    ~Run() {
        auto d = binding.device;
        if (fence) vkDestroyFence(d, fence, nullptr);
        if (command_pool) vkDestroyCommandPool(d, command_pool, nullptr);
        for (auto p : pipelines) if (p) vkDestroyPipeline(d, p, nullptr);
        if (pipeline_layout) vkDestroyPipelineLayout(d, pipeline_layout, nullptr);
        if (descriptor_pool) vkDestroyDescriptorPool(d, descriptor_pool, nullptr);
        if (descriptor_layout) vkDestroyDescriptorSetLayout(d, descriptor_layout, nullptr);
        for (auto &b : buffers) {
            if (b.mapped) vkUnmapMemory(d, b.memory);
            if (b.buffer) vkDestroyBuffer(d, b.buffer, nullptr);
            if (b.memory) vkFreeMemory(d, b.memory, nullptr);
        }
    }
};
struct Retire {
    void operator()(Run *r) const noexcept {
        if (r->pending) {
            auto result = vkWaitForFences(r->binding.device, 1, &r->fence, VK_TRUE, UINT64_MAX);
            if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
                result = vkDeviceWaitIdle(r->binding.device);
            // Never free resources possibly in flight. An unprovable driver
            // failure deliberately quarantines the complete run AND device owner.
            if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) return;
        }
        delete r;
    }
};
class VulkanDevice final : public Device {
public:
    VulkanBinding binding;
    std::shared_ptr<Native> native;
    explicit VulkanDevice(VulkanBinding b) : binding(std::move(b)) {}
    Result execute(const jobs::JobEnvelope &job, const Request &request, jobs::JobMonitor &monitor) override {
        try {
            auto valid = jobs::validate(job);
            if (!valid.ok()) return Result::failure(job, std::move(valid.diagnostics));
            valid = monitor.checkpoint();
            if (!valid.ok()) return Result::failure(job, std::move(valid.diagnostics));
            need(request.device == id(), data::ErrorCode::invalid_input, "compute device identity mismatch");
            need(!request.buffers.empty() && request.buffers.size() <= 16 &&
                 !request.dispatches.empty() && request.dispatches.size() <= 16 && request.allocation_limit,
                 data::ErrorCode::resource_limit, "compute request count/allocation limit");
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(binding.physical, &properties);
            const auto &limits = properties.limits;
            need(request.buffers.size() <= limits.maxPerStageDescriptorStorageBuffers &&
                 request.buffers.size() <= limits.maxDescriptorSetStorageBuffers &&
                 limits.maxComputeWorkGroupInvocations >= 256 && limits.maxComputeWorkGroupSize[0] >= 256,
                 data::ErrorCode::unsupported_capability, "device does not support compute kernel requirements");
            uint64_t bytes = 0;
            for (const auto &b : request.buffers) {
                need(b.size && b.size <= limits.maxStorageBufferRange && b.initial.size() <= b.size,
                     data::ErrorCode::resource_limit, "compute storage buffer size");
                need(b.size <= request.allocation_limit - bytes,
                     data::ErrorCode::resource_limit, "compute allocation budget");
                bytes += b.size;
            }
            for (const auto &d : request.dispatches) {
                need(!d.spirv.empty() && d.spirv[0] == 0x07230203u && d.spirv.size() <= 1024 * 1024 &&
                     d.groups_x && d.groups_x <= limits.maxComputeWorkGroupCount[0],
                     data::ErrorCode::invalid_input, "invalid compute program or dispatch size");
            }
            std::unique_lock<std::mutex> lock(*binding.queue_mutex, std::try_to_lock);
            need(lock.owns_lock(), data::ErrorCode::resource_limit, "compute queue busy");
            std::unique_ptr<Run, Retire> run(new Run(binding));
            run->buffers.resize(request.buffers.size());
            VkPhysicalDeviceMemoryProperties memory{};
            vkGetPhysicalDeviceMemoryProperties(binding.physical, &memory);
            Readback output;
            for (size_t i = 0; i < request.buffers.size(); ++i) {
                auto &b = run->buffers[i]; const auto &input = request.buffers[i];
                VkBufferCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                info.size = input.size; info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                check(vkCreateBuffer(binding.device, &info, nullptr, &b.buffer), "create buffer");
                VkMemoryRequirements requirements{};
                vkGetBufferMemoryRequirements(binding.device, b.buffer, &requirements);
                need(requirements.size <= request.allocation_limit - output.allocated_bytes,
                     data::ErrorCode::resource_limit, "aligned device allocation exceeds budget");
                output.allocated_bytes += requirements.size;
                uint32_t type = UINT32_MAX;
                constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                for (uint32_t t = 0; t < memory.memoryTypeCount; ++t)
                    if ((requirements.memoryTypeBits & (1u << t)) && (memory.memoryTypes[t].propertyFlags & flags) == flags) {
                        type = t; break;
                    }
                need(type != UINT32_MAX, data::ErrorCode::unsupported_capability, "coherent compute memory unavailable");
                VkMemoryAllocateInfo allocation{}; allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
                check(vkAllocateMemory(binding.device, &allocation, nullptr, &b.memory), "allocate buffer");
                check(vkBindBufferMemory(binding.device, b.buffer, b.memory, 0), "bind buffer");
                check(vkMapMemory(binding.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "map buffer");
                if (!input.initial.empty()) std::memcpy(b.mapped, input.initial.data(), input.initial.size());
            }
            const auto count = static_cast<uint32_t>(run->buffers.size());
            std::vector<VkDescriptorSetLayoutBinding> bindings(count);
            for (uint32_t i = 0; i < count; ++i) {
                bindings[i].binding = i; bindings[i].descriptorCount = 1;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo layout{}; layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout.bindingCount = count; layout.pBindings = bindings.data();
            check(vkCreateDescriptorSetLayout(binding.device, &layout, nullptr, &run->descriptor_layout), "descriptor layout");
            VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count};
            VkDescriptorPoolCreateInfo pool{}; pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &pool_size;
            check(vkCreateDescriptorPool(binding.device, &pool, nullptr, &run->descriptor_pool), "descriptor pool");
            VkDescriptorSetAllocateInfo set_info{}; set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            set_info.descriptorPool = run->descriptor_pool; set_info.descriptorSetCount = 1;
            set_info.pSetLayouts = &run->descriptor_layout;
            VkDescriptorSet set{};
            check(vkAllocateDescriptorSets(binding.device, &set_info, &set), "descriptor set");
            std::vector<VkDescriptorBufferInfo> buffer_info(count);
            std::vector<VkWriteDescriptorSet> writes(count);
            for (uint32_t i = 0; i < count; ++i) {
                buffer_info[i] = {run->buffers[i].buffer, 0, request.buffers[i].size};
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &buffer_info[i];
            }
            vkUpdateDescriptorSets(binding.device, count, writes.data(), 0, nullptr);
            VkPipelineLayoutCreateInfo pipeline_layout{}; pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipeline_layout.setLayoutCount = 1; pipeline_layout.pSetLayouts = &run->descriptor_layout;
            check(vkCreatePipelineLayout(binding.device, &pipeline_layout, nullptr, &run->pipeline_layout), "pipeline layout");
            run->pipelines.resize(request.dispatches.size());
            for (size_t i = 0; i < request.dispatches.size(); ++i) {
                const auto &words = request.dispatches[i].spirv;
                VkShaderModuleCreateInfo shader_info{}; shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                shader_info.codeSize = words.size() * sizeof(uint32_t); shader_info.pCode = words.data();
                VkShaderModule shader{};
                check(vkCreateShaderModule(binding.device, &shader_info, nullptr, &shader), "shader module");
                VkComputePipelineCreateInfo pipeline{}; pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = shader; pipeline.stage.pName = "main";
                pipeline.layout = run->pipeline_layout;
                auto status = vkCreateComputePipelines(binding.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &run->pipelines[i]);
                vkDestroyShaderModule(binding.device, shader, nullptr); check(status, "compute pipeline");
            }
            VkCommandPoolCreateInfo command_pool{}; command_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            command_pool.queueFamilyIndex = binding.queue_family;
            check(vkCreateCommandPool(binding.device, &command_pool, nullptr, &run->command_pool), "command pool");
            VkCommandBufferAllocateInfo command_info{}; command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            command_info.commandPool = run->command_pool; command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; command_info.commandBufferCount = 1;
            VkCommandBuffer command{};
            check(vkAllocateCommandBuffers(binding.device, &command_info, &command), "command buffer");
            VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(command, &begin), "begin compute");
            auto barrier = [&](VkPipelineStageFlags src, VkAccessFlags from, VkPipelineStageFlags dst, VkAccessFlags to) {
                VkMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; b.srcAccessMask = from; b.dstAccessMask = to;
                vkCmdPipelineBarrier(command, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
            };
            barrier(VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            for (size_t i = 0; i < request.dispatches.size(); ++i) {
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, run->pipelines[i]);
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, run->pipeline_layout, 0, 1, &set, 0, nullptr);
                vkCmdDispatch(command, request.dispatches[i].groups_x, 1, 1);
                barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            }
            barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            check(vkEndCommandBuffer(command), "end compute");
            VkFenceCreateInfo fence{}; fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            check(vkCreateFence(binding.device, &fence, nullptr, &run->fence), "compute fence");
            valid = monitor.checkpoint();
            if (!valid.ok()) return Result::failure(job, std::move(valid.diagnostics));
            VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            check(vkQueueSubmit(binding.queue, 1, &submit, run->fence), "compute submit");
            run->pending = true;
            // Bounded kernels cannot be preempted portably. Even after a deadline
            // or cancellation, drain the fence before returning failure.
            check(vkWaitForFences(binding.device, 1, &run->fence, VK_TRUE, UINT64_MAX), "compute wait");
            run->pending = false;
            valid = monitor.checkpoint();
            if (!valid.ok()) return Result::failure(job, std::move(valid.diagnostics));
            output.buffers.resize(count);
            for (uint32_t i = 0; i < count; ++i) if (request.buffers[i].readback) {
                output.buffers[i].resize(static_cast<size_t>(request.buffers[i].size));
                std::memcpy(output.buffers[i].data(), run->buffers[i].mapped, output.buffers[i].size());
            }
            valid = monitor.checkpoint();
            if (!valid.ok()) return Result::failure(job, std::move(valid.diagnostics));
            return Result::success(job, std::move(output));
        } catch (const Failure &f) { return Result::failure(job, f.diagnostic.diagnostics); }
        catch (const std::bad_alloc &) { return Result::failure(job, data::ErrorCode::resource_limit, "compute allocation failed"); }
        catch (const std::exception &e) { return Result::failure(job, data::ErrorCode::backend_failure, e.what()); }
        catch (...) { return Result::failure(job, data::ErrorCode::backend_failure, "compute backend exception"); }
    }
};
}
std::shared_ptr<Device> bind_vulkan(VulkanBinding binding, data::ValidationResult &result) {
    result = {};
    if (!binding.physical || !binding.device || !binding.queue || !binding.owner || !binding.queue_mutex) {
        result = error(data::ErrorCode::invalid_input, "compute binding requires handles, owner and shared queue mutex"); return {};
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(binding.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(binding.physical, &count, families.data());
    if (binding.queue_family >= count || !(families[binding.queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        result = error(data::ErrorCode::unsupported_capability, "compute queue family required"); return {};
    }
    return std::make_shared<VulkanDevice>(std::move(binding));
}
std::shared_ptr<Device> create_vulkan(bool validation, data::ValidationResult &result) {
    result = {};
    try {
        auto owner = std::make_shared<Native>();
        const char *layer = "VK_LAYER_KHRONOS_validation";
        const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        if (validation) {
            uint32_t count = 0; check(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate layers");
            std::vector<VkLayerProperties> layers(count);
            check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "read layers");
            need(std::any_of(layers.begin(), layers.end(), [&](const auto &l) { return std::strcmp(l.layerName, layer) == 0; }),
                 data::ErrorCode::unsupported_capability, "required Vulkan validation layer unavailable");
        }
        VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion = VK_API_VERSION_1_2;
        VkDebugUtilsMessengerCreateInfoEXT debug{}; debug.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        debug.pfnUserCallback = message; debug.pUserData = owner.get();
        VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT features{}; features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        features.enabledValidationFeatureCount = 1; features.pEnabledValidationFeatures = &sync; features.pNext = &debug;
        VkInstanceCreateInfo instance{}; instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; instance.pApplicationInfo = &app;
        if (validation) {
            instance.enabledLayerCount = 1; instance.ppEnabledLayerNames = &layer;
            instance.enabledExtensionCount = 2; instance.ppEnabledExtensionNames = extensions; instance.pNext = &features;
        }
        check(vkCreateInstance(&instance, nullptr, &owner->instance), "create compute instance");
        if (validation) {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(owner->instance, "vkCreateDebugUtilsMessengerEXT"));
            need(create != nullptr, data::ErrorCode::unsupported_capability, "validation messenger unavailable");
            check(create(owner->instance, &debug, nullptr, &owner->messenger), "create validation messenger");
        }
        uint32_t count = 0; check(vkEnumeratePhysicalDevices(owner->instance, &count, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(owner->instance, &count, devices.data()), "read devices");
        VulkanBinding binding;
        for (auto physical : devices) {
            VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_2) continue;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
            for (uint32_t i = 0; i < count; ++i) if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                binding.physical = physical; binding.queue_family = i; break;
            }
            if (binding.physical) break;
        }
        need(binding.physical != VK_NULL_HANDLE, data::ErrorCode::unsupported_capability, "Vulkan 1.2 compute device unavailable");
        float priority = 1;
        VkDeviceQueueCreateInfo queue{}; queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = binding.queue_family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo device{}; device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
        check(vkCreateDevice(binding.physical, &device, nullptr, &owner->device), "create compute device");
        binding.device = owner->device;
        vkGetDeviceQueue(owner->device, binding.queue_family, 0, &binding.queue);
        binding.owner = owner; binding.queue_mutex = std::make_shared<std::mutex>();
        auto out = std::make_shared<VulkanDevice>(std::move(binding)); out->native = owner;
        return out;
    } catch (const Failure &f) { result = f.diagnostic; }
    catch (const std::bad_alloc &) { result = error(data::ErrorCode::resource_limit, "compute device allocation failed"); }
    catch (const std::exception &e) { result = error(data::ErrorCode::backend_failure, e.what()); }
    return {};
}
uint32_t validation_errors(const std::shared_ptr<Device> &device) noexcept {
    auto vk = std::dynamic_pointer_cast<VulkanDevice>(device);
    return vk && vk->native ? vk->native->errors.load() : 0;
}
std::vector<std::string> validation_messages(const std::shared_ptr<Device> &device) {
    auto vk = std::dynamic_pointer_cast<VulkanDevice>(device);
    if (!vk || !vk->native) return {};
    std::lock_guard<std::mutex> guard(vk->native->messages_mutex);
    return vk->native->messages;
}
} // namespace matter::compute
