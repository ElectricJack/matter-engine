// libs/MatterSurfaceLib/src/occupancy.cpp
//
// Backing implementation for `Occupancy` (see `include/occupancy.h`): the
// sparse set of occupied lattice slots for a part, plus the `SlotCoord` <-> key
// packing it is built on.
//
// Key layout. Each axis is biased by +2^20 and packed into 21 bits, laid out
// x:[63..42] y:[41..21] z:[20..0]. Representable range is therefore
// [-1048576, 1048575] per axis, far beyond any realistic part size. The bias is
// what lets negative coordinates pack without sign-extension: the packed value
// is an unsigned magnitude, and `for_each` subtracts the bias back off.
//
// Range checking is by `assert` only, so in an NDEBUG build an out-of-range
// coordinate silently wraps through `SLOT_MASK` and aliases some other slot
// rather than failing. Callers with untrusted coordinates must clamp first.
//
// Ordering. The backing store is an `unordered_map`, so `for_each` visits slots
// in an unspecified, hash-dependent order. Do NOT rely on it for anything that
// must be deterministic across runs or builds (e.g. floating-point accumulation
// or emission order) — sort the coordinates yourself if you need that.
//
// No thread safety of its own: concurrent `set` and `occupied`/`for_each`/
// `count` on the same instance is a data race. The engine populates an
// Occupancy during authoring and reads it afterwards.
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

// Marks `c` occupied, OVERWRITING any SlotData already stored there. There is
// no erase/clear on this class — an Occupancy only ever grows.
void Occupancy::set(SlotCoord c, const SlotData& d) { slots_[pack_slot(c)] = d; }

bool Occupancy::occupied(SlotCoord c) const {
    return slots_.find(pack_slot(c)) != slots_.end();
}

size_t Occupancy::count() const { return slots_.size(); }

// Visits every occupied slot, unpacking the key back into a SlotCoord. Order is
// hash-dependent and unspecified (see the file header). `fn` must not mutate
// this Occupancy — the iteration holds no protection against rehashing.
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
