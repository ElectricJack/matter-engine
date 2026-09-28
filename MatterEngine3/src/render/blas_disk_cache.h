#pragma once
#include "asset_store.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace viewer {
// Optional device-derived cache. All filesystem calls belong to this worker;
// render-thread calls only enqueue or collect bounded, immutable byte arrays.
class BlasDiskCache {
public:
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
    static constexpr size_t max_blob = 1u << 20;
    BlasDiskCache() : worker_([this] { run(); }) {}
    ~BlasDiskCache() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        wake_.notify_one(); worker_.join();
    }
    uint64_t read(std::string directory, std::string key) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (results_.size() >= 32 || work_.size() >= 64) return 0;
        const auto id = ++next_;
        results_[id] = {};
        work_.push_back({id, std::move(directory), std::move(key), {}});
        wake_.notify_one(); return id;
    }
    // False means pending/backpressure; true means an actual completed lookup.
    // Only a completed lookup with empty bytes is a cache miss.
    bool poll(const std::string& directory, const std::string& key, uint64_t& ticket, Bytes& bytes) {
        if (!ticket) ticket=read(directory,key);
        if (!ticket || !take(ticket,bytes)) return false;
        ticket=0;return true;
    }
    bool take(uint64_t id, Bytes& bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = results_.find(id);
        if (it == results_.end() || !it->second.done) return false;
        bytes = std::move(it->second.bytes); results_.erase(it); return true;
    }
    void cancel(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_); results_.erase(id);
    }
    bool write(std::string directory, std::string key, Bytes bytes) {
        if (!bytes || bytes->size() < 48 || bytes->size() > max_blob) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (work_.size() >= 64 || queued_bytes_ + bytes->size() > 32u << 20) return false;
        queued_bytes_ += bytes->size();
        work_.push_back({0, std::move(directory), std::move(key), std::move(bytes)});
        wake_.notify_one(); return true;
    }
private:
    struct Work { uint64_t id; std::string directory, key; Bytes bytes; };
    struct Result { bool done = false; Bytes bytes; };
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Work> work_;
    std::map<uint64_t, Result> results_;
    bool stopping_ = false;
    uint64_t next_ = 0;
    size_t queued_bytes_ = 0;
    std::thread worker_;
    void run() {
        std::unique_ptr<asset_store::BlobStore> store;
        std::unique_ptr<asset_store::RefTable> refs;
        std::string directory;
        std::unique_ptr<MemArena, decltype(&mem_arena_destroy)> arena(mem_arena_create(max_blob), mem_arena_destroy);
        for (;;) {
            std::vector<Work> batch;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || !work_.empty(); });
                if (work_.empty() && stopping_) break;
                const auto dir = work_.front().directory;
                const bool writing = bool(work_.front().bytes);
                while (!work_.empty() && batch.size() < 32 && work_.front().directory == dir && bool(work_.front().bytes) == writing) {
                    if (work_.front().bytes) queued_bytes_ -= work_.front().bytes->size();
                    batch.push_back(std::move(work_.front())); work_.pop_front();
                }
            }
            if (directory != batch.front().directory) {
                refs.reset(); store.reset(); directory = batch.front().directory;
                asset_store::StoreConfig config; config.dir = directory;
                std::string error;
                store = asset_store::BlobStore::open(config, &error);
                if (store) refs = asset_store::RefTable::open(*store, {}, &error);
                // Lock contention/corruption disables this directory for this
                // session. A miss must never delay rendering or take the cache down.
            }
            if (batch.front().bytes) {
                if (store && refs) {
                    std::vector<asset_store::BlobInput> inputs;
                    for (const auto& item : batch) inputs.push_back({item.bytes->data(), item.bytes->size()});
                    std::vector<asset_store::BlobHash> hashes;
                    // Coalesce appends, then make payload/index durable before refs.
                    if (store->put_batch(inputs, 34u << 20, hashes) == asset_store::Status::Ok && store->flush_index()) {
                        for (size_t i = 0; i < batch.size(); ++i)
                            refs->put(batch[i].key, hashes[i], 1, batch[i].bytes->size());
                        refs->flush();
                    }
                }
                continue;
            }
            std::unique_ptr<asset_store::ReadBatch> reads;
            std::vector<size_t> request_index(batch.size(), SIZE_MAX);
            if (store && refs && arena) {
                reads = std::make_unique<asset_store::ReadBatch>(*store);
                for (size_t i = 0; i < batch.size(); ++i) {
                    asset_store::RefInfo ref;
                    if (refs->lookup(batch[i].key, &ref) && store->size_of(ref.hash) >= 48 && store->size_of(ref.hash) <= max_blob) {
                        request_index[i] = reads->size(); reads->add(ref.hash);
                    }
                }
                mem_arena_reset(arena.get());
                // Account for coalescing holes as well as returned blob bytes.
                if (reads->allocation_bytes() > 40u << 20 || !reads->submit(arena.get())) reads.reset();
            }
            for (size_t i = 0; i < batch.size(); ++i) {
                Bytes bytes;
                if (reads && request_index[i] != SIZE_MAX) {
                    const auto& result = reads->result(request_index[i]);
                    if (result.status == asset_store::Status::Ok)
                        bytes = std::make_shared<const std::vector<uint8_t>>(result.data, result.data + result.size);
                }
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = results_.find(batch[i].id);
                if (it != results_.end()) it->second = {true, std::move(bytes)};
            }
        }
    }
};
} // namespace viewer
