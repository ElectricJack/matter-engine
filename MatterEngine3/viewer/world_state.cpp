#include "world_source.h"

namespace viewer {

void WorldState::reset(const WorldManifest& m) {
    ++version_;
    entries_ = m.instances;
}

const WorldManifestEntry* WorldState::find(uint32_t instance_id) const {
    for (const auto& e : entries_)
        if (e.instance_id == instance_id) return &e;
    return nullptr;
}

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
