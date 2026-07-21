#include "vk_emitter_gather.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace viewer {

// ---------------------------------------------------------------------------
// Row-major 4x4 helpers.  Layout: M[row][col] = flat[row*4 + col].
// The codebase convention is:
//   out[i] = M[0][i]*v.x + M[1][i]*v.y + M[2][i]*v.z + M[3][i]
// which in flat indexing gives:
//   out[i] = flat[i]*v.x + flat[4+i]*v.y + flat[8+i]*v.z + flat[12+i]
// ---------------------------------------------------------------------------

static void transform_point(const float M[16], const float v[3], float out[3]) {
    for (int i = 0; i < 3; ++i)
        out[i] = M[i] * v[0] + M[4 + i] * v[1] + M[8 + i] * v[2] + M[12 + i];
}

static void transform_dir(const float M[16], const float d[3], float out[3]) {
    for (int i = 0; i < 3; ++i)
        out[i] = M[i] * d[0] + M[4 + i] * d[1] + M[8 + i] * d[2];
}

static float length3(const float v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void normalize3(float v[3]) {
    float len = length3(v);
    if (len > 1e-12f) {
        float inv = 1.0f / len;
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
    }
}

static float dist_sq(const float a[3], const float b[3]) {
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

// ---------------------------------------------------------------------------
// gather()
// ---------------------------------------------------------------------------

std::vector<GpuVolumeEmitter> VolumeEmitterGatherer::gather(
    const float camera_pos[3],
    const std::vector<EmitterInstance>& instances)
{
    const float range_sq = kMaxRange * kMaxRange;

    // Phase 1: transform positions, distance-filter.
    struct Candidate {
        float    world_pos[3];
        float    world_dir[3];
        float    d2;
        size_t   src_idx;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(instances.size());

    for (size_t i = 0; i < instances.size(); ++i) {
        const auto& inst = instances[i];
        float wp[3];
        transform_point(inst.transform, inst.emitter.pos, wp);

        float d2 = dist_sq(camera_pos, wp);
        if (d2 > range_sq) continue;

        float wd[3];
        transform_dir(inst.transform, inst.emitter.dir, wd);
        normalize3(wd);

        Candidate c;
        c.world_pos[0] = wp[0]; c.world_pos[1] = wp[1]; c.world_pos[2] = wp[2];
        c.world_dir[0] = wd[0]; c.world_dir[1] = wd[1]; c.world_dir[2] = wd[2];
        c.d2 = d2;
        c.src_idx = i;
        candidates.push_back(c);
    }

    // Phase 2: sort by distance (ascending), cap at kMaxEmitters.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.d2 < b.d2; });

    if (candidates.size() > kMaxEmitters) {
        if (!overflow_logged_) {
            fprintf(stderr,
                    "[volumetrics] %zu emitters in range, capped to %u\n",
                    candidates.size(),
                    static_cast<unsigned>(kMaxEmitters));
            overflow_logged_ = true;
        }
        candidates.resize(kMaxEmitters);
    }

    // Phase 3: convert to GPU format.
    std::vector<GpuVolumeEmitter> result;
    result.reserve(candidates.size());

    for (const auto& c : candidates) {
        const auto& em = instances[c.src_idx].emitter;
        GpuVolumeEmitter g{};
        g.world_pos[0] = c.world_pos[0];
        g.world_pos[1] = c.world_pos[1];
        g.world_pos[2] = c.world_pos[2];
        g.radius       = em.radius;
        g.world_dir[0] = c.world_dir[0];
        g.world_dir[1] = c.world_dir[1];
        g.world_dir[2] = c.world_dir[2];
        g.spread       = em.spread;
        g.length       = em.length;
        g.density      = em.density;
        g.rise         = em.rise;
        g.turbulence   = em.turbulence;
        g.color[0]     = em.color[0];
        g.color[1]     = em.color[1];
        g.color[2]     = em.color[2];
        g.pad          = 0.0f;
        result.push_back(g);
    }
    return result;
}

// ---------------------------------------------------------------------------
// gather_flat() — test helper
// ---------------------------------------------------------------------------

std::vector<GpuVolumeEmitter> VolumeEmitterGatherer::gather_flat(
    const float camera_pos[3],
    const std::vector<std::pair<part_asset::VolumeEmitter,
                                std::array<float, 16>>>& pairs)
{
    std::vector<EmitterInstance> instances;
    instances.reserve(pairs.size());
    for (const auto& p : pairs) {
        EmitterInstance inst;
        inst.emitter = p.first;
        std::memcpy(inst.transform, p.second.data(), 16 * sizeof(float));
        instances.push_back(inst);
    }
    return gather(camera_pos, instances);
}

} // namespace viewer
