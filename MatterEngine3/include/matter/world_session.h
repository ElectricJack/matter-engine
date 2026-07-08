#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "raylib.h"   // Camera3D — POD math types are part of the API by design

#include "matter/events.h"
#include "matter/query.h"

namespace matter {

struct WorldDesc {
    const char* schemas_dir    = nullptr;
    const char* world_data_dir = nullptr;
    const char* world_name     = nullptr;  // world subdir of world_data_dir
    const char* shared_lib_dir = nullptr;  // shared .js library dir
};

enum class RenderPath { GpuDriven, Raytrace };
enum class ResolverKind { SectorLod, PassThrough };

struct RenderOptions {
    RenderPath   path     = RenderPath::GpuDriven;
    ResolverKind resolver = ResolverKind::SectorLod;
    bool  wireframe       = false;
    bool  hiz_occlusion   = false;    // default OFF (known false-positive issue)
    float pixel_budget    = 0.0f;     // 0 = default (1.0); clamped to [0.05, 4.0]
    float active_radius   = 0.0f;     // SectorLod knob; 0 = default (64.0)
    float min_projected_size = 0.0f;  // SectorLod sub-pixel cull; 0 = off
};

struct FrameStats {
    // per-frame timings (ms)
    float resolve_ms = 0, build_ms = 0, draw_ms = 0;
    // per-frame counters
    uint32_t instances_resolved = 0;  // resolver output count
    uint32_t instances_drawn   = 0;   // clusters emitted by the GPU cull
    uint32_t clusters_culled   = 0;   // frustum-culled clusters
    uint32_t hiz_culled        = 0;   // HiZ-occlusion-culled clusters
    uint32_t triangles         = 0;   // rasterized triangle count
    // world/bake census (filled by request_bake / reload)
    uint32_t instances_total = 0;
    uint32_t parts_baked = 0;         // cache misses last bake
    uint32_t cache_hits  = 0;         // cache hits last bake
    int probe_dims[3] = {0, 0, 0};    // probe grid (all zero = unavailable)
};

class WorldSession {
public:
    ~WorldSession();   // releases session GL resources — destroy before CloseWindow

    // Phase A: synchronous — returns after the full bake + GPU upload completes.
    // Emits BakeStarted, BakePartDone xN, then BakeFinished or BakeError.
    void request_bake();

    // Poll provider deltas and apply them to world state. Call once per frame.
    void tick();

    // Resolve -> cull -> clear (kernel-derived sky color) -> draw into the
    // currently bound framebuffer. Requires a live GL context on this thread.
    void render(const Camera3D& cam, int fb_width, int fb_height,
                const RenderOptions& opts);

    bool poll_event(Event& out);       // drain one; loop until false
    const FrameStats& frame_stats() const;

    // Live-edit rebake. Fail-closed: on error a BakeError event is emitted and
    // render() no-ops until a later request_bake()/reload() succeeds (the old
    // world is torn down before rebaking). Same event sequence as request_bake.
    void reload();

    // Query API (backed by a lazily built CPU BVH; first call after a bake pays
    // the build cost).
    bool raycast(const float origin[3], const float dir[3], float max_t, RayHit& out);
    uint32_t instance_count() const;
    bool instance_info(uint32_t idx, InstanceInfo& out);

    struct Impl;
    explicit WorldSession(std::unique_ptr<Impl> impl);   // internal; use open_world
    WorldSession(const WorldSession&) = delete;
    WorldSession& operator=(const WorldSession&) = delete;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace matter
