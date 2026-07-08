// autoremesher/remesh.h — public API for the vendored headless retopo pipeline.
//
// Single entrypoint: autoremesher::remesh(). Deterministic given (input,
// options). Never throws. See docs/superpowers/specs/2026-07-07-autoremesher-
// integration-design.md for the design rationale.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace autoremesher {

struct Mesh {
    std::vector<float>    positions;  // xyz, xyz, ...  (size = 3 * vertexCount)
    std::vector<uint32_t> indices;    // 3 per triangle
};

struct Options {
    float    target_ratio    = 1.0f;   // relative to input tri count, clamped (0, 4.0]
    // v1: accepted but ignored — reserved for future implementation.
    // Upstream's AutoRemesher class exposes no setter for parameterizer
    // iteration count; wiring this up requires modifying vendored sources
    // (deferred to Phase 6+). MSL / cache-key logic MAY still include this
    // field in the cache key so future changes invalidate old cache entries.
    int      iterations      = 3;
    // v1: accepted but ignored — reserved for future implementation.
    // Upstream's MIQ solver has no seed setter. Same rationale as `iterations`.
    uint32_t seed            = 0;
    int      timeout_seconds = 60;     // 0 = no limit
    // Pinned for FP-summation determinism (>= 1). NOTE: the TBB scheduler is
    // constructed once for the process lifetime on the FIRST remesh() call,
    // so the `threads` value from that first invocation is baked in for all
    // subsequent calls in the same process. A later call passing a different
    // `threads` value silently reuses the first-call setting.
    int      threads         = 1;
};

struct Result {
    bool        ok = false;
    Mesh        mesh;
    std::string err;
    double      elapsed_seconds = 0.0;
};

// Runs the sanitize → parameterize → quad-extract → triangulate pipeline.
// Never throws. Never modifies `in`. Deterministic given (in, opts).
//
// v1 note: only `target_ratio`, `timeout_seconds`, and `threads` (first call
// only, see Options) affect output. `iterations` and `seed` are reserved
// for future implementation and are silently ignored by this version.
Result remesh(const Mesh& in, const Options& opts);

// Version string embedded at compile time. Change this string whenever an
// extraction sync changes the vendored pipeline sources — MSL's cache key
// depends on it.
extern const char* const AUTOREMESHER_CORE_VERSION;

} // namespace autoremesher
