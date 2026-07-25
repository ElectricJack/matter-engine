#ifndef VIEWER_VK_INSTANCE_CACHE_H
#define VIEWER_VK_INSTANCE_CACHE_H

#include "vk_scene_renderer.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace viewer {

struct ResolvedInstance;

uint64_t fingerprint_resolved_instances(
    const std::vector<ResolvedInstance>& resolved) noexcept;

// Caches the expansion of a resolved instance set into renderer instances.
//
// Two levels, because the two things that invalidate them are very different:
//
//   * The flat level (matches()/store()/instances()) answers "is the whole
//     expanded set still current?". It is missed by any change at all to the
//     resolved set -- including a streaming world publishing one new sector.
//
//   * The per-source level (find_source()/store_source()) memoises the
//     expansion of ONE resolved instance, keyed by its stable id. A publish
//     only adds a source, so on the rebuild that follows, every pre-existing
//     source is served from this level and only the new one is expanded. This
//     is what keeps a continuously streaming world from re-expanding its whole
//     instance set every frame.
//
// invalidate() drops BOTH levels and is what every existing caller wants: each
// of its call sites accompanies a release_part()/store->release(), after which
// a memoised expansion could name a part the renderer no longer has registered.
// invalidate_expansion() drops only the flat level and is for changes that
// purely add resolved instances.
class VulkanInstanceCache {
public:
    bool matches(const std::vector<ResolvedInstance>& resolved) const noexcept;
    void store(const std::vector<ResolvedInstance>& resolved,
               std::vector<VkSceneInstance> instances);
    void invalidate() noexcept;
    // Forces the next frame to rebuild the flat set while keeping per-source
    // memos usable. Safe only when nothing has been released or unregistered.
    void invalidate_expansion() noexcept;
    // Drops every per-source memo but keeps the flat set. For changes that
    // alter what an expansion *produces* rather than which sources exist.
    void invalidate_sources() noexcept;
    const std::vector<VkSceneInstance>& instances() const noexcept;
    uint64_t expansion_count() const noexcept;

    // Per-source memo. `source` is matched on its full identity (part hash,
    // stable id, transform, segment), so a moved or re-pointed instance misses.
    // Returns nullptr on a miss.
    const std::vector<VkSceneInstance>* find_source(
        const ResolvedInstance& source) const noexcept;
    // Records `expansion` as the expansion of `source`. Callers must only
    // memoise a *complete* expansion -- see the note in matter_engine.cpp.
    void store_source(const ResolvedInstance& source,
                      std::vector<VkSceneInstance> expansion);
    // Drops memos for sources absent from `resolved`, bounding the map to the
    // live set (a streaming world would otherwise accumulate evicted sectors).
    void prune_sources(const std::vector<ResolvedInstance>& resolved);
    uint64_t source_expansion_count() const noexcept;
    size_t source_memo_size() const noexcept;

private:
    struct SourceEntry {
        uint64_t key = 0;
        std::vector<VkSceneInstance> instances;
    };

    uint64_t fingerprint_ = 0;
    size_t resolved_count_ = 0;
    bool valid_ = false;
    uint64_t expansion_count_ = 0;
    uint64_t source_expansion_count_ = 0;
    std::vector<VkSceneInstance> instances_;
    std::unordered_map<uint64_t, SourceEntry> sources_;
};

} // namespace viewer

#endif // VIEWER_VK_INSTANCE_CACHE_H
