// Compatibility boundary; implementation is owned by MatterGraphics.
#ifdef MATTER_CODEC_SOURCE_QUALIFICATION
#include "../../libs/MatterGraphics/tests/native/blas_disk_cache_tests.cpp"
#else
#include "check.h"
#include "../../libs/MatterGraphics/src/native/blas_disk_cache.h"
#include <chrono>

int main() {
    viewer::BlasDiskCache cache;
    CHECK(!cache.write("", "null", {}), "null derived payload rejected");
    CHECK(!cache.write("", "short", std::make_shared<const std::vector<uint8_t>>(47)),
          "undersized derived payload rejected");
    CHECK(!cache.write("", "oversized", std::make_shared<const std::vector<uint8_t>>(
              viewer::BlasDiskCache::max_blob + 1)), "oversized input rejected");
    std::vector<uint64_t> tickets;
    // Empty directory is the worker's disabled initial cache: actual queue and
    // completion paths run, with no BlobStore open or filesystem access.
    for (int i = 0; i < 32; ++i) tickets.push_back(cache.read("", "disabled"));
    CHECK(cache.read("", "overflow") == 0,
          "pending and completed reads share the bounded admission limit");
    for (auto ticket : tickets) {
        CHECK(ticket != 0, "bounded batch read admitted");
        cache.cancel(ticket);
    }
    auto ticket = cache.read("", "after-cancel");
    CHECK(ticket != 0, "cancelled reads do not strand subsequent admission");
    viewer::BlasDiskCache::Bytes bytes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool complete = false;
    while (!(complete = cache.take(ticket, bytes)) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK(complete && !bytes, "disabled worker completes an ordinary miss");
    return check_summary();
}
#endif
