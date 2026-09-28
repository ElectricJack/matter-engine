#pragma once
#include <limits>

#include "check.h"
#include "matter/vt_budgets.h"
#include "matter/vulkan_device.h"
#include "render/vt_residency.h"
#include "render/vt_chart_gpu.h"
#include "render/vk_resources.h"
#include "vt_finite_source_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace vt_queue_tests {

struct Budgets {
    matter::VtResidencyBudgets previous;
    Budgets() {
        matter::ensure_vt_residency_env_applied();
        previous = matter::vt_residency_budgets();
        auto& current = matter::vt_residency_budgets();
        current.max_variants = 512;
        current.pool_mb = 0;
        current.pool_pages = 512;
        current.indirection_mb = 1;
        current.mesh_budget_mb = 4;
        current.fills_per_frame = 8;
        current.tail_fills_per_frame = 16;
        current.queue_cap = 256;
        current.enrich_per_frame = 0;
    }
    ~Budgets() { matter::vt_residency_budgets() = previous; }
};

// Real GPU submissions and the shipped deterministic page writer. No fake
// success flag stands in for written page content in this fixture.
class Frames {
public:
    explicit Frames(matter::VulkanDevice& device) : device_(device) {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = device.graphics_queue_family();
        if (vkCreateCommandPool(device.device(), &pool_info, nullptr, &pool_) != VK_SUCCESS) return;
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = pool_;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device.device(), &allocation, &command_) != VK_SUCCESS) return;
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        valid_ = vkCreateFence(device.device(), &fence_info, nullptr, &fence_) == VK_SUCCESS;
    }
    ~Frames() {
        if (fence_) vkDestroyFence(device_.device(), fence_, nullptr);
        if (pool_) vkDestroyCommandPool(device_.device(), pool_, nullptr);
    }
    bool valid() const { return valid_; }
    bool next(vt::VtResidency& residency, uint64_t serial,
              const std::function<void(VkCommandBuffer)>& after_record = {}) {
        if (!valid_) return false;
        residency.begin_frame(serial, 0);
        if (vkResetCommandBuffer(command_, 0) != VK_SUCCESS ||
            vkResetFences(device_.device(), 1, &fence_) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(command_, &begin) != VK_SUCCESS) return false;
        std::string error;
        if (!residency.record_frame(command_, error)) return false;
        if (after_record) after_record(command_);
        if (vkEndCommandBuffer(command_) != VK_SUCCESS) return false;
        bool completion_proven = false;
        const bool submitted = device_.submit_and_wait(command_, fence_, completion_proven, error);
        if (!completion_proven) {
            // The device is poisoned and the command may still be pending.
            // Never destroy its command pool/resources or report a passing run.
            std::fprintf(stderr, "VT queue fixture: GPU completion unknown: %s\n", error.c_str());
            std::fflush(nullptr);
            std::_Exit(2);
        }
        if (!submitted) std::fprintf(stderr, "VT queue fixture: %s\n", error.c_str());
        return submitted;
    }
private:
    matter::VulkanDevice& device_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool valid_ = false;
};

class RetryWriter final : public vt::VtPageFiller {
public:
    explicit RetryWriter(std::unique_ptr<vt::VtPageFiller> writer)
        : writer_(std::move(writer)) {}
    std::function<void(const vt::VtFillRequest&)> on_request;
    bool preparation_ready = true;
    uint32_t preparation_calls = 0;
    std::function<PageReadiness(const vt::VtFillRequest&)> page_probe;
    PageReadiness probe_page(const vt::VtFillRequest& request) override {
        return page_probe ? page_probe(request) : PageReadiness::NeedsPreparation;
    }
    bool prepare(const vt::VtPreparationKey&,
                 const std::shared_ptr<const vt::VtPartSnapshot>&) override { ++preparation_calls; return preparation_ready; }
    void fill(VkCommandBuffer command, const vt::VtFillRequest* requests, size_t count) override {
        std::vector<vt::VtFillRequest> admitted;
        admitted.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (on_request) on_request(requests[i]);
            const auto* atlas = requests[i].atlas;
            const bool tail = atlas &&
                (atlas->atlas_w >> requests[i].mip) <= chart_atlas::kVtTailDim &&
                (atlas->atlas_h >> requests[i].mip) <= chart_atlas::kVtTailDim;
            if (failures_left_ && requests[i].variant_hash == 0x9001u && tail) {
                --failures_left_;
                continue;
            }
            admitted.push_back(requests[i]);
        }
        if (!admitted.empty()) writer_->fill(command, admitted.data(), admitted.size());
        for (const auto& request : admitted)
            if (request.out_filled && *request.out_filled) {
                ++written_[{request.variant_hash, request.mip}];
                ++preparation_uses[request.preparation_key()];
            }
    }
    void release_preparation(const vt::VtPreparationKey& key) override {
        released.push_back(key);
        writer_->release_preparation(key);
    }
    void invalidate_surface(const vt::VtPreparationKey& key) override {
        surface_updates.push_back(key);
        writer_->invalidate_surface(key);
    }
    std::map<vt::VtPreparationKey, uint32_t> preparation_uses;
    std::vector<vt::VtPreparationKey> released, surface_updates;
    uint32_t written(uint64_t owner, uint32_t mip) const {
        const auto found = written_.find({owner, mip});
        return found == written_.end() ? 0u : found->second;
    }
    void refuse_next_two_tails() { failures_left_ = 2; }
private:
    std::unique_ptr<vt::VtPageFiller> writer_;
    uint32_t failures_left_ = 2;
    std::map<std::pair<uint64_t, uint32_t>, uint32_t> written_;
};

// Writes real page bytes, optionally refuses publication AFTER recording them.
// This catches an unsafe producer path even when it partially wrote its output.
class PublicationWriter final : public vt::VtPageFiller {
public:
    explicit PublicationWriter(matter::VulkanDevice& vulkan, std::unique_ptr<vt::VtPageFiller> writer)
        : vulkan_(vulkan), writer_(std::move(writer)) {}
    uint32_t marker = 7;
    bool share_pixels = false;
    bool write_then_refuse = false;
    std::function<void()> after_produce;
    vt::VtPoolBinding pool;
    vt::VtPreparationKey last_preparation;
    std::weak_ptr<const vt::VtPartSnapshot> last_snapshot;
    uint64_t last_revision = 0;
    uint32_t last_triangles = 0;
    vt::VtDrawGeometry geometry;
    void fill(VkCommandBuffer cmd, const vt::VtFillRequest* requests, size_t count) override {
        std::vector<vt::VtPartContext> contexts(count);
        std::vector<vt::VtFillRequest> copies(requests, requests + count);
        if (count) {
            pool = *requests[0].pool;
            last_preparation = requests[0].preparation_key();
            last_revision = requests[0].content_revision;
            last_triangles = requests[0].part()->triangle_count;
            last_snapshot = requests[0].part_snapshot;
        }
        for (size_t i = 0; i < count; ++i) {
            contexts[i] = *requests[i].part();
            contexts[i].dominant_material = marker;
            if (share_pixels && (copies[i].variant_hash & 1u)) contexts[i].dominant_material += 32;
            contexts[i].material_ids = nullptr;
            copies[i].part_context = &contexts[i];
        }
        writer_->fill(cmd, copies.data(), copies.size());
        // Change actual height bytes AND decode metadata after the shipped
        // writer. Refusal/supersession must keep both away from visible slots.
        constexpr uint32_t height_bytes = vt::kVtPageStride * vt::kVtPageStride * 2u;
        std::string error;
        if (!height_staging_.buffer) {
            CHECK(matter::create_buffer(vulkan_, height_bytes,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, height_staging_, error),
                  "VT publication: height writer allocated");
        }
        if (height_staging_.buffer) {
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);
            const uint32_t value = marker * 1000u;
            vkCmdFillBuffer(cmd, height_staging_.buffer, 0, height_bytes, value | (value << 16u));
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier2(cmd, &dep);
            for (auto& request : copies) {
                uint32_t layer, x, y;
                vt::vt_slot_origin(request.physical_slot, layer, x, y);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
                region.imageOffset = {int32_t(x), int32_t(y), 0};
                region.imageExtent = {vt::kVtPageStride, vt::kVtPageStride, 1};
                vkCmdCopyBufferToImage(cmd, height_staging_.buffer,
                    pool.image[vt::kVtChannelHeight], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                if (*request.out_filled) {
                    auto receiver_geometry = geometry;
                    if (share_pixels && (request.variant_hash & 1u)) receiver_geometry.gpu.charts += 0x100u;
                    request.mark_filled({-float(marker) * .001f, float(marker) * .002f, 1u}, receiver_geometry,
                        share_pixels ? vt::VtMaterialPixelKey{marker, 0x517A} : vt::VtMaterialPixelKey{});
                }
            }
        }
        if (write_then_refuse || !height_staging_.buffer)
            for (auto& request : copies) *request.out_filled = false;
        if (after_produce) { auto callback = std::move(after_produce); after_produce = {}; callback(); }
        for (size_t i = 0; i < count; ++i) {
            const auto& snapshot = requests[i].part_snapshot;
            CHECK(snapshot && requests[i].part() == &snapshot->context &&
                      requests[i].atlas == snapshot->context.atlas && snapshot->owns_context_inputs(),
                  "VT snapshot: recorded inputs survive reentrant promotion and release");
        }
    }
private:
    matter::VulkanDevice& vulkan_;
    matter::VkBufferResource height_staging_;
    std::unique_ptr<vt::VtPageFiller> writer_;
};

inline void run_page_probe(matter::VulkanDevice& vulkan) {
    Budgets budgets;
    Frames frames(vulkan);
    vt::VtResidency residency;
    std::string error;
    CHECK(frames.valid() && residency.init(vulkan, error), "VT page probe: native resources");
    if (!frames.valid() || !residency.available()) return;
    auto writer = vt::make_vt_stub_filler(vulkan, 64, error);
    CHECK(writer != nullptr, error.c_str()); if (!writer) return;
    auto observed = std::make_unique<RetryWriter>(std::move(writer));
    auto* producer = observed.get(); residency.set_filler(std::move(observed));
    chart_atlas::ChartAtlasRung atlas;
    atlas.atlas_w = atlas.atlas_h = 128; atlas.charts.resize(1);
    atlas.charts[0].rect_w = atlas.charts[0].rect_h = 128;
    atlas.charts[0].tri_count = 1; atlas.charts[0].texels_per_meter = 1; atlas.tri_order = {0};
    const float positions[] = {0,0,0, 1,0,0, 0,1,0};
    const uint32_t indices[] = {0,1,2};
    vt::VtPartContext context;
    context.variant_hash = 0xabcdef; context.positions = positions; context.vertex_count = 3;
    context.indices = indices; context.triangle_count = 1;
    const auto owner = residency.register_variant(context.variant_hash, 0, atlas, context);
    CHECK(owner != vt::kVtNoSlot, "VT page probe: owner admitted"); if (owner == vt::kVtNoSlot) return;
    const vt::VtFeedbackRequest detail{owner-1u, 0, 0, 0};
    residency.inject_feedback_for_test(&detail, 1);
    using Ready = vt::VtPageFiller::PageReadiness;
    uint32_t phase = 0, probes = 0;
    producer->preparation_ready = false;
    producer->page_probe = [&](const vt::VtFillRequest& request) {
        ++probes;
        CHECK(request.part_snapshot && request.input_snapshot == nullptr && !request.pool &&
              request.physical_slot == UINT32_MAX && !request.out_filled && !request.out_geometry &&
              request.variant_hash == context.variant_hash,
              "VT page probe: immutable input identity without destination or output pointers");
        if (phase == 0) return Ready::Pending;
        return request.mip == 1 ? Ready::Ready : phase == 1 ? Ready::Pending : Ready::NeedsPreparation;
    };
    CHECK(frames.next(residency, 1), "VT page probe: pending lookup frame");
    CHECK(probes && producer->preparation_calls == 0 && residency.stats().fills_total == 0,
          "pending cache lookup neither prepares geometry nor publishes a page");
    phase = 1;
    CHECK(frames.next(residency, 2), "VT page probe: ready tail frame");
    CHECK(producer->preparation_calls == 0 && producer->written(context.variant_hash, 1) == 1 &&
          producer->written(context.variant_hash, 0) == 0,
          "ready cached tail bypasses unavailable owner preparation while detail stays pending");
    phase = 2;
    CHECK(frames.next(residency, 3), "VT page probe: cache miss frame");
    CHECK(producer->preparation_calls == 1 && producer->written(context.variant_hash, 0) == 0,
          "cache miss invokes existing preparation and preserves deferred demand");
    producer->preparation_ready = true;
    CHECK(frames.next(residency, 4), "VT page probe: cache miss fallback frame");
    CHECK(producer->written(context.variant_hash, 0) == 1,
          "deferred cache miss is eventually filled by the original producer");

    // Cached tails must not consume or wait for a one-page baking budget.
    matter::vt_residency_budgets().tail_fills_per_frame = 1;
    const uint64_t first = context.variant_hash + 1;
    for (uint64_t hash = first; hash < first + 5; ++hash) {
        context.variant_hash = hash;
        CHECK(residency.register_variant(hash, 0, atlas, context) != vt::kVtNoSlot,
              "VT page probe: mixed cached and uncached owners admitted");
    }
    producer->page_probe = [first](const vt::VtFillRequest& request) {
        return request.variant_hash < first + 3 ? Ready::Ready : Ready::NeedsPreparation;
    };
    const auto before = producer->preparation_calls;
    CHECK(frames.next(residency, 5), "VT page probe: independent cache admission frame");
    uint32_t imports = 0, baked = 0;
    for (uint64_t hash = first; hash < first + 5; ++hash) {
        if (hash < first + 3) imports += producer->written(hash, 1);
        else baked += producer->written(hash, 1);
    }
    CHECK(imports == 3 && baked == 1 && producer->preparation_calls == before + 1,
          "three ready imports bypass one-page bake budget without increasing bake work");
    CHECK(frames.next(residency, 6), "VT page probe: remaining uncached tail frame");
    CHECK(producer->written(first + 3, 1) + producer->written(first + 4, 1) == 2,
          "remaining uncached tail retains demand for next frame");

    // A forced re-queue that queue_page declines (the page is outside its
    // owner's indirection range) must be dropped, not looked up with
    // std::map::at, which threw std::out_of_range out of record_frame. No
    // shipped path narrows a live owner's range today, so the seam stands in.
    producer->page_probe = {};
    {
        // Durable dirty path: queue_dirty_pages re-feeds the dirty tail.
        context.variant_hash = 0x9010u;
        const uint32_t owner = residency.register_variant(0x9010u, /*rung=*/0, atlas, context);
        CHECK(owner != vt::kVtNoSlot, "queue probe: dirty-path owner registers");
        CHECK(frames.next(residency, 7) && producer->written(0x9010u, 1) == 1,
              "queue probe: dirty-path owner tail filled");
        residency.narrow_indirection_for_test(owner, /*mip_count=*/1);
        residency.invalidate_owners({owner}, vt::VtInvalidationReason::SourceInputs);
        bool survived = true;
        try { survived = frames.next(residency, 8); } catch (const std::exception&) { survived = false; }
        CHECK(survived, "queue probe: dirty page outside the owner's range is dropped without throwing");
        CHECK(residency.stats().dirty_pages == 0 && residency.queued_requests_consistent_for_test(),
              "queue probe: the unqueueable dirty page is retired, not retried forever");
        residency.release_variant(0x9010u);
    }
    {
        // Stale-fill retry: the owner is invalidated while its tail fill is
        // being recorded, so record_frame re-queues the tail it just dropped.
        context.variant_hash = 0x9011u;
        const uint32_t owner = residency.register_variant(0x9011u, /*rung=*/0, atlas, context);
        CHECK(owner != vt::kVtNoSlot, "queue probe: stale-retry owner registers");
        bool armed = true;
        producer->on_request = [&](const vt::VtFillRequest& request) {
            if (!armed || request.variant_hash != 0x9011u || request.mip != 1) return;
            armed = false;
            residency.narrow_indirection_for_test(owner, /*mip_count=*/1);
            residency.invalidate_owners({owner}, vt::VtInvalidationReason::SourceInputs);
        };
        bool survived = true;
        try { survived = frames.next(residency, 9); } catch (const std::exception&) { survived = false; }
        producer->on_request = {};
        CHECK(!armed, "queue probe: stale-retry tail fill was recorded");
        CHECK(survived, "queue probe: stale tail retry outside the owner's range is dropped without throwing");
        bool drained = true;
        try { drained = frames.next(residency, 10); } catch (const std::exception&) { drained = false; }
        CHECK(drained && residency.stats().dirty_pages == 0 && residency.queued_requests_consistent_for_test(),
              "queue probe: the stale tail's durable dirty entry is retired on the next frame");
        residency.release_variant(0x9011u);
    }
}

inline void run_replacements(matter::VulkanDevice& vulkan) {
    const uint32_t errors_before = vulkan.validation_error_count();
    Budgets budgets;
    Frames frames(vulkan);
    vt::VtResidency residency;
    std::string error;
    CHECK(frames.valid() && residency.init(vulkan, error), "VT replacement: native resources initialized");
    if (!frames.valid() || !residency.available()) return;
    auto writer = vt::make_vt_stub_filler(vulkan, 64, error);
    CHECK(writer != nullptr, "VT replacement: shipped writer initialized");
    if (!writer) return;
    auto observed = std::make_unique<PublicationWriter>(vulkan, std::move(writer));
    auto* producer = observed.get();
    residency.set_filler(std::move(observed));
    chart_atlas::ChartAtlasRung atlas;
    atlas.atlas_w = atlas.atlas_h = 128;
    atlas.charts.resize(1);
    atlas.charts[0].rect_w = atlas.charts[0].rect_h = 128;
    atlas.charts[0].tri_count = 1; atlas.charts[0].texels_per_meter = 1;
    atlas.tri_order = {0};
    const float positions[] = {0,0,0, 1,0,0, 0,1,0};
    const uint32_t indices[] = {0,1,2};
    vt::VtPartContext context;
    context.variant_hash = 0x123456;
    context.positions = positions; context.vertex_count = 3;
    context.indices = indices; context.triangle_count = 1;
    const uint32_t owner = residency.register_variant(context.variant_hash, 2, atlas, context);
    CHECK(owner != vt::kVtNoSlot, "VT replacement: owner admitted");
    if (!owner) return;
    CHECK(residency.register_variant(context.variant_hash, 3, atlas, context) == owner,
          "VT promotion: coarse aliases share their owner");
    const vt::VtFeedbackRequest detail{owner-1u, 0, 0, 0};
    residency.inject_feedback_for_test(&detail, 1);
    CHECK(frames.next(residency, 1), "VT replacement: initial pages produced");
    const uint32_t tail_slot = residency.resident_page_slot_for_test(owner, {1,0,0});
    const uint32_t detail_slot = residency.resident_page_slot_for_test(owner, {0,0,0});
    CHECK(tail_slot != UINT32_MAX && detail_slot != UINT32_MAX, "VT replacement: initial mappings exist");
    if (tail_slot == UINT32_MAX || detail_slot == UINT32_MAX) return;
    matter::VkBufferResource readback;
    CHECK(matter::create_buffer(vulkan, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT, readback, error) &&
          matter::map_buffer(readback, error), "VT replacement: exact auxiliary-byte readback allocated");
    if (!readback.mapped) return;
    uint32_t read_offset = 0;
    const auto sample = [&](VkCommandBuffer cmd) {
        const auto& pool = producer->pool;
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = pool.image[vt::kVtChannelAux];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, pool.layer_count};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
        const uint32_t slots[] = {tail_slot, detail_slot};
        for (uint32_t i = 0; i < 2; ++i) {
            uint32_t layer, x, y; vt::vt_slot_origin(slots[i], layer, x, y);
            VkBufferImageCopy copy{};
            copy.bufferOffset = read_offset + i*4;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
            copy.imageOffset = {int32_t(x+64), int32_t(y+64), 0};
            copy.imageExtent = {1,1,1};
            vkCmdCopyImageToBuffer(cmd, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback.buffer, 1, &copy);
        }
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    CHECK(frames.next(residency, 2, sample), "VT replacement: initial content read after completion");
    const auto* bytes = static_cast<const uint8_t*>(readback.mapped);
    CHECK(bytes[0] == 7 && bytes[4] == 7, "VT replacement: initial tail and detail carry known bytes");
    const uint32_t used = residency.stats().pool_used;
    const auto coarse_key = producer->last_preparation;
    const auto coarse_revision = producer->last_revision;
    auto coarse_snapshot = producer->last_snapshot.lock();
    auto oversized = context;
    oversized.vertex_count = 1u << 20;
    const auto before_rejection = residency.stats();
    CHECK(residency.register_variant(context.variant_hash, 1, atlas, oversized) == vt::kVtNoSlot,
          "VT promotion: oversized preparation is rejected before reading its arrays");
    CHECK(residency.slot_for(context.variant_hash, 2) == owner &&
              residency.slot_for(context.variant_hash, 3) == owner && residency.slot_active(owner) &&
              residency.stats().mesh_bytes == before_rejection.mesh_bytes &&
              residency.stats().pool_used == before_rejection.pool_used,
          "VT promotion: rejected finer mesh retains old aliases, readiness and storage");
    if (residency.slot_for(context.variant_hash, 2) != owner) return;
    producer->marker = 9;
    producer->write_then_refuse = true;
    const float finer_positions[] = {0,0,0, 1,0,0, 0,1,0, 1,1,0};
    const uint32_t finer_indices[] = {0,1,2, 1,3,2};
    auto finer_context = context;
    finer_context.positions = finer_positions; finer_context.vertex_count = 4;
    finer_context.indices = finer_indices; finer_context.triangle_count = 2;
    auto finer_atlas = atlas;
    finer_atlas.charts[0].tri_count = 2;
    finer_atlas.tri_order = {0,1};
    CHECK(residency.register_variant(context.variant_hash, 1, finer_atlas, finer_context) == owner &&
              residency.slot_for(context.variant_hash, 2) == owner &&
              residency.slot_for(context.variant_hash, 3) == owner && residency.slot_active(owner),
          "VT promotion: finer mesh retains the transport owner, aliases and readiness");
    CHECK(residency.context_storage_owned_for_test(owner),
          "VT promotion: adopted context points at the owner's stored mesh");
    CHECK(coarse_snapshot && coarse_snapshot->owns_context_inputs() &&
              coarse_snapshot->context.vertex_count == 3 &&
              coarse_snapshot->context.triangle_count == 1 &&
              coarse_snapshot->geometry->atlas.tri_order.size() == 1,
          "VT snapshot: retained coarse preparation keeps its original geometry after promotion");
    coarse_snapshot.reset();
    CHECK(residency.stats().pool_used == used &&
          residency.resident_page_slot_for_test(owner, {0,0,0}) == detail_slot,
          "VT replacement: dirtiness retains the old detail mapping and capacity");
    CHECK(frames.next(residency, 3, sample), "VT replacement: refused writes submitted and read back");
    CHECK(bytes[0] == 7 && bytes[4] == 7,
          "VT replacement: even a producer that writes then refuses cannot alter current pixels");
    producer->write_then_refuse = false;
    CHECK(frames.next(residency, 4, sample), "VT replacement: successful retry submitted");
    CHECK(bytes[0] == 9 && bytes[4] == 9,
          "VT replacement: dirty detail retries without another feedback request");
    CHECK(producer->last_preparation.owner_key == coarse_key.owner_key &&
              producer->last_preparation.owner_generation == coarse_key.owner_generation &&
              producer->last_preparation.rung == 1 && producer->last_triangles == 2 &&
              producer->last_revision > coarse_revision,
          "VT promotion: new mesh/content revision uses the original owner generation");
    producer->marker = 11;
    producer->after_produce = [&] {
        CHECK(residency.register_variant(context.variant_hash, 0, finer_atlas, finer_context) == owner,
              "VT promotion: finer preparation can supersede an already recorded fill");
        producer->marker = 13;
    };
    residency.invalidate_owners({owner});
    CHECK(frames.next(residency, 5, sample), "VT replacement: superseded producer work submitted");
    CHECK(bytes[0] == 9 && bytes[4] == 9 && residency.stats().fills_stale_total == 2,
          "VT replacement: superseded tail/detail bytes never reach current slots");
    CHECK(frames.next(residency, 6, sample), "VT replacement: newest revision submitted");
    CHECK(bytes[0] == 13 && bytes[4] == 13 && residency.stats().dirty_pages == 0,
          "VT replacement: latest revision replaces both pages and clears durable work");
    // Both reads and the intervening replacement execute in ONE submission:
    // no CPU fence or idle wait can accidentally make publication safe here.
    // Use a different retired staging-ring slot for the second frame record.
    CHECK(frames.next(residency, 7, [&](VkCommandBuffer cmd) {
        sample(cmd);
        producer->marker = 15;
        residency.invalidate_owners({owner});
        residency.begin_frame(8, 1);
        CHECK(residency.record_frame(cmd, error), "VT replacement: second frame recorded before submit");
        read_offset = 8;
        sample(cmd);
    }), "VT replacement: old/new GPU readers submitted without an intermediate wait");
    CHECK(bytes[0] == 13 && bytes[4] == 13 && bytes[8] == 15 && bytes[12] == 15,
          "VT replacement: earlier readers see old bytes and later readers see published bytes");
    read_offset = 0;
    producer->marker = 17;
    producer->after_produce = [&] { residency.release_variant(context.variant_hash); };
    residency.invalidate_owners({owner});
    CHECK(frames.next(residency, 9, sample), "VT replacement: released owner producer work submitted");
    CHECK(bytes[0] == 15 && bytes[4] == 15 && residency.stats().variants == 0 &&
          residency.stats().dirty_pages == 0 && residency.stats().fills_stale_total == 4,
          "VT replacement: released owner cannot publish into retiring slots");
    CHECK(vulkan.validation_error_count() == errors_before, "VT replacement: zero Vulkan validation errors");
}

inline void run_shared_enrichment_transition(matter::VulkanDevice& vulkan) {
    Budgets budgets;
    matter::vt_residency_budgets().pool_mb=100;
    Frames frames(vulkan);vt::VtResidency residency;std::string error;
    CHECK(frames.valid() && residency.init(vulkan,error),"VT shared enrichment: runtime initialized");
    if(!frames.valid() || !residency.available()) return;
    auto writer=vt::make_vt_stub_filler(vulkan,64,error);
    CHECK(writer!=nullptr,"VT shared enrichment: page writer initialized");
    if(!writer) return;
    auto producer=std::make_unique<PublicationWriter>(vulkan,std::move(writer));
    producer->share_pixels=true;residency.set_filler(std::move(producer));
    class Probe final : public vt::VtPageEnricher {
    public:
        std::vector<uint32_t> targets;
        void enrich(VkCommandBuffer,const vt::VtEnrichRequest* requests,size_t count) override {
            for(size_t i=0;i<count;++i) targets.push_back(requests[i].physical_slot);
        }
        void invalidate_part(uint64_t) override {}
        uint32_t sample_count() const override {return 1;}
        float max_footprint_meters() const override {return 1000;}
    };
    auto probe=std::make_unique<Probe>();auto* observed=probe.get();
    residency.set_enricher(std::move(probe));
    chart_atlas::ChartAtlasRung atlas;
    atlas.atlas_w=atlas.atlas_h=128;atlas.charts.resize(1);atlas.tri_order={0};
    auto& chart=atlas.charts[0];chart.rect_w=chart.rect_h=128;chart.tri_count=1;chart.texels_per_meter=1;
    const float positions[]={0,0,0,1,0,0,0,1,0};const uint32_t indices[]={0,1,2};
    vt::VtFeedbackRequest requests[2];uint32_t owners[2];
    for(uint32_t i=0;i<2;++i) {
        vt::VtPartContext ctx;ctx.positions=positions;ctx.vertex_count=3;ctx.indices=indices;ctx.triangle_count=1;
        ctx.variant_hash=0xB010+i;
        owners[i]=residency.register_variant(ctx.variant_hash,0,atlas,ctx);
        CHECK(owners[i]!=vt::kVtNoSlot,"VT shared enrichment: independent receiver admitted");
        if(owners[i]==vt::kVtNoSlot) return;
        requests[i]={owners[i]-1,0,0,0};
    }
    residency.inject_feedback_for_test(requests,2);
    CHECK(frames.next(residency,1) && residency.stats().material_pages==1 &&
          residency.stats().shared_material_references==3 && observed->targets.empty(),
          "VT shared enrichment: installed zero-budget enricher permits sharing without writes");
    matter::vt_residency_budgets().enrich_per_frame=4;
    CHECK(frames.next(residency,2) && residency.stats().material_pages==4 &&
          residency.stats().shared_material_references==0 && residency.stats().dirty_pages==0 &&
          observed->targets.empty(),
          "VT shared enrichment: enabling budget replaces shared pages with private material storage");
    CHECK(frames.next(residency,3),"VT shared enrichment: following frame drains enrichment after publication");
    std::sort(observed->targets.begin(),observed->targets.end());
    CHECK(observed->targets.size()==4 &&
          std::adjacent_find(observed->targets.begin(),observed->targets.end())==observed->targets.end(),
          "VT shared enrichment: every enrichment write targets a distinct material allocation");
}

inline void run_input_snapshot_publication(matter::VulkanDevice& vulkan) {
    run_shared_enrichment_transition(vulkan);
    const uint32_t errors_before = vulkan.validation_error_count();
    Budgets budgets;
    matter::vt_residency_budgets().pool_mb = 100;
    Frames frames(vulkan);
    vt::VtResidency residency;
    std::string error;
    CHECK(frames.valid() && residency.init(vulkan, error), "VT input identity: native runtime initialized");
    CHECK(residency.stats().pool_capacity == 512 &&
              residency.stats().pool_bytes == uint64_t(512) * 136u * 136u * 9u &&
              residency.input_snapshot_buffer_size() == uint64_t(512) * sizeof(vt::VtPageMetadata),
          "VT height: fixed 100 MiB pool accounts for all five channels and draw-geometry metadata");
    std::printf("VT height pool: %u pages, %llu image bytes, %llu metadata bytes\n",
                residency.stats().pool_capacity,
                static_cast<unsigned long long>(residency.stats().pool_bytes),
                static_cast<unsigned long long>(residency.input_snapshot_buffer_size()));
    if (!frames.valid() || !residency.available()) return;
    auto writer = vt::make_vt_stub_filler(vulkan, 64, error);
    CHECK(writer != nullptr, "VT input identity: native page writer initialized");
    if (!writer) return;
    auto observed = std::make_unique<PublicationWriter>(vulkan, std::move(writer));
    auto* producer = observed.get();
    producer->share_pixels = true;
    residency.set_filler(std::move(observed));
    // Synthetic addresses exercise publication/ownership only; this test copies
    // metadata back without dereferencing them. Compositor tests use real BDA.
    const auto geometry_version = [](uint32_t marker) {
        vt::VtDrawGeometry geometry;
        geometry.gpu.charts = (uint64_t(marker) << 32u) | 0x1230u;
        geometry.gpu.triangles = (uint64_t(marker) << 32u) | 0x4560u;
        geometry.gpu.chart_count = marker;
        geometry.gpu.triangle_count = marker * 2u;
        geometry.lifetime = std::make_shared<uint32_t>(marker);
        return geometry;
    };
    producer->geometry = geometry_version(7);
    const std::weak_ptr<const void> old_geometry = producer->geometry.lifetime;
    const auto version = [](uint32_t index) {
        return std::make_shared<const vt::VtInputSnapshot>(index, std::make_shared<uint32_t>(index));
    };
    auto a = version(0), b = version(1), c = version(2), d = version(3), e = version(4);
    const std::weak_ptr<void> old_a = a->lifetime, old_b = b->lifetime, old_c = c->lifetime;
    CHECK(residency.set_input_snapshot(a, {}), "VT input identity: first immutable binding admitted");
    CHECK(!residency.set_input_snapshot(version(vt::kVtMaxInputSnapshots), {}),
          "VT input identity: out-of-range binding index rejected");
    CHECK(!residency.set_input_snapshot(version(0), {}),
          "VT input identity: live index cannot name a different binding");
    chart_atlas::ChartAtlasRung atlas;
    atlas.atlas_w = atlas.atlas_h = 128;
    atlas.charts.resize(1);
    atlas.charts[0].rect_w = atlas.charts[0].rect_h = 128;
    atlas.charts[0].tri_count = 1;
    atlas.charts[0].texels_per_meter = 1;
    atlas.tri_order = {0};
    const float positions[] = {0,0,0, 1,0,0, 0,1,0};
    const uint32_t indices[] = {0,1,2};
    uint32_t owners[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        vt::VtPartContext context;
        context.variant_hash = 0xB001u + i;
        context.positions = positions; context.vertex_count = 3;
        context.indices = indices; context.triangle_count = 1;
        context.dominant_material = i;
        owners[i] = residency.register_variant(context.variant_hash, 0, atlas, context);
        CHECK(owners[i] != vt::kVtNoSlot, "VT input identity: independent material owner admitted");
        if (!owners[i]) return;
    }
    const vt::VtFeedbackRequest requests[] = {{owners[0] - 1u, 0, 0, 0}, {owners[1] - 1u, 0, 0, 0}};
    residency.inject_feedback_for_test(requests, 2);
    CHECK(frames.next(residency, 1), "VT input identity: initial pages produced");
    CHECK(residency.stats().material_pages == 1 && residency.stats().shared_material_references == 3,
          "VT shared pixels: four receiver pages use one material allocation");
    uint32_t slots[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        slots[i] = residency.resident_page_slot_for_test(owners[i / 2u], {i % 2u, 0, 0});
        CHECK(slots[i] != UINT32_MAX, "VT input identity: exact tail/detail mapping exists");
        if (slots[i] == UINT32_MAX) return;
    }
    struct Sample { vt::VtPageMetadata metadata[4]; uint32_t pixels[4]; uint32_t height[4]; };
    static_assert(sizeof(Sample) == 352, "VT input identity: readback offsets stay packed");
    matter::VkBufferResource readback;
    CHECK(matter::create_buffer(vulkan, sizeof(Sample) * 2u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT, readback, error) && matter::map_buffer(readback, error),
          "VT input identity: GPU metadata and pixel readback allocated");
    if (!readback.mapped) return;
    uint32_t read_offset = 0;
    const auto sample = [&](VkCommandBuffer cmd) {
        VkBufferMemoryBarrier2 input{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        input.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        input.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
        input.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        input.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        input.srcQueueFamilyIndex = input.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        input.buffer = residency.input_snapshot_buffer(); input.size = VK_WHOLE_SIZE;
        VkImageMemoryBarrier2 page{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        page.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        page.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        page.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        page.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        page.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        page.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        page.srcQueueFamilyIndex = page.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        page.image = producer->pool.image[vt::kVtChannelAux];
        page.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, producer->pool.layer_count};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount = 1; dep.pBufferMemoryBarriers = &input;
        VkImageMemoryBarrier2 images[2] = {page, page};
        images[1].image = producer->pool.image[vt::kVtChannelHeight];
        dep.imageMemoryBarrierCount = 2; dep.pImageMemoryBarriers = images;
        vkCmdPipelineBarrier2(cmd, &dep);
        for (uint32_t i = 0; i < 4; ++i) {
            VkBufferCopy id{static_cast<VkDeviceSize>(slots[i]) * sizeof(vt::VtPageMetadata),
                            read_offset + i * sizeof(vt::VtPageMetadata), sizeof(vt::VtPageMetadata)};
            vkCmdCopyBuffer(cmd, residency.input_snapshot_buffer(), readback.buffer, 1, &id);
            uint32_t layer, x, y;
            vt::vt_slot_origin(slots[i], layer, x, y);
            VkBufferImageCopy pixel{};
            pixel.bufferOffset = read_offset + offsetof(Sample, pixels) + i * 4u;
            pixel.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
            pixel.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
            pixel.imageExtent = {1, 1, 1};
            vkCmdCopyImageToBuffer(cmd, page.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback.buffer, 1, &pixel);
            pixel.bufferOffset = read_offset + offsetof(Sample, height) + i * 4u;
            vt::vt_slot_origin(residency.material_page_slot_for_test(slots[i]), layer, x, y);
            pixel.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
            pixel.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
            pixel.imageExtent = {2, 1, 1};
            vkCmdCopyImageToBuffer(cmd, images[1].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  readback.buffer, 1, &pixel);
        }
        std::swap(page.oldLayout, page.newLayout);
        page.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        page.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        page.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        page.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        images[0] = images[1] = page;
        images[1].image = producer->pool.image[vt::kVtChannelHeight];
        input.buffer = readback.buffer;
        input.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        input.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        input.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        input.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    const auto* samples = static_cast<const Sample*>(readback.mapped);
    const auto matches = [&](uint32_t sample_index, uint32_t left_id, uint32_t right_id,
                             uint32_t left_pixel, uint32_t right_pixel) {
        const auto& result = samples[sample_index];
        bool good = true;
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t marker = i < 2 ? left_pixel : right_pixel;
            good &= result.metadata[i].input_snapshot == (i < 2 ? left_id : right_id) &&
                    (result.pixels[i] & 255u) == marker + (i < 2 ? 32u : 0u) &&
                    result.metadata[i].height.version == 1u &&
                    result.metadata[i].height.min_m == -float(marker) * .001f &&
                    result.metadata[i].height.range_m == float(marker) * .002f &&
                    result.metadata[i].geometry.charts == (((uint64_t(marker) << 32u) | 0x1230u) + (i < 2 ? 0x100u : 0u)) &&
                    result.metadata[i].geometry.triangles == ((uint64_t(marker) << 32u) | 0x4560u) &&
                    result.metadata[i].geometry.chart_count == marker &&
                    result.metadata[i].geometry.triangle_count == marker * 2u &&
                    result.height[i] == ((marker * 1000u) | ((marker * 1000u) << 16u));
        }
        return good;
    };
    const auto initial_fills = residency.stats().fills_total;
    residency.pause_page_fills_for_test(true);
    CHECK(residency.set_input_snapshot(b, {0}), "VT input identity: material A edit staged");
    a.reset();
    CHECK(frames.next(residency, 2, sample), "VT input identity: paused edit submitted");
    CHECK(matches(0, 0, 1, 7, 7) && residency.stats().fills_total == initial_fills,
          "VT input identity: unchanged owner retags without fills; dirty owner retains old identity");
    CHECK(residency.set_input_snapshot(c, {1}), "VT input identity: second material edit staged");
    b.reset();
    CHECK(frames.next(residency, 3, sample), "VT input identity: consecutive edit submitted");
    CHECK(matches(0, 0, 1, 7, 7) && !old_a.expired() && !old_b.expired(),
          "VT input identity: a later unrelated edit cannot retag an already dirty older page");
    residency.pause_page_fills_for_test(false);
    producer->write_then_refuse = true;
    producer->marker = 9;
    producer->geometry = geometry_version(9);
    const std::weak_ptr<const void> refused_geometry = producer->geometry.lifetime;
    CHECK(frames.next(residency, 4, sample), "VT input identity: refused candidates submitted");
    CHECK(matches(0, 0, 1, 7, 7), "VT input identity: refusal keeps old pixels and input ids together");
    producer->geometry = geometry_version(9);
    CHECK(refused_geometry.expired(), "VT geometry: refused candidates leave no retained geometry");
    const std::weak_ptr<const void> stale_geometry = producer->geometry.lifetime;
    producer->write_then_refuse = false;
    producer->after_produce = [&] {
        CHECK(residency.set_input_snapshot(d, {0, 1}), "VT input identity: recorded candidates superseded");
    };
    const auto stale_before = residency.stats().fills_stale_total;
    CHECK(frames.next(residency, 5, sample), "VT input identity: stale candidates submitted");
    c.reset();
    CHECK(matches(0, 0, 1, 7, 7) && residency.stats().fills_stale_total == stale_before + 4 && old_c.expired(),
          "VT input identity: superseded candidates cannot publish ids or retain unused draw bindings");
    producer->geometry = geometry_version(9);
    CHECK(stale_geometry.expired(), "VT geometry: superseded candidates leave no retained geometry");
    const std::weak_ptr<const void> middle_geometry = producer->geometry.lifetime;
    CHECK(frames.next(residency, 6, sample), "VT input identity: latest replacement submitted");
    CHECK(matches(0, 3, 3, 9, 9) && residency.stats().dirty_pages == 0,
          "VT input identity: successful copies publish matching pixels, height, range and binding ids");
    CHECK(!old_a.expired() && !old_b.expired(), "VT input identity: replaced bindings survive retiring readers");
    CHECK(frames.next(residency, 7, [&](VkCommandBuffer cmd) {
        sample(cmd);
        CHECK(residency.set_input_snapshot(e, {0}), "VT input identity: second record stages next binding");
        producer->marker = 11;
        producer->geometry = geometry_version(11);
        residency.begin_frame(8, 1);
        CHECK(residency.record_frame(cmd, error), "VT input identity: two frames recorded before one submit");
        read_offset = sizeof(Sample);
        sample(cmd);
    }), "VT input identity: old/new readers execute in one native submission");
    d.reset();
    CHECK(matches(0, 3, 3, 9, 9) && matches(1, 4, 4, 11, 9),
          "VT input identity: queue ordering preserves both metadata versions and unchanged page bytes");
    CHECK(samples[0].metadata[0].material_slot == samples[0].metadata[2].material_slot &&
          samples[0].metadata[0].geometry.charts != samples[0].metadata[2].geometry.charts &&
          samples[1].metadata[0].material_slot != samples[1].metadata[2].material_slot &&
          residency.stats().material_pages == 2,
          "VT shared pixels: private receiver geometry/coverage survives copy-on-write appearance edits");
    read_offset = 0;
    const uint64_t retire_ab = 6u + vt::kVtRetireHorizonFrames;
    for (uint64_t serial = 9; serial < retire_ab; ++serial)
        CHECK(frames.next(residency, serial, sample) && !old_a.expired() && !old_b.expired() &&
                  !old_geometry.expired(),
              "VT input identity: binding ownership spans the complete retirement horizon");
    CHECK(frames.next(residency, retire_ab, sample) && old_a.expired() && old_b.expired() &&
              old_geometry.expired(),
          "VT input identity: superseded bindings are reclaimed after retirement");
    auto reused = version(0);
    const std::weak_ptr<void> released = reused->lifetime;
    const auto before_retag = residency.stats().fills_total;
    CHECK(residency.set_input_snapshot(reused, {}), "VT input identity: retired index can be reused");
    e.reset();
    const uint64_t reuse_frame = retire_ab + 1u;
    CHECK(frames.next(residency, reuse_frame, sample) && matches(0, 0, 0, 11, 9) &&
              residency.stats().fills_total == before_retag,
          "VT input identity: compatible retag needs metadata only, no texture regeneration");
    residency.release_variant(0xB001u);
    residency.release_variant(0xB002u);
    const std::weak_ptr<const void> released_geometry = producer->geometry.lifetime;
    producer->geometry = {};
    CHECK(residency.set_input_snapshot(nullptr, {}), "VT input identity: desired draw binding released");
    reused.reset();
    const uint64_t release_retire = reuse_frame + vt::kVtRetireHorizonFrames;
    for (uint64_t serial = reuse_frame + 1u; serial < release_retire; ++serial) {
        CHECK(frames.next(residency, serial, sample), "VT input identity: released page retirement advanced");
        CHECK(!released.expired() && !released_geometry.expired() && !middle_geometry.expired() &&
                  matches(0, 0, 0, 11, 9),
              "VT input identity: released pages keep compatible metadata and bindings for earlier draws");
    }
    CHECK(frames.next(residency, release_retire) && released.expired() &&
              released_geometry.expired() && middle_geometry.expired(),
          "VT input identity: final owner release eventually frees its binding");
    CHECK(vulkan.validation_error_count() == errors_before, "VT input identity: zero Vulkan validation errors");
}

inline void run_scoped_updates(matter::VulkanDevice& vulkan) {
    const uint32_t errors_before = vulkan.validation_error_count();
    Budgets budgets;
    Frames frames(vulkan);
    vt::VtResidency residency;
    std::string error;
    CHECK(frames.valid() && residency.init(vulkan, error), "VT dependencies: runtime initialized");
    if (!frames.valid() || !residency.available()) return;
    auto writer = vt::make_vt_stub_filler(vulkan, 64, error);
    CHECK(writer != nullptr, "VT dependencies: real writer initialized");
    if (!writer) return;
    auto observed = std::make_unique<RetryWriter>(std::move(writer));
    RetryWriter* writes = observed.get();
    residency.set_filler(std::move(observed));
    chart_atlas::ChartAtlasRung atlas;
    atlas.atlas_w = atlas.atlas_h = 256;
    atlas.tri_order = {0};
    atlas.charts.resize(1);
    atlas.charts[0].rect_w = atlas.charts[0].rect_h = 256;
    atlas.charts[0].tri_count = 1;
    atlas.charts[0].texels_per_meter = 4;
    const float positions[] = {0,0,0, 1,0,0, 0,1,0};
    const float normals[] = {0,0,1, 0,0,1, 0,0,1};
    const float uvs[] = {0,0, 1,0, 0,1};
    const uint32_t indices[] = {0,1,2};
    uint32_t ids[] = {0,0,0};
    const float material[] = {.8f,.7f,.6f,.5f, .2f,.3f,.4f,.5f, .4f,.5f,.6f,.7f};
    vt::VtPartContext context{};
    context.positions = positions;
    context.normals = normals;
    context.surface_uvs = uvs;
    context.indices = indices;
    context.vertex_count = 3;
    context.triangle_count = 1;
    context.material_table = material;
    context.material_count = 3;
    context.material_stride = 4;
    std::vector<uint32_t> owners;
    for (uint32_t i = 0; i < 3; ++i) {
        ids[0] = ids[1] = ids[2] = i;
        context.variant_hash = 0xA000u + i;
        context.dominant_material = i;
        context.material_ids = i == 2 ? nullptr : ids;
        owners.push_back(residency.register_variant(context.variant_hash, 0, atlas, context));
        CHECK(owners.back() != vt::kVtNoSlot, "VT dependencies: owner admitted");
        if (owners.back() == vt::kVtNoSlot) return;
    }
    context.variant_hash = 0xA000u;
    ids[0] = ids[1] = ids[2] = 0;
    context.material_ids = ids;
    context.dominant_material = 0;
    const uint32_t alias = residency.register_variant(0xA000u, 1, atlas, context);
    CHECK(alias == owners[0], "VT dependencies: compatible rung aliases share an owner");
    uint64_t serial = 0;
    const auto step = [&]() {
        std::vector<vt::VtFeedbackRequest> feedback;
        for (uint32_t owner : owners) feedback.push_back({owner - 1u, 0, 0, 0});
        residency.inject_feedback_for_test(feedback.data(), feedback.size());
        const bool ok = frames.next(residency, ++serial);
        CHECK(ok, "VT dependencies: frame submitted and completed");
        return ok;
    };
    if (!step() || !step()) return;
    for (uint32_t i = 0; i < 3; ++i)
        CHECK(writes->written(0xA000u + i, 0) == 1 && writes->written(0xA000u + i, 2) == 1,
              "VT dependencies: each owner has initialized detail and tail");

    const auto clean = residency.stats();
    CHECK(residency.invalidate_material_content({31}) == 0, "VT dependencies: unused material drops no pages");
    if (!step()) return;
    CHECK(residency.stats().fills_total == clean.fills_total &&
          residency.stats().invalidations_total == clean.invalidations_total,
          "VT dependencies: unused material causes no work or invalidation");
    CHECK(residency.invalidate_material_content({0, 0}) == 1,
          "VT dependencies: duplicate material changes affect one owner detail page");
    if (!step() || !step()) return;
    CHECK(writes->written(0xA000u, 0) == 2 && writes->written(0xA000u, 2) == 2 &&
          writes->written(0xA001u, 0) == 1 && writes->written(0xA002u, 0) == 1,
          "VT dependencies: unrelated pages remain mapped under feedback");
    CHECK(residency.invalidate_owners({owners[0], alias, vt::kVtNoSlot}) == 1,
          "VT dependencies: two aliases invalidate shared content only once");
    if (!step() || !step()) return;
    CHECK(writes->written(0xA000u, 2) == 3, "VT dependencies: alias update refills exactly one tail");

    residency.release_variant(0xA000u, 0);
    CHECK(residency.invalidate_material_content({0}) == 1,
          "VT dependencies: releasing one alias keeps the surviving dependency");
    if (!step() || !step()) return;
    residency.release_variant(0xA000u, 1);
    const uint64_t before_release_probe = residency.stats().invalidations_total;
    CHECK(residency.invalidate_material_content({0}) == 0 &&
          residency.stats().invalidations_total == before_release_probe,
          "VT dependencies: releasing the last alias removes reverse links");

    const uint8_t weights[] = {255,255,255};
    const uint32_t palette[] = {2};
    bool changed = false;
    CHECK(residency.update_variant_surface(0xA001u, 0, weights, 3, palette, 1, 7,
              nullptr, nullptr, 0, &changed) && changed,
          "VT dependencies: surface edit replaces effective material inputs");
    CHECK(residency.invalidate_material_content({1}) == 1,
          "VT dependencies: tape retains the conditional zero-weight triangle fallback");
    if (!step() || !step()) return;
    CHECK(residency.invalidate_material_content({2}) == 2,
          "VT dependencies: shared tape and scalar fallback material affects both owners");
    if (!step() || !step()) return;
    CHECK(writes->written(0xA001u, 2) == 3 && writes->written(0xA002u, 2) == 2,
          "VT dependencies: both consumers refresh");
    changed = true;
    CHECK(residency.update_variant_surface(0xA001u, 0, weights, 3, palette, 1, 7,
              nullptr, nullptr, 0, &changed) && !changed,
          "VT dependencies: identical surface publication is accepted without dirtying");
    float receiver_frame[]={1,0,0,8, 0,1,0,16, 0,0,1,3};
    CHECK(residency.update_variant_surface(0xA001u,0,weights,3,palette,1,7,
              nullptr,nullptr,0,&changed,receiver_frame,1) && changed,
          "VT dependencies: receiver world frame edit changes the retained source inputs");
    CHECK(residency.update_variant_surface(0xA001u,0,weights,3,palette,1,7,
              nullptr,nullptr,0,&changed,receiver_frame,1) && !changed,
          "VT dependencies: identical receiver frame is a no-op");
    receiver_frame[7]=17;
    CHECK(residency.update_variant_surface(0xA001u,0,weights,3,palette,1,7,
              nullptr,nullptr,0,&changed,receiver_frame,1) && changed,
          "VT dependencies: vertical motion cannot reuse stale world-space input");
    receiver_frame[7]=std::numeric_limits<float>::quiet_NaN();
    CHECK(!residency.update_variant_surface(0xA001u,0,weights,3,palette,1,7,
              nullptr,nullptr,0,&changed,receiver_frame,1) && !changed,
          "VT dependencies: invalid receiver frame preserves the committed snapshot");
    CHECK(residency.update_variant_surface(0xA001u, 0, nullptr, 0, nullptr, 0, 0,
              nullptr, nullptr, 0, &changed) && changed,
          "VT dependencies: removing the tape restores triangle-material dependencies");
    CHECK(residency.invalidate_material_content({2}) == 1,
          "VT dependencies: removing the tape removes its obsolete palette dependency");
    CHECK(residency.invalidate_material_content({1}) == 1,
          "VT dependencies: restored triangle material selects its owner");
    if (!step() || !step()) return;
    context.variant_hash = 0x9001u; // RetryWriter refuses this tail twice.
    const auto retry_owner = residency.register_variant(context.variant_hash, 0, atlas, context);
    CHECK(retry_owner != vt::kVtNoSlot, "VT age: retry owner admitted");
    if (retry_owner == vt::kVtNoSlot) return;
    owners.push_back(retry_owner);
    if (!step()) return;
    CHECK(residency.stats().mandatory_queue_depth == 1 &&
          residency.stats().oldest_mandatory_age_frames == 1,
          "VT age: first refusal keeps the original request age");
    if (!step()) return;
    CHECK(residency.stats().mandatory_queue_depth == 1 &&
          residency.stats().oldest_mandatory_age_frames == 2,
          "VT age: repeated refusal does not reset the waiting time");
    if (!step() || !step()) return;
    CHECK(residency.stats().mandatory_queue_depth == 0 &&
          residency.stats().oldest_mandatory_age_frames == 0,
          "VT age: successful retry clears the mandatory backlog");
    CHECK(vulkan.validation_error_count() == errors_before, "VT dependencies: zero Vulkan validation errors");
    std::printf("VT dependencies: scoped material, alias, fallback, tape and no-op cases completed\n");
}

// Records the production enrichment seam's identities; actual AO output and
// acceleration-structure reuse are checked by the vt-enrich rendering fixture.
class EnrichmentIdentityProbe final : public vt::VtPageEnricher {
public:
    std::vector<vt::VtPreparationKey> used, released;
    std::vector<std::weak_ptr<const vt::VtPartSnapshot>> snapshots;
    void enrich(VkCommandBuffer, const vt::VtEnrichRequest* requests, size_t count) override {
        for (size_t i = 0; i < count; ++i) {
            used.push_back(requests[i].preparation_key());
            snapshots.push_back(requests[i].part_snapshot);
            CHECK(requests[i].part_snapshot &&
                      requests[i].part() == &requests[i].part_snapshot->context &&
                      requests[i].atlas == requests[i].part()->atlas &&
                      requests[i].part_snapshot->owns_context_inputs(),
                  "VT snapshot: enrichment carries owned CPU inputs");
        }
    }
    void release_preparation(const vt::VtPreparationKey& key) override { released.push_back(key); }
    void invalidate_part(uint64_t) override {}
    uint32_t sample_count() const override { return 1; }
    float max_footprint_meters() const override { return 1000; }
};

inline void run_preparation_owners(matter::VulkanDevice& vulkan) {
    Budgets budgets;
    matter::vt_residency_budgets().enrich_per_frame = 4;
    Frames frames(vulkan);
    vt::VtResidency residency;
    std::string error;
    CHECK(frames.valid() && residency.init(vulkan, error), "VT preparation: runtime initialized");
    if (!frames.valid() || !residency.available()) return;
    auto writer = vt::make_vt_stub_filler(vulkan, 64, error);
    CHECK(writer != nullptr, "VT preparation: page writer initialized");
    if (!writer) return;
    auto observed = std::make_unique<RetryWriter>(std::move(writer));
    auto* fills = observed.get();
    std::vector<std::shared_ptr<const vt::VtPartSnapshot>> snapshots;
    fills->on_request = [&](const vt::VtFillRequest& request) {
        CHECK(request.part_snapshot && request.part() == &request.part_snapshot->context &&
                  request.atlas == request.part()->atlas && request.part_snapshot->owns_context_inputs(),
              "VT snapshot: fill carries the same immutable context it retains");
        if (request.part_snapshot) snapshots.push_back(request.part_snapshot);
    };
    residency.set_filler(std::move(observed));
    auto enrichment = std::make_unique<EnrichmentIdentityProbe>();
    auto* enrich = enrichment.get();
    residency.set_enricher(std::move(enrichment));
    chart_atlas::ChartAtlasRung atlas;
    atlas.atlas_w = atlas.atlas_h = 128;
    atlas.charts.resize(1);
    atlas.charts[0].rect_w = atlas.charts[0].rect_h = 128;
    atlas.charts[0].texels_per_meter = 4;
    atlas.charts[0].tri_count = 1;
    atlas.tri_order = {0};
    float positions[] = {0,0,0, 1,0,0, 0,1,0};
    float normals[] = {0,0,1, 0,0,1, 0,0,1};
    float uvs[] = {0,0, 1,0, 0,1};
    uint32_t indices[] = {0,1,2};
    uint32_t material_ids[] = {7,8,9};
    uint8_t tint[] = {2,3,4,255, 5,6,7,255, 8,9,10,255};
    float material_table[] = {.1f,.2f,.3f,.4f};
    uint8_t source_weights[] = {25,230, 125,130, 225,30};
    uint32_t source_palette[] = {7,8};
    uint16_t source_lanes[] = {0x3c00,0x3800, 0x4000,0x3c00, 0x4200,0x4000};
    std::string source_tape = "snapshot-a";
    vt::VtPartContext context;
    context.variant_hash = 0xA110;
    context.positions = positions; context.vertex_count = 3;
    context.indices = indices; context.triangle_count = 1;
    context.normals = normals; context.surface_uvs = uvs;
    context.material_ids = material_ids; context.tint_rgba = tint;
    context.material_table = material_table; context.material_count = 1; context.material_stride = 4;
    context.surface_weights = source_weights; context.surface_materials = source_palette;
    context.surface_material_count = 2; context.surface_tape_hash = 17;
    context.surface_tape_text = source_tape.c_str();
    context.surface_lanes = source_lanes; context.surface_lane_count = 2;
    std::vector<vt::GpuChart> golden_charts;
    std::vector<vt::GpuTri> golden_weights, golden_lanes;
    CHECK(vt::vt_build_chart_gpu_streams(atlas, context, golden_charts, golden_weights) &&
              vt::vt_build_chart_gpu_streams(atlas, context, golden_charts, golden_lanes,
                  source_lanes, 2),
          "VT snapshot: independent caller inputs produce both reference packing modes");
    const uint32_t old_owner = residency.register_variant(context.variant_hash, 0, atlas, context);
    CHECK(old_owner && residency.register_variant(context.variant_hash, 1, atlas, context) == old_owner,
          "VT preparation: compatible aliases share one owner");
    CHECK(frames.next(residency, 1) && frames.next(residency, 2), "VT preparation: old owner submitted");
    CHECK(fills->preparation_uses.size() == 1 && enrich->used.size() == 1,
          "VT preparation: fill and enrichment exercised");
    if (fills->preparation_uses.size() != 1 || enrich->used.size() != 1) return;
    const auto old_key = fills->preparation_uses.begin()->first;
    CHECK(old_key.owner_key && old_key.owner_generation && enrich->used[0] == old_key,
          "VT preparation: fill and enrichment use the same live owner identity");
    auto changed_atlas = atlas;
    changed_atlas.charts[0].origin[0] = 2;
    const uint32_t new_owner = residency.register_variant(context.variant_hash, 0, changed_atlas, context);
    CHECK(new_owner && new_owner != old_owner && residency.slot_for(context.variant_hash, 1) == old_owner,
          "VT preparation: old and new owners coexist under the same part and canonical rung");
    CHECK(fills->released.empty() && enrich->released.empty(),
          "VT preparation: remaining alias protects the old preparation");
    CHECK(frames.next(residency, 3) && frames.next(residency, 4), "VT preparation: new owner submitted");
    CHECK(fills->preparation_uses.size() == 2 && enrich->used.size() == 2,
          "VT preparation: replacement gets an independent preparation identity");
    if (fills->preparation_uses.size() != 2 || enrich->used.size() != 2) return;
    const auto new_key = enrich->used[1];
    CHECK(new_key.variant_hash == old_key.variant_hash && new_key.rung == old_key.rung &&
              new_key.owner_key != old_key.owner_key && new_key.owner_generation != old_key.owner_generation &&
              fills->preparation_uses.count(new_key) == 1,
          "VT preparation: parameterization and generation distinguish canonical-rung overlap");
    const uint8_t weights[] = {255,255,255};
    const uint32_t palette[] = {3};
    bool changed = false;
    CHECK(residency.update_variant_surface(context.variant_hash, 1, weights, 3, palette, 1, 19,
              nullptr, nullptr, 0, &changed) && changed &&
              fills->surface_updates.size() == 1 && fills->surface_updates[0] == old_key,
          "VT preparation: alias edit targets its canonical owner only");
    CHECK(residency.update_variant_surface(context.variant_hash, 1, weights, 3, palette, 1, 19,
              nullptr, nullptr, 0, &changed) && !changed && fills->surface_updates.size() == 1,
          "VT preparation: identical surface publication keeps cached preparation");
    residency.invalidate_owners({old_owner}, vt::VtInvalidationReason::Surface);
    CHECK(frames.next(residency, 5) && frames.next(residency, 6),
          "VT snapshot: surface replacement recorded through the production request seam");
    CHECK(snapshots.size() == 3, "VT snapshot: captured original, other owner and edited inputs");
    if (snapshots.size() != 3) return;
    CHECK(snapshots[0]->geometry == snapshots[2]->geometry &&
              snapshots[0]->surface != snapshots[2]->surface &&
              snapshots[2]->context.surface_material_count == 1 &&
              snapshots[2]->context.surface_weights[0] == 255 &&
              snapshots[2]->context.surface_materials[0] == 3 &&
              !snapshots[2]->context.surface_tape_text && !snapshots[2]->context.surface_lanes,
          "VT snapshot: surface edits share geometry and replace only classification inputs");
    std::vector<std::weak_ptr<const vt::VtPartSnapshot>> weak(snapshots.begin(), snapshots.end());
    std::promise<void> proceed;
    bool worker_ok = false;
    std::thread worker([snapshot = snapshots[0], ready = proceed.get_future(), &worker_ok,
                        &golden_charts, &golden_weights, &golden_lanes]() mutable {
        ready.wait();
        const auto& ctx = snapshot->context;
        std::vector<vt::GpuChart> charts;
        std::vector<vt::GpuTri> weights, lanes;
        const auto same = [](const auto& a, const auto& b) {
            return a.size() == b.size() && (a.empty() ||
                std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0);
        };
        worker_ok = snapshot->owns_context_inputs() &&
            vt::vt_build_chart_gpu_streams(*ctx.atlas, ctx, charts, weights) &&
            same(charts, golden_charts) && same(weights, golden_weights) &&
            vt::vt_build_chart_gpu_streams(*ctx.atlas, ctx, charts, lanes,
                ctx.surface_lanes, ctx.surface_lane_count) &&
            same(charts, golden_charts) && same(lanes, golden_lanes) &&
            ctx.surface_uvs[2] == 1 && ctx.tint_rgba[0] == 2 &&
            ctx.material_table[0] == .1f && ctx.surface_materials[0] == 7 &&
            std::string(ctx.surface_tape_text) == "snapshot-a" &&
            ctx.surface_tape_hash == vt::vt_page_content_salt(17, vt::vt_tape_gpu_enabled() ? 3u : 2u);
    });
    // Overwrite the caller's arrays while the delayed job owns the original
    // snapshot. Both residency owners are then removed before the job reads.
    positions[0] = 99; normals[0] = 1; uvs[2] = 0; indices[0] = 2;
    material_ids[0] = 1; tint[0] = 0; material_table[0] = 1;
    source_weights[0] = 0; source_palette[0] = 1; source_lanes[0] = 0;
    source_tape.assign(256, 'z');
    residency.release_variant(context.variant_hash, 1);
    CHECK(fills->released.size() == 1 && enrich->released.size() == 1 &&
              fills->released[0] == old_key && enrich->released[0] == old_key &&
              residency.slot_for(context.variant_hash, 0) == new_owner,
          "VT preparation: last old alias releases exactly the old owner lifetime");
    residency.release_variant(context.variant_hash, 1);
    CHECK(fills->released.size() == 1 && enrich->released.size() == 1,
          "VT preparation: stale alias release is harmless");
    residency.release_variant(context.variant_hash);
    CHECK(fills->released.size() == 2 && enrich->released.size() == 2 &&
              fills->released[1] == new_key && enrich->released[1] == new_key,
          "VT preparation: part release reaches each producer exactly once per owner");
    snapshots.clear();
    CHECK(!weak[0].expired() && weak[1].expired() && weak[2].expired(),
          "VT snapshot: only the explicit worker retains inputs after owner release");
    proceed.set_value();
    worker.join();
    CHECK(worker_ok, "VT snapshot: delayed worker retains exact geometry, weights, lanes and optional inputs");
    CHECK(weak[0].expired() &&
              std::all_of(enrich->snapshots.begin(), enrich->snapshots.end(),
                  [](const auto& snapshot) { return snapshot.expired(); }),
          "VT snapshot: completion releases inputs without waiting for another frame");
}

inline void run_finite_source_edits(matter::VulkanDevice& vulkan) {
    Budgets budgets;Frames frames(vulkan);vt::VtResidency residency;std::string error;
    CHECK(frames.valid() && residency.init(vulkan,error),"VT finite edit: runtime initialized");
    if (!frames.valid() || !residency.available()) return;
    auto writer=vt::make_vt_stub_filler(vulkan,64,error);
    CHECK(writer!=nullptr,"VT finite edit: page writer initialized");if (!writer) return;
    auto observed=std::make_unique<RetryWriter>(std::move(writer));auto *fills=observed.get();
    std::shared_ptr<const vt::VtPartSnapshot> latest;
    fills->on_request=[&](const vt::VtFillRequest &request){latest=request.part_snapshot;};
    residency.set_filler(std::move(observed));
    chart_atlas::ChartAtlasRung atlas;atlas.atlas_w=atlas.atlas_h=64;
    atlas.charts.resize(1);atlas.charts[0].rect_w=atlas.charts[0].rect_h=64;
    atlas.charts[0].texels_per_meter=32;atlas.charts[0].tri_count=1;atlas.tri_order={0};
    const float positions[]={0,0,0, 1,0,0, 0,1,0};
    const uint32_t indices[]={0,1,2},materials[]={1,1,1},palette[]={1};
    const uint8_t weights[]={255,255,255};
    vt::VtPartContext ctx;ctx.variant_hash=0xA120;ctx.positions=positions;ctx.vertex_count=3;
    ctx.indices=indices;ctx.triangle_count=1;ctx.material_ids=materials;
    ctx.surface_weights=weights;ctx.surface_materials=palette;ctx.surface_material_count=1;
    ctx.surface_tape_text=vt_finite_test::base();ctx.surface_tape_hash=1;
    const auto owner=residency.register_variant(ctx.variant_hash,0,atlas,ctx);
    CHECK(owner!=vt::kVtNoSlot && frames.next(residency,1) && frames.next(residency,2),
          "VT finite edit: initial receiver published");
    if (!latest) return;
    const auto original=latest;const auto initial_bytes=residency.stats().mesh_bytes;
    vt::VtFiniteSourceBinding binding;binding.stamp=vt_finite_test::source();
    std::shared_ptr<const vt::VtFiniteSources> catalog;
    CHECK(vt::vt_make_finite_sources({binding},catalog,error),error.c_str());
    uint32_t ids[]={1,1,1};bool changed=true;
    CHECK(!residency.update_variant_finite_sources(ctx.variant_hash,0,catalog,ids,2,&changed) && !changed,
          "VT finite edit: incomplete selectors rejected without publication");
    ids[1]=2;
    CHECK(!residency.update_variant_finite_sources(ctx.variant_hash,0,catalog,ids,3,&changed) && !changed,
          "VT finite edit: out-of-range source IDs rejected");
    ids[1]=1;
    CHECK(residency.update_variant_finite_sources(ctx.variant_hash,0,catalog,ids,3,&changed) && changed &&
          fills->surface_updates.size()==1 && residency.stats().mesh_bytes>initial_bytes,
          "VT finite edit: source publication invalidates preparation and accounts memory");
    ids[0]=0;const uint32_t expected_ids[]={1,1,1};
    CHECK(residency.update_variant_finite_sources(ctx.variant_hash,0,catalog,expected_ids,3,&changed) && !changed &&
          fills->surface_updates.size()==1,"VT finite edit: copied selectors and identical content retain preparation");
    residency.invalidate_owners({owner},vt::VtInvalidationReason::Surface);
    CHECK(frames.next(residency,3) && frames.next(residency,4),"VT finite edit: source replacement submitted");
    const auto first=latest;
    CHECK(first && first->owns_context_inputs() && first->context.finite_source_ids[0]==1 &&
          first->context.finite_sources==catalog && first->geometry==original->geometry &&
          !original->context.finite_sources,"VT finite edit: replacement owns new sources and shares immutable geometry");
    binding.stamp=vt_finite_test::source(true);
    CHECK(vt::vt_make_finite_sources({binding},catalog,error),error.c_str());
    CHECK(residency.update_variant_finite_sources(ctx.variant_hash,0,catalog,expected_ids,3,&changed) && changed,
          "VT finite edit: changed source accepted");
    residency.invalidate_owners({owner},vt::VtInvalidationReason::Surface);
    CHECK(frames.next(residency,5) && frames.next(residency,6),"VT finite edit: second source submitted");
    CHECK(latest && latest->context.finite_sources==catalog &&
          first->context.finite_sources->height_max_m==.02f && catalog->height_max_m==.04f,
          "VT finite edit: delayed readers retain old height and pixels");
    CHECK(residency.update_variant_finite_sources(ctx.variant_hash,0,{},nullptr,0,&changed) && changed &&
          residency.stats().mesh_bytes==initial_bytes,"VT finite edit: removal restores receiver memory accounting");
    residency.invalidate_owners({owner},vt::VtInvalidationReason::Surface);
    CHECK(frames.next(residency,7) && frames.next(residency,8) && latest &&
          !latest->context.finite_sources && !latest->context.finite_source_ids,
          "VT finite edit: removal publishes the original direct base");
}

inline void run(matter::VulkanDevice& vulkan) {
    run_finite_source_edits(vulkan);
    run_preparation_owners(vulkan);
    const uint32_t errors_before = vulkan.validation_error_count();
    Budgets guard;
    Frames frames(vulkan);
    CHECK(frames.valid(), "VT queue: one-shot command resources initialized");
    if (!frames.valid()) return;
    for (const uint32_t scenario : {0u, 1u, 2u}) {
        const bool release_first = scenario == 1;
        const bool detail_pressure = scenario == 2;
        auto& budgets = matter::vt_residency_budgets();
        budgets.fills_per_frame = detail_pressure ? 64u : 8u;
        budgets.queue_cap = 256;
        vt::VtResidency residency;
        std::string error;
        CHECK(residency.init(vulkan, error), "VT queue: residency initialized");
        if (!residency.available()) return;
        auto writer = vt::make_vt_stub_filler(vulkan, 64, error);
        CHECK(writer != nullptr, "VT queue: real page writer initialized");
        if (!writer) return;
        auto retry_writer = std::make_unique<RetryWriter>(std::move(writer));
        RetryWriter* observed_writer = retry_writer.get();
        residency.set_filler(std::move(retry_writer));
        chart_atlas::ChartAtlasRung atlas;
        atlas.atlas_w = atlas.atlas_h = detail_pressure ? 512u : 128u;
        atlas.tri_order = {0};
        atlas.charts.resize(1);
        atlas.charts[0].rect_w = atlas.charts[0].rect_h = atlas.atlas_w;
        atlas.charts[0].texels_per_meter = 4;
        atlas.charts[0].tri_count = 1;
        const float positions[] = {0,0,0, 1,0,0, 0,1,0};
        const float normals[] = {0,0,1, 0,0,1, 0,0,1};
        const float uvs[] = {0,0, 1,0, 0,1};
        const uint32_t material_ids[] = {0,0,0};
        const uint32_t indices[] = {0,1,2};
        const float material[] = {.8f,.7f,.6f,.5f};
        vt::VtPartContext context{};
        context.positions = positions;
        context.normals = normals;
        context.surface_uvs = uvs;
        context.material_ids = material_ids;
        context.indices = indices;
        context.vertex_count = 3;
        context.triangle_count = 1;
        context.material_table = material;
        context.material_count = 1;
        context.material_stride = 4;
        std::vector<uint32_t> slots;
        for (uint64_t i = 0; i < 300; ++i) {
            context.variant_hash = 0x9000u + i;
            const uint32_t slot = residency.register_variant(context.variant_hash, 0, atlas, context);
            CHECK(slot != vt::kVtNoSlot, "VT queue: all 300 variants admitted");
            if (slot == vt::kVtNoSlot) return;
            slots.push_back(slot);
        }
        CHECK(residency.queued_requests_consistent_for_test(), "VT queue: registration indices consistent");
        vt::VtVariantLayout layout;
        CHECK(vt::vt_build_layout(atlas.atlas_w, atlas.atlas_h, layout), "VT queue: fixture layout valid");
        const uint32_t tail_mip = layout.mip_count - 1;
        std::vector<vt::VtFeedbackRequest> detail_requests;
        if (detail_pressure) {
            for (uint32_t slot : slots)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x)
                        detail_requests.push_back({slot - 1u, 0, x, y});
        }
        if (release_first) {
            residency.release_variant(0x9000u);
            const bool consistent = residency.queued_requests_consistent_for_test();
            CHECK(consistent, "VT queue: removing an early entry reindexes every surviving lookup");
            // The pre-fix failure must not then dereference a stale index.
            if (!consistent) continue;
            residency.invalidate_all_content();
            CHECK(residency.queued_requests_consistent_for_test(), "VT queue: forced refresh keeps owners and slots paired");
        }
        for (uint64_t frame = 1; frame <= 30; ++frame) {
            if (detail_pressure) {
                residency.inject_feedback_for_test(detail_requests.data(), detail_requests.size());
                if (frame == 2) budgets.queue_cap = 16;
            }
            const bool ok = frames.next(residency, frame);
            CHECK(ok, "VT queue: real frame submitted and completed");
            if (!ok) return;
            if (detail_pressure && frame == 1) {
                uint32_t tails_written = 0;
                for (uint64_t i = 0; i < 300; ++i)
                    tails_written += observed_writer->written(0x9000u + i, tail_mip);
                CHECK(tails_written != 0, "VT queue: detail batch limit cannot consume all mandatory service");
            }
            CHECK(residency.stats().queue_depth <= 300u + budgets.queue_cap,
                  "VT queue: pending state bounded by owners plus detail cap");
            CHECK(residency.stats().queue_depth == residency.stats().mandatory_queue_depth +
                  residency.stats().detail_queue_depth,
                  "VT queue: mandatory/detail counters account for all pending requests");
        }
        uint32_t active = 0;
        for (size_t i = release_first ? 1u : 0u; i < slots.size(); ++i)
            if (residency.slot_active(slots[i])) ++active;
        const uint32_t expected = release_first ? 299u : 300u;
        const auto stats = residency.stats();
        std::printf("VT queue: scenario=%u active=%u/%u dropped=%llu failed=%llu queued=%u\n",
                    scenario, active, expected,
                    static_cast<unsigned long long>(stats.requests_dropped_total),
                    static_cast<unsigned long long>(stats.fills_failed_total), stats.queue_depth);
        CHECK(active == expected, "VT queue: every live mandatory tail eventually activates");
        CHECK(detail_pressure ? stats.requests_dropped_total > 0 : stats.requests_dropped_total == 0,
              "VT queue: capacity drops affect excess feedback only");
        CHECK(stats.fills_failed_total == 2, "VT queue: both injected producer failures are retried");
        if (!detail_pressure)
            CHECK(stats.queue_depth == 0, "VT queue: mandatory backlog drains completely");
        CHECK(residency.queued_requests_consistent_for_test(), "VT queue: drained indices consistent");
        if (release_first) {
            context.variant_hash = 0xB000u;
            const uint32_t reused = residency.register_variant(context.variant_hash, 0, atlas, context);
            CHECK(reused == slots.front(), "VT queue: fixture exercises a retired owner index reused by a new owner");
            CHECK(!residency.slot_active(reused), "VT queue: reused owner does not inherit old activation");
            for (uint64_t frame = 31; frame <= 33; ++frame) {
                const bool ok = frames.next(residency, frame);
                CHECK(ok, "VT queue: reused owner frame submitted and completed");
                if (!ok) return;
            }
            CHECK(residency.slot_active(reused), "VT queue: reused owner activates after its own successful fill");
            CHECK(observed_writer->written(0x9000u, tail_mip) == 0 &&
                  observed_writer->written(0xB000u, tail_mip) == 1,
                  "VT queue: cancelled owner work is not dispatched after index reuse");
        }
        if (detail_pressure) {
            CHECK(stats.pool_used == stats.pool_capacity - stats.replacement_reserve_pages,
                  "VT queue: pressure fills the usable pool while preserving candidate reserve");
            for (uint64_t i = 0; i < 300; ++i)
                CHECK(observed_writer->written(0x9000u + i, tail_mip) == 1,
                      "VT queue: each initial tail completes once under sustained detail pressure");
            observed_writer->refuse_next_two_tails();
            residency.invalidate_all_content();
            residency.invalidate_all_content();
            for (uint64_t frame = 31; frame <= 60; ++frame) {
                residency.inject_feedback_for_test(detail_requests.data(), detail_requests.size());
                const bool ok = frames.next(residency, frame);
                CHECK(ok, "VT queue: active refresh frame submitted and completed");
                if (!ok) return;
                for (uint32_t slot : slots)
                    CHECK(residency.slot_active(slot), "VT queue: active coarse fallback survives refresh backlog");
            }
            for (uint64_t i = 0; i < 300; ++i)
                CHECK(observed_writer->written(0x9000u + i, tail_mip) == 2,
                      "VT queue: duplicate active refreshes coalesce and survive pressure");
            CHECK(residency.queued_requests_consistent_for_test(), "VT queue: pressure refresh indices consistent");
            CHECK(residency.stats().fills_failed_total == 4,
                  "VT queue: active refresh retries both failures while keeping old coverage active");
            CHECK(residency.stats().dirty_pages == 0 && residency.stats().pages_dropped_total == 0,
                  "VT queue: full-pool refresh completes without destructively dropping resident pages");
            observed_writer->preparation_ready = false;
            const auto before_defer = residency.stats();
            for (uint64_t frame = 70; frame < 74; ++frame) {
                residency.inject_feedback_for_test(detail_requests.data(), detail_requests.size());
                CHECK(frames.next(residency, frame), "VT preparation: deferred full-pool frame completes");
            }
            CHECK(residency.stats().fills_total == before_defer.fills_total &&
                      residency.stats().fills_failed_total == before_defer.fills_failed_total &&
                      residency.stats().evictions_total == before_defer.evictions_total &&
                      residency.stats().pool_used == before_defer.pool_used,
                  "VT preparation: deferral neither evicts resident pages nor counts as a failed fill");
            observed_writer->preparation_ready = true;
            CHECK(frames.next(residency, 90) && residency.stats().fills_total > before_defer.fills_total &&
                      residency.stats().evictions_total > before_defer.evictions_total,
                  "VT preparation: retained demand resumes and evicts only after readiness");
        }
    }
    CHECK(vulkan.validation_error_count() == errors_before, "VT queue: zero new Vulkan validation errors");
}
} // namespace vt_queue_tests
