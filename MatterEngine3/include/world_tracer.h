#pragma once
// GL-free CPU tracer over the placed world for the probe baker. Loads each unique
// part hash ONCE (flat artifact preferred, compositional fallback expands children
// into extra instances, depth cap 8), keeps the prebuilt BVHs from load_v2 alive in
// owning scratch managers, and intersects through a custom int32 instance BVH.
// (MSL's TLAS packs instance index into 12 bits of instPrim and uses u16 node
// links — too small for meadow scale, hence this instance layer.)
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace world_tracer {

struct TraceInstance {
    uint64_t part_hash;
    float    transform[16];   // row-major world placement
};

struct Hit {
    float t = -1.0f;
    float normal[3] = {0,0,0};   // world-space geometric normal, faces the ray origin
    int   material_id = -1;      // registry index (TriEx materialId % 1000000), -1 if no TriEx
    float emission = 0.0f;       // MaterialRegistryGet(material_id)->emission (0 if id<0)
    float albedo[3] = {0.5f,0.5f,0.5f};
};

class WorldTracer {
public:
    WorldTracer();
    ~WorldTracer();

    // cache_root contains parts/<hash>.part and optionally parts/<hash>.flat.part.
    bool build(const std::string& cache_root,
               const std::vector<TraceInstance>& instances, std::string& err);
    bool trace(const float origin[3], const float dir[3], float max_t, Hit& hit) const;
    bool occluded(const float origin[3], const float dir[3], float max_t) const;
    void world_bounds(float mn[3], float mx[3]) const;   // valid after build
    size_t instance_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace world_tracer
