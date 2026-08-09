#pragma once

// The editor's streaming-LOD override, as a property group (property-system
// design S10 "Phase 2 — breadth"). This is the RequiresReload flagship: every
// field here is consumed ONCE, by WorldSession::set_streaming_lod_overrides,
// at the next world connect — editing it live would look applied while nothing
// consumed it, which is exactly what the draft flow exists to prevent.
//
// Why an editor-side struct rather than WorldSession::StreamingLodConfig:
// the config's ring lists are std::vectors, and the schema has no list type
// (adding one is a Phase-4 dynamic-group problem). Encoding each list as ONE
// String field keeps the whole override persistable, FIFO-settable
// (`set stream.lod.scatter_rings 128:2,384:1,900:0`) and diffable, at the cost
// of a text round-trip the hand-written ring widgets do for the panel.
//
// EMPTY STRING IS MEANINGFUL: it maps to an empty ring list, which
// set_streaming_lod_overrides documents as "fall back to the world's own
// values / engine defaults". So a default-constructed StreamingLodPrefs is a
// no-op override, and the sparse save writes nothing for an untouched world.
//
// This header is deliberately ImGui-free AND engine-free (props.h only) so the
// ring parse/format round-trip is unit-testable in the headless suite.

#include "matter/props.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace viewer {

// Mirrors WorldSession::StreamingLodRing without depending on world_session.h.
struct LodRing {
    float radius = 0.0f;
    int   value = 0;
};

// "128:2,384:1,900:0" -> rings. Separators are lenient (comma, semicolon or
// whitespace between pairs; ':' or '=' inside one). A malformed pair ends the
// parse and yields what was read so far — the tolerant-reader rule (S5), since
// this text can arrive from a hand-edited JSON file or a FIFO line.
inline std::vector<LodRing> parse_lod_rings(const std::string& text) {
    std::vector<LodRing> out;
    const char* p = text.c_str();
    while (*p) {
        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        char* end = nullptr;
        const double radius = std::strtod(p, &end);
        if (end == p) break;
        p = end;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != ':' && *p != '=') break;
        ++p;
        const long value = std::strtol(p, &end, 10);
        if (end == p) break;
        p = end;
        LodRing ring;
        ring.radius = static_cast<float>(radius);
        ring.value = static_cast<int>(value);
        out.push_back(ring);
    }
    return out;
}

// Radii print without decimals: the panel drags them in whole metres and a
// "128.000000:2" file entry would be noise in a committed diff.
inline std::string format_lod_rings(const std::vector<LodRing>& rings) {
    std::string out;
    char buf[64];
    for (size_t i = 0; i < rings.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%.0f:%d",
                      static_cast<double>(rings[i].radius), rings[i].value);
        if (!out.empty()) out += ',';
        out += buf;
    }
    return out;
}

struct StreamingLodPrefs {
    // Matches make_streaming_profile's default (on since 2026-07-30).
    bool        terrain_lod_enabled = true;
    // Both empty by default = "use the world's own rings".
    std::string scatter_rings;
    std::string terrain_bands;
    // ---- Streamer pacing (WS2) --------------------------------------------
    // These three have NO "keep the world's own value" encoding, because no
    // world script authors them: the defaults below are matter_stream::Config's
    // own defaults, so applying them unconditionally at connect is a no-op and
    // the sparse save still writes nothing for an untouched world. Same
    // RequiresReload discipline as everything else here — the SectorStreamer is
    // constructed from the resolved profile at connect and never reconfigured.
    float hysteresis = 16.0f;
    int32_t max_inflight = 64;
    int32_t fail_cooldown_updates = 64;
};

inline const matter::props::Group& streaming_lod_group() {
    using matter::props::prop;
    static const auto def = matter::props::group<StreamingLodPrefs>(
        "stream.lod", "Streaming LOD",
        // NOT "Heightfield terrain LOD" any more. The name dated from when
        // LODs 0-4 meshed as a heightfield grid and only LOD 5 was voxels --
        // two different surfaces, which is exactly what made the near/far
        // transition visible as a seam no band tuning could hide. The ladder
        // is one voxel representation at every rung now, so the checkbox turns
        // COARSENING on and off, not a change of representation.
        prop(&StreamingLodPrefs::terrain_lod_enabled, "terrain_lod_enabled")
            .label("Terrain LOD ladder")
            .doc("Coarsen distant terrain by meshing it at a bigger voxel. "
                 "Off: every sector bakes at full detail regardless of "
                 "distance (far more memory and bake time). Every rung is the "
                 "same voxel mesher -- only the voxel size changes.")
            .requires_reload(),
        prop(&StreamingLodPrefs::scatter_rings, "scatter_rings")
            .label("Scatter rings")
            .doc("radius:tier pairs, innermost first (tier 2 = densest). "
                 "Empty keeps the world's own rings.")
            .requires_reload(),
        prop(&StreamingLodPrefs::terrain_bands, "terrain_bands")
            .label("Terrain LOD bands")
            .doc("radius:lod pairs, innermost first (5 = native voxel). "
                 "Empty keeps the world's own bands.")
            .requires_reload(),
        prop(&StreamingLodPrefs::hysteresis, "hysteresis")
            .label("Ring hysteresis").range(0.0f, 512.0f).units("m")
            .env("MATTER_STREAM_HYSTERESIS")
            .doc("Extra distance a sector must fall behind its ring before it "
                 "is demoted or evicted. Too small and a camera parked on a "
                 "ring boundary churns.")
            .requires_reload(),
        prop(&StreamingLodPrefs::max_inflight, "max_inflight")
            .label("Max in-flight").range(1.0f, 256.0f)
            .env("MATTER_STREAM_INFLIGHT")
            .doc("Sectors that may be dispatched but not yet ACKNOWLEDGED. By "
                 "Little's law this bounds fill rate at max_inflight / "
                 "publish-latency no matter how fast sectors bake.")
            .requires_reload(),
        prop(&StreamingLodPrefs::fail_cooldown_updates, "fail_cooldown_updates")
            .label("Fail cooldown").range(0.0f, 1024.0f).units("updates")
            .env("MATTER_STREAM_FAIL_COOLDOWN")
            .doc("Streamer updates a failed sector waits before it may be "
                 "requested again.")
            .requires_reload());
    return def.group();
}

}  // namespace viewer
