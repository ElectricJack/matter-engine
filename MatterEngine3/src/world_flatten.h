#pragma once
#include "bvh.h"          // mat4
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace world_flatten {

// Mirror of SP-1 ChildInstance (identical fields: hash + row-major transform).
// Kept local so world_flatten is testable on a bare PartGraph without loading
// .part files; layout matches part_asset::ChildInstance by design.
struct ChildInstance {
    uint64_t child_resolved_hash;
    float    transform[16];   // row-major, child placement under parent's frame
};

// A part-graph: resolved_hash -> its child-instance rows. A part with no entry
// (or an empty vector) is a leaf. SP-3 guarantees this is a DAG (no cycles).
using PartGraph = std::map<uint64_t, std::vector<ChildInstance>>;

// One flattened world instance: a leaf part placed by a composed world transform.
struct FlatInstance {
    uint64_t resolved_hash;
    mat4     world;
};

struct FlattenLimits {
    uint32_t max_depth     = 32;
    uint32_t max_instances = 1000000;
};

// Recursively flatten `root`'s child graph into leaf instances, composing
// world = parent_world * child.transform down the tree. Returns false and sets
// `err` (naming the offending part/path) if max_depth or max_instances is
// exceeded. Leaf parts (no children) emit a FlatInstance; interior parts only
// compose transforms.
bool flatten(const PartGraph& graph, uint64_t root, const FlattenLimits& limits,
             std::vector<FlatInstance>& out, std::string& err);

// Row-major 4x4 multiply: result = a * b.
mat4 mat4_mul(const mat4& a, const mat4& b);

} // namespace world_flatten
