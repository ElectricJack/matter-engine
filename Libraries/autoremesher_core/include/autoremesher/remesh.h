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
    int      iterations      = 3;      // upstream default
    uint32_t seed            = 0;      // deterministic RNG seed
    int      timeout_seconds = 60;     // 0 = no limit
    int      threads         = 1;      // pinned for determinism (>= 1)
};

struct Result {
    bool        ok = false;
    Mesh        mesh;
    std::string err;
    double      elapsed_seconds = 0.0;
};

// Runs the sanitize → parameterize → quad-extract → triangulate pipeline.
// Never throws. Never modifies `in`. Deterministic given (in, opts).
Result remesh(const Mesh& in, const Options& opts);

// Version string embedded at compile time. Change this string whenever an
// extraction sync changes the vendored pipeline sources — MSL's cache key
// depends on it.
extern const char* const AUTOREMESHER_CORE_VERSION;

} // namespace autoremesher
