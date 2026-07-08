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
 *  huxingyi/autoremesher. The upstream pipeline ends at quad extraction and
 *  has no quad-to-triangle stage. See Libraries/autoremesher_core/UPSTREAM.md
 *  Deviation #1.
 */
#ifndef AUTOREMESHER_QUAD_TO_TRI_H
#define AUTOREMESHER_QUAD_TO_TRI_H

#include <cstdint>
#include <string>
#include <vector>

namespace ar_internal {

// Triangulate a flat index buffer of quads.
//
// Input: `quad_indices` — a flat array of 4-tuples (a, b, c, d) per quad. Size
// must be a positive multiple of 4, or zero.
// Output: `tri_indices` — a flat array of 3-tuples. Each quad (a, b, c, d)
// produces two triangles: (a, b, c) and (a, c, d) via a fan/diagonal split.
// On success, `err` is left empty and the function returns true. Any prior
// contents of `tri_indices` are cleared.
//
// On malformed input (size not divisible by 4), `err` is populated with a
// human-readable message and the function returns false. `tri_indices` is
// left empty in that case.
//
// Note on "pass-through for triangles": the driver layer (Task 6) is
// responsible for detecting a triangle stream (size % 3 == 0 && size % 4 != 0)
// and short-circuiting the call; this function operates exclusively on quads
// and reports a size-mismatch error otherwise. Keeping the contract narrow
// makes the malformed-input test unambiguous.
bool triangulate(const std::vector<uint32_t>& quad_indices,
                 std::vector<uint32_t>& tri_indices,
                 std::string& err);

} // namespace ar_internal

#endif // AUTOREMESHER_QUAD_TO_TRI_H
