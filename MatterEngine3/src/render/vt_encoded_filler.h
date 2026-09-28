#pragma once
#include "vt_encoded_async.h"
#include "vt_encoded_upload.h"
#include "vt_encoded_capture.h"
#include "vk_resources.h"
#include <functional>

namespace vt::encoded {
// Cache-first adapter. Frame-slot fences, not CPU frame counts or disk tickets,
// authorize staging reuse. Misses retain the original compositor lifecycle.
// The caller must disable geometry-dependent sampling when requires_geometry is
// false; otherwise composed-height hits deliberately use the normal producer.
class Filler final : public VtPageFiller {
public:
    using Inputs = std::function<asset_store::BlobHash()>;
    struct Stats {
        uint64_t hits = 0, misses = 0, errors = 0, pending = 0;
        uint64_t captured = 0, persisted = 0, capture_rejected = 0, pending_pages = 0;
    };
    static std::unique_ptr<Filler> create(matter::VulkanDevice& device,
        std::unique_ptr<VtPageFiller> producer, asset_store::PageCacheConfig config,
        Inputs inputs, bool requires_geometry, std::string& error, uint32_t pages_per_frame = 32, bool capture = false) {
        if (!producer || !inputs || !config.bank || !pages_per_frame || pages_per_frame > 256 || (capture && pages_per_frame > kMaxPages)) {
            error = "invalid encoded VT filler configuration"; return {};
        }
        auto result = std::unique_ptr<Filler>(new Filler);
        result->producer_ = std::move(producer); result->inputs_ = std::move(inputs);
        result->requires_geometry_ = requires_geometry; result->pages_per_frame_ = pages_per_frame;
        for (auto& buffer : result->staging_) {
            if (!matter::create_buffer(device, size_t(pages_per_frame)*kPixelBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    0, buffer, error) || !matter::map_buffer(buffer, error)) return {};
        }
        result->capture_enabled_ = capture;
        if (capture) for (auto& slot : result->captures_) {
            if (!matter::create_buffer(device, size_t(pages_per_frame)*kPixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT, slot.buffer, error) || !matter::map_buffer(slot.buffer, error)) return {};
            slot.pages.reserve(pages_per_frame);
            slot.payload = std::make_shared<std::vector<Page>>(pages_per_frame);
            for (auto& page : *slot.payload) page.pixels.resize(kPixelBytes);
        }
        config.max_read_bytes = std::max<uint64_t>(config.max_read_bytes, kMaxBytes);
        result->store_ = std::make_unique<AsyncStore>(std::move(config), capture, AsyncStore::Limits{});
        return result;
    }
    void begin_residency_frame(uint64_t serial, uint32_t slot) override {
        producer_->begin_residency_frame(serial, slot);
        frame_ready_ = slot < staging_.size(); frame_serial_ = serial; slot_ = slot; used_ = 0;
        capture_used_ = 0;
        if (frame_ready_ && capture_enabled_) retire_capture(captures_[slot]);
        current_inputs_ = inputs_();
    }
    void begin_preparation_frame() override { producer_->begin_preparation_frame(); }
    PageReadiness probe_page(const VtFillRequest& request) override {
        if (!frame_ready_ || !current_inputs_.valid() || !request.part_snapshot || request.export_points)
            return PageReadiness::NeedsPreparation;
        auto found = entries_.find(address(request));
        if (found == entries_.end()) {
            if (entries_.size() >= 4096) return PageReadiness::NeedsPreparation;
            found = entries_.try_emplace(address(request)).first;
        }
        auto& entry = found->second;
        if (entry.snapshot != request.part_snapshot || entry.inputs != current_inputs_) {
            if (entry.ticket) entry.ticket->cancel();
            entry = {}; entry.snapshot = request.part_snapshot; entry.inputs = current_inputs_;
        }
        if (!entry.ticket)
            entry.ticket = store_->read_receiver(entry.snapshot, entry.inputs,
                {{}, request.rung, request.mip, request.page_x, request.page_y});
        const auto* completion = entry.ticket ? entry.ticket->poll() : nullptr;
        if (!completion) { ++stats_.pending; return PageReadiness::Pending; }
        if (!entry.unsupported && completion->read.status == asset_store::PageStatus::Ok) {
            const auto* page = completion->read.bundle.find(completion->key);
            if (page && (!requires_geometry_ || page->height.version == 0)) {
                // Reserve before residency can evict/acquire a destination.
                // Repeated probes and fill() reuse this exact slice this frame.
                if (entry.slice_frame != frame_serial_) {
                    if (used_ == pages_per_frame_) return PageReadiness::Pending;
                    entry.slice = used_++; entry.slice_frame = frame_serial_;
                }
                return PageReadiness::Ready;
            }
        }
        if (!entry.miss_counted) {
            if (completion->read.status == asset_store::PageStatus::Missing) ++stats_.misses;
            else if (completion->read.status != asset_store::PageStatus::Ok) ++stats_.errors;
            entry.miss_counted = true;
        }
        if (needs_capture(entry)) {
            const auto& slot = captures_[slot_];
            if (!slot.pages.empty() && slot.serial != frame_serial_) return PageReadiness::Pending;
            if (entry.capture_frame != frame_serial_) {
                if (capture_used_ == pages_per_frame_) return PageReadiness::Pending;
                entry.capture_slice = capture_used_++; entry.capture_frame = frame_serial_;
            }
        }
        return PageReadiness::NeedsPreparation;
    }
    bool prepare(const VtPreparationKey& key, const std::shared_ptr<const VtPartSnapshot>& snapshot) override {
        return producer_->prepare(key, snapshot);
    }
    void fill(VkCommandBuffer cmd, const VtFillRequest* requests, size_t count) override {
        std::vector<VtFillRequest> misses; misses.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto& request = requests[i];
            const auto ready = probe_page(request);
            if (ready == PageReadiness::Pending) continue;
            if (ready == PageReadiness::NeedsPreparation) { misses.push_back(request); continue; }
            auto found = entries_.find(address(request));
            const auto* completion = found->second.ticket->poll();
            auto& buffer = staging_[slot_];
            const UploadSpan span{buffer.buffer, buffer.mapped, size_t(buffer.size), size_t(found->second.slice)*kPixelBytes};
            if (record_upload(cmd, completion->read.bundle, completion->key, request, span, requires_geometry_)) {
                ++stats_.hits;
                entries_.erase(found); // bytes copied; the GPU now owns the staging slice, not the disk lease
            } else found->second.unsupported = true; // retry through normal preparation next frame
        }
        if (!misses.empty()) {
            producer_->fill(cmd, misses.data(), misses.size());
            for (const auto& request : misses) {
                if (!request.out_filled || !*request.out_filled) continue;
                auto found = entries_.find(address(request));
                if (found != entries_.end() && needs_capture(found->second) &&
                    found->second.capture_frame == frame_serial_) {
                    auto& slot = captures_[slot_];
                    const auto slice = found->second.capture_slice;
                    const UploadSpan target{slot.buffer.buffer, slot.buffer.mapped, size_t(slot.buffer.size), size_t(slice)*kPixelBytes};
                    if (record_capture(cmd, request, target)) {
                        const auto key = found->second.ticket->poll()->key;
                        const auto height = request.out_height ? *request.out_height : VtPageHeight{};
                        slot.pages.push_back({key, {height.min_m, height.range_m, height.version}, slice});
                        slot.serial = frame_serial_; ++stats_.captured;
                    } else ++stats_.capture_rejected;
                }
                entries_.erase(address(request));
            }
        }
    }
    void release_preparation(const VtPreparationKey& key) override {
        discard(key); producer_->release_preparation(key);
    }
    void invalidate_surface(const VtPreparationKey& key) override {
        discard(key); producer_->invalidate_surface(key);
    }
    Stats stats() const {
        auto result = stats_;
        for (const auto& slot : captures_)
            result.pending_pages += slot.pages.size() + (slot.payload_pending ? slot.payload_count : 0);
        return result;
    }
private:
    using Address = std::tuple<VtPreparationKey, uint16_t, uint16_t, uint16_t>;
    static Address address(const VtFillRequest& r) { return {r.preparation_key(), r.mip, r.page_x, r.page_y}; }
    struct Entry {
        std::shared_ptr<const VtPartSnapshot> snapshot;
        asset_store::BlobHash inputs;
        AsyncStore::Handle ticket;
        bool miss_counted = false, unsupported = false;
        uint64_t slice_frame = UINT64_MAX;
        uint32_t slice = 0, capture_slice = 0;
        uint64_t capture_frame = UINT64_MAX;
    };
    struct Captured { Key key; Height height; uint32_t slice = 0; };
    struct CaptureSlot {
        matter::VkBufferResource buffer;
        std::vector<Captured> pages;
        std::shared_ptr<std::vector<Page>> payload;
        AsyncStore::Handle write;
        size_t payload_count = 0;
        uint64_t serial = 0;
        bool payload_pending = false;
    };
    bool needs_capture(const Entry& entry) const {
        if (!capture_enabled_ || !entry.ticket) return false;
        const auto* done = entry.ticket->poll();
        return done && done->key.content.valid() &&
            (done->read.status == asset_store::PageStatus::Missing || done->read.status == asset_store::PageStatus::Corrupt);
    }
    // Only called after this slot's GPU fence. Payload storage is allocated at
    // initialization and never resized/freed during streaming. Worker ownership
    // delays reuse; it never causes a render-thread wait or unbounded backlog.
    void retire_capture(CaptureSlot& slot) {
        if (slot.write) {
            if (const auto* done = slot.write->poll()) {
                if (done->written) { stats_.persisted += slot.payload_count; slot.payload_pending = false; }
                else ++stats_.errors;
                slot.write.reset();
            }
        }
        if (!slot.write && !slot.payload_pending && slot.payload.use_count() == 1 && !slot.pages.empty()) {
            slot.payload_count = slot.pages.size();
            for (size_t i = 0; i < slot.pages.size(); ++i) {
                const auto& source = slot.pages[i]; auto& page = (*slot.payload)[i];
                page.key = source.key; page.height = source.height;
                std::memcpy(page.pixels.data(), static_cast<const uint8_t*>(slot.buffer.mapped)+size_t(source.slice)*kPixelBytes, kPixelBytes);
            }
            slot.pages.clear(); slot.payload_pending = true;
        }
        if (!slot.write && slot.payload_pending && slot.payload.use_count() == 1)
            slot.write = store_->write(slot.payload, slot.payload_count);
    }
    void discard(const VtPreparationKey& key) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (std::get<0>(it->first) == key) {
                if (it->second.ticket) it->second.ticket->cancel();
                it = entries_.erase(it);
            } else ++it;
        }
    }
    Filler() = default;
    std::unique_ptr<VtPageFiller> producer_;
    Inputs inputs_;
    bool requires_geometry_ = true, frame_ready_ = false, capture_enabled_ = false;
    uint32_t pages_per_frame_ = 0, slot_ = 0, used_ = 0, capture_used_ = 0;
    uint64_t frame_serial_ = 0;
    asset_store::BlobHash current_inputs_;
    std::array<matter::VkBufferResource, 3> staging_;
    std::array<CaptureSlot, 3> captures_;
    std::unique_ptr<AsyncStore> store_;
    std::map<Address, Entry> entries_;
    Stats stats_;
};
} // namespace vt::encoded
