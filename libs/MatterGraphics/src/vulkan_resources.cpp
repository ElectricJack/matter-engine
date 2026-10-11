#include <matter/graphics/vulkan_resources.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

// Allocation, mapped-memory coherency, staging copies and fence retirement
// extracted/adapted from MatterEngine3/src/render/vk_resources.cpp (create_buffer,
// map/flush/invalidate_buffer, submit_immediate, upload/readback_buffer). The
// original callers remain until coordinator integration; never link that source
// into this target. Device injection replaces VulkanDevice/DeviceSubmitAccess.
namespace matter::graphics {
namespace {
using E = data::ErrorCode;
data::ValidationResult error(const char *message, VkResult result = VK_SUCCESS) {
  return {{{data::Severity::error, E::backend_failure,
      std::string(message) + " (VkResult " + std::to_string(result) + ")", {}, {}, {}}}};
}
struct Context {
  VulkanDeviceBinding binding;
  VkPhysicalDeviceMemoryProperties memory{};
};
struct Allocation {
  std::shared_ptr<Context> context;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0, allocated = 0;
  VkMemoryPropertyFlags properties = 0;
  void *mapped = nullptr;
  ~Allocation() {
    const auto device = context->binding.device;
    if (mapped) vkUnmapMemory(device, memory);
    if (buffer) vkDestroyBuffer(device, buffer, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
  }
  data::ValidationResult map() {
    if (mapped) return {};
    const auto r = vkMapMemory(context->binding.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    return r == VK_SUCCESS ? data::ValidationResult{} : error("map buffer", r);
  }
  data::ValidationResult coherent(bool invalidate) {
    if (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return {};
    // Whole allocation satisfies nonCoherentAtomSize alignment on both ends.
    VkMappedMemoryRange range{}; range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = memory; range.size = VK_WHOLE_SIZE;
    const auto r = invalidate ? vkInvalidateMappedMemoryRanges(context->binding.device, 1, &range)
                              : vkFlushMappedMemoryRanges(context->binding.device, 1, &range);
    return r == VK_SUCCESS ? data::ValidationResult{} : error("mapped buffer coherency", r);
  }
};
data::ValidationResult allocate(const std::shared_ptr<Context> &context, VkDeviceSize size,
    VkBufferUsageFlags usage, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
    std::shared_ptr<Allocation> &out) {
  auto a = std::make_shared<Allocation>(); a->context = context; a->size = size;
  VkBufferCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = size; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  auto r = vkCreateBuffer(context->binding.device, &info, nullptr, &a->buffer);
  if (r != VK_SUCCESS) return error("create buffer", r);
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(context->binding.device, a->buffer, &requirements);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < context->memory.memoryTypeCount; ++i) {
    const auto flags = context->memory.memoryTypes[i].propertyFlags;
    if ((requirements.memoryTypeBits & (1u << i)) && (flags & required) == required) {
      if (type == UINT32_MAX) type = i;
      if ((flags & preferred) == preferred) { type = i; break; }
    }
  }
  if (type == UINT32_MAX) return error("no compatible buffer memory type");
  VkMemoryAllocateInfo memory{}; memory.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory.allocationSize = requirements.size; memory.memoryTypeIndex = type;
  r = vkAllocateMemory(context->binding.device, &memory, nullptr, &a->memory);
  if (r != VK_SUCCESS) return error("allocate buffer memory", r);
  a->allocated = requirements.size; a->properties = context->memory.memoryTypes[type].propertyFlags;
  r = vkBindBufferMemory(context->binding.device, a->buffer, a->memory, 0);
  if (r != VK_SUCCESS) return error("bind buffer memory", r);
  out = std::move(a); return {};
}
void barrier(VkCommandBuffer cmd, VkBuffer buffer, VkAccessFlags source, VkAccessFlags destination,
             VkPipelineStageFlags from, VkPipelineStageFlags to) {
  VkBufferMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  b.srcAccessMask = source; b.dstAccessMask = destination;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.buffer = buffer; b.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd, from, to, 0, 0, nullptr, 1, &b, 0, nullptr);
}
} // namespace
struct VulkanSubmission::Impl : std::enable_shared_from_this<VulkanSubmission::Impl> {
  std::shared_ptr<Context> context;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;
  bool uncertain_submit = false;
  VkResult completion = VK_SUCCESS;
  // No allocation is needed to quarantine an unprovable failed submission.
  // Break this self-owner only after a fence/idle/device-loss result proves it
  // safe. If every driver wait fails, retain native storage and the device owner.
  std::shared_ptr<Impl> pending_owner;
  std::vector<std::shared_ptr<Allocation>> allocations;
  std::vector<std::shared_ptr<DeviceBuffer>> dependencies;
  VkResult finish(bool wait) noexcept {
    if (!pending) return completion;
    auto r = uncertain_submit ? completion :
        (wait ? vkWaitForFences(context->binding.device, 1, &fence, VK_TRUE, UINT64_MAX)
              : vkGetFenceStatus(context->binding.device, fence));
    if (r == VK_NOT_READY || r == VK_TIMEOUT) return r;
    if (r != VK_SUCCESS) {
      // Establish device completion on an error path before releasing owners.
      std::lock_guard<std::mutex> lock(*context->binding.queue_mutex);
      const auto idle = vkDeviceWaitIdle(context->binding.device);
      if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST) return r;
    }
    pending = false; completion = r; dependencies.clear(); allocations.clear();
    pending_owner.reset(); return r;
  }
  ~Impl() {
    finish(true);
    if (fence) vkDestroyFence(context->binding.device, fence, nullptr);
    if (pool) vkDestroyCommandPool(context->binding.device, pool, nullptr);
  }
  data::ValidationResult start(const std::function<void(VkCommandBuffer)> &record) {
    const auto device = context->binding.device;
    VkCommandPoolCreateInfo pool_info{}; pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = context->binding.queue_family; pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    auto r = vkCreateCommandPool(device, &pool_info, nullptr, &pool);
    if (r != VK_SUCCESS) return error("create command pool", r);
    VkCommandBufferAllocateInfo alloc{}; alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 1;
    r = vkAllocateCommandBuffers(device, &alloc, &command);
    if (r != VK_SUCCESS) return error("allocate command buffer", r);
    VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(command, &begin);
    if (r != VK_SUCCESS) return error("begin command buffer", r);
    record(command);
    r = vkEndCommandBuffer(command);
    if (r != VK_SUCCESS) return error("end command buffer", r);
    VkFenceCreateInfo f{}; f.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    r = vkCreateFence(device, &f, nullptr, &fence);
    if (r != VK_SUCCESS) return error("create submission fence", r);
    VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    pending_owner = shared_from_this(); pending = true;
    {
      std::lock_guard<std::mutex> lock(*context->binding.queue_mutex);
      r = vkQueueSubmit(context->binding.queue, 1, &submit, fence);
    }
    if (r != VK_SUCCESS) {
      // Submission failure may be ambiguous. Retain all dependencies until the
      // device wait returns (including device loss), as in the legacy helper.
      uncertain_submit = true; completion = r; finish(true);
      return error("submit buffer commands", r);
    }
    return {};
  }
};
VulkanSubmission::VulkanSubmission(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
VulkanSubmission::~VulkanSubmission() { impl_->finish(true); }
data::ValidationResult VulkanSubmission::poll(bool &complete) {
  const auto r = impl_->finish(false); complete = r == VK_SUCCESS;
  return r == VK_SUCCESS || r == VK_NOT_READY ? data::ValidationResult{} : error("poll submission", r);
}
data::ValidationResult VulkanSubmission::wait() {
  const auto r = impl_->finish(true); return r == VK_SUCCESS ? data::ValidationResult{} : error("wait submission", r);
}
struct VulkanBuffer::Impl {
  std::shared_ptr<Allocation> allocation;
  std::shared_ptr<VulkanSubmission> upload;
  std::vector<std::weak_ptr<VulkanSubmission>> uses;
};
VulkanBuffer::VulkanBuffer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VulkanBuffer::~VulkanBuffer() {
  if (impl_->upload) impl_->upload->impl_->finish(true);
}
data::ValidationResult VulkanBuffer::poll(bool &done) {
  if (!impl_->upload) { done = true; return {}; }
  auto result = impl_->upload->poll(done);
  if (result.ok() && done) impl_->upload.reset();
  return result;
}
bool VulkanBuffer::idle() const noexcept {
  if (impl_->upload && impl_->upload->impl_->finish(false) != VK_SUCCESS) return false;
  for (const auto &weak : impl_->uses) if (auto use = weak.lock())
    if (use->impl_->finish(false) != VK_SUCCESS) return false;
  return true;
}
uint64_t VulkanBuffer::resident_bytes() const noexcept {
  uint64_t bytes = impl_->allocation->allocated;
  if (impl_->upload) for (const auto &a : impl_->upload->impl_->allocations)
    if (a != impl_->allocation) bytes += a->allocated;
  return bytes;
}
VkBuffer VulkanBuffer::native_buffer() const noexcept { return impl_->allocation->buffer; }
VkDeviceSize VulkanBuffer::size() const noexcept { return impl_->allocation->size; }
struct VulkanResourceDevice::Impl {
  std::shared_ptr<Context> context;
  std::vector<std::shared_ptr<VulkanSubmission>> submissions;
  void reap() {
    submissions.erase(std::remove_if(submissions.begin(), submissions.end(), [](const auto &s) {
      return s->impl_->finish(false) != VK_NOT_READY;
    }), submissions.end());
  }
};
VulkanResourceDevice::VulkanResourceDevice(VulkanDeviceBinding binding) : impl_(new Impl) {
  if (!binding.device || !binding.physical_device || !binding.queue || !binding.lifetime || !binding.queue_mutex)
    throw std::invalid_argument("Vulkan device, queue, lifetime owner and shared queue mutex are required");
  impl_->context = std::make_shared<Context>(); impl_->context->binding = std::move(binding);
  vkGetPhysicalDeviceMemoryProperties(impl_->context->binding.physical_device, &impl_->context->memory);
}
VulkanResourceDevice::~VulkanResourceDevice() { drain(); }
data::ValidationResult VulkanResourceDevice::upload(BufferUsage usage, services::BytesOwner bytes,
    std::shared_ptr<DeviceBuffer> &out) {
  if (!bytes || bytes->empty()) return error("upload requires nonempty byte owner");
  impl_->reap();
  VkBufferUsageFlags flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (usage == BufferUsage::vertex) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (usage == BufferUsage::index) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  std::shared_ptr<Allocation> destination, staging;
  auto r = allocate(impl_->context, bytes->size(), flags, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, destination);
  if (!r.ok()) return r;
  r = allocate(impl_->context, bytes->size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging);
  if (!r.ok()) return r;
  r = staging->map(); if (!r.ok()) return r;
  std::memcpy(staging->mapped, bytes->data(), bytes->size());
  r = staging->coherent(false); if (!r.ok()) return r;
  auto state = std::make_shared<VulkanSubmission::Impl>(); state->context = impl_->context;
  state->allocations = {destination, staging};
  auto submission = std::shared_ptr<VulkanSubmission>(new VulkanSubmission(state));
  auto buffer = std::make_unique<VulkanBuffer::Impl>(); buffer->allocation = destination; buffer->upload = submission;
  out = std::shared_ptr<VulkanBuffer>(new VulkanBuffer(std::move(buffer)));
  impl_->submissions.push_back(submission); // All owners installed before submit.
  return state->start([&](VkCommandBuffer cmd) {
    VkBufferCopy copy{0, 0, destination->size}; vkCmdCopyBuffer(cmd, staging->buffer, destination->buffer, 1, &copy);
    barrier(cmd, destination->buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  });
}
data::ValidationResult VulkanResourceDevice::submit(const std::function<void(VkCommandBuffer)> &record,
    const std::vector<std::shared_ptr<DeviceBuffer>> &dependencies, std::shared_ptr<VulkanSubmission> &out) {
  if (!record) return error("missing command recorder");
  impl_->reap();
  std::vector<std::shared_ptr<VulkanBuffer>> native;
  for (const auto &b : dependencies) {
    auto v = std::dynamic_pointer_cast<VulkanBuffer>(b);
    if (!v || v->impl_->allocation->context != impl_->context) return error("foreign device buffer");
    native.push_back(std::move(v));
  }
  auto state = std::make_shared<VulkanSubmission::Impl>(); state->context = impl_->context;
  state->dependencies = dependencies;
  auto submission = std::shared_ptr<VulkanSubmission>(new VulkanSubmission(state));
  for (const auto &b : native) {
    auto &uses = b->impl_->uses;
    uses.erase(std::remove_if(uses.begin(), uses.end(), [](const auto &w) { return w.expired(); }), uses.end());
    uses.push_back(submission);
  }
  out = submission; impl_->submissions.push_back(submission);
  return state->start(record);
}
data::ValidationResult VulkanResourceDevice::readback(const std::shared_ptr<DeviceBuffer> &buffer, std::vector<uint8_t> &out) {
  impl_->reap();
  auto native = std::dynamic_pointer_cast<VulkanBuffer>(buffer);
  if (!native || native->impl_->allocation->context != impl_->context) return error("foreign readback buffer");
  std::shared_ptr<Allocation> staging;
  auto r = allocate(impl_->context, native->size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging);
  if (!r.ok()) return r;
  auto state = std::make_shared<VulkanSubmission::Impl>(); state->context = impl_->context;
  state->dependencies = {buffer}; state->allocations = {staging};
  auto submission = std::shared_ptr<VulkanSubmission>(new VulkanSubmission(state));
  native->impl_->uses.push_back(submission); impl_->submissions.push_back(submission);
  r = state->start([&](VkCommandBuffer cmd) {
    barrier(cmd, native->native_buffer(), VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferCopy copy{0, 0, native->size()}; vkCmdCopyBuffer(cmd, native->native_buffer(), staging->buffer, 1, &copy);
    barrier(cmd, staging->buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
  });
  if (!r.ok()) return r;
  r = submission->wait(); if (!r.ok()) return r;
  r = staging->map(); if (!r.ok()) return r;
  r = staging->coherent(true); if (!r.ok()) return r;
  const auto *bytes = static_cast<const uint8_t *>(staging->mapped);
  out.assign(bytes, bytes + static_cast<size_t>(native->size())); return {};
}
void VulkanResourceDevice::drain() noexcept {
  for (const auto &s : impl_->submissions) s->impl_->finish(true);
  impl_->submissions.clear();
}
} // namespace matter::graphics
