// MatterEngine3/src/render/vk_instance_cache.cpp
//
// Implementation of VulkanInstanceCache (see vk_instance_cache.h for what the
// two cache levels are for). Nothing here touches Vulkan or the GPU: it is
// bookkeeping over vectors of VkSceneInstance, and its whole job is to make
// the O(world) expansion in matter_engine.cpp run for new sources only.
//
// The fingerprint is defined TWICE on purpose — once per instance
// (fingerprint_one, for the per-source key) and once over a whole set
// (fingerprint_resolved_instances, for the flat key) — folding exactly the
// same four fields in the same order, so the two levels can never disagree
// about what "the same source" means. Change one fold and you must change the
// other.

#include "vk_instance_cache.h"

#include "../provider/sector_resolver.h"

#include <utility>

namespace viewer {
namespace {

// FNV-1a over exactly the fields fingerprint_resolved_instances folds for one
// instance, so the per-source key and the whole-set fingerprint always agree on
// what "the same source" means.
uint64_t fingerprint_one(const ResolvedInstance& instance) noexcept {
    uint64_t fingerprint = 1469598103934665603ull;
    const auto fold = [&fingerprint](const void* bytes, size_t size) {
        const auto* data = static_cast<const unsigned char*>(bytes);
        for (size_t index = 0; index < size; ++index)
            fingerprint = (fingerprint ^ data[index]) * 1099511628211ull;
    };
    fold(&instance.part_hash, sizeof(instance.part_hash));
    fold(&instance.stable_id, sizeof(instance.stable_id));
    fold(instance.transform, sizeof(instance.transform));
    fold(&instance.segment, sizeof(instance.segment));
    return fingerprint;
}

} // namespace

// FNV-1a over the concatenated bytes of every instance's four identity fields.
// Because it is a running fold, it is sensitive to ORDER as well as content:
// the same set resolved in a different order is a miss. O(n) with no
// allocation; called once per matches() and once per store().
uint64_t fingerprint_resolved_instances(
    const std::vector<ResolvedInstance>& resolved) noexcept {
    uint64_t fingerprint = 1469598103934665603ull;
    const auto fold = [&fingerprint](const void* bytes, size_t size) {
        const auto* data = static_cast<const unsigned char*>(bytes);
        for (size_t index = 0; index < size; ++index)
            fingerprint = (fingerprint ^ data[index]) * 1099511628211ull;
    };
    for (const ResolvedInstance& instance : resolved) {
        fold(&instance.part_hash, sizeof(instance.part_hash));
        fold(&instance.stable_id, sizeof(instance.stable_id));
        fold(instance.transform, sizeof(instance.transform));
        fold(&instance.segment, sizeof(instance.segment));
    }
    return fingerprint;
}

bool VulkanInstanceCache::matches(
    const std::vector<ResolvedInstance>& resolved) const noexcept {
    return valid_ && resolved_count_ == resolved.size() &&
           fingerprint_ == fingerprint_resolved_instances(resolved);
}

void VulkanInstanceCache::store(
    const std::vector<ResolvedInstance>& resolved,
    std::vector<VkSceneInstance> instances) {
    fingerprint_ = fingerprint_resolved_instances(resolved);
    resolved_count_ = resolved.size();
    instances_ = std::move(instances);
    valid_ = true;
    ++expansion_count_;
}

void VulkanInstanceCache::invalidate() noexcept {
    invalidate_expansion();
    invalidate_sources();
}

void VulkanInstanceCache::invalidate_sources() noexcept {
    sources_.clear();
}

void VulkanInstanceCache::invalidate_expansion() noexcept {
    fingerprint_ = 0;
    resolved_count_ = 0;
    valid_ = false;
    instances_.clear();
}

const std::vector<VkSceneInstance>& VulkanInstanceCache::instances() const noexcept {
    return instances_;
}

uint64_t VulkanInstanceCache::expansion_count() const noexcept {
    return expansion_count_;
}

const std::vector<VkSceneInstance>* VulkanInstanceCache::find_source(
    const ResolvedInstance& source) const noexcept {
    const auto found = sources_.find(source.stable_id);
    if (found == sources_.end()) return nullptr;
    // The stable id only selects the candidate; the full fingerprint decides.
    // A reused id whose placement changed therefore misses and re-expands.
    if (found->second.key != fingerprint_one(source)) return nullptr;
    return &found->second.instances;
}

void VulkanInstanceCache::store_source(
    const ResolvedInstance& source,
    std::vector<VkSceneInstance> expansion) {
    SourceEntry& entry = sources_[source.stable_id];
    entry.key = fingerprint_one(source);
    entry.instances = std::move(expansion);
    ++source_expansion_count_;
}

// Rebuilds sources_ containing only the ids still present in `resolved`,
// dropping the memos of evicted sectors that would otherwise accumulate
// forever in a streaming world. Early-outs when the map is no larger than the
// live set, so the common frame pays only a size comparison; when it does run
// it allocates a whole replacement map and moves the survivors across.
void VulkanInstanceCache::prune_sources(
    const std::vector<ResolvedInstance>& resolved) {
    if (sources_.size() <= resolved.size()) return;
    std::unordered_map<uint64_t, SourceEntry> kept;
    kept.reserve(resolved.size());
    for (const ResolvedInstance& source : resolved) {
        auto found = sources_.find(source.stable_id);
        if (found == sources_.end()) continue;
        kept.emplace(found->first, std::move(found->second));
    }
    sources_ = std::move(kept);
}

size_t VulkanInstanceCache::source_memo_size() const noexcept {
    return sources_.size();
}

} // namespace viewer
