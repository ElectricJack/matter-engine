// probe_bake.cpp — CPU SH-L1 probe volume baker (GL-free, multithreaded).
// See probe_bake.h for public API. Algorithm spec in task-4-brief.md.
#include "probe_bake.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace probe_bake {

// ---------------------------------------------------------------------------
// splitmix64 — deterministic per-cell RNG for sun cone jitter
// ---------------------------------------------------------------------------

static uint64_t splitmix64(uint64_t& state) {
    state += 0x9e3779b97f4a7c15ULL;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// Map a raw 64-bit value to [0,1)
static float u64_to_float01(uint64_t v) {
    // Use top 23 bits for mantissa
    return (float)((double)(v >> 11) * (1.0 / (double)(1ULL << 53)));
}

// ---------------------------------------------------------------------------
// Spherical Fibonacci directions (full sphere)
// ---------------------------------------------------------------------------

static void build_fibonacci_dirs(int N, std::vector<float>& dirs) {
    // dirs: N * 3 floats
    dirs.resize(N * 3);
    const double golden = 2.399963229728653;  // 2*pi*(1 - 1/phi)
    for (int i = 0; i < N; ++i) {
        double z   = 1.0 - 2.0 * (i + 0.5) / N;
        double r   = std::sqrt(std::max(0.0, 1.0 - z*z));
        double phi = i * golden;
        dirs[i*3+0] = (float)(r * std::cos(phi));
        dirs[i*3+1] = (float)z;
        dirs[i*3+2] = (float)(r * std::sin(phi));
    }
}

// ---------------------------------------------------------------------------
// Orthonormal basis around a given unit direction
// ---------------------------------------------------------------------------

static void make_onb(const float n[3], float t[3], float b[3]) {
    // Duff et al. "Building an Orthonormal Basis, Revisited"
    float s = (n[2] >= 0.f) ? 1.f : -1.f;
    float a = -1.f / (s + n[2]);
    float c = n[0] * n[1] * a;
    t[0] = 1.f + s * n[0]*n[0]*a;
    t[1] = s * c;
    t[2] = -s * n[0];
    b[0] = c;
    b[1] = s + n[1]*n[1]*a;
    b[2] = -n[1];
}

// ---------------------------------------------------------------------------
// Luminance helper (Rec. 709 coefficients)
// ---------------------------------------------------------------------------

static float lum(float r, float g, float b) {
    return 0.2126f*r + 0.7152f*g + 0.0722f*b;
}

// ---------------------------------------------------------------------------
// Per-cell bake (fully self-contained, no locks needed)
// ---------------------------------------------------------------------------

static void bake_cell(
    int cx, int cy, int cz,
    int nx, int ny, int nz,
    const probe_volume::ProbeGrid& grid,
    const std::vector<float>& fib_dirs,  // rays_per_cell * 3
    const world_tracer::WorldTracer& tracer,
    const world_lights::WorldLights& lights,
    const BakeParams& p,
    float* ambient_out,  // 4 floats at offset [cell_idx*4]
    float* dominant_out  // 4 floats at offset [cell_idx*4]
) {
    int N = p.rays_per_cell;
    // Cell center in world space
    float P[3] = {
        grid.origin[0] + cx * grid.cell,
        grid.origin[1] + cy * grid.cell,
        grid.origin[2] + cz * grid.cell
    };

    float sumL[3]  = {0,0,0};
    float sumL1[3] = {0,0,0};

    for (int i = 0; i < N; ++i) {
        const float* d = &fib_dirs[i * 3];
        world_tracer::Hit h;
        bool hit = tracer.trace(P, d, 1e30f, h);

        float L[3];
        if (!hit) {
            // Sky
            L[0] = lights.sky_color[0];
            L[1] = lights.sky_color[1];
            L[2] = lights.sky_color[2];
        } else {
            // Mesh light: albedo * emission (no bounce in v1)
            L[0] = h.albedo[0] * h.emission;
            L[1] = h.albedo[1] * h.emission;
            L[2] = h.albedo[2] * h.emission;
        }

        sumL[0] += L[0];
        sumL[1] += L[1];
        sumL[2] += L[2];

        float lm = lum(L[0], L[1], L[2]);
        sumL1[0] += d[0] * lm;
        sumL1[1] += d[1] * lm;
        sumL1[2] += d[2] * lm;
    }

    // Average over ray count
    float invN = 1.f / (float)N;
    float amb_r = sumL[0] * invN;
    float amb_g = sumL[1] * invN;
    float amb_b = sumL[2] * invN;

    // Spotlight contributions (analytic, added after MC average)
    float spot_ambient[3] = {0,0,0};
    // Pre-scaled L1 analytic: sumL1_analytic in dir * lum(Ls) * N units
    float sumL1_analytic[3] = {0,0,0};

    for (const world_lights::SpotLight& spot : lights.spots) {
        float v[3] = {
            spot.pos[0] - P[0],
            spot.pos[1] - P[1],
            spot.pos[2] - P[2]
        };
        float dist2 = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
        float dist  = std::sqrt(dist2);

        if (dist > spot.range || dist < 1e-4f) continue;

        float dirToLight[3] = { v[0]/dist, v[1]/dist, v[2]/dist };

        // cos angle between -dirToLight and spot.dir
        float neg_dir_to_light[3] = { -dirToLight[0], -dirToLight[1], -dirToLight[2] };
        float cosang = neg_dir_to_light[0]*spot.dir[0]
                     + neg_dir_to_light[1]*spot.dir[1]
                     + neg_dir_to_light[2]*spot.dir[2];

        // Cone attenuation
        float denom = std::max(spot.cos_inner - spot.cos_outer, 1e-4f);
        float k = std::max(0.f, std::min(1.f, (cosang - spot.cos_outer) / denom));
        if (k == 0.f) continue;

        // Shadow test
        if (tracer.occluded(P, dirToLight, dist - 1e-3f)) continue;

        // Same quadratic falloff as lighting.glsl: 1 / (1 + 0.005*dist*dist)
        float falloff = 1.f / (1.f + 0.005f * dist2);
        float Ls[3] = {
            spot.color[0] * k * falloff,
            spot.color[1] * k * falloff,
            spot.color[2] * k * falloff
        };

        spot_ambient[0] += Ls[0];
        spot_ambient[1] += Ls[1];
        spot_ambient[2] += Ls[2];

        // Pre-scale by N so the /N below treats analytic and MC consistently
        float lm_s = lum(Ls[0], Ls[1], Ls[2]);
        sumL1_analytic[0] += dirToLight[0] * lm_s * (float)N;
        sumL1_analytic[1] += dirToLight[1] * lm_s * (float)N;
        sumL1_analytic[2] += dirToLight[2] * lm_s * (float)N;
    }

    // Sun visibility: jittered cone rays, RNG seeded by linear cell index
    int linear_idx = ((cz * ny) + cy) * nx + cx;
    uint64_t rng_state = (uint64_t)linear_idx;

    // Orthonormal basis around -sun_dir (the direction toward the sun)
    float sun_toward[3] = {
        -lights.sun_dir[0],
        -lights.sun_dir[1],
        -lights.sun_dir[2]
    };
    float sun_t[3], sun_b[3];
    make_onb(sun_toward, sun_t, sun_b);

    float cone_half_rad = p.sun_cone_deg * (float)(M_PI / 180.0) * 0.5f;
    float cone_cos = std::cos(cone_half_rad);
    float cone_sin = std::sin(cone_half_rad);

    int unoccluded_count = 0;
    for (int s = 0; s < p.sun_rays; ++s) {
        float u1 = u64_to_float01(splitmix64(rng_state));
        float u2 = u64_to_float01(splitmix64(rng_state));

        // Sample uniform disk then project into cone
        float r   = cone_sin * std::sqrt(u1);
        float phi = u2 * 2.f * (float)M_PI;
        float dx  = r * std::cos(phi);
        float dy  = r * std::sin(phi);
        float dz  = std::sqrt(std::max(0.f, 1.f - r*r));

        // Rotate into sun direction basis
        // The cone center is sun_toward (z-axis); tangent plane = (sun_t, sun_b)
        float sun_ray[3] = {
            dx * sun_t[0] + dy * sun_b[0] + dz * sun_toward[0],
            dx * sun_t[1] + dy * sun_b[1] + dz * sun_toward[1],
            dx * sun_t[2] + dy * sun_b[2] + dz * sun_toward[2]
        };
        // Normalize (should be unit already but account for float drift)
        float len2 = sun_ray[0]*sun_ray[0] + sun_ray[1]*sun_ray[1] + sun_ray[2]*sun_ray[2];
        if (len2 > 1e-12f) {
            float inv_len = 1.f / std::sqrt(len2);
            sun_ray[0] *= inv_len; sun_ray[1] *= inv_len; sun_ray[2] *= inv_len;
        }

        if (!tracer.occluded(P, sun_ray, 1e30f)) {
            ++unoccluded_count;
        }
    }
    float vis = (float)unoccluded_count / (float)p.sun_rays;

    // Final outputs
    ambient_out[0] = amb_r + spot_ambient[0];
    ambient_out[1] = amb_g + spot_ambient[1];
    ambient_out[2] = amb_b + spot_ambient[2];
    ambient_out[3] = vis;

    // L1 vector: (sumL1 + sumL1_analytic) / N
    float L1[3] = {
        (sumL1[0] + sumL1_analytic[0]) * invN,
        (sumL1[1] + sumL1_analytic[1]) * invN,
        (sumL1[2] + sumL1_analytic[2]) * invN
    };
    float L1_mag = std::sqrt(L1[0]*L1[0] + L1[1]*L1[1] + L1[2]*L1[2]);
    if (L1_mag > 1e-6f) {
        dominant_out[0] = L1[0] / L1_mag;
        dominant_out[1] = L1[1] / L1_mag;
        dominant_out[2] = L1[2] / L1_mag;
    } else {
        dominant_out[0] = dominant_out[1] = dominant_out[2] = 0.f;
    }
    dominant_out[3] = L1_mag;
}

// ---------------------------------------------------------------------------
// Worker: bakes a contiguous range of z-planes
// ---------------------------------------------------------------------------

static void bake_z_slice(
    int z_start, int z_end,
    const probe_volume::ProbeGrid& grid,
    const std::vector<float>& fib_dirs,
    const world_tracer::WorldTracer& tracer,
    const world_lights::WorldLights& lights,
    const BakeParams& p,
    float* ambient_data,
    float* dominant_data
) {
    int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    for (int cz = z_start; cz < z_end; ++cz) {
        for (int cy = 0; cy < ny; ++cy) {
            for (int cx = 0; cx < nx; ++cx) {
                int idx = ((cz * ny) + cy) * nx + cx;
                bake_cell(cx, cy, cz, nx, ny, nz,
                          grid, fib_dirs, tracer, lights, p,
                          ambient_data  + idx * 4,
                          dominant_data + idx * 4);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

probe_volume::ProbeVolume bake_probes(const world_tracer::WorldTracer& tracer,
                                      const world_lights::WorldLights& lights,
                                      const BakeParams& p)
{
    probe_volume::ProbeVolume vol;

    // --- Step 1: Determine grid ---
    float world_mn[3], world_mx[3];
    if (p.has_bounds) {
        world_mn[0] = p.bounds_min[0];
        world_mn[1] = p.bounds_min[1];
        world_mn[2] = p.bounds_min[2];
        world_mx[0] = p.bounds_max[0];
        world_mx[1] = p.bounds_max[1];
        world_mx[2] = p.bounds_max[2];
    } else {
        tracer.world_bounds(world_mn, world_mx);
    }

    // Pad
    float pad = p.pad_cells * p.cell;
    float padded_min[3] = {
        world_mn[0] - pad,
        world_mn[1] - pad,
        world_mn[2] - pad
    };
    float padded_max[3] = {
        world_mx[0] + pad,
        world_mx[1] + pad,
        world_mx[2] + pad
    };

    float extent[3] = {
        padded_max[0] - padded_min[0],
        padded_max[1] - padded_min[1],
        padded_max[2] - padded_min[2]
    };

    // Grow cell if any axis would exceed max_cells_axis
    float cell = p.cell;
    float max_extent = std::max({extent[0], extent[1], extent[2]});
    if (max_extent / cell > (float)p.max_cells_axis) {
        cell = max_extent / (float)p.max_cells_axis;
    }

    probe_volume::ProbeGrid& grid = vol.grid;
    grid.cell = cell;
    grid.nx = std::max(1, (int)std::ceil((double)extent[0] / cell));
    grid.ny = std::max(1, (int)std::ceil((double)extent[1] / cell));
    grid.nz = std::max(1, (int)std::ceil((double)extent[2] / cell));
    // origin = center of cell (0,0,0)
    grid.origin[0] = padded_min[0] + 0.5f * cell;
    grid.origin[1] = padded_min[1] + 0.5f * cell;
    grid.origin[2] = padded_min[2] + 0.5f * cell;

    size_t total_cells = (size_t)grid.nx * grid.ny * grid.nz;
    vol.ambient.resize(total_cells * 4, 0.f);
    vol.dominant.resize(total_cells * 4, 0.f);

    // --- Step 2: Precompute Fibonacci directions ---
    std::vector<float> fib_dirs;
    build_fibonacci_dirs(p.rays_per_cell, fib_dirs);

    // --- Step 3: Bake cells, sliced by z-plane ---
    int nthreads = p.threads;
    if (nthreads <= 0) {
        nthreads = (int)std::thread::hardware_concurrency();
        if (nthreads <= 0) nthreads = 1;
    }
    // Can't use more threads than z-planes
    nthreads = std::min(nthreads, grid.nz);

    float* ambient_data  = vol.ambient.data();
    float* dominant_data = vol.dominant.data();

    if (nthreads <= 1) {
        bake_z_slice(0, grid.nz, grid, fib_dirs, tracer, lights, p,
                     ambient_data, dominant_data);
    } else {
        std::vector<std::thread> workers;
        workers.reserve(nthreads);
        int z_per_thread = grid.nz / nthreads;
        int z_start = 0;
        for (int t = 0; t < nthreads; ++t) {
            int z_end = (t == nthreads - 1) ? grid.nz : z_start + z_per_thread;
            workers.emplace_back(bake_z_slice,
                                 z_start, z_end,
                                 std::cref(grid), std::cref(fib_dirs),
                                 std::cref(tracer), std::cref(lights), std::cref(p),
                                 ambient_data, dominant_data);
            z_start = z_end;
        }
        for (auto& w : workers) w.join();
    }

    return vol;
}

} // namespace probe_bake
