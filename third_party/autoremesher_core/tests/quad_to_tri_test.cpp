/*
 *  Copyright (c) 2026 MatterEngine2 contributors. All rights reserved.
 *
 *  MIT license — see UPSTREAM.md and LICENSE for terms.
 *
 *  Standalone unit test for ar_internal::triangulate (quad_to_tri.cpp).
 *  Does NOT link against the full autoremesher pipeline; builds with just
 *  quad_to_tri.cpp on the compiler command line.
 *
 *  Suggested build (Task 6 will wire this into tests/Makefile):
 *    c++ -std=c++17 -I../include \
 *        quad_to_tri_test.cpp ../src/quad_to_tri.cpp -o quad_to_tri_test
 */
#include "autoremesher/quad_to_tri.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define AR_EXPECT_EQ(actual, expected)                                       \
    do {                                                                     \
        const auto _a = (actual);                                            \
        const auto _e = (expected);                                          \
        if (!(_a == _e)) {                                                   \
            std::fprintf(stderr,                                             \
                         "FAIL %s:%d: %s == %s\n",                           \
                         __FILE__, __LINE__, #actual, #expected);            \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define AR_EXPECT_TRUE(cond)                                                 \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr,                                             \
                         "FAIL %s:%d: %s\n",                                 \
                         __FILE__, __LINE__, #cond);                         \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// (a) A single quad -> two triangles.
void test_single_quad()
{
    const std::vector<uint32_t> quads = {10, 11, 12, 13};
    std::vector<uint32_t> tris;
    std::string err;
    const bool ok = ar_internal::triangulate(quads, tris, err);
    AR_EXPECT_TRUE(ok);
    AR_EXPECT_TRUE(err.empty());
    AR_EXPECT_EQ(tris.size(), std::size_t{6});
    // (a, b, c) then (a, c, d)
    AR_EXPECT_EQ(tris[0], uint32_t{10});
    AR_EXPECT_EQ(tris[1], uint32_t{11});
    AR_EXPECT_EQ(tris[2], uint32_t{12});
    AR_EXPECT_EQ(tris[3], uint32_t{10});
    AR_EXPECT_EQ(tris[4], uint32_t{12});
    AR_EXPECT_EQ(tris[5], uint32_t{13});
}

// (b) Two quads -> four triangles.
void test_two_quads()
{
    const std::vector<uint32_t> quads = {0, 1, 2, 3, 4, 5, 6, 7};
    std::vector<uint32_t> tris;
    std::string err;
    const bool ok = ar_internal::triangulate(quads, tris, err);
    AR_EXPECT_TRUE(ok);
    AR_EXPECT_TRUE(err.empty());
    AR_EXPECT_EQ(tris.size(), std::size_t{12});
    // First quad
    AR_EXPECT_EQ(tris[0], uint32_t{0});
    AR_EXPECT_EQ(tris[2], uint32_t{2});
    AR_EXPECT_EQ(tris[5], uint32_t{3});
    // Second quad
    AR_EXPECT_EQ(tris[6], uint32_t{4});
    AR_EXPECT_EQ(tris[8], uint32_t{6});
    AR_EXPECT_EQ(tris[11], uint32_t{7});
}

// (c) Empty input -> empty output, no error.
void test_empty()
{
    const std::vector<uint32_t> quads;
    std::vector<uint32_t> tris;
    std::string err;
    const bool ok = ar_internal::triangulate(quads, tris, err);
    AR_EXPECT_TRUE(ok);
    AR_EXPECT_TRUE(err.empty());
    AR_EXPECT_TRUE(tris.empty());
}

// (d) Malformed input (size % 4 != 0) -> err populated, returns false.
void test_malformed()
{
    // 5 indices — not a multiple of 4.
    const std::vector<uint32_t> quads = {1, 2, 3, 4, 5};
    std::vector<uint32_t> tris;
    std::string err;
    // Pre-fill tris to confirm it gets cleared.
    tris.push_back(99);
    const bool ok = ar_internal::triangulate(quads, tris, err);
    AR_EXPECT_TRUE(!ok);
    AR_EXPECT_TRUE(!err.empty());
    AR_EXPECT_TRUE(tris.empty());
}

} // namespace

int main()
{
    test_single_quad();
    test_two_quads();
    test_empty();
    test_malformed();

    if (g_failures == 0) {
        std::printf("quad_to_tri_test: PASS (4 cases)\n");
        return 0;
    }
    std::fprintf(stderr, "quad_to_tri_test: FAIL (%d assertion(s))\n", g_failures);
    return 1;
}
