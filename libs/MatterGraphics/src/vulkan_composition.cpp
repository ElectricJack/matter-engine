#include <matter/graphics/vulkan_composition.h>
#include "composition_spirv.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>

// The descriptor ABI, fullscreen pipelines and push packing come from
// vk_scene_renderer.cpp's composite/display passes. The shaders are compiled
// directly from their source of truth. Device/world globals are replaced by
// explicit inputs and resources retained by VulkanResourceDevice submissions.
namespace matter::graphics {
namespace {
using E = data::ErrorCode;
struct Failure { data::ValidationResult result; };
void require(bool ok, E code, const char *message) {
  if (!ok) throw Failure{{{{data::Severity::error, code, message, {}, {}, {}}}}};
}
void vkcheck(VkResult result, const char *message) {
  if (result != VK_SUCCESS) throw Failure{{{{data::Severity::error, E::backend_failure,
      std::string(message) + ": VkResult " + std::to_string(result), {}, {}, {}}}}};
}
void checked(data::ValidationResult r) { if (!r.ok()) throw Failure{std::move(r)}; }
data::Vec3 normalized(data::Vec3 v) {
  const double n = std::sqrt(double(v.x)*v.x + double(v.y)*v.y + double(v.z)*v.z);
  return {float(v.x/n), float(v.y/n), float(v.z/n)};
}
Rgba rgba(data::Vec3 v) { return {v.x, v.y, v.z, 0}; }
struct LightingPush {
  float sun[3], intensity = 1, diffuse, emission, debug, forward[3], tan_fov, aspect;
  float jitter[2], volume, vol_debug = 0, near_z, far_z, camera_y, cloud_top, height_layer;
  float sun_angle = 0.53f, cos_edge, cos_core, camera_x, camera_z;
};
static_assert(sizeof(LightingPush) == 104 && offsetof(LightingPush, camera_x) == 96);
static_assert(sizeof(ShadingMaterial) == 144 && offsetof(ShadingMaterial, flags_misc) == 128);
struct EnvironmentGpu {
  float transforms[2][16];
  Rgba cloud_state, cloud_filter, sun, disc, sky, irradiance;
};
static_assert(sizeof(EnvironmentGpu) == 224 && offsetof(EnvironmentGpu, sun) == 160);
struct LocalLightGpu {
  Rgba position_range, direction_outer, color_radius;
  float inner;
  uint32_t kind, flags = 0, reserved = 0;
};
static_assert(sizeof(LocalLightGpu) == 64);

struct Pipelines {
  VulkanDeviceBinding binding;
  VkRenderPass pass = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, 4> sets{};
  std::array<VkPipelineLayout, 2> layouts{};
  std::array<VkPipeline, 2> pipelines{};
  std::array<VkShaderModule, 3> shaders{};
  VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
  explicit Pipelines(VulkanDeviceBinding b) : binding(std::move(b)) {}
  ~Pipelines() {
    const auto d = binding.device;
    for (auto p : pipelines) if (p) vkDestroyPipeline(d, p, nullptr);
    for (auto l : layouts) if (l) vkDestroyPipelineLayout(d, l, nullptr);
    for (auto s : sets) if (s) vkDestroyDescriptorSetLayout(d, s, nullptr);
    for (auto s : shaders) if (s) vkDestroyShaderModule(d, s, nullptr);
    if (nearest) vkDestroySampler(d, nearest, nullptr);
    if (linear) vkDestroySampler(d, linear, nullptr);
    if (pass) vkDestroyRenderPass(d, pass, nullptr);
  }
  void init() {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(binding.physical_device, &properties);
    require(properties.limits.maxPushConstantsSize >= sizeof(LightingPush) &&
            properties.limits.maxPerStageDescriptorSamplers >= 17 &&
            properties.limits.maxPerStageDescriptorSampledImages >= 17 &&
            properties.limits.maxPerStageDescriptorStorageBuffers >= 6 &&
            properties.limits.maxDescriptorSetSamplers >= 17 &&
            properties.limits.maxDescriptorSetSampledImages >= 17 &&
            properties.limits.maxDescriptorSetStorageBuffers >= 6,
            E::unsupported_capability, "Device lacks composition descriptor/push limits");
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(binding.physical_device, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    require((fp.optimalTilingFeatures & needed) == needed, E::unsupported_capability, "RGBA32F composition format unavailable");
    vkGetPhysicalDeviceFormatProperties(binding.physical_device, VK_FORMAT_R32G32_UINT, &fp);
    require((fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) ==
        (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT), E::unsupported_capability, "RG32UI identity format unavailable");
    for (size_t i = 0; i < sets.size(); ++i) {
      const uint32_t count = i == 0 ? 12 : i == 1 ? 7 : i == 2 ? 5 : 1;
      std::vector<VkDescriptorSetLayoutBinding> bindings;
      for (uint32_t j = 0; j < count; ++j) {
        auto type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if (i == 2 || (i == 0 && j == 7)) type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (i == 1 && j == 6) type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings.push_back({j, type, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
      }
      VkDescriptorSetLayoutCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      info.bindingCount = count; info.pBindings = bindings.data();
      vkcheck(vkCreateDescriptorSetLayout(binding.device, &info, nullptr, &sets[i]), "composition descriptor layout");
    }
    for (size_t i = 0; i < layouts.size(); ++i) {
      VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, i == 0 ? uint32_t(sizeof(LightingPush)) : 12u};
      VkPipelineLayoutCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
      info.setLayoutCount = i == 0 ? 3 : 1; info.pSetLayouts = i == 0 ? sets.data() : &sets[3];
      info.pushConstantRangeCount = 1; info.pPushConstantRanges = &push;
      vkcheck(vkCreatePipelineLayout(binding.device, &info, nullptr, &layouts[i]), "composition pipeline layout");
    }
    VkAttachmentDescription color{}; color.format = VK_FORMAT_R32G32B32A32_SFLOAT; color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp{}; rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1; rp.pAttachments = &color; rp.subpassCount = 1; rp.pSubpasses = &subpass;
    vkcheck(vkCreateRenderPass(binding.device, &rp, nullptr, &pass), "composition render pass");
    const uint32_t *code[] = {spirv::composite_vert_spv, spirv::composite_frag_spv, spirv::display_transform_frag_spv};
    const size_t sizes[] = {sizeof(spirv::composite_vert_spv), sizeof(spirv::composite_frag_spv), sizeof(spirv::display_transform_frag_spv)};
    for (size_t i = 0; i < shaders.size(); ++i) {
      VkShaderModuleCreateInfo sm{}; sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      sm.codeSize = sizes[i]; sm.pCode = code[i];
      vkcheck(vkCreateShaderModule(binding.device, &sm, nullptr, &shaders[i]), "composition shader module");
    }
    for (size_t i = 0; i < pipelines.size(); ++i) {
      VkPipelineShaderStageCreateInfo stages[2]{};
      for (size_t j = 0; j < 2; ++j) {
        stages[j].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[j].stage = j == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[j].module = j == 0 ? shaders[0] : shaders[i + 1]; stages[j].pName = "main";
      }
      VkPipelineVertexInputStateCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
      VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
      ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkPipelineViewportStateCreateInfo vp{}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
      vp.viewportCount = 1; vp.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
      rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1;
      VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
      ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask = 15;
      VkPipelineColorBlendStateCreateInfo blend{}; blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
      blend.attachmentCount = 1; blend.pAttachments = &attachment;
      VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo ds{}; ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
      ds.dynamicStateCount = 2; ds.pDynamicStates = dynamic;
      VkGraphicsPipelineCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
      pi.stageCount = 2; pi.pStages = stages; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia;
      pi.pViewportState = &vp; pi.pRasterizationState = &rs; pi.pMultisampleState = &ms;
      pi.pColorBlendState = &blend; pi.pDynamicState = &ds; pi.layout = layouts[i]; pi.renderPass = pass;
      vkcheck(vkCreateGraphicsPipelines(binding.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipelines[i]), "composition graphics pipeline");
    }
    VkSamplerCreateInfo sampler{}; sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkcheck(vkCreateSampler(binding.device, &sampler, nullptr, &nearest), "composition nearest sampler");
    sampler.minFilter = sampler.magFilter = VK_FILTER_LINEAR;
    vkcheck(vkCreateSampler(binding.device, &sampler, nullptr, &linear), "composition linear sampler");
  }
};
struct Image {
  VulkanDeviceBinding binding;
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkExtent3D extent{};
  std::shared_ptr<DeviceBuffer> upload;
  explicit Image(VulkanDeviceBinding b) : binding(std::move(b)) {}
  ~Image() {
    if (view) vkDestroyImageView(binding.device, view, nullptr);
    if (image) vkDestroyImage(binding.device, image, nullptr);
    if (memory) vkFreeMemory(binding.device, memory, nullptr);
  }
};
struct Frame {
  std::shared_ptr<Pipelines> state;
  std::vector<std::shared_ptr<Image>> images;
  std::vector<std::shared_ptr<DeviceBuffer>> buffers;
  uint64_t resident_bytes = 0;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 4> sets{};
  std::array<VkFramebuffer, 2> framebuffers{};
  ~Frame() {
    for (auto f : framebuffers) if (f) vkDestroyFramebuffer(state->binding.device, f, nullptr);
    if (pool) vkDestroyDescriptorPool(state->binding.device, pool, nullptr);
  }
};
VkBuffer native(const std::shared_ptr<DeviceBuffer> &b) { return std::static_pointer_cast<VulkanBuffer>(b)->native_buffer(); }
void image_barrier(VkCommandBuffer cmd, const Image &image, VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags before, VkPipelineStageFlags after) {
  VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout = from; b.newLayout = to; b.srcAccessMask = src; b.dstAccessMask = dst;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image.image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, before, after, 0, 0, nullptr, 0, nullptr, 1, &b);
}
} // namespace
struct VulkanComposition::Impl {
  VulkanDeviceBinding binding;
  CompositionLimits limits;
  std::shared_ptr<Pipelines> pipelines;
  VulkanResourceDevice resources;
  explicit Impl(VulkanDeviceBinding b, CompositionLimits l) : binding(b), limits(l), resources(std::move(b)) {}
  uint64_t buffer_allocation_size(size_t size, VkBufferUsageFlags usage) {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkBufferCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size; info.usage = usage;
    vkcheck(vkCreateBuffer(binding.device, &info, nullptr, &buffer), "composition buffer budget probe");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(binding.device, buffer, &requirements);
    vkDestroyBuffer(binding.device, buffer, nullptr);
    return requirements.size;
  }
  std::shared_ptr<DeviceBuffer> buffer(Frame &f, const void *bytes, size_t count, BufferUsage usage = BufferUsage::texture_payload) {
    require(f.resident_bytes <= limits.max_gpu_bytes && count <= limits.max_gpu_bytes-f.resident_bytes,
            E::resource_limit, "Composition buffer budget exceeded");
    // Preflight driver padding before allocating destination or upload staging.
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (usage == BufferUsage::uniform) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    const uint64_t destination = buffer_allocation_size(count, flags);
    const uint64_t staging = buffer_allocation_size(count, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const uint64_t available = limits.max_gpu_bytes - f.resident_bytes;
    require(destination <= available && staging <= available-destination,
            E::resource_limit, "Composition padded buffer/staging budget exceeded");
    auto owner = std::make_shared<std::vector<uint8_t>>(count);
    if (bytes) std::memcpy(owner->data(), bytes, count);
    std::shared_ptr<DeviceBuffer> result;
    checked(resources.upload(usage, owner, result));
    require(result->resident_bytes() <= limits.max_gpu_bytes-f.resident_bytes,
            E::resource_limit, "Composition padded buffer/staging budget exceeded");
    f.resident_bytes += destination + staging;
    f.buffers.push_back(result); return result;
  }
  std::shared_ptr<Image> image(Frame &f, uint32_t w, uint32_t h, uint32_t d, bool volume,
      VkFormat format, const void *bytes, size_t byte_count) {
    auto result = std::make_shared<Image>(binding); result->extent = {w, h, d};
    VkImageCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D; ci.format = format; ci.extent = result->extent;
    ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!bytes) {
      ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(binding.physical_device, &properties);
      require(w <= properties.limits.maxFramebufferWidth && h <= properties.limits.maxFramebufferHeight,
              E::unsupported_capability, "Composition framebuffer extent unavailable on device");
    }
    VkImageFormatProperties format_limits{};
    auto supported = vkGetPhysicalDeviceImageFormatProperties(binding.physical_device, format,
        ci.imageType, ci.tiling, ci.usage, 0, &format_limits);
    require(supported == VK_SUCCESS && w <= format_limits.maxExtent.width &&
        h <= format_limits.maxExtent.height && d <= format_limits.maxExtent.depth,
        E::unsupported_capability, "Composition image format/extent unavailable on device");
    vkcheck(vkCreateImage(binding.device, &ci, nullptr, &result->image), "composition image");
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(binding.device, result->image, &req);
    // Driver padding counts too. Bound all image allocations, not just texels.
    require(f.resident_bytes <= limits.max_gpu_bytes && req.size <= limits.max_gpu_bytes - f.resident_bytes,
            E::resource_limit, "Composition image allocation budget exceeded"); f.resident_bytes += req.size;
    VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(binding.physical_device, &memory);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) if (req.memoryTypeBits & (1u << i)) {
      if (type == UINT32_MAX) type = i;
      if (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { type = i; break; }
    }
    require(type != UINT32_MAX, E::unsupported_capability, "Composition image memory unavailable");
    VkMemoryAllocateInfo alloc{}; alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size; alloc.memoryTypeIndex = type;
    vkcheck(vkAllocateMemory(binding.device, &alloc, nullptr, &result->memory), "composition image memory");
    vkcheck(vkBindImageMemory(binding.device, result->image, result->memory, 0), "composition bind image");
    VkImageViewCreateInfo view{}; view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = result->image; view.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    view.format = format; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkcheck(vkCreateImageView(binding.device, &view, nullptr, &result->view), "composition image view");
    if (bytes) result->upload = buffer(f, bytes, byte_count);
    f.images.push_back(result); return result;
  }
  void bind_image(Frame &f, uint32_t set, uint32_t slot, const Image &im, bool linear) {
    VkDescriptorImageInfo info{linear ? pipelines->linear : pipelines->nearest, im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{}; write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = f.sets[set]; write.dstBinding = slot; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &info;
    vkUpdateDescriptorSets(binding.device, 1, &write, 0, nullptr);
  }
  void bind_buffer(Frame &f, uint32_t set, uint32_t slot, const void *data, size_t size, bool uniform = false) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(binding.physical_device, &properties);
    require(size <= (uniform ? properties.limits.maxUniformBufferRange : properties.limits.maxStorageBufferRange),
            E::resource_limit, "Composition descriptor buffer range exceeds device limit");
    auto b = buffer(f, data, size, uniform ? BufferUsage::uniform : BufferUsage::texture_payload);
    VkDescriptorBufferInfo info{native(b), 0, size};
    VkWriteDescriptorSet write{}; write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = f.sets[set]; write.dstBinding = slot; write.descriptorCount = 1;
    write.descriptorType = uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info; vkUpdateDescriptorSets(binding.device, 1, &write, 0, nullptr);
  }
  void render(const CompositionInputs &in, CompositionOutput &out) {
    if (!pipelines) { auto candidate = std::make_shared<Pipelines>(binding); candidate->init(); pipelines = std::move(candidate); }
    auto frame = std::make_shared<Frame>(); frame->state = pipelines;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 18},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pool{}; pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 4; pool.poolSizeCount = 3; pool.pPoolSizes = sizes;
    vkcheck(vkCreateDescriptorPool(binding.device, &pool, nullptr, &frame->pool), "composition descriptor pool");
    VkDescriptorSetAllocateInfo sets{}; sets.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    sets.descriptorPool = frame->pool; sets.descriptorSetCount = 4; sets.pSetLayouts = pipelines->sets.data();
    vkcheck(vkAllocateDescriptorSets(binding.device, &sets, frame->sets.data()), "composition descriptor sets");
    const FloatImage *gbuffer[] = {&in.albedo, &in.normal, &in.orm, &in.visibility, &in.diffuse, &in.specular,
                                 nullptr, nullptr, &in.transmission, &in.integrated_volume, &in.depth, &in.local_direct};
    for (uint32_t i = 0; i < 12; ++i) if (gbuffer[i]) {
      const auto &src = *gbuffer[i];
      auto im = image(*frame, src.width, src.height, src.depth, i == 9, VK_FORMAT_R32G32B32A32_SFLOAT,
                      src.pixels.data(), src.pixels.size() * sizeof(Rgba));
      bind_image(*frame, 0, i, *im, i == 9);
    }
    const auto &identity = in.identity;
    auto id = image(*frame, identity.width, identity.height, 1, false, VK_FORMAT_R32G32_UINT,
                    identity.pixels.data(), identity.pixels.size() * sizeof(identity.pixels[0]));
    bind_image(*frame, 0, 6, *id, false);
    bind_buffer(*frame, 0, 7, in.materials.data(), in.materials.size() * sizeof(ShadingMaterial));
    const auto &env = in.environment;
    const FloatImage *environment[] = {&env.sky_view, &env.irradiance_sh, &env.cloud_optical_depth[0],
        &env.cloud_optical_depth[1], &env.cloud_optical_depth[2], &env.cloud_optical_depth[3]};
    for (uint32_t i = 0; i < 6; ++i) {
      const auto &src = *environment[i];
      auto im = image(*frame, src.width, src.height, src.depth, i >= 2, VK_FORMAT_R32G32B32A32_SFLOAT,
                      src.pixels.data(), src.pixels.size() * sizeof(Rgba));
      bind_image(*frame, 1, i, *im, i != 1);
    }
    EnvironmentGpu eg{};
    for (size_t i = 0; i < 2; ++i) for (size_t r = 0; r < 4; ++r) for (size_t c = 0; c < 4; ++c)
      eg.transforms[i][c*4+r] = env.cloud_world_to_uvw[i].m[r*4+c];
    eg.cloud_state = env.cloud_state; eg.cloud_filter = env.cloud_filter;
    eg.sun = rgba(env.direct_sun); eg.disc = rgba(env.sun_disc); eg.sky = rgba(env.sky_scale); eg.irradiance = rgba(env.irradiance_scale);
    bind_buffer(*frame, 1, 6, &eg, sizeof(eg), true);
    std::vector<LocalLightGpu> lights;
    std::vector<uint32_t> indices;
    for (const auto &l : in.local_lights) {
      const auto d = normalized(l.direction);
      lights.push_back({{l.position.x, l.position.y, l.position.z, l.range_metres}, {d.x, d.y, d.z, l.cos_outer},
          {l.color.x, l.color.y, l.color.z, l.source_radius_metres}, l.cos_inner, l.kind == services::LightKind::spot ? 1u : 0u});
      indices.push_back(static_cast<uint32_t>(indices.size()));
    }
    if (lights.empty()) lights.push_back({});
    if (indices.empty()) indices.push_back(0);
    // Explicit all-light oversized list: reuse production BRDF/attenuation;
    // spatial/tile acceleration stays a separately tracked integration feature.
    bind_buffer(*frame, 2, 0, lights.data(), lights.size() * sizeof(LocalLightGpu));
    const uint32_t zero[8]{};
    bind_buffer(*frame, 2, 1, zero, sizeof(zero)); bind_buffer(*frame, 2, 2, zero, sizeof(uint32_t));
    bind_buffer(*frame, 2, 3, indices.data(), indices.size() * sizeof(uint32_t));
    std::array<uint32_t, 12> meta{};
    meta[0] = meta[2] = meta[6] = static_cast<uint32_t>(in.local_lights.size());
    meta[3] = in.local_direct_owner == LocalDirectOwner::ray_traced_product ? 1u : 0u;
    bind_buffer(*frame, 2, 4, meta.data(), sizeof(meta));
    const auto w = in.albedo.width, h = in.albedo.height;
    const size_t output_bytes = size_t(w) * h * sizeof(Rgba);
    std::array<std::shared_ptr<Image>, 2> outputs;
    std::array<std::shared_ptr<DeviceBuffer>, 2> readbacks;
    for (size_t i = 0; i < 2; ++i) {
      outputs[i] = image(*frame, w, h, 1, false, VK_FORMAT_R32G32B32A32_SFLOAT, nullptr, 0);
      readbacks[i] = buffer(*frame, nullptr, output_bytes);
      VkFramebufferCreateInfo fb{}; fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      fb.renderPass = pipelines->pass; fb.attachmentCount = 1; fb.pAttachments = &outputs[i]->view;
      fb.width = w; fb.height = h; fb.layers = 1;
      vkcheck(vkCreateFramebuffer(binding.device, &fb, nullptr, &frame->framebuffers[i]), "composition framebuffer");
    }
    bind_image(*frame, 3, 0, *outputs[0], false);
    const auto sun = normalized(in.lighting.sun_direction), forward = normalized(in.camera.forward);
    LightingPush push{};
    push.sun[0] = sun.x; push.sun[1] = sun.y; push.sun[2] = sun.z;
    push.diffuse = in.lighting.diffuse_rt_multiplier; push.emission = in.lighting.emission_multiplier;
    push.debug = float(in.view); push.forward[0] = forward.x; push.forward[1] = forward.y; push.forward[2] = forward.z;
    push.tan_fov = in.camera.tan_half_fov; push.aspect = in.camera.aspect_ratio;
    push.jitter[0] = in.camera.jitter_uv[0]; push.jitter[1] = in.camera.jitter_uv[1];
    push.volume = in.volume_enabled ? 1.0f : 0.0f; push.near_z = in.camera.near_metres; push.far_z = in.camera.far_metres;
    push.camera_y = in.camera.position.y; push.camera_x = in.camera.position.x; push.camera_z = in.camera.position.z;
    push.cloud_top = in.cloud_top_metres; push.height_layer = in.volume_height_layer ? 1.0f : 0.0f;
    push.cos_edge = in.lighting.sun_disc_cos_edge; push.cos_core = in.lighting.sun_disc_cos_core;
    const float display[] = {in.exposure_ev, in.view == CompositionView::shaded ? 0.0f : 1.0f, 0};
    std::shared_ptr<VulkanSubmission> submission;
    checked(resources.submit([&](VkCommandBuffer cmd) {
      for (const auto &im : frame->images) if (im->upload) {
        image_barrier(cmd, *im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = im->extent;
        vkCmdCopyBufferToImage(cmd, native(im->upload), im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        image_barrier(cmd, *im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      }
      for (size_t i = 0; i < 2; ++i) {
        VkRenderPassBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = pipelines->pass; begin.framebuffer = frame->framebuffers[i]; begin.renderArea.extent = {w, h};
        vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0, 0, float(w), float(h), 0, 1}; VkRect2D scissor{{0, 0}, {w, h}};
        vkCmdSetViewport(cmd, 0, 1, &viewport); vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->pipelines[i]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->layouts[i], 0,
                                i == 0 ? 3u : 1u, i == 0 ? frame->sets.data() : &frame->sets[3], 0, nullptr);
        vkCmdPushConstants(cmd, pipelines->layouts[i], VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           i == 0 ? uint32_t(sizeof(push)) : uint32_t(sizeof(display)),
                           i == 0 ? static_cast<const void *>(&push) : static_cast<const void *>(display));
        vkCmdDraw(cmd, 3, 1, 0, 0); vkCmdEndRenderPass(cmd);
        if (i == 0) image_barrier(cmd, *outputs[0], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      }
      for (size_t i = 0; i < 2; ++i) {
        image_barrier(cmd, *outputs[i], i == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, i == 0 ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, i == 0 ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = outputs[i]->extent;
        VkBufferMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = native(readbacks[i]); b.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &b, 0, nullptr);
        vkCmdCopyImageToBuffer(cmd, outputs[i]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, native(readbacks[i]), 1, &copy);
      }
    }, frame->buffers, submission, {frame}));
    checked(submission->wait());
    CompositionOutput result; result.width = w; result.height = h;
    std::vector<Rgba> *destinations[] = {&result.linear_hdr, &result.display_srgb};
    for (size_t i = 0; i < 2; ++i) {
      // readback() owns one transient host-visible transfer buffer at a time.
      require(buffer_allocation_size(output_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT) <=
                  limits.max_gpu_bytes-frame->resident_bytes,
              E::resource_limit, "Composition readback staging budget exceeded");
      std::vector<uint8_t> bytes; checked(resources.readback(readbacks[i], bytes));
      destinations[i]->resize(size_t(w) * h); std::memcpy(destinations[i]->data(), bytes.data(), output_bytes);
      for (size_t pixel = 0; pixel < destinations[i]->size(); ++pixel) for (size_t channel = 0; channel < 4; ++channel) {
        if (std::isfinite((*destinations[i])[pixel][channel])) continue;
        const std::string message = "Nonfinite composition output: attachment=" + std::to_string(i) +
            " x=" + std::to_string(pixel % w) + " y=" + std::to_string(pixel / w) +
            " channel=" + std::to_string(channel);
        require(false, E::backend_failure, message.c_str());
      }
    }
    out = std::move(result);
  }
};
VulkanComposition::VulkanComposition(VulkanDeviceBinding b, CompositionLimits limits)
    : impl_(std::make_unique<Impl>(std::move(b), limits)) {}
VulkanComposition::~VulkanComposition() = default;
data::ValidationResult VulkanComposition::render(const CompositionInputs &in, CompositionOutput &out) {
  auto valid = validate_composition(in, impl_->limits); if (!valid.ok()) return valid;
  try { impl_->render(in, out); return {}; }
  catch (const Failure &f) { impl_->resources.drain(); return f.result; }
  catch (const std::exception &e) { impl_->resources.drain(); return {{{data::Severity::error, E::backend_failure, e.what(), {}, {}, {}}}}; }
  catch (...) { impl_->resources.drain(); return {{{data::Severity::error, E::backend_failure, "Composition backend exception", {}, {}, {}}}}; }
}
} // namespace matter::graphics
