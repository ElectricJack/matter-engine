// Compatibility boundary; implementation is owned by MatterGraphics.
#ifdef MATTER_CODEC_SOURCE_QUALIFICATION
#include "../../libs/MatterGraphics/tests/native/blas_cache_queue_tests.cpp"
#else
#include "../../libs/MatterGraphics/src/native/blas_disk_cache.h"
#include <cassert>
#include <chrono>
#include <cstdio>

int main() {
    using Clock = std::chrono::steady_clock;
    viewer::BlasDiskCache cache;
    std::vector<uint64_t> requests;
    // The disabled initial directory runs the real bounded worker without
    // opening a BlobStore. Original disk transport remains a build gate.
    const std::string directory;
    for (int i = 0; i < 32; ++i) {
        auto id = cache.read(directory, "missing");
        assert(id);
        requests.push_back(id);
    }
    uint64_t ticket = 0;
    viewer::BlasDiskCache::Bytes bytes;
    assert(!cache.poll(directory, "missing", ticket, bytes) && ticket == 0);
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!cache.take(requests.front(), bytes)) {
        assert(Clock::now() < deadline);
        std::this_thread::yield();
    }
    assert(!bytes);
    bool complete = cache.poll(directory, "missing", ticket, bytes);
    assert(complete || ticket != 0);
    while (!complete) {
        assert(Clock::now() < deadline);
        std::this_thread::yield();
        complete = cache.poll(directory, "missing", ticket, bytes);
    }
    assert(ticket == 0 && !bytes);
    for (size_t i = 1; i < requests.size(); ++i) cache.cancel(requests[i]);
    std::puts("ALL PASS: BLAS queue saturation defers lookup without reporting a false cache miss");
}
#endif
