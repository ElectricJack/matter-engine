#pragma once

// libs/MatterSurfaceLib/include/occupancy.h
//
// `Occupancy` — the sparse "which lattice slots exist" set for a procedural
// part — and `pack_slot`, the 64-bit key encoding it is indexed by.
//
// Where it sits: MatterSurfaceLib's particle-authoring side, one level above
// `lattice.h` (which maps a `SlotCoord` to a local-space position and defines
// the neighbour offsets). It feeds `particle_culling.h` — shell detection asks
// whether each of a slot's lattice neighbours is occupied — and `vertex_ao.h`.
//
// Usage: `set` every authored slot while building the part, then query with
// `occupied` or walk with `for_each`. This is a build-time structure, not a
// per-frame one. Nothing here is synchronized, so keep one instance to the
// thread that is building it.
//
// Conventions and gotchas:
//   - `SlotCoord` is an integer lattice coordinate, not a position. Go
//     through `Lattice::slot_position` for local space.
//   - `set` on an already-occupied slot overwrites its `SlotData`. There is
//     no "already present" signal and no erase.
//   - `for_each` walks `std::unordered_map` bucket order — neither insertion
//     order nor coordinate order. Anything that needs a deterministic
//     sequence must sort what it collects.
//   - `pack_slot`'s per-axis range is enforced by `assert` only (see
//     `src/occupancy.cpp`). In a release build an out-of-range coordinate is
//     silently masked to 21 bits and aliases onto a different slot.

#include "lattice.h"
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <functional>

// Per-slot authoring data. Kept minimal; per-particle visual variation is
// derived deterministically from SlotCoord at emit time (see particle_culling).
struct SlotData { uint32_t materialId; };

// Pack a SlotCoord into a 64-bit key. Each axis is biased by +2^20 and stored
// in 21 bits, so coordinates in [-1048576, 1048575] per axis are representable
// -- far beyond any realistic part size.
uint64_t pack_slot(SlotCoord c);

// Sparse set of occupied slots. Sparse (hash map) so parts need not be dense
// axis-aligned blocks.
class Occupancy {
public:
    void set(SlotCoord c, const SlotData& d);   // mark slot occupied
    bool occupied(SlotCoord c) const;
    size_t count() const;
    // Visits every occupied slot exactly once, in hash-bucket order (see the
    // determinism note in the file header). `fn` must not mutate this
    // Occupancy — the walk holds an iterator over the underlying map.
    void for_each(const std::function<void(SlotCoord, const SlotData&)>& fn) const;
private:
    std::unordered_map<uint64_t, SlotData> slots_;
};
