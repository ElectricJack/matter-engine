/* matter_math_c_smoke.c
 *
 * Compile-only smoke test proving libs/MathLib/include/matter_math_c.h is
 * valid C, not just C-flavored C++. Compiled with the C compiler (not g++)
 * by the Makefile's `c-smoke` target -- if a future edit sneaks in a C++-only
 * construct (a namespace, a default member initializer, a reference), this
 * fails to compile where mathlib_tests.cpp (built with g++) would not
 * notice, because g++ silently accepts C++ features in a header included
 * from a .cpp file.
 *
 * This mirrors how libs/MatterSurfaceLib/src/surface.c and fat_primitive.c
 * actually consume the header: real C, via gcc.
 */

#include "../include/matter_math_c.h"

static MtVec3 add3(MtVec3 a, MtVec3 b) {
    MtVec3 out;
    out.x = a.x + b.x;
    out.y = a.y + b.y;
    out.z = a.z + b.z;
    return out;
}

int main(void) {
    MtVec2 v2 = {1.0f, 2.0f};
    MtVec3 v3a = {1.0f, 2.0f, 3.0f};
    MtVec3 v3b = {4.0f, 5.0f, 6.0f};
    MtVec4 v4 = {1.0f, 2.0f, 3.0f, 4.0f};
    MtMat4 m;
    int i;

    for (i = 0; i < 16; ++i) {
        m.m[i] = (i % 5 == 0) ? 1.0f : 0.0f; /* identity-ish, values unused */
    }

    v3a = add3(v3a, v3b);

    return (int)(v2.x + v3a.x + v4.x + m.m[0]) - (int)(v2.x + v3a.x + v4.x + m.m[0]);
}
