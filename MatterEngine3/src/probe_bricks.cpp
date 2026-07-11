// probe_bricks.cpp — see probe_bricks.h.
#include "probe_bricks.h"
#include <algorithm>
#include <cmath>

namespace probe_bricks {

bool sample_volume(const probe_volume::ProbeVolume& v, const float wp[3],
                   float ambient_out[4], float dominant_out[4]) {
    if (!v.valid()) return false;
    const auto& g = v.grid;
    // Continuous cell coords (origin = centre of cell 0).
    float fx = (wp[0] - g.origin[0]) / g.cell;
    float fy = (wp[1] - g.origin[1]) / g.cell;
    float fz = (wp[2] - g.origin[2]) / g.cell;
    fx = std::clamp(fx, 0.0f, (float)(g.nx - 1));
    fy = std::clamp(fy, 0.0f, (float)(g.ny - 1));
    fz = std::clamp(fz, 0.0f, (float)(g.nz - 1));
    int x0 = (int)fx, y0 = (int)fy, z0 = (int)fz;
    int x1 = std::min(x0 + 1, g.nx - 1);
    int y1 = std::min(y0 + 1, g.ny - 1);
    int z1 = std::min(z0 + 1, g.nz - 1);
    float tx = fx - x0, ty = fy - y0, tz = fz - z0;

    auto idx = [&](int x, int y, int z) {
        return ((((size_t)z * g.ny) + y) * g.nx + x) * 4;
    };
    auto lerp_fetch = [&](const std::vector<float>& src, float out[4]) {
        for (int k = 0; k < 4; ++k) {
            float c000 = src[idx(x0,y0,z0)+k], c100 = src[idx(x1,y0,z0)+k];
            float c010 = src[idx(x0,y1,z0)+k], c110 = src[idx(x1,y1,z0)+k];
            float c001 = src[idx(x0,y0,z1)+k], c101 = src[idx(x1,y0,z1)+k];
            float c011 = src[idx(x0,y1,z1)+k], c111 = src[idx(x1,y1,z1)+k];
            float c00 = c000 + (c100 - c000) * tx;
            float c10 = c010 + (c110 - c010) * tx;
            float c01 = c001 + (c101 - c001) * tx;
            float c11 = c011 + (c111 - c011) * tx;
            float c0 = c00 + (c10 - c00) * ty;
            float c1 = c01 + (c11 - c01) * ty;
            out[k] = c0 + (c1 - c0) * tz;
        }
    };
    lerp_fetch(v.ambient,  ambient_out);
    lerp_fetch(v.dominant, dominant_out);
    return true;
}

void BrickStore::put(int64_t tx, int64_t tz, probe_volume::ProbeVolume brick) {
    std::lock_guard<std::mutex> lk(mu_);
    bricks_[{tx, tz}] = std::move(brick);
}
bool BrickStore::erase(int64_t tx, int64_t tz) {
    std::lock_guard<std::mutex> lk(mu_);
    return bricks_.erase({tx, tz}) != 0;
}
bool BrickStore::has(int64_t tx, int64_t tz) const {
    std::lock_guard<std::mutex> lk(mu_);
    return bricks_.count({tx, tz}) != 0;
}
void BrickStore::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    bricks_.clear();
}
size_t BrickStore::count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bricks_.size();
}

size_t BrickStore::composite(float cx, float cy, float cz,
                             const world_lights::WorldLights& lights,
                             float sector_size, const WindowParams& wp,
                             probe_volume::ProbeVolume& out) const {
    // Snap centre to whole cells so re-centres shift by integral cells.
    auto snap = [&](float v) { return std::floor(v / wp.cell) * wp.cell; };
    out.grid.cell = wp.cell;
    out.grid.nx = wp.nx; out.grid.ny = wp.ny; out.grid.nz = wp.nz;
    out.grid.origin[0] = snap(cx) - wp.cell * (wp.nx / 2) + wp.cell * 0.5f;
    out.grid.origin[1] = snap(cy) - wp.cell * (wp.ny / 2) + wp.cell * 0.5f;
    out.grid.origin[2] = snap(cz) - wp.cell * (wp.nz / 2) + wp.cell * 0.5f;
    const size_t n = out.cells();
    out.ambient.resize(n * 4);
    out.dominant.resize(n * 4);

    std::lock_guard<std::mutex> lk(mu_);
    size_t covered = 0;
    size_t i = 0;
    for (int z = 0; z < wp.nz; ++z)
    for (int y = 0; y < wp.ny; ++y)
    for (int x = 0; x < wp.nx; ++x, ++i) {
        float wpos[3] = { out.grid.origin[0] + x * wp.cell,
                          out.grid.origin[1] + y * wp.cell,
                          out.grid.origin[2] + z * wp.cell };
        int64_t stx = (int64_t)std::floor(wpos[0] / sector_size);
        int64_t stz = (int64_t)std::floor(wpos[2] / sector_size);
        auto it = bricks_.find({stx, stz});
        float* amb = &out.ambient[i * 4];
        float* dom = &out.dominant[i * 4];
        if (it != bricks_.end() &&
            sample_volume(it->second, wpos, amb, dom)) {
            ++covered;
        } else {
            amb[0] = lights.sky_color[0];
            amb[1] = lights.sky_color[1];
            amb[2] = lights.sky_color[2];
            amb[3] = 1.0f;                       // full sun visibility
            dom[0] = dom[1] = dom[2] = dom[3] = 0.0f;
        }
    }
    return covered;
}

} // namespace probe_bricks
