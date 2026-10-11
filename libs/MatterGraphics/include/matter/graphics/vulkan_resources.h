#pragma once
#include <matter/graphics/scene_backend.h>
#include <vulkan/vulkan.h>
#include <functional>
#include <mutex>

namespace matter::graphics {
// The application owns device creation/extensions. Keep its owner alive until
// the last buffer/submission dies, and share this mutex with every queue user.
// All other adapter operations are thread confined, like the graphics facade.
struct VulkanDeviceBinding {
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  std::shared_ptr<void> lifetime;
  std::shared_ptr<std::mutex> queue_mutex;
};
class VulkanResourceDevice;
class VulkanSubmission final {
public:
  ~VulkanSubmission(); // Wait before releasing command storage/dependencies.
  data::ValidationResult poll(bool &complete);
  data::ValidationResult wait();
private:
  struct Impl;
  explicit VulkanSubmission(std::shared_ptr<Impl>);
  std::shared_ptr<Impl> impl_;
  friend class VulkanResourceDevice;
  friend class VulkanBuffer;
};
class VulkanBuffer final : public DeviceBuffer {
public:
  ~VulkanBuffer() override;
  data::ValidationResult poll(bool &upload_complete) override;
  bool idle() const noexcept override;
  uint64_t resident_bytes() const noexcept override;
  VkBuffer native_buffer() const noexcept;
  VkDeviceSize size() const noexcept;
private:
  struct Impl;
  explicit VulkanBuffer(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
  friend class VulkanResourceDevice;
};
// Extracted allocation/staging/fence machinery. No renderer globals, window,
// world, procedural source, Streamline, or VulkanDevice implementation needed.
class VulkanResourceDevice final : public ResourceDevice {
public:
  explicit VulkanResourceDevice(VulkanDeviceBinding); // Throws on invalid binding.
  ~VulkanResourceDevice() override;
  data::ValidationResult upload(BufferUsage, services::BytesOwner,
                                std::shared_ptr<DeviceBuffer> &) override;
  // Record on the injected queue; list EVERY buffer referenced by the commands.
  // Dependencies stay alive until completion, even if caller releases its ticket.
  // No externally submitted uses are tracked. Use this entrypoint for retirement.
  data::ValidationResult submit(const std::function<void(VkCommandBuffer)> &record,
      const std::vector<std::shared_ptr<DeviceBuffer>> &dependencies,
      std::shared_ptr<VulkanSubmission> &);
  data::ValidationResult readback(const std::shared_ptr<DeviceBuffer> &, std::vector<uint8_t> &);
  void drain() noexcept override;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace matter::graphics
