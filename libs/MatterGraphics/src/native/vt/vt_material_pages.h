#pragma once

// Material pixels have different ownership from a receiver's chart coverage
// and POM geometry. This bounded allocator binds receiver page slots to
// independently allocated material slots. Publication and retirement remain
// the residency recorder's responsibility; this class owns no GPU resources.
#include "vt_types.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace vt {

class VtMaterialPages {
    struct ReadOwner { VtMaterialPages* pool = nullptr; };
public:
    struct Key {
        VtMaterialPixelKey pixels;
        uint64_t snapshot = 0;
        std::array<uint32_t, 3> height{};
        bool operator<(const Key& other) const {
            return std::tie(pixels.low, pixels.high, snapshot, height) <
                   std::tie(other.pixels.low, other.pixels.high, other.snapshot, other.height);
        }
        bool shareable() const { return pixels.low || pixels.high; }
    };
    struct Binding { uint32_t slot = UINT32_MAX; bool write = false; };

    // Render-thread lease over immutable encoded pixels, independent of the
    // receiver slot. Replacement must copy on write while a reader exists.
    // Before recording a GPU read, extend retain_until to its reader horizon.
    // Destruction/reset of the allocator detaches old leases safely; it does
    // not itself keep the caller's Vulkan images alive across shutdown.
    class ReadLease {
    public:
        ~ReadLease() {
            if (owner_->pool) owner_->pool->release_reader(slot_);
        }
        uint32_t slot() const { return slot_; }
        void retain_until(uint64_t serial) const {
            if (owner_->pool) {
                auto& record = owner_->pool->records_[slot_];
                record.retire_serial = std::max(record.retire_serial, serial);
            }
        }
        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;
    private:
        friend class VtMaterialPages;
        ReadLease(std::shared_ptr<ReadOwner> owner, uint32_t slot)
            : owner_(std::move(owner)), slot_(slot) {
            auto& pool = *owner_->pool;
            if (!pool.records_[slot_].readers++) ++pool.read_pages_;
        }
        std::shared_ptr<ReadOwner> owner_;
        uint32_t slot_;
    };
    using Read = std::shared_ptr<const ReadLease>;
    VtMaterialPages() = default;
    ~VtMaterialPages() { if (read_owner_) read_owner_->pool = nullptr; }
    VtMaterialPages(const VtMaterialPages&) = delete;
    VtMaterialPages& operator=(const VtMaterialPages&) = delete;

    Read retain_read(uint32_t receiver) {
        const auto slot = this->slot(receiver);
        if (slot == UINT32_MAX) return {};
        return Read(new ReadLease(read_owner_, slot));
    }

    static Key key(VtMaterialPixelKey pixels, uint64_t snapshot, VtPageHeight height) {
        Key result{pixels, snapshot, {0, 0, height.version}};
        std::memcpy(&result.height[0], &height.min_m, sizeof(float));
        std::memcpy(&result.height[1], &height.range_m, sizeof(float));
        return result;
    }

    void reset(uint32_t capacity) {
        if (read_owner_) read_owner_->pool = nullptr;
        read_owner_ = std::make_shared<ReadOwner>(ReadOwner{this});
        records_.assign(capacity, {});
        bindings_.assign(capacity, UINT32_MAX);
        free_.clear(); retired_.clear(); shared_.clear();
        retired_.reserve(capacity);
        used_ = references_ = bound_pages_ = read_pages_ = 0;
        completed_serial_ = 0;
        // Allocate in the opposite order from receiver slots. No caller may
        // accidentally depend on material and coverage addresses being equal.
        for (uint32_t i = 0; i < capacity; ++i) free_.push_back(i);
    }

    // Call only after successful, current-generation production. The old
    // binding survives allocation failure. `write == false` means an identical
    // complete payload already exists; receiver AUX still needs publication.
    Binding publish(uint32_t receiver, const Key& key) {
        if (receiver >= bindings_.size()) return {};
        const uint32_t previous = bindings_[receiver];
        if (key.shareable()) {
            const auto found = shared_.find(key);
            if (found != shared_.end()) {
                if (previous != found->second) {
                    if (!records_[found->second].references) ++bound_pages_;
                    ++records_[found->second].references; ++references_;
                    release_reference(previous, 0);
                    bindings_[receiver] = found->second;
                }
                return {found->second, false};
            }
        }
        uint32_t slot;
        if (previous != UINT32_MAX && records_[previous].references == 1 &&
            records_[previous].readers == 0 &&
            records_[previous].retire_serial <= completed_serial_) {
            slot = previous;
            records_[slot].retire_serial = 0;
            if (records_[slot].key.shareable()) shared_.erase(records_[slot].key);
        } else {
            if (free_.empty()) return {};
            slot = free_.back(); free_.pop_back();
            records_[slot].references = 1; ++used_; ++references_; ++bound_pages_;
            release_reference(previous, 0);
            bindings_[receiver] = slot;
        }
        records_[slot].key = key;
        if (key.shareable()) shared_.emplace(key, slot);
        return {slot, true};
    }

    // A zero serial is legal for queue-ordered replacement/eviction and for
    // never-published candidates. Owner deletion must use the GPU retire serial.
    void release(uint32_t receiver, uint64_t retire_serial = 0) {
        if (receiver >= bindings_.size()) return;
        release_reference(bindings_[receiver], retire_serial);
        bindings_[receiver] = UINT32_MAX;
    }
    void collect(uint64_t serial) {
        completed_serial_ = std::max(completed_serial_, serial);
        size_t keep = 0;
        for (const auto& r : retired_) {
            if (r.second <= serial) free_.push_back(r.first);
            else retired_[keep++] = r;
        }
        retired_.resize(keep);
    }
    uint32_t slot(uint32_t receiver) const {
        return receiver < bindings_.size() ? bindings_[receiver] : UINT32_MAX;
    }
    uint32_t used() const { return used_; }
    uint32_t references() const { return references_; }
    uint32_t shared_references() const { return references_ - bound_pages_; }
    uint32_t read_pages() const { return read_pages_; }

private:
    struct Record { Key key{}; uint32_t references = 0, readers = 0; uint64_t retire_serial = 0; };
    void release_reference(uint32_t slot, uint64_t retire_serial) {
        if (slot == UINT32_MAX) return;
        auto& record = records_[slot];
        record.retire_serial = std::max(record.retire_serial, retire_serial);
        --references_;
        if (--record.references) return;
        --bound_pages_;
        if (record.readers) return;
        release_storage(slot);
    }
    void release_reader(uint32_t slot) {
        auto& record = records_[slot];
        if (--record.readers) return;
        --read_pages_;
        if (!record.references) release_storage(slot);
    }
    void release_storage(uint32_t slot) {
        auto& record = records_[slot];
        if (record.key.shareable()) shared_.erase(record.key);
        if (record.retire_serial > completed_serial_) retired_.push_back({slot, record.retire_serial});
        else free_.push_back(slot);
        record = {};
        --used_;
    }
    std::vector<Record> records_;
    std::vector<uint32_t> bindings_, free_;
    std::vector<std::pair<uint32_t, uint64_t>> retired_;
    std::map<Key, uint32_t> shared_;
    uint32_t used_ = 0, references_ = 0, bound_pages_ = 0, read_pages_ = 0;
    uint64_t completed_serial_ = 0;
    std::shared_ptr<ReadOwner> read_owner_;
};

} // namespace vt
