#include <matter/graphics/graphics.h>
#include <matter/graphics/vulkan_resources.h>
#include <matter/graphics/vulkan_composition.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "vulkan_device_fixture.h"
namespace {
ms::UploadRequest triangle() {
  mc::AssetDocument a; a.id = {"gpu.synthetic"};
  a.provenance = {{"gpu-fixture"}, "fixture://gpu", "1", {}, {}};
  a.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  mc::MeshPrototype m; m.id = {"triangle"};
  m.positions = {{-0.1f, -0.1f, 0}, {0.1f, -0.1f, 0}, {0, 0.1f, 0}};
  m.indices = {0, 1, 2}; m.bounds = {{-0.1f, -0.1f, 0}, {0.1f, 0.1f, 0}};
  m.submeshes = {{0, 3, {"paint"}}};
  mc::MaterialDescription material; material.id = {"paint"}; a.materials = {material};
  a.meshes = {m}; a.bounds = m.bounds;
  return {std::make_shared<const mc::AssetDocument>(std::move(a)), {}, {}};
}
void exercise(const std::shared_ptr<NativeDevice> &native) {
  auto device = std::make_shared<mg::VulkanResourceDevice>(native->binding(native));
  auto scene = new mg::SceneBackend(device);
  mg::Graphics graphics{std::unique_ptr<mg::Backend>(scene)};
  const ms::JobEnvelope job{"gpu", "r1", 1};
  auto submitted = graphics.upload(job, triangle(), {});
  if (!submitted.ok()) for (const auto &d : submitted.diagnostics()) std::cerr << d.message << '\n';
  check(submitted.ok(), "VK scene upload");
  auto ticket = *submitted.value();
  device->drain();
  auto uploaded = graphics.poll(job, ticket);
  check(uploaded.ok() && uploaded.value()->state == ms::UploadState::ready, "VK real upload completion");
  auto handle = uploaded.value()->resource; valid(graphics.release_ticket(ticket), "VK release ticket");
  check(graphics.set_camera(job, {}).ok(), "VK camera");
  ms::SceneInstance a{{"a"}, {"triangle"}, handle, mm::translation({0, 0, 0.5f})};
  ms::SceneInstance b = a; b.id = {"b"}; b.world_transform.m[3] = 0.3f;
  check(graphics.update_scene(job, {1, {a, b}, {}}).ok(), "VK scene placement");
  mg::SceneFrame frame; valid(scene->frame(frame), "VK acquire frame");
  check(frame.visible.size() == 2 && frame.visible[0].asset == frame.visible[1].asset, "VK shared geometry");
  auto buffer = frame.visible[0].asset->meshes[0].indices;
  auto vkbuffer = std::dynamic_pointer_cast<mg::VulkanBuffer>(buffer);
  check(vkbuffer && vkbuffer->native_buffer() && vkbuffer->size() == 12 && vkbuffer->resident_bytes() >= 12, "VK native allocation");
  std::vector<uint8_t> bytes;
  valid(device->readback(buffer, bytes), "VK readback");
  check(bytes == std::vector<uint8_t>({0,0,0,0,1,0,0,0,2,0,0,0}), "VK exact device bytes");
  b.world_transform.m[3] = 4;
  check(graphics.update_scene(job, {2, {b}, {}}).ok(), "VK visibility update");
  mg::SceneFrame culled; valid(scene->frame(culled), "VK cull frame");
  check(culled.visible.size() == 1, "VK only in-frustum draw"); culled = {};
  // A host-signalled timeline semaphore gates the queue before the adapter's
  // submission, so the retirement assertion cannot race a fast GPU.
  VkSemaphoreTypeCreateInfo type{}; type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  VkSemaphoreCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO; si.pNext = &type;
  VkSemaphore gate = VK_NULL_HANDLE; vkcheck(vkCreateSemaphore(native->device, &si, nullptr, &gate), "VK timeline gate");
  const uint64_t value = 1;
  VkTimelineSemaphoreSubmitInfo timeline{}; timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  timeline.waitSemaphoreValueCount = 1; timeline.pWaitSemaphoreValues = &value;
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo blocked{}; blocked.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; blocked.pNext = &timeline;
  blocked.waitSemaphoreCount = 1; blocked.pWaitSemaphores = &gate; blocked.pWaitDstStageMask = &stage;
  { std::lock_guard<std::mutex> lock(*native->mutex);
    vkcheck(vkQueueSubmit(native->queue, 1, &blocked, VK_NULL_HANDLE), "VK block queue"); }
  std::shared_ptr<mg::VulkanSubmission> use;
  auto frame_owner = std::make_shared<int>(42);
  std::weak_ptr<int> retained_frame = frame_owner;
  valid(device->submit([&](VkCommandBuffer cmd) {
    VkBufferMemoryBarrier barrier{}; barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = vkbuffer->native_buffer(); barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    vkCmdFillBuffer(cmd, vkbuffer->native_buffer(), 0, VK_WHOLE_SIZE, 0x01020304);
  }, {buffer}, use, {frame_owner}), "VK queued use");
  frame_owner.reset();
  bool complete = true; valid(use->poll(complete), "VK poll blocked use");
  check(!complete && !buffer->idle(), "VK pending use prevents retirement");
  valid(graphics.release_resource(handle), "VK caller releases resource");
  check(graphics.update_scene(job, {3, {}, {{"a"}, {"b"}}}).ok(), "VK scene releases resource");
  frame = {}; vkbuffer.reset(); std::weak_ptr<mg::DeviceBuffer> retained = buffer; buffer.reset();
  graphics.collect_garbage(); check(!retained.expired(), "VK submission retains buffer");
  use.reset(); // Device retains the submission even after the ticket is dropped.
  check(!retained_frame.expired(), "VK pending submission retains frame resources");
  VkSemaphoreSignalInfo signal{}; signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
  signal.semaphore = gate; signal.value = value;
  vkcheck(vkSignalSemaphore(native->device, &signal), "VK unblock queue");
  device->drain(); graphics.collect_garbage();
  check(retained.expired(), "VK fence completion releases buffer");
  check(retained_frame.expired(), "VK fence completion releases frame resources");
  vkDestroySemaphore(native->device, gate, nullptr);
  auto second = graphics.upload(job, triangle(), {}); check(second.ok(), "VK pending disposal upload");
  graphics.dispose(); graphics.dispose(); // Drains uploads without polling tickets.
  check(graphics.disposed(), "VK idempotent pending disposal");
  auto foreign = std::make_shared<mg::VulkanResourceDevice>(native->binding(native));
  auto standalone = std::make_shared<mg::VulkanResourceDevice>(native->binding(native));
  auto input = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{9,8,7,6});
  std::shared_ptr<mg::DeviceBuffer> owned;
  valid(standalone->upload(mg::BufferUsage::vertex, input, owned), "VK direct upload");
  check(!foreign->readback(owned, bytes).ok(), "VK foreign adapter rejected");
  valid(standalone->readback(owned, bytes), "VK direct readback");
  check(bytes == *input, "VK byte owner copied before return");
  standalone.reset();
  check(owned->idle(), "VK buffer survives adapter destruction");
  owned.reset();
}
} // namespace
#include "composition_native.h"
int main() {
  std::cout << std::unitbuf;
  auto native = std::make_shared<NativeDevice>(); exercise(native);
  exercise_composition(native);
  std::weak_ptr<NativeDevice> lifetime = native;
  auto device = std::make_shared<mg::VulkanResourceDevice>(native->binding(native));
  std::shared_ptr<mg::DeviceBuffer> survivor;
  valid(device->upload(mg::BufferUsage::index,
      std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1,2,3,4}), survivor), "VK final upload");
  native.reset(); device.reset();
  check(!lifetime.expired() && survivor->idle(), "VK last buffer retains injected device owner");
  survivor.reset(); check(lifetime.expired(), "VK last buffer releases native device");
  check(validation_errors == 0, "VK no validation errors including teardown");
  std::cout << "PASS VK-UPLOAD/READBACK/SCENE/SHARING/CULL/FENCE/RETIRE/DISPOSE/OWNERSHIP\n";
}
