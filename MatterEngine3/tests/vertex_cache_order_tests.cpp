#include "render/vertex_cache_order.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <random>

using Triangle = std::array<uint32_t, 3>;
static std::vector<Triangle> triangles(const std::vector<uint32_t>& indices) {
    std::vector<Triangle> result;
    for (size_t i = 0; i < indices.size(); i += 3)
        result.push_back({indices[i], indices[i + 1], indices[i + 2]});
    std::sort(result.begin(), result.end());
    return result;
}

int main() {
    std::mt19937 random(7123);
    constexpr uint32_t side = 65;
    std::vector<Triangle> grid;
    for (uint32_t y = 0; y + 1 < side; ++y) {
        for (uint32_t x = 0; x + 1 < side; ++x) {
            const uint32_t a = y * side + x;
            grid.push_back({a, a + 1, a + side});
            grid.push_back({a + 1, a + side + 1, a + side});
        }
    }
    std::shuffle(grid.begin(), grid.end(), random);
    std::vector<uint32_t> indices;
    for (const auto& t : grid) indices.insert(indices.end(), t.begin(), t.end());
    const auto original = indices;
    const size_t before = viewer::vertex_cache_misses(indices, side * side);
    assert(viewer::optimize_vertex_cache_order(indices, side * side));
    const size_t after = viewer::vertex_cache_misses(indices, side * side);
    assert(triangles(indices) == triangles(original));
    assert(after < before / 2);
    auto repeat = original;
    assert(viewer::optimize_vertex_cache_order(repeat, side * side));
    assert(repeat == indices);
    std::printf("grid FIFO misses: %zu -> %zu\n", before, after);

    // Triangle multiplicity, winding and provoking vertices remain exact,
    // including repeated corners, disconnected components and sparse IDs.
    for (int trial = 0; trial < 100; ++trial) {
        std::vector<uint32_t> input;
        const uint32_t vertices = 16 + random() % 256;
        for (int t = 0; t < 1000; ++t) {
            const uint32_t a = random() % vertices;
            input.insert(input.end(), {a, t % 3 ? uint32_t(random() % vertices) : a,
                                      uint32_t(random() % vertices)});
            if (t % 10 == 0) input.insert(input.end(), {a, a, a});
        }
        const auto saved = input;
        viewer::optimize_vertex_cache_order(input, vertices + 17);
        assert(triangles(input) == triangles(saved));
        assert(viewer::vertex_cache_misses(input, vertices + 17) <=
               viewer::vertex_cache_misses(saved, vertices + 17));
    }
    for (auto invalid : {std::vector<uint32_t>{}, std::vector<uint32_t>{0, 1},
                         std::vector<uint32_t>(99, 10)}) {
        const auto saved = invalid;
        assert(!viewer::optimize_vertex_cache_order(invalid, 3));
        assert(invalid == saved);
    }
    auto soup = original;
    for (size_t i = 0; i < soup.size(); ++i) soup[i] = static_cast<uint32_t>(i);
    const auto saved = soup;
    assert(!viewer::optimize_vertex_cache_order(soup, soup.size()));
    assert(soup == saved);
    std::puts("vertex_cache_order_tests: ALL PASS");
}
