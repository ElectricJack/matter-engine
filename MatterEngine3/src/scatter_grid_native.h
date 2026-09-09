#ifndef MATTER_SCATTER_GRID_NATIVE_H
#define MATTER_SCATTER_GRID_NATIVE_H

// MatterEngine3/src/scatter_grid_native.h
//
// The scatter candidate grid, natively — the exact algorithm behind
// __candidatesInRect / __planCandidates.
//
// Extracted from dsl_bindings.cpp's anonymous namespace so the binding and the
// benchmark share ONE definition. A copy would be the wrong move twice over:
// this repo's history is a list of copies that diverged (CLAUDE.md), and here a
// divergence in the last bit MOVES EVERY TREE IN EVERY WORLD.
//
// This is shared-lib/scatter_grid.js's candidatesInRect, op for op.
//
// BITWISE IDENTITY IS THE CONTRACT. Every placement in every world is derived
// from these hashes. uint32_t arithmetic reproduces JS exactly, and the
// equivalences are worth stating because they are the whole reason it is safe:
//   - Math.imul(a,b)  == (uint32)a * (uint32)b truncated to 32 bits
//   - x >>> n         == (uint32)x >> n
//   - a ^ b           == ToInt32(a) ^ ToInt32(b), same bits as uint32 XOR
//   - h + Math.imul(...) is an exact integer sum of two int32 values, and the
//     ToInt32 the following ^= applies is the bit pattern uint32 addition
//     wraps to.
// The float math is double throughout, as it is in JS.
//
// sector_bake_tests cross-checks this against the JS implementation over the
// real rects rather than taking that argument on trust.
//
// COST SHAPE (measured, tests/scatter_grid_bench.cpp): per grid cell the
// caller evaluates sg_cell_candidate TEN times — once for the cell and once
// for each of its eight neighbours inside sg_survives — so each cell's
// candidate is recomputed nine times over as a neighbour of others. A 96 m
// tree rect at 1.65 m spacing is 59x59 cells; the grass families are worse, at
// 0.63 m spacing over a 64 m cell, i.e. 101x101.
//
// Nothing here is game-specific — (seed, kind, minDist, rect) in, deterministic
// points out — which is what makes it engine code rather than ecology.

// Consumers: MatterEngine3/src/dsl_bindings.cpp (the __candidatesInRect and
// __planCandidates JS bindings the scatter DSL calls) and
// MatterEngine3/tests/scatter_grid_bench.cpp.  Header-only and dependency-free
// — no engine headers, no allocation, no global or static state — so every
// function here is pure and callable from any thread.
//
// Conventions:
//   - `seed` and `kind` are the scatter family's identity.  Changing either
//     reshuffles every placement of that family everywhere.
//   - `min_dist` is the grid pitch in world units, and is simultaneously the
//     minimum spacing sg_survives enforces.
//   - (cx, cz) are signed integer cell indices; cell cx covers
//     [cx*min_dist, (cx+1)*min_dist) on X, and negative cells are ordinary.
//     There is no origin and no extent — the grid is infinite and a rect merely
//     selects a range of cells, which is why two neighbouring sectors agree
//     exactly on the placements they share.
#include <cstdint>

namespace scatter_grid {

// One mixing round, bit-for-bit the JS `mix(h, c)`.  `c` is a per-use salt —
// callers pass a different fixed constant for each output channel — and `c | 1u`
// forces it odd.  Every operation is uint32 wraparound on purpose: that is what
// Math.imul and `>>> 0` do on the JS side.
inline uint32_t sg_mix(uint32_t h, uint32_t c) {
    h = (h ^ (h >> 15)) * (c | 1u);
    h ^= h + (h ^ (h >> 7)) * (h | 61u);
    return h ^ (h >> 14);
}

// Per-cell root hash.  Everything a cell's candidate needs is derived from this
// single value, so one candidate costs seven sg_mix rounds in total (two here,
// five in sg_cell_candidate).  The (uint32_t) casts on cx/cz reproduce the
// two's-complement wraparound JS applies to negative indices, so negative cells
// hash identically in both languages.
inline uint32_t sg_base_hash(uint32_t seed, uint32_t kind,
                             int32_t cx, int32_t cz) {
    uint32_t h = seed ^ (kind * 374761393u);
    h = sg_mix(h ^ ((uint32_t)cx * 668265263u), 2246822519u);
    h = sg_mix(h ^ ((uint32_t)cz * 1274126177u), 374761393u);
    return h;
}

// Hash -> [0, 1) as a double: exactly h / 2^32, as in the JS.  Never reaches 1.
inline double sg_unit(uint32_t h) { return (double)h / 4294967296.0; }

// The single point a grid cell proposes, before neighbour rejection.  Fully
// determined by (seed, kind, cx, cz, min_dist) — no state, no ordering, no
// dependence on which sector asked.
struct SgCandidate {
    // x, z  position in world units, same frame as the rect the caller scans:
    //       the cell origin plus a jitter confined to the middle half of the
    //       cell (0.25 .. 0.75 of min_dist on each axis).
    // rot   yaw in radians, 0 .. 2*pi.
    // u, v  two further independent uniform [0, 1) draws, unused here and left
    //       for the caller to spend on scale / species / tint.
    double x, z, rot, u, v;
    // Rejection priority: the raw cell hash.  HIGHER wins; exact ties are broken
    // by cell index inside sg_survives, never by iteration order — which is what
    // makes the outcome independent of who evaluates the cell.
    uint32_t pri;
};

inline SgCandidate sg_cell_candidate(uint32_t seed, uint32_t kind,
                                     int32_t cx, int32_t cz, double min_dist) {
    const uint32_t h = sg_base_hash(seed, kind, cx, cz);
    const double jx = sg_unit(sg_mix(h, 0x9E3779B1u));
    const double jz = sg_unit(sg_mix(h, 0x85EBCA77u));
    SgCandidate c;
    c.x = ((double)cx + 0.25 + 0.5 * jx) * min_dist;
    c.z = ((double)cz + 0.25 + 0.5 * jz) * min_dist;
    c.rot = sg_unit(sg_mix(h, 0xC2B2AE3Du)) * 3.141592653589793 * 2.0;
    c.u = sg_unit(sg_mix(h, 0x27D4EB2Fu));
    c.v = sg_unit(sg_mix(h, 0x165667B1u));
    c.pri = h;
    return c;
}

// Neighbour-priority rejection: an exact min-distance guarantee that is
// order-independent, so a candidate's fate is the same from any sector that
// happens to look at it. Identical rule and identical tie-break to the JS.
// Returns true if the candidate is kept.  Recomputes all eight neighbouring
// candidates from scratch on every call — nothing is cached, which is the
// nine-times-over recomputation the COST SHAPE note above measures.  Only the
// 3x3 neighbourhood is examined: a candidate two cells away cannot be closer
// than min_dist, because the jitter keeps every point inside the middle half of
// its own cell.
inline bool sg_survives(uint32_t seed, uint32_t kind, int32_t cx, int32_t cz,
                        double min_dist, const SgCandidate& c) {
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const int32_t nx = cx + dx, nz = cz + dz;
            const SgCandidate o =
                sg_cell_candidate(seed, kind, nx, nz, min_dist);
            const double ddx = c.x - o.x, ddz = c.z - o.z;
            if (ddx * ddx + ddz * ddz >= min_dist * min_dist) continue;
            if (o.pri > c.pri) return false;
            if (o.pri == c.pri && (nz < cz || (nz == cz && nx < cx)))
                return false;
        }
    return true;
}

}  // namespace scatter_grid

#endif  // MATTER_SCATTER_GRID_NATIVE_H
