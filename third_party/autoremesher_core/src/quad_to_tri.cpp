/*
 *  Copyright (c) 2026 MatterEngine2 contributors. All rights reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 *  Note: This file is authored locally in MatterEngine2, not vendored from
 *  huxingyi/autoremesher. See UPSTREAM.md Deviation #1.
 */
#include "autoremesher/quad_to_tri.h"

namespace ar_internal {

bool triangulate(const std::vector<uint32_t>& quad_indices,
                 std::vector<uint32_t>& tri_indices,
                 std::string& err)
{
    tri_indices.clear();
    err.clear();

    if (quad_indices.empty()) {
        return true;
    }

    if ((quad_indices.size() % 4u) != 0u) {
        err = "quad_to_tri::triangulate: quad_indices size (" +
              std::to_string(quad_indices.size()) +
              ") is not a multiple of 4";
        return false;
    }

    const std::size_t quad_count = quad_indices.size() / 4u;
    tri_indices.reserve(quad_count * 6u);

    for (std::size_t q = 0; q < quad_count; ++q) {
        const std::size_t base = q * 4u;
        const uint32_t a = quad_indices[base + 0];
        const uint32_t b = quad_indices[base + 1];
        const uint32_t c = quad_indices[base + 2];
        const uint32_t d = quad_indices[base + 3];

        // Fan triangulation along the a-c diagonal:
        //   quad (a, b, c, d) → (a, b, c) + (a, c, d)
        tri_indices.push_back(a);
        tri_indices.push_back(b);
        tri_indices.push_back(c);

        tri_indices.push_back(a);
        tri_indices.push_back(c);
        tri_indices.push_back(d);
    }

    return true;
}

} // namespace ar_internal
