#include "rt_lighting.h"
#include <cstdio>
#include <cassert>

int main() {
    viewer::RtLighting rt;
    std::string err;
    bool ok = rt.init(err);
    if (!ok) {
        printf("SKIP: RT not available: %s\n", err.c_str());
        return 0;  // not a failure — just no NVIDIA GPU
    }
    assert(rt.available());
    printf("PASS: OptiX initialized successfully\n");
    rt.shutdown();
    assert(!rt.available());
    printf("PASS: shutdown clean\n");
    return 0;
}
