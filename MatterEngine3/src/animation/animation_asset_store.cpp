// MatterEngine3/src/animation/animation_asset_store.cpp
//
// Implementation of `AnimationAssetStore` (see the header). Assets are held
// by `std::unique_ptr` inside a `std::map`, so a `const AnimAsset*` handed
// out by `insert`/`find` stays valid across later inserts and erases of
// *other* entries — node-based storage never reseats existing elements.
//
// Only `erase` invalidates a pointer, and it invalidates just that one.
// There is no internal synchronisation: the store is expected to be owned by
// whichever thread drives asset loading.
#include "animation/animation_asset_store.h"
namespace matter::animation {
// Insert, or return the existing entry when (hash, nonce) is already present.
// Three distinct outcomes:
//  - new key: takes ownership and returns the stored asset,
//  - key present and byte-identical: returns the EXISTING pointer and drops
//    the argument (so callers must use the return value, not their copy),
//  - key present but different content: returns nullptr and inserts nothing.
// The last case is an identity violation — two different assets claiming the
// same hash+nonce — and is reported rather than silently resolved.
const AnimAsset* AnimationAssetStore::insert(AnimAsset asset) {
    const Key key{asset.resolved_hash,asset.nonce.high,asset.nonce.low};
    auto it=assets_.find(key);
    if (it != assets_.end()) return *it->second == asset ? it->second.get() : nullptr;
    auto owned=std::make_unique<AnimAsset>(std::move(asset)); const AnimAsset* result=owned.get(); assets_.emplace(key,std::move(owned)); return result;
}
const AnimAsset* AnimationAssetStore::find(uint64_t hash, BuildNonce nonce) const {
    const auto it=assets_.find({hash,nonce.high,nonce.low}); return it==assets_.end()?nullptr:it->second.get();
}
// Identity (pointer) membership test, not a key lookup: O(n) linear scan over
// every stored asset. Use `find` when you have the hash and nonce.
bool AnimationAssetStore::contains(const AnimAsset* asset) const {
    for (const auto& entry : assets_)
        if (entry.second.get() == asset) return true;
    return false;
}
// Erase by pointer identity; O(n) scan. Returns false for null or for a
// pointer this store does not own. `asset` dangles after a true return.
bool AnimationAssetStore::erase(const AnimAsset* asset) {
    if (!asset) return false;
    for (auto it = assets_.begin(); it != assets_.end(); ++it) {
        if (it->second.get() != asset) continue;
        assets_.erase(it);
        return true;
    }
    return false;
}
} // namespace matter::animation
