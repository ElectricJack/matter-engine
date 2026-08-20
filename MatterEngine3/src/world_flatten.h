#pragma once
// MatterEngine3/src/world_flatten.h
//
// Flattening a part graph into placed leaf instances.
//
// A world is authored as a DAG of parts: each part may carry geometry and may
// place child parts by a row-major 4x4. Downstream stages (sector binning, the
// resolve cache, the tracer) want none of that structure — they want a flat list
// of "this leaf part, at this world transform". `flatten()` is that conversion.
//
// How it fits. Upstream, a resolver produces the PartGraph (resolved hash ->
// child rows). Downstream, src/sector_grid.cpp bins the resulting FlatInstance
// list into sectors and src/provider/resolvers.cpp carries the same rows through
// the resolve path. This header intentionally depends on nothing but `mat4` and
// the standard library, so tests can exercise it on a hand-built graph without
// touching the asset pipeline.
//
// Conventions:
//   - Every 4x4 here is ROW-MAJOR, and composition is
//     child_world = parent_world * child.transform (see mat4_mul below).
//   - Hashes are RESOLVED part hashes — the same key space PartGraph is keyed
//     by and the same one part_asset uses on disk.
//   - Costs are linear in emitted leaves; `flatten()` is a plain recursive walk
//     with no memoisation, so a part referenced by N parents is walked N times
//     and emits N instances (that is the point — they are distinct placements).
//
// Threading: no shared or static state. Concurrent calls on distinct graphs and
// distinct output vectors are fine; the same output vector is not.
#include "tri.h"          // mat4
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
    uint64_t resolved_hash;   // leaf part, in PartGraph's resolved-hash key space
    mat4     world;           // row-major, fully composed from the root down
    // Caller-assigned identity, for matching instances across re-flattens.
    // flatten() NEVER writes it: every instance it emits leaves this at 0, so a
    // consumer that needs stable ids must assign them after the call.
    uint64_t stable_id = 0;
};

// Safety valves for the recursive walk. `max_depth` bounds the C++ stack (and is
// the only backstop against a cyclic graph, which is contractually impossible
// but would otherwise hang); `max_instances` bounds the output vector. Exceeding
// either aborts the flatten with a descriptive `err` — see flatten() below for
// what happens to `out`.
// Safety valves for the recursive walk. `max_depth` bounds the C++ stack (and is
// the only backstop against a cyclic graph, which is contractually impossible
// but would otherwise hang); `max_instances` bounds the output vector. Exceeding
// either aborts the flatten with a descriptive `err` — see flatten() below for
// what happens to `out`.
struct FlattenLimits {
    uint32_t max_depth     = 32;
    uint32_t max_instances = 1000000;
};

// Recursively flatten `root`'s child graph into leaf instances, composing
// world = parent_world * child.transform down the tree. Returns false and sets
// `err` (naming the offending part/path) if max_depth or max_instances is
// exceeded. Leaf parts (no children) emit a FlatInstance; interior parts only
// compose transforms.
// `out` and `err` are cleared on entry. On failure `out` is NOT rolled back for
// a limit breach — it holds the leaves emitted before the breach — so treat any
// false return as "discard out". The one exception is an out-of-memory failure,
// which is caught, reported through `err` (naming the root hash) and does clear
// `out`, so a runaway world reports instead of taking the process down.
// FlatInstance::stable_id is left at 0 for every emitted instance.
// `out` and `err` are cleared on entry. On failure `out` is NOT rolled back for
// a limit breach — it holds the leaves emitted before the breach — so treat any
// false return as "discard out". The one exception is an out-of-memory failure,
// which is caught, reported through `err` (naming the root hash) and does clear
// `out`, so a runaway world reports instead of taking the process down.
// FlatInstance::stable_id is left at 0 for every emitted instance.
bool flatten(const PartGraph& graph, uint64_t root, const FlattenLimits& limits,
             std::vector<FlatInstance>& out, std::string& err);

// Row-major 4x4 multiply: result = a * b.
mat4 mat4_mul(const mat4& a, const mat4& b);

} // namespace world_flatten
