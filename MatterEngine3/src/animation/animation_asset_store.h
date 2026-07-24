#pragma once
#include "animation/anim_asset.h"
#include <map>
#include <memory>

namespace matter::animation {
// Immutable cache-side ownership. Phase B instances must retain this exact
// identity with their PartStore peer; it intentionally owns no pose state.
class AnimationAssetStore {
public:
    const AnimAsset* insert(AnimAsset asset);
    const AnimAsset* find(uint64_t resolved_hash, BuildNonce nonce) const;
    size_t size() const { return assets_.size(); }
private:
    struct Key { uint64_t hash, high, low; bool operator<(const Key& x) const { return hash!=x.hash?hash<x.hash:high!=x.high?high<x.high:low<x.low; } };
    std::map<Key, std::unique_ptr<AnimAsset>> assets_;
};
} // namespace matter::animation
