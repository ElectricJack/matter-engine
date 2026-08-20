#pragma once
// MatterEngine3/src/animation/animation_asset_store.h
//
// Process-lifetime owner of loaded animation assets, keyed by
// (resolved_hash, nonce.high, nonce.low) — the full bundle identity, not
// just the hash, so two bakes of the same part coexist while old runtime
// instances still reference the older one.
//
// It is a pure ownership table: no loading, no eviction policy, no pose or
// playback state. Callers load with `load_committed_animation_bundle`
// (anim_bundle.h), hand the result to `insert`, and thereafter refer to the
// asset by the returned `const AnimAsset*`.
//
// Lifetime: assets live in a node-based `std::map`, so handles stay valid
// across other inserts and erases; only erasing an asset invalidates its own
// pointer, and nothing here refcounts, so the caller must not erase an asset
// a runtime instance still holds.
//
// Threading: no internal synchronisation. Treat the store as owned by one
// thread (the asset-loading side), or guard it externally.
#include "animation/anim_asset.h"
#include <map>
#include <memory>

namespace matter::animation {
// Immutable cache-side ownership. Phase B instances must retain this exact
// identity with their PartStore peer; it intentionally owns no pose state.
// Keyed by the full identity, so a rebake (new nonce) is a different entry
// rather than an overwrite; `insert` refuses to let two different assets
// share one identity.
class AnimationAssetStore {
public:
    // Takes ownership and returns the stored asset. If the identity is
    // already present, returns the EXISTING pointer when the content is
    // identical (the argument is dropped), or nullptr when it differs —
    // always use the return value rather than assuming the argument landed.
    const AnimAsset* insert(AnimAsset asset);
    // nullptr when that identity is not loaded — a normal miss, not an error.
    const AnimAsset* find(uint64_t resolved_hash, BuildNonce nonce) const;
    // Pointer-identity membership; O(n) scan, not a keyed lookup.
    bool contains(const AnimAsset* asset) const;
    // Erase by pointer identity; O(n) scan. False for null or a foreign
    // pointer. The pointer dangles after a true return, and nothing checks
    // whether a runtime instance still references it.
    bool erase(const AnimAsset* asset);
    size_t size() const { return assets_.size(); }
private:
    // Lexicographic over (resolved hash, nonce.high, nonce.low).
    struct Key { uint64_t hash, high, low; bool operator<(const Key& x) const { return hash!=x.hash?hash<x.hash:high!=x.high?high<x.high:low<x.low; } };
    std::map<Key, std::unique_ptr<AnimAsset>> assets_;
};
} // namespace matter::animation
