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
//   - Entries are keyed by WorldManifestEntry::instance_id, but the vector is
//     neither sorted nor indexed: find() and every lookup inside apply() are
//     linear scans, so a delta costs O((removed + added) * entries).
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
    // Removals first so a same-frame re-add of an id is honored.
    for (uint32_t id : d.removed) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].instance_id == id) {
                entries_.erase(entries_.begin() + i);
                break;
            }
        }
    }
    // Adds: replace existing id in place (a "move"), else append.
    for (const auto& add : d.added) {
        bool replaced = false;
        for (auto& e : entries_) {
            if (e.instance_id == add.instance_id) { e = add; replaced = true; break; }
        }
        if (!replaced) entries_.push_back(add);
    }
}

} // namespace viewer
