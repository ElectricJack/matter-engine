#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
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
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_); ++generation_;
        outstanding_-=pending_.size()+ready_.size()+completed_.size();
        pending_.clear(); ready_.clear(); completed_.clear();
    }
    void counts(size_t& reads, size_t& preparing, size_t& done) const {
        std::lock_guard<std::mutex> lock(mutex_);
        reads=pending_.size(); preparing=ready_.size(); done=completed_.size();
    }
private:
    struct Item { T value; uint64_t generation; };
    void io_loop() {
        for (;;) {
            std::vector<T> batch; uint64_t generation;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock,[&]{return stopping_ || !pending_.empty();});
                if(stopping_)return;
                generation=pending_.front().generation;
                const auto count=std::min(batch_,pending_.size()); batch.reserve(count);
                for(size_t i=0;i<count;++i){batch.push_back(std::move(pending_.front().value));pending_.pop_front();}
            }
            try { read_(batch); }
            catch(const std::exception& e){for(auto& value:batch)failure_(value,e.what());}
            catch(...){for(auto& value:batch)failure_(value,"unknown I/O exception");}
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(stopping_ || generation!=generation_)outstanding_-=batch.size();
                else for(auto& value:batch)ready_.push_back({std::move(value),generation});
            }
            wake_.notify_all();
        }
    }
    void prepare_loop() {
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock,[&]{return stopping_ || !ready_.empty();});
                if(stopping_)return;
                item=std::move(ready_.front());ready_.pop_front();
            }
            try { prepare_(item.value); }
            catch(const std::exception& e){failure_(item.value,e.what());}
            catch(...){failure_(item.value,"unknown preparation exception");}
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(stopping_ || item.generation!=generation_)--outstanding_;
                else completed_.push_back(std::move(item.value));
            }
        }
    }
    size_t capacity_,batch_,outstanding_=0;
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
