#pragma once

// MatterEngine3/include/matter/lod_contract.h
//
// One number that the bake side and the render side must agree on: the
// maximum number of LOD levels a flattened part may carry. It lives alone in
// its own header so neither side has to include the other's.
//
// Enforced in two places:
//   * MatterEngine3/src/part_flatten.cpp stops extending a cluster's ladder
//     once it has this many rungs, and MatterEngine3/src/part_flatten.h
//     normalizes a requested cap at or above this value back to 0 ("no cap");
//   * MatterEngine3/src/part_asset_v2.cpp rejects a .part whose cluster
//     declares more levels than this, on both the write and the read path, so
//     a stale artifact cannot smuggle an unaddressable rung into the GPU cull
//     lane.
//
// It is a COUNT of levels, not a maximum level index. Raising it means
// re-checking every GPU path that indexes a rung.
//
// This is unrelated to LOD SELECTION, which has exactly one rule:
// MatterEngine3/src/render/lod_distance.h.

#include <cstddef>

namespace matter {

// Shared serialized and renderer capacity. Flat artifacts must never publish
// more levels than either GPU culling path can address.
inline constexpr std::size_t kMaxSerializedLodLevels = 9;

} // namespace matter
