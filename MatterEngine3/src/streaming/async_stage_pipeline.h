#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace streaming {
// Two bounded background stages. One thread owns I/O handles; preparation can
// overlap the next read without sharing those handles. Capacity counts queued,
// executing and completed items until the publication lane drains them.
// Callbacks execute without the queue mutex. Cancel drops queued generations;
// active callbacks retain their values/resources until they actually return.
template<class T> class AsyncStagePipeline {
public:
    using Read = std::function<void(std::vector<T>&)>;
    using Prepare = std::function<void(T&)>;
    using Failure = std::function<void(T&, const char*)>;
    AsyncStagePipeline(size_t capacity, size_t batch, Read read, Prepare prepare, Failure failure)
        : capacity_(capacity), batch_(batch), read_(std::move(read)), prepare_(std::move(prepare)), failure_(std::move(failure)) {
        if (!capacity || !batch) throw std::invalid_argument("empty async pipeline capacity");
        io_ = std::thread([this] { io_loop(); });
        try { preparation_ = std::thread([this] { prepare_loop(); }); }
        catch (...) { { std::lock_guard<std::mutex> lock(mutex_); stopping_=true; } wake_.notify_all(); io_.join(); throw; }
    }
    ~AsyncStagePipeline() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_=true; }
        wake_.notify_all(); io_.join(); preparation_.join();
    }
    size_t available() const { std::lock_guard<std::mutex> lock(mutex_); return capacity_-outstanding_; }
    bool submit(T value) {
        { std::lock_guard<std::mutex> lock(mutex_);
          if (stopping_ || outstanding_==capacity_) return false;
          pending_.push_back({std::move(value), generation_}); ++outstanding_; }
        wake_.notify_all(); return true;
    }
    std::deque<T> take() {
        std::deque<T> result;
        std::lock_guard<std::mutex> lock(mutex_);
        result.swap(completed_); outstanding_-=result.size(); return result;
    }
    // Queue allocation/move failures cannot always produce a T completion.
    // Owners must reconcile pending state when this returns a nonzero count.
    size_t take_dropped() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto count=dropped_; dropped_=0; return count;
    }
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_); ++generation_;
        outstanding_-=pending_.size()+ready_.size()+completed_.size();
        pending_.clear(); ready_.clear(); completed_.clear();
        dropped_=0;
    }
    void counts(size_t& reads, size_t& preparing, size_t& done) const {
        std::lock_guard<std::mutex> lock(mutex_);
        reads=pending_.size(); preparing=ready_.size(); done=completed_.size();
    }
private:
    struct Item { T value; uint64_t generation; };
    void report_failure(T& value, const char* why) noexcept {
        try { failure_(value, why); }
        catch (...) {
            // Give the owner one chance to record a reporter failure too.
            // An unconditionally throwing reporter must not kill the lane.
            try { failure_(value, "failure callback threw"); }
            catch (...) {}
        }
    }
    void io_loop() {
        for (;;) {
            std::vector<T> batch;
            size_t active = 0; // Items removed from pending but not yet handed off.
            uint64_t generation = 0;
            try {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    wake_.wait(lock,[&]{return stopping_ || !pending_.empty();});
                    if(stopping_)return;
                    generation=pending_.front().generation;
                    const auto count=std::min(batch_,pending_.size());
                    try { batch.reserve(count); }
                    catch (...) {
                        // Drop one request instead of hot-spinning on an
                        // allocation that cannot currently be satisfied.
                        pending_.pop_front(); --outstanding_; ++dropped_; throw;
                    }
                    for(size_t i=0;i<count;++i) {
                        try { batch.push_back(std::move(pending_.front().value)); }
                        catch (...) {
                            // A throwing move may have changed its source.
                            pending_.pop_front(); --outstanding_; ++dropped_; throw;
                        }
                        pending_.pop_front(); ++active;
                    }
                }
                try { read_(batch); }
                catch(const std::exception& e){for(auto& value:batch)report_failure(value,e.what());}
                catch(...){for(auto& value:batch)report_failure(value,"unknown I/O exception");}
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if(stopping_ || generation!=generation_) {
                        outstanding_-=active; active=0;
                    } else for(auto& value:batch) {
                        ready_.push_back({std::move(value),generation}); --active;
                    }
                }
            } catch (...) {
                // Failed queue transfers cannot escape the thread or strand
                // capacity. Already transferred items still belong to ready_.
                std::lock_guard<std::mutex> lock(mutex_);
                outstanding_-=active;
                if(generation==generation_)dropped_+=active;
            }
            wake_.notify_all();
        }
    }
    void prepare_loop() {
        for (;;) {
            std::optional<Item> item;
            bool active = false;
            try {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    wake_.wait(lock,[&]{return stopping_ || !ready_.empty();});
                    if(stopping_)return;
                    try { item.emplace(std::move(ready_.front())); }
                    catch (...) { ready_.pop_front(); --outstanding_; ++dropped_; throw; }
                    ready_.pop_front(); active=true;
                }
                try { prepare_(item->value); }
                catch(const std::exception& e){report_failure(item->value,e.what());}
                catch(...){report_failure(item->value,"unknown preparation exception");}
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if(stopping_ || item->generation!=generation_)--outstanding_;
                    else completed_.push_back(std::move(item->value));
                    active=false;
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                if(active) {
                    --outstanding_;
                    if(item->generation==generation_)++dropped_;
                }
            }
        }
    }
    size_t capacity_,batch_,outstanding_=0,dropped_=0;
    uint64_t generation_=0;
    bool stopping_=false;
    Read read_; Prepare prepare_; Failure failure_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Item> pending_,ready_;
    std::deque<T> completed_;
    std::thread io_,preparation_;
};
}
