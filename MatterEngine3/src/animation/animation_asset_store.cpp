#include "animation/animation_asset_store.h"
namespace matter::animation {
const AnimAsset* AnimationAssetStore::insert(AnimAsset asset) {
    const Key key{asset.resolved_hash,asset.nonce.high,asset.nonce.low};
    auto it=assets_.find(key);
    if (it != assets_.end()) return *it->second == asset ? it->second.get() : nullptr;
    auto owned=std::make_unique<AnimAsset>(std::move(asset)); const AnimAsset* result=owned.get(); assets_.emplace(key,std::move(owned)); return result;
}
const AnimAsset* AnimationAssetStore::find(uint64_t hash, BuildNonce nonce) const {
    const auto it=assets_.find({hash,nonce.high,nonce.low}); return it==assets_.end()?nullptr:it->second.get();
}
} // namespace matter::animation
