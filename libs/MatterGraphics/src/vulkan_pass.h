#pragma once
#include <algorithm>
#include <cstring>
#include <matter/graphics/vulkan_resources.h>
#include <stdexcept>

// Internal allocation/recording support shared by graphics producer passes.
namespace matter::graphics::pass {
using E = data::ErrorCode;
struct Failure {
  data::ValidationResult result;
};
inline void require(bool ok, E code, const char *message) {
  if (!ok)
    throw Failure{{{{data::Severity::error, code, message, {}, {}, {}}}}};
}
inline void checked(data::ValidationResult r) {
  if (!r.ok())
    throw Failure{std::move(r)};
}
inline void vkcheck(VkResult r, const char *what) {
  if (r != VK_SUCCESS)
    throw Failure{{{{data::Severity::error,
                     E::backend_failure,
                     std::string(what) + ": VkResult " + std::to_string(r),
                     {},
                     {},
                     {}}}}};
}
inline VkBuffer native(const std::shared_ptr<DeviceBuffer> &b) {
  return std::static_pointer_cast<VulkanBuffer>(b)->native_buffer();
}
inline std::array<float, 16> matrix(const data::Mat4 &m) {
  std::array<float, 16> result{};
  for (size_t r = 0; r < 4; ++r)
    for (size_t c = 0; c < 4; ++c)
      result[c * 4 + r] = m.m[r * 4 + c];
  return result;
}
struct Image {
  VulkanDeviceBinding binding;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkExtent3D extent{};
  uint32_t layers = 1, mips = 1, stride = 16;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  std::shared_ptr<DeviceBuffer> upload;
  explicit Image(VulkanDeviceBinding b) : binding(std::move(b)) {}
  ~Image() {
    if (sampler)
      vkDestroySampler(binding.device, sampler, nullptr);
    if (view)
      vkDestroyImageView(binding.device, view, nullptr);
    if (image)
      vkDestroyImage(binding.device, image, nullptr);
    if (memory)
      vkFreeMemory(binding.device, memory, nullptr);
  }
};
struct Frame {
  VulkanDeviceBinding binding;
  std::shared_ptr<void> pipelines;
  std::vector<std::shared_ptr<Image>> images;
  std::vector<std::shared_ptr<DeviceBuffer>> buffers;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> framebuffers;
  uint64_t bytes = 0;
  explicit Frame(VulkanDeviceBinding b) : binding(std::move(b)) {}
  ~Frame() {
    for (auto fb : framebuffers)
      vkDestroyFramebuffer(binding.device, fb, nullptr);
    if (pool)
      vkDestroyDescriptorPool(binding.device, pool, nullptr);
  }
};
inline void barrier(VkCommandBuffer cmd, const Image &im, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags before,
                    VkPipelineStageFlags after) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im.image;
  b.subresourceRange = {im.aspect, 0, im.mips, 0, im.layers};
  vkCmdPipelineBarrier(cmd, before, after, 0, 0, nullptr, 0, nullptr, 1, &b);
}
class Device {
public:
  VulkanDeviceBinding binding;
  VulkanResourceDevice resources;
  uint64_t budget;
  Device(VulkanDeviceBinding b, uint64_t limit) : binding(b), resources(std::move(b)), budget(limit) {}
  uint64_t buffer_size(size_t bytes, VkBufferUsageFlags usage) const {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    vkcheck(vkCreateBuffer(binding.device, &info, nullptr, &buffer), "producer buffer probe");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(binding.device, buffer, &req);
    vkDestroyBuffer(binding.device, buffer, nullptr);
    return req.size;
  }
  void charge(Frame &f, uint64_t bytes) const {
    require(f.bytes <= budget && bytes <= budget - f.bytes, E::resource_limit,
            "Producer GPU budget exceeded");
    f.bytes += bytes;
  }
  std::shared_ptr<DeviceBuffer> buffer(Frame &f, const void *data, size_t size,
                                       BufferUsage usage = BufferUsage::texture_payload) {
    require(size > 0, E::invalid_input, "Empty producer buffer");
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (usage == BufferUsage::vertex)
      flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (usage == BufferUsage::index)
      flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (usage == BufferUsage::uniform)
      flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (usage == BufferUsage::indirect)
      flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    charge(f, buffer_size(size, flags));
    charge(f, buffer_size(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    auto bytes = std::make_shared<std::vector<uint8_t>>(size);
    if (data)
      std::memcpy(bytes->data(), data, size);
    std::shared_ptr<DeviceBuffer> result;
    checked(resources.upload(usage, bytes, result));
    f.buffers.push_back(result);
    return result;
  }
  std::shared_ptr<Image> image(Frame &f, uint32_t w, uint32_t h, VkFormat format, uint32_t stride,
                               uint32_t layers = 1, uint32_t mips = 1,
                               const std::vector<uint8_t> *pixels = nullptr,
                               const data::TextureDescription *sampler = nullptr) {
    auto im = std::make_shared<Image>(binding);
    im->extent = {w, h, 1};
    im->layers = layers;
    im->mips = mips;
    im->stride = stride;
    const bool depth = format == VK_FORMAT_D32_SFLOAT;
    im->aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = im->extent;
    info.mipLevels = mips;
    info.arrayLayers = layers;
    info.format = format;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = pixels
                     ? (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                     : (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                                 : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
    VkImageFormatProperties supported{};
    auto r = vkGetPhysicalDeviceImageFormatProperties(binding.physical_device, format, info.imageType,
                                                      info.tiling, info.usage, 0, &supported);
    require(r == VK_SUCCESS && w <= supported.maxExtent.width && h <= supported.maxExtent.height &&
                layers <= supported.maxArrayLayers && mips <= supported.maxMipLevels,
            E::unsupported_capability, "Producer image format/extent unsupported");
    vkcheck(vkCreateImage(binding.device, &info, nullptr, &im->image), "producer image");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(binding.device, im->image, &req);
    charge(f, req.size);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(binding.physical_device, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if (req.memoryTypeBits & (1u << i)) {
        if (type == UINT32_MAX)
          type = i;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
          type = i;
          break;
        }
      }
    require(type != UINT32_MAX, E::unsupported_capability, "Producer image memory unavailable");
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    vkcheck(vkAllocateMemory(binding.device, &alloc, nullptr, &im->memory), "producer image allocation");
    vkcheck(vkBindImageMemory(binding.device, im->image, im->memory, 0), "producer image binding");
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = im->image;
    view.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {im->aspect, 0, mips, 0, layers};
    vkcheck(vkCreateImageView(binding.device, &view, nullptr, &im->view), "producer image view");
    if (pixels) {
      VkSamplerCreateInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
      auto wrap = [](data::WrapMode mode) {
        return mode == data::WrapMode::repeat   ? VK_SAMPLER_ADDRESS_MODE_REPEAT
               : mode == data::WrapMode::mirror ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                                                : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      };
      si.addressModeU = sampler ? wrap(sampler->wrap_u) : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      si.addressModeV = sampler ? wrap(sampler->wrap_v) : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      si.minFilter = (!sampler || sampler->min_filter == data::FilterMode::linear) ? VK_FILTER_LINEAR
                                                                                   : VK_FILTER_NEAREST;
      si.magFilter = (!sampler || sampler->mag_filter == data::FilterMode::linear) ? VK_FILTER_LINEAR
                                                                                   : VK_FILTER_NEAREST;
      si.mipmapMode =
          si.minFilter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
      si.maxLod = float(mips - 1);
      VkFormatProperties fp{};
      vkGetPhysicalDeviceFormatProperties(binding.physical_device, format, &fp);
      if (si.minFilter == VK_FILTER_LINEAR || si.magFilter == VK_FILTER_LINEAR)
        require((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0,
                E::unsupported_capability, "Producer texture linear filtering unsupported");
      vkcheck(vkCreateSampler(binding.device, &si, nullptr, &im->sampler), "producer sampler");
      im->upload = buffer(f, pixels->data(), pixels->size());
    }
    f.images.push_back(im);
    return im;
  }
  void upload_images(VkCommandBuffer cmd, const Frame &f) const {
    for (const auto &im : f.images)
      if (im->upload) {
        barrier(cmd, *im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkDeviceSize offset = 0;
        for (uint32_t level = 0; level < im->mips; ++level) {
          VkBufferImageCopy copy{};
          copy.bufferOffset = offset;
          copy.imageSubresource = {im->aspect, level, 0, im->layers};
          copy.imageExtent = {std::max(1u, im->extent.width >> level),
                              std::max(1u, im->extent.height >> level), 1};
          vkCmdCopyBufferToImage(cmd, native(im->upload), im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &copy);
          offset += uint64_t(copy.imageExtent.width) * copy.imageExtent.height * im->layers * im->stride;
        }
        barrier(cmd, *im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      }
  }
  void copy_output(VkCommandBuffer cmd, const Image &im,
                   const std::shared_ptr<DeviceBuffer> &destination) const {
    const bool depth = im.aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier(cmd, im,
            depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                  : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT,
            depth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
                  : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = native(destination);
    b.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 1, &b, 0, nullptr);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {im.aspect, 0, 0, 1};
    copy.imageExtent = im.extent;
    vkCmdCopyImageToBuffer(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, native(destination), 1,
                           &copy);
  }
  std::vector<uint8_t> readback(const Frame &f, const std::shared_ptr<DeviceBuffer> &buffer) {
    const auto size = std::static_pointer_cast<VulkanBuffer>(buffer)->size();
    require(f.bytes <= budget && buffer_size(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT) <= budget - f.bytes,
            E::resource_limit, "Producer readback budget exceeded");
    std::vector<uint8_t> result;
    checked(resources.readback(buffer, result));
    return result;
  }
};
inline void bind_buffer(VkDevice device, VkDescriptorSet set, uint32_t slot,
                        const std::shared_ptr<DeviceBuffer> &buffer, bool uniform = false) {
  VkDescriptorBufferInfo info{native(buffer), 0, std::static_pointer_cast<VulkanBuffer>(buffer)->size()};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = slot;
  write.descriptorCount = 1;
  write.descriptorType = uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &info;
  vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}
inline void bind_image(VkDevice device, VkDescriptorSet set, uint32_t slot, const Image &im) {
  VkDescriptorImageInfo info{im.sampler, im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = slot;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &info;
  vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}
inline VkDescriptorSet descriptor_set(Frame &frame, VkDescriptorSetLayout layout) {
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  info.descriptorPool = frame.pool;
  info.descriptorSetCount = 1;
  info.pSetLayouts = &layout;
  vkcheck(vkAllocateDescriptorSets(frame.binding.device, &info, &set), "producer descriptor set");
  return set;
}
inline VkRenderPass render_pass(VkDevice device, const std::vector<VkFormat> &formats) {
  std::vector<VkAttachmentDescription> attachments;
  std::vector<VkAttachmentReference> colors;
  VkAttachmentReference depth{};
  bool has_depth = false;
  for (size_t i = 0; i < formats.size(); ++i) {
    const bool z = formats[i] == VK_FORMAT_D32_SFLOAT;
    VkAttachmentDescription d{};
    d.format = formats[i];
    d.samples = VK_SAMPLE_COUNT_1_BIT;
    d.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    d.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    d.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    d.finalLayout =
        z ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments.push_back(d);
    VkAttachmentReference ref{uint32_t(i), d.finalLayout};
    if (z) {
      depth = ref;
      has_depth = true;
    } else
      colors.push_back(ref);
  }
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = uint32_t(colors.size());
  sub.pColorAttachments = colors.data();
  sub.pDepthStencilAttachment = has_depth ? &depth : nullptr;
  VkRenderPassCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = uint32_t(attachments.size());
  info.pAttachments = attachments.data();
  info.subpassCount = 1;
  info.pSubpasses = &sub;
  VkRenderPass result = VK_NULL_HANDLE;
  vkcheck(vkCreateRenderPass(device, &info, nullptr, &result), "producer render pass");
  return result;
}
} // namespace matter::graphics::pass
