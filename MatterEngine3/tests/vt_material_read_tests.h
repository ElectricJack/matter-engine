#pragma once
#include "vt_queue_tests.h"
#include "vt_prepare_tests.h"
#include "render/vt_compositor.h"

namespace vt_material_read_tests {

class Writer final : public vt::VtPageFiller {
public:
    explicit Writer(std::unique_ptr<vt::VtCompositor> value) : writer_(std::move(value)) {}
    vt::VtPoolBinding pool;
    void begin_preparation_frame() override { writer_->begin_preparation_frame(); }
    bool prepare(const vt::VtPreparationKey& key, const std::shared_ptr<const vt::VtPartSnapshot>& inputs) override {
        return writer_->prepare(key, inputs);
    }
    void release_preparation(const vt::VtPreparationKey& key) override { writer_->release_preparation(key); }
    void invalidate_surface(const vt::VtPreparationKey& key) override { writer_->invalidate_surface(key); }
    void fill(VkCommandBuffer cmd, const vt::VtFillRequest* requests, size_t count) override {
        if (count) pool = *requests[0].pool;
        writer_->fill(cmd, requests, count);
    }
private:
    std::unique_ptr<vt::VtCompositor> writer_;
};

inline bool run_impl(matter::VulkanDevice& vk, std::string& error) {
    vt_queue_tests::Budgets budgets;
    matter::vt_residency_budgets().pool_pages = 256;
    vt::VtResidency residency; vt_queue_tests::Frames frames(vk); uint64_t serial = 0;
    if (!frames.valid() || !residency.init(vk, error)) return false;
    auto native = vt::VtCompositor::create(vk.device(), vk.physical_device(), VK_NULL_HANDLE, error);
    if (!native) return false;
    vt::VtCompositorMaterial materials[2]{}; native->set_materials(materials, 2);
    auto wrapper = std::make_unique<Writer>(std::move(native)); auto* writer = wrapper.get();
    residency.set_filler(std::move(wrapper));
    const auto advance = [&] { return frames.next(residency, ++serial); };
    const gpu_meshing::FaceFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    vt::VtPeriodicDomain domain, pressure_domain;
    if (!vt::vt_make_periodic_domain(frame, {8,8}, 128, domain, error) ||
        !vt::vt_make_periodic_domain(frame, {32,32}, 128, pressure_domain, error)) return false;
    const auto module = [&](const vt::VtPeriodicDomain& source_domain, float color,
                            vt::VtMaterialModuleLease& out) {
        const auto tape = "const " + std::to_string(color) +
            "\nconst 0.7\nconst 0\nconst 1\nconst -0.03\nmaterial 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.04 0\n";
        std::shared_ptr<const vt::VtPartSnapshot> input;
        return vt::vt_make_periodic_material(source_domain, {}, tape, 1, input, error) &&
            residency.acquire_material_module(input, out, error);
    };
    vt::VtMaterialModuleLease a, b;
    if (!module(domain, .2f, a) || !module(pressure_domain, .8f, b)) return false;
    vt::VtMaterialReadLease read;
    const vt::VtMaterialReadBounds bounds{-.01,.04,.01,.06};
    CHECK(residency.acquire_material_read(a, 0, bounds, read, error) == vt::VtMaterialReadStatus::Pending && !read,
          "material read: unfilled tail/detail never substitutes coarse material");
    if (!vt_prepare_tests::until([&] {
        return advance() && residency.acquire_material_read(a, 0, bounds, read, error) == vt::VtMaterialReadStatus::Ready;
    })) return false;
    CHECK(read && read->pages().size() == 2 && read->mip() == 0 && residency.material_read_current(read),
          "material read: wrapped footprint owns two exact fine-page addresses");
    if (!read || read->pages().size() != 2) return false;
    const auto binding = residency.material_module_binding(a);
    matter::VkBufferResource readback;
    if (!matter::create_buffer(vk, 128, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT, readback, error) || !matter::map_buffer(readback, error)) return false;
    const auto copy = [&](VkCommandBuffer cmd) {
        for (uint32_t channel : {vt::kVtChannelAlbedo, vt::kVtChannelHeight}) {
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = writer->pool.image[channel];
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, writer->pool.layer_count};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1; dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
            for (uint32_t i = 0; i < read->pages().size(); ++i) {
                uint32_t layer, x, y; vt::vt_slot_origin(read->pages()[i].material_slot, layer, x, y);
                VkBufferImageCopy region{}; region.bufferOffset = i * 64u + (channel == vt::kVtChannelHeight ? 16u : 0u);
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
                region.imageOffset = {int32_t(x + 68), int32_t(y + 68), 0}; region.imageExtent = {4,4,1};
                vkCmdCopyImageToBuffer(cmd, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
            }
            std::swap(barrier.oldLayout, barrier.newLayout);
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }
        VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo visible{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        visible.memoryBarrierCount = 1; visible.pMemoryBarriers = &host;
        vkCmdPipelineBarrier2(cmd, &visible);
    };
    const auto sample = [&](bool invalidate_after_admission) {
        return frames.next(residency, ++serial, [&](VkCommandBuffer cmd) {
            CHECK(residency.retain_material_read(read), "material read: recorded use extends reader lifetime");
            if (invalidate_after_admission) residency.invalidate_all_content();
            copy(cmd);
        });
    };
    if (!sample(false)) return false;
    std::array<unsigned char, 128> before{};
    // Padding is not copied by the GPU; compare only the 48 defined bytes/page.
    for (uint32_t i = 0; i < 2; ++i)
        std::memcpy(before.data() + i*64, static_cast<unsigned char*>(readback.mapped) + i*64, 48);
    std::vector<vt::VtFeedbackRequest> pressure;
    if (!vt_prepare_tests::until([&] { return advance() && residency.material_module_binding(b).slot; })) return false;
    const auto other = residency.material_module_binding(b);
    // Residency rounds pools to full 256-page layers. Fill the actual capacity,
    // beyond the resident source and pinned tails, so this proves eviction.
    const auto capacity = residency.stats().pool_capacity;
    if (capacity > 32u * 32u || capacity > matter::vt_residency_budgets().queue_cap) {
        error = "material read eviction fixture cannot fill the actual pool"; return false;
    }
    for (uint32_t i = 0; i < capacity; ++i)
        pressure.push_back({other.slot - 1, 0, i % 32u, i / 32u});
    residency.inject_feedback_for_test(pressure.data(), pressure.size());
    if (!vt_prepare_tests::until([&] {
        return advance() && residency.resident_page_slot_for_test(binding.slot, {0,0,0}) == UINT32_MAX &&
            residency.resident_page_slot_for_test(binding.slot, {0,7,0}) == UINT32_MAX;
    })) { error = "material read fixture did not evict both source receiver pages"; return false; }
    CHECK(residency.material_read_current(read) && residency.stats().material_read_pages == 2,
          "material read: source pixels remain owned after receiver-page eviction");
    if (!sample(true)) return false;
    for (uint32_t i = 0; i < 2; ++i)
        CHECK(std::memcmp(before.data() + i*64, static_cast<unsigned char*>(readback.mapped) + i*64, 48) == 0,
              "material read: actual compressed color and height survive source eviction and replacement pressure");
    CHECK(!residency.material_read_current(read) && !residency.retain_material_read(read),
          "material read: edit during recorded consumption rejects stale publication and new uses");
    CHECK(residency.queued_requests_consistent_for_test(), "material read: dependency requests preserve queue accounting");
    a.reset(); b.reset();
    CHECK(residency.stats().module_variants == 1, "material read: last consumer retains its module owner");
    read.reset();
    for (uint32_t i = 0; i < vt::kVtRetireHorizonFrames + 2; ++i) if (!advance()) return false;
    CHECK(residency.stats().module_variants == 0 && residency.stats().material_pages == 0 &&
          residency.stats().material_read_pages == 0 && residency.stats().shared_material_references == 0,
          "material read: releasing final readers returns material and module ownership without counter underflow");
    std::printf("VT_MATERIAL_READ wrapped_pages=2 evicted_receivers=2 retained_color_height=identical stale_publication=rejected\n");
    return true;
}
inline void run(matter::VulkanDevice& vk) {
    std::string error; const bool ok = run_impl(vk, error);
    CHECK(ok, error.empty() ? "material read integration completed" : error.c_str());
}
} // namespace vt_material_read_tests
