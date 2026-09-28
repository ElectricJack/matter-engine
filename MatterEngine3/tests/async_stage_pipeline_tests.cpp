#include "streaming/async_stage_pipeline.h"
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <cstdio>
#include <cstring>
#include <string>
using namespace std::chrono_literals;
struct Work { int id=0; std::shared_ptr<int> pin; std::string error; };
struct MoveFailure {
    bool armed=false;
    unsigned thrown=0;
};
struct MovingWork {
    int id=0;
    std::shared_ptr<MoveFailure> failure;
    MovingWork()=default;
    MovingWork(int id, std::shared_ptr<MoveFailure> failure) : id(id), failure(std::move(failure)) {}
    MovingWork(MovingWork&& other) { *this=std::move(other); }
    MovingWork& operator=(MovingWork&& other) {
        if(other.failure && other.failure->armed) {
            other.failure->armed=false;
            ++other.failure->thrown;
            throw std::runtime_error("queue transfer failed");
        }
        id=other.id;failure=std::move(other.failure);return *this;
    }
};
template<class Predicate> void until(Predicate done) {
    const auto end=std::chrono::steady_clock::now()+5s;
    while(!done()){assert(std::chrono::steady_clock::now()<end);std::this_thread::yield();}
}
int main() {
    for(bool after_prepare : {false,true}) {
        // A handoff failure must release capacity and leave both lanes usable.
        streaming::AsyncStagePipeline<MovingWork> pipeline(1,1,
            [&](auto& values){if(!after_prepare)for(auto& value:values)if(value.failure)value.failure->armed=true;},
            [&](MovingWork& value){if(after_prepare && value.failure)value.failure->armed=true;},
            [](MovingWork&,const char*){});
        auto failure=std::make_shared<MoveFailure>();
        assert(pipeline.submit({1,failure}));
        until([&]{return pipeline.available()==1;});
        assert(failure->thrown==1 && pipeline.take().empty());
        // The owner must learn that its completion was lost so it can clear
        // pending state and retry, even though the queue has regained capacity.
        assert(pipeline.take_dropped()==1);
        assert(pipeline.take_dropped()==0);
        pipeline.cancel();
        assert(pipeline.submit({2,{}}));
        std::deque<MovingWork> done;until([&]{done=pipeline.take();return !done.empty();});
        assert(done.size()==1 && done[0].id==2 && pipeline.available()==1);
    }
    for(bool in_prepare : {false,true})for(bool standard_exception : {false,true}) {
        // Both exception categories on both lanes survive a throwing reporter
        // and deliver its fallback diagnostic to the publication lane.
        const auto fail=[&]{if(standard_exception)throw std::runtime_error("stage failed");throw 42;};
        streaming::AsyncStagePipeline<Work> pipeline(1,1,
            [&](auto& values){if(!in_prepare && values[0].id==9)fail();},
            [&](Work& value){if(in_prepare && value.id==9)fail();},
            [](Work& value,const char* why){
                if(std::strcmp(why,"failure callback threw")!=0)throw std::runtime_error("reporter failed");
                value.error=why;
            });
        assert(pipeline.submit({9,{}}));
        std::deque<Work> done;until([&]{done=pipeline.take();return !done.empty();});
        assert(done.size()==1 && done[0].error=="failure callback threw" && pipeline.available()==1);
        assert(pipeline.submit({10,{}}));
        until([&]{done=pipeline.take();return !done.empty();});
        assert(done.size()==1 && done[0].id==10 && done[0].error.empty());
    }
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
    {
        // A failure callback that throws must not take the process down.
        streaming::AsyncStagePipeline<Work> pipeline(2,2,
            [](auto&){throw std::runtime_error("read failed");},
            [](Work&){},
            [](Work&,const char*){throw std::runtime_error("failure callback threw");});
        assert(pipeline.submit({9,{}}));
        std::deque<Work> done;until([&]{done=pipeline.take();return !done.empty();});
        assert(done.size()==1 && pipeline.available()==2);
    }
    std::puts("ALL PASS: asynchronous overlap, bounded admission, cancellation, failures and shutdown pins");
}
