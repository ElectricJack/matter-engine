#pragma once
#include "vt_surface_boundary.h"

namespace vt {
// Explicit physical continuity, independent of material identity. Both sides
// use their authored world frames. Zero domains never authorize a connection.
struct VtSurfaceConnectionPair {
    uint64_t first=0,second=0,domain=0;
    uint32_t first_rung=0,second_rung=0;
};
struct VtSurfaceLinkGpu {
    uint32_t edge[4]{}; // source triangle/edge, target triangle/chart
    uint32_t target[4]{}; // slot, generation, atlas width/height
    VtPageMetadata metadata;
    float interval[4]{};
    float transform[12]{}; // source-local -> target-local, rigid row-major
};
struct VtSurfaceLinksHeaderGpu {
    VtPageMetadata metadata;
    uint32_t source[4]{}; // slot, generation, link count, reserved
};
static_assert(sizeof(VtSurfaceLinkGpu)==176 && sizeof(VtSurfaceLinksHeaderGpu)==96,
              "vt_surface_walk.glsl immutable connection table ABI");
static_assert(offsetof(VtSurfaceLinkGpu,metadata)==32,
              "vt_surface_walk.glsl retained metadata-reference offset");
struct VtSurfaceConnectionState;
} // namespace vt
