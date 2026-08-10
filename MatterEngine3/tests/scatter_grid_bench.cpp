// scatter_grid_bench.cpp — what does candidate GENERATION actually cost?
//
// channels_at_bench.cpp established that a habitat tape sample is ~574 ns while
// the profiler attributes ~22 us to one scatter candidate. That left ~97%
// unaccounted for, and elimination pointed at candidate generation. This
// measures it directly, at the real per-family spacings.
//
// It also measures the obvious alternative. The current loop calls
// sg_cell_candidate TEN times per cell (once for the cell, once per neighbour
// inside sg_survives), so every cell's candidate is computed nine extra times
// as somebody else's neighbour. MEMOIZED below computes each cell's candidate
// exactly once into a row window and has survives() read from it. Same
// candidates, same order — asserted here, not assumed — so the difference is
// pure headroom.
//
// Build (from MatterEngine3/tests):
//   g++ -O2 -std=c++17 -I../src scatter_grid_bench.cpp -o build/scatter_grid_bench

#include "scatter_grid_native.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace scatter_grid;

namespace {

struct Rect { double x0, z0, w, h; };

// ---- CURRENT: exactly what j_candidatesInRect does today -------------------
size_t gen_current(uint32_t seed, uint32_t kind, double min_dist,
                   const Rect& r, std::vector<SgCandidate>* out) {
    const double c0 = std::floor(r.x0 / min_dist);
    const double c1 = std::floor((r.x0 + r.w) / min_dist);
    const double r0 = std::floor(r.z0 / min_dist);
    const double r1 = std::floor((r.z0 + r.h) / min_dist);
    size_t n = 0;
    for (double dcz = r0; dcz <= r1; dcz += 1.0)
        for (double dcx = c0; dcx <= c1; dcx += 1.0) {
            const int32_t cx = (int32_t)dcx, cz = (int32_t)dcz;
            const SgCandidate cand = sg_cell_candidate(seed, kind, cx, cz, min_dist);
            if (!sg_survives(seed, kind, cx, cz, min_dist, cand)) continue;
            ++n;
            if (out) out->push_back(cand);
        }
    return n;
}

// ---- MEMOIZED: each cell's candidate computed exactly once -----------------
// Three-row sliding window. survives() reads neighbours from the window rather
// than rehashing them. Identical arithmetic, identical order.
size_t gen_memoized(uint32_t seed, uint32_t kind, double min_dist,
                    const Rect& r, std::vector<SgCandidate>* out) {
    const double dc0 = std::floor(r.x0 / min_dist);
    const double dc1 = std::floor((r.x0 + r.w) / min_dist);
    const double dr0 = std::floor(r.z0 / min_dist);
    const double dr1 = std::floor((r.z0 + r.h) / min_dist);
    const int32_t cx0 = (int32_t)dc0, cx1 = (int32_t)dc1;
    const int32_t cz0 = (int32_t)dr0, cz1 = (int32_t)dr1;
    // One column of padding each side so neighbours of edge cells are covered.
    const int width = (int)(cx1 - cx0) + 3;
    if (width <= 0) return 0;

    std::vector<SgCandidate> rows[3];
    for (int i = 0; i < 3; ++i) rows[i].resize((size_t)width);
    auto fill_row = [&](int slot, int32_t cz) {
        for (int i = 0; i < width; ++i)
            rows[slot][(size_t)i] =
                sg_cell_candidate(seed, kind, cx0 - 1 + i, cz, min_dist);
    };

    fill_row(0, cz0 - 1);
    fill_row(1, cz0);
    fill_row(2, cz0 + 1);

    size_t n = 0;
    for (int32_t cz = cz0; cz <= cz1; ++cz) {
        if (cz > cz0) {   // advance the window: reuse two rows, fill one
            std::swap(rows[0], rows[1]);
            std::swap(rows[1], rows[2]);
            fill_row(2, cz + 1);
        }
        for (int32_t cx = cx0; cx <= cx1; ++cx) {
            const int i = (int)(cx - cx0) + 1;
            const SgCandidate& cand = rows[1][(size_t)i];
            bool survives = true;
            for (int dz = -1; dz <= 1 && survives; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dz == 0) continue;
                    const SgCandidate& o = rows[dz + 1][(size_t)(i + dx)];
                    const double ddx = cand.x - o.x, ddz = cand.z - o.z;
                    if (ddx * ddx + ddz * ddz >= min_dist * min_dist) continue;
                    if (o.pri > cand.pri) { survives = false; break; }
                    if (o.pri == cand.pri &&
                        ((cz + dz) < cz || ((cz + dz) == cz && (cx + dx) < cx))) {
                        survives = false; break;
                    }
                }
            if (!survives) continue;
            ++n;
            if (out) out->push_back(cand);
        }
    }
    return n;
}

bool same(const std::vector<SgCandidate>& a, const std::vector<SgCandidate>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        // Bitwise on the doubles: this grid decides where every tree stands.
        if (a[i].x != b[i].x || a[i].z != b[i].z || a[i].rot != b[i].rot ||
            a[i].u != b[i].u || a[i].v != b[i].v || a[i].pri != b[i].pri)
            return false;
    }
    return true;
}

void row(const char* label, uint32_t kind, double min_dist, const Rect& r,
         int iters) {
    std::vector<SgCandidate> a, b;
    const size_t na = gen_current(20260709u, kind, min_dist, r, &a);
    const size_t nb = gen_memoized(20260709u, kind, min_dist, r, &b);
    const bool ok = same(a, b);

    const double cells_x = std::floor((r.x0 + r.w) / min_dist) - std::floor(r.x0 / min_dist) + 1;
    const double cells_z = std::floor((r.z0 + r.h) / min_dist) - std::floor(r.z0 / min_dist) + 1;
    const double cells = cells_x * cells_z;

    auto time_it = [&](size_t (*fn)(uint32_t, uint32_t, double, const Rect&,
                                    std::vector<SgCandidate>*)) {
        const auto t0 = std::chrono::high_resolution_clock::now();
        size_t sink = 0;
        for (int i = 0; i < iters; ++i)
            sink += fn(20260709u + (uint32_t)i, kind, min_dist, r, nullptr);
        const auto t1 = std::chrono::high_resolution_clock::now();
        if (sink == 12345678u) std::printf(" ");   // keep the calls
        return std::chrono::duration<double, std::micro>(t1 - t0).count() / iters;
    };

    const double us_cur = time_it(&gen_current);
    const double us_mem = time_it(&gen_memoized);

    std::printf("  %-26s %6.0f cells -> %5zu cand | current %8.1f us | memo %8.1f us"
                " | %4.1fx | %s\n",
                label, cells, na, us_cur, us_mem,
                us_mem > 0 ? us_cur / us_mem : 0.0,
                ok ? "identical" : "*** DIFFERS ***");
    if (!ok)
        std::printf("      current=%zu memo=%zu -- the memo variant is NOT a"
                    " drop-in; ignore its timing\n", na, nb);
}

} // namespace

int main() {
    std::printf("scatter candidate generation, real per-family spacings\n");
    std::printf("(a 64 m sector cell; tree rect is the 96 m one planTrees uses)\n\n");

    const Rect sector{0.0, 0.0, 64.0, 64.0};
    const Rect treerect{-16.0, -16.0, 96.0, 96.0};

    row("tree      1.65 m / 96 m", 3u, 1.65, treerect, 200);
    row("shrub     1.05 m / 64 m", 5u, 1.05, sector, 200);
    row("groundCvr 0.85 m / 64 m", 6u, 0.85, sector, 200);
    row("flower    0.75 m / 64 m", 7u, 0.75, sector, 200);
    row("grass     0.63 m / 64 m", 8u, 0.63, sector, 200);

    std::printf("\nOne band-5 bake plans 5 families over the sector plus the\n"
                "tree rect. Sum the 'current' column for the per-bake cost, and\n"
                "compare against ~522 ms/bake wall and the ~574 ns/sample the\n"
                "habitat tape costs (channels_at_bench).\n");
    return 0;
}
