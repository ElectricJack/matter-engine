#include "render/blas_disk_cache.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <cstdio>
int main() {
    using Clock=std::chrono::steady_clock;
    auto path=std::filesystem::temp_directory_path()/("matter-blas-queue-"+std::to_string(Clock::now().time_since_epoch().count()));
    {
        viewer::BlasDiskCache cache;
        std::vector<uint64_t> requests;
        // Completed but unconsumed results still occupy the bounded queue, so
        // saturation is deterministic regardless of worker scheduling.
        for(int i=0;i<32;++i){auto id=cache.read(path.string(),"missing");assert(id);requests.push_back(id);}
        uint64_t ticket=0;viewer::BlasDiskCache::Bytes bytes;
        assert(!cache.poll(path.string(),"missing",ticket,bytes) && ticket==0);
        const auto deadline=Clock::now()+std::chrono::seconds(10);
        while(!cache.take(requests.front(),bytes)){assert(Clock::now()<deadline);std::this_thread::yield();}
        assert(!bytes); // A real completed miss, unlike the saturated poll above.
        bool complete=cache.poll(path.string(),"missing",ticket,bytes);
        assert(complete || ticket!=0);
        while(!complete){assert(Clock::now()<deadline);std::this_thread::yield();complete=cache.poll(path.string(),"missing",ticket,bytes);}
        assert(ticket==0 && !bytes);
        for(size_t i=1;i<requests.size();++i)cache.cancel(requests[i]);
    }
    std::filesystem::remove_all(path);
    std::puts("ALL PASS: BLAS queue saturation defers lookup without reporting a false cache miss");
}
