// MatterEngine3/src/render/world_state.cpp — viewer::WorldState, the renderer's
// mirror of the world manifest.
//
// WorldState is declared in MatterEngine3/src/provider/world_source.h alongside
// WorldManifest, WorldDelta and the WorldProvider interface; this file is its
// entire implementation. It holds one thing: the flat list of
// WorldManifestEntry the provider last published. No GPU state and no part
// payloads — part data lives in PartStore.
//
// How it fits: a provider (LocalProvider in-process, a NetworkProvider later)
// hands out a WorldManifest at connect() and WorldDeltas from poll_deltas();
// the renderer feeds those in with reset() / apply() and reads entries() back.
// The monotonic version() bumped here is the cache key resolvers and the
// composer test to skip per-frame re-derivation when the world has not moved.
//
// Conventions and gotchas:
//   - Entries are keyed by WorldManifestEntry::instance_id. find() is a linear
//     scan; apply() builds a temporary index for bulk updates, giving expected
//     O(entries + removed + added) work instead of one full scan per addition.
//   - Entry ORDER is not stable and carries no meaning. A removal erases from
//     the middle (shifting the tail) and an unseen add appends, so a position
//     in entries() must never be treated as an identity across calls.
//   - version_ is bumped by reset() and apply() unconditionally — including
//     for an empty delta or an identical manifest. A changed version means
//     "may have changed", never "did change".
//   - Pointers from find() and references into entries() are invalidated by
//     the next reset()/apply() (vector reallocation and erase).
//   - No internal synchronization; the caller owns thread affinity.

#include "world_source.h"
#include <algorithm>
#include <unordered_map>
#include <utility>

namespace viewer {

void WorldState::reset(const WorldManifest& m) {
    ++version_;
    entries_ = m.instances;
}

// Linear scan by instance_id. A null return is the normal "not in the current
// manifest" outcome, not an error. The pointer is only valid until the next
// reset()/apply().
const WorldManifestEntry* WorldState::find(uint32_t instance_id) const {
    for (const auto& e : entries_)
        if (e.instance_id == instance_id) return &e;
    return nullptr;
}

// Applies one delta in place. Removals are processed before adds, so a delta
// that removes and re-adds the same instance_id ends with the added entry; an
// add whose id is already present overwrites that entry where it sits (the
// "move" case) instead of appending a duplicate.
void WorldState::apply(const WorldDelta& d) {
    ++version_;
    // Count removals, then compact once. Repeated removal requests consume
    // successive occurrences, preserving the old first-match semantics even
    // if a reset manifest contained duplicate ids. Keep survivor order.
    if (!d.removed.empty()) {
        std::unordered_map<uint32_t, size_t> remaining;
        remaining.reserve(d.removed.size());
        for (uint32_t id : d.removed) ++remaining[id];
        size_t out = 0;
        for (size_t in = 0; in < entries_.size(); ++in) {
            auto removed = remaining.find(entries_[in].instance_id);
            if (removed != remaining.end() && removed->second) {
                --removed->second;
                continue;
            }
            if (out != in) entries_[out] = std::move(entries_[in]);
            ++out;
        }
        entries_.resize(out);
    }
    // Tiny edits need at most four scans and avoid allocating an index over
    // the whole forest for, for example, a single moved object.
    if (d.added.size() <= 4) {
        for (const auto& add : d.added) {
            bool replaced = false;
            for (auto& entry : entries_) {
                if (entry.instance_id == add.instance_id) {
                    entry = add; replaced = true; break;
                }
            }
            if (!replaced) entries_.push_back(add);
        }
        return;
    }
    // The forest publishes millions of leaves. Scanning the growing vector
    // for every one made a single delta quadratic and stalled the render
    // thread for minutes. Store indices, never pointers: appending can move
    // the vector. emplace retains the FIRST match and later adds overwrite it.
    std::unordered_map<uint32_t, size_t> positions;
    positions.reserve(std::max(entries_.size(), d.added.size()));
    for (size_t i = 0; i < entries_.size(); ++i)
        positions.emplace(entries_[i].instance_id, i);
    for (const auto& add : d.added) {
        const auto inserted = positions.emplace(add.instance_id, entries_.size());
        if (inserted.second) entries_.push_back(add);
        else entries_[inserted.first->second] = add;
    }
}

} // namespace viewer
