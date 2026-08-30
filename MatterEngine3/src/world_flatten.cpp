// MatterEngine3/src/world_flatten.cpp
//
// Implementation of the part-graph flattener declared in world_flatten.h. One
// depth-first walk turns a DAG of parts (each interior node placing children by
// a row-major 4x4) into a flat list of leaf placements in world space.
//
// Where it sits: this runs in the bake pipeline, upstream of sector binning —
// src/sector_grid.cpp bins the FlatInstance list this produces, and
// src/provider/resolvers.cpp builds FlatInstance rows for the resolve path.
// It has no dependency beyond `mat4` (tri.h, from libs/SpatialQueryLib) and the
// standard library: no GL, no Vulkan, no file I/O, which is what makes it
// directly unit-testable (MatterEngine3/tests/composition_tests.cpp).
//
// Conventions and gotchas:
//   - All matrices are ROW-MAJOR and compose left-to-right:
//     child_world = parent_world * child.transform. Feeding a column-major
//     transform in here transposes every placement silently.
//   - The walk is recursive on the C++ stack. `FlattenLimits::max_depth` (32 by
//     default) is what bounds that stack, and it is also the only thing that
//     stops a cyclic graph — the PartGraph is contractually a DAG, but a cycle
//     that slipped through terminates as a max_depth error rather than a hang.
//   - Failure IS transactional at the public boundary: every false return from
//     flatten() leaves `out` empty, whether the cause was a limit breach or an
//     OOM. `recurse()` itself unwinds without cleaning up — flatten() clears.
//   - Single-threaded; no shared state, so separate roots may be flattened on
//     separate threads into separate vectors.
#include "world_flatten.h"
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

// Adopt a ChildInstance's 16 floats as a mat4. Both sides are row-major, so this
// is a straight element copy in storage order — no transpose. Only correct for a
// transform that already follows this file's row-major convention.
static mat4 from_row16(const float t[16]) { mat4 m; for (int i=0;i<16;++i) m.cell[i]=t[i]; return m; }

// Depth-first body of flatten(). `world` is the composed transform of `hash`'s
// placement; a part missing from the graph, or present with an empty child
// vector, is a LEAF and emits one FlatInstance. Interior parts emit nothing of
// their own — they only compose transforms down to their children, so a part
// that both carries geometry and places children contributes solely through its
// leaves here.
//
// Returns false on the first limit breach and writes a message into `err`
// naming the offending part hash; the failure then unwinds without further
// emission, leaving `out` holding the instances produced so far. flatten() is
// what discards those — see there. Both caps are checked against the state at
// the moment of the check: depth on entry, out.size() just before an emit.
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
        // A limit breach unwinds out of recurse() with `out` holding whatever
        // was emitted before the breach. Discard it here so a false return
        // ALWAYS means "out is empty" -- a half-flattened world silently
        // treated as complete is the failure mode this closes, and it costs
        // nothing on the success path.
        if (recurse(graph, root, mat4::Identity(), 0, limits, out, err))
            return true;
        out.clear();
        return false;
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
