#include "check.h"
#include "render/blas_disk_cache.h"
#include <chrono>
#include <filesystem>
#include <fstream>

using viewer::BlasDiskCache;
static BlasDiskCache::Bytes read(BlasDiskCache& cache, const std::string& directory, const std::string& key) {
    const auto ticket = cache.read(directory, key);
    CHECK(ticket != 0, "read admitted");
    BlasDiskCache::Bytes result;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!cache.take(ticket, result)) {
        if (std::chrono::steady_clock::now() > end) { CHECK(false, "cache worker completed within timeout"); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return result;
}
int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("matter-blas-cache-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto payload = std::make_shared<const std::vector<uint8_t>>(256, 71);
    {
        BlasDiskCache cache;
        CHECK(!read(cache, directory.string(), "absent"), "missing derived data is an ordinary miss");
        CHECK(cache.write(directory.string(), "device/geometry", payload), "enqueue derived data");
        const auto same_session = read(cache, directory.string(), "device/geometry");
        CHECK(same_session && *same_session == *payload, "ordered read observes preceding committed write");
        CHECK(!cache.write(directory.string(), "oversized", std::make_shared<const std::vector<uint8_t>>(BlasDiskCache::max_blob+1)), "oversized input rejected");
    }
    {
        BlasDiskCache cache;
        const auto loaded = read(cache, directory.string(), "device/geometry");
        CHECK(loaded && *loaded == *payload, "fresh cache instance reads durable bytes");
        CHECK(!read(cache, directory.string(), "different-device/geometry"), "different compatibility identity misses");
        CHECK(!read(cache, directory.string(), "device/changed-geometry"), "changed geometry misses");
    }
    {
        BlasDiskCache cache;
        std::vector<uint64_t> tickets;
        for (int i=0; i<32; ++i) tickets.push_back(cache.read(directory.string(), "device/geometry"));
        CHECK(cache.read(directory.string(), "overflow") == 0, "pending and completed read payloads share a bounded admission limit");
        for (auto ticket : tickets) { CHECK(ticket != 0, "bounded batch read admitted"); cache.cancel(ticket); }
        CHECK(read(cache, directory.string(), "device/geometry") != nullptr, "cancelled reads cannot strand subsequent requests");
        for (int i=0; i<24; ++i)
            CHECK(cache.write(directory.string(), "batch/"+std::to_string(i), std::make_shared<const std::vector<uint8_t>>(512, uint8_t(i))), "batch write admitted");
        for (int i=0; i<24; ++i) {
            const auto bytes = read(cache, directory.string(), "batch/"+std::to_string(i));
            CHECK(bytes && bytes->size()==512 && bytes->front()==i && bytes->back()==i, "batched writes retain key/payload association");
        }
    }
    // Damage the payload through the store's own index location, then reopen.
    {
        std::string error; asset_store::StoreConfig config; config.dir = directory.string();
        auto store = asset_store::BlobStore::open(config, &error);
        CHECK(store != nullptr, error.c_str());
        auto refs = asset_store::RefTable::open(*store, {}, &error);
        asset_store::RefInfo ref; CHECK(refs && refs->lookup("device/geometry", &ref), "durable reference exists");
        asset_store::BlobLocation location;
        CHECK(store->locate(ref.hash, &location), "payload is indexed");
        // Truncation is sufficient to exercise the read failure path without
        // relying on the private pack header format.
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == ".pack") std::filesystem::resize_file(entry.path(), 0);
    {
        BlasDiskCache cache;
        CHECK(!read(cache, directory.string(), "device/geometry"), "damaged cache degrades to miss");
    }
    std::filesystem::remove_all(directory);
    return check_summary();
}
