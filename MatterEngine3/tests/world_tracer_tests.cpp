#include "world_tracer.h"
#include "tlas_manager.hpp"
#include "check.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>

// Bound individual C++ allocations in the missing-artifact regression. This
// catches the former multi-MiB reservation without exhausting the test host.
// The standalone executable owns these replacements; production is unchanged.
static thread_local size_t allocation_limit = std::numeric_limits<size_t>::max();
static thread_local size_t largest_allocation = 0;
void* operator new(size_t size) {
    largest_allocation = std::max(largest_allocation, size);
    if (size > allocation_limit) throw std::bad_alloc();
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

struct AllocationBound {
    AllocationBound() { largest_allocation = 0; allocation_limit = 1024 * 1024; }
    ~AllocationBound() { allocation_limit = std::numeric_limits<size_t>::max(); }
};

static world_tracer::TraceInstance placement(uint64_t hash, float x = 0.f) {
    world_tracer::TraceInstance p{};
    p.part_hash = hash;
    for (int i = 0; i < 16; ++i) p.transform[i] = i % 5 == 0 ? 1.f : 0.f;
    p.transform[3] = x;
    return p;
}

static void test_missing_parts(const std::string& root) {
    std::vector<world_tracer::TraceInstance> instances;
    for (uint64_t i = 1; i <= 128; ++i) {
        instances.push_back(placement(i));
        instances.push_back(placement(i, 10.f));
    }
    world_tracer::WorldTracer tracer;
    std::string error;
    bool built = false;
    try {
        AllocationBound bound;
        built = tracer.build(root, instances, error);
    } catch (const std::bad_alloc&) {
        CHECK(false, "missing artifacts must not make multi-MiB reservations");
    }
    std::printf("missing parts: largest new request=%zu bytes; DrawRecord=%zu bytes\n",
                largest_allocation, sizeof(TLASManager::DrawRecord));
    CHECK(built, "missing artifacts remain nonfatal");
    if (!built) return;
    CHECK(error.empty(), "missing artifact diagnostics are consumed");
    CHECK(tracer.disk_loads() == 128, "a failed hash is attempted once per build");
    CHECK(tracer.expanded_instance_count() == 0, "failed parts add no geometry");
    const float origin[] = {.25f, .25f, 2.f}, dir[] = {0.f, 0.f, -1.f};
    world_tracer::Hit hit;
    CHECK(!tracer.trace(origin, dir, 10.f, hit), "empty tracer misses");
}

static void test_disk_geometry(const std::string& root) {
    constexpr uint64_t hash = 0xface1234;
    BLASManager blas;
    TLASManager tlas(32);
    for (int n = 0; n < 2; ++n) {
        Tri tri{};
        const float x = 4.f * n;
        tri.vertex0 = make_float3(x, 0.f, 0.f);
        tri.vertex1 = make_float3(x + 1.f, 0.f, 0.f);
        tri.vertex2 = make_float3(x, 1.f, 0.f);
        tri.centroid = make_float3(x + 1.f / 3.f, 1.f / 3.f, 0.f);
        const BLASHandle handle = blas.register_triangles(&tri, 1, nullptr);
        CHECK(handle != INVALID_BLAS_HANDLE, "fixture geometry builds");
        for (int i = 0; i < 10; ++i)
            CHECK(tlas.draw(handle) != 0, "fixture draw is recorded");
    }
    part_asset::LodLevels lods(2);
    lods[0].screen_size_threshold = 256.f;
    lods[0].blas_indices = {0, 1};
    lods[1].screen_size_threshold = 32.f;
    lods[1].blas_indices = {1};
    const std::string path = root + "/" + part_asset::cache_path_resolved(hash);
    CHECK(part_asset::save_v2(path, blas, tlas, nullptr, 0, lods, hash),
          "fixture artifact saves");

    // A decoder knows the serialized count. Its caller need not guess it.
    BLASManager decoded;
    TLASManager decoded_tlas(0);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels decoded_lods;
    CHECK(part_asset::load_v2(path, hash, decoded, decoded_tlas, children, decoded_lods),
          "decode accepts an initially empty draw capacity");
    CHECK(decoded_tlas.get_draw_records().size() == 20,
          "decode preserves every serialized draw without a caller capacity guess");
    CHECK(decoded_tlas.get_instance_count() == 20, "all decoded draws enter the TLAS");
    CHECK(part_asset::load_v2(path, hash, decoded, decoded_tlas, children, decoded_lods) &&
              decoded_tlas.get_draw_records().size() == 40 &&
              decoded_tlas.get_instance_count() == 40,
          "decoder growth includes previously recorded instances");

    world_tracer::WorldTracer tracer;
    std::string error;
    bool built = false;
    try {
        AllocationBound bound;
        built = tracer.build(root, {placement(hash, 10.f), placement(hash, 20.f)}, error);
    } catch (const std::bad_alloc&) {
        CHECK(false, "small disk parts must not make multi-MiB reservations");
    }
    CHECK(built, "disk tracer builds with bounded small-part allocations");
    if (!built) return;
    CHECK(tracer.disk_loads() == 1, "successful duplicate placements share a decode");
    CHECK(tracer.expanded_instance_count() == 2, "both world placements survive");
    const float dir[] = {0.f, 0.f, -1.f};
    world_tracer::Hit hit;
    for (float x : {14.25f, 24.25f}) {
        const float origin[] = {x, .25f, 2.f};
        CHECK(tracer.trace(origin, dir, 10.f, hit) && std::fabs(hit.t - 2.f) < 1e-5f,
              "coarse geometry survives destruction of temporary loading storage");
        CHECK(tracer.occluded(origin, dir, 10.f), "disk geometry still occludes");
    }
    const float fine_only[] = {10.25f, .25f, 2.f};
    CHECK(!tracer.trace(fine_only, dir, 10.f, hit), "coarsest LOD selection is preserved");
}

int main() {
    // A private directory avoids depending on, or deleting, production caches.
    const auto root = std::filesystem::temp_directory_path() /
        ("matter-world-tracer-tests-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "parts");
    test_missing_parts(root.string());
    test_disk_geometry(root.string());
    std::filesystem::remove_all(root);
    return check_summary();
}
