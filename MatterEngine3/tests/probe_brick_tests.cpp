// MatterEngine3/tests/probe_brick_tests.cpp
#include "check.h"
#include "../src/probe_bricks.h"
#include <cmath>

using probe_volume::ProbeVolume;

// Constant-valued brick covering sector (tx,tz): sector_size m in x/z from
// tx*S, vertical y0..y0+ny*cell, given cell size.
static ProbeVolume make_brick(int64_t tx, int64_t tz, float S, float cell,
                              float y0, float ar, float ag, float ab, float sun) {
    ProbeVolume v;
    v.grid.cell = cell;
    v.grid.nx = (int)std::lround(S / cell);
    v.grid.nz = (int)std::lround(S / cell);
    v.grid.ny = 8;
    // origin = centre of cell (0,0,0)
    v.grid.origin[0] = (float)tx * S + cell * 0.5f;
    v.grid.origin[1] = y0 + cell * 0.5f;
    v.grid.origin[2] = (float)tz * S + cell * 0.5f;
    size_t n = v.cells();
    v.ambient.assign(n * 4, 0.0f);
    v.dominant.assign(n * 4, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        v.ambient[i*4+0] = ar; v.ambient[i*4+1] = ag;
        v.ambient[i*4+2] = ab; v.ambient[i*4+3] = sun;
        v.dominant[i*4+1] = 1.0f;   // dir +y
        v.dominant[i*4+3] = 0.5f;   // intensity
    }
    return v;
}

int main() {
    const float S = 64.0f;
    world_lights::WorldLights lights;   // sky_color {0.38,0.43,0.52}

    // ---- sample_volume: trilinear between two known cells ----
    {
        ProbeVolume v;
        v.grid.cell = 2.0f; v.grid.nx = 2; v.grid.ny = 1; v.grid.nz = 1;
        v.grid.origin[0] = 0; v.grid.origin[1] = 0; v.grid.origin[2] = 0;
        v.ambient = {0,0,0,0,  1,1,1,1};       // cell0 black, cell1 white
        v.dominant = {0,0,0,0, 0,0,0,0};
        float a[4], d[4];
        float mid[3] = {1.0f, 0.0f, 0.0f};     // halfway between cell centres
        CHECK(probe_bricks::sample_volume(v, mid, a, d), "sample ok");
        CHECK(std::fabs(a[0] - 0.5f) < 1e-4f, "trilinear midpoint = 0.5");
        float lo[3] = {-50.0f, 0.0f, 0.0f};    // clamps to cell 0
        probe_bricks::sample_volume(v, lo, a, d);
        CHECK(a[0] < 1e-4f, "clamped to first cell");
        ProbeVolume bad;
        CHECK(!probe_bricks::sample_volume(bad, mid, a, d), "invalid volume rejected");
    }

    probe_bricks::BrickStore store;
    probe_bricks::WindowParams wp;      // 64x32x64 @ 4m
    ProbeVolume win;

    // ---- empty store: full fallback, 0 covered ----
    {
        size_t covered = store.composite(32, 20, 32, lights, S, wp, win);
        CHECK(covered == 0, "empty store covers nothing");
        CHECK(win.valid(), "window still a valid volume");
        CHECK(win.grid.nx == wp.nx && win.grid.ny == wp.ny && win.grid.nz == wp.nz,
              "window dims match params");
        // every cell = sky fallback
        CHECK(std::fabs(win.ambient[0] - lights.sky_color[0]) < 1e-4f, "fallback r");
        CHECK(std::fabs(win.ambient[3] - 1.0f) < 1e-4f, "fallback sun_vis = 1");
        CHECK(std::fabs(win.dominant[3]) < 1e-4f, "fallback dominant intensity 0");
    }

    // ---- one red brick at sector (0,0): inside red, outside fallback ----
    {
        store.put(0, 0, make_brick(0, 0, S, 4.0f, 0.0f, 1, 0, 0, 0.25f));
        CHECK(store.has(0, 0), "has after put");
        CHECK(store.count() == 1, "count 1");
        size_t covered = store.composite(32, 16, 32, lights, S, wp, win);
        CHECK(covered > 0, "some cells covered");

        auto cell_at = [&](float x, float y, float z, float out[4]) {
            int ix = (int)std::floor((x - win.grid.origin[0]) / win.grid.cell + 0.5f);
            int iy = (int)std::floor((y - win.grid.origin[1]) / win.grid.cell + 0.5f);
            int iz = (int)std::floor((z - win.grid.origin[2]) / win.grid.cell + 0.5f);
            size_t idx = (((size_t)iz * win.grid.ny) + iy) * win.grid.nx + ix;
            for (int k = 0; k < 4; ++k) out[k] = win.ambient[idx*4 + k];
        };
        float a[4];
        cell_at(30, 16, 30, a);                    // inside sector (0,0), inside brick y
        CHECK(std::fabs(a[0] - 1.0f) < 1e-3f && a[1] < 1e-3f, "inside brick = red");
        CHECK(std::fabs(a[3] - 0.25f) < 1e-3f, "inside brick sun_vis");
        cell_at(-30, 16, 30, a);                   // sector (-1,0): no brick
        CHECK(std::fabs(a[0] - lights.sky_color[0]) < 1e-3f, "outside brick = fallback");
    }

    // ---- mixed densities: 8m green brick at sector (1,0) ----
    {
        store.put(1, 0, make_brick(1, 0, S, 8.0f, 0.0f, 0, 1, 0, 1.0f));
        size_t covered = store.composite(64, 16, 32, lights, S, wp, win);
        CHECK(covered > 0, "covered with two bricks");
        float a[4], d[4];
        float p0[3] = {30, 16, 30}, p1[3] = {96, 16, 30};
        probe_bricks::sample_volume(win, p0, a, d);
        CHECK(a[0] > 0.9f, "sector 0 still red");
        probe_bricks::sample_volume(win, p1, a, d);
        CHECK(a[1] > 0.9f, "sector 1 green via 8m brick");
    }

    // ---- window centre snapping: re-centres move by whole cells ----
    {
        ProbeVolume w1, w2, w3;
        store.composite(32.0f, 16.0f, 32.0f, lights, S, wp, w1);
        store.composite(33.9f, 16.7f, 31.2f, lights, S, wp, w2);   // same 4m cell
        CHECK(std::fabs(w2.grid.origin[0] - w1.grid.origin[0]) < 1e-3f,
              "same snapped cell -> identical origin");
        store.composite(32.0f + 3.0f * wp.cell, 16.0f, 32.0f, lights, S, wp, w3);
        float frac = (w3.grid.origin[0] - w1.grid.origin[0]) / wp.cell;
        CHECK(std::fabs(frac - std::round(frac)) < 1e-3f, "origin delta is integer cells");
        CHECK(std::fabs(frac - 3.0f) < 1e-3f, "moved exactly 3 cells");
    }

    // ---- erase / clear ----
    {
        store.erase(0, 0);
        CHECK(!store.has(0, 0), "erased");
        CHECK(store.count() == 1, "one left");
        store.clear();
        CHECK(store.count() == 0, "cleared");
    }

    return check_summary();
}
