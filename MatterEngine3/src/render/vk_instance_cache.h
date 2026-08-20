#ifndef VIEWER_VK_INSTANCE_CACHE_H
#define VIEWER_VK_INSTANCE_CACHE_H

// MatterEngine3/src/render/vk_instance_cache.h
//
// Memoisation between the provider layer and the renderer. Each frame the
// engine turns the resolved instance set (ResolvedInstance, from
// MatterEngine3/src/provider/sector_resolver.h — one entry per placed thing in
// the world) into the renderer's per-draw instances (VkSceneInstance, from
// vk_scene_renderer.h). That expansion is one-to-MANY and is O(world), so a
// streaming world that publishes one new sector must not pay for all of it
// again: this class is what stops that. See the class comment below for how
// the two levels differ and which invalidator to use.
//
// Owned by the engine (`viewer::VulkanInstanceCache vk_instance_cache` in
// MatterEngine3/src/matter_engine.cpp) and used only from the render/publish
// path — it holds no Vulkan handles and takes no locks, so it must stay on one
// thread. `expansion_count()` is reported as FrameStats::
// vk_instance_cache_expansions, which is the metric that tells you whether the
// cache is actually working: it should stay flat while the camera moves.
//
// Correctness rule: the cache is keyed on a FINGERPRINT of the resolved set,
// never on identity or pointers, so a stale entry survives only if it is
// byte-identical in the fields folded below.

#include "vk_scene_renderer.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace viewer {

struct ResolvedInstance;

// FNV-1a over the (part_hash, stable_id, transform, segment) of every entry, in
// order. Order-sensitive: the same instances in a different order fingerprint
// differently and force a rebuild. Only those four fields participate, so a
// change to anything else in ResolvedInstance is invisible here and must be
// signalled by calling one of the invalidators. O(n) over the whole set.
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
    // True only if the flat level is valid AND `resolved` fingerprints
    // identically — i.e. the cached instances() may be reused verbatim. Costs
    // a full fingerprint pass over `resolved`.
    bool matches(const std::vector<ResolvedInstance>& resolved) const noexcept;
    // Adopts `instances` as the expansion of `resolved` (moved, not copied) and
    // marks the flat level valid. Also bumps expansion_count(), which is what
    // the frame stats count as "a full re-expansion happened".
    void store(const std::vector<ResolvedInstance>& resolved,
               std::vector<VkSceneInstance> instances);
    void invalidate() noexcept;
    // Forces the next frame to rebuild the flat set while keeping per-source
    // memos usable. Safe only when nothing has been released or unregistered.
    void invalidate_expansion() noexcept;
    // Drops every per-source memo but keeps the flat set. For changes that
    // alter what an expansion *produces* rather than which sources exist.
    void invalidate_sources() noexcept;
    // The cached flat expansion. Returns a reference to internal storage that
    // any store()/invalidate() call invalidates, and it is only MEANINGFUL
    // after matches() returned true — otherwise it is whatever the last valid
    // expansion left behind, or empty.
    const std::vector<VkSceneInstance>& instances() const noexcept;
    // Lifetime count of full flat expansions stored. Reported as
    // FrameStats::vk_instance_cache_expansions; never reset.
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
    size_t source_memo_size() const noexcept;

private:
    struct SourceEntry {
        uint64_t key = 0;
        std::vector<VkSceneInstance> instances;
    };

    // Flat level: the fingerprint and element count of the resolved set that
    // produced instances_. `valid_` is the real gate — a zero fingerprint is
    // also what invalidate_expansion() leaves behind, so the two are always
    // cleared together.
    uint64_t fingerprint_ = 0;
    size_t resolved_count_ = 0;
    bool valid_ = false;
    uint64_t expansion_count_ = 0;
    uint64_t source_expansion_count_ = 0;
    std::vector<VkSceneInstance> instances_;
    // Per-source memos keyed by ResolvedInstance::stable_id. The id only
    // selects the candidate; SourceEntry::key holds the full per-instance
    // fingerprint and is what decides a hit, so a reused id whose placement
    // changed misses. Bounded by prune_sources(), not by an LRU.
    std::unordered_map<uint64_t, SourceEntry> sources_;
};

} // namespace viewer

#endif // VIEWER_VK_INSTANCE_CACHE_H
