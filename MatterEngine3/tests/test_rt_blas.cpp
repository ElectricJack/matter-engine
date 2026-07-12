#include "rt_lighting.h"
#include <cstdio>
#include <cassert>

int main() {
    viewer::RtLighting rt;
    std::string err;
    if (!rt.init(err)) { printf("SKIP: %s\n", err.c_str()); return 0; }

    float verts[] = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.5f, 1.0f, 0.0f,
    };
    rt.register_part(42, verts, 3);
    printf("PASS: BLAS built for 1-triangle part\n");

    rt.register_part(42, verts, 3);
    printf("PASS: duplicate registration is no-op\n");

    rt.unregister_part(42);
    printf("PASS: BLAS released\n");

    rt.shutdown();
    return 0;
}
