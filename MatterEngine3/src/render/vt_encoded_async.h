#pragma once
#include "vt_encoded_store.h"
#include "vt_encoded_identity.h"
#include "matter/event/channel.h"
#include <atomic>
#include <thread>

namespace vt::encoded {
// Render-thread submissions, one disk worker, immutable completion handoff.
// Jobs and retained write payloads are bounded separately from the caller's
// preallocated read bank. Shutdown joins only during session teardown.
class AsyncStore {
public:
    struct Limits { size_t jobs = 32, write_bytes = 32u << 20; };
    struct Completion { Store::Read read; Key key; bool written = false; std::string error; };
    class Ticket {
    public:
        const Completion* poll() const {
            return done_.load(std::memory_order_acquire) ? &result_ : nullptr;
        }
        void cancel() { cancelled_.store(true, std::memory_order_relaxed); }
    private:
        friend class AsyncStore;
        Completion result_;
        std::atomic<bool> done_{false}, cancelled_{false};
    };
    using Handle = std::shared_ptr<Ticket>;
    struct Stats { size_t jobs = 0, write_bytes = 0; };
    AsyncStore(asset_store::PageCacheConfig config, bool writable, Limits limits)
        : limits_(limits), queue_({limits.jobs, matter::evt::OnFull::RejectNewest}),
          budget_(std::make_shared<Budget>()),
          worker_([this, config=std::move(config), writable]() mutable { work(std::move(config), writable); }) {}
    ~AsyncStore() { shutdown(); }
    AsyncStore(const AsyncStore&) = delete;
    AsyncStore& operator=(const AsyncStore&) = delete;
    Handle read(Key key) { return submit(key, {}); }
    // Hash immutable mesh inputs on the worker, then perform the page lookup.
    // Completion returns the resolved persistent key even on an ordinary miss,
    // so the eventual bake can be captured under that exact same identity.
    Handle read_receiver(std::shared_ptr<const VtPartSnapshot> receiver,
                         asset_store::BlobHash inputs, Key page) {
        if (!receiver || !inputs.valid()) return {};
        return submit(page, {}, std::move(receiver), inputs);
    }
    // Immutable capture batch. Rejection leaves the caller's payload untouched
    // for retry; queued writes never borrow a mapped GPU readback buffer.
    Handle write(std::shared_ptr<const std::vector<Page>> pages, size_t count = SIZE_MAX) {
        if (!pages) return {};
        if (count == SIZE_MAX) count = pages->size();
        if (!count || count > pages->size() || count > kMaxPages) return {};
        return submit({}, std::move(pages), {}, {}, count);
    }
    Stats stats() const {
        return {budget_->jobs.load(std::memory_order_relaxed), budget_->bytes.load(std::memory_order_relaxed)};
    }
    void shutdown() {
        if (stopped_.exchange(true)) return;
        queue_.shut_down();
        if (worker_.joinable()) worker_.join();
    }
private:
    struct Budget { std::atomic<size_t> jobs{0}, bytes{0}; };
    struct Job {
        std::shared_ptr<Budget> budget;
        size_t bytes = 0;
        Handle ticket;
        Key key;
        size_t page_count = SIZE_MAX;
        std::shared_ptr<const VtPartSnapshot> receiver;
        asset_store::BlobHash inputs;
        std::shared_ptr<const std::vector<Page>> pages;
        ~Job() {
            if (ticket && !ticket->done_.load(std::memory_order_acquire)) {
                ticket->result_.read.status = asset_store::PageStatus::Cancelled;
                ticket->done_.store(true, std::memory_order_release);
            }
            pages.reset();
            if (budget) {
                budget->bytes.fetch_sub(bytes, std::memory_order_relaxed);
                budget->jobs.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    };
    Handle submit(Key key, std::shared_ptr<const std::vector<Page>> pages,
                  std::shared_ptr<const VtPartSnapshot> receiver = {}, asset_store::BlobHash inputs = {}, size_t page_count = SIZE_MAX) {
        if (stopped_.load(std::memory_order_relaxed) || !limits_.jobs) return {};
        size_t bytes = 0;
        if (pages) {
            if (pages->capacity() > limits_.write_bytes/sizeof(Page)) return {};
            bytes = pages->capacity()*sizeof(Page);
            for (const auto& page : *pages) {
                if (page.pixels.capacity() > limits_.write_bytes-bytes) return {};
                bytes += page.pixels.capacity();
            }
        }
        if (budget_->jobs.load(std::memory_order_relaxed) >= limits_.jobs ||
            bytes > limits_.write_bytes-std::min(limits_.write_bytes, budget_->bytes.load(std::memory_order_relaxed))) return {};
        auto job = std::make_shared<Job>();
        job->ticket = std::make_shared<Ticket>(); job->key = key; job->pages = std::move(pages);
        job->receiver = std::move(receiver); job->inputs = inputs; job->page_count = page_count;
        job->bytes = bytes;
        budget_->jobs.fetch_add(1, std::memory_order_relaxed);
        budget_->bytes.fetch_add(bytes, std::memory_order_relaxed);
        job->budget = budget_;
        const auto ticket = job->ticket;
        if (queue_.push(std::move(job)) != matter::evt::PushResult::Queued) return {};
        return ticket;
    }
    void work(asset_store::PageCacheConfig config, bool writable) {
        std::string error;
        std::unique_ptr<Store> store;
        try { store = Store::open(std::move(config), writable, error); }
        catch (const std::exception& e) { error = e.what(); }
        // Weak ownership avoids pinning evicted terrain. The bounded digest
        // cache amortizes fingerprinting over all pages of a receiver snapshot.
        using Weak = std::weak_ptr<const VtPartSnapshot>;
        std::map<const VtPartSnapshot*, std::pair<Weak, asset_store::BlobHash>> receivers;
        std::shared_ptr<Job> job;
        while (queue_.wait_pop(job) == matter::evt::WaitResult::Item) {
            const auto cancelled = [&] {
                return stopped_.load(std::memory_order_relaxed) || job->ticket->cancelled_.load(std::memory_order_relaxed);
            };
            if (!cancelled()) {
                auto& result = job->ticket->result_;
                try {
                    result.key = job->key;
                    if (job->receiver) {
                        const auto* address = job->receiver.get();
                        auto found = receivers.find(address);
                        if (found != receivers.end() && found->second.first.expired()) {
                            receivers.erase(found); found = receivers.end();
                        }
                        if (found == receivers.end()) {
                            if (receivers.size() >= 1024) {
                                for (auto it = receivers.begin(); it != receivers.end();)
                                    if (it->second.first.expired()) it = receivers.erase(it); else ++it;
                                if (receivers.size() >= 1024) receivers.erase(receivers.begin());
                            }
                            found = receivers.emplace(address, std::make_pair(Weak(job->receiver), receiver_key(*job->receiver))).first;
                        }
                        result.key.content = page_content_key(found->second.second, job->inputs);
                    }
                    if (!store) { result.error = error; result.read.status = asset_store::PageStatus::IoError; }
                    else if (job->pages) {
                        result.written = store->write(*job->pages, result.error, job->page_count);
                        result.read.status = result.written ? asset_store::PageStatus::Ok : asset_store::PageStatus::IoError;
                    } else result.read = store->read(result.key);
                } catch (const std::exception& e) {
                    result.error = e.what(); result.read.status = asset_store::PageStatus::IoError;
                }
                // Cancellation suppresses delivery, not an already completed
                // immutable disk commit. Such bytes are harmless reusable cache.
                if (cancelled()) {
                    result = {}; result.read.status = asset_store::PageStatus::Cancelled;
                }
                job->ticket->done_.store(true, std::memory_order_release);
            }
            job.reset(); // release payload and admission charge before parking
        }
    }
    Limits limits_;
    matter::evt::Channel<std::shared_ptr<Job>> queue_;
    std::shared_ptr<Budget> budget_;
    std::atomic<bool> stopped_{false};
    std::thread worker_;
};
} // namespace vt::encoded
