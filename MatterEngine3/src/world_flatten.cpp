#include "../include/world_flatten.h"
#include <cstdio>
#include <new>       // std::bad_alloc

namespace world_flatten {

mat4 mat4_mul(const mat4& a, const mat4& b) {
    mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.cell[i*4+k] * b.cell[k*4+j];
            r.cell[i*4+j] = s;
        }
    return r;
}

static mat4 from_row16(const float t[16]) { mat4 m; for (int i=0;i<16;++i) m.cell[i]=t[i]; return m; }

static bool recurse(const PartGraph& g, uint64_t hash, const mat4& world,
                    uint32_t depth, const FlattenLimits& lim,
                    std::vector<FlatInstance>& out, std::string& err) {
    if (depth > lim.max_depth) {
        char buf[128];
        snprintf(buf, sizeof(buf), "max_depth %u exceeded at part %llu",
                 lim.max_depth, (unsigned long long)hash);
        err = buf; return false;
    }
    auto it = g.find(hash);
    bool is_leaf = (it == g.end() || it->second.empty());
    if (is_leaf) {
        if (out.size() >= lim.max_instances) {
            char buf[128];
            snprintf(buf, sizeof(buf), "max_instances %u exceeded at part %llu",
                     lim.max_instances, (unsigned long long)hash);
            err = buf; return false;
        }
        out.push_back({hash, world});
        return true;
    }
    for (const ChildInstance& c : it->second) {
        mat4 child_world = mat4_mul(world, from_row16(c.transform));
        if (!recurse(g, c.child_resolved_hash, child_world, depth + 1, lim, out, err))
            return false;
    }
    return true;
}

bool flatten(const PartGraph& graph, uint64_t root, const FlattenLimits& limits,
             std::vector<FlatInstance>& out, std::string& err) {
    out.clear(); err.clear();
    // Outer boundary: `out` grows up to limits.max_instances (1M by default),
    // and mat4 stack copies during recurse can spike briefly. A structured
    // OOM message beats a viewer crash.
    try {
        return recurse(graph, root, mat4::Identity(), 0, limits, out, err);
    } catch (const std::bad_alloc& e) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "OOM in world_flatten (root=%016llx, phase=flatten): %s",
                      (unsigned long long)root, e.what());
        err = buf;
        out.clear();
        return false;
    }
}

} // namespace world_flatten
