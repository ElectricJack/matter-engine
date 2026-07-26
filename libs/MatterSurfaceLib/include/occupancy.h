#pragma once

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
    void for_each(const std::function<void(SlotCoord, const SlotData&)>& fn) const;
private:
    std::unordered_map<uint64_t, SlotData> slots_;
};
