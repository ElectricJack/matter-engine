#include "rt_lighting.h"
#include <cstdio>
#include <cassert>
#include <cstring>

int main() {
    viewer::RtLighting rt;
    std::string err;
    if (!rt.init(err)) { printf("SKIP: %s\n", err.c_str()); return 0; }

    float verts[] = {
        -10,0,-10,  10,0,-10,  10,0,10,
        -10,0,-10,  10,0,10,  -10,0,10,
    };
    rt.register_part(1, verts, 6);

    float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    viewer::RtLighting::InstanceInput instances[3];
    for (int i = 0; i < 3; ++i) {
        instances[i].part_hash = 1;
        instances[i].lod_level = 0;
        memcpy(instances[i].transform, identity, sizeof(identity));
        instances[i].transform[3] = (float)i * 25.0f;
    }
    rt.update_instances(instances, 3);
    assert(rt.tlas_handle() != 0);
    printf("PASS: TLAS built with 3 instances\n");

    instances[0].transform[7] = 5.0f;
    rt.update_instances(instances, 3);
    assert(rt.tlas_handle() != 0);
    printf("PASS: TLAS rebuilt with updated transforms\n");

    rt.shutdown();
    return 0;
}
