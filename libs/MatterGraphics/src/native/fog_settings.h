#pragma once
#include "cloud_layers.h"
#include <limits>

namespace matter {

// Everything IN the froxel volume. Two independent things live here:
//
//   1. Ground fog — density / floor / falloff / color / wind. An exponential
//      falling off upward from `floor` with no upper bound. This is what most
//      worlds mean by "fog" and it is always evaluated.
//
//   2. Cloud decks — `clouds`, up to kMaxCloudLayers of them, each bounded
//      between its own min and max height. These ADD extinction on top of the
//      ground fog; a world with no enabled layer costs nothing for them
//      (vol_density.comp is specialized on the enabled count and the loop is
//      compiled away at zero).
//
// The two are deliberately not unified. Ground fog has no top, clouds are
// defined by having one, and every existing world depends on the first shape.
struct FogSettings {
    float density  = 0.0f;
    float floor    = 0.0f;
    float falloff  = 30.0f;
    float color[3] = {0.9f, 0.92f, 0.95f};
    float wind[3]  = {0.0f, 0.0f, 0.0f};

    // Fixed-size, not a vector: this struct is copied whole through
    // RenderOptions::fog_override every frame, is the target of a props
    // Group whose Descs are byte offsets into it, and is the thing a world
    // reload memcpy-assigns. A ceiling of 4 costs 4 * sizeof(CloudLayer)
    // (~200 bytes) and buys all of that staying trivially copyable.
    // `cloud_count` bounds the meaningful prefix; entries past it are
    // untouched defaults.
    CloudLayer clouds[kMaxCloudLayers]{};
    int32_t cloud_count = 0;

    // Legacy single bounded layer (pre-2026-07-31). Kept ONLY as an authoring
    // alias: world_definition_loader maps minHeight/maxHeight/noiseScale onto
    // clouds[0] and sets this so the composite pass's above-the-deck early-out
    // still knows a deck exists. Nothing reads min_height/max_height for
    // shading any more — vol_density.comp sees the CloudLayer.
    bool height_layer = false;
    float min_height = 0.0f;
    float max_height = 0.0f;
    float noise_scale = 0.0018f;
};

// The number of leading `clouds` entries that are enabled AND well formed.
// This is the value vol_density.comp is specialized on, so it must be a
// PREFIX count: a disabled layer 0 with an enabled layer 1 would otherwise
// compile a 2-layer permutation that evaluates a dead entry. compact_clouds()
// below is what guarantees the prefix property.
//
// DERIVED FROM THE LAYERS, NOT FROM `cloud_count`. This used to clamp the scan
// to `fog.cloud_count`, which made that field a second, authoritative gate —
// and every write path that sets a layer WITHOUT running compaction left it
// stale. Enabling a deck through the World-scope property file, through
// MATTER_CLOUD_LAYER<i>, or through the FIFO `set` then rendered nothing at
// all, because the count those paths never touch stayed at whatever the world
// script had compacted to. The 0-deck case was the sharp one: a world that
// authors no clouds pins cloud_count at 0, so a deck enabled by an override
// file was invisible on every launch until the user happened to nudge some
// other cloud field in the panel and trip the panel's compaction — which
// resurrected it, making the bug look intermittent.
//
// Scanning the array directly is equivalent wherever compaction HAS run
// (compact_clouds sets cloud_count to exactly the number of leading live
// layers, and it parks every non-live layer behind them, so the scan stops at
// the same index), and correct where it has not. `cloud_count` survives as the
// cached result and as the world's requested count for the overflow warning in
// vk_volumetrics.cpp; nothing about what renders depends on it any more.
inline int32_t active_cloud_count(const FogSettings& fog) {
    int32_t n = 0;
    for (int32_t i = 0; i < kMaxCloudLayers; ++i) {
        if (!fog.clouds[i].enabled) break;
        if (!(fog.clouds[i].max_height > fog.clouds[i].min_height)) break;
        ++n;
    }
    return n;
}

inline float active_cloud_top(const FogSettings& fog) {
    const int32_t count = active_cloud_count(fog);
    if (count == 0) return 0.0f;
    float top = -std::numeric_limits<float>::infinity();
    for (int32_t i = 0; i < count; ++i) {
        if (std::isfinite(fog.clouds[i].max_height))
            top = std::fmax(top, fog.clouds[i].max_height);
    }
    return std::isfinite(top) ? top : 0.0f;
}

// Slides every enabled layer down to the front, so `clouds[0 .. count)` is
// exactly the live set. Call after any edit that can disable a middle layer —
// the editor toggling `clouds[0].enabled` off with `clouds[1]` still on is
// the ordinary case, and without this that world would render layer 1's
// parameters as if they were layer 0's, or nothing at all.
//
// A STABLE PARTITION, not a wipe: non-surviving layers (disabled, or
// degenerate with max_height <= min_height) are moved to the tail WITH their
// parameters intact, in their original relative order, rather than being
// reset to CloudLayer{}. The prefix property everything above depends on
// only constrains `[0, cloud_count)` — what sits behind that point is free to
// be anything, including a parked deck's untouched values. Zeroing it was
// gratuitous, and destructive: switching a deck off ran through here on
// every edit (property_editor.cpp), so the old behaviour deleted a deck's
// authored parameters the instant it was switched off, with no way to
// recover them by switching it back on (see the
// editor-cloud-deck-cannot-be-enabled issue).
inline void compact_clouds(FogSettings& fog) {
    CloudLayer snapshot[kMaxCloudLayers];
    for (int32_t i = 0; i < kMaxCloudLayers; ++i) snapshot[i] = fog.clouds[i];

    CloudLayer parked[kMaxCloudLayers];
    int32_t out = 0;
    int32_t parked_n = 0;
    for (int32_t i = 0; i < kMaxCloudLayers; ++i) {
        const CloudLayer& layer = snapshot[i];
        const bool live =
            layer.enabled && layer.max_height > layer.min_height;
        if (live) {
            fog.clouds[out++] = layer;
        } else {
            parked[parked_n++] = layer;
        }
    }
    for (int32_t i = 0; i < parked_n; ++i) fog.clouds[out + i] = parked[i];
    fog.cloud_count = out;
}

} // namespace matter
