#include "streaming/async_stage_pipeline.h"
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <cstdio>
using namespace std::chrono_literals;
struct Work { int id=0; std::shared_ptr<int> pin; };
template<class Predicate> void until(Predicate done) {
    const auto end=std::chrono::steady_clock::now()+5s;
    while(!done()){assert(std::chrono::steady_clock::now()<end);std::this_thread::yield();}
}
int main() {
    {
        std::promise<void> preparing,release,second_read;
        auto prepared=preparing.get_future(),gate=release.get_future(),second=second_read.get_future();
        streaming::AsyncStagePipeline<Work> pipeline(2,2,
            [&](auto& values){for(auto& value:values)if(value.id==2)second_read.set_value();},
            [&](Work& value){if(value.id==1){preparing.set_value();gate.wait();}},
            [](Work& value,const char*){value.id=-value.id;});
        auto pin=std::make_shared<int>(42);std::weak_ptr<int> weak=pin;
        assert(pipeline.submit({1,pin}));pin.reset();
        assert(prepared.wait_for(5s)==std::future_status::ready);
        assert(pipeline.submit({2,{}}));
        assert(second.wait_for(5s)==std::future_status::ready); // I/O overlaps blocked preparation.
        assert(!pipeline.submit({3,{}})); // Active and undrained work consume capacity.
        pipeline.cancel();
        assert(!weak.expired()); // Active callback still owns its slot/pin.
        release.set_value();
        until([&]{return pipeline.available()==2;});
        until([&]{return weak.expired();});
        assert(pipeline.take().empty()); // Old generation never publishes.
        assert(pipeline.submit({3,{}}));
        std::deque<Work> done;until([&]{done=pipeline.take();return !done.empty();});
        assert(done.size()==1 && done[0].id==3);
    }
    {
        streaming::AsyncStagePipeline<Work> pipeline(4,4,
            [](auto&){throw std::runtime_error("injected read failure");},
            [](Work&){},[](Work& value,const char*){value.id=-value.id;});
        assert(pipeline.submit({4,{}}));std::deque<Work> done;
        until([&]{done=pipeline.take();return !done.empty();});
        assert(done[0].id==-4 && pipeline.available()==4);
    }
    {
        std::promise<void> reading,release;auto started=reading.get_future(),gate=release.get_future();
        auto pipeline=std::make_unique<streaming::AsyncStagePipeline<Work>>(1,1,
            [&](auto&){reading.set_value();gate.wait();},[](Work&){},[](Work&,const char*){});
        auto pin=std::make_shared<int>(7);std::weak_ptr<int> weak=pin;
        assert(pipeline->submit({1,pin}));pin.reset();
        assert(started.wait_for(5s)==std::future_status::ready);
        auto shutdown=std::async(std::launch::async,[owned=std::move(pipeline)]()mutable{owned.reset();});
        assert(shutdown.wait_for(10ms)==std::future_status::timeout && !weak.expired());
        release.set_value();assert(shutdown.wait_for(5s)==std::future_status::ready);shutdown.get();
        assert(weak.expired());
    }
    std::puts("ALL PASS: asynchronous overlap, bounded admission, cancellation, failures and shutdown pins");
}
