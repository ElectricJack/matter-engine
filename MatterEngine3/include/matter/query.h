#pragma once
#include <cstdint>

namespace matter {

struct RayHit {
    float t = -1.0f;
    float normal[3] = {0, 0, 0};   // world-space, faces the ray origin
    uint32_t instance = 0;         // index usable with instance_info()
    uint64_t part_hash = 0;
    int material_id = -1;
};

struct InstanceInfo {
    float transform[16];           // row-major world placement
    uint64_t part_hash = 0;
    const char* module_name = nullptr;  // may be null; valid until next bake/reload
};

} // namespace matter
