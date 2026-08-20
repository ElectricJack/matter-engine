#pragma once
// tileset_slot_allocator.h — LRU allocator over the viewer's detail-tileset
// sampler slots (chart-VT spec Phase 3 / plan contract C3).
//
// Before this existed, slots were handed out by a monotonically increasing
// counter (`baked_tileset_count_`) and a world that declared more tileset roots
// than slots failed the whole load. With `defineMaterial` any number of
// materials can declare a detail tileset, so the slot pool became a cache:
// keyed by `.gtex` content hash, least-recently-acquired evicted first.
//
// Eviction is not an error. The provider unbinds every material that pointed at
// the evicted slot (MaterialRegistrySetGroundTilesetSlot(mat, -1)), which makes
// those materials fall back to their scalar albedo, and warns once.
//
// Header-only and dependency-free (no Vulkan, no GL, no registry) so headless
// suites can exercise the ordering directly.

#include "tileset_gtex.h"   // kMaxTilesetSlots — the one slot-count source of truth

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace tileset {

// Capacity of the detail-tileset slot pool. tileset_gtex.h owns the count (it
// also sizes the renderer's descriptor arrays and bounds
// MaterialRegistrySetGroundTilesetSlot); never re-declare a literal here.
inline constexpr int kSlotAllocatorCapacity = kMaxTilesetSlots;

class SlotAllocator {
public:
    struct Result {
        int      slot         = -1;     // slot now owned by the requested key
        bool     reused       = false;  // key was already resident (slot unchanged)
        bool     evicted      = false;  // an LRU victim was displaced
        int      evicted_slot = -1;     // == slot when evicted (the reused storage)
        uint64_t evicted_key  = 0;      // victim's .gtex content hash
    };

    explicit SlotAllocator(int capacity = kSlotAllocatorCapacity)
        : entries_(capacity > 0 ? static_cast<size_t>(capacity) : 0) {}

    int capacity() const { return static_cast<int>(entries_.size()); }

    // Resident entry count. A linear pass over the pool, which is
    // kMaxTilesetSlots (8) entries -- deliberately not maintained as a
    // counter. find() is linear for the same reason.
    int size() const {
        int n = 0;
        for (const Entry& e : entries_) if (e.used) ++n;
        return n;
    }

    // Slot currently holding `key`, or -1. Does not affect recency.
    int find(uint64_t key) const {
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].used && entries_[i].key == key)
                return static_cast<int>(i);
        return -1;
    }

    // Mark `key` as most-recently-used. No-op when `key` is not resident.
    void touch(uint64_t key) {
        const int slot = find(key);
        if (slot >= 0) entries_[static_cast<size_t>(slot)].stamp = ++clock_;
    }

    // Resolve `key` to a slot, evicting the least-recently-acquired entry when
    // the pool is full. Returns slot == -1 only for a zero-capacity allocator.
    Result acquire(uint64_t key) {
        Result r;
        const int resident = find(key);
        if (resident >= 0) {
            entries_[static_cast<size_t>(resident)].stamp = ++clock_;
            r.slot = resident;
            r.reused = true;
            return r;
        }
        if (entries_.empty()) return r;

        // Prefer a free slot; lowest index first so a world that fits keeps the
        // same deterministic slot assignment it had before the LRU existed.
        int chosen = -1;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (!entries_[i].used) { chosen = static_cast<int>(i); break; }
        }
        if (chosen < 0) {
            size_t victim = 0;
            for (size_t i = 1; i < entries_.size(); ++i)
                if (entries_[i].stamp < entries_[victim].stamp) victim = i;
            chosen = static_cast<int>(victim);
            r.evicted = true;
            r.evicted_slot = chosen;
            r.evicted_key = entries_[victim].key;
        }
        Entry& e = entries_[static_cast<size_t>(chosen)];
        e.key = key;
        e.used = true;
        e.stamp = ++clock_;
        r.slot = chosen;
        return r;
    }

    // Drop every residency record (the caller unbinds materials separately).
    void reset() {
        for (Entry& e : entries_) e = Entry{};
        clock_ = 0;
    }

    // Resident keys, most-recently-acquired first. Test/diagnostic helper.
    std::vector<uint64_t> keys_mru_first() const {
        std::vector<const Entry*> live;
        for (const Entry& e : entries_) if (e.used) live.push_back(&e);
        for (size_t i = 1; i < live.size(); ++i)          // insertion sort: tiny N
            for (size_t j = i; j > 0 && live[j]->stamp > live[j - 1]->stamp; --j)
                std::swap(live[j], live[j - 1]);
        std::vector<uint64_t> out;
        out.reserve(live.size());
        for (const Entry* e : live) out.push_back(e->key);
        return out;
    }

private:
    // One slot's residency record: `key` is the resident atlas's .gtex content
    // hash, and `stamp` is a monotonic acquisition clock rather than a frame
    // or wall-clock time -- both acquire() and touch() bump it, and the lowest
    // stamp among used entries is the eviction victim. key and stamp are
    // meaningless while `used` is false.
    struct Entry {
        uint64_t key   = 0;
        uint64_t stamp = 0;
        bool     used  = false;
    };
    std::vector<Entry> entries_;   // index == slot id
    uint64_t clock_ = 0;
};

// SlotAllocator plus the material bookkeeping the provider needs around it:
// which materials each resident atlas bound, so an eviction can name exactly
// the ones that must fall back to scalar albedo. Deliberately performs no
// registry writes — it reports them, the provider applies them — so the
// ordering can be asserted headlessly without a material registry.
class DetailSlotBinder {
public:
    struct Acquired {
        int              slot = -1;
        bool             reused = false;
        bool             evicted = false;
        uint64_t         evicted_key = 0;
        // Materials the eviction displaced. The caller must unbind these
        // (MaterialRegistrySetGroundTilesetSlot(material, -1)) before binding
        // the new atlas, or they would sample someone else's texels.
        std::vector<int> unbound;
    };

    explicit DetailSlotBinder(int capacity = kSlotAllocatorCapacity)
        : allocator_(capacity) {}

    int capacity() const { return allocator_.capacity(); }
    const SlotAllocator& allocator() const { return allocator_; }

    // Reserve a slot for `key` and report which materials the eviction, if
    // any, displaced. Bookkeeping only: the caller performs the actual
    // registry unbind/bind, and must call bind() once the atlas has loaded or
    // forget() if it never does -- otherwise the slot stays reserved with no
    // materials recorded against it.
    Acquired acquire(uint64_t key) {
        Acquired out;
        const SlotAllocator::Result r = allocator_.acquire(key);
        out.slot = r.slot;
        out.reused = r.reused;
        out.evicted = r.evicted;
        out.evicted_key = r.evicted_key;
        if (!r.evicted) return out;
        const auto victim = bound_.find(r.evicted_key);
        if (victim != bound_.end()) {
            out.unbound = victim->second;
            bound_.erase(victim);
        }
        return out;
    }

    // Record the binding once the atlas actually loaded into its slot.
    // Replaces any previous list for `key` wholesale.
    void bind(uint64_t key, const std::vector<int>& materials) {
        bound_[key] = materials;
    }

    // Drop a reservation whose atlas never loaded (headless cache miss): the
    // slot stays reserved so scheduling stays deterministic, but no material
    // points at it.
    void forget(uint64_t key) {
        bound_.erase(key);
    }

    // Empty the pool and return every material that was bound, so the caller
    // can unbind them all on world (re)connect. Ascending material order.
    //
    // The union is DERIVED from `bound_` here rather than maintained
    // incrementally, and that is the whole point: a running total went wrong in
    // both directions. bind() replaces a key's list wholesale, so a material
    // dropped from the new list would have stayed in a maintained set forever
    // (over-report); and one material can be bound by two atlases at once, so
    // erasing on eviction/forget would drop it while the other atlas still
    // points at it (under-report). `bound_` is the only authority.
    std::vector<int> reset() {
        std::set<int> all;
        for (const auto& kv : bound_)
            all.insert(kv.second.begin(), kv.second.end());
        std::vector<int> out(all.begin(), all.end());
        bound_.clear();
        allocator_.reset();
        return out;
    }

private:
    SlotAllocator                        allocator_;
    std::map<uint64_t, std::vector<int>> bound_;      // gtex key -> material ids
};

} // namespace tileset
