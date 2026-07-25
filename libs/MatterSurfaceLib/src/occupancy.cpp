#include "occupancy.h"
#include <cassert>

static constexpr int64_t SLOT_BIAS = 1 << 20;   // 1048576
static constexpr uint64_t SLOT_MASK = 0x1FFFFF; // 21 bits
static constexpr int SLOT_MIN = -(1 << 20);     // -1048576
static constexpr int SLOT_MAX = (1 << 20) - 1;  //  1048575

uint64_t pack_slot(SlotCoord c) {
    assert(c.x >= SLOT_MIN && c.x <= SLOT_MAX && "SlotCoord.x out of packable range");
    assert(c.y >= SLOT_MIN && c.y <= SLOT_MAX && "SlotCoord.y out of packable range");
    assert(c.z >= SLOT_MIN && c.z <= SLOT_MAX && "SlotCoord.z out of packable range");
    uint64_t x = (uint64_t)(c.x + SLOT_BIAS) & SLOT_MASK;
    uint64_t y = (uint64_t)(c.y + SLOT_BIAS) & SLOT_MASK;
    uint64_t z = (uint64_t)(c.z + SLOT_BIAS) & SLOT_MASK;
    return (x << 42) | (y << 21) | z;
}

void Occupancy::set(SlotCoord c, const SlotData& d) { slots_[pack_slot(c)] = d; }

bool Occupancy::occupied(SlotCoord c) const {
    return slots_.find(pack_slot(c)) != slots_.end();
}

size_t Occupancy::count() const { return slots_.size(); }

void Occupancy::for_each(const std::function<void(SlotCoord, const SlotData&)>& fn) const {
    for (const auto& kv : slots_) {
        uint64_t k = kv.first;
        SlotCoord c;
        c.x = static_cast<int>((int64_t)((k >> 42) & SLOT_MASK) - SLOT_BIAS);
        c.y = static_cast<int>((int64_t)((k >> 21) & SLOT_MASK) - SLOT_BIAS);
        c.z = static_cast<int>((int64_t)( k        & SLOT_MASK) - SLOT_BIAS);
        fn(c, kv.second);
    }
}
